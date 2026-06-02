// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024-2026, Advanced Micro Devices, Inc.
 *
 * VE2 management backend — XRS resource request, AIE partition lifecycle,
 * and command scheduling via the Linux xlnx-aie partition APIs.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>
#include <linux/xlnx-ai-engine.h>

#include "amdxdna_ctx.h"
#include "amdxdna_drv.h"
#include "amdxdna_solver.h"
#include "ve2_aux.h"
#include "ve2_hwctx.h"
#include "ve2_host_queue.h"
#include "ve2_mgmt.h"

#define VE2_COL_SHIFT			25
#define VE2_ROW_SHIFT			20
#define VE2_ADDR(col, row, off) \
	(((col) << VE2_COL_SHIFT) + ((row) << VE2_ROW_SHIFT) + (off))
#define VE2_HANDSHAKE_OFF		0x88000
#define CERT_HANDSHAKE_OFF(col)		VE2_ADDR(col, 0, VE2_HANDSHAKE_OFF)
#define VE2_EVENT_GENERATE_REG		0x00034008
#define VE2_USER_EVENT_ID		0xB6

/* VE2 AIE partition column granularity (partitions are 4-column aligned). */
#define VE2_MIN_COL_SUPPORT		4

static int ve2_partition_read_privileged_mem(struct amdxdna_mgmtctx *mgmtctx, size_t field_off,
				      size_t size, void *buf)
{
	u32 offset = CERT_HANDSHAKE_OFF(0) + field_off;

	return aie_partition_read_privileged_mem(mgmtctx->aie_dev, offset, size, buf);
}

static int ve2_aie_read_idle(struct amdxdna_mgmtctx *mgmtctx, u32 *idle)
{
	return ve2_partition_read_privileged_mem(mgmtctx, offsetof(struct handshake, cert_idle_status),
					  sizeof(*idle), idle);
}

static void ve2_scheduler_work(struct work_struct *work);
static void ve2_irq_handler(u32 partition_id, void *priv);
static int ve2_mgmt_handshake_init(struct amdxdna_mgmtctx *mgmtctx,
				   struct amdxdna_hwctx *hwctx);
static int notify_fw_cmd_ready(struct amdxdna_mgmtctx *mgmtctx);

/*
 * ve2_xrs_col_list - Build the candidate start-column list for the XRS solver.
 * @hwctx: hardware context (uses @num_tiles as the AIE column count).
 * @total_col: total number of AIE columns available on the device.
 *
 * VE2 has no clock/QoS/DPM model, so the solver only needs a set of aligned
 * start columns it may place the partition at. Enumerate every column that is a
 * multiple of VE2_MIN_COL_SUPPORT and can host @num_tiles columns. This lets
 * multiple contexts land on different columns instead of all contending for
 * column 0. The list is consumed by amdxdna_alloc_resource() and freed by the
 * caller (ve2_xrs_request()).
 */
static int ve2_xrs_col_list(struct amdxdna_hwctx *hwctx, u32 total_col)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	u32 num_col = hwctx->num_tiles;
	u32 entries = 0;
	u32 start;
	u32 i;

	if (!num_col || num_col > total_col) {
		XDNA_ERR(xdna, "Invalid num_col %u (total_col %u)", num_col, total_col);
		return -EINVAL;
	}

	for (start = 0; start + num_col <= total_col; start += VE2_MIN_COL_SUPPORT)
		entries++;

	if (!entries) {
		XDNA_ERR(xdna, "No valid start col for num_col %u in total_col %u",
			 num_col, total_col);
		return -EINVAL;
	}

	hwctx->col_list = kmalloc_array(entries, sizeof(*hwctx->col_list), GFP_KERNEL);
	if (!hwctx->col_list)
		return -ENOMEM;

	hwctx->col_list_len = entries;
	for (i = 0, start = 0; i < entries; i++, start += VE2_MIN_COL_SUPPORT)
		hwctx->col_list[i] = start;

	print_hex_dump_debug("col_list: ", DUMP_PREFIX_OFFSET, 16, 4, hwctx->col_list,
			     entries * sizeof(*hwctx->col_list), false);
	return 0;
}

static int ve2_fifo_enqueue(struct amdxdna_mgmtctx *mgmtctx,
			    struct amdxdna_hwctx *hwctx, u64 command_index)
{
	struct amdxdna_ctx_priv *vp = ve2_hw_priv(hwctx);
	struct ve2_ctx_fifo_entry *entry;
	unsigned long flags;

	if (!vp)
		return -EINVAL;

	if (vp->in_fifo)
		return 0;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->ctx = hwctx;
	entry->command_index = command_index;
	entry->next = NULL;

	spin_lock_irqsave(&mgmtctx->fifo_lock, flags);
	if (!mgmtctx->fifo_tail) {
		mgmtctx->fifo_head = entry;
		mgmtctx->fifo_tail = entry;
	} else {
		mgmtctx->fifo_tail->next = entry;
		mgmtctx->fifo_tail = entry;
	}
	vp->in_fifo = true;
	spin_unlock_irqrestore(&mgmtctx->fifo_lock, flags);

	return 0;
}

static void pop_from_ctx_command_fifo_till(struct amdxdna_mgmtctx *mgmtctx,
				  struct amdxdna_hwctx *active_ctx,
				  u64 read_index)
{
	struct ve2_ctx_fifo_entry *entry;
	struct amdxdna_ctx_priv *vp;
	unsigned long flags;

	while (1) {
		spin_lock_irqsave(&mgmtctx->fifo_lock, flags);
		entry = mgmtctx->fifo_head;
		if (!entry || entry->ctx != active_ctx) {
			spin_unlock_irqrestore(&mgmtctx->fifo_lock, flags);
			break;
		}

		if (entry->command_index > read_index) {
			spin_unlock_irqrestore(&mgmtctx->fifo_lock, flags);
			break;
		}

		mgmtctx->fifo_head = entry->next;
		if (!mgmtctx->fifo_head)
			mgmtctx->fifo_tail = NULL;
		vp = ve2_hw_priv(entry->ctx);
		if (vp)
			vp->in_fifo = false;
		kfree(entry);
		spin_unlock_irqrestore(&mgmtctx->fifo_lock, flags);
	}
}

static int get_ctx_read_index(struct amdxdna_hwctx *hwctx, u64 *read_index)
{
	struct amdxdna_ctx_priv *vp;
	u64 *index_ptr;

	if (!hwctx || !read_index)
		return -EINVAL;

	vp = ve2_hw_priv(hwctx);
	if (!vp || !vp->hsa_queue.hsa_queue_p)
		return -EINVAL;

	hsa_queue_sync_read_index_for_read(&vp->hsa_queue);
	index_ptr = (u64 *)((char *)vp->hsa_queue.hsa_queue_p + HSA_QUEUE_READ_INDEX_OFFSET);
	*read_index = *index_ptr;

	return 0;
}

static bool ve2_check_idle(struct amdxdna_mgmtctx *mgmtctx)
{
	u32 idle_status = 0;

	ve2_aie_read_idle(mgmtctx, &idle_status);

	if (idle_status & CERT_IS_IDLE) {
		XDNA_DBG(mgmtctx->xdna,
			 "%s: active hwctx %p cert_idle_status:%x -->FOUND\n",
			 __func__, mgmtctx->active_ctx, idle_status);
		return true;
	}
	XDNA_DBG(mgmtctx->xdna,
		 "%s: active hwctx %p cert_idle_status:%x -->NOT Found\n",
		 __func__, mgmtctx->active_ctx, idle_status);

	return false;
}

static bool ve2_check_idle_or_queue_not_empty(struct amdxdna_mgmtctx *mgmtctx)
{
	u32 cert_idle_status = 0;

	ve2_partition_read_privileged_mem(mgmtctx, offsetof(struct handshake, cert_idle_status),
					  sizeof(cert_idle_status), &cert_idle_status);

	if (cert_idle_status & HSA_QUEUE_NOT_EMPTY || cert_idle_status & CERT_IS_IDLE) {
		XDNA_DBG(mgmtctx->xdna,
			 "%s: active hwctx %p cert_idle_status:%x -> FOUND\n",
			 __func__, mgmtctx->active_ctx, cert_idle_status);
		return true;
	}
	XDNA_DBG(mgmtctx->xdna,
		 "%s: active hwctx %p cert_idle_status:%x -> Not Found\n",
		 __func__, mgmtctx->active_ctx, cert_idle_status);
	return false;
}

static bool ve2_check_misc_interrupt(struct amdxdna_mgmtctx *mgmtctx)
{
	u32 off = CERT_HANDSHAKE_OFF(mgmtctx->start_col) +
		  offsetof(struct handshake, misc_status);
	u32 misc_status = 0;
	struct amdxdna_ctx_priv *vp;
	int ret;

	ret = aie_partition_read_privileged_mem(mgmtctx->aie_dev, off,
						sizeof(misc_status), &misc_status);
	if (ret || !misc_status)
		return false;

	if (mgmtctx->active_ctx) {
		vp = ve2_hw_priv(mgmtctx->active_ctx);
		if (vp)
			vp->misc_intrpt_flag = true;
	}

	return true;
}

static void cert_setup_partition(struct amdxdna_mgmtctx *mgmtctx,
				 struct amdxdna_hwctx *hwctx, u32 col,
				 struct handshake *cert_hs)
{
	struct amdxdna_ctx_priv *vp = ve2_hw_priv(hwctx);
	u64 hsa_addr = U64_MAX;

	if (col == 0 && vp && vp->hsa_queue.hsa_queue_dma_addr)
		hsa_addr = vp->hsa_queue.hsa_queue_dma_addr;

	cert_hs->partition_base_address = VE2_ADDR(mgmtctx->start_col, 0, 0);
	cert_hs->aie_info.partition_size = mgmtctx->num_col;
	cert_hs->hsa_addr_high = upper_32_bits(hsa_addr);
	cert_hs->hsa_addr_low = lower_32_bits(hsa_addr);

	cert_hs->ctx_switch_req = 0;
	cert_hs->hsa_location = 0;
	cert_hs->mpaie_alive = ALIVE_MAGIC;
}

static void ve2_free_hs_data(struct aie_op_handshake_data *hs_data, u32 max_cols)
{
	if (!hs_data)
		return;

	for (u32 col = 0; col < max_cols; col++) {
		kfree(hs_data[col].addr);
		hs_data[col].addr = NULL;
	}
	kfree(hs_data);
}

static struct aie_op_handshake_data *
ve2_prepare_hs_data(struct amdxdna_mgmtctx *mgmtctx, struct amdxdna_hwctx *hwctx, bool init)
{
	struct amdxdna_dev *xdna = mgmtctx->xdna;
	struct aie_op_handshake_data *hs_data;
	u32 num_col = mgmtctx->num_col;
	struct aie_location aie_loc;

	hs_data = kmalloc_array(num_col, sizeof(*hs_data), GFP_KERNEL);
	if (!hs_data) {
		XDNA_ERR(xdna, "No memory for handshake data allocation");
		return NULL;
	}

	for (u32 col = 0; col < num_col; col++) {
		struct handshake *cert_hs;

		aie_loc.col = col;
		aie_loc.row = 0;
		cert_hs = kzalloc(sizeof(*cert_hs), GFP_KERNEL);
		if (!cert_hs) {
			XDNA_ERR(xdna, "No memory for cert hs packet");
			ve2_free_hs_data(hs_data, col);
			return NULL;
		}
		if (init)
			cert_setup_partition(mgmtctx, hwctx, col, cert_hs);

		hs_data[col].addr = cert_hs;
		hs_data[col].size = sizeof(struct handshake);
		hs_data[col].offset = 0x0;
		hs_data[col].loc = aie_loc;
	}

	return hs_data;
}

static int notify_fw_cmd_ready(struct amdxdna_mgmtctx *mgmtctx)
{
	u32 event_val = VE2_USER_EVENT_ID;
	struct aie_location loc = { .col = 0, .row = 0 };
	int ret;

	ret = aie_partition_write(mgmtctx->aie_dev, loc, VE2_EVENT_GENERATE_REG,
				  sizeof(event_val), &event_val, 0);
	if (ret < 0)
		return ret;

	/* aie_partition_write returns bytes written on success (typically 4). */
	return 0;
}

static int ve2_mgmt_handshake_init(struct amdxdna_mgmtctx *mgmtctx,
				   struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev *xdna = mgmtctx->xdna;
	struct amdxdna_ctx_priv *vp = ve2_hw_priv(hwctx);
	struct aie_partition_init_args args = { };
	struct aie_op_handshake_data *hs_data;
	u32 num_col = mgmtctx->num_col;
	int col, ret;

	if (!vp)
		return -EINVAL;

	hs_data = ve2_prepare_hs_data(mgmtctx, hwctx, true);
	if (!hs_data) {
		XDNA_ERR(xdna, "preparing cert handshake data failed");
		return -ENOMEM;
	}

	args.handshake = hs_data;
	args.handshake_cols = num_col;
	args.locs = NULL;
	args.num_tiles = 0;
	args.init_opts = (AIE_PART_INIT_OPT_DEFAULT | AIE_PART_INIT_OPT_HANDSHAKE |
			  AIE_PART_INIT_OPT_DIS_TLAST_ERROR) &
			 ~AIE_PART_INIT_OPT_UC_ENB_MEM_PRIV;

	ret = aie_partition_initialize(mgmtctx->aie_dev, &args);
	if (ret < 0) {
		XDNA_ERR(xdna, "aie partition init failed: %d", ret);
		goto release_hs_data;
	}

	for (col = num_col - 1; col >= 0; col--) {
		struct aie_location loc = { .col = col, .row = 0 };

		ret = aie_partition_uc_wakeup(mgmtctx->aie_dev, &loc);
		if (ret)
			goto release_hs_data;
	}

	vp->handshake_initialized = true;
	ret = 0;

release_hs_data:
	ve2_free_hs_data(hs_data, num_col);
	return ret;
}

int ve2_mgmt_schedule_cmd(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx,
			  u64 command_index)
{
	struct amdxdna_ctx_priv *vp = ve2_hw_priv(hwctx);
	struct amdxdna_mgmtctx *mgmtctx;
	int ret;

	if (!vp || !vp->mgmtctx)
		return -EINVAL;

	mgmtctx = vp->mgmtctx;

	guard(mutex)(&mgmtctx->ctx_lock);

	if (!mgmtctx->active_ctx) {
		mgmtctx->active_ctx = hwctx;
		if (!vp->handshake_initialized) {
			ret = ve2_mgmt_handshake_init(mgmtctx, hwctx);
			if (ret) {
				mgmtctx->active_ctx = NULL;
				return ret;
			}
		}
	}

	ret = ve2_fifo_enqueue(mgmtctx, hwctx, command_index);
	if (ret)
		return ret;

	return notify_fw_cmd_ready(mgmtctx);
}

static void ve2_scheduler_work(struct work_struct *work)
{
	struct amdxdna_mgmtctx *mgmtctx = container_of(work, struct amdxdna_mgmtctx,
						       scheduler_work);
	struct amdxdna_ctx_priv *vp;

	guard(mutex)(&mgmtctx->ctx_lock);

	if (!mgmtctx->active_ctx)
		return;

	vp = ve2_hw_priv(mgmtctx->active_ctx);
	if (!vp)
		return;

	if (vp->misc_intrpt_flag) {
		XDNA_ERR(mgmtctx->xdna, "MISC interrupt from firmware");
	} else if (ve2_check_idle(mgmtctx)) {
		XDNA_DBG(mgmtctx->xdna, "Partition now idle, no pending contexts");
	} else {
		XDNA_DBG(mgmtctx->xdna, "Scheduler: no action needed, active_ctx=%p",
			 mgmtctx->active_ctx);
	}
	XDNA_DBG(mgmtctx->xdna, "scheduler: exit start_col=%u hwctx=%p pid=%d",
		 mgmtctx->start_col, mgmtctx->active_ctx,
		 mgmtctx->active_ctx->client->pid);
}

static void ve2_irq_handler(u32 partition_id, void *priv)
{
	struct amdxdna_mgmtctx *mgmtctx = priv;
	struct amdxdna_hwctx *active_ctx;
	struct amdxdna_ctx_priv *vp;
	u64 read_index;

	guard(mutex)(&mgmtctx->ctx_lock);
	active_ctx = mgmtctx->active_ctx;
	if (!active_ctx)
		return;

	if (get_ctx_read_index(active_ctx, &read_index)) {
		XDNA_ERR(mgmtctx->xdna, "IRQ: failed to get read index");
		return;
	}
	pop_from_ctx_command_fifo_till(mgmtctx, active_ctx, read_index);
	vp = ve2_hw_priv(active_ctx);
	if (vp)
		wake_up_interruptible_all(&vp->waitq);

	if (mgmtctx->work_queue &&
	    (ve2_check_idle_or_queue_not_empty(mgmtctx) || ve2_check_misc_interrupt(mgmtctx))) {
		XDNA_DBG(mgmtctx->xdna, "IRQ: queueing sched_work start_col=%u", mgmtctx->start_col);
		queue_work(mgmtctx->work_queue, &mgmtctx->scheduler_work);
	} else {
		XDNA_DBG(mgmtctx->xdna, "IRQ: sched_work not queued start_col=%u (wq=%p)",
			 mgmtctx->start_col, mgmtctx->work_queue);
	}
	XDNA_DBG(mgmtctx->xdna, "completion IRQ: exit read_index=%llu hwctx=%p pid=%d",
		 read_index, active_ctx, active_ctx->client->pid);
}

static int ve2_create_mgmt_partition(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx,
				     u32 start_col, u32 num_col, u32 *partition_id)
{
	struct amdxdna_dev_hdl *xdna_hdl = xdna->dev_handle;
	struct amdxdna_ctx_priv *vp = ve2_hw_priv(hwctx);
	struct amdxdna_mgmtctx *mgmtctx;
	struct aie_partition_req req = { };
	struct device *aie_dev;
	int ret;

	if (!vp || start_col >= xdna_hdl->aie_dev_info.cols)
		return -EINVAL;

	if (vp->mgmtctx)
		return -EBUSY;

	mgmtctx = kzalloc(sizeof(*mgmtctx), GFP_KERNEL);
	if (!mgmtctx)
		return -ENOMEM;

	mgmtctx->xdna = xdna;
	mgmtctx->start_col = start_col;
	mgmtctx->num_col = num_col;
	mgmtctx->active_ctx = NULL;
	mgmtctx->fifo_head = NULL;
	mgmtctx->fifo_tail = NULL;
	spin_lock_init(&mgmtctx->fifo_lock);
	mutex_init(&mgmtctx->ctx_lock);

	mgmtctx->work_queue = create_singlethread_workqueue("ve2_aie_sched");
	if (!mgmtctx->work_queue) {
		ret = -ENOMEM;
		goto free_mgmtctx;
	}

	INIT_WORK(&mgmtctx->scheduler_work, ve2_scheduler_work);

	req.partition_id = (start_col << AIE_PART_ID_START_COL_SHIFT) |
			   (num_col << AIE_PART_ID_NUM_COLS_SHIFT);
	req.user_event1_complete = ve2_irq_handler;
	req.user_event1_priv = mgmtctx;
	aie_dev = aie_partition_request(&req);
	if (IS_ERR(aie_dev)) {
		ret = PTR_ERR(aie_dev);
		goto destroy_wq;
	}

	mgmtctx->aie_dev = aie_dev;

	*partition_id = req.partition_id;
	vp->mgmtctx = mgmtctx;

	return 0;

destroy_wq:
	destroy_workqueue(mgmtctx->work_queue);
free_mgmtctx:
	kfree(mgmtctx);
	return ret;
}

int ve2_xrs_request(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev_hdl *hdl = ve2_dev_hdl(xdna);
	struct amdxdna_ctx_priv *vp = ve2_hw_priv(hwctx);
	u32 partition_id = 0;
	int ret;

	if (!vp || !hdl)
		return -EINVAL;

	/* Build the candidate start-column list, then let XRS place the partition. */
	ret = ve2_xrs_col_list(hwctx, hdl->aie_dev_info.cols);
	if (ret)
		return ret;

	ret = amdxdna_alloc_resource(hwctx);
	kfree(hwctx->col_list);
	hwctx->col_list = NULL;
	if (ret) {
		XDNA_ERR(xdna, "XRS resource request failed, ret %d", ret);
		return ret;
	}

	ret = ve2_create_mgmt_partition(xdna, hwctx, hwctx->start_col, hwctx->num_col,
					&partition_id);
	if (ret) {
		XDNA_ERR(xdna, "Creating AIE partition failed, ret %d", ret);
		amdxdna_release_resource(hwctx);
		return ret;
	}

	vp->partition_id = partition_id;
	vp->in_fifo = false;
	vp->handshake_initialized = false;

	return 0;
}

int ve2_mgmt_destroy_partition(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_ctx_priv *vp = hwctx ? ve2_hw_priv(hwctx) : NULL;
	struct ve2_ctx_fifo_entry *entry, *next;
	struct amdxdna_mgmtctx *mgmtctx;
	unsigned long flags;

	mgmtctx = vp ? vp->mgmtctx : NULL;
	if (!mgmtctx) {
		/* No partition was created; still release any XRS reservation. */
		if (hwctx)
			amdxdna_release_resource(hwctx);
		return -EINVAL;
	}

	if (mgmtctx->active_ctx == hwctx)
		mgmtctx->active_ctx = NULL;

	flush_workqueue(mgmtctx->work_queue);

	spin_lock_irqsave(&mgmtctx->fifo_lock, flags);
	entry = mgmtctx->fifo_head;
	while (entry) {
		next = entry->next;
		kfree(entry);
		entry = next;
	}
	mgmtctx->fifo_head = NULL;
	mgmtctx->fifo_tail = NULL;
	spin_unlock_irqrestore(&mgmtctx->fifo_lock, flags);

	aie_partition_teardown(mgmtctx->aie_dev);
	aie_partition_release(mgmtctx->aie_dev);

	destroy_workqueue(mgmtctx->work_queue);
	mutex_destroy(&mgmtctx->ctx_lock);
	kfree(mgmtctx);
	vp->mgmtctx = NULL;

	amdxdna_release_resource(hwctx);

	return 0;
}

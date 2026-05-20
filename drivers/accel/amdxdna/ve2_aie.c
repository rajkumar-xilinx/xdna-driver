// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024-2026, Advanced Micro Devices, Inc.
 *
 * VE2 AIE firmware backend — Linux xlnx-aie partition APIs (temporal sharing).
 * Implements amdxdna_hal_ops; registered from ve2_aux.c at device probe.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>
#include <linux/xlnx-ai-engine.h>

#include "amdxdna_ctx.h"
#include "amdxdna_drv.h"
#include "amdxdna_hal.h"
#include "ve2_aie.h"
#include "ve2_aux.h"
#include "ve2_handshake.h"
#include "ve2_hq.h"
#include "ve2_hwctx.h"
#include "ve2_host_queue.h"

#define VE2_COL_SHIFT			25
#define VE2_ROW_SHIFT			20
#define VE2_ADDR(col, row, off) \
	(((col) << VE2_COL_SHIFT) + ((row) << VE2_ROW_SHIFT) + (off))
#define VE2_HANDSHAKE_OFF		0x88000
#define CERT_HANDSHAKE_OFF(col)		VE2_ADDR(col, 0, VE2_HANDSHAKE_OFF)
#define VE2_EVENT_GENERATE_REG		0x00034008
#define VE2_USER_EVENT_ID		0xB6

static int ve2_aie_read_idle(struct ve2_aie_mgmtctx *mgmtctx, u32 *idle)
{
	u32 off = CERT_HANDSHAKE_OFF(mgmtctx->start_col) +
		  offsetof(struct handshake, cert_idle_status);

	return aie_partition_read_privileged_mem(mgmtctx->aie_dev, off,
						sizeof(*idle), idle);
}

static void ve2_aie_scheduler_work(struct work_struct *work);
static void ve2_aie_irq_handler(u32 partition_id, void *priv);
static int ve2_aie_activate_context(struct ve2_aie_mgmtctx *mgmtctx,
				    struct ve2_aie_context *hal_ctx);
static int ve2_aie_schedule_context(struct ve2_aie_mgmtctx *mgmtctx,
				    struct ve2_aie_context *hal_ctx);

static void ve2_fifo_enqueue(struct ve2_aie_mgmtctx *mgmtctx,
			      struct ve2_aie_context *hal_ctx)
{
	struct ve2_ctx_fifo_entry *entry;
	unsigned long flags;

	if (hal_ctx->in_fifo)
		return;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return;

	entry->ctx = hal_ctx;
	entry->command_index = hal_ctx->write_idx;
	entry->next = NULL;

	spin_lock_irqsave(&mgmtctx->fifo_lock, flags);
	if (!mgmtctx->fifo_tail) {
		mgmtctx->fifo_head = entry;
		mgmtctx->fifo_tail = entry;
	} else {
		mgmtctx->fifo_tail->next = entry;
		mgmtctx->fifo_tail = entry;
	}
	hal_ctx->in_fifo = true;
	spin_unlock_irqrestore(&mgmtctx->fifo_lock, flags);
}

static struct ve2_aie_context *ve2_fifo_dequeue(struct ve2_aie_mgmtctx *mgmtctx)
{
	struct ve2_ctx_fifo_entry *entry;
	struct ve2_aie_context *hal_ctx = NULL;
	unsigned long flags;

	spin_lock_irqsave(&mgmtctx->fifo_lock, flags);
	entry = mgmtctx->fifo_head;
	if (entry) {
		mgmtctx->fifo_head = entry->next;
		if (!mgmtctx->fifo_head)
			mgmtctx->fifo_tail = NULL;
		hal_ctx = entry->ctx;
		if (hal_ctx)
			hal_ctx->in_fifo = false;
		kfree(entry);
	}
	spin_unlock_irqrestore(&mgmtctx->fifo_lock, flags);

	return hal_ctx;
}

static bool ve2_fifo_empty(struct ve2_aie_mgmtctx *mgmtctx)
{
	bool empty;
	unsigned long flags;

	spin_lock_irqsave(&mgmtctx->fifo_lock, flags);
	empty = (mgmtctx->fifo_head == NULL);
	spin_unlock_irqrestore(&mgmtctx->fifo_lock, flags);

	return empty;
}

static bool ve2_check_idle(struct ve2_aie_mgmtctx *mgmtctx)
{
	u32 idle_status = 0;
	int ret;

	ret = ve2_aie_read_idle(mgmtctx, &idle_status);
	if (ret)
		return false;

	return idle_status == 1;
}

static int ve2_aie_prepare_handshake(struct ve2_aie_context *hal_ctx,
				struct aie_partition_init_args *args)
{
	struct ve2_aie_mgmtctx *mgmtctx = hal_ctx->mgmtctx;
	struct handshake *hs;
	struct aie_op_handshake_data *hs_data;
	struct aie_location aie_loc;
	u32 col;
	int ret;

	hs = kcalloc(mgmtctx->num_col, sizeof(*hs), GFP_KERNEL);
	if (!hs)
		return -ENOMEM;

	hs_data = kcalloc(mgmtctx->num_col, sizeof(*hs_data), GFP_KERNEL);
	if (!hs_data) {
		kfree(hs);
		return -ENOMEM;
	}

	for (col = 0; col < mgmtctx->num_col; col++) {
		memset(&hs[col], 0, sizeof(hs[col]));
		hs[col].partition_base_address = mgmtctx->start_col + col;
		hs[col].aie_info.partition_size = mgmtctx->num_col;
		hs[col].hsa_addr_high = upper_32_bits(hal_ctx->hsa_queue_pa);
		hs[col].hsa_addr_low = lower_32_bits(hal_ctx->hsa_queue_pa);
		hs[col].mpaie_alive = ALIVE_MAGIC;

		if (hal_ctx->hwctx)
			ve2_hwctx_fill_hs_config(hal_ctx->hwctx, &hs[col], col);

		aie_loc.col = mgmtctx->start_col + col;
		aie_loc.row = 0;
		hs_data[col].addr = &hs[col];
		hs_data[col].offset = 0;
		hs_data[col].size = sizeof(hs[col]);
		hs_data[col].loc = aie_loc;
	}

	ret = aie_partition_handshake_update(mgmtctx->aie_dev, hs_data, mgmtctx->num_col);
	if (ret) {
		kfree(hs_data);
		kfree(hs);
		return ret;
	}

	args->handshake = hs_data;
	args->handshake_cols = mgmtctx->num_col;
	args->locs = NULL;
	args->num_tiles = 0;
	args->init_opts = (AIE_PART_INIT_OPT_DEFAULT | AIE_PART_INIT_OPT_DIS_TLAST_ERROR) &
			   ~AIE_PART_INIT_OPT_UC_ENB_MEM_PRIV;

	kfree(mgmtctx->handshake);
	mgmtctx->handshake = hs;

	return 0;
}

static int ve2_aie_notify_firmware(struct ve2_aie_mgmtctx *mgmtctx)
{
	u32 event_val = VE2_USER_EVENT_ID;
	struct aie_location loc = { .col = 0, .row = 0 };

	return aie_partition_write(mgmtctx->aie_dev, loc, VE2_EVENT_GENERATE_REG,
				   sizeof(event_val), &event_val, 0);
}

static int ve2_aie_activate_context(struct ve2_aie_mgmtctx *mgmtctx,
				    struct ve2_aie_context *hal_ctx)
{
	struct aie_partition_init_args args = { };
	int col, ret = 0;

	ret = ve2_aie_prepare_handshake(hal_ctx, &args);
	if (ret)
		return ret;

	ret = aie_partition_initialize(mgmtctx->aie_dev, &args);
	if (ret) {
		kfree(mgmtctx->handshake);
		mgmtctx->handshake = NULL;
		goto cleanup_hs;
	}

	for (col = 0; col < mgmtctx->num_col; col++) {
		struct aie_location loc = { .col = mgmtctx->start_col + col, .row = 0 };

		ret = aie_partition_uc_wakeup(mgmtctx->aie_dev, &loc);
		if (ret)
			goto cleanup_hs;
	}

	ret = ve2_aie_notify_firmware(mgmtctx);
	if (ret)
		goto cleanup_hs;

	hal_ctx->handshake_initialized = true;

cleanup_hs:
	kfree(args.handshake);
	if (ret) {
		kfree(mgmtctx->handshake);
		mgmtctx->handshake = NULL;
	}
	return ret;
}

static int ve2_aie_schedule_context(struct ve2_aie_mgmtctx *mgmtctx,
				    struct ve2_aie_context *hal_ctx)
{
	int ret;

	if (!mgmtctx->active_ctx) {
		if (!ve2_check_idle(mgmtctx)) {
			ve2_fifo_enqueue(mgmtctx, hal_ctx);
			return 0;
		}

		mgmtctx->active_ctx = hal_ctx;
		ret = ve2_aie_activate_context(mgmtctx, hal_ctx);
		if (ret) {
			mgmtctx->active_ctx = NULL;
			return ret;
		}
		return 0;
	}

	if (mgmtctx->active_ctx != hal_ctx) {
		ve2_fifo_enqueue(mgmtctx, hal_ctx);
		return 0;
	}

	return ve2_aie_notify_firmware(mgmtctx);
}

static void ve2_aie_scheduler_work(struct work_struct *work)
{
	struct ve2_aie_mgmtctx *mgmtctx = container_of(work, struct ve2_aie_mgmtctx,
						       scheduler_work);
	struct ve2_aie_context *next_ctx;
	int timeout = 100;

	mgmtctx->switching = true;

	while (timeout-- > 0) {
		if (ve2_check_idle(mgmtctx))
			break;
		msleep(1);
	}

	if (timeout <= 0) {
		mgmtctx->switching = false;
		return;
	}

	next_ctx = ve2_fifo_dequeue(mgmtctx);
	if (!next_ctx) {
		mgmtctx->active_ctx = NULL;
		mgmtctx->switching = false;
		return;
	}

	mgmtctx->active_ctx = next_ctx;
	if (ve2_aie_activate_context(mgmtctx, next_ctx))
		mgmtctx->active_ctx = NULL;

	mgmtctx->switching = false;
}

static void ve2_aie_irq_handler(u32 partition_id, void *priv)
{
	struct ve2_aie_mgmtctx *mgmtctx = priv;
	struct ve2_aie_context *active_ctx = mgmtctx->active_ctx;

	(void)partition_id;

	if (!active_ctx)
		return;

	active_ctx->read_idx = active_ctx->write_idx;
	wake_up_all(&active_ctx->waitq);

	if (active_ctx->hwctx) {
		struct ve2_hwctx_priv *vp = ve2_hw_priv(active_ctx->hwctx);

		if (vp)
			wake_up_interruptible_all(&vp->waitq);
	}

	if (!ve2_fifo_empty(mgmtctx) && !mgmtctx->switching)
		queue_work(mgmtctx->work_queue, &mgmtctx->scheduler_work);
}

static int ve2_aie_init(struct amdxdna_hal_device *hal_dev)
{
	(void)hal_dev;
	return 0;
}

static void ve2_aie_fini(struct amdxdna_hal_device *hal_dev)
{
	(void)hal_dev;
}

static int ve2_aie_get_device_info(struct amdxdna_hal_device *hal_dev,
				   struct amdxdna_hal_device_info *info)
{
	struct amdxdna_dev *xdna = amdxdna_hal_get_priv(hal_dev);
	struct amdxdna_dev_hdl *xdna_hdl = xdna->dev_handle;

	info->cols = xdna_hdl->aie_dev_info.cols;
	info->rows = xdna_hdl->aie_dev_info.rows;
	info->version = 0;
	info->core_rows = 0;
	info->mem_rows = 0;
	info->shim_rows = 0;
	info->dev_mem_base = 0;
	info->dev_mem_size = 0;

	return 0;
}

static int ve2_aie_get_firmware_version(struct amdxdna_hal_device *hal_dev,
					struct amdxdna_hal_fw_version *ver)
{
	struct amdxdna_dev *xdna = amdxdna_hal_get_priv(hal_dev);
	struct amdxdna_dev_hdl *hdl = xdna->dev_handle;

	ver->major = hdl->fw_version.major;
	ver->minor = hdl->fw_version.minor;
	ver->sub = hdl->fw_version.hotfix;
	ver->build = hdl->fw_version.build;

	return 0;
}

static struct ve2_aie_mgmtctx *ve2_aie_find_mgmtctx(struct amdxdna_dev_hdl *hdl,
						    u32 partition_id)
{
	u32 i;

	if (!hdl->hal_mgmt_slot)
		return NULL;

	for (i = 0; i < hdl->aie_dev_info.cols; i++) {
		struct ve2_aie_mgmtctx *m = hdl->hal_mgmt_slot[i];

		if (m && m->partition_id == partition_id)
			return m;
	}
	return NULL;
}

static int ve2_aie_partition_create(struct amdxdna_hal_device *hal_dev,
				  struct amdxdna_hal_partition_param *param,
				  struct amdxdna_hal_partition_info *info)
{
	struct amdxdna_dev *xdna = amdxdna_hal_get_priv(hal_dev);
	struct amdxdna_dev_hdl *xdna_hdl = xdna->dev_handle;
	struct ve2_aie_mgmtctx *mgmtctx;
	struct aie_partition_req req = { };
	struct device *aie_dev;
	u32 start = param->partition_col_start;
	int ret;

	if (!xdna_hdl->hal_mgmt_slot || start >= xdna_hdl->aie_dev_info.cols)
		return -EINVAL;

	if (xdna_hdl->hal_mgmt_slot[start])
		return -EBUSY;

	mgmtctx = kzalloc(sizeof(*mgmtctx), GFP_KERNEL);
	if (!mgmtctx)
		return -ENOMEM;

	mgmtctx->xdna = xdna;
	mgmtctx->start_col = param->partition_col_start;
	mgmtctx->num_col = param->partition_col_count;
	mgmtctx->active_ctx = NULL;
	mgmtctx->fifo_head = NULL;
	mgmtctx->fifo_tail = NULL;
	mgmtctx->switching = false;
	spin_lock_init(&mgmtctx->fifo_lock);

	mgmtctx->work_queue = create_singlethread_workqueue("ve2_aie_sched");
	if (!mgmtctx->work_queue) {
		ret = -ENOMEM;
		goto free_mgmtctx;
	}

	INIT_WORK(&mgmtctx->scheduler_work, ve2_aie_scheduler_work);

	req.partition_id = ((param->partition_col_start) << AIE_PART_ID_START_COL_SHIFT) |
			   ((param->partition_col_count) << AIE_PART_ID_NUM_COLS_SHIFT);
	req.user_event1_complete = ve2_aie_irq_handler;
	req.user_event1_priv = mgmtctx;

	aie_dev = aie_partition_request(&req);
	if (IS_ERR(aie_dev)) {
		ret = PTR_ERR(aie_dev);
		goto destroy_wq;
	}

	mgmtctx->aie_dev = aie_dev;
	mgmtctx->partition_id = req.partition_id;

	info->partition_id = req.partition_id;
	xdna_hdl->hal_mgmt_slot[start] = mgmtctx;

	return 0;

destroy_wq:
	destroy_workqueue(mgmtctx->work_queue);
free_mgmtctx:
	kfree(mgmtctx);
	return ret;
}

static int ve2_aie_partition_destroy(struct amdxdna_hal_device *hal_dev,
				   u32 partition_id)
{
	struct amdxdna_dev *xdna = amdxdna_hal_get_priv(hal_dev);
	struct amdxdna_dev_hdl *xdna_hdl = xdna->dev_handle;
	struct ve2_aie_mgmtctx *mgmtctx;
	struct ve2_ctx_fifo_entry *entry, *next;
	unsigned long flags;
	u32 i;

	mgmtctx = ve2_aie_find_mgmtctx(xdna_hdl, partition_id);
	if (!mgmtctx)
		return -EINVAL;

	for (i = 0; i < xdna_hdl->aie_dev_info.cols; i++) {
		if (xdna_hdl->hal_mgmt_slot[i] == mgmtctx)
			xdna_hdl->hal_mgmt_slot[i] = NULL;
	}

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
	kfree(mgmtctx->handshake);
	kfree(mgmtctx);

	return 0;
}

static int ve2_aie_context_create(struct amdxdna_hal_device *hal_dev,
				  struct amdxdna_hal_context_param *param,
				  struct amdxdna_hal_context **context)
{
	struct amdxdna_dev *xdna = amdxdna_hal_get_priv(hal_dev);
	struct amdxdna_dev_hdl *xdna_hdl = xdna->dev_handle;
	struct ve2_aie_context *hal_ctx;
	struct ve2_aie_mgmtctx *mgmtctx;

	if (param->start_col >= xdna_hdl->aie_dev_info.cols)
		return -EINVAL;

	mgmtctx = xdna_hdl->hal_mgmt_slot[param->start_col];
	if (!mgmtctx)
		return -EINVAL;

	hal_ctx = kzalloc(sizeof(*hal_ctx), GFP_KERNEL);
	if (!hal_ctx)
		return -ENOMEM;

	hal_ctx->hal_dev = hal_dev;
	hal_ctx->ctx = param->ctx_handle;
	hal_ctx->mgmtctx = mgmtctx;
	hal_ctx->hwctx = param->ctx_handle;

	{
		struct amdxdna_hwctx *hwctx = param->ctx_handle;
		struct ve2_hwctx_priv *vp = hwctx ? ve2_hw_priv(hwctx) : NULL;
		struct ve2_hwctx_link *link = hwctx ? hwctx->aux_ctx_priv : NULL;
		u32 mem_bitmap = link ? link->mem_bitmap : 0;

		hal_ctx->hsa_dma_dev = ve2_dma_dev(xdna, mem_bitmap);

		if (vp && vp->hsa_queue.hsa_queue_p) {
			hal_ctx->hsa_queue_va = vp->hsa_queue.hsa_queue_p;
			hal_ctx->hsa_queue_pa = vp->hsa_queue.hsa_queue_mem.dma_addr;
			hal_ctx->hsa_queue_size = sizeof(struct hsa_queue) +
				HOST_QUEUE_ENTRY * sizeof(u64);
			if (vp->hsa_queue.alloc_dev)
				hal_ctx->hsa_dma_dev = vp->hsa_queue.alloc_dev;
			goto hsa_ready;
		}
	}

	hal_ctx->hsa_queue_size = PAGE_SIZE;
	hal_ctx->hsa_queue_va = dma_alloc_coherent(hal_ctx->hsa_dma_dev, hal_ctx->hsa_queue_size,
						   &hal_ctx->hsa_queue_pa, GFP_KERNEL);
	if (!hal_ctx->hsa_queue_va) {
		kfree(hal_ctx);
		return -ENOMEM;
	}

	memset(hal_ctx->hsa_queue_va, 0, hal_ctx->hsa_queue_size);
hsa_ready:
	hal_ctx->write_idx = 0;
	hal_ctx->read_idx = 0;

	init_waitqueue_head(&hal_ctx->waitq);
	hal_ctx->in_fifo = false;
	hal_ctx->handshake_initialized = false;

	*context = (struct amdxdna_hal_context *)hal_ctx;

	return 0;
}

static void ve2_aie_context_destroy(struct amdxdna_hal_context *context)
{
	struct ve2_aie_context *hal_ctx = (struct ve2_aie_context *)context;
	struct ve2_aie_mgmtctx *mgmtctx;

	if (!hal_ctx)
		return;

	mgmtctx = hal_ctx->mgmtctx;
	if (mgmtctx && mgmtctx->active_ctx == hal_ctx)
		mgmtctx->active_ctx = NULL;

	if (hal_ctx->hsa_queue_va && hal_ctx->hsa_dma_dev) {
		struct ve2_hwctx_priv *vp = hal_ctx->hwctx ? ve2_hw_priv(hal_ctx->hwctx) : NULL;
		bool shared = vp && vp->hsa_queue.hsa_queue_p == hal_ctx->hsa_queue_va;

		if (!shared)
			dma_free_coherent(hal_ctx->hsa_dma_dev, hal_ctx->hsa_queue_size,
					  hal_ctx->hsa_queue_va, hal_ctx->hsa_queue_pa);
	}

	kfree(hal_ctx);
}

int ve2_aie_kick_cmd(struct amdxdna_hal_context *context)
{
	struct ve2_aie_context *hal_ctx = (struct ve2_aie_context *)context;

	if (!hal_ctx || !hal_ctx->mgmtctx)
		return -EINVAL;

	return ve2_aie_schedule_context(hal_ctx->mgmtctx, hal_ctx);
}

static int ve2_aie_context_config(struct amdxdna_hal_context *context,
				  struct amdxdna_hal_context_config *config)
{
	(void)context;
	(void)config;
	return -EOPNOTSUPP;
}

static int ve2_aie_job_submit(struct amdxdna_hal_context *context,
			      struct amdxdna_hal_cmd *cmd, u64 *seq)
{
	struct ve2_aie_context *hal_ctx = (struct ve2_aie_context *)context;

	(void)cmd;

	/* Host queue path fills packets in ve2_hq.c; HAL submit is notify-only. */
	if (seq) {
		hal_ctx->write_idx++;
		*seq = hal_ctx->write_idx;
		hal_ctx->last_seq = *seq;
	}

	return ve2_aie_kick_cmd(context);
}

static int ve2_aie_job_wait(struct amdxdna_hal_context *context,
			    u64 seq, u32 timeout_ms)
{
	struct ve2_aie_context *hal_ctx = (struct ve2_aie_context *)context;
	long ret;

	ret = wait_event_interruptible_timeout(hal_ctx->waitq,
					       hal_ctx->read_idx >= seq,
					       msecs_to_jiffies(timeout_ms));

	if (ret == 0)
		return -ETIMEDOUT;
	if (ret < 0)
		return ret;

	return 0;
}

static int ve2_aie_suspend(struct amdxdna_hal_device *hal_dev)
{
	(void)hal_dev;
	return 0;
}

static int ve2_aie_resume(struct amdxdna_hal_device *hal_dev)
{
	(void)hal_dev;
	return 0;
}

static int ve2_aie_debug_op(struct amdxdna_hal_device *hal_dev,
			    enum amdxdna_hal_debug_op_type type,
			    void *param)
{
	(void)hal_dev;
	(void)type;
	(void)param;
	return -EOPNOTSUPP;
}

static const struct amdxdna_hal_ops ve2_aie_hal_ops = {
	.init			= ve2_aie_init,
	.fini			= ve2_aie_fini,
	.get_device_info	= ve2_aie_get_device_info,
	.get_firmware_version	= ve2_aie_get_firmware_version,
	.partition_create	= ve2_aie_partition_create,
	.partition_destroy	= ve2_aie_partition_destroy,
	.context_create		= ve2_aie_context_create,
	.context_destroy	= ve2_aie_context_destroy,
	.context_config		= ve2_aie_context_config,
	.job_submit		= ve2_aie_job_submit,
	.job_wait		= ve2_aie_job_wait,
	.suspend		= ve2_aie_suspend,
	.resume			= ve2_aie_resume,
	.debug_op		= ve2_aie_debug_op,
};

const struct amdxdna_hal_ops *ve2_aie_get_hal_ops(void)
{
	return &ve2_aie_hal_ops;
}

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024-2026, Advanced Micro Devices, Inc.
 *
 * VE2 management backend — XRS resource request, AIE partition lifecycle,
 * and command scheduling via the Linux xlnx-aie partition APIs.
 */

#ifndef _VE2_MGMT_H_
#define _VE2_MGMT_H_

#include <linux/mutex.h>
#include <linux/wait.h>

struct amdxdna_dev;
struct amdxdna_hwctx;

struct ve2_ctx_fifo_entry {
	struct amdxdna_hwctx		*ctx;
	u32				command_index;
	struct ve2_ctx_fifo_entry	*next;
};

struct amdxdna_mgmtctx {
	struct device			*aie_dev;
	struct amdxdna_hwctx		*active_ctx;
	struct ve2_ctx_fifo_entry	*fifo_head;
	struct ve2_ctx_fifo_entry	*fifo_tail;
	struct workqueue_struct		*work_queue;
	struct work_struct		scheduler_work;
	spinlock_t			fifo_lock;/* protect command fifo list */
	struct mutex			ctx_lock;/* protect active_ctx and scheduler */
	u32				start_col;
	u32				num_col;
	struct amdxdna_dev		*xdna;
};

/* Request XRS resources and create the AIE management partition for @hwctx. */
int ve2_xrs_request(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx);

/* Tear down the AIE management partition and release XRS resources. */
int ve2_mgmt_destroy_partition(struct amdxdna_hwctx *hwctx);

/* Schedule a command (host queue commit path, ve2_hq.c). */
int ve2_mgmt_schedule_cmd(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx,
			  u64 command_index);

#endif /* _VE2_MGMT_H_ */

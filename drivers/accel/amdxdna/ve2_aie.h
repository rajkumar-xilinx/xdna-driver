/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024-2026, Advanced Micro Devices, Inc.
 *
 * VE2 firmware backend — Linux xlnx-aie driver (aie_partition_*).
 * Registers with the core HAL via amdxdna_hal_register_backend().
 * struct ve2_aie_context matches struct amdxdna_hal_context_impl prefix for job APIs.
 */

#ifndef _VE2_AIE_H_
#define _VE2_AIE_H_

#include "amdxdna_hal.h"
#include <linux/wait.h>

struct amdxdna_hwctx;
struct ve2_aie_context;

struct ve2_ctx_fifo_entry {
	struct ve2_aie_context		*ctx;
	u32				command_index;
	struct ve2_ctx_fifo_entry	*next;
};

struct ve2_aie_mgmtctx {
	struct device			*aie_dev;
	struct ve2_aie_context		*active_ctx;
	struct ve2_ctx_fifo_entry	*fifo_head;
	struct ve2_ctx_fifo_entry	*fifo_tail;
	struct workqueue_struct		*work_queue;
	struct work_struct		scheduler_work;
	spinlock_t			fifo_lock;
	u32				start_col;
	u32				num_col;
	u32				partition_id;
	void				*handshake;
	struct amdxdna_dev		*xdna;
	bool				switching;
};

/*
 * Binary-compatible prefix with amdxdna_hal_context_impl:
 *   struct amdxdna_hal_device *hal_dev;
 *   void *ctx;
 */
struct ve2_aie_context {
	struct amdxdna_hal_device	*hal_dev;
	void				*ctx;
	struct ve2_aie_mgmtctx		*mgmtctx;
	struct amdxdna_hwctx		*hwctx;

	struct device			*hsa_dma_dev;
	void				*hsa_queue_va;
	dma_addr_t			hsa_queue_pa;
	u32				hsa_queue_size;
	u32				write_idx;
	u32				read_idx;

	wait_queue_head_t		waitq;
	u64				last_seq;

	bool				in_fifo;
	bool				handshake_initialized;
};

const struct amdxdna_hal_ops *ve2_aie_get_hal_ops(void);

/* Notify CERT after host queue commit (Layer 1 command path). */
int ve2_aie_kick_cmd(struct amdxdna_hal_context *context);

#endif /* _VE2_AIE_H_ */

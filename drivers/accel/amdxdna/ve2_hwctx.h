/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * VE2 DRM hardware context: HAL link state and per-column handshake config.
 */

#ifndef _VE2_HWCTX_H_
#define _VE2_HWCTX_H_

#include <linux/types.h>

#include "ve2_handshake.h"

struct amdxdna_hal_context;
struct amdxdna_hwctx;

struct ve2_config_hwctx {
	u32	opcode_timeout_config;
};

/* Links DRM hwctx to HAL partition/context (see amdxdna_hal_create_hwctx). */
struct ve2_hwctx_link {
	u32				mem_bitmap;
	u32				hal_partition_id;
	struct amdxdna_hal_context	*hal_ctx;
	struct ve2_config_hwctx		*col_config;
};

int ve2_hwctx_layer1_init(struct amdxdna_hwctx *hwctx);
void ve2_hwctx_layer1_fini(struct amdxdna_hwctx *hwctx);

int ve2_hwctx_config_opcode_timeout(struct amdxdna_hwctx *hwctx, u32 op_timeout);
void ve2_hwctx_fill_hs_config(struct amdxdna_hwctx *hwctx, struct handshake *hs,
			      u32 col_idx);

#endif /* _VE2_HWCTX_H_ */

// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * VE2 Layer 1 debug / info ioctls (stub — extend when porting test ve2_debug.c).
 */

#include <linux/errno.h>

#include "amdxdna_drv.h"
#include "amdxdna_aux_drv.h"
#include "ve2_debug.h"

int ve2_debug_get_aie_info(struct amdxdna_client *client, struct amdxdna_drm_get_info *args)
{
	(void)client;
	(void)args;
	return -EOPNOTSUPP;
}

int ve2_debug_set_aie_state(struct amdxdna_client *client, struct amdxdna_drm_set_state *args)
{
	(void)client;
	(void)args;
	return -EOPNOTSUPP;
}

int ve2_debug_get_array(struct amdxdna_client *client, struct amdxdna_drm_get_array *args)
{
	(void)client;
	(void)args;
	return -EOPNOTSUPP;
}

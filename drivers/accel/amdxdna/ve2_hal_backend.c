// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Map aux-selected VE2 firmware interface to HAL backend ops (ve2_aie only).
 */

#include "amdxdna_drv.h"
#include "amdxdna_hal.h"
#include "ve2_aie.h"
#include "ve2_hal_backend.h"

const char *ve2_fw_interface_name(enum ve2_fw_interface iface)
{
	switch (iface) {
	case VE2_FW_INTERFACE_AIE:
	default:
		return "aie";
	}
}

const struct amdxdna_hal_ops *
ve2_hal_ops_for_interface(struct amdxdna_dev *xdna, enum ve2_fw_interface iface,
			  enum amdxdna_hal_impl_type *impl_type)
{
	/* Only Xilinx AIE interface is supported for now */
	if (iface != VE2_FW_INTERFACE_AIE) {
		if (xdna)
			XDNA_ERR(xdna, "Unsupported VE2 firmware interface %d", iface);
		return NULL;
	}

	if (impl_type)
		*impl_type = AMDXDNA_HAL_BACKEND_AIE_DRIVER;

	return ve2_aie_get_hal_ops();
}

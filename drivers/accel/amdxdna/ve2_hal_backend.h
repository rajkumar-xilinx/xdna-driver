/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * VE2 HAL firmware-backend selection (aux ID → dev_ve2_info_*; see §5.2).
 */

#ifndef _VE2_HAL_BACKEND_H_
#define _VE2_HAL_BACKEND_H_

#include <linux/types.h>

struct amdxdna_dev;
struct amdxdna_hal_ops;

enum amdxdna_hal_impl_type;

enum ve2_fw_interface {
	VE2_FW_INTERFACE_AIE = 0,
	VE2_FW_INTERFACE_MAILBOX,
};

const struct amdxdna_hal_ops *
ve2_hal_ops_for_interface(struct amdxdna_dev *xdna, enum ve2_fw_interface iface,
			  enum amdxdna_hal_impl_type *impl_type);

const char *ve2_fw_interface_name(enum ve2_fw_interface iface);

#endif /* _VE2_HAL_BACKEND_H_ */

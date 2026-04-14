/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * VE2 auxiliary backend. struct amdxdna_dev_hdl below is VE2-specific; do not
 * include aie2_pci.h in the same translation unit (different dev_hdl layout).
 */

#ifndef _VE2_AUX_H_
#define _VE2_AUX_H_

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <linux/xlnx-ai-engine.h>

#include "amdxdna_drv.h"

struct amdxdna_dev_priv;
#define VE2_MAX_MEM_REGIONS		8

#define VE2_PROG_DATA_MEMORY_OFF	0x80000
#define VE2_CERT_VERSION_OFF		0x50
#define VE2_CERT_VERSION_SIZE		0x40

#define VE2_FW_HASH_STRING_LENGTH	41
#define VE2_FW_DATE_STRING_LENGTH	11

struct ve2_firmware_version {
	u8 major;
	u8 minor;
	char git_hash[VE2_FW_HASH_STRING_LENGTH];
	char date[VE2_FW_DATE_STRING_LENGTH];
	u8 hotfix;
	u8 build;
};

struct ve2_firmware_status {
	u32 state;
	u32 abs_page_index;
	u32 ppc;
	u32 idle_status;
	u32 misc_status;
};

struct ve2_mem_region {
	u32 start_col;
	u32 end_col;
	u32 mem_bitmap;
};

struct ve2_mem_topology {
	u32 num_regions;
	struct ve2_mem_region regions[VE2_MAX_MEM_REGIONS];
};

struct ve2_aux_dev_priv {
	const char *fw_path;
	u32 hwctx_limit;
	u32 ctx_limit;
};

struct amdxdna_dev;

struct ve2_mgmtctx {
	struct amdxdna_dev		*xdna;
	void				*active_ctx;
	struct device			*mgmt_aiedev;
	u32				start_col;
	u32				mgmt_partid;
	struct aie_partition_init_args	args;
	struct list_head		ctx_command_fifo_head;
	struct mutex			ctx_lock;
	struct work_struct		sched_work;
	struct workqueue_struct		*mgmtctx_workq;
	u32				is_partition_idle;
	u32				is_context_req;
	u32				is_idle_due_to_context;
};

struct amdxdna_dev_hdl {
	struct amdxdna_dev			*xdna;
	const struct ve2_aux_dev_priv		*ve2_priv;
	u32					hwctx_limit;
	u32					hwctx_cnt;
	struct ve2_firmware_version		fw_version;
	struct aie_device_info			aie_dev_info;
	struct ve2_firmware_status		**fw_slots;
	struct ve2_mgmtctx			*ve2_mgmtctx;
	struct ve2_mem_topology			mem_topology;
	struct device				*cma_region_devs[VE2_MAX_MEM_REGIONS];
};

extern const struct amdxdna_dev_info dev_ve2_info;
extern const struct amdxdna_dev_ops ve2_ops;

static inline struct amdxdna_dev_hdl *ve2_dev_hdl(struct amdxdna_dev *xdna)
{
	return xdna->dev_handle;
}

#endif /* _VE2_AUX_H_ */

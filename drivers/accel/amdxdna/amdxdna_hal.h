/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * AMDXDNA Hardware Abstraction Layer (HAL)
 */

#ifndef _AMDXDNA_HAL_H_
#define _AMDXDNA_HAL_H_

#include <linux/types.h>
#include <linux/device.h>

struct amdxdna_dev;
struct amdxdna_dev_hdl;
struct amdxdna_mgmt_dma_hdl;

enum amdxdna_platform_id {
	AMDXDNA_PLATFORM_AIE2,
	AMDXDNA_PLATFORM_AIE4,
	AMDXDNA_PLATFORM_VE2,
	AMDXDNA_PLATFORM_MAX,
};

enum amdxdna_hal_impl_type {
	AMDXDNA_HAL_BACKEND_AIE_DRIVER,
	AMDXDNA_HAL_BACKEND_MSGPROTO,
	AMDXDNA_HAL_BACKEND_MAX,
};

struct amdxdna_hal_device_info {
	u32	cols;
	u32	rows;
	u32	version;
	u32	core_rows;
	u32	mem_rows;
	u32	shim_rows;
	u64	dev_mem_base;
	u64	dev_mem_size;
};

struct amdxdna_hal_fw_version {
	u32	major;
	u32	minor;
	u32	sub;
	u32	build;
};

struct amdxdna_hal_partition_param {
	u32	partition_col_start;
	u32	partition_col_count;
};

struct amdxdna_hal_partition_info {
	u32	partition_id;
};

struct amdxdna_hal_context_param {
	u32	start_col;
	u32	num_cols;
	u32	pasid;
	u32	priority;
	void	*ctx_handle;	/* struct amdxdna_hwctx * */
};

/*
 * Convenience bring-up: partition_create + context_create in one call.
 * Upper layers (e.g. ve2_hwctx) use amdxdna_hal_create_hwctx() / destroy_hwctx().
 */
struct amdxdna_hal_hwctx_param {
	u32	start_col;
	u32	num_cols;
	u32	pasid;
	u32	priority;
	void	*hwctx;		/* struct amdxdna_hwctx * */
};

struct amdxdna_hal_hwctx_info {
	u32				partition_id;
	struct amdxdna_hal_context	*context;
};

enum amdxdna_hal_context_config_type {
	AMDXDNA_HAL_CONTEXT_CONFIG_DEBUG_BO,
};

struct amdxdna_hal_context_config {
	enum amdxdna_hal_context_config_type type;
	union {
		struct {
			u32	bo_handle;
			int	attach;
		} debug_bo;
	};
};

struct amdxdna_hal_cmd {
	void	*cmd_bo;
	void	*arg_bo;
	void	*priv;
};

enum amdxdna_hal_debug_op_type {
	AMDXDNA_HAL_DEBUG_FW_LOG_INIT,
	AMDXDNA_HAL_DEBUG_FW_LOG_CONFIG,
	AMDXDNA_HAL_DEBUG_FW_LOG_FINI,
	AMDXDNA_HAL_DEBUG_FW_TRACE_INIT,
	AMDXDNA_HAL_DEBUG_FW_TRACE_CONFIG,
	AMDXDNA_HAL_DEBUG_FW_TRACE_FINI,
};

struct amdxdna_hal_debug_param {
	union {
		struct {
			struct amdxdna_mgmt_dma_hdl *dma_hdl;
			size_t size;
			u8 level;
			u32 *msi_idx;
			u32 *msi_addr;
		} fw_log_init;

		struct {
			u8 level;
		} fw_log_config;

		struct {
			struct amdxdna_mgmt_dma_hdl *dma_hdl;
			size_t size;
			u32 categories;
			u32 *msi_idx;
			u32 *msi_addr;
		} fw_trace_init;

		struct {
			u32 categories;
		} fw_trace_config;
	};
};

struct amdxdna_hal_device;
struct amdxdna_hal_context;

struct amdxdna_hal_ops {
	int (*init)(struct amdxdna_hal_device *hal_dev);
	void (*fini)(struct amdxdna_hal_device *hal_dev);

	int (*get_device_info)(struct amdxdna_hal_device *hal_dev,
			       struct amdxdna_hal_device_info *info);
	int (*get_firmware_version)(struct amdxdna_hal_device *hal_dev,
				    struct amdxdna_hal_fw_version *ver);

	int (*partition_create)(struct amdxdna_hal_device *hal_dev,
				struct amdxdna_hal_partition_param *param,
				struct amdxdna_hal_partition_info *info);
	int (*partition_destroy)(struct amdxdna_hal_device *hal_dev,
				 u32 partition_id);

	int (*context_create)(struct amdxdna_hal_device *hal_dev,
			      struct amdxdna_hal_context_param *param,
			      struct amdxdna_hal_context **context);
	void (*context_destroy)(struct amdxdna_hal_context *context);
	int (*context_config)(struct amdxdna_hal_context *context,
			      struct amdxdna_hal_context_config *config);

	int (*job_submit)(struct amdxdna_hal_context *context,
			  struct amdxdna_hal_cmd *cmd, u64 *seq);
	int (*job_wait)(struct amdxdna_hal_context *context,
			u64 seq, u32 timeout_ms);

	int (*suspend)(struct amdxdna_hal_device *hal_dev);
	int (*resume)(struct amdxdna_hal_device *hal_dev);

	int (*debug_op)(struct amdxdna_hal_device *hal_dev,
			enum amdxdna_hal_debug_op_type type,
			void *param);
};

/*
 * Register a firmware-interaction backend before amdxdna_hal_create_device().
 * Example: ve2_aie.c (Linux xlnx-aie UAPI), aie2_hal_message.c (mailbox).
 */
int amdxdna_hal_register_backend(enum amdxdna_platform_id platform_id,
				 enum amdxdna_hal_impl_type impl_type,
				 const struct amdxdna_hal_ops *ops);

void amdxdna_hal_unregister_backend(enum amdxdna_platform_id platform_id);

struct amdxdna_hal_device *
amdxdna_hal_create_device(const struct amdxdna_hal_ops *ops,
			  struct device *dev,
			  enum amdxdna_platform_id platform_id);

void amdxdna_hal_destroy_device(struct amdxdna_hal_device *hal_dev);

int amdxdna_hal_get_device_info(struct amdxdna_hal_device *hal_dev,
				struct amdxdna_hal_device_info *info);

int amdxdna_hal_get_firmware_version(struct amdxdna_hal_device *hal_dev,
				     struct amdxdna_hal_fw_version *ver);

int amdxdna_hal_create_partition(struct amdxdna_hal_device *hal_dev,
				 struct amdxdna_hal_partition_param *param,
				 struct amdxdna_hal_partition_info *info);

int amdxdna_hal_destroy_partition(struct amdxdna_hal_device *hal_dev,
				  u32 partition_id);

int amdxdna_hal_create_context(struct amdxdna_hal_device *hal_dev,
			       struct amdxdna_hal_context_param *param,
			       struct amdxdna_hal_context **context);

void amdxdna_hal_destroy_context(struct amdxdna_hal_context *context);

int amdxdna_hal_create_hwctx(struct amdxdna_hal_device *hal_dev,
			     struct amdxdna_hal_hwctx_param *param,
			     struct amdxdna_hal_hwctx_info *info);

void amdxdna_hal_destroy_hwctx(struct amdxdna_hal_device *hal_dev,
			       struct amdxdna_hal_context *context,
			       u32 partition_id);

int amdxdna_hal_config_context(struct amdxdna_hal_context *context,
			       struct amdxdna_hal_context_config *config);

int amdxdna_hal_submit_job(struct amdxdna_hal_context *context,
			   struct amdxdna_hal_cmd *cmd, u64 *seq);

int amdxdna_hal_wait_job(struct amdxdna_hal_context *context,
			 u64 seq, u32 timeout_ms);

int amdxdna_hal_suspend(struct amdxdna_hal_device *hal_dev);
int amdxdna_hal_resume(struct amdxdna_hal_device *hal_dev);

int amdxdna_hal_debug_op(struct amdxdna_hal_device *hal_dev,
			 enum amdxdna_hal_debug_op_type type,
			 void *param);

void amdxdna_hal_set_priv(struct amdxdna_hal_device *hal_dev, void *priv);
void *amdxdna_hal_get_priv(struct amdxdna_hal_device *hal_dev);

typedef int (*hal_platform_ctx_create_cb)(void *ndev, void *ctx, void *extra_data);
typedef int (*hal_platform_ctx_destroy_cb)(void *ndev, void *ctx, int graceful);

int amdxdna_hal_create_context_common(struct amdxdna_hal_device *hal_dev,
				      void *ctx,
				      hal_platform_ctx_create_cb create_cb,
				      void *extra_data,
				      struct amdxdna_hal_context **context);

void amdxdna_hal_destroy_context_common(struct amdxdna_hal_context *context,
					hal_platform_ctx_destroy_cb destroy_cb,
					int graceful);

typedef int (*hal_platform_pm_cb)(void *ndev);

int amdxdna_hal_pm_op_common(struct amdxdna_hal_device *hal_dev,
			     hal_platform_pm_cb pm_cb);

#endif /* _AMDXDNA_HAL_H_ */

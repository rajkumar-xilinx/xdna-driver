// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * AMDXDNA Hardware Abstraction Layer — core
 */

#include <linux/mutex.h>
#include <linux/slab.h>
#include "amdxdna_hal.h"

struct amdxdna_hal_backend_entry {
	const struct amdxdna_hal_ops	*ops;
	enum amdxdna_hal_impl_type	impl_type;
};

static struct amdxdna_hal_backend_entry hal_backends[AMDXDNA_PLATFORM_MAX];
static DEFINE_MUTEX(hal_backend_lock);

struct amdxdna_hal_device_impl {
	struct device			*dev;
	const struct amdxdna_hal_ops	*ops;
	void				*priv;
	enum amdxdna_platform_id	platform_id;
};

struct amdxdna_hal_context_impl {
	struct amdxdna_hal_device	*hal_dev;
	void				*ctx;
};

int amdxdna_hal_create_context_common(struct amdxdna_hal_device *hal_dev,
				      void *ctx,
				      hal_platform_ctx_create_cb create_cb,
				      void *extra_data,
				      struct amdxdna_hal_context **context)
{
	struct amdxdna_hal_context_impl *hal_ctx;
	void *ndev;
	int ret;

	if (!hal_dev || !ctx || !context || !create_cb)
		return -EINVAL;

	ndev = amdxdna_hal_get_priv(hal_dev);
	if (!ndev)
		return -EINVAL;

	hal_ctx = kzalloc(sizeof(*hal_ctx), GFP_KERNEL);
	if (!hal_ctx)
		return -ENOMEM;

	hal_ctx->hal_dev = hal_dev;
	hal_ctx->ctx = ctx;

	ret = create_cb(ndev, ctx, extra_data);
	if (ret) {
		kfree(hal_ctx);
		return ret;
	}

	*context = (struct amdxdna_hal_context *)hal_ctx;
	return 0;
}

void amdxdna_hal_destroy_context_common(struct amdxdna_hal_context *context,
					hal_platform_ctx_destroy_cb destroy_cb,
					int graceful)
{
	struct amdxdna_hal_context_impl *hal_ctx;
	void *ndev;

	if (!context)
		return;

	hal_ctx = (struct amdxdna_hal_context_impl *)context;

	if (destroy_cb && hal_ctx->hal_dev) {
		ndev = amdxdna_hal_get_priv(hal_ctx->hal_dev);
		if (ndev && hal_ctx->ctx)
			destroy_cb(ndev, hal_ctx->ctx, graceful);
	}

	kfree(hal_ctx);
}

int amdxdna_hal_pm_op_common(struct amdxdna_hal_device *hal_dev,
			     hal_platform_pm_cb pm_cb)
{
	void *ndev;

	if (!hal_dev || !pm_cb)
		return -EINVAL;

	ndev = amdxdna_hal_get_priv(hal_dev);
	if (!ndev)
		return -EINVAL;

	return pm_cb(ndev);
}

int amdxdna_hal_register_backend(enum amdxdna_platform_id platform_id,
				   enum amdxdna_hal_impl_type impl_type,
				   const struct amdxdna_hal_ops *ops)
{
	if (platform_id >= AMDXDNA_PLATFORM_MAX || !ops)
		return -EINVAL;

	mutex_lock(&hal_backend_lock);
	if (hal_backends[platform_id].ops) {
		mutex_unlock(&hal_backend_lock);
		return -EBUSY;
	}

	hal_backends[platform_id].ops = ops;
	hal_backends[platform_id].impl_type = impl_type;
	mutex_unlock(&hal_backend_lock);

	return 0;
}

void amdxdna_hal_unregister_backend(enum amdxdna_platform_id platform_id)
{
	if (platform_id >= AMDXDNA_PLATFORM_MAX)
		return;

	mutex_lock(&hal_backend_lock);
	hal_backends[platform_id].ops = NULL;
	hal_backends[platform_id].impl_type = AMDXDNA_HAL_BACKEND_MAX;
	mutex_unlock(&hal_backend_lock);
}

static const struct amdxdna_hal_ops *amdxdna_hal_lookup_backend(enum amdxdna_platform_id platform_id)
{
	const struct amdxdna_hal_ops *ops;

	if (platform_id >= AMDXDNA_PLATFORM_MAX)
		return NULL;

	mutex_lock(&hal_backend_lock);
	ops = hal_backends[platform_id].ops;
	mutex_unlock(&hal_backend_lock);

	return ops;
}

struct amdxdna_hal_device *
amdxdna_hal_create_device(const struct amdxdna_hal_ops *ops,
			  struct device *dev,
			  enum amdxdna_platform_id platform_id)
{
	struct amdxdna_hal_device_impl *hal_dev;

	if (!dev)
		return ERR_PTR(-EINVAL);

	if (!ops)
		ops = amdxdna_hal_lookup_backend(platform_id);
	if (!ops)
		return ERR_PTR(-ENODEV);

	if (!ops->init || !ops->fini ||
	    !ops->get_device_info || !ops->get_firmware_version ||
	    !ops->partition_create || !ops->partition_destroy ||
	    !ops->context_create || !ops->context_destroy ||
	    !ops->context_config ||
	    !ops->job_submit || !ops->job_wait ||
	    !ops->suspend || !ops->resume) {
		dev_err(dev, "Missing mandatory HAL operations\n");
		return ERR_PTR(-EINVAL);
	}

	hal_dev = kzalloc(sizeof(*hal_dev), GFP_KERNEL);
	if (!hal_dev)
		return ERR_PTR(-ENOMEM);

	hal_dev->dev = dev;
	hal_dev->ops = ops;
	hal_dev->platform_id = platform_id;

	if (ops->init) {
		int ret = ops->init((struct amdxdna_hal_device *)hal_dev);

		if (ret) {
			kfree(hal_dev);
			return ERR_PTR(ret);
		}
	}

	return (struct amdxdna_hal_device *)hal_dev;
}

void amdxdna_hal_destroy_device(struct amdxdna_hal_device *hal_dev)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl)
		return;

	if (impl->ops && impl->ops->fini)
		impl->ops->fini(hal_dev);

	kfree(impl);
}

int amdxdna_hal_get_device_info(struct amdxdna_hal_device *hal_dev,
				struct amdxdna_hal_device_info *info)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl || !info)
		return -EINVAL;

	if (!impl->ops->get_device_info)
		return -EOPNOTSUPP;

	return impl->ops->get_device_info(hal_dev, info);
}

int amdxdna_hal_get_firmware_version(struct amdxdna_hal_device *hal_dev,
				     struct amdxdna_hal_fw_version *ver)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl || !ver)
		return -EINVAL;

	if (!impl->ops->get_firmware_version)
		return -EOPNOTSUPP;

	return impl->ops->get_firmware_version(hal_dev, ver);
}

int amdxdna_hal_create_context(struct amdxdna_hal_device *hal_dev,
			       struct amdxdna_hal_context_param *param,
			       struct amdxdna_hal_context **context)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl || !param || !context)
		return -EINVAL;

	if (!impl->ops->context_create)
		return -EOPNOTSUPP;

	return impl->ops->context_create(hal_dev, param, context);
}

void amdxdna_hal_destroy_context(struct amdxdna_hal_context *context)
{
	struct amdxdna_hal_context_impl *hal_ctx;
	struct amdxdna_hal_device_impl *impl;

	if (!context)
		return;

	hal_ctx = (struct amdxdna_hal_context_impl *)context;
	if (!hal_ctx->hal_dev)
		return;

	impl = (struct amdxdna_hal_device_impl *)hal_ctx->hal_dev;
	if (impl && impl->ops && impl->ops->context_destroy)
		impl->ops->context_destroy(context);
}

int amdxdna_hal_create_hwctx(struct amdxdna_hal_device *hal_dev,
			     struct amdxdna_hal_hwctx_param *param,
			     struct amdxdna_hal_hwctx_info *info)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;
	struct amdxdna_hal_partition_param pp;
	struct amdxdna_hal_partition_info pi;
	struct amdxdna_hal_context_param cp;
	int ret;

	if (!impl || !param || !info)
		return -EINVAL;

	pp.partition_col_start = param->start_col;
	pp.partition_col_count = param->num_cols;
	ret = amdxdna_hal_create_partition(hal_dev, &pp, &pi);
	if (ret)
		return ret;

	cp.start_col = param->start_col;
	cp.num_cols = param->num_cols;
	cp.pasid = param->pasid;
	cp.priority = param->priority;
	cp.ctx_handle = param->hwctx;

	ret = amdxdna_hal_create_context(hal_dev, &cp, &info->context);
	if (ret) {
		amdxdna_hal_destroy_partition(hal_dev, pi.partition_id);
		return ret;
	}

	info->partition_id = pi.partition_id;
	return 0;
}

void amdxdna_hal_destroy_hwctx(struct amdxdna_hal_device *hal_dev,
			       struct amdxdna_hal_context *context,
			       u32 partition_id)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl || !context)
		return;

	amdxdna_hal_destroy_context(context);
	amdxdna_hal_destroy_partition(hal_dev, partition_id);
}

int amdxdna_hal_submit_job(struct amdxdna_hal_context *context,
			   struct amdxdna_hal_cmd *cmd, u64 *seq)
{
	struct amdxdna_hal_context_impl *hal_ctx;
	struct amdxdna_hal_device_impl *impl;

	if (!context || !cmd || !seq)
		return -EINVAL;

	hal_ctx = (struct amdxdna_hal_context_impl *)context;
	impl = (struct amdxdna_hal_device_impl *)hal_ctx->hal_dev;

	if (!impl || !impl->ops->job_submit)
		return -EOPNOTSUPP;

	return impl->ops->job_submit(context, cmd, seq);
}

int amdxdna_hal_wait_job(struct amdxdna_hal_context *context,
			 u64 seq, u32 timeout_ms)
{
	struct amdxdna_hal_context_impl *hal_ctx;
	struct amdxdna_hal_device_impl *impl;

	if (!context)
		return -EINVAL;

	hal_ctx = (struct amdxdna_hal_context_impl *)context;
	impl = (struct amdxdna_hal_device_impl *)hal_ctx->hal_dev;

	if (!impl || !impl->ops->job_wait)
		return -EOPNOTSUPP;

	return impl->ops->job_wait(context, seq, timeout_ms);
}

int amdxdna_hal_suspend(struct amdxdna_hal_device *hal_dev)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl)
		return -EINVAL;

	if (!impl->ops->suspend)
		return -EOPNOTSUPP;

	return impl->ops->suspend(hal_dev);
}

int amdxdna_hal_resume(struct amdxdna_hal_device *hal_dev)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl)
		return -EINVAL;

	if (!impl->ops->resume)
		return -EOPNOTSUPP;

	return impl->ops->resume(hal_dev);
}

void amdxdna_hal_set_priv(struct amdxdna_hal_device *hal_dev, void *priv)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (impl)
		impl->priv = priv;
}

void *amdxdna_hal_get_priv(struct amdxdna_hal_device *hal_dev)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	return impl ? impl->priv : NULL;
}

int amdxdna_hal_create_partition(struct amdxdna_hal_device *hal_dev,
				 struct amdxdna_hal_partition_param *param,
				 struct amdxdna_hal_partition_info *info)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl || !param || !info)
		return -EINVAL;

	if (!impl->ops->partition_create)
		return -EOPNOTSUPP;

	return impl->ops->partition_create(hal_dev, param, info);
}

int amdxdna_hal_destroy_partition(struct amdxdna_hal_device *hal_dev,
				  u32 partition_id)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl)
		return -EINVAL;

	if (!impl->ops->partition_destroy)
		return -EOPNOTSUPP;

	return impl->ops->partition_destroy(hal_dev, partition_id);
}

int amdxdna_hal_config_context(struct amdxdna_hal_context *context,
			       struct amdxdna_hal_context_config *config)
{
	struct amdxdna_hal_context_impl *hal_ctx;
	struct amdxdna_hal_device_impl *impl;

	if (!context || !config)
		return -EINVAL;

	hal_ctx = (struct amdxdna_hal_context_impl *)context;
	impl = (struct amdxdna_hal_device_impl *)hal_ctx->hal_dev;

	if (!impl || !impl->ops->context_config)
		return -EOPNOTSUPP;

	return impl->ops->context_config(context, config);
}

int amdxdna_hal_debug_op(struct amdxdna_hal_device *hal_dev,
			 enum amdxdna_hal_debug_op_type type,
			 void *param)
{
	struct amdxdna_hal_device_impl *impl = (struct amdxdna_hal_device_impl *)hal_dev;

	if (!impl)
		return -EINVAL;

	if (!impl->ops->debug_op)
		return -EOPNOTSUPP;

	return impl->ops->debug_op(hal_dev, type, param);
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * VE2 DRM hardware context: XRS, host queue, HAL partition/context.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "drm/amdxdna_accel.h"

#include "amdxdna_ctx.h"
#include "amdxdna_drv.h"
#include "amdxdna_hal.h"
#include "ve2_aux.h"
#include "ve2_hq.h"
#include "ve2_hwctx.h"
#include "amdxdna_ve2_solver.h"

static void ve2_hwctx_link_init(struct amdxdna_hwctx *hwctx, u32 start_col, u32 num_cols)
{
	struct ve2_hwctx_link *link;

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		return;

	link->col_config = kcalloc(num_cols, sizeof(*link->col_config), GFP_KERNEL);
	if (!link->col_config) {
		kfree(link);
		return;
	}

	hwctx->start_col = start_col;
	hwctx->num_col = num_cols;
	hwctx->aux_ctx_priv = link;
}

static void ve2_hwctx_link_fini(struct amdxdna_hwctx *hwctx)
{
	struct ve2_hwctx_link *link = hwctx->aux_ctx_priv;

	if (!link)
		return;

	kfree(link->col_config);
	kfree(link);
	hwctx->aux_ctx_priv = NULL;
}

static void ve2_hal_teardown_hwctx(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev_hdl *hdl = ve2_dev_hdl(xdna);
	struct ve2_hwctx_link *link = hwctx->aux_ctx_priv;

	if (!hdl || !hdl->hal_dev || !link || !link->hal_ctx)
		return;

	amdxdna_hal_destroy_hwctx(hdl->hal_dev, link->hal_ctx, link->hal_partition_id);
	link->hal_ctx = NULL;
	link->hal_partition_id = 0;
}

static void ve2_release_xrs(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx)
{
	struct solver_state *xrs = xdna->xrs_hdl;
	struct xrs_action_load la = { };

	if (!xrs)
		return;

	mutex_lock(&xrs->xrs_lock);
	ve2_xrs_release_resource(xrs, (u64)(uintptr_t)hwctx, &la);
	mutex_unlock(&xrs->xrs_lock);
}

int enable_polling;
module_param(enable_polling, int, 0644);
MODULE_PARM_DESC(enable_polling, "Enable polling mode. Polling mode disabled by default.");

static int ve2_xrs_align_cols = 4;
module_param(ve2_xrs_align_cols, int, 0644);
MODULE_PARM_DESC(ve2_xrs_align_cols, "VE2 XRS start-column stride (default 4)");

int ve2_hwctx_config_opcode_timeout(struct amdxdna_hwctx *hwctx, u32 op_timeout)
{
	struct ve2_hwctx_link *link = hwctx->aux_ctx_priv;
	u32 col;

	if (!link || !link->col_config)
		return -EINVAL;

	for (col = 0; col < hwctx->num_col; col++)
		link->col_config[col].opcode_timeout_config = op_timeout;

	return 0;
}

void ve2_hwctx_fill_hs_config(struct amdxdna_hwctx *hwctx, struct handshake *hs, u32 col_idx)
{
	struct ve2_hwctx_link *link = hwctx->aux_ctx_priv;

	if (!link || !link->col_config || col_idx >= hwctx->num_col)
		return;

	hs->opcode_timeout_config = link->col_config[col_idx].opcode_timeout_config;
}

/**
 * ve2_hwctx_layer1_init - col list, XRS, host queue, HAL partition/context.
 */
int ve2_hwctx_layer1_init(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_dev_hdl *hdl;
	struct amdxdna_hwctx_priv *priv;
	struct ve2_hwctx_priv *vp;
	struct ve2_hwctx_link *link;
	int ret;

	hdl = ve2_dev_hdl(xdna);
	if (!hdl)
		return -ENODEV;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	vp = kzalloc(sizeof(*vp), GFP_KERNEL);
	if (!vp) {
		kfree(priv);
		return -ENOMEM;
	}
	priv->hw_priv = vp;
	hwctx->priv = priv;

	mutex_init(&vp->privctx_lock);
	init_waitqueue_head(&vp->waitq);
	init_waitqueue_head(&priv->job_free_wq);

	ret = amdxdna_hwctx_col_list(hwctx, hdl->aie_dev_info.rows,
				     hdl->aie_dev_info.cols, true);
	if (ret) {
		XDNA_ERR(xdna, "Create col list failed, ret %d", ret);
		goto free_vp;
	}

	ret = ve2_xrs_request(xdna, hwctx);
	if (ret) {
		XDNA_ERR(xdna, "XRS resource request failed, ret %d", ret);
		goto free_col_list;
	}

	ve2_hwctx_link_init(hwctx, hwctx->start_col, hwctx->num_col);
	if (!hwctx->aux_ctx_priv) {
		ret = -ENOMEM;
		goto release_xrs;
	}

	ve2_auto_select_mem_bitmap(xdna, hwctx);
	link = hwctx->aux_ctx_priv;
	if (ve2_hw_priv(hwctx))
		ve2_hw_priv(hwctx)->mem_bitmap = link->mem_bitmap;

	ret = ve2_hq_alloc(hwctx);
	if (ret) {
		XDNA_ERR(xdna, "Host queue alloc failed, ret %d", ret);
		goto cleanup_link;
	}

	if (hdl->hal_dev) {
		struct amdxdna_hal_hwctx_param hp = {
			.start_col = hwctx->start_col,
			.num_cols = hwctx->num_col,
			.pasid = (u32)hwctx->client->pasid,
			.priority = 0,
			.hwctx = hwctx,
		};
		struct amdxdna_hal_hwctx_info hi;

		ret = amdxdna_hal_create_hwctx(hdl->hal_dev, &hp, &hi);
		if (ret)
			goto free_hq;
		link->hal_partition_id = hi.partition_id;
		link->hal_ctx = hi.context;
	}

	ret = amdxdna_ctx_syncobj_create(hwctx);
	if (ret) {
		XDNA_ERR(xdna, "Create syncobj failed, ret %d", ret);
		goto cleanup_hal;
	}

	if (!hwctx->max_opc)
		hwctx->max_opc = HWCTX_MAX_CMDS;

	XDNA_DBG(xdna, "hwctx init: ready hwctx=%p start_col=%u pid=%d",
		 hwctx, hwctx->start_col, client->pid);

	return 0;

cleanup_hal:
	ve2_hal_teardown_hwctx(xdna, hwctx);
free_hq:
	ve2_hq_free(hwctx);
cleanup_link:
	ve2_hwctx_link_fini(hwctx);
release_xrs:
	ve2_release_xrs(xdna, hwctx);
free_col_list:
	kfree(hwctx->col_list);
	hwctx->col_list = NULL;
free_vp:
	mutex_destroy(&vp->privctx_lock);
	kfree(vp);
	kfree(priv);
	hwctx->priv = NULL;
	return ret;
}

/**
 * ve2_hwctx_layer1_fini - Reverse layer1 init; caller holds @xdna->dev_lock.
 */
void ve2_hwctx_layer1_fini(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct ve2_hwctx_priv *vp;

	ve2_hal_teardown_hwctx(xdna, hwctx);
	ve2_hq_free(hwctx);
	ve2_hwctx_link_fini(hwctx);
	ve2_release_xrs(xdna, hwctx);

	kfree(hwctx->col_list);
	hwctx->col_list = NULL;

	if (priv) {
		amdxdna_ctx_syncobj_destroy(hwctx);
		vp = priv->hw_priv;
		if (vp)
			mutex_destroy(&vp->privctx_lock);
		kfree(vp);
		kfree(priv);
		hwctx->priv = NULL;
	}
}

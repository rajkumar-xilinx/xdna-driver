// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022-2026, Advanced Micro Devices, Inc.
 *
 * PCI attachment for AMD XDNA devices. Board identity comes from the PCI id
 * table: each entry's .driver_data points at a null-terminated table of
 * (revision, dev_info) rows so one PCI device ID can map to several NPU
 * profiles (e.g. 0x17f0 revisions -> NPU4/5/6). Add a new pci_device_id row
 * and revision map when a new PCI device is supported; keep AIE2/AIE4
 * implementation in aie2_pci.c / aie4_pci.c and npu*_regs.c.
 */

#include "drm/amdxdna_accel.h"
#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/gpu_scheduler.h>
#include <linux/iommu.h>
#include <linux/pci.h>

#include "amdxdna_ctx.h"
#include "amdxdna_drv.h"
#include "amdxdna_gem.h"
#include "amdxdna_pci_drv.h"
#include "amdxdna_pm.h"

MODULE_FIRMWARE("amdnpu/1502_00/npu.dev.sbin");
MODULE_FIRMWARE("amdnpu/17f0_10/npu.dev.sbin");
MODULE_FIRMWARE("amdnpu/17f0_11/npu.dev.sbin");
MODULE_FIRMWARE("amdnpu/17f0_20/npu.dev.sbin");
MODULE_FIRMWARE("amdnpu/1502_00/npu.sbin");
MODULE_FIRMWARE("amdnpu/17f0_10/npu.sbin");
MODULE_FIRMWARE("amdnpu/17f0_11/npu.sbin");
MODULE_FIRMWARE("amdnpu/17f0_20/npu.sbin");
MODULE_FIRMWARE("amdnpu/1502_00/npu_7.sbin");
MODULE_FIRMWARE("amdnpu/17f0_10/npu_7.sbin");
MODULE_FIRMWARE("amdnpu/17f0_11/npu_7.sbin");

/**
 * struct amdxdna_pci_rev_map - Map PCI revision to &struct amdxdna_dev_info.
 * Tables are null-terminated with an entry where %dev_info is NULL.
 */
struct amdxdna_pci_rev_map {
	u8 revision;
	const struct amdxdna_dev_info *dev_info;
};

static const struct amdxdna_pci_rev_map amdxdna_pci_1502[] = {
	{ 0x00, &dev_npu1_info },
	{ }
};

static const struct amdxdna_pci_rev_map amdxdna_pci_17f0[] = {
	{ 0x10, &dev_npu4_info },
	{ 0x11, &dev_npu5_info },
	{ 0x20, &dev_npu6_info },
	{ }
};

static const struct amdxdna_pci_rev_map amdxdna_pci_17f2[] = {
	{ 0x10, &dev_npu3_pf_info },
	{ }
};

static const struct amdxdna_pci_rev_map amdxdna_pci_1b0b[] = {
	{ 0x10, &dev_npu3_pf_info },
	{ }
};

static const struct pci_device_id pci_ids[] = {
	{ PCI_VDEVICE(AMD, 0x1502), .driver_data = (kernel_ulong_t)amdxdna_pci_1502 },
	{ PCI_VDEVICE(AMD, 0x17f0), .driver_data = (kernel_ulong_t)amdxdna_pci_17f0 },
	{ PCI_VDEVICE(AMD, 0x17f2), .driver_data = (kernel_ulong_t)amdxdna_pci_17f2 },
	{ PCI_VDEVICE(AMD, 0x1b0b), .driver_data = (kernel_ulong_t)amdxdna_pci_1b0b },
	{ }
};

MODULE_DEVICE_TABLE(pci, pci_ids);

static const struct amdxdna_dev_info *
amdxdna_pci_match_dev_info(struct pci_dev *pdev, const struct pci_device_id *id)
{
	const struct amdxdna_pci_rev_map *row =
		(const struct amdxdna_pci_rev_map *)id->driver_data;

	if (!row)
		return NULL;

	for (; row->dev_info; row++) {
		if (pdev->revision == row->revision)
			return row->dev_info;
	}

	return NULL;
}

static int amdxdna_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct amdxdna_dev *xdna;
	int ret;

	xdna = devm_drm_dev_alloc(dev, &amdxdna_drm_drv, typeof(*xdna), ddev);
	if (IS_ERR(xdna))
		return PTR_ERR(xdna);

	xdna->dev_info = amdxdna_pci_match_dev_info(pdev, id);
	if (!xdna->dev_info || !xdna->dev_info->ops)
		return -ENODEV;

	pci_set_drvdata(pdev, xdna);

	ret = amdxdna_iommu_init(xdna);
	if (ret)
		return ret;

	init_rwsem(&xdna->notifier_lock);

	if (IS_ENABLED(CONFIG_LOCKDEP)) {
		fs_reclaim_acquire(GFP_KERNEL);
		might_lock(&xdna->notifier_lock);
		fs_reclaim_release(GFP_KERNEL);
	}

	xdna->notifier_wq = alloc_ordered_workqueue("amdxdna_pci_notifier", WQ_MEM_RECLAIM);
	if (!xdna->notifier_wq) {
		ret = -ENOMEM;
		goto failed_iommu_fini;
	}

	ret = amdxdna_dev_init(xdna);
	if (ret)
		goto failed_destroy_wq;

	return 0;

failed_destroy_wq:
	destroy_workqueue(xdna->notifier_wq);
failed_iommu_fini:
	amdxdna_iommu_fini(xdna);
	return ret;
}

static void amdxdna_remove(struct pci_dev *pdev)
{
	struct amdxdna_dev *xdna = pci_get_drvdata(pdev);

	amdxdna_dev_cleanup(xdna);
	destroy_workqueue(xdna->notifier_wq);
	amdxdna_iommu_fini(xdna);
}

static const struct dev_pm_ops amdxdna_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(amdxdna_pm_suspend, amdxdna_pm_resume)
	RUNTIME_PM_OPS(amdxdna_pm_suspend, amdxdna_pm_resume, NULL)
};

static int amdxdna_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	struct amdxdna_dev *xdna = pci_get_drvdata(pdev);

	guard(mutex)(&xdna->dev_lock);
	if (xdna->dev_info->ops->sriov_configure)
		return xdna->dev_info->ops->sriov_configure(xdna, num_vfs);

	return -ENOENT;
}

static struct pci_driver amdxdna_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = pci_ids,
	.probe = amdxdna_probe,
	.remove = amdxdna_remove,
	.driver.pm = &amdxdna_pm_ops,
	.sriov_configure = amdxdna_sriov_configure,
};

module_pci_driver(amdxdna_pci_driver);

MODULE_LICENSE(AMDXDNA_MODULE_LICENSE);
MODULE_IMPORT_NS("AMD_PMF");
MODULE_AUTHOR(AMDXDNA_MODULE_AUTHOR);
MODULE_VERSION(AMDXDNA_MODULE_VERSION);
MODULE_DESCRIPTION(AMDXDNA_MODULE_DESCRIPTION);

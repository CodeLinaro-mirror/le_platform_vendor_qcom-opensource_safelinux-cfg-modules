/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/rbtree.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>
#include <linux/dma-iommu.h>
#include <linux/irqdomain.h>
#include "vfio.h"
#define DRIVER_VERSION  "0.1"
#define DRIVER_DESC     "QCOM IOMMU driver for VFIO"

static void *vfio_iommu_qcom_open(unsigned long arg)
{
	return NULL;
}

static void vfio_iommu_qcom_release(void *data)
{
}

static int vfio_iommu_qcom_check_extension(void *data, unsigned long arg)
{
	switch (arg) {
		/* We are repurposing this IOMMU type for our usecase for now */
		case VFIO_SPAPR_TCE_IOMMU:
			return 1;
		default:
			return 0;
	}
}

static long vfio_iommu_qcom_ioctl(void *data,
				unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
		case VFIO_CHECK_EXTENSION:
			return vfio_iommu_qcom_check_extension(data, arg);
		case VFIO_IOMMU_GET_INFO:
		case VFIO_IOMMU_MAP_DMA:
		case VFIO_IOMMU_UNMAP_DMA:
		case VFIO_IOMMU_DIRTY_PAGES:
			return 0;
		default:
			return -ENOTTY;
	}
}

static int vfio_iommu_qcom_attach_group(void *data,
			struct iommu_group *iommu_group, enum vfio_group_type type)
{
	pr_err("%s: IOMMU: Group is = %d\n", __func__, iommu_group_id(iommu_group));
	return 0;
}

static void vfio_iommu_qcom_detach_group(void *data,
			struct iommu_group *iommu_group)
{
}

static int vfio_iommu_qcom_pin_pages(void *data,
			struct iommu_group *iommu_group,
			unsigned long *user_pfn,
			int npage, int prt,
			unsigned long *phys_pfn)
{
	return 0;
}

static int vfio_iommu_qcom_unpin_pages(void *data,
			unsigned long *user_pfn,
			int npage)
{
	return 0;
}

static int vfio_iommu_qcom_register_notifier(void *data,
			unsigned long *events,
			struct notifier_block *nb)
{
	return 0;
}

static int vfio_iommu_qcom_unregister_notifier(void *data,
			struct notifier_block *nb)
{
	return 0;
}

static int vfio_iommu_qcom_dma_rw(void *iommu_data, dma_addr_t user_iova,
		void *data, size_t count, bool write)
{
	return 0;
}

static struct iommu_domain *
vfio_iommu_qcom_group_iommu_domain(void *data,
				struct iommu_group *iommu_group)
{
	return NULL;
}

static void vfio_iommu_qcom_notify(void *data,
				enum vfio_iommu_notify_type event)
{
}

static const struct vfio_iommu_driver_ops vfio_iommu_driver_ops_qcom = {
	.name			= "vfio-iommu-qcom",
	.owner			= THIS_MODULE,
	.open			= vfio_iommu_qcom_open,
	.release		= vfio_iommu_qcom_release,
	.ioctl			= vfio_iommu_qcom_ioctl,
	.attach_group		= vfio_iommu_qcom_attach_group,
	.detach_group		= vfio_iommu_qcom_detach_group,
	.pin_pages		= vfio_iommu_qcom_pin_pages,
	.unpin_pages		= vfio_iommu_qcom_unpin_pages,
	.register_notifier	= vfio_iommu_qcom_register_notifier,
	.unregister_notifier	= vfio_iommu_qcom_unregister_notifier,
	.dma_rw			= vfio_iommu_qcom_dma_rw,
	.group_iommu_domain	= vfio_iommu_qcom_group_iommu_domain,
	.notify			= vfio_iommu_qcom_notify,
};

int __init vfio_iommu_qcom_init(void)
{
	return vfio_register_iommu_driver(&vfio_iommu_driver_ops_qcom);
}
//EXPORT_SYMBOL_GPL(vfio_iommu_qcom_init);
/* Note: This is a workaround for now, the above statement will be
 * removed when we can ensure this driver is modprobed after
 * the vfio core driver.
 * The associated header file and vfio change will also be removed
 * then*/

static void __exit vfio_iommu_qcom_exit(void)
{
	vfio_unregister_iommu_driver(&vfio_iommu_driver_ops_qcom);
}

module_init(vfio_iommu_qcom_init);
module_exit(vfio_iommu_qcom_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_SOFTDEP("pre: vfio");

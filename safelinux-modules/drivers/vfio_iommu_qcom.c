// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/vfio.h>
#include <linux/notifier.h>

#include "vfio.h"
#define DRIVER_VERSION  "0.1"
#define DRIVER_DESC     "QCOM IOMMU driver for VFIO"

/*
 * This is a kernel redefinition of the struct - iommu_group, to obtain
 * iommu domain from default domain, the iommu domain from iommu group is
 * a blocking domain with the latest update from vfio frameworks on linux6.1
 * and shouldn't be used
 */

struct vfio_iommu_qcom_group {
	struct kobject kobj;
	struct kobject *devices_kobj;
	struct list_head devices;
	struct xarray pasid_array;
	struct mutex mutex;
	void *iommu_data;
	void (*iommu_data_release)(void *iommu_data);
	char *name;
	int id;
	struct iommu_domain *default_domain;
	struct iommu_domain *blocking_domain;
	struct iommu_domain *domain;
	struct list_head entry;
	unsigned int owner_cnt;
	void *owner;
};

static void *vfio_iommu_group_default_domain(void *group)
{
	struct vfio_iommu_qcom_group *iommu_group = (struct vfio_iommu_qcom_group *) group;

	if (!iommu_group)
		return NULL;

	return (void *)iommu_group->default_domain;
}

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
	if (!vfio_iommu_group_default_domain(iommu_group))
		return -EINVAL;

	pr_info("%s: IOMMU: Group is = %d\n", __func__, iommu_group_id(iommu_group));
	return 0;
}

static void vfio_iommu_qcom_detach_group(void *data,
			struct iommu_group *iommu_group)
{
}

static int vfio_iommu_qcom_pin_pages(void *iommu_data,
				      struct iommu_group *iommu_group,
				      dma_addr_t user_iova,
				      int npage, int prot,
				      struct page **pages)
{

	return 0;
}

static void vfio_iommu_qcom_unpin_pages(void *iommu_data,
					 dma_addr_t user_iova, int npage)
{

}

static void vfio_iommu_qcom_register_device(void *iommu_data,
					     struct vfio_device *vdev)
{

}

static void vfio_iommu_qcom_unregister_device(void *iommu_data,
					       struct vfio_device *vdev)
{

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
	.register_device	= vfio_iommu_qcom_register_device,
	.unregister_device	= vfio_iommu_qcom_unregister_device,
	.dma_rw			= vfio_iommu_qcom_dma_rw,
	.group_iommu_domain	= vfio_iommu_qcom_group_iommu_domain,
};

int __init vfio_iommu_qcom_init(void)
{
	return vfio_register_iommu_driver(&vfio_iommu_driver_ops_qcom);
}

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

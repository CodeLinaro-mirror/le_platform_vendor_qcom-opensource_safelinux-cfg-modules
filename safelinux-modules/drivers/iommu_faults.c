// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__
#define dev_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include "arm-smmu/arm-smmu.h"
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/version.h>

/*
 * struct device_fault_info - This structure holds information about faults information
 * detected by the System Memory Management Unit (SMMU).
 *
 * @fsr: Fault Status Register, indicates the type of fault that occurred.
 * @iova: Input/Output Virtual Address, the address that caused the fault.
 * @flag: A flag indicating the status or state of the device.
 * @dev: pointer to device structure
 * @kobj: Pointer to a kernel object associated with the device.
 * @fault_attr: kobj attributes
 *
 */
struct device_fault_info {
	uint32_t fsr;
	uint64_t iova;
	uint32_t flag;
	struct device *dev;
	struct kobject *kobj;
	struct kobj_attribute fault_attr;
};

/*
 * struct qcom_iommu_group - This is a kernel redefinition of the
 * struct iommu_group, to obtain iommu domain from default domain,
 * the iommu domain from iommu group is a blocking domain with the
 * latest update from vfio frameworks on linux6.1 and shouldn't be used
 */
struct qcom_iommu_group {
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

/*
 * iommu_get_dma_domain() - This function returns the default iommu domain.
 * This is replica of original implementation in iommu.c
 *
 * @dev dev pointer to device structure
 *
 * Return: default iommu domain pointer
 */
struct iommu_domain *iommu_get_dma_domain(struct device *dev)
{
	if (!dev->iommu_group) {
		dev_err(dev, "%s:iommu group is invalid\n", __func__);
		return NULL;
	}
	return ((struct qcom_iommu_group *) dev->iommu_group)->default_domain;
}

/*
 * fsr_iova_show() - This function facilitates to get the faulty iova FSR value
 * when user reads the sysfs node as a result of smmu faults.
 *
 * @kobj: kernel object pointer of device
 * @attr: kernel object attribute
 * @buf: user space buffer
 *
 * Return: number of bytes successfully written
 */
static ssize_t fsr_iova_show(struct kobject *kobj, struct kobj_attribute *attr,
				char *buf)
{
	struct device_fault_info *fault_info;
	uint64_t iova = 0;
	uint32_t fsr = 0;

	fault_info = container_of(attr, struct device_fault_info, fault_attr);

	if (!fault_info->flag)
		return scnprintf(buf, PAGE_SIZE, "0x%x:0x%llx\n", fsr, iova);

	fsr = fault_info->fsr;
	iova = fault_info->iova;
	fault_info->fsr = 0;
	fault_info->iova = 0;
	fault_info->flag = 0;

	pr_err("%s on dev 0x%x:0x%llx attr %p\n", __func__, fsr, iova, attr);

	return scnprintf(buf, PAGE_SIZE, "0x%x:0x%llx\n", fsr, iova);
}

/**
 * iommu_fault_custom_handler() - Custom IOMMU fault handler function.
 * Logs fault details.
 *
 * @domain: domain Pointer to the IOMMU domain
 * @dev: dev Pointer to the device struct
 * @iova: iova IOVA where the fault occurred
 * @flags: flags associated with the fault
 * @token: Custom token (dev_info)
 *
 * Return: 0 on success, error code on failure
 */
static int iommu_fault_custom_handler(struct iommu_domain *domain,
		struct device *dev, unsigned long iova, int flags, void *token)
{
	struct device_fault_info *fault_info = (struct device_fault_info *)token;
	struct arm_smmu_domain *smmu_domain;
	struct arm_smmu_cfg *cfg;

	smmu_domain = container_of(domain, struct arm_smmu_domain, domain);
	if (!smmu_domain || !(smmu_domain->pgtbl_ops) || !fault_info) {
		pr_err("%s:smmu domain/pagetable ops/fault_info is invalid\n", __func__);
		return -EINVAL;
	}

	cfg = &smmu_domain->cfg;
	if (fault_info->flag) {
		pr_debug("%s: Fault info is not read by userspace\n", fault_info->kobj->name);
		return 0;
	}

	fault_info->fsr = arm_smmu_cb_read(smmu_domain->smmu, cfg->cbndx,
					ARM_SMMU_CB_FSR);
	fault_info->iova = iova;
	fault_info->flag = 1;
	sysfs_notify(fault_info->kobj, NULL, "fsr_iova");

	pr_err("IOMMU fault on device %s at IOVA 0x%lx FSR %x\n",
			fault_info->kobj->name, iova, fault_info->fsr);
	return 0;
}

/**
 * create_device_fault_info(): this function allocates dev_info struct
 * and updates device and kobj information
 *
 * @dev: dev Pointer to the device struct
 * @data: Custom data (device_obj_obj)
 *
 * Return: 0 on success, error code on failure
 */
static int create_device_fault_info(struct device *dev, void *data)
{
	struct kobject *device_obj = (struct kobject *) data;
	struct device_fault_info *dev_info;
	struct iommu_domain *domain;

	dev_info = devm_kzalloc(dev, sizeof(struct device_fault_info), GFP_KERNEL);
	if (!dev_info)
		return -ENOMEM;

	dev_info->dev = dev;
	dev_info->kobj = device_obj;
	dev_info->fault_attr.attr.name = "fsr_iova";
	dev_info->fault_attr.attr.mode = 0644;
	dev_info->fault_attr.show = fsr_iova_show;

	domain = iommu_get_dma_domain(dev);
	if (!domain) {
		dev_err(dev, "%s:iommu domain is NULL\n", __func__);
		return -EINVAL;
	}

	iommu_set_fault_handler(domain, iommu_fault_custom_handler, dev_info);

	if (sysfs_create_file(device_obj, &(dev_info->fault_attr.attr))) {
		dev_err(dev, "failed to create sysfs file in /sys/kernel/\n");
		return -ENOMEM;
	}

	return 0;
}

/**
 * match_and_register_fault(): Matches and registers the custom
 * IOMMU fault handler for compatible devices.
 *
 * @dev: dev Pointer to the device struct
 * @data: Custom data (smmu_obj)
 *
 * Return: 0 on success, error code on failure
 */
static int match_and_register_fault(struct device *dev, void *data)
{
	struct kobject *device_obj;
	int ret = 0;

	if (dev->of_node == NULL)
		return 0;

	// custom bool property "iommu-faults" set in the device tree
	if (!of_property_read_bool(dev->of_node, "iommu-faults"))
		return 0;

	if (!device_iommu_mapped(dev))
		return 0;

	device_obj = kobject_create_and_add(dev_name(dev), (struct kobject *)data);
	if (!device_obj) {
		dev_err(dev, "/sys/kernel/smmu_fault/%s creation failed\n",
				dev->kobj.name);
		return -ENOMEM;
	}

	ret = create_device_fault_info(dev, device_obj);
	if (ret) {
		kobject_put(device_obj);
		return ret;
	}

	return 0;
}

/**
 * iommu_fault_init(): Initializes the IOMMU fault handler module.
 *
 * Return: 0 on success, negative error code on failure
 */
static int __init iommu_fault_init(void)
{
	struct kobject *smmu_obj;

	smmu_obj = kobject_create_and_add("smmu_faults", kernel_kobj);
	if (!smmu_obj) {
		pr_err("smmu_faults kernel object creation failure\n");
		return -ENOMEM;
	}

	return bus_for_each_dev(&platform_bus_type, NULL, smmu_obj,
			match_and_register_fault);
}

/**
 * iommu_fault_exit(): Cleans up the IOMMU fault handler module.
 *
 * Return: void
 */
static void __exit iommu_fault_exit(void)
{
}

module_init(iommu_fault_init);
module_exit(iommu_fault_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Custom IOMMU Fault Handler Module");

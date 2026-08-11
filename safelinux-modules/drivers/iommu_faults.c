// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__
#define dev_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#define CREATE_TRACE_POINTS
#define TRACE_SAFELINUX_IOMMU_FAULTS
#include "safelinux_modules_trace.h"

#include "arm-smmu/arm-smmu.h"
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/kernfs.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/version.h>

/* 100 messages/second — kernel default (10/5s) is too low for fault bursts. */
static DEFINE_RATELIMIT_STATE(iommufault_rs, 1 * HZ, 100);

static struct kobject *smmu_obj;

static LIST_HEAD(fault_registry);
static DEFINE_MUTEX(fault_registry_mutex);

/*
 * struct iommu_fault_entry - Per-device registry entry for a fault handler.
 *
 * @dev:        Device pointer.
 * @domain:     Domain on which the handler was installed; saved so teardown
 *              can clear it even if the device's iommu_group is gone by then.
 * @device_obj: kobject under /sys/kernel/smmu_faults/ for this device.
 * @fault_info: Fault state for this device.
 * @node:       Link in fault_registry.
 */
struct iommu_fault_entry {
	struct device *dev;
	struct iommu_domain *domain;
	struct kobject *device_obj;
	struct device_fault_info *fault_info;
	struct list_head node;
};

/*
 * struct device_fault_info - SMMU fault state for one device.
 *
 * @fsr:              Fault Status Register value of the last recorded fault.
 * @iova:             IOVA of the last recorded fault.
 * @flag:             Set when fsr/iova hold an unread fault; cleared on sysfs
 *                     read. While set, new faults are dropped rather than
 *                     overwriting the unread record.
 * @lock:             Protects fsr/iova/flag (IRQ vs sysfs context).
 * @dev:              Device pointer.
 * @kobj:             kobject for this device's sysfs directory.
 * @fault_attr:       sysfs attribute (fsr_iova).
 * @kn:               kernfs node for kernfs_notify().
 */
struct device_fault_info {
	uint32_t fsr;
	uint64_t iova;
	bool flag;
	spinlock_t lock;
	struct device *dev;
	struct kobject *kobj;
	struct kobj_attribute fault_attr;
	struct kernfs_node *kn;
};

/*
 * struct qcom_iommu_group - Layout mirror of kernel-internal struct iommu_group.
 *
 * iommu_get_domain_for_dev() returns group->domain, which is a blocking domain
 * when the device is VFIO-assigned (Linux 6.1+). We need default_domain instead.
 * iommu_get_dma_domain() is declared in linux/iommu.h but not exported via
 * EXPORT_SYMBOL, so it is unavailable to out-of-tree modules.
 *
 * WARNING: This mirrors struct iommu_group via a type-pun. Any kernel update
 * that adds or reorders fields will silently corrupt the default_domain offset.
 * Validated for Linux 6.1–6.6 — re-validate against drivers/iommu/iommu.c on
 * every kernel update.
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

static struct iommu_domain *get_default_iommu_domain(struct device *dev)
{
	if (!dev->iommu_group) {
		dev_err(dev, "iommu group is invalid\n");
		return NULL;
	}
	return ((struct qcom_iommu_group *)dev->iommu_group)->default_domain;
}

/**
 * fsr_iova_show() - sysfs read for fsr_iova.
 * Returns "0x<fsr>:0x<iova>\n" for the most recently recorded unread fault,
 * or zeros if none is pending. Reading consumes the record: fsr/iova/flag
 * are cleared so a subsequent read returns zeros until the next fault.
 */
static ssize_t fsr_iova_show(struct kobject *kobj, struct kobj_attribute *attr,
				char *buf)
{
	struct device_fault_info *fault_info;
	unsigned long irqflags;
	uint64_t iova = 0;
	uint32_t fsr = 0;

	fault_info = container_of(attr, struct device_fault_info, fault_attr);

	spin_lock_irqsave(&fault_info->lock, irqflags);
	if (fault_info->flag) {
		fsr  = fault_info->fsr;
		iova = fault_info->iova;
		fault_info->fsr = 0;
		fault_info->iova = 0;
		fault_info->flag = false;
	}
	spin_unlock_irqrestore(&fault_info->lock, irqflags);

	/* Fire trace outside the spinlock to minimise lock hold time. */
	trace_iommu_fault(fault_info->kobj->name, "sysfs_read", iova, fsr, 1);

	return scnprintf(buf, PAGE_SIZE, "0x%x:0x%llx\n", fsr, iova);
}

/**
 * iommu_fault_custom_handler() - IRQ-context SMMU fault callback.
 * Records the fault in fault_info and wakes userspace via kernfs_notify().
 * If the previous fault has not yet been read via fsr_iova_show(), the new
 * fault is dropped rather than overwriting the unread record.
 *
 * Runs inside rcu_read_lock()/rcu_read_unlock() so unregister_device_fault()'s
 * synchronize_rcu() can wait for an in-flight invocation to finish reading
 * fault_info before the caller frees it.
 *
 * @domain: IOMMU domain.
 * @dev:    Faulting device.
 * @iova:   Faulting IOVA.
 * @flags:  Fault flags.
 * @token:  Pointer to device_fault_info (set at registration).
 *
 * Return: 0 on success, -EINVAL if domain or device state is invalid.
 */
static int iommu_fault_custom_handler(struct iommu_domain *domain,
		struct device *dev, unsigned long iova, int flags, void *token)
{
	struct device_fault_info *fault_info = (struct device_fault_info *)token;
	struct arm_smmu_domain *smmu_domain;
	struct arm_smmu_cfg *cfg;
	unsigned long irqflags;
	uint32_t fsr;
	int ret = 0;

	rcu_read_lock();

	if (!domain || !dev || !fault_info) {
		pr_err("bad args:%s%s%s iova=0x%lx\n",
		       !domain     ? " domain=NULL" : "",
		       !dev        ? " dev=NULL"    : "",
		       !fault_info ? " token=NULL"  : "",
		       iova);
		ret = -EINVAL;
		goto out_unlock;
	}

	smmu_domain = container_of(domain, struct arm_smmu_domain, domain);
	if (!smmu_domain->pgtbl_ops) {
		dev_err(dev, "fault with no page table ops: iova=0x%lx\n", iova);
		ret = -EINVAL;
		goto out_unlock;
	}

	/*
	 * smmu_domain->smmu can become NULL if the SMMU parent device is
	 * torn down after the fault handler was registered.
	 */
	if (!smmu_domain->smmu) {
		dev_err(dev, "SMMU device unavailable at fault time: iova=0x%lx\n", iova);
		ret = -EINVAL;
		goto out_unlock;
	}

	cfg = &smmu_domain->cfg;

	spin_lock_irqsave(&fault_info->lock, irqflags);
	if (fault_info->flag) {
		spin_unlock_irqrestore(&fault_info->lock, irqflags);
		pr_debug("%s: fault info is not read by userspace\n", fault_info->kobj->name);
		goto out_unlock;
	}
	spin_unlock_irqrestore(&fault_info->lock, irqflags);

	fsr = arm_smmu_cb_read(smmu_domain->smmu, cfg->cbndx, ARM_SMMU_CB_FSR);

	spin_lock_irqsave(&fault_info->lock, irqflags);
	fault_info->fsr = fsr;
	fault_info->iova = iova;
	fault_info->flag = true;
	spin_unlock_irqrestore(&fault_info->lock, irqflags);

	kernfs_notify(fault_info->kn);

	/* No rate limit on tracepoints — use to observe fault storms. */
	trace_iommu_fault(fault_info->kobj->name, "fault", iova, fsr, flags);

	/*
	 * FSR bit legend: TF=Translation Fault, AFF=Access Flag Fault,
	 * PF=Permission Fault, EF=External Fault, TLBMCF=TLB Match-Conflict Fault,
	 * TLBLKF=TLB Lock Fault, ASF=Address Size Fault,
	 * UUT=Unsupported Upstream Transaction, SS=Stall State, MULTI=Multiple Faults.
	 */
	if (__ratelimit(&iommufault_rs))
		dev_err(dev, "SMMU fault: iova=0x%lx fsr=0x%08x [%s%s%s%s%s%s%s%s%s%s] flags=0x%x\n",
			iova, fsr,
			(fsr & ARM_SMMU_FSR_TF)     ? "TF "     : "",
			(fsr & ARM_SMMU_FSR_AFF)    ? "AFF "    : "",
			(fsr & ARM_SMMU_FSR_PF)     ? "PF "     : "",
			(fsr & ARM_SMMU_FSR_EF)     ? "EF "     : "",
			(fsr & ARM_SMMU_FSR_TLBMCF) ? "TLBMCF " : "",
			(fsr & ARM_SMMU_FSR_TLBLKF) ? "TLBLKF " : "",
			(fsr & ARM_SMMU_FSR_ASF)    ? "ASF "    : "",
			(fsr & ARM_SMMU_FSR_UUT)    ? "UUT "    : "",
			(fsr & ARM_SMMU_FSR_SS)     ? "SS "     : "",
			(fsr & ARM_SMMU_FSR_MULTI)  ? "MULTI "  : "",
			flags);

out_unlock:
	rcu_read_unlock();

	return ret;
}

/**
 * create_device_fault_info() - Allocate and initialise a device_fault_info.
 * Creates the fsr_iova sysfs attribute. Does NOT install the fault handler;
 * the caller (match_and_register_fault) does that inside fault_registry_mutex.
 *
 * @dev:        Device.
 * @device_obj: kobject under which fsr_iova is created.
 *
 * Return: Pointer to device_fault_info on success, ERR_PTR on failure.
 */
static struct device_fault_info *create_device_fault_info(struct device *dev,
		struct kobject *device_obj)
{
	struct device_fault_info *fault_info;
	int ret;

	fault_info = kzalloc(sizeof(*fault_info), GFP_KERNEL);
	if (!fault_info)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&fault_info->lock);
	fault_info->dev = dev;
	fault_info->kobj = device_obj;
	fault_info->fault_attr.attr.name = "fsr_iova";
	fault_info->fault_attr.attr.mode = 0444;
	fault_info->fault_attr.show = fsr_iova_show;

	ret = sysfs_create_file(device_obj, &fault_info->fault_attr.attr);
	if (ret) {
		dev_err(dev, "failed to create sysfs fsr_iova attribute: %d\n", ret);
		kfree(fault_info);
		return ERR_PTR(ret);
	}

	fault_info->kn = kernfs_find_and_get(device_obj->sd, "fsr_iova");
	if (!fault_info->kn) {
		dev_err(dev, "kernfs_find_and_get(fsr_iova) failed\n");
		sysfs_remove_file(device_obj, &fault_info->fault_attr.attr);
		kfree(fault_info);
		return ERR_PTR(-ENOENT);
	}

	return fault_info;
}

/**
 * unregister_device_fault() - Clear the fault handler, tear down sysfs, free entry.
 * Must NOT be called with fault_registry_mutex held.
 *
 * iommu_set_fault_handler() clears domain->handler/handler_token but provides
 * no synchronisation of its own against an in-flight IRQ-context invocation
 * of iommu_fault_custom_handler() that already loaded the old token. Since
 * that handler now runs inside rcu_read_lock()/rcu_read_unlock(), the
 * synchronize_rcu() below blocks until any such in-flight call has returned,
 * making it safe to free fault_info afterwards.
 *
 * @entry: Registry entry to tear down and free.
 */
static void unregister_device_fault(struct iommu_fault_entry *entry)
{
	dev_info(entry->dev, "removing fault handler, sysfs entry deleted\n");
	iommu_set_fault_handler(entry->domain, NULL, NULL);
	synchronize_rcu();
	sysfs_remove_file(entry->device_obj, &entry->fault_info->fault_attr.attr);
	kernfs_put(entry->fault_info->kn);
	kobject_put(entry->device_obj);
	kfree(entry->fault_info);
	put_device(entry->dev);
	kfree(entry);
}

/**
 * match_and_register_fault() - Register the fault handler for @dev.
 * Skips already-registered devices.
 * The entire operation — duplicate check, kobject creation, sysfs setup,
 * handler installation, and list insertion — runs under fault_registry_mutex
 * to prevent concurrent registrations for the same device from racing.
 *
 * Callers must have already verified dev->of_node and the "iommu-faults"
 * DT property before calling.
 *
 * @dev:    Device to register.
 * @domain: Default IOMMU domain for @dev, obtained by the caller from
 *          dev_iommu_ready() to avoid a redundant lookup.
 * @data:   smmu_obj kobject (parent for the device's sysfs directory).
 *
 * Return: 0 on success or skip, negative error code on hard failure.
 */
static int match_and_register_fault(struct device *dev,
		struct iommu_domain *domain, void *data)
{
	struct iommu_fault_entry *entry, *existing;
	struct device_fault_info *fault_info;
	struct kobject *device_obj;

	mutex_lock(&fault_registry_mutex);

	list_for_each_entry(existing, &fault_registry, node) {
		if (existing->dev == dev) {
			dev_err(dev, "fault handler already registered, skipping\n");
			mutex_unlock(&fault_registry_mutex);
			return 0;
		}
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		mutex_unlock(&fault_registry_mutex);
		return -ENOMEM;
	}

	device_obj = kobject_create_and_add(dev_name(dev), (struct kobject *)data);
	if (!device_obj) {
		dev_err(dev, "failed to create kobject under smmu_faults\n");
		mutex_unlock(&fault_registry_mutex);
		kfree(entry);
		return -ENOMEM;
	}

	fault_info = create_device_fault_info(dev, device_obj);
	if (IS_ERR(fault_info)) {
		kobject_put(device_obj);
		mutex_unlock(&fault_registry_mutex);
		kfree(entry);
		return PTR_ERR(fault_info);
	}

	entry->dev        = get_device(dev);
	entry->domain     = domain;
	entry->device_obj = device_obj;
	entry->fault_info = fault_info;

	/* Install handler last, after fault_info is fully initialised. */
	iommu_set_fault_handler(entry->domain, iommu_fault_custom_handler,
				fault_info);
	list_add_tail(&entry->node, &fault_registry);

	mutex_unlock(&fault_registry_mutex);

	trace_iommu_fault(dev_name(dev), "register", 0, 0, 0);
	dev_info(dev, "fault handler active at /sys/kernel/smmu_faults/%s/fsr_iova\n",
		 dev_name(dev));

	return 0;
}

/**
 * dev_has_fault_property() - Check if @dev is tagged for fault handling in DT.
 * Returns true if the device has a DT node with the "iommu-faults" property.
 */
static bool dev_has_fault_property(struct device *dev)
{
	return dev->of_node &&
	       of_property_read_bool(dev->of_node, "iommu-faults");
}

/**
 * dev_iommu_ready() - Check if @dev's IOMMU is ready for fault handler installation.
 * Returns true if the device is IOMMU-mapped and has a default domain, setting
 * *domain_out to the domain so callers avoid a redundant lookup.
 * Emits a tracepoint for each failing condition.
 */
static bool dev_iommu_ready(struct device *dev, struct iommu_domain **domain_out)
{
	struct iommu_domain *domain;

	if (!device_iommu_mapped(dev)) {
		dev_err(dev, "device is not IOMMU-mapped\n");
		return false;
	}
	domain = get_default_iommu_domain(dev);
	if (!domain) {
		dev_err(dev, "default IOMMU domain unavailable\n");
		return false;
	}
	*domain_out = domain;
	return true;
}

/**
 * scan_and_count_fault() - bus_for_each_dev callback for the initial scan.
 * Registers each eligible, already IOMMU-mapped device. Because the notifier
 * is registered before the scan, it may fire for a device mid-scan; the
 * already_registered guard in match_and_register_fault() absorbs any
 * resulting duplicates.
 * Always returns 0 so bus_for_each_dev continues past per-device failures.
 */
static int scan_and_count_fault(struct device *dev, void *data)
{
	struct iommu_domain *domain;
	int ret;

	if (!dev_has_fault_property(dev))
		return 0;

	if (!dev_iommu_ready(dev, &domain))
		return 0;

	ret = match_and_register_fault(dev, domain, data);
	if (ret) {
		dev_err(dev, "match_and_register_fault failed\n");
		return 0;
	}

	return 0;
}

/**
 * iommu_fault_bus_notifier() - Handle driver bind and unbind for tracked devices.
 *
 * On BUS_NOTIFY_BOUND_DRIVER: attempt registration for the newly-bound device.
 * Duplicate registrations are prevented by the mutex-protected list check inside
 * match_and_register_fault(), which serialises concurrent scan and notifier paths.
 *
 * On BUS_NOTIFY_UNBIND_DRIVER: remove and tear down the entry for the device
 * before its driver (and IOMMU domain) are released, avoiding use-after-free on
 * entry->domain. The notifier is kept active until module exit to catch unbinds
 * for all registered devices.
 *
 * Return: NOTIFY_OK on handled event, NOTIFY_DONE otherwise.
 */
static int iommu_fault_bus_notifier(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct device *dev = (struct device *)data;
	struct iommu_fault_entry *entry, *tmp;
	struct iommu_domain *domain;
	int ret;

	/* Defensive: smmu_obj should not be NULL while the notifier is active. */
	if (!smmu_obj)
		return NOTIFY_DONE;

	if (event == BUS_NOTIFY_BOUND_DRIVER) {
		if (!dev_has_fault_property(dev))
			return NOTIFY_DONE;

		trace_iommu_fault(dev_name(dev), "bound_driver", 0, 0, 0);
		if (!dev_iommu_ready(dev, &domain))
			return NOTIFY_DONE;

		ret = match_and_register_fault(dev, domain, smmu_obj);
		if (ret)
			dev_err(dev, "failed to register fault handler on bind: %d\n", ret);

		return NOTIFY_OK;
	}

	if (event == BUS_NOTIFY_UNBIND_DRIVER) {
		mutex_lock(&fault_registry_mutex);
		list_for_each_entry_safe(entry, tmp, &fault_registry, node) {
			if (entry->dev == dev) {
				list_del(&entry->node);
				mutex_unlock(&fault_registry_mutex);
				unregister_device_fault(entry);
				return NOTIFY_OK;
			}
		}
		mutex_unlock(&fault_registry_mutex);
	}

	return NOTIFY_DONE;
}

static struct notifier_block iommu_fault_bus_nb = {
	.notifier_call = iommu_fault_bus_notifier,
};

/**
 * iommu_fault_cleanup() - Tear down all fault handlers and the smmu_faults kobject.
 * Called on module exit and on init failure paths.
 */
static void iommu_fault_cleanup(void)
{
	struct iommu_fault_entry *entry, *tmp;
	LIST_HEAD(to_unregister);

	bus_unregister_notifier(&platform_bus_type, &iommu_fault_bus_nb);

	mutex_lock(&fault_registry_mutex);
	list_splice_init(&fault_registry, &to_unregister);
	mutex_unlock(&fault_registry_mutex);

	list_for_each_entry_safe(entry, tmp, &to_unregister, node)
		unregister_device_fault(entry);

	kobject_put(smmu_obj);
	smmu_obj = NULL;
}

/**
 * iommu_fault_init() - Initialise the IOMMU fault handler module.
 * Registers a bus notifier first, then scans the platform bus to register
 * devices already IOMMU-mapped. Registering the notifier before the scan
 * ensures no BUS_NOTIFY_BOUND_DRIVER event is missed for devices that bind
 * during or after the scan. The notifier also handles BUS_NOTIFY_UNBIND_DRIVER
 * to tear down handlers before the IOMMU domain is released, and remains
 * active until module exit.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int __init iommu_fault_init(void)
{
	int ret;

	smmu_obj = kobject_create_and_add("smmu_faults", kernel_kobj);
	if (!smmu_obj) {
		pr_err("failed to create /sys/kernel/smmu_faults kobject\n");
		return -ENOMEM;
	}

	ret = bus_register_notifier(&platform_bus_type, &iommu_fault_bus_nb);
	if (ret) {
		pr_err("failed to register platform bus notifier: %d\n", ret);
		kobject_put(smmu_obj);
		smmu_obj = NULL;
		return ret;
	}

	bus_for_each_dev(&platform_bus_type, NULL, smmu_obj, scan_and_count_fault);

	return 0;
}

module_init(iommu_fault_init);

static void __exit iommu_fault_exit(void)
{
	iommu_fault_cleanup();
}

module_exit(iommu_fault_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Custom IOMMU Fault Handler Module");


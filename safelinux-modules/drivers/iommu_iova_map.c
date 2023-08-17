/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/sort.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vfio.h>
#include <linux/hashtable.h>
#include <uapi/misc/iommu_iova_map_user.h>
#include <linux/iommu_iova_map.h>

struct miscdevice *iovadev = NULL;

struct qcom_iommu_range_prop_cb_data {
	int (*range_prop_entry_cb_fn)(const __be32 *p, int naddr, int nsize, void *arg);
	void *arg;
};

struct device_node *qcom_iommu_group_parse_phandle(struct device *dev)
{
	struct device_node *np;

	if (!dev->of_node)
		return NULL;

	np = of_parse_phandle(dev->of_node, "qcom,iommu-group", 0);
	return np ? np : dev->of_node;
}

static int of_property_walk_each_entry(struct device *dev, const char *propname,
						       struct qcom_iommu_range_prop_cb_data *cb_data)
{
	struct device_node *np;
	const __be32 *p, *property_end;
	int ret, len, naddr, nsize;

	np = qcom_iommu_group_parse_phandle(dev);
	if (!np)
		return -EINVAL;

	p = of_get_property(np, propname, &len);
	if (!p)
		return -ENODEV;

	len /= sizeof(u32);
	naddr = of_n_addr_cells(np);
	nsize = of_n_size_cells(np);
	if (!naddr || !nsize || len % (naddr + nsize)) {
		dev_err(dev, "%s Invalid length %d. Address cells %d. Size cells %d\n",
									propname, len, naddr, nsize);
		return -EINVAL;
	}
	property_end = p + len;

	while (p < property_end) {
		ret = cb_data->range_prop_entry_cb_fn(p, naddr, nsize, cb_data->arg);
		if (ret)
			return ret;

		p += naddr + nsize;
	}

	return 0;
}

static bool check_overlap(struct iommu_resv_region *region, u64 start, u64 end)
{
	u64 region_end = region->start + region->length - 1;
	return end >= region->start && start <= region_end;
}

static int insert_range(const __be32 *p, int naddr, int nsize, void *arg)
{
	struct list_head *head = arg;
	struct iommu_resv_region *region, *new;
	u64 start = of_read_number(p, naddr);
	u64 end = start + of_read_number(p + naddr, nsize) - 1;

	list_for_each_entry(region, head, list) {
		if (check_overlap(region, start, end))
			return -EINVAL;

		if (start < region->start)
			break;
	}

	new = iommu_alloc_resv_region(start, end - start + 1,
						0, IOMMU_RESV_RESERVED);
	if (!new)
		return -ENOMEM;
	list_add_tail(&new->list, &region->list);
	return 0;
}

/*
 *  * Returns a sorted list of all regions described by the
 *   * "qcom,iommu-dma-addr-pool" property.
 *    *
 *     * Caller is responsible for freeing the entries on the list via
 *      * generic_iommu_put_resv_regions
 *       */
int qcom_iommu_generate_dma_regions(struct device *dev,
				struct list_head *head)
{
	struct qcom_iommu_range_prop_cb_data insert_range_cb_data = {
		.range_prop_entry_cb_fn = insert_range,
		.arg = head,
	};

	return of_property_walk_each_entry(dev, "qcom,iommu-dma-addr-pool",
					   &insert_range_cb_data);
}

static int invert_regions(struct list_head *head, struct list_head *inverted)
{
	struct iommu_resv_region *prev, *curr, *new;
	phys_addr_t rsv_start;
	size_t rsv_size;
	int ret = 0;

	/*
	 * Since its not possible to express start 0, size 1<<64 return
	 * an error instead. Also an iova allocator without any iovas doesn't
	 * make sense.
	 */
	if (list_empty(head))
		return -EINVAL;

	/*
	 * Handle case where there is a non-zero sized area between
	 * iommu_resv_regions A & B.
	 */
	prev = NULL;
	list_for_each_entry(curr, head, list) {
		if (!prev)
			goto next;

		rsv_start = prev->start + prev->length;
		rsv_size = curr->start - rsv_start;
		if (!rsv_size)
			goto next;

		new = iommu_alloc_resv_region(rsv_start, rsv_size,
						0, IOMMU_RESV_RESERVED);
		if (!new) {
			ret = -ENOMEM;
			goto out_err;
		}
		list_add_tail(&new->list, inverted);
next:
		prev = curr;
	}

	/* Now handle the beginning */
	curr = list_first_entry(head, struct iommu_resv_region, list);
	rsv_start = 0;
	rsv_size = curr->start;
	if (rsv_size) {
		new = iommu_alloc_resv_region(rsv_start, rsv_size,
						0, IOMMU_RESV_RESERVED);
		if (!new) {
			ret = -ENOMEM;
			goto out_err;
		}
		list_add(&new->list, inverted);
	}

	/* Handle the end - checking for overflow */
	rsv_start = prev->start + prev->length;
	rsv_size = -rsv_start;

	if (rsv_size && (U64_MAX - prev->start > prev->length)) {
		new = iommu_alloc_resv_region(rsv_start, rsv_size,
						0, IOMMU_RESV_RESERVED);
		if (!new) {
			ret = -ENOMEM;
			goto out_err;
		}
		list_add_tail(&new->list, inverted);
	}

	return 0;

out_err:
	list_for_each_entry_safe(curr, prev, inverted, list)
		kfree(curr);
	return ret;
}

/* Used by iommu drivers to generate reserved regions for qcom,iommu-dma-addr-pool property */
void qcom_iommu_generate_resv_regions(struct device *dev,
						      struct list_head *head)
{
	struct iommu_resv_region *region;
	LIST_HEAD(dma_regions);
	LIST_HEAD(resv_regions);
	int ret;

	ret = qcom_iommu_generate_dma_regions(dev, &dma_regions);
	if (ret)
		return;

	ret = invert_regions(&dma_regions, &resv_regions);
	generic_iommu_put_resv_regions(dev, &dma_regions);
	if (ret)
		return;

	list_for_each_entry(region, &resv_regions, list) {
		dev_dbg(dev, "Reserved region-2 %llx-%llx\n",
					(u64)region->start,
					(u64)(region->start + region->length - 1));
	}
	list_splice(&resv_regions, head);
}

EXPORT_SYMBOL(qcom_iommu_generate_resv_regions);

static int iommu_iova_open(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "iommu_iova_open open");
	return 0;
}

static long iommu_iova_ioctl(struct file *file, unsigned int cmd,
						unsigned long arg)
{
	printk(KERN_INFO "iommu_iova_ioctl called");
	return 0;
}

static const struct file_operations iova_fops = {
	.open = iommu_iova_open,
	.unlocked_ioctl = iommu_iova_ioctl,
	.compat_ioctl = iommu_iova_ioctl,
};

static int iommu_iova_init(void)
{
	int err;
	char *devname = "iommumap";
	iovadev = kzalloc(sizeof(struct miscdevice), GFP_KERNEL);
	if (!iovadev)
		return -ENOMEM;

	iovadev->minor = MISC_DYNAMIC_MINOR;
	iovadev->name = devname;
	iovadev->fops = &iova_fops;
	printk(KERN_INFO "iova_range: iommu_iova_init");
	err = misc_register(iovadev);
	if (err) {
		pr_err("iova_range: misc device creation failure\n");
		return err;
	}

	return 0;
}

module_init(iommu_iova_init);
MODULE_DESCRIPTION("");
MODULE_LICENSE("GPL v2");

// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.


#include <linux/eventfd.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <uapi/misc/qcom_vdev.h>

#define VDEV_IRQ_INFO_MASKABLE  BIT(2)

#define VDEV_INDEX_OFFSET_SHIFT    40
#define VDEV_OFFSET_MASK (((u64)(1) << VDEV_INDEX_OFFSET_SHIFT) - 1)
#define VDEV_INDEX_TO_OFFSET(index) ((u64)(index) << VDEV_INDEX_OFFSET_SHIFT)


#define CREATE_TRACE_POINTS
#include "qcom_vdev_trace.h"

struct vdev_irq {
	const char *name;
	int hwirq;
	u32 count;
	u32 flags;
	struct eventfd_ctx *eventfd_ctx;
	bool masked;
};

struct vdev_region {
	u64 addr;
	resource_size_t size;
	u32 flags;
	u32 type;
#define VDEV_REGION_TYPE_MMIO	1
#define VDEV_REGION_TYPE_MEM	2
};

struct vdev_data {
	const char *name;
	struct device *dev;
	int num_regions;
	struct vdev_region *regions;
	int num_irqs;
	struct vdev_irq *irqs;
	int num_reserved_regions;
	struct miscdevice mdev;
};

#define miscdev_to_data(d) container_of(d, struct vdev_data, mdev)

/**
 * vdev_register_eventfd - Register an eventfd for a given IRQ
 * @vdev_data: Pointer to VDEV driver context
 * @eventfd: Eventfd file descriptor
 * @irq_index: Index of the IRQ to associate with the eventfd
 *
 * Associates an eventfd with a hardware IRQ and enables the IRQ.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int vdev_register_eventfd(struct vdev_data *vdev_data, int eventfd, u32 irq_index)
{
	struct vdev_irq *irq_ctx;
	struct eventfd_ctx *eventfd_ctx;
	int hwirq;

	if (irq_index >= vdev_data->num_irqs)
		return -EINVAL;

	irq_ctx = &vdev_data->irqs[irq_index];
	hwirq = vdev_data->irqs[irq_index].hwirq;
	if (irq_ctx->eventfd_ctx) {
		disable_irq(hwirq);
		eventfd_ctx_put(irq_ctx->eventfd_ctx);
		irq_ctx->eventfd_ctx = NULL;
	}

	if (eventfd < 0) /* Disable only */
		return 0;

	eventfd_ctx = eventfd_ctx_fdget(eventfd);
	if (IS_ERR(eventfd_ctx)) {
		dev_err(vdev_data->dev, "Failed to get eventfd context\n");
		return PTR_ERR(eventfd_ctx);
	}

	irq_ctx->eventfd_ctx = eventfd_ctx;

	enable_irq(hwirq);

	trace_qcom_vdev_register_eventfd(dev_name(vdev_data->dev),
		hwirq, eventfd, irq_index);

	return 0;
}

/**
 * vdev_mask_interrupt - Masked a IRQ
 * @vdev_data: Pointer to VDEV driver context
 * @irq_index: Index of the IRQ to unmask
 *
 * Disable the IRQ if it was previously enabled.
 *
 * Return: 0 on success, -EINVAL if the index is invalid.
 */
static int vdev_mask_interrupt(struct vdev_data *vdev_data, u32 irq_index)
{
	struct vdev_irq  *irq_ctx;

	if (irq_index >= vdev_data->num_irqs)
		return -EINVAL;

	irq_ctx = &vdev_data->irqs[irq_index];

	if (!(irq_ctx->flags & VDEV_IRQ_INFO_MASKABLE))
		return 0;

	if (!irq_ctx->masked) {
		disable_irq_nosync(irq_ctx->hwirq);
		irq_ctx->masked = true;
	}

	trace_qcom_vdev_mask_interrupt(dev_name(vdev_data->dev),
		irq_index, irq_ctx->hwirq, irq_ctx->flags, irq_ctx->masked);

	return 0;
}

/**
 * vdev_unmask_interrupt - Unmask a masked IRQ
 * @vdev_data: Pointer to VDEV driver context
 * @irq_index: Index of the IRQ to unmask
 *
 * Enables the IRQ if it was previously masked.
 *
 * Return: 0 on success, -EINVAL if the index is invalid.
 */
static int vdev_unmask_interrupt(struct vdev_data *vdev_data, u32 irq_index)
{
	struct vdev_irq  *irq_ctx;

	if (irq_index >= vdev_data->num_irqs)
		return -EINVAL;

	irq_ctx = &vdev_data->irqs[irq_index];

	if (!(irq_ctx->flags & VDEV_IRQ_INFO_MASKABLE))
		return 0;

	if (irq_ctx->masked) {
		irq_ctx->masked = false;
		enable_irq(irq_ctx->hwirq);
	}

	trace_qcom_vdev_unmask_interrupt(dev_name(vdev_data->dev),
		irq_index, irq_ctx->hwirq, irq_ctx->flags, irq_ctx->masked);

	return 0;
}

/**
 * vdev_send_eventfd - The trigger eventfd
 * @vdev_irq: Pointer to VDEV_IRQ context
 *
 * Send the eventfd signal.
 *
 */
static void vdev_send_eventfd(struct vdev_irq *irq_ctx)
{
	if (likely(irq_ctx->eventfd_ctx))
		eventfd_signal(irq_ctx->eventfd_ctx, 1);
}

/**
 * vdev_automasked_irq_handler - Level trigger IRQ handler for VDEV device
 * @irq: IRQ number
 * @dev_id: Pointer to IRQ context structure
 *
 * Disables the IRQ, marks it as masked, and signals the associated eventfd.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t vdev_automasked_irq_handler(int irq, void *dev_id)
{
	struct vdev_irq  *irq_ctx = (struct vdev_irq  *)dev_id;
	struct irq_data *d = irq_get_irq_data(irq);
	int ret = IRQ_NONE;

	if (!irq_ctx->masked) {
		ret = IRQ_HANDLED;
		/* automask maskable interrupts */
		disable_irq_nosync(irq_ctx->hwirq);
		irq_ctx->masked = true;
	}

	if (ret == IRQ_HANDLED)
		vdev_send_eventfd(irq_ctx);

	trace_qcom_vdev_automasked_irq_handler(irq_ctx->name, d->hwirq, irq_ctx->hwirq, ret);

	return ret;
}

/**
 * vdev_irq_handler - Edge trigger IRQ handler for VDEV device
 * @irq: IRQ number
 * @dev_id: Pointer to IRQ context structure
 *
 * Signals the associated eventfd.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t vdev_irq_handler(int irq, void *dev_id)
{
	struct vdev_irq  *irq_ctx = (struct vdev_irq  *)dev_id;
	struct irq_data *d = irq_get_irq_data(irq);

	vdev_send_eventfd(irq_ctx);

	trace_qcom_vdev_irq_handler(irq_ctx->name, d->hwirq, irq_ctx->hwirq);

	return IRQ_HANDLED;
}

/**
 * qcom_vdev_open - Open handler for the virtual device
 * @inode: Inode structure representing the device
 * @file: File structure for the device
 *
 * This function is called when the device is opened.
 *
 * Return: Always returns 0 (success)
 */
static int qcom_vdev_open(struct inode *inode, struct file *file)
{
	return 0;
}

/**
 * qcom_vdev_release - Release handler for the virtual device
 * @inode: Inode structure representing the device
 * @file: File structure for the device
 *
 * This function is called when the device is closed. It unregisters
 * all eventfds associated with the device's IRQs.
 *
 * Return: Always returns 0 (success)
 */
static int qcom_vdev_release(struct inode *inode, struct file *file)
{
	struct vdev_data *vdev_data = miscdev_to_data(file->private_data);
	int i;

	for (i = 0; i < vdev_data->num_irqs; i++)
		vdev_register_eventfd(vdev_data, -1, i);

	return 0;
}

static long qcom_vdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vdev_data *vdev_data = miscdev_to_data(file->private_data);
	int eventfd, ret = 0;
	u32 irq_index;
	struct irq_user irquser;
	unsigned long minsz;

	switch (cmd) {
	case IOCTL_VDEV_GET_INFO:
		struct vdev_device_info info;

		minsz = offsetofend(struct vdev_device_info, num_irqs);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.num_regions = vdev_data->num_regions;
		info.num_reserved_regions = vdev_data->num_reserved_regions;
		info.num_irqs = vdev_data->num_irqs;

		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;

		break;
	case IOCTL_VDEV_GET_REGION_INFO:
		struct vdev_region_info rinfo;

		minsz = offsetofend(struct vdev_region_info, offset);

		if (copy_from_user(&rinfo, (void __user *)arg, minsz))
			return -EFAULT;

		if (rinfo.argsz < minsz || rinfo.argsz > sizeof(struct vdev_region_info))
			return -EINVAL;

		if (rinfo.index >= (vdev_data->num_regions + vdev_data->num_reserved_regions))
			return -EINVAL;

		/* map offset to the physical address */
		rinfo.offset = VDEV_INDEX_TO_OFFSET(rinfo.index);
		rinfo.size   = vdev_data->regions[rinfo.index].size;
		rinfo.flags  = vdev_data->regions[rinfo.index].flags;

		if (copy_to_user((void __user *)arg, &rinfo, minsz))
			return -EFAULT;

		break;
	case IOCTL_VDEV_REGISTER_EVENTFD:
		if (copy_from_user(&irquser, (int __user *)arg, sizeof(struct irq_user)))
			return -EFAULT;

		irq_index = irquser.irq_index;
		eventfd   = irquser.event_fd;
		ret = vdev_register_eventfd(vdev_data, eventfd, irq_index);
		if (ret)
			return ret;

		break;
	case IOCTL_VDEV_UNMASK_INTERRUPT:
		if (copy_from_user(&irquser, (int __user *)arg, sizeof(struct irq_user)))
			return -EFAULT;

		irq_index = irquser.irq_index;
		ret = vdev_unmask_interrupt(vdev_data, irq_index);
		if (ret)
			return ret;

		break;
	case IOCTL_VDEV_MASK_INTERRUPT:
		if (copy_from_user(&irquser, (int __user *)arg, sizeof(struct irq_user)))
			return -EFAULT;

		irq_index = irquser.irq_index;
		ret = vdev_mask_interrupt(vdev_data, irq_index);
		if (ret)
			return ret;

		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/**
 * qcom_vdev_mmap - This mmap the register and DDR regions
 * @file: file ptr
 * @vma : pointer to struct vma
 *
 * map a reserved DDR region as normal memory
 * in user space and register region as device memory.
 *
 * Return: value is errno in failure cases
 * or 0 in case of success
 */

static int qcom_vdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct vdev_data *vdev_data = miscdev_to_data(filp->private_data);
	unsigned long size, rg_size;
	unsigned int index;
	u64 pgoff, start;

	index = vma->vm_pgoff >> (VDEV_INDEX_OFFSET_SHIFT - PAGE_SHIFT);

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;
	if (index >= (vdev_data->num_regions + vdev_data->num_reserved_regions))
		return -EINVAL;
	if (vma->vm_start & ~PAGE_MASK)
		return -EINVAL;
	if (vma->vm_end & ~PAGE_MASK)
		return -EINVAL;

	pgoff = vma->vm_pgoff &
		((1ULL << (VDEV_INDEX_OFFSET_SHIFT - PAGE_SHIFT)) - 1);

	start = pgoff << PAGE_SHIFT;
	size = vma->vm_end - vma->vm_start;
	rg_size = vdev_data->regions[index].size;

	if (rg_size < PAGE_SIZE || size > rg_size || start > rg_size - size)
		return -EINVAL;

	if (vdev_data->regions[index].type & VDEV_REGION_TYPE_MEM)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	else if (vdev_data->regions[index].type & VDEV_REGION_TYPE_MMIO)
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	vma->vm_pgoff = (vdev_data->regions[index].addr >> PAGE_SHIFT) + pgoff;

	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size,
						vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

static const struct file_operations qcom_vdev_fops = {
	.owner = THIS_MODULE,
	.open = qcom_vdev_open,
	.unlocked_ioctl = qcom_vdev_ioctl,
	.release = qcom_vdev_release,
	.mmap = qcom_vdev_mmap,
};

static int vdev_mem_regions_init(struct platform_device *pdev,
			struct vdev_data *vdev_data)
{
	int rindex, mrindex = 0;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *mem_np;
	struct reserved_mem *rmem;

	for (rindex = vdev_data->num_regions; rindex < (vdev_data->num_regions
					+ vdev_data->num_reserved_regions); rindex++) {
		mem_np = of_parse_phandle(np, "memory-region", mrindex);
		if (!mem_np) {
			dev_dbg(dev, "%s:can't find phandle\n", __func__);
			continue;
		}

		mrindex++;
		rmem = of_reserved_mem_lookup(mem_np);
		if (!rmem) {
			of_node_put(mem_np);
			return dev_err_probe(dev, -EINVAL,
			"%s: No memory address assigned to the reserved region\n",
				__func__);
		}

		of_node_put(mem_np);
		vdev_data->regions[rindex].addr = rmem->base;
		vdev_data->regions[rindex].size = rmem->size;
		vdev_data->regions[rindex].flags = 0;
		vdev_data->regions[rindex].type = VDEV_REGION_TYPE_MEM;

		trace_qcom_vdev_mem_regions_init(dev_name(vdev_data->dev),
			rindex, vdev_data->regions[rindex].addr, vdev_data->regions[rindex].size);
	}

	return 0;
}

static int vdev_regions_init(struct platform_device *pdev,
			struct vdev_data *vdev_data)
{
	int num_regions = 0, rindex;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;

	while (platform_get_resource(pdev, IORESOURCE_MEM, num_regions))
		num_regions++;

	if (!num_regions)
		return dev_err_probe(dev, -EINVAL,
				"%s: number of reg regions are zero\n", __func__);

	if (of_find_property(np, "memory-region", NULL)) {
		vdev_data->num_reserved_regions = of_property_count_elems_of_size(np,
					"memory-region", sizeof(phandle));
		if (vdev_data->num_reserved_regions < 0)
			vdev_data->num_reserved_regions = 0;
	} else {
		vdev_data->num_reserved_regions = 0;
	}

	vdev_data->regions = devm_kzalloc(&pdev->dev,
		(num_regions + vdev_data->num_reserved_regions) *
							sizeof(struct vdev_region), GFP_KERNEL);
	if (!vdev_data->regions)
		return dev_err_probe(dev, -ENOMEM,
				"failed to allocate memory\n");

	for (rindex = 0; rindex < num_regions; rindex++) {
		struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, rindex);

		if (!res)
			return dev_err_probe(dev, -ENOMEM, "Resource failed\n");

		vdev_data->regions[rindex].addr = res->start;
		vdev_data->regions[rindex].size = resource_size(res);
		vdev_data->regions[rindex].flags = 0;
		vdev_data->regions[rindex].type = VDEV_REGION_TYPE_MMIO;

		trace_qcom_vdev_regions_init(dev_name(vdev_data->dev), rindex,
			(unsigned long long)res->start, (unsigned long long)resource_size(res));
	}

	pdev->num_resources = num_regions;
	vdev_data->num_regions = num_regions;

	if (vdev_data->num_reserved_regions > 0)
		vdev_mem_regions_init(pdev, vdev_data);

	return 0;
}

static int vdev_irq_init(struct platform_device *pdev,
					struct vdev_data *vdev_data)
{
	int ret = 0, i, hwirq;

	vdev_data->num_irqs = platform_irq_count(pdev);
	if (vdev_data->num_irqs <= 0) {
		dev_info(&pdev->dev, "no IRQ resources provided\n");
		return ret;
	}

	vdev_data->irqs = devm_kzalloc(&pdev->dev,
			vdev_data->num_irqs * sizeof(struct vdev_irq), GFP_KERNEL);
	if (!vdev_data->irqs)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				"failed to allocate memory\n");

	for (i = 0; i < vdev_data->num_irqs; i++) {
		irq_handler_t handler = vdev_irq_handler;

		hwirq = platform_get_irq(pdev, i);
		if (hwirq < 0)
			return dev_err_probe(&pdev->dev, hwirq,
				"failed to get interrupt %d\n", i);

		if (irq_get_trigger_type(hwirq) & IRQ_TYPE_LEVEL_MASK) {
			vdev_data->irqs[i].flags |= VDEV_IRQ_INFO_MASKABLE;
			handler = vdev_automasked_irq_handler;
		}

		vdev_data->irqs[i].count = 1;
		vdev_data->irqs[i].hwirq = hwirq;
		vdev_data->irqs[i].masked = false;
		vdev_data->irqs[i].name = devm_kstrdup(&pdev->dev, vdev_data->name, GFP_KERNEL);

		ret = devm_request_irq(&pdev->dev, vdev_data->irqs[i].hwirq, handler,
				IRQF_NO_AUTOEN, vdev_data->name, &vdev_data->irqs[i]);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					"failed to register interrupt handler for IRQ %d\n",
						vdev_data->irqs[i].hwirq);
	}

	return ret;
}

static int qcom_vdev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const char *name;
	struct  vdev_data *vdev_data;
	int ret;

	vdev_data = devm_kzalloc(&pdev->dev, sizeof(*vdev_data), GFP_KERNEL);
	if (!vdev_data)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				"failed to allocate memory\n");

	vdev_data->dev = &pdev->dev;

	if (!of_property_read_string(np, "qcom,dev-name", &name))
		vdev_data->name = devm_kstrdup(dev, name, GFP_KERNEL);
	else
		vdev_data->name = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);

	if (!vdev_data->name)
		return dev_err_probe(dev, -ENOMEM, "failed to allocate device name\n");

	ret = vdev_regions_init(pdev, vdev_data);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				"failed to initialize dev reg regions\n");

	ret = vdev_irq_init(pdev, vdev_data);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to irqs registration\n");

	vdev_data->mdev.minor = MISC_DYNAMIC_MINOR;
	vdev_data->mdev.name = vdev_data->name;
	vdev_data->mdev.fops = &qcom_vdev_fops;

	ret = misc_register(&vdev_data->mdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to register misc device\n");

	dev_set_drvdata(&pdev->dev, vdev_data);

	return 0;
}

static int qcom_vdev_remove(struct platform_device *pdev)
{
	struct  vdev_data *vdev_data = dev_get_drvdata(&pdev->dev);

	misc_deregister(&vdev_data->mdev);

	return 0;
}

static const struct of_device_id qcom_vdev_of_match[] = {
	{ .compatible = "qcom,vdev-device", },
	{},
};
MODULE_DEVICE_TABLE(of, qcom_vdev_of_match);

static struct platform_driver qcom_vdev_driver = {
	.probe = qcom_vdev_probe,
	.remove = qcom_vdev_remove,
	.driver = {
		.name = "qcom_vdev",
		.of_match_table = qcom_vdev_of_match,
	},
};

module_platform_driver(qcom_vdev_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm virtual device driver");
MODULE_AUTHOR("Qualcomm Technologies, Inc.");

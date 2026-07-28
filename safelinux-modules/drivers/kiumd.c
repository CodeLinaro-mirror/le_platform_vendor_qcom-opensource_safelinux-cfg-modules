// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/kiumd_common.h>
#include <linux/miscdevice.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#define CREATE_TRACE_POINTS
#include "safelinux_modules_trace.h"
#include "vfio.h"

#ifdef CONFIG_SAFELINUX_KERNEL
#define kiumd_set_dma_max_seg_size(_dev, _size)				\
({									\
	dma_set_max_seg_size(_dev, _size);				\
})
#else
#define kiumd_set_dma_max_seg_size(_dev, _size)				\
({									\
	ret = dma_set_max_seg_size(_dev, _size);			\
	if (ret)							\
		pr_warn("%s: max_segment size not set.\n", __func__);	\
})
#endif

#define IOVA_ZERO	((dma_addr_t)0)

static struct kmem_cache *iommu_addr_cache;

/**
 * Added in kernel tag v6.12-rc1
 */
DEFINE_FREE(fput, struct file *, if (_T) fput(_T))

/**
 * @Brief: This function provide the device
 * pointer for the given file descriptor
 *
 * Parameters:
 * @fd: file descriptor
 *
 * Returns vfio device * upon success and NULL
 * on failure
 */
static struct device *kiumd_vfio_get_device(unsigned int vfio_fd,
					    struct file **vfio_file)
{
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct file *file;

	file = fget(vfio_fd);
	if (!file) {
		pr_err("Failed to get file from vfio_fd\n");
		return NULL;
	}

	*vfio_file = file;
	if (!vfio_file_is_valid(file))
		goto close_file;

	df = (struct vfio_device_file *)file->private_data;
	if (!df)
		goto close_file;

	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev || !vfio_dev->dev)
		goto close_file;

	return vfio_dev->dev;
close_file:
	pr_err("Failed to get device from vfio_fd\n");
	return NULL;
}

/**
 * kiumd_set_pgtbl_context - Set the page table context for an SMMU device.
 *
 * Parameters:
 * @arg: User-provided pointer to a struct kiumd_user containing context
 * information
 *
 * Return: 0 on success, negative error code on failure.
 */
static int kiumd_set_pgtbl_context(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct iommu_domain *iommu_dom;
	struct kiumd_user pgtbl_ctx;
	struct kiumd_ctx *kiumd_ctx;
	struct device *dev;
	int ret;

	if (copy_from_user(&pgtbl_ctx, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	dev = kiumd_vfio_get_device(pgtbl_ctx.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	iommu_dom = kiumd_iommu_get_dma_domain(dev);
	if (!iommu_dom) {
		pr_err("%s: iommu domain is NULL\n", __func__);
		return -EINVAL;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	switch (pgtbl_ctx.flags) {
	case KIUMD_SMMU_SET_TTBR0_CONFIG:
		ret = kiumd_set_pgtble_ttbr0_context(iommu_dom, kiumd_ctx);
		break;
	case KIUMD_SMMU_SET_TTBR1_CONFIG:
		ret = kiumd_set_pgtble_ttbr1_context(iommu_dom);
		break;
	default:
		pr_err("%s: Invalid flags: %d\n", __func__, pgtbl_ctx.flags);
		ret = -ENOTTY;
		break;
	}

	return ret;
}

/**
 * kiumd_perprocess_pt_alloc - Call the api to provides per process page table
 * allocation
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Return:  0 upon success and -EINVAL on failure
 */
static  int kiumd_perprocess_pt_alloc(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct arm_smmu_domain *smmu_dom;
	struct pgtable_map *pgtbl_ctx;
	struct kiumd_ctx *kiumd_ctx;
	struct io_pgtable *pgtable;
	struct io_pgtable_cfg cfg;
	struct kiumd_user kiusr;
	struct device *dev;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	trace_kiumd_perprocess_pt_alloc_start(kiusr.vfio_fd);
	smmu_dom = kiumd_get_smmu_domain(dev);
	if (!smmu_dom)
		return -EINVAL;

	pgtable = kiumd_ctx->pgtable;
	if (!pgtable)
		return -EINVAL;

	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	kiusr.asid = smmu_dom->cfg.asid;
	kiusr.pgtbl_ops_ptr = (long)alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg,
							 NULL);
	if (!(kiusr.pgtbl_ops_ptr)) {
		pr_err("%s:%d failed to allocate pagetable ops\n", __func__,
		       __LINE__);
		return -EINVAL;
	}

	kiusr.ttbr0 = cfg.arm_lpae_s1_cfg.ttbr;
	pgtbl_ctx = kzalloc(sizeof(*pgtbl_ctx), GFP_KERNEL);
	if (!pgtbl_ctx) {
		ret = -EINVAL;
		goto free_pgtable_ops;
	}

	spin_lock_init(&pgtbl_ctx->kgsl_rbtree_lock);
	pgtbl_ctx->rbtree = RB_ROOT;
	pgtbl_ctx->ttbr0_addr = kiusr.ttbr0;
	pgtbl_ctx->start_iova = KGSL_PER_PROCESS_PT_BASE_IOVA;
	pgtbl_ctx->end_iova = KGSL_PER_PROCESS_PT_END_IOVA;
	pgtbl_ctx->pgtbl_ops_ptr = kiusr.pgtbl_ops_ptr;
	pgtbl_ctx->last_allocated_end = pgtbl_ctx->start_iova;
	ret = xa_alloc(&kiumd_ctx->kiumd_xa_kgsl_pt, &pgtbl_ctx->idx, pgtbl_ctx, xa_limit_32b,
		       GFP_KERNEL);
	if (ret)
		goto free_pt_context;

	kiusr.pt_id = pgtbl_ctx->idx;
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		ret = -EFAULT;
		goto erase_pt_context;
	}

	trace_kiumd_perprocess_pt_alloc_end(kiusr.vfio_fd, kiusr.pgtbl_ops_ptr,
					    kiusr.ttbr0, kiusr.asid);

	return 0;

erase_pt_context:
	(void)xa_erase(&kiumd_ctx->kiumd_xa_kgsl_pt, pgtbl_ctx->idx);
free_pt_context:
	kfree(pgtbl_ctx);
free_pgtable_ops:
	free_io_pgtable_ops((struct io_pgtable_ops *)kiusr.pgtbl_ops_ptr);
	return ret;

}

/**
 * kiumd_global_pgtble_set - Call the api to provide global page table
 * allocation
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Return: 0 upon success and error codes on failure
 */
static int kiumd_global_pgtble_set(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct io_pgtable_ops *ki_pgtbl_ops;
	struct arm_smmu_domain *smmu_dom;
	struct kiumd_user kismmu_pproc;
	struct kiumd_ctx *kiumd_ctx;
	struct io_pgtable *pgtable;
	struct device *dev;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	trace_kiumd_global_pgtble_set_start(kismmu_pproc.vfio_fd);
	dev = kiumd_vfio_get_device(kismmu_pproc.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	smmu_dom = kiumd_get_smmu_domain(dev);
	if (!smmu_dom)
		return -EINVAL;

	if (!kiumd_ctx->pgtable) {
		kiumd_ctx->pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
		if (!kiumd_ctx->pgtable) {
			pr_err("%s:pagetable is NULL\n", __func__);
			return -EINVAL;
		}
	}

	pgtable = kiumd_ctx->pgtable;
	ki_pgtbl_ops = (struct io_pgtable_ops *) (&pgtable->ops);
	if (!ki_pgtbl_ops) {
		pr_err("%s:pagetable ops is NULL\n", __func__);
		return -ENOMEM;
	}

	smmu_dom->pgtbl_ops = ki_pgtbl_ops;
	trace_kiumd_global_pgtble_set_end(kismmu_pproc.vfio_fd);
	return 0;
}

/**
 * kiumd_perprocess_pgtble_set - This function call the api to set the per
 * process page table ops
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Return: 0 upon success and error codes on failure
 */
static int kiumd_perprocess_pgtble_set(char __user *arg)
{
	struct file *vfio_file __free(fput) = NULL;
	struct io_pgtable_ops *ki_pgtbl_ops;
	struct arm_smmu_domain *smmu_dom;
	struct kiumd_user kismmu_pproc;
	struct device *dev;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	trace_kiumd_perprocess_pgtble_set_start(kismmu_pproc.vfio_fd,
						kismmu_pproc.pgtbl_ops_ptr,
						kismmu_pproc.ttbr0,
						kismmu_pproc.asid);
	dev = kiumd_vfio_get_device(kismmu_pproc.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	smmu_dom = kiumd_get_smmu_domain(dev);
	if (!smmu_dom)
		return -EINVAL;

	ki_pgtbl_ops = (struct io_pgtable_ops *)kismmu_pproc.pgtbl_ops_ptr;
	if (!ki_pgtbl_ops) {
		pr_err("%s:pagetable ops is NULL\n", __func__);
		return -EINVAL;
	}

	smmu_dom->pgtbl_ops = ki_pgtbl_ops;
	trace_kiumd_perprocess_pgtble_set_end(kismmu_pproc.vfio_fd);
	return 0;
}

/**
 * kiumd_perprocess_pgtble_free - Calls the api to free the per process page table
 * ops
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Return: 0 upon success and errno on failure
 */
static int kiumd_perprocess_pgtble_free(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct io_pgtable_ops *pgtable_ops;
	struct pgtable_map *pgtble_ctx;
	struct kiumd_ctx *kiumd_ctx;
	struct kiumd_user kiusr;
	struct device *dev;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	trace_kiumd_perprocess_pgtble_free_start(kiusr.vfio_fd,
						 kiusr.pgtbl_ops_ptr,
						 kiusr.ttbr0, kiusr.asid);
	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	pgtble_ctx = xa_load(&kiumd_ctx->kiumd_xa_kgsl_pt, kiusr.pt_id);
	if (!pgtble_ctx) {
		pr_err("%s:%d Invalid id for hash table: id: %d\n",
		        __func__, __LINE__, kiusr.pt_id);
		return -ENOENT;
	}

	if (!check_pgtable_context(dev, pgtble_ctx)) {
		pr_err("%s:%d check_pgtable_context failed\n", __func__,
			__LINE__);
		return -EINVAL;
	}

	/*
	 * TODO: unmap the remaining iovas and buffers in rbtree, if any,
	 * for that ttbr should be pointing to this pagetable
	 */
	pgtable_ops = (struct io_pgtable_ops *)pgtble_ctx->pgtbl_ops_ptr;
	free_io_pgtable_ops(pgtable_ops);
	xa_erase(&kiumd_ctx->kiumd_xa_kgsl_pt, kiusr.pt_id);
	kfree(pgtble_ctx);
	trace_kiumd_perprocess_pgtble_free_end(kiusr.vfio_fd);

	return 0;
}

/**
 * kiumd_manage_runtime_pm - Handle runtime PM for a VFIO device
 *
 * Parameters:
 * @arg: User pointer to struct kiumd_user with VFIO fd and PM state
 *
 * Return: 0 on success or a negative error code on failure.
 */
static int kiumd_manage_runtime_pm(char __user *arg)
{
	struct file *vfio_file __free(fput) = NULL;
	struct kiumd_user kiusr;
	struct device *dev;
	int ret;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	switch (kiusr.pm_state) {
	case DEV_PWR_OFF:
		ret = pm_runtime_put_sync(dev);
		break;
	case DEV_PWR_ON:
		ret = pm_runtime_resume_and_get(dev);
		break;
	default:
		ret = -EINVAL;
		dev_warn(dev, "%s: Unsupported state(%d)\n", __func__,
			kiusr.pm_state);
		break;
	}

	if (ret < 0)
		dev_err(dev, "%s: Operation(%d) failed with err=%d\n",
			__func__, kiusr.pm_state, ret);

	return ret;
}

/**
 *  kiumd_dmabuf_custom_iova_init - Maps the IOVAs in a predefined address
 *  range from the IOVA address range specified in the device tree using the
 *  attribute qcom,iommu-dma-addr-pool
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Return: 0 upon success and error codes on failure
 */
static int kiumd_dmabuf_custom_iova_init(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct kiumd_iommu_dma_cookie *cookie;
	struct iommu_resv_region *region;
	struct iommu_domain *domain;
	struct kiumd_ctx *kiumd_ctx;
	struct iova_domain *iovad;
	struct kiumd_user kiusr;
	unsigned long lo, hi;
	struct device *dev;
	LIST_HEAD(resrvd);
	int ret;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	trace_kiumd_dmabuf_custom_iova_init_start(kiusr.vfio_fd);
	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	/*
	 * Get the maximum shift from DT for managed_iova_map api to
	 * determine alignment for large buffers
	 */
	kiumd_ctx->max_shift = get_shift_from_dt(dev);
	kiumd_set_dma_max_seg_size(dev, (unsigned int) DMA_BIT_MASK(32));
	ret = kiumd_set_dma_addr_ranges(kiumd_ctx, dev);
	if (ret) {
		pr_err("%s:set dma addr ranges failed for %s\n", __func__,
		       dev_name(dev));
		return -EINVAL;
	}

	domain = kiumd_iommu_get_dma_domain(dev);
	if (!domain) {
		pr_err("%s:dma_domain is invalid\n", __func__);
		return -EINVAL;
	}

	cookie = (struct kiumd_iommu_dma_cookie *)domain->iova_cookie;
	iovad = &cookie->iovad;
	qcom_iommu_generate_resv_regions(dev, &resrvd);
	list_for_each_entry(region, &resrvd, list) {
		lo = iova_pfn(iovad, region->start);
		hi = iova_pfn(iovad, region->start + region->length - 1);
		reserve_iova(iovad, lo, hi);
	}

	trace_kiumd_dmabuf_custom_iova_init_end(kiusr.vfio_fd);

	return 0;
}

static int __kiumd_dmabuf_vfio_map(struct kiumd_ctx *kiumd_ctx,
				   struct kiumd_user kiusr,
				   struct smmu_map_data *smap,
				   unsigned int cmd)
{
	int ret = 0;

	if (cmd == KIUMD_SMMU_MAP_BUF)
		ret = set_kgsl_map_iova(iommu_addr_cache, kiumd_ctx, kiusr,
					smap);
	else if (!kiusr.is_iova_zero)
		ret = init_and_allocate_iova(iommu_addr_cache, smap->dev, kiumd_ctx,
					     smap, kiumd_ctx->max_shift,
					     kiusr.dma_addr, kiusr.is_fix_map);

	if (ret)
		return ret;

	smap->dmabuf_ptr = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR(smap->dmabuf_ptr)) {
		ret = PTR_ERR(smap->dmabuf_ptr);
		goto iova_clear;
	}

	if (KIUSR_IS_IOVA_ZERO(kiusr)) {
		smap->staging_dev = kiumd_ctx->staging_dev;
		ret = kiumd_dmabuf_zero_map(smap);
	} else if (KIUSR_IS_PRIV(kiusr)) {
		ret = kiumd_dmabuf_priv_map(smap);
	} else {
		ret = kiumd_dmabuf_map(smap);
	}

	if (ret)
		goto dmabuf_put;

	ret = add_smap(kiumd_ctx, smap);
	if (ret) {
		pr_err("Error in adding to smap\n");
		clean_map(iommu_addr_cache, kiumd_ctx, smap);
		return ret;
	}

	return 0;

dmabuf_put:
	dma_buf_put(smap->dmabuf_ptr);
iova_clear:
	if (smap->is_kgsl_ctx)
		(void)clear_kgsl_map_iova(iommu_addr_cache, kiumd_ctx, smap);

	if (smap->iova_rb)
		(void)free_allocated_iova(iommu_addr_cache, kiumd_ctx, smap->iova_rb);

	return ret;
}

/**
 * kiumd_dmabuf_map_common - Facilitates the mapping of a DMA-BUF based buffer
 * to a SMMU backed device represented via a vfio_device.
 *
 * Parameters:
 * @arg: user space argument pointer
 * @fp: file pointer to the vfio device
 * @cmd: Map type
 * Return: errno if fail; 0 in case of successful mapping
 */
static int kiumd_dmabuf_map_common(char __user *arg, struct file *fp,
				   unsigned int map_type)
{
	struct file *vfio_file __free(fput) = NULL;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	struct device *dev;
	u64 size;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr)))
		return -EFAULT;

	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	size = kiumd_get_dmabuf_size(kiusr.dma_buf_fd);
	if (!size)
		return -EBADF;

	if (kiusr.dma_direction < DMA_BIDIRECTIONAL ||
	    kiusr.dma_direction > DMA_NONE)
		return -EINVAL;

	trace_kiumd_dmabuf_vfio_map_start(dev_name(dev), kiusr.vfio_fd,
					  kiusr.dma_buf_fd, kiusr.dma_attr,
					  kiusr.dma_direction, kiusr.ptselect,
					  kiusr.is_iova_zero, size, kiumd_ctx);
	smap = allocate_init_smap(kiusr, dev, size);
	if (!smap)
		return -ENOMEM;

	mutex_lock(&kiumd_ctx->map_lock);
	ret = __kiumd_dmabuf_vfio_map(kiumd_ctx, kiusr, smap, map_type);
	mutex_unlock(&kiumd_ctx->map_lock);
	if (ret)
		goto smap_free;

	kiusr.id = smap->id;
	if (smap->is_iova_zero)
		kiusr.dma_addr = (unsigned long) IOVA_ZERO;
	else
		kiusr.dma_addr = (unsigned long) sg_dma_address(smap->sgt_ptr->sgl);

	trace_kiumd_dmabuf_vfio_map_end(kiusr.vfio_fd, kiusr.id, kiusr.dma_addr);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		ret = -EFAULT;
		goto clean_map_res;
	}

	return 0;

clean_map_res:
	clean_map(iommu_addr_cache, kiumd_ctx, smap);
smap_free:
	kfree(smap);
	return ret;
}

static int __kiumd_dmabuf_vfio_unmap(struct kiumd_ctx *kiumd_ctx,
				     struct smmu_map_data *smap)
{
	struct iommu_domain *iommu_dom;
	unsigned long dma_addr;
	int ret;

	dma_addr = smap->is_iova_zero ?
		   IOVA_ZERO : sg_dma_address(smap->sgt_ptr->sgl);

	trace_kiumd_dmabuf_vfio_unmap_start(dev_name(smap->dev), smap->id, dma_addr);

	if (smap->is_fixed_map) {
		ret = kiumd_configure_dma_cookie(smap->dev, IOMMU_DMA_MSI_COOKIE,
						 dma_addr);
		if (ret)
			return -ENODEV;
	}

	if (smap->is_iova_zero)
		kiumd_dmabuf_zero_unmap(smap);
	else if (smap->is_priv_map)
		kiumd_dmabuf_priv_unmap(smap);
	else
		kiumd_dmabuf_unmap(smap);

	if (smap->is_kgsl_ctx)
		clear_kgsl_map_iova(iommu_addr_cache, kiumd_ctx, smap);

	if (smap->iova_rb)
		free_allocated_iova(iommu_addr_cache, kiumd_ctx, smap->iova_rb);

	if (smap->is_kgsl_map || smap->is_fixed_map) {
		iommu_dom = kiumd_iommu_get_dma_domain(smap->dev);
		if (!iommu_dom) {
			dev_err(smap->dev, "%s:iommu_dom is NULL, Can't flush GPU TLB\n",
				__func__);
			return -EINVAL;
		}
		iommu_flush_iotlb_all(iommu_dom);
	}

	trace_kiumd_dmabuf_vfio_unmap_end(dev_name(smap->dev), smap->id, dma_addr);

	return 0;
}

/**
 *  kiumd_dmabuf_unmap_common -Facilitates the unmap the buffer mapped to SMMU backed device
 * also decrements the dma_buf kref count
 *
 * Parameters:
 * @arg: user space argument pointer
 * @fp: file pointer for kiumd_ctx
 *
 * Return: errno if fail; 0 in case of success
 */
static int kiumd_dmabuf_unmap_common(char __user *arg, struct file *fp)
{
	struct smmu_map_data *smap __free(kfree) = NULL;
	struct kiumd_user_unmap kiusr;
	struct kiumd_ctx *kiumd_ctx;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr)))
		return -EFAULT;

	smap = remove_smap(kiumd_ctx, kiusr.id);
	if (!smap || smap->context)
		return -ENOENT;

	guard(mutex)(&kiumd_ctx->map_lock);
	ret = __kiumd_dmabuf_vfio_unmap(kiumd_ctx, smap);
	if (ret)
		return ret;

	return 0;
}

static void __kiumd_vfio_mmio_unmap(struct kiumd_ctx *kiumd_ctx,
				   struct smmu_map_data *smap);
static int __kiumd_mmio_vfio_map(struct kiumd_ctx *kiumd_ctx,
				 struct kiumd_smmu_mmio_map kiusr,
				 struct smmu_map_data *smap)
{
	int ret = 0;

	if (smap->is_fixed_map)
		ret = init_and_allocate_iova(iommu_addr_cache, smap->dev,
					     kiumd_ctx, smap, PAGE_SHIFT,
					     kiusr.iova, kiusr.fixed_iova);
	if (ret)
		return -EINVAL;

	ret = kiumd_mmio_map(smap);
	if (ret)
		goto iova_free;

	ret = add_smap(kiumd_ctx, smap);
	if (ret) {
		pr_err("Error in adding to smap\n");
		goto mmio_free;
	}

	return 0;

mmio_free:
	kiumd_mmio_unmap(smap);
iova_free:
	if (smap->is_fixed_map)
		free_allocated_iova(iommu_addr_cache, kiumd_ctx,
				    smap->iova_rb);
	return ret;
}

static void __kiumd_vfio_mmio_unmap(struct kiumd_ctx *kiumd_ctx,
				   struct smmu_map_data *smap);
/**
 * kiumd_mmio_smmu_map - Facilitate to map mmio region into smmu at fixed
 * IOVA
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * Return: value is errno in failure cases
 * or 0 in case of successful mapping
 */
static int kiumd_mmio_smmu_map(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	char *reg_name __free(kfree) = NULL;
	struct kiumd_smmu_mmio_map kiusr;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct resource *res;
	struct device *dev;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr)))
		return -EFAULT;

	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;


	reg_name = strndup_user(kiusr.reg_name, KIUMD_MAX_REG_NAME_LEN);
	if (IS_ERR(reg_name))
		return PTR_ERR(reg_name);

	if (!dev_is_platform(dev))
		return -EINVAL;

	res = platform_get_resource_byname(to_platform_device(dev),
					   IORESOURCE_MEM, reg_name);
	if (!res)
		return -EINVAL;

	smap = allocate_init_smap_mmio(kiusr, res, dev);
	if (!smap)
		return -ENOMEM;

	trace_kiumd_mmio_smmu_map_start(kiusr.vfio_fd, kiusr.fixed_iova, kiusr.iova);

	mutex_lock(&kiumd_ctx->map_lock);
	ret = __kiumd_mmio_vfio_map(kiumd_ctx, kiusr, smap);
	mutex_unlock(&kiumd_ctx->map_lock);
	if (ret) {
		kfree(smap->context);
		goto smap_free;
	}

	kiusr.id = smap->id;
	kiusr.iova = smap->context->iova;
	kiusr.reg_len = smap->context->size;
	trace_kiumd_mmio_smmu_map_end(reg_name, kiusr.vfio_fd,  kiusr.id, kiusr.iova,
				      kiusr.reg_len);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		ret = -EFAULT;
		goto clean_map_res;
	}

	return 0;

clean_map_res:
	__kiumd_vfio_mmio_unmap(kiumd_ctx, smap);
	remove_smap(kiumd_ctx, smap->id);
smap_free:
	kfree(smap);
	return ret;
}

static void __kiumd_vfio_mmio_unmap(struct kiumd_ctx *kiumd_ctx,
				   struct smmu_map_data *smap)
{
	kiumd_mmio_unmap(smap);
	if (smap->is_fixed_map)
		free_allocated_iova(iommu_addr_cache, kiumd_ctx,
				    smap->iova_rb);
}

/**
 * @Brief: This function facilitates to
 * unmap mmio region into smmu at fixed
 * IOVA.The function is called via IOCTL
 * interface and input is provided via
 * struct kiumd_user from the user space.
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * return value is errno in failure cases
 * or 0 in case of successful mapping
 */
static int kiumd_mmio_smmu_unmap(char __user *arg, struct file *fp)
{
	struct smmu_map_data *smap __free(kfree) = NULL;
	struct kiumd_user_unmap kiusr;
	struct kiumd_ctx *kiumd_ctx;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr)))
		return -EFAULT;

	trace_kiumd_mmio_smmu_unmap_start(kiusr.id);
	smap = remove_smap(kiumd_ctx, kiusr.id);
	if (!smap || !smap->context)
		return -ENOENT;

	guard(mutex)(&kiumd_ctx->map_lock);
	__kiumd_vfio_mmio_unmap(kiumd_ctx, smap);
	kfree(smap->context);
	trace_kiumd_mmio_smmu_unmap_end(kiusr.id);
	return 0;
}

/**
 * kiumd_iova_ctrl - Facilitates to set the cookie type based on flag passed
 * from user space and then set cookie in dma
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Return: 0 upon success and error codes on failure
 */
static int kiumd_iova_ctrl(char __user *arg)
{
	struct file *vfio_file __free(fput) = NULL;
	dma_addr_t iova_usr = IOVA_ZERO;
	struct kiumd_iova iovausr;
	int cookie_type, ret;
	struct device *dev;

	if (copy_from_user(&iovausr, arg, sizeof(struct kiumd_iova)))
		return -EFAULT;

	trace_kiumd_iova_ctrl_start(iovausr.vfio_fd, iovausr.iova_flag, iovausr.iova);
	if (iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_CLEAR)
		cookie_type = 0;
	else
		cookie_type = 1;

	if (iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_SET) {
		cookie_type = 1;
		iova_usr = iovausr.iova;
	}

	dev = kiumd_vfio_get_device(iovausr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	ret = kiumd_configure_dma_cookie(dev, cookie_type, iova_usr);
	if (ret)
		dev_err(dev, "%s failed to set cookie\n", __func__);

	trace_kiumd_iova_ctrl_end(iovausr.vfio_fd);
	return 0;
}

/**
 * kiumd_dmabuf_assign_buf - Facilitates the hyp assigning of a system heap
 * allocated dmabuffer to a SMMU backed device represented via a vfio_device
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * Return: value is errno in failure cases or 0 in case of successful mapping
 */
static int kiumd_dmabuf_assign_buf(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct kiumd_secure_map_context *map_ctx;
	struct dma_buf_attachment *dmabufattach;
	struct dma_buf *kiumd_dmabuf;
	struct kiumd_ctx *kiumd_ctx;
	struct hyp_map_data *smap;
	struct kiumd_user kiusr;
	struct sg_table *sgt;
	int *vmids, *perms;
	struct device *dev;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		ret = !kiumd_dmabuf ? -EINVAL : PTR_ERR(kiumd_dmabuf);
		return ret;
	}

	ret = kiumd_acl_to_vmid_perms_list(kiusr.mem_parcel.nr_acl_entries,
					   (void *)kiusr.mem_parcel.acl_list,
					   &vmids, &perms);
	if (ret) {
		pr_err("%s:%d Invalid params\n", __func__, __LINE__);
		return ret;
	}

	dmabufattach = dma_buf_attach(kiumd_dmabuf, dev);
	if (IS_ERR(dmabufattach)) {
		pr_err("%s:%d dmabufattach is invalid\n", __func__, __LINE__);
		ret = PTR_ERR(dmabufattach);
		goto free_mem;
	}

	if (!dmabufattach->priv) {
		ret = -EINVAL;
		pr_err("%s:%d dma heap attachment is NULL\n", __func__, __LINE__);
		goto detach;
	}

	sgt = ((struct kiumd_dma_heap_attachment *)(dmabufattach->priv))->table;
	if (!sgt) {
		pr_err("%s:%d sgt is NULL\n", __func__, __LINE__);
		ret = -EINVAL;
		goto detach;
	}

	if (!sgt->sgl) {
		ret = -EINVAL;
		pr_err("%s:%d sgl is NULL\n", __func__, __LINE__);
		goto detach;
	}

	pr_debug("%s:sgt from attachment:%p %llx\n", __func__, sgt, sg_phys(sgt->sgl));
	ret = kiumd_hyp_assign_sg(sgt, vmids, kiusr.mem_parcel.nr_acl_entries,
				  true, perms);
	if (ret < 0) {
		pr_err("%s:%d ownership transfer error\n", __func__, __LINE__);
		goto detach;
	}

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap) {
		ret = -ENOMEM;
		goto hyp_unassign_sg;
	}

	smap->dmabufattach = dmabufattach;
	smap->sgt_ptr = sgt;
	smap->dmabuf_ptr = kiumd_dmabuf;
	map_ctx = kzalloc(sizeof(*map_ctx), GFP_KERNEL);
	if (!map_ctx) {
		ret = -ENOMEM;
		goto free_smap;
	}

	map_ctx->nr_acl_entries = kiusr.mem_parcel.nr_acl_entries;
	map_ctx->vmids = vmids;
	map_ctx->perms = perms;
	smap->secure_ctx = map_ctx;
	ret = xa_alloc(&kiumd_ctx->kiumd_xa_hyp, &smap->id, smap, xa_limit_31b,
			GFP_KERNEL);
	if (ret) {
		dev_err(dev, ":couldnt allocate ID\n");
		goto context_free;
	}

	kiusr.hyp_id = smap->id;
	kiusr.dma_addr = sg_dma_address(sgt->sgl);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s:%d copy_to_user failed...\n", __func__, __LINE__);
		ret = -EFAULT;
		goto xa_free;
	}

	return 0;

xa_free:
	xa_erase(&kiumd_ctx->kiumd_xa_hyp, smap->id);
context_free:
	kfree(smap->secure_ctx);
free_smap:
	kfree(smap);
hyp_unassign_sg:
	kiumd_hyp_unassign_sg((struct sg_table *)sgt, vmids,
			      kiusr.mem_parcel.nr_acl_entries, true);
detach:
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);
free_mem:
	kfree(vmids);
	kfree(perms);
	return ret;
}

/**
 * kiumd_dmabuf_unassign_buf - Facilitates the hyp unassigning of a system heap
 * allocated dmabuffer to a SMMU backed device represented via a vfio_device
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * Return: value is errno in failure cases or 0 in case of successful mapping
 */
static int kiumd_dmabuf_unassign_buf(char __user *arg, struct file *fp)
{
	struct kiumd_secure_map_context *map_ctx;
	struct dma_buf_attachment *dmabufattach;
	struct dma_buf *kiumd_dmabuf;
	struct kiumd_ctx *kiumd_ctx;
	struct hyp_map_data *smap;
	struct kiumd_user kiusr;
	int *vmids, *perms;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr)))
		return -EFAULT;

	smap = xa_erase(&kiumd_ctx->kiumd_xa_hyp, kiusr.id);
	if (!smap)
		return -ENOENT;

	map_ctx = smap->secure_ctx;
	if (!map_ctx) {
		pr_err("%s:Invalid ctx\n", __func__);
		return -EINVAL;
	}

	vmids = map_ctx->vmids;
	perms = map_ctx->perms;
	if (!smap->sgt_ptr) {
		ret = -EINVAL;
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		goto err;
	}

	ret = kiumd_hyp_unassign_sg(smap->sgt_ptr, vmids,
				    map_ctx->nr_acl_entries, true);
	if (ret < 0) {
		pr_err("%s:%d memory ownership transfer error:%d\n", __func__, __LINE__, ret);
		goto err;
	}

	kfree(vmids);
	kfree(perms);
	dmabufattach = smap->dmabufattach;
	if (!dmabufattach) {
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto err;
	}

	kiumd_dmabuf = smap->dmabuf_ptr;
	if (!kiumd_dmabuf) {
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto err;
	}

	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);
	kfree(smap->secure_ctx);
	kfree(smap);
err:
	return ret;
}

/**
 * kiumd_open - Facilitates to initialize the locks, hash tables and
 * allocate the memory for device ctx
 *
 * Parameters:
 * @inode: inode ptr for device file
 * @filp: file ptr for device context
 *
 * Return: value is errno in failure cases or 0 in case of successful
 * mapping
 */
static int kiumd_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *misc = filp->private_data;
	struct kiumd_ctx *kictx;

	kictx = kzalloc(sizeof(*kictx), GFP_KERNEL);
	if (!kictx)
		return -ENOMEM;

	kictx->staging_dev = misc->parent ? misc->parent : misc->this_device;
	kictx->pt_start_iova = KIUMD_32BIT_START_IOVA;
	kictx->pt_end_iova = KIUMD_32BIT_END_IOVA;
	xa_init_flags(&kictx->kiumd_xa_smap, XA_FLAGS_ALLOC);
	xa_init_flags(&kictx->kiumd_xa_hyp, XA_FLAGS_ALLOC);
	xa_init_flags(&kictx->kiumd_xa_kgsl_pt, XA_FLAGS_ALLOC);
	spin_lock_init(&kictx->pt_lock);
	mutex_init(&kictx->resmem.resmem_lock);
	mutex_init(&kictx->managed_rbtree_lock);
	mutex_init(&kictx->map_lock);
	filp->private_data = kictx;

	return 0;
}

/**
 * kiumd_close - Facilitate to free the locks, hash tables and
 * free the memory for device ctx
 *
 * Parameters:
 * @inode: inode ptr for device file
 * @filp: file ptr for device context
 *
 * Return: value is errno in failure cases or 0 in case of success
 */
static int kiumd_close(struct inode *inode, struct file *filp)
{
	struct kiumd_ctx *ki_ctx = (struct kiumd_ctx *)filp->private_data;
	struct smmu_map_data *smap;
	unsigned long index;

	xa_for_each(&ki_ctx->kiumd_xa_smap, index, smap) {
		if (smap->is_kgsl_map) {
			kfree(smap);
			//TODO: pagetable/mappings to be cleaned up in future
			//change as part of switching to correct pagetable
			continue;
		}

		if (smap->context) {
			__kiumd_vfio_mmio_unmap(ki_ctx, smap);
			kfree(smap->context);
		} else {
			__kiumd_dmabuf_vfio_unmap(ki_ctx, smap);
		}

		kfree(smap);
	}

	xa_destroy(&ki_ctx->kiumd_xa_smap);
	xa_destroy(&ki_ctx->kiumd_xa_hyp);
	kfree(ki_ctx->pgtable_ctx);
	kfree(ki_ctx->resmem.area);
	kfree(ki_ctx);

	return 0;
}

/**
 * kiumd_vfio_ctx_init - Facilitate to initialise the context of the device
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * Return: value is errno in failure cases or 0 in case of success
 */
static int kiumd_vfio_ctx_init(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct kiumd_reserved_mem *resmem;
	struct kiumd_dev_mem_info kiusr;
	struct kiumd_ctx *kiumd_ctx;
	struct device_node *mem_np;
	struct reserved_mem *rmem;
	struct device_node *np;
	struct device *dev;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	resmem = &kiumd_ctx->resmem;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr)))
		return -EINVAL;

	trace_kiumd_vfio_ctx_init_start(kiusr.vfio_fd);
	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	np = dev_of_node(dev);
	if (!np)
		return -EINVAL;

	guard(mutex)(&resmem->resmem_lock);
	if (resmem->num_regions)
		return -ENOMEM;

	resmem->num_regions = of_property_count_elems_of_size(np,
			      "memory-region", sizeof(phandle));
	if (resmem->num_regions <= 0)
		return -EINVAL;

	if (resmem->area)
		pr_err("%s:res_mem_area repeatedly allocates memory\n", __func__);

	resmem->area = kcalloc(resmem->num_regions, sizeof(*resmem->area),
			       GFP_KERNEL);
	if (!resmem->area)
		return -ENOMEM;

	for (u64 i = 0; i < resmem->num_regions; i++) {
		mem_np = of_parse_phandle(dev->of_node, "memory-region", i);
		if (!mem_np)
			continue;

		rmem = of_reserved_mem_lookup(mem_np);
		if (!rmem) {
			of_node_put(mem_np);
			pr_err("%s:No memory address assigned to the reserved region\n", __func__);
			kfree(resmem->area);
			return -EINVAL;
		}

		of_node_put(mem_np);
		resmem->area[i].size = rmem->size;
		resmem->area[i].base = rmem->base;
	}

	kiusr.num_regions = resmem->num_regions;
	if (copy_to_user(arg, &kiusr, sizeof(kiusr)))
		return -EFAULT;

	trace_kiumd_vfio_ctx_init_end(kiusr.vfio_fd);

	return 0;
}

/**
 * @Brief: This function facilitates to
 * copy the reserved region information saved
 * in struct kiumd_ctx to user space.
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * return value is errno in failure cases
 * or 0 in case of success
 */
static int kiumd_vfio_ctx_get_data(char __user *arg, struct file *fp)
{
	struct kiumd_mem_info *mem_info __free(kfree) = NULL;
	struct kiumd_reserved_mem *resmem;
	struct kiumd_dev_mem_info kiusr;
	struct kiumd_ctx *kiumd_ctx;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	resmem = &kiumd_ctx->resmem;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr)))
		return -EINVAL;

	if (!resmem->num_regions || kiusr.num_regions != resmem->num_regions)
		return -EINVAL;

	if (!kiusr.mem_info)
		return -EINVAL;

	mem_info = kcalloc(resmem->num_regions, sizeof(struct kiumd_mem_info),
			   GFP_KERNEL);
	if (!mem_info)
		return -ENOMEM;

	for (u64 i = 0; i < resmem->num_regions; i++) {
		mem_info[i].size = resmem->area[i].size;
		mem_info[i].offset = i << KIUMD_INDEX_OFFSET;
	}

	if (copy_to_user(kiusr.mem_info, mem_info,
			 resmem->num_regions * sizeof(struct kiumd_mem_info)))
		return -EFAULT;

	return 0;
}

/**
 * kiumd_ioctl - Facilitate to call the different ioctls based on
 * commands received from user space
 *
 * Parameters:
 * @file: file ptr for device file
 * @cmd: ioctl cmd
 * @arg: argument
 *
 * Return: value is errno in failure cases
 * or 0 in case of success
 */
static long kiumd_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	char __user *argp = (char __user *)arg;
	int err = 0;

	switch (cmd) {
	case KIUMD_SMMU_MAP_BUF:
	case KIUMD_SMMU_MANAGED_IOVA_MAP:
		err = kiumd_dmabuf_map_common(argp, file, cmd);
		break;
	case KIUMD_SMMU_UNMAP_BUF:
	case KIUMD_SMMU_MANAGED_IOVA_UNMAP:
		err = kiumd_dmabuf_unmap_common(argp, file);
		break;
	case KIUMD_SMMU_MMIO_MAP:
		err = kiumd_mmio_smmu_map(argp, file);
		break;
	case KIUMD_SMMU_MMIO_UNMAP:
		err = kiumd_mmio_smmu_unmap(argp, file);
		break;
	case KIUMD_IOVA_MAP_CTRL:
		err = kiumd_iova_ctrl(argp);
		break;
	case KIUMD_SET_PGTBL_CONTEXT:
		err = kiumd_set_pgtbl_context(argp, file);
		break;
	case KIUMD_PER_PROCESS_ALLOC:
		err = kiumd_perprocess_pt_alloc(argp, file);
		break;
	case KIUMD_PER_PROCESS_SET:
		err = kiumd_perprocess_pgtble_set(argp);
		break;
	case KIUMD_PER_PROCESS_FREE:
		err = kiumd_perprocess_pgtble_free(argp, file);
		break;
	case KIUMD_CUSTOM_IOVA_INIT:
		err = kiumd_dmabuf_custom_iova_init(argp, file);
		break;
	case KIUMD_GLOBAL_PT_SET:
		err = kiumd_global_pgtble_set(argp, file);
		break;
/* Will remove below 2 ioctls, once tech teams update the code */
	case KIUMD_SMMU_FAULT_HANDLE_REGISTER:
//		err = kiumd_smmu_fault_handler_register(argp);
		break;
	case KIUMD_SMMU_FAULT_HANDLE_DEREGISTER:
//		err = kiumd_smmu_fault_handler_deregister(argp);
		break;
	case KIUMD_VFIO_CTX_INIT:
		err = kiumd_vfio_ctx_init(argp, file);
		break;
	case KIUMD_VFIO_CTX_GET_DATA:
		err = kiumd_vfio_ctx_get_data(argp, file);
		break;
	case KIUMD_SMMU_ASSIGN_BUF:
		err = kiumd_dmabuf_assign_buf(argp, file);
		break;
	case KIUMD_SMMU_UNASSIGN_BUF:
		err = kiumd_dmabuf_unassign_buf(argp, file);
		break;
	case KIUMD_MANAGE_RUNTIME_PM:
		err = kiumd_manage_runtime_pm(argp);
		break;
	default:
		err = -ENOTTY;
		break;
	}

	return err;
}

/**
 * kiumd_mmap - Facilitates to map a reserved DDR region as normal memory
 * in user space
 *
 * Parameters:
 * @file: file ptr
 * @vma : pointer to struct vma
 *
 * Return: value is errno in failure cases
 * or 0 in case of success
 */
static int kiumd_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct kiumd_reserved_mem *resmem;
	struct kiumd_ctx *kiumd_ctx;
	u64 req_len, index, base;
	size_t size;

	kiumd_ctx = (struct kiumd_ctx *)file->private_data;
	resmem = &kiumd_ctx->resmem;
	index = vma->vm_pgoff >> (KIUMD_INDEX_OFFSET - PAGE_SHIFT);
	if (resmem->num_regions <= index)
		return -EINVAL;

	guard(mutex)(&resmem->resmem_lock);
	if (!resmem->area) {
		pr_err("%s:No reserved mem areas\n", __func__);
		return -EINVAL;
	}

	base = resmem->area[index].base;
	size = resmem->area[index].size;
	if (vma->vm_end <= vma->vm_start)
		return -EINVAL;

	if (base & ~PAGE_MASK)
		return -EINVAL;

	req_len = vma->vm_end - vma->vm_start;
	vma->vm_pgoff = vma->vm_pgoff &
			((1ULL << (KIUMD_INDEX_OFFSET - PAGE_SHIFT)) - 1);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start,
			       base >> PAGE_SHIFT,
			       req_len, vma->vm_page_prot);
}

static const struct file_operations kiumd_fops = {
	.open = kiumd_open,
	.unlocked_ioctl = kiumd_ioctl,
	.compat_ioctl = kiumd_ioctl,
	.release = kiumd_close,
	.mmap = kiumd_mmap,
};

/**
 * kiumd_probe - Facilitates to register the kiumd driver as a miscellaneous
 * device to kernel driverframework
 *
 * Parameters:
 * @pdev: platform device ptr
 *
 * Return: Value is errno in failure cases or 0 in case of success
 */
static int kiumd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const char *name, *devname;
	struct miscdevice *miscdev;
	struct device_node *np;
	int err;

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48));
	np = dev->of_node;
	miscdev = kzalloc(sizeof(*miscdev), GFP_KERNEL);
	if (!miscdev)
		return -ENOMEM;

	if (!of_property_read_string(np, "qcom,dev-name", &name))
		devname = devm_kstrdup(dev, name, GFP_KERNEL);
	else
		devname = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);

	iommu_addr_cache = kmem_cache_create("iommu_addr_cache",
					     sizeof(struct iommu_addr_entry),
					     0, 0, NULL);
	if (!iommu_addr_cache) {
		pr_err("kiumd kmem cache creation failure\n");
		return -ENOMEM;
	}

	miscdev->minor = MISC_DYNAMIC_MINOR;
	miscdev->name = devname;
	miscdev->fops = &kiumd_fops;
	miscdev->parent = dev;
	err = misc_register(miscdev);
	if (err) {
		pr_err("kiumd misc device %s creation failure\n", devname);
		return err;
	}

	return 0;
}

static const struct of_device_id kiumd_match_table[] = {
	{ .compatible = "qcom,kiumd", },
	{ },
};

MODULE_DEVICE_TABLE(of, kiumd_match_table);

static struct platform_driver kiumd_driver = {
	.driver = {
		.name = "kiumd",
		.of_match_table = kiumd_match_table,
	},
	.probe = kiumd_probe,
};
module_platform_driver(kiumd_driver);

MODULE_IMPORT_NS(DMA_BUF);
MODULE_DESCRIPTION("KIUMD");
MODULE_LICENSE("GPL v2");

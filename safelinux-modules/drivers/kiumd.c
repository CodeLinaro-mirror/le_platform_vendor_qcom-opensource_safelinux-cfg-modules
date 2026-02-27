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

struct kmem_cache *iommu_addr_cache;

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
 * @arg: User-provided pointer to a struct kiumd_user containing context information
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
	struct kiumd_kgsl_context *kgsl_context;
	struct arm_smmu_domain *smmu_dom;
	struct pgtable_map *pgtbl_ctx;
	struct kiumd_ctx *kiumd_ctx;
	struct io_pgtable *pgtable;
	struct io_pgtable_cfg cfg;
	struct kiumd_user kiusr;
	struct device *dev;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	trace_kiumd_perprocess_pt_alloc_start(kiusr.vfio_fd);
	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	smmu_dom = kiumd_get_smmu_domain(dev);
	if (!smmu_dom)
		return -EINVAL;

	pgtable = kiumd_ctx->pgtable;
	if (!pgtable) {
		pr_err("%s: pgtable is null\n", __func__);
		return -EINVAL;
	}

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
	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	kgsl_context = kiumd_ctx->kgsl_context;
	if (kgsl_context->kgsl_pt_id == UINT_MAX) {
		pr_err("%s:%d integer overflow in pt_id.\n", __func__, __LINE__);
		return -EINVAL;
	}

	pgtbl_ctx = kzalloc(sizeof(*pgtbl_ctx), GFP_KERNEL);
	if (!pgtbl_ctx)
		return -EINVAL;

	spin_lock_init(&pgtbl_ctx->kgsl_rbtree_lock);
	pgtbl_ctx->rbtree = RB_ROOT;
	pgtbl_ctx->ttbr0_addr = kiusr.ttbr0;
	pgtbl_ctx->start_iova = kiumd_ctx->kgsl_context->kgsl_start_iova;
	pgtbl_ctx->end_iova = kiumd_ctx->kgsl_context->kgsl_end_iova;
	pgtbl_ctx->pgtbl_ops_ptr = kiusr.pgtbl_ops_ptr;
	pgtbl_ctx->last_allocated_end = pgtbl_ctx->start_iova;
	spin_lock(&kgsl_context->kgsl_hash_lock);
	pgtbl_ctx->idx = kgsl_context->kgsl_pt_id++;
	hash_add(kiumd_ctx->kgsl_page_table, &pgtbl_ctx->node, pgtbl_ctx->idx);
	spin_unlock(&kgsl_context->kgsl_hash_lock);
	kiusr.pt_id = pgtbl_ctx->idx;
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s:%d copy_to_user failed...\n", __func__, __LINE__);
		return -EFAULT;
	}

	trace_kiumd_perprocess_pt_alloc_end(kiusr.vfio_fd, kiusr.pgtbl_ops_ptr,
					    kiusr.ttbr0, kiusr.asid);

	return 0;
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

	pgtble_ctx = kiumd_get_pgtable_entry(kiumd_ctx, kiusr.pt_id);
	if (!pgtble_ctx) {
		pr_err("%s:%d Invalid id for hash table: id: %d\n",
		       __func__, __LINE__, kiusr.pt_id);
		return -EINVAL;
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
	if (!pgtable_ops) {
		pr_err("%s:%d pagegetable ops is NULL\n", __func__, __LINE__);
		return -EINVAL;
	}

	free_io_pgtable_ops(pgtable_ops);
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

static void clean_map(struct kiumd_ctx *kiumd_ctx, struct smmu_map_data *smap)
{
	if (smap->is_kgsl_ctx)
		(void)clear_kgsl_map_iova(kiumd_ctx, smap);

	if (smap->is_iova_zero)
		kiumd_dmabuf_zero_unmap(smap);
	else if (smap->is_priv_map)
		kiumd_dmabuf_priv_unmap(smap);
	else
		kiumd_dmabuf_unmap(smap);
}

static int __kiumd_dmabuf_vfio_map(struct kiumd_ctx *kiumd_ctx,
				   struct kiumd_user kiusr,
				   struct smmu_map_data *smap)
{
	int ret;

	ret = set_kgsl_map_iova(kiumd_ctx, kiusr, smap);
	if (ret)
		return ret;

	smap->dmabuf_ptr = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(smap->dmabuf_ptr)) {
		pr_err("%s dmabuf_ptr err\n", __func__);
		ret = -EBADF;
		goto kgsl_iova_clear;
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

	add_to_smmu_table(kiumd_ctx, smap);
	return 0;
dmabuf_put:
	dma_buf_put(smap->dmabuf_ptr);
kgsl_iova_clear:
	if (smap->is_kgsl_ctx)
		(void)clear_kgsl_map_iova(kiumd_ctx, smap);

	return ret;
}

/**
 * kiumd_dmabuf_vfio_map - Facilitates the mapping of a DMA-BUF based buffer
 * to a SMMU backed device represented via a vfio_device.
 *
 * Parameters:
 * @arg: user space argument pointer
 * @fp: file pointer to the vfio device
 *
 * Return: errno if fail; 0 in case of successful mapping
 */
static int kiumd_dmabuf_vfio_map(char __user *arg, struct file *fp)
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

	ret = __kiumd_dmabuf_vfio_map(kiumd_ctx, kiusr, smap);
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
	clean_map(kiumd_ctx, smap);
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
	if (smap->is_fixed_map) {
		ret = kiumd_configure_dma_cookie(smap->dev, IOMMU_DMA_MSI_COOKIE,
						 dma_addr);
		if (ret)
			return -ENODEV;
	}

	if (smap->is_kgsl_ctx) {
		ret = clear_kgsl_map_iova(kiumd_ctx, smap);
		if (ret)
			return -ENOENT;
	}

	if (smap->is_iova_zero)
		kiumd_dmabuf_zero_unmap(smap);
	else if (smap->is_priv_map)
		kiumd_dmabuf_priv_unmap(smap);
	else
		kiumd_dmabuf_unmap(smap);

	if (smap->is_kgsl_map || smap->is_fixed_map) {
		iommu_dom = kiumd_iommu_get_dma_domain(smap->dev);
		if (!iommu_dom) {
			dev_err(smap->dev, "%s:iommu_dom is NULL, Can't flush GPU TLB\n",
				__func__);
			return -EINVAL;
		}
		iommu_flush_iotlb_all(iommu_dom);
	}

	return 0;
}

/**
 *  kiumd_dmabuf_vfio_unmap -Facilitates the unmap the buffer mapped to SMMU backed device
 * also decrements the dma_buf kref count
 *
 * Parameters:
 * @arg: user space argument pointer
 * @fp: file pointer for kiumd_ctx
 *
 * Return: errno if fail; 0 in case of success
 */
static int kiumd_dmabuf_vfio_unmap(char __user *arg, struct file *fp)
{
	struct smmu_map_data *smap __free(kfree) = NULL;
	struct kiumd_ctx *kiumd_ctx;
	struct kiumd_user kiusr;
	bool found = false;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr)))
		return -EFAULT;

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_for_each_possible(kiumd_ctx->smmu_table, smap, node, kiusr.id) {
		if (smap->id == kiusr.id) {
			found = true;
			break;
		}
	}
	if (found)
		hash_del(&smap->node);

	spin_unlock(&kiumd_ctx->smmu_lock);
	if (!found) {
		smap = NULL;
		return -ENOENT;
	}

	trace_kiumd_dmabuf_vfio_unmap_start(dev_name(smap->dev), kiusr.vfio_fd,
					    kiusr.dma_buf_fd, kiusr.dma_addr,
					    kiusr.dma_attr, kiusr.dma_direction,
					    kiusr.ptselect, kiusr.is_iova_zero,
					    smap->size, kiumd_ctx);
	ret = __kiumd_dmabuf_vfio_unmap(kiumd_ctx, smap);
	if (ret)
		return ret;

	trace_kiumd_dmabuf_vfio_unmap_end(kiusr.vfio_fd);

	return 0;
}

/**
 * kiumd_dmabuf_managed_iova_map - Maps a DMA buffer to an IOVA for VFIO device access.
 * @arg: User-space pointer to a struct kiumd_user containing mapping parameters.
 * @fp: File pointer associated with the current context (used to retrieve kiumd_ctx).
 *
 * This function performs the following steps:
 * 1. Copies user-provided mapping parameters from user space.
 * 2. Retrieves the VFIO device using the provided vfio_fd.
 * 3. Gets the size of the DMA buffer using dma_buf_fd.
 * 4. Validates the DMA direction.
 * 5. Allocates and initializes the smmu_map_data structure.
 * 6. Allocates IOVA space if required.
 * 7. Maps the DMA buffer to the VFIO device.
 * 8. Updates the user structure with the mapped IOVA address and ID.
 * 9. Copies the updated structure back to user space.
 *
 * On failure, it performs cleanup including freeing IOVA and smap memory.
 *
 * Return: 0 on success, negative error code on failure.
 */
int kiumd_dmabuf_managed_iova_map(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	unsigned long fixed_iova = 0;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	struct device *dev;
	u64 size;
	int ret;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	size = kiumd_get_dmabuf_size(kiusr.dma_buf_fd);
	if (!size)
		return -EBADF;

	if ((kiusr.dma_direction < DMA_BIDIRECTIONAL)
	    || (kiusr.dma_direction > DMA_NONE)) {
		pr_err("%s:Invalid DMA direction: %d\n", __func__, kiusr.dma_direction);
		return -EINVAL;
	}

	if (kiusr.is_fix_map)
		fixed_iova = kiusr.dma_addr;

	smap = allocate_init_smap(kiusr, dev, size);
	if (!smap)
		return -ENOMEM;

	/* Need to review/remove this lock after cookie change removed */
	mutex_lock(&kiumd_ctx->map_lock);
	if (!kiusr.is_iova_zero) {
		ret = init_and_allocate_iova(dev, kiumd_ctx, smap,
					     kiumd_ctx->max_shift, fixed_iova, kiusr.is_fix_map);
		if (ret) {
			pr_err("%s: failed to allocate iova for: %s, ret: %d\n",
			       __func__, dev_name(dev), ret);
			mutex_unlock(&kiumd_ctx->map_lock);
			kfree(smap);
			return ret;
		}
	}

	ret = __kiumd_dmabuf_vfio_map(kiumd_ctx, kiusr, smap);
	mutex_unlock(&kiumd_ctx->map_lock);
	if (ret)
		goto smap_free;

	kiusr.id = smap->id;
	if (smap->is_iova_zero)
		kiusr.dma_addr = (unsigned long) IOVA_ZERO;
	else
		kiusr.dma_addr = (unsigned long) sg_dma_address(smap->sgt_ptr->sgl);

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed...\n", __func__);
		ret = -EFAULT;
		goto clean_map_res;
	}

	return 0;

clean_map_res:
	clean_map(kiumd_ctx, smap);
smap_free:
	if (!kiusr.is_iova_zero && smap->iova_rb) {
		if (free_allocated_iova(kiumd_ctx, smap->iova_rb))
			pr_err("%s:unable to free iova\n", __func__);
	}

	kfree(smap);
	return ret;
}

/**
 * kiumd_dmabuf_managed_iova_unmap - Unmaps a previously mapped DMA buffer from IOVA.
 * @arg: User-space pointer to a struct kiumd_user containing unmap parameters.
 * @fp: File pointer associated with the current context (used to retrieve kiumd_ctx).
 *
 * This function performs the following steps:
 * 1. Copies user-provided unmap parameters from user space.
 * 2. Validates the mapping ID.
 * 3. Retrieves the VFIO device using the provided vfio_fd.
 * 4. Searches for the smmu_map_data entry in the hash table using the ID.
 * 5. Removes the entry from the hash table if found.
 * 6. Unmaps the DMA buffer from the VFIO device.
 * 7. Frees the allocated IOVA space if applicable.
 *
 * Return: 0 on success, negative error code on failure.
 */
int kiumd_dmabuf_managed_iova_unmap(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	struct device *dev;
	bool found = false;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr)))
		return -EFAULT;

	if (kiusr.id < 0) {
		pr_err("%s:smap id passed from user should be positive value\n", __func__);
		return -EFAULT;
	}

	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_for_each_possible(kiumd_ctx->smmu_table, smap, node, kiusr.id) {
		if (smap->id == kiusr.id) {
			found = true;
			break;
		}
	}

	if (found)
		hash_del(&smap->node);

	spin_unlock(&kiumd_ctx->smmu_lock);

	if (!found)
		return -ENOENT;

	guard(mutex)(&kiumd_ctx->map_lock);
	ret = __kiumd_dmabuf_vfio_unmap(kiumd_ctx, smap);
	if (ret)
		return ret;

	if (!smap->is_iova_zero && smap->iova_rb) {
		ret = free_allocated_iova(kiumd_ctx, smap->iova_rb);
		if (ret) {
			pr_err("%s:unable to free iova\n", __func__);
			return ret;
		}
	}

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
	mutex_lock(&kiumd_ctx->hyp_lock);
	smap->id = kiumd_ctx->hyp_idx++;
	hash_add(kiumd_ctx->hyp_table, &smap->node, smap->id);
	map_ctx = kzalloc(sizeof(*map_ctx), GFP_KERNEL);
	if (!map_ctx) {
		ret = -ENOMEM;
		mutex_unlock(&kiumd_ctx->hyp_lock);
		goto hyp_unassign_sg;
	}

	map_ctx->nr_acl_entries = kiusr.mem_parcel.nr_acl_entries;
	map_ctx->vmids = vmids;
	map_ctx->perms = perms;
	smap->secure_ctx = map_ctx;
	mutex_unlock(&kiumd_ctx->hyp_lock);
	pr_debug("mapping attachment sgt:%pK attachmemt:%pK dmabuf:%pK, id: %d\n",
		 smap->sgt_ptr, smap->dmabufattach, smap->dmabuf_ptr, smap->id);
	kiusr.hyp_id = smap->id;
	kiusr.dma_addr = sg_dma_address(sgt->sgl);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s:%d copy_to_user failed...\n", __func__, __LINE__);
		ret = -EFAULT;
		goto hyp_unassign_sg;
	}

	pr_debug("returning from ioctl ret:%d dma add:%lx\n", ret, kiusr.dma_addr);
	return 0;
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
	bool found = false;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	if (kiusr.hyp_id < 0) {
		pr_err("%s:id passed from user should be positive value\n", __func__);
		return -EFAULT;
	}

	mutex_lock(&kiumd_ctx->hyp_lock);
	hash_for_each_possible(kiumd_ctx->hyp_table, smap, node, kiusr.hyp_id) {
		if (smap->id == kiusr.hyp_id) {
			found = true;
			break;
		}
	}

	hash_del(&smap->node);
	mutex_unlock(&kiumd_ctx->hyp_lock);
	if (!found) {
		pr_err("%s:Id not found id: %d\n", __func__, kiusr.hyp_id);
		return -ENOENT;
	}

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

	pr_debug("kiumd secure unmap:sgt from attachment:%p\n",
		 ((struct kiumd_dma_heap_attachment *)(dmabufattach->priv))->table);
	kiumd_dmabuf = smap->dmabuf_ptr;
	if (!kiumd_dmabuf) {
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto err;
	}

	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);
	mutex_lock(&kiumd_ctx->hyp_lock);
	kfree(smap->secure_ctx);
	kfree(smap);
	mutex_unlock(&kiumd_ctx->hyp_lock);
	pr_debug("%s: Hyp unassign done\n", __func__);
err:
	return ret;
}

static int kiumd_dmabuf_vfio_secure_map(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct dma_buf_attachment *dmabufattach;
	struct dma_buf *kiumd_dmabuf;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	int kiumd_dma_direction;
	struct sg_table *sgt;
	struct device *dev;
	int *vmids, *perms;
	u64 pgd;
	int ret;

	pr_debug(" %s: Entering.....\n", __func__);
	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user))) {
		pr_err("%s:%d bad params from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	ret = kiumd_get_pgd(dev, &pgd);
	if (ret)
		return -EINVAL;

	pr_debug("Kiumd VM page table ttbr:%llx\n", pgd);
	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		ret = !kiumd_dmabuf ? -EINVAL : PTR_ERR(kiumd_dmabuf);
		return ret;
	}

	trace_kiumd_dmabuf_vfio_secure_map_start(kiusr.vfio_fd,
						 kiusr.dma_buf_fd,
						 kiumd_dmabuf->size);
	ret = kiumd_acl_to_vmid_perms_list(kiusr.mem_parcel.nr_acl_entries,
					   (void *)kiusr.mem_parcel.acl_list,
					   &vmids, &perms);
	if (ret)
		return ret;

	dmabufattach = dma_buf_attach(kiumd_dmabuf, dev);
	if (IS_ERR(dmabufattach)) {
		dev_err(dev, "%s:%d dmabufattach is invalid\n", __func__, __LINE__);
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
	/* Grant Page table read access to peripheral VM*/
	ret = kiumd_io_pgtable_hyp_assign_page(vmids, pgd,
					       kiusr.mem_parcel.nr_acl_entries);
	if (ret < 0) {
		pr_err("%s:%d ownership transfer error:%d\n", __func__,
			__LINE__, ret);
		goto detach;
	}

	pr_debug("Pgtable ownership transfer success\n");
	ret = kiumd_hyp_assign_sg(sgt, vmids, kiusr.mem_parcel.nr_acl_entries,
				  true, perms);
	if (ret < 0) {
		pr_err("%s:%d ownership transfer error\n", __func__, __LINE__);
		goto hyp_unassign_table;
	}

	if (kiusr.dma_direction == 1)
		kiumd_dma_direction = kiusr.dma_direction;
	else
		kiumd_dma_direction = 0;

	sgt = dma_buf_map_attachment_unlocked(dmabufattach, kiumd_dma_direction);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		pr_err("%s:%d sgt is invalid\n", __func__, __LINE__);
		goto hyp_unassign_sg;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap) {
		ret = -ENOMEM;
		goto hyp_unassign_sg;
	}

	smap->dmabufattach = dmabufattach;
	smap->sgt_ptr = sgt;
	smap->dmabuf_ptr = kiumd_dmabuf;
	spin_lock(&kiumd_ctx->smmu_lock);
	smap->id = kiumd_ctx->id++;
	hash_add(kiumd_ctx->smmu_table, &smap->node, smap->id);
	spin_unlock(&kiumd_ctx->smmu_lock);
	kiusr.id = smap->id;
	kiusr.dma_addr = sg_dma_address(sgt->sgl);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s:%d copy_to_user failed...\n", __func__, __LINE__);
		ret = -EFAULT;
		goto unmap;
	}

	kfree(vmids);
	kfree(perms);
	pr_debug("returning from ioctl ret:%d\n", ret);
	trace_kiumd_dmabuf_vfio_secure_map_end(kiusr.vfio_fd, kiusr.id,
						kiusr.dma_addr);
	return 0;
unmap:
	dma_buf_unmap_attachment_unlocked(dmabufattach, (struct sg_table *)sgt,
					  DMA_BIDIRECTIONAL);
hyp_unassign_sg:
	kiumd_hyp_unassign_sg((struct sg_table *)sgt, vmids,
			      kiusr.mem_parcel.nr_acl_entries, true);
hyp_unassign_table:
	kiumd_io_pgtable_hyp_unassign_page(vmids, pgd,
					   kiusr.mem_parcel.nr_acl_entries);
detach:
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);
free_mem:
	kfree(vmids);
	kfree(perms);
	return ret;
}

/**
 * kiumd_dmabuf_vfio_secure_unmap - This function facilitates the secure
 * unmapping of a DMA-BUF based buffer to a SMMU backed device represented via
 * a vfio_device
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * Return: value is errno in failure casesor 0 in case of successful mapping
 */
static int kiumd_dmabuf_vfio_secure_unmap(char __user *arg, struct file *fp)
{
	struct file *vfio_file __free(fput) = NULL;
	struct dma_buf_attachment *dmabufattach;
	struct dma_buf *kiumd_dmabuf;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	int *vmids, *perms;
	bool found = false;
	struct device *dev;
	u64 pgd;
	int ret;

	pr_debug("%s entering\n", __func__);
	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	trace_kiumd_dmabuf_vfio_secure_unmap_start(kiusr.vfio_fd,
						   kiusr.dma_buf_fd,
						   kiusr.ptselect,
						   kiusr.dma_addr,
						   kiusr.id);
	if (kiusr.id < 0) {
		pr_err("%s:id passed from user should be positive value\n", __func__);
		return -EFAULT;
	}

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_for_each_possible(kiumd_ctx->smmu_table, smap, node, kiusr.id) {
		if (smap->id == kiusr.id) {
			found = true;
			break;
		}
	}
	if (!found) {
		spin_unlock(&kiumd_ctx->smmu_lock);
		return -ENOENT;
	}

	spin_unlock(&kiumd_ctx->smmu_lock);
	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	ret = kiumd_get_pgd(dev, &pgd);
	if (ret)
		return ret;

	/*Input param error checking for ACL happens in kiumd_acl_to_vmid_perms*/
	ret = kiumd_acl_to_vmid_perms_list(kiusr.mem_parcel.nr_acl_entries,
					   (void *)kiusr.mem_parcel.acl_list,
					   &vmids, &perms);
	if (ret)
		return ret;

	dmabufattach = smap->dmabufattach;
	if (!dmabufattach) {
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto free_mem;
	}

	if (!smap->sgt_ptr) {
		ret = -EINVAL;
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		goto free_mem;
	}

	dma_buf_unmap_attachment_unlocked(dmabufattach, smap->sgt_ptr,
					  DMA_BIDIRECTIONAL);
	ret = kiumd_hyp_unassign_sg(smap->sgt_ptr, vmids,
				    kiusr.mem_parcel.nr_acl_entries, true);
	if (ret < 0) {
		pr_err("%s:%d memory ownership transfer error:%d\n", __func__,
			__LINE__, ret);
		goto free_mem;
	}

	ret = kiumd_io_pgtable_hyp_unassign_page(vmids, pgd,
						 kiusr.mem_parcel.nr_acl_entries);
	if (ret < 0) {
		pr_err("%s:%d memory ownership transfer error:%d\n", __func__,
			__LINE__, ret);
		ret = -EINVAL;
		goto free_mem;
	}

	pr_debug("memory ownership transfer success\n");
	kiumd_dmabuf = smap->dmabuf_ptr;
	if (!kiumd_dmabuf) {
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto free_mem;
	}

	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);
	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	kfree(smap);
	spin_unlock(&kiumd_ctx->smmu_lock);
free_mem:
	kfree(vmids);
	kfree(perms);
	trace_kiumd_dmabuf_vfio_secure_unmap_end(kiusr.vfio_fd, kiusr.dma_buf_fd);
	return ret;
}

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
	struct kiumd_smmu_mmio_ctx *mmio_ctx;
	struct kiumd_smmu_mmio_map kiusr;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct resource *res;
	dma_addr_t dma_addr;
	struct device *dev;
	int iter, ret = 0;
	char *reg_name;
	u64 addr, size;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	trace_kiumd_mmio_smmu_map_start(kiusr.vfio_fd, kiusr.fixed_iova, kiusr.iova);
	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	spin_lock(&kiumd_ctx->smmu_lock);
	if (!hash_empty(kiumd_ctx->smmu_table)) {
		hash_for_each(kiumd_ctx->smmu_table, iter, smap, node) {
			if (!smap->context)
				continue;
			pr_debug("mmio ctx%p iova:%llx size:%lx\n", smap->context,
				 smap->context->iova,
				 smap->context->size);
			if (kiusr.fixed_iova &&
			    smap->context->iova == kiusr.iova) {
				spin_unlock(&kiumd_ctx->smmu_lock);
				pr_err("%s:error IOVA:%llx exists..\n", __func__, kiusr.iova);
				return -EINVAL;
			}
		}
	}
	spin_unlock(&kiumd_ctx->smmu_lock);

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap)
		return -ENOMEM;

	mmio_ctx = kzalloc(sizeof(*mmio_ctx), GFP_KERNEL);
	if (!mmio_ctx) {
		kfree(smap);
		return -ENOMEM;
	}

	reg_name = strndup_user(kiusr.reg_name, KIUMD_MAX_REG_NAME_LEN);
	if (IS_ERR(reg_name)) {
		pr_err("%s:%d invalid str\n", __func__, __LINE__);
		ret = -EINVAL;
		goto smap_free;
	}

	if (!dev_is_platform(dev)) {
		pr_err("%s:%d not platform device\n", __func__, __LINE__);
		ret = -EINVAL;
		goto reg_free;
	}

	res = platform_get_resource_byname(to_platform_device(dev),
					   IORESOURCE_MEM, reg_name);
	if (!res) {
		ret = -EINVAL;
		pr_err("%s:%d resource error\n", __func__, __LINE__);
		goto reg_free;
	}

	addr = res->start;
	size = resource_size(res);
	smap->size = size;
	ret = init_and_allocate_iova(dev, kiumd_ctx, smap,
				     PAGE_SHIFT, kiusr.iova, kiusr.fixed_iova);
	if (ret) {
		pr_err("%s: failed to allocate iova for: %s, ret: %d\n",
		       __func__, dev_name(dev), ret);
		ret = -EINVAL;
		goto reg_free;
	}

	dma_addr = dma_map_resource(dev, addr, size, 0, 0);
	ret = dma_mapping_error(dev, dma_addr);

	if (ret) {
		pr_err("%s:Failed to map with error: %d\n", __func__, ret);
		ret = free_allocated_iova(kiumd_ctx, smap->iova_rb);
		if (ret) {
			pr_err("%s:unable to free iova\n", __func__);
			ret = -ENOMEM;
		} else {
			ret = -EINVAL;
		}

		goto reg_free;
	}

	kiusr.iova = dma_addr;
	kiusr.reg_len = size;

	spin_lock(&kiumd_ctx->smmu_lock);
	smap->id = kiumd_ctx->id++;
	kiusr.id = smap->id;
	hash_add(kiumd_ctx->smmu_table, &smap->node, smap->id);
	mmio_ctx->iova = dma_addr;
	mmio_ctx->size = size;
	mmio_ctx->dev = dev;
	smap->context = mmio_ctx;
	spin_unlock(&kiumd_ctx->smmu_lock);

	pr_debug("%s:%s mapped pa:%llx size:%llx user iova:%llx dma_addr:%llx id:%d\n",
		 __func__, reg_name, addr, size, kiusr.iova, dma_addr, kiusr.id);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("kiumd:error in copying data:%d\n", ret);
		ret = -EFAULT;
		goto smap_del;
	}

	trace_kiumd_mmio_smmu_map_end(reg_name, kiusr.vfio_fd,  kiusr.id, kiusr.iova,
				      kiusr.reg_len);
	kfree(reg_name);
	return 0;
smap_del:
	dma_unmap_resource(dev, dma_addr, size, 0, 0);
	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	spin_unlock(&kiumd_ctx->smmu_lock);
reg_free:
	kfree(reg_name);
smap_free:
	kfree(smap->context);
	kfree(smap);

	return ret;
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
	struct file *vfio_file __free(fput) = NULL;
	struct kiumd_smmu_mmio_ctx *mmio_ctx;
	struct kiumd_smmu_mmio_map kiusr;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	bool found = false;
	struct device *dev;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	trace_kiumd_mmio_smmu_unmap_start(kiusr.vfio_fd, kiusr.iova, kiusr.id);

	if (kiusr.id < 0) {
		pr_err("%s:id passed from user should be positive value\n",
		       __func__);
		return -EFAULT;
	}

	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_for_each_possible(kiumd_ctx->smmu_table, smap, node, kiusr.id) {
		if (smap->id == kiusr.id && smap->context) {
			found = true;
			break;
		}
	}
	spin_unlock(&kiumd_ctx->smmu_lock);

	if (!found) {
		pr_err("%s:smmu id not found: %d\n", __func__, kiusr.id);
		return -ENOENT;
	}

	mmio_ctx = smap->context;
	if (!mmio_ctx) {
		pr_err("%s:invalid context:%d\n", __func__, __LINE__);
		return -EINVAL;
	}

	pr_debug("%s:mapping found:%d mmio_ctx:%p iova:%llx size:%lx\n",
		 __func__, kiusr.id, mmio_ctx, mmio_ctx->iova, mmio_ctx->size);

	ret = free_allocated_iova(kiumd_ctx, mmio_ctx->iova);
	if (ret) {
		pr_err("%s:unable to free iova\n", __func__);
		return ret;
	}

	dma_unmap_resource(mmio_ctx->dev, mmio_ctx->iova, mmio_ctx->size,
			   0, 0);

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	kfree(smap->context);
	kfree(smap);
	spin_unlock(&kiumd_ctx->smmu_lock);

	trace_kiumd_mmio_smmu_unmap_end(kiusr.id);

	return 0;
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
	kictx->kgsl_context = kzalloc(sizeof(*kictx->kgsl_context), GFP_KERNEL);
	if (!kictx->kgsl_context) {
		kfree(kictx);
		return -ENOMEM;
	}

	kictx->id = 0;
	kictx->pt_start_iova = KIUMD_32BIT_START_IOVA;
	kictx->pt_end_iova = KIUMD_32BIT_END_IOVA;
	kictx->is_initialized = false;
	kictx->hyp_idx = 0;
	kictx->kgsl_context->kgsl_start_iova = KGSL_PER_PROCESS_PT_BASE_IOVA;
	kictx->kgsl_context->kgsl_end_iova = KGSL_PER_PROCESS_PT_END_IOVA;
	hash_init(kictx->smmu_table);
	hash_init(kictx->hyp_table);
	spin_lock_init(&kictx->smmu_lock);
	spin_lock_init(&kictx->kgsl_context->kgsl_hash_lock);
	spin_lock_init(&kictx->pt_lock);
	mutex_init(&kictx->resmem_lock);
	mutex_init(&kictx->hyp_lock);
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
	struct iommu_domain *iommu_dom;
	struct dma_buf *kiumd_dmabuf;
	struct smmu_map_data *smap;
	struct hlist_node *tmp;
	int ret, iter;

	if (!hash_empty(ki_ctx->smmu_table)) {
		hash_for_each_safe(ki_ctx->smmu_table, iter, tmp, smap, node) {
			if (smap->context) {
				struct kiumd_smmu_mmio_ctx *mmio_ctx = smap->context;
				pr_debug("Free mmio ctx%p iova:%llx size:%lx\n",
					 mmio_ctx, mmio_ctx->iova, mmio_ctx->size);
				dma_unmap_resource(mmio_ctx->dev, mmio_ctx->iova, mmio_ctx->size, 0, 0);
				kfree(smap->context);
			}

			if (smap->dmabuf_ptr) {
				kiumd_dmabuf = smap->dmabuf_ptr;
				pr_debug("kiumd_debug: Driver close : unmap sgt_ptr:%pK\n",
					 smap->sgt_ptr);
				if (smap->ptselect == KGSL_GLOBAL_PT ||
				    smap->ptselect == KGSL_PER_PROCESS_PT ||
				    smap->ptselect == KGSL_DEFAULT_PT)
					continue;

				if (smap->is_fixed_map) {
					ret = kiumd_configure_dma_cookie(
							smap->dev,
							IOMMU_DMA_MSI_COOKIE,
							(dma_addr_t)smap->dmabuf_ptr);
					if (ret) {
						pr_err("%s %d failed to configure cookie\n", __func__, __LINE__);
						break;
					}
				}

				if (smap->dmabufattach && smap->sgt_ptr)
					dma_buf_unmap_attachment_unlocked(smap->dmabufattach,
									  smap->sgt_ptr,
									  smap->dma_dir);

				pr_debug("kiumd_debug: unmap dmabufatach:%pK\n",
						smap->dmabufattach);
				if (smap->is_iova_zero) {
					iommu_dom = kiumd_iommu_get_dma_domain(smap->dev);
					if (!iommu_dom) {
						pr_err("%s:IOMMU domain is NULL\n", __func__);
						continue;
					}
					pr_debug("kiumd_debug: unmap iommu_dom:%llx, size: %lu\n",
						 (u64)iommu_dom,
						 kiumd_dmabuf->size);
					ret = iommu_unmap(iommu_dom, 0,
							  kiumd_dmabuf->size);
					if (ret != kiumd_dmabuf->size) {
						pr_err("%s:iommu_unmap failed\n", __func__);
						continue;
					}
				}

				if (smap->is_fixed_map) {
					ret = kiumd_configure_dma_cookie(
							smap->dev,
							IOMMU_DMA_IOVA_COOKIE,
							(dma_addr_t)smap->dmabuf_ptr);
					if (ret) {
						pr_err("%s %d failed to configure cookie\n", __func__, __LINE__);
						break;
					}
				}

				if (ki_ctx->pgtable_ctx && !smap->is_iova_zero && smap->iova_rb)
					free_iova_range(ki_ctx->pgtable_ctx, smap->iova_rb);

				dma_buf_detach(kiumd_dmabuf,
						(struct dma_buf_attachment *)smap->dmabufattach);
				dma_buf_put(kiumd_dmabuf);
				pr_debug("kiumd_debug: unmap done\n");
			}

			hash_del(&smap->node);
			kfree(smap);
		}
	}

	kfree(ki_ctx->pgtable_ctx);
	kfree(ki_ctx->res_mem_area);
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
	struct kiumd_dev_mem_info kiusr;
	struct kiumd_ctx *kiumd_ctx;
	struct device_node *mem_np;
	struct reserved_mem *rmem;
	struct device_node *np;
	struct device *dev;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EINVAL;
	}

	trace_kiumd_vfio_ctx_init_start(kiusr.vfio_fd);
	dev = kiumd_vfio_get_device(kiusr.vfio_fd, &vfio_file);
	if (!dev)
		return -EBADF;

	np = dev_of_node(dev);
	if (!np) {
		dev_err(dev, "%s:No memory-region specified\n", __func__);
		return -EINVAL;
	}

	guard(mutex)(&kiumd_ctx->resmem_lock);
	if (kiumd_ctx->num_reserved_regions)
		return -ENOMEM;

	kiumd_ctx->num_reserved_regions = of_property_count_elems_of_size(np,
									  "memory-region",
									  sizeof(phandle));
	if (kiumd_ctx->num_reserved_regions <= 0)
		return -EINVAL;

	kiusr.num_regions = kiumd_ctx->num_reserved_regions;
	if (kiumd_ctx->res_mem_area)
		pr_err("%s:res_mem_area repeatedly allocates memory\n", __func__);

	kiumd_ctx->res_mem_area = kcalloc(kiumd_ctx->num_reserved_regions,
					  sizeof(*kiumd_ctx->res_mem_area),
					  GFP_KERNEL);
	if (!kiumd_ctx->res_mem_area) {
		pr_err("%s:%d fail to allocate kiumd_ctx->res_mem_area\n", __func__, __LINE__);
		return -ENOMEM;
	}

	for (u64 i = 0; i < kiusr.num_regions; i++) {
		mem_np = of_parse_phandle(dev->of_node, "memory-region", i);
		if (!mem_np) {
			pr_debug("%s:cant find phandle\n", __func__);
			continue;
		}

		rmem = of_reserved_mem_lookup(mem_np);
		if (!rmem) {
			of_node_put(mem_np);
			pr_err("%s:No memory address assigned to the reserved region\n", __func__);
			kfree(kiumd_ctx->res_mem_area);
			return -EINVAL;
		}

		of_node_put(mem_np);
		kiumd_ctx->res_mem_area[i].size = rmem->size;
		kiumd_ctx->res_mem_area[i].base = rmem->base;
	}
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s:error in copying vfio ctx data for reserved memory\n",
		       __func__);
		return -EFAULT;
	}

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
	struct kiumd_dev_mem_info kiusr;
	struct kiumd_ctx *kiumd_ctx;
	struct kiumd_mem_info *mem_info;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (!kiusr.num_regions ||
			kiusr.num_regions != kiumd_ctx->num_reserved_regions)
		return -EINVAL;

	if (!kiusr.mem_info) {
		pr_err("%s: Invalid kiusr.mem_info pointer\n", __func__);
		return -EINVAL;
	}

	mem_info = kcalloc(kiumd_ctx->num_reserved_regions,
			sizeof(struct kiumd_mem_info), GFP_KERNEL);
	if (!mem_info)
		return -ENOMEM;

	for (u64 i = 0; i < kiusr.num_regions; i++) {
		mem_info[i].size = kiumd_ctx->res_mem_area[i].size;
		mem_info[i].offset = i << KIUMD_INDEX_OFFSET;
		pr_debug("%s:base:%llx size:%llx offset:%llx\n", __func__,
			 kiumd_ctx->res_mem_area[i].base,
			 mem_info[i].size, mem_info[i].offset);
	}
	if (copy_to_user(kiusr.mem_info, mem_info,
				kiumd_ctx->num_reserved_regions * sizeof(struct kiumd_mem_info))) {
		kfree(mem_info);
		pr_err("%s:error in copying vfio ctx data for reserved memory\n",
		       __func__);
		return -EFAULT;
	}

	kfree(mem_info);

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
		err = kiumd_dmabuf_vfio_map(argp, file);
		break;
	case KIUMD_SMMU_UNMAP_BUF:
		err = kiumd_dmabuf_vfio_unmap(argp, file);
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
	case KIUMD_SMMU_SECURE_MAP:
		err = kiumd_dmabuf_vfio_secure_map(argp, file);
		break;
	case KIUMD_SMMU_SECURE_UNMAP:
		err = kiumd_dmabuf_vfio_secure_unmap(argp, file);
		break;
	case KIUMD_SMMU_MMIO_MAP:
		err = kiumd_mmio_smmu_map(argp, file);
		break;
	case KIUMD_SMMU_MMIO_UNMAP:
		err = kiumd_mmio_smmu_unmap(argp, file);
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
	case KIUMD_SMMU_MANAGED_IOVA_MAP:
		err = kiumd_dmabuf_managed_iova_map(argp, file);
		break;
	case KIUMD_SMMU_MANAGED_IOVA_UNMAP:
		err = kiumd_dmabuf_managed_iova_unmap(argp, file);
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
	struct kiumd_ctx *ki_ctx;
	u64 req_len, index, base;
	size_t size;

	if (!file) {
		pr_err("%s:file ptr returns NULL\n", __func__);
		return -EINVAL;
	}

	ki_ctx = (struct kiumd_ctx *)file->private_data;
	if (!ki_ctx) {
		pr_err("%s:kiumd ctx is NULL\n", __func__);
		return -EINVAL;
	}

	if (!vma) {
		pr_err("%s:vma is NULL\n", __func__);
		return -EINVAL;
	}

	index = vma->vm_pgoff >> (KIUMD_INDEX_OFFSET - PAGE_SHIFT);
	if (ki_ctx->num_reserved_regions <= index) {
		pr_err("%s:Invalid index:%llx\n", __func__, index);
		return -EINVAL;
	}

	guard(mutex)(&ki_ctx->resmem_lock);
	if (!ki_ctx->res_mem_area) {
		pr_err("%s:No reserved mem areas\n", __func__);
		return -EINVAL;
	}

	base = ki_ctx->res_mem_area[index].base;
	size = ki_ctx->res_mem_area[index].size;
	if (vma->vm_end < vma->vm_start) {
		pr_err("%s:Invalid vm start and end\n", __func__);
		return -EINVAL;
	}

	index = vma->vm_pgoff >> (KIUMD_INDEX_OFFSET - PAGE_SHIFT);
	if (ki_ctx->num_reserved_regions <= index) {
		pr_err("%s:Invalid index:%llx\n", __func__, index);
		return -EINVAL;
	}

	pr_debug("%s:index:%llx\n", __func__, index);
	if (base & ~PAGE_MASK) {
		pr_err("%s:Unalligned base address\n", __func__);
		return -EINVAL;
	}

	req_len = vma->vm_end - vma->vm_start;
	vma->vm_pgoff = vma->vm_pgoff &
			((1ULL << (KIUMD_INDEX_OFFSET - PAGE_SHIFT)) - 1);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	pr_debug("%s:res mem start:%llx End:%llx size:%lx vma start:%lx vma end:%lx size:%lx offset:%lx\n",
			__func__, base, base + size, size, vma->vm_start,
			vma->vm_end, vma->vm_end - vma->vm_start, vma->vm_pgoff);

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

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vfio.h>
#include <linux/hashtable.h>
#include <uapi/misc/kiumd.h>
#include <linux/iommu.h>
#include <linux/types.h>
#include <linux/iova.h>
#include <linux/adreno-smmu-priv.h>
#include <linux/io-pgtable.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/iommu_iova_map.h>
#include <linux/sizes.h>
#include <linux/xarray.h>
#include <uapi/misc/scm_user_intf.h>
#include <linux/dma-direction.h>

#include "arm-smmu.h"
#include "vfio.h"
/*Global Data structures needed for buffer sharing */
static DEFINE_XARRAY_ALLOC(kiumd_xa);

/*No.of pages for 3GB IOVA space with 4K page size*/
#define KGSL_PT_MEM_PAGES 0xC0000
static DECLARE_BITMAP(global_map, KGSL_PT_MEM_PAGES);
static DECLARE_BITMAP(perprocess_map, KGSL_PT_MEM_PAGES);

#define KGSL_GLOBAL_PT_BASE_IOVA 0xFFFFFF8000000000
#define KGSL_PER_PROCESS_PT_BASE_IOVA 0x60000000
#define SMMU_MAPTABLE_SIZE (10)

#define MAX_KIUMD_ACL_ENTRIES 64
#define KIUMD_MAX_VMID        64
#define KIUMD_MAX_PERMS       8

#define KIUMD_MAX_REG_NAME_LEN (100)

struct kiumd_smmu_mmio_ctx {
	struct device *dev;
	dma_addr_t iova;
	size_t size;
};

struct dmabuf_fd {
	struct dma_buf *kiumd_dmabuf; //Value
	uint32_t token;  //Key
	struct list_head *next;
};

enum iommu_dma_cookie_type {
	IOMMU_DMA_IOVA_COOKIE,
	IOMMU_DMA_MSI_COOKIE,
};

struct dma_buf_handle {
	unsigned long dmabuf;
	atomic_t handle_refcount;
};

/*
 * This is a redefinition of kernel struct - struct dma_heap_attachment
 * to obtain the sgtable to map buffers with specific attributes
 * using dma_map_sgtable
 */

struct kiumd_dma_heap_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
	bool mapped;
};

/*
 * This is a redefinition of kernel struct - struct iommu_dma_cookie
 * We are using the iova cookie to set the IOVA, when device want to
 * map a buffer at a specific IOVA
 */

struct kiumd_iommu_dma_cookie {
	enum iommu_dma_cookie_type      type;
	union {
		struct {
			struct iova_domain      iovad;
			struct iova_fq __percpu *fq;    /* Flush queue */
			atomic64_t              fq_flush_start_cnt;
			atomic64_t              fq_flush_finish_cnt;
			struct timer_list       fq_timer;
			atomic_t                fq_timer_on;
		};
		dma_addr_t              msi_iova;
	};
	struct list_head                msi_page_list;
	struct iommu_domain             *fq_domain;
	struct mutex			mutex;
};

/*
 * This is a kernel redefinition of the struct - iommu_group, to obtain
 * iommu domain from default domain, the iommu domain from iommu group is
 * a blocking domain with the latest update from vfio frameworks on linux6.1
 * and shouldn't be used
 */

struct kiumd_iommu_group {
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

static void _tlb_flush_all(void *cookie)
{
}

static void _tlb_flush_walk(unsigned long iova, size_t size,
		size_t granule, void *cookie)
{
}

static void _tlb_add_page(struct iommu_iotlb_gather *gather,
		unsigned long iova, size_t granule, void *cookie)
{
}

static const struct iommu_flush_ops kgsl_iopgtbl_tlb_ops = {
	.tlb_flush_all = _tlb_flush_all,
	.tlb_flush_walk = _tlb_flush_walk,
	.tlb_add_page = _tlb_add_page,
};

/**
 * struct kiumd_ctx: Structure for kiumd_ctx .
 * @id: id to map/unmap entries in hashtable
 * @smmu_map_data: structurefor hashtable data
 * @smmu_lock: Lock for map/unmap operations
 * @smmu_table: Hashtable to hold entries based on id
 *
 */

struct kiumd_ctx {
	int id;
	struct hlist_node smmu_map_data;
	spinlock_t smmu_lock;
	DECLARE_HASHTABLE(smmu_table, SMMU_MAPTABLE_SIZE);
};

/**
 * struct  smmu_map_data: Structure for hashtable data .
 * @id: id to map/unmap entries in hashtable
 * @sgt_ptr: sgt pointer value
 * @dmabuf_ptr: dma buf pointer for map operations
 * @dmabufattach: dmabufattach value
 * @node: hlist_node
 *
 */

struct smmu_map_data {
	int id;
	long sgt_ptr;
	long dmabuf_ptr;
	long dmabufattach;
	void *context;
	struct hlist_node node;
};

static struct io_pgtable *pgtable;

struct iommu_domain *kiumd_iommu_get_dma_domain(struct device *dev)
{
	struct kiumd_iommu_group *iommu_group;

	iommu_group = (struct kiumd_iommu_group *) dev->iommu_group;
	if (!iommu_group) {
		dev_err(dev, "%s:iommu group is invalid\n", __func__);
		return NULL;
	}

	return iommu_group->default_domain;
}

void *kiumd_iommu_group_default_domain(void *group)
{
	struct kiumd_iommu_group *iommu_group = (struct kiumd_iommu_group *) group;

	if (!iommu_group)
		return NULL;

	return (void *)iommu_group->default_domain;
}
EXPORT_SYMBOL_GPL(kiumd_iommu_group_default_domain);

static struct vfio_device *kiumd_get_vfio_device(int fd)
{
	struct vfio_device *vfio_dev = NULL;
	struct file *file;
	struct vfio_device_file *df;

	if (fd < 0)
		return NULL;

	file = fget(fd);
	if (!file)
		return NULL;

	if (!vfio_file_is_valid(file))
		goto close_file;

	df = (struct vfio_device_file *)file->private_data;
	if (!df)
		goto close_file;

	vfio_dev = (struct vfio_device *)df->device;

close_file:
	fput(file);
	return vfio_dev;
}

static struct kiumd_iommu_dma_cookie *kiumd_get_dma_cookie(struct vfio_device *vfio_dev)
{
	struct iommu_domain *domain;
	struct kiumd_iommu_dma_cookie *cookie;

	if (!vfio_dev) {
		pr_err("%s:vfio dev is NULL\n", __func__);
		return NULL;
	}

	if (!vfio_dev->dev) {
		pr_err("%s:vfio dev is NULL\n", __func__);
		return NULL;
	}

	domain = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!domain) {
		pr_err("%s:iommu domain is NULL for %s\n", __func__, dev_name(vfio_dev->dev));
		return NULL;
	}

	cookie = (struct kiumd_iommu_dma_cookie *)domain->iova_cookie;
	if (!cookie) {
		pr_err("%s:cookie not found\n", __func__);
		return NULL;
	}

	return cookie;
}

static int kiumd_set_dma_cookie(struct kiumd_iommu_dma_cookie *cookie,
				enum iommu_dma_cookie_type type, dma_addr_t iova)
{
	if (!cookie) {
		pr_err("%s:Unable to set cookie\n", __func__);
		return -EINVAL;
	}

	cookie->type = type;
	cookie->msi_iova = iova;

	return 0;
}

static int kiumd_set_dma_cookie_unlocked(struct kiumd_iommu_dma_cookie *cookie,
					 enum iommu_dma_cookie_type type, dma_addr_t iova)
{
	int ret;

	if (!cookie) {
		pr_err("%s:Unable to set cookie\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&cookie->mutex);
	ret = kiumd_set_dma_cookie(cookie, type, iova);
	mutex_unlock(&cookie->mutex);
	return ret;
}

void kiumd_smmuv2_write_context_bank(struct arm_smmu_device *smmu, int idx)
{
	u32 reg;
	bool stage1;
	struct arm_smmu_cb *cb = &smmu->cbs[idx];
	struct arm_smmu_cfg *cfg = cb->cfg;

	stage1 = cfg->cbar != CBAR_TYPE_S2_TRANS;

	if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64)
		reg = ARM_SMMU_CBA2R_VA64;
	else
		reg = 0;

	arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBA2R(idx), reg);
	reg = FIELD_PREP(ARM_SMMU_CBAR_TYPE, cfg->cbar);

	if (stage1) {
		reg |= FIELD_PREP(ARM_SMMU_CBAR_S1_BPSHCFG,
			ARM_SMMU_CBAR_S1_BPSHCFG_NSH) |
			FIELD_PREP(ARM_SMMU_CBAR_S1_MEMATTR,
				ARM_SMMU_CBAR_S1_MEMATTR_WB);
	} else if (!(smmu->features & ARM_SMMU_FEAT_VMID16)) {
		/* 8-bit VMIDs live in CBAR */
		reg |= FIELD_PREP(ARM_SMMU_CBAR_VMID, cfg->vmid);
	}

	arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBAR(idx), reg);

	if (stage1)
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TCR2, cb->tcr[1]);

	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TCR, cb->tcr[0]);

	arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_TTBR0, cb->ttbr[0]);

	if (stage1)
		arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_TTBR1, cb->ttbr[1]);

	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_S1_MAIR0, cb->mair[0]);
	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_S1_MAIR1, cb->mair[1]);

	reg = ARM_SMMU_SCTLR_CFIE | ARM_SMMU_SCTLR_CFRE | ARM_SMMU_SCTLR_AFE |
		ARM_SMMU_SCTLR_TRE | ARM_SMMU_SCTLR_M;

	reg |= ARM_SMMU_SCTLR_S1_ASIDPNE;

	smmu->impl->write_sctlr(smmu, idx, reg);
}

int kiumd_smmuv2_set_ttbr0_cfg(const void *cookie,
		const struct io_pgtable_cfg *pgtbl_cfg)
{

	struct arm_smmu_domain *smmu_domain = (void *)cookie;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_cb *cb = &smmu_domain->smmu->cbs[cfg->cbndx];
	u32 tcr = cb->tcr[0];

	if (!(cb->tcr[0] & ARM_SMMU_TCR_EPD0)) {
		pr_err("TTBR0 translation is already enabled");
		return -EINVAL;
	}

	tcr |= arm_smmu_lpae_tcr(pgtbl_cfg);
	tcr &= ~(ARM_SMMU_TCR_EPD0 | ARM_SMMU_TCR_EPD1);

	cb->tcr[0] = tcr;
	cb->ttbr[0] = pgtbl_cfg->arm_lpae_s1_cfg.ttbr;
	cb->ttbr[0] |= FIELD_PREP(ARM_SMMU_TTBRn_ASID, cb->cfg->asid);

	kiumd_smmuv2_write_context_bank(smmu_domain->smmu, cb->cfg->cbndx);

	return 0;
}

int kiumd_perprocess_set_user_context(char __user *arg)
{
	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct io_pgtable_cfg cfg;
	struct arm_smmu_domain *smmu_dom;
	struct iommu_domain *iommu_dom;
	int cbindx, ret;
	void *cookie;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	if (kismmu_pproc.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kismmu_pproc.vfio_fd);
	if (!file) {
		pr_err("%s:failed to get file from vfio fd\n", __func__);
		return -EBADF;
	}
	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:iommu domain is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if ((!smmu_dom) || (!(smmu_dom->pgtbl_ops))) {
		pr_err("%s:smmu domain/pagetable ops is invalid\n", __func__);
		fput(file);
		return -EINVAL;
	}

	if (!pgtable) {
		pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
		if (!pgtable) {
			pr_err("%s:pagetable is NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}
	}

	cbindx = smmu_dom->cfg.cbndx;
	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	/*Allocate a default pagetable for TTBR0 in case per process allocation fails*/
	kismmu_pproc.pgtbl_ops_ptr = (long)alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!kismmu_pproc.pgtbl_ops_ptr) {
		pr_err("%s:failed to allocate pagetable ops.\n", __func__);
		fput(file);
		return -ENOMEM;
	}

	cookie = (void *)smmu_dom;
	kiumd_smmuv2_set_ttbr0_cfg(cookie, &cfg);
	ret = qcom_scm_kgsl_set_smmu_aperture(cbindx);
	if (ret == -EBUSY)
		ret = qcom_scm_kgsl_set_smmu_aperture(cbindx);

	if (ret) {
		pr_err("%s:Setting smmu aperture error.\n", __func__);
		fput(file);
		return ret;
	}

	fput(file);
	return 0;
}

int kiumd_perprocess_pt_alloc(char __user *arg)
{
	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct io_pgtable_cfg cfg;
	struct arm_smmu_domain *smmu_dom;
	struct iommu_domain *iommu_dom;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	if (kismmu_pproc.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kismmu_pproc.vfio_fd);
	if (!file) {
		pr_err("%s:failed to get file from vfio fd\n", __func__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:iommu domain is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if ((!smmu_dom) || (!(smmu_dom->pgtbl_ops))) {
		pr_err("%s:smmu domain/pagetable ops is invalid\n", __func__);
		fput(file);
		return -EINVAL;
	}

	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	kismmu_pproc.asid = smmu_dom->cfg.asid;
	kismmu_pproc.pgtbl_ops_ptr = (long)alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!(kismmu_pproc.pgtbl_ops_ptr)) {
		pr_err("%s:failed to allocate pagetable ops\n", __func__);
		fput(file);
		return -EINVAL;
	}

	kismmu_pproc.ttbr0 = cfg.arm_lpae_s1_cfg.ttbr;
	fput(file);

	if (copy_to_user(arg, &kismmu_pproc, sizeof(kismmu_pproc))) {
		pr_err("%s: copy_to_user failed...\n", __func__);
		return -EFAULT;
	}

	return 0;
}

int kiumd_global_pgtble_set(char __user *arg)
{

	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct iommu_domain *iommu_dom;
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable_ops *ki_pgtbl_ops;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	if (kismmu_pproc.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kismmu_pproc.vfio_fd);
	if (!file) {
		pr_err("%s:failed to get file from vfio fd\n", __func__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		fput(file);
		return -ENOTTY;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:IOMMU domain is NULL\n", __func__);
		fput(file);
		return -ENOMEM;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_dom) {
		pr_err("%s:SMMU domain is NULL\n", __func__);
		fput(file);
		return -ENOMEM;
	}

	if (!pgtable) {
		pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
		if (!pgtable) {
			pr_err("%s:pagetable is NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}
	}

	ki_pgtbl_ops = (struct io_pgtable_ops *) (&pgtable->ops);
	if (!ki_pgtbl_ops) {
		pr_err("%s:pagetable ops is NULL\n", __func__);
		fput(file);
		return -ENOMEM;
	}

	smmu_dom->pgtbl_ops = ki_pgtbl_ops;
	fput(file);

	return 0;
}

int kiumd_perprocess_pgtble_set(char __user *arg)
{
	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct iommu_domain *iommu_dom;
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable_ops *ki_pgtbl_ops;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	if (kismmu_pproc.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kismmu_pproc.vfio_fd);
	if (!file) {
		pr_err("%s:failed to get file from vfio fd\n", __func__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:IOMMU domain is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_dom) {
		pr_err("%s:SMMU domain is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	ki_pgtbl_ops = (struct io_pgtable_ops *)kismmu_pproc.pgtbl_ops_ptr;
	smmu_dom->pgtbl_ops = ki_pgtbl_ops;
	fput(file);

	return 0;
}

int kiumd_perprocess_pgtble_free(char __user *arg)
{
	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct io_pgtable_ops *ki_pgtbl_ops;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	if (kismmu_pproc.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kismmu_pproc.vfio_fd);
	if (!file) {
		pr_err("%s:failed to get file from vfio fd\n", __func__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	ki_pgtbl_ops = (struct io_pgtable_ops *)kismmu_pproc.pgtbl_ops_ptr;
	if (!ki_pgtbl_ops) {
		pr_err("%s:pagegetable ops is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	free_io_pgtable_ops(ki_pgtbl_ops);
	fput(file);

	return 0;
}

int kiumd_dmabuf_custom_iova_init(char __user *arg)
{
	struct kiumd_user kiusr;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct file *file;
	struct kiumd_iommu_dma_cookie *cookie = NULL;
	struct iommu_domain *domain = NULL;
	struct iova_domain *iovad = NULL;
	struct iommu_resv_region *region;
	unsigned long lo, hi;
	LIST_HEAD(resrvd);
	int ret;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	if (kiusr.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kiusr.vfio_fd);
	if (!file) {
		pr_err("%s:Invalid vfio fd\n", __func__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev)  {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	ret = dma_set_max_seg_size(vfio_dev->dev, (unsigned int) DMA_BIT_MASK(32));
	//Print a warning and continue.
	if (ret)
		pr_err("%s:WARNING: max_segment size not set.\n", __func__);

	domain = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!domain) {
		pr_err("%s:dma_domain is invalid \n", __func__);
		fput(file);
		return -EINVAL;
	}

	cookie = (struct kiumd_iommu_dma_cookie *)domain->iova_cookie;
	iovad = &cookie->iovad;

	qcom_iommu_generate_resv_regions(vfio_dev->dev, &resrvd);
	list_for_each_entry(region, &resrvd, list) {
		lo = iova_pfn(iovad, region->start);
		hi = iova_pfn(iovad, region->start + region->length - 1);
		reserve_iova(iovad, lo, hi);
	}

	fput(file);
	return 0;
}

void clear_map_iova(u64 iova, u64 size, int ptselect)
{
	u64 bit;

	if (ptselect == KGSL_GLOBAL_PT) {
		bit = (iova & ~KGSL_GLOBAL_PT_BASE_IOVA) >> PAGE_SHIFT;
		bitmap_clear(global_map, bit, size >> PAGE_SHIFT);
	} else if (ptselect == KGSL_PER_PROCESS_PT) {
		bit = (iova - KGSL_PER_PROCESS_PT_BASE_IOVA) >> PAGE_SHIFT;
		bitmap_clear(perprocess_map, bit, size >> PAGE_SHIFT);
	}
}

s64 get_map_offset(u64 size, int ptselect)
{
	static u64 last_offset_global = 0;
	static u64 last_offset_perprocess = 0;
	u64 *last_offset = (ptselect == KGSL_GLOBAL_PT) ? &last_offset_global : &last_offset_perprocess;
	u64 bit, offset;

	if (ptselect != KGSL_GLOBAL_PT && ptselect != KGSL_PER_PROCESS_PT) {
		pr_err("%s: Invalid ptselect: %d\n", __func__, ptselect);
		return (s64) -EINVAL;
	}

	unsigned long *map = (ptselect == KGSL_GLOBAL_PT) ? global_map : perprocess_map;
	if(map == NULL) {
		pr_err("%s: Bitmap map is NULL for ptselect: %d\n", __func__, ptselect);
		return (s64) -EFAULT;
	}

	if (size == 0 || (size >> PAGE_SHIFT) == 0) {
		pr_err("%s: Invalid size: 0x%llx, for ptselect: %d\n", __func__, size, ptselect);
		return (s64) -EINVAL;
	}

	bit = bitmap_find_next_zero_area(map, KGSL_PT_MEM_PAGES, *last_offset, size >> PAGE_SHIFT, 0);

	if (bit + (size >> PAGE_SHIFT) >= KGSL_PT_MEM_PAGES) {
		bit = bitmap_find_next_zero_area(map, KGSL_PT_MEM_PAGES, 0, size >> PAGE_SHIFT, 0);
		if (bit >= KGSL_PT_MEM_PAGES) {
			pr_err("%s: No free area in bitmap for size: 0x%llx, ptselect: %d\n", __func__, size, ptselect);
			return (s64) -ENOSPC;
		}
	}

	bitmap_set(map, bit, size >> PAGE_SHIFT);
	offset = bit << PAGE_SHIFT;

	*last_offset = (bit + (size >> PAGE_SHIFT)) % KGSL_PT_MEM_PAGES;

	return (s64) offset;
}

int set_map_iova(u64 offset, struct vfio_device *vfio_dev, int ptselect)
{
	dma_addr_t iova;
	int ret;
	struct kiumd_iommu_dma_cookie *cookie;

	if (ptselect == KGSL_GLOBAL_PT)
		iova = KGSL_GLOBAL_PT_BASE_IOVA + offset;
	else if (ptselect == KGSL_PER_PROCESS_PT)
		iova = KGSL_PER_PROCESS_PT_BASE_IOVA + offset;
	else
		pr_err("%s invalid ptselect\n", __func__);

	cookie = kiumd_get_dma_cookie(vfio_dev);
	if (!cookie) {
		pr_err("%s failed to get cookie\n", __func__);
		return -EINVAL;
	}

	ret = kiumd_set_dma_cookie_unlocked(cookie, IOMMU_DMA_MSI_COOKIE, iova);
	if (ret)
		pr_err("%s failed to set cookie\n", __func__);

	return ret;
}

static void kiumd_mangle_sg_table(struct sg_table *sg_table)
{
	int i;
	struct scatterlist *sg;

	for_each_sgtable_sg(sg_table, sg, i)
		sg->page_link ^= ~0xffUL;
}


/**
 * kiumd_dmabuf_vfio_map(char __user *arg, struct file *fp)
 *
 * This function facilitates the mapping of a DMA-BUF based buffer to a SMMU
 * backed device represented via a vfio_device.
 *
 * The function is called via IOCTL interface and input is provided via struct
 * kiumd_user from the userspace.
 *
 * return value is errno or 0 in case of successful mapping
 */

int kiumd_dmabuf_vfio_map(char __user *arg, struct file *fp)
{
	struct kiumd_user kiusr;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct file *file;
	struct dma_buf *kiumd_dmabuf = NULL;
	struct dma_buf_attachment *dmabufattach = NULL;
	struct sg_table *sgt = NULL;
	int kiumd_dma_direction, ret;
	u64 size;
	s64 offset;
	struct kiumd_dma_heap_attachment *dmaheapattachment;
	struct iommu_domain *iommu_dom;
	dma_addr_t iova_zero = 0;
	int prot = 0;
	struct kiumd_ctx *kiumd_ctx = NULL;
	struct smmu_map_data *smap = NULL;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	if (kiusr.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kiusr.vfio_fd);
	if (!file) {
		pr_err("%s:failed to get file from vfio fd\n", __func__);
		return -EBADF;
	}
	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (vfio_dev == NULL) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		ret = -EINVAL;
		goto fail_fput;
	}

	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:dma_buf_get failed with error: %ld, for device: %s\n", __func__, PTR_ERR(kiumd_dmabuf), vfio_dev->dev->kobj.name);
		ret = (kiumd_dmabuf == NULL ? -EINVAL : PTR_ERR(kiumd_dmabuf));
		goto fail_fput;
	}

	if ((kiusr.ptselect == KGSL_GLOBAL_PT) || (kiusr.ptselect == KGSL_PER_PROCESS_PT)) {
		size = kiumd_dmabuf->size;
		offset = get_map_offset(size, kiusr.ptselect);
		if (offset < 0) {
			pr_err("%s:failed to get offset\n", __func__);
			ret = offset;
			goto fail_put;
		}

		ret = set_map_iova((u64)offset, vfio_dev, kiusr.ptselect);
		if (ret < 0) {
			pr_err("%s:failed to set offset\n", __func__);
			goto fail_put;
		}
	}

	if (!(vfio_dev->dev)) {
		pr_err("%s:vfio_dev->dev is NULL\n", __func__);
		ret = -ENODEV;
		goto fail_put;
	}

	dmabufattach = dma_buf_attach(kiumd_dmabuf, vfio_dev->dev);
	if (IS_ERR(dmabufattach)) {
		pr_err("%s:dmabufattach failed with error: %ld, for device: %s\n", __func__, PTR_ERR(dmabufattach), vfio_dev->dev->kobj.name);
		ret = PTR_ERR(dmabufattach);
		goto fail_put;
	}

	if ((kiusr.dma_direction < DMA_BIDIRECTIONAL) || (kiusr.dma_direction > DMA_NONE)) {
		pr_err("%s:Invalid DMA direction: %d\n", __func__, kiusr.dma_direction);
		ret = -EINVAL;
		goto fail_detach;
	}

	if (kiusr.dma_direction == DMA_TO_DEVICE)
		kiumd_dma_direction = kiusr.dma_direction;
	else
		kiumd_dma_direction = DMA_BIDIRECTIONAL;

	if ((kiusr.dma_attr == DMA_ATTR_PRIVILEGED) && (kiusr.is_iova_zero != FIXED_IOVA_AT_ZERO)) {
		if (!(dmabufattach->priv)) {
			pr_err("%s:dmabufattach-priv is NULL\n", __func__);
			ret = -EINVAL;
			goto fail_detach;
		}

		dmaheapattachment = (struct kiumd_dma_heap_attachment *)dmabufattach->priv;
		if (!dmaheapattachment) {
			pr_err("%s:dmaheapattachment is NULL\n", __func__);
			ret = -EINVAL;
			goto fail_detach;
		}

		sgt = dmaheapattachment->table;
		if (!sgt) {
			pr_err("%s:sglist is NULL\n", __func__);
			ret = -EINVAL;
			goto fail_detach;
		}

		ret = dma_map_sgtable(vfio_dev->dev, sgt, kiumd_dma_direction, DMA_ATTR_PRIVILEGED | DMA_ATTR_SKIP_CPU_SYNC);
		if (ret) {
			pr_err("%s:dma_map_sgtable failed with err: %d, for device: %s\n", __func__, ret, vfio_dev->dev->kobj.name);
			goto fail_detach;
		}
	} else {

		if (IS_ERR_OR_NULL(dmabufattach->dmabuf)) {
			pr_err("%s:dmabuf is NULL\n", __func__);
			return -EINVAL;
		}

		sgt = dma_buf_map_attachment_unlocked(dmabufattach, kiumd_dma_direction);
		if (IS_ERR_OR_NULL(sgt)) {
			pr_err("%s: mapping failed with error: %ld, for device: %s\n", __func__, PTR_ERR(sgt), vfio_dev->dev->kobj.name);
			ret = (sgt == NULL ? -EINVAL : PTR_ERR(sgt));
			goto fail_detach;
		}

		if (kiusr.is_iova_zero == FIXED_IOVA_AT_ZERO) {
			if (kiusr.dma_attr == DMA_ATTR_PRIVILEGED)
				prot = IOMMU_CACHE | IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV;
			else
				prot = IOMMU_CACHE | IOMMU_READ | IOMMU_WRITE;

			iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
			if (!iommu_dom) {
				pr_err("%s:IOMMU domain is NULL\n", __func__);
				ret = -EINVAL;
				goto fail_fput;
			}

#ifdef CONFIG_DMABUF_DEBUG
			/*
			 * For debug builds there is a mangling done in the regular buffer map path,
			 * so iommu_map_sg is expecting a mangled physical address of the buffer
			 */

			kiumd_mangle_sg_table(sgt);
#endif

			/*
			 * For mapping at 0x0 we create a mapping using
			 * dma_buf_map_attachment_unlocked and then take the sg list
			 * and map it at iova - 0x0
			 */

			size = iommu_map_sg(iommu_dom, iova_zero, sgt->sgl, sgt->orig_nents, prot, GFP_ATOMIC);
			if (size < 0) {
				pr_err("%s:iommu_map_sg failed\n", __func__);
				dma_buf_unmap_attachment_unlocked(dmabufattach, sgt, kiumd_dma_direction);
				ret = -EFAULT;
				goto fail_detach;
			}
		}
	}

	if (!fp) {
		pr_err("%s:file ptr returns NULL\n", __func__);
		ret = -EINVAL;
		goto fail_fput;
	}
	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (!kiumd_ctx) {
		pr_err("%s:kiumd ctx is NULL \n", __func__);
		ret = -EINVAL;
		goto fail_fput;
	}

	smap = kzalloc(sizeof(struct smmu_map_data), GFP_KERNEL);
	if (!smap) {
		pr_err("%s:No memory for smap \n", __func__);
		ret = -ENOMEM;
		goto fail_fput;
	}
	smap->dmabufattach = (long)dmabufattach;
	smap->sgt_ptr = (long)sgt;
	smap->dmabuf_ptr = (long)kiumd_dmabuf;
	spin_lock(&kiumd_ctx->smmu_lock);
	smap->id = kiumd_ctx->id++;
	hash_add(kiumd_ctx->smmu_table, &smap->node, smap->id);
	spin_unlock(&kiumd_ctx->smmu_lock);

	kiusr.id = smap->id;
	if (kiusr.is_iova_zero == FIXED_IOVA_AT_ZERO)
		kiusr.dma_addr = (unsigned long) iova_zero;
	else
		kiusr.dma_addr = (unsigned long) sg_dma_address(sgt->sgl);

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed...\n", __func__);
		dma_buf_unmap_attachment_unlocked(dmabufattach, sgt, kiumd_dma_direction);
		ret = -EFAULT;
		goto fail_detach;
	}

	fput(file);
	return 0;

fail_detach:
	dma_buf_detach(kiumd_dmabuf, dmabufattach);

fail_put:
	dma_buf_put(kiumd_dmabuf);

fail_fput:
	fput(file);

	return ret;
}

struct iommu_domain *kiumd_get_iommu_domain(int vfio_fd)
{
	struct file *file;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct iommu_domain *iommu_dom;

	file = fget(vfio_fd);
	if (!file) {
		pr_err("%s:fget returns NULL\n", __func__);
		return NULL;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:vfio dev returns NULL\n", __func__);
		fput(file);
		return NULL;
	}

	if (!(vfio_dev->dev)) {
		pr_err("%s:vfio device returns NULL\n", __func__);
		fput(file);
		return NULL;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:iommu_dom is NULL\n", __func__);
		fput(file);
		return NULL;
	}

	fput(file);
	return iommu_dom;
}

/**
 * kiumd_dmabuf_vfio_unmap(char __user *arg, struct file *fp)
 *
 * This function facilitates the unmap the buffer mapped to SMMU backed device
 * also decrements the dma_buf kref count.
 *
 * return errno or 0 in case of success
 */

int kiumd_dmabuf_vfio_unmap(char __user *arg, struct file *fp)
{

	struct kiumd_user kiusr;
	struct dma_buf_attachment *dmabufattach = NULL;
	struct dma_buf *kiumd_dmabuf = NULL;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct iommu_domain *iommu_dom;
	struct file *file;
	int kiumd_dma_direction, ret = 0;
	struct sg_table *sgtable = NULL;
	struct kiumd_dma_heap_attachment *dmaheapattachment = NULL;
	dma_addr_t iova_zero = 0;
	u64 size;
	struct smmu_map_data *smap;
	bool found = false;
	struct kiumd_ctx *kiumd_ctx = NULL;

	if (!fp) {
		pr_err("%s:file ptr returns NULL\n", __func__);
		return -EINVAL;
	}
	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (!kiumd_ctx) {
		pr_err("%s:kiumd ctx is NULL\n", __func__);
		return -EINVAL;
	}

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

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
	kiumd_dmabuf = (struct dma_buf *)smap->dmabuf_ptr;
	if (!kiumd_dmabuf) {
		pr_err("%s:kiumd_dmabuf is NULL\n", __func__);
		return -EINVAL;
	}

	dmabufattach = (struct dma_buf_attachment *)smap->dmabufattach;
	if (!dmabufattach) {
		pr_err("%s:dmabufattach is NULL\n", __func__);
		return -EINVAL;
	}

	if (kiusr.ptselect == KGSL_GLOBAL_PT || kiusr.ptselect == KGSL_PER_PROCESS_PT)
		clear_map_iova(kiusr.dma_addr, kiumd_dmabuf->size, kiusr.ptselect);

	if (kiusr.dma_direction == 1)
		kiumd_dma_direction = kiusr.dma_direction;
	else
		kiumd_dma_direction = 0;

	if ((kiusr.dma_attr == DMA_ATTR_PRIVILEGED) && (kiusr.is_iova_zero != FIXED_IOVA_AT_ZERO)) {
		file = fget(kiusr.vfio_fd);
		if (!file) {
			pr_err("%s:fget returns NULL\n", __func__);
			return -EINVAL;
		}

		df = (struct vfio_device_file *)file->private_data;
		vfio_dev = (struct vfio_device *)df->device;
		if (!vfio_dev) {
			pr_err("%s:vfio dev returns NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}

		if (!(dmabufattach->priv)) {
			pr_err("%s:dmabufattach-priv is NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}

		dmaheapattachment = (struct kiumd_dma_heap_attachment *)dmabufattach->priv;
		if (!dmaheapattachment) {
			pr_err("%s:dmaheapattachment is NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}

		sgtable = dmaheapattachment->table;
		if (!sgtable) {
			pr_err("%s:sglist is NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}

		dma_unmap_sgtable(vfio_dev->dev, sgtable, kiumd_dma_direction, DMA_ATTR_PRIVILEGED);
		fput(file);
	} else {
		dma_buf_unmap_attachment_unlocked(dmabufattach, (struct sg_table *)smap->sgt_ptr,
									kiumd_dma_direction);

		if (kiusr.is_iova_zero == FIXED_IOVA_AT_ZERO) {
			iommu_dom = kiumd_get_iommu_domain(kiusr.vfio_fd);
			if (!iommu_dom) {
				pr_err("%s:iommu_dom is NULL\n", __func__);
				return -EINVAL;
			}

			size = iommu_unmap(iommu_dom, iova_zero, kiumd_dmabuf->size);
			if (size != kiumd_dmabuf->size) {
				pr_err("%s:iommu_unmap failed\n", __func__);
				return -EINVAL;
			}
		}
	}

	if (kiusr.ptselect == KGSL_GLOBAL_PT || kiusr.ptselect == KGSL_PER_PROCESS_PT
							|| kiusr.ptselect == KGSL_DEFAULT_PT) {
			iommu_dom = kiumd_get_iommu_domain(kiusr.vfio_fd);
			if (!iommu_dom) {
				pr_err("%s:iommu_dom is NULL\n", __func__);
				return -EINVAL;
			}

			iommu_flush_iotlb_all(iommu_dom);
	}


	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	kfree(smap);
	spin_unlock(&kiumd_ctx->smmu_lock);
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);

	return ret;
}

int kiumd_iova_ctrl(char __user *arg)
{
	struct kiumd_iova iovausr;
	struct vfio_device *vfio_dev;
	int cookie_type, ret;
	dma_addr_t iova_usr = 0;
	struct kiumd_iommu_dma_cookie *cookie;

	if (copy_from_user(&iovausr, arg, sizeof(struct kiumd_iova)))
		return -EFAULT;

	if (iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_CLEAR)
		cookie_type = 0;
	else
		cookie_type = 1;

	if (iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_SET) {
		cookie_type = 1;
		iova_usr = iovausr.iova;
	}

	vfio_dev = kiumd_get_vfio_device(iovausr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s failed to get vfio device\n", __func__);
		return -EINVAL;
	}

	cookie = kiumd_get_dma_cookie(vfio_dev);
	if (!cookie) {
		pr_err("%s failed to get cookie\n", __func__);
		return -EINVAL;
	}

	ret = kiumd_set_dma_cookie_unlocked(cookie, cookie_type, iova_usr);
	if (ret)
		pr_err("%s failed to set cookie\n", __func__);

	return ret;
}

int kiumd_fd_dmabuf_handler(char __user *arg)
{
	struct kiumd_user kiusr;
	struct dma_buf *kiumd_dmabuf = NULL;
	uint32_t local_id = 0;
	void *ret;
	int err;
	void *xa_entry;
	unsigned long dmabuf;
	unsigned long xa_index;
	bool handle_available = false;
	struct dma_buf_handle *dmabuf_handle = NULL;
	struct dma_buf_handle *dmabuf_xarray_entry = NULL;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user))) {
		pr_err("%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	/* FD to Handle */
	if (kiusr.handle == FD_TO_HANDLE) {

		if (kiusr.dma_buf_fd < 0) {
			pr_err("%s: dma_buf_fd is invalid\n", __func__);
			return -EBADF;
		}

		/* Retrieve struct dma_buf from FD*/
		dmabuf  = (unsigned long) dma_buf_get(kiusr.dma_buf_fd);
		if (((struct dma_buf *) dmabuf) == NULL) {
			pr_err("%s: dma_buf_get returns NULL\n", __func__);
			return -EINVAL;
		}

		/* Check if handle for buffer already exists, RCU lock acquired*/
		xa_for_each(&kiumd_xa, xa_index, xa_entry) {
			dmabuf_xarray_entry = (struct dma_buf_handle *) xa_entry;
			if (dmabuf_xarray_entry->dmabuf == dmabuf) {
				handle_available = true;
				local_id = xa_index;
				atomic_inc(&dmabuf_xarray_entry->handle_refcount);
				ret = xa_store(&kiumd_xa, xa_index, dmabuf_xarray_entry, GFP_KERNEL);
				if (xa_is_err(ret)) {
					pr_err("%s: xa_store failed\n", __func__);
					dma_buf_put((struct dma_buf *) dmabuf);
					return xa_err(ret);
				}
			}
		}

		/* If Handle does not exist, allocate xa_array entry*/
		if (!handle_available) {
			dmabuf_handle = kzalloc(sizeof(struct dma_buf_handle), GFP_KERNEL);
			if (!dmabuf_handle) {
				pr_err("%s: kzalloc failed.\n", __func__);
				return -ENOMEM;
			}

			dmabuf_handle->dmabuf = dmabuf;
			atomic_inc(&dmabuf_handle->handle_refcount);
			err = xa_alloc(&kiumd_xa, &local_id, dmabuf_handle, xa_limit_32b, GFP_KERNEL);
			if (err < 0) {
				pr_err("%s:xarray alloc failure %d\n", __func__, err);
				dma_buf_put((struct dma_buf *) dmabuf);
				return err;
			}
		}

		kiumd_dmabuf = (struct dma_buf *) dmabuf;
		kiusr.handle = local_id;
	} else if (kiusr.dma_buf_fd == HANDLE_TO_FD) { /* Handle to FD */

		if (kiusr.handle < 0) {
			pr_err("%s: dmabuf handle is invalid\n", __func__);
			return -EINVAL;
		}

		local_id = kiusr.handle;
		dmabuf_handle = xa_load(&kiumd_xa, local_id);
		if (!dmabuf_handle) {
			pr_err("%s: dmabuf_handle is NULL\n", __func__);
			return -EINVAL;
		}

		if (!IS_ERR_OR_NULL(dmabuf_handle->dmabuf)) {
			kiusr.dma_buf_fd = dma_buf_fd((struct dma_buf *) dmabuf_handle->dmabuf, (O_CLOEXEC));
		}
		if (kiusr.dma_buf_fd < 0) {
			pr_err("%s:dma_buf_fd failed\n", __func__);
			return -EBADF;
		}

		get_dma_buf((struct dma_buf *) dmabuf_handle->dmabuf);

	} else if (kiusr.dma_buf_fd == CLOSE_HANDLE) {  /* Close Handle */

		if (kiusr.handle < 0) {
			pr_err("%s: Invalid dma buf handle.\n", __func__);
			return -EINVAL;
		}

		local_id = (int32_t)kiusr.handle;
		dmabuf_handle = xa_load(&kiumd_xa, local_id);
		if (!dmabuf_handle) {
			pr_err("%s:Entry not available in xarray\n", __func__);
			return -EINVAL;
		}

		kiumd_dmabuf = ((struct dma_buf *)dmabuf_handle->dmabuf);
		if (atomic_dec_and_test(&dmabuf_handle->handle_refcount)) {
			xa_erase(&kiumd_xa, local_id);
			kfree(dmabuf_handle);
		}

		else {
			ret = xa_store(&kiumd_xa, local_id, dmabuf_handle, GFP_KERNEL);
			if (xa_is_err(ret)) {
				pr_err("%s: xa_store failed in close handle\n", __func__);
				return xa_err(ret);
			}
		}

		if (!IS_ERR_OR_NULL(kiumd_dmabuf))
			dma_buf_put(kiumd_dmabuf);
		kiusr.dma_buf_fd = 0;
	}
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed...\n", __func__);
		return -EFAULT;
	}

	return 0;
}

static int kiumd_io_pgtable_hyp_assign_page(u32 *vmid, u64 page, u32 nr_acl_entries)
{
	int ret;
	int i = 0;
	u64 src_vmid_list = BIT(QCOM_SCM_VMID_HLOS);
	struct qcom_scm_vmperm *dst_vmids;

	dst_vmids = kcalloc((nr_acl_entries + 1), sizeof(struct qcom_scm_vmperm), GFP_KERNEL);
	if (!dst_vmids)
		return -ENOMEM;

	dst_vmids[i].vmid = QCOM_SCM_VMID_HLOS;
	dst_vmids[i].perm = QCOM_SCM_PERM_RW;
	pr_debug("Hyp assign page for dst:%d vmid:%d perm:%d total vmids:%d\n",
		 i, dst_vmids[i].vmid, dst_vmids[i].perm, nr_acl_entries + 1);
	i++;

	for (; i < nr_acl_entries + 1; i++) {
		dst_vmids[i].vmid = vmid[i - 1];
		dst_vmids[i].perm = QCOM_SCM_PERM_READ;
		pr_debug("Hyp assign page for dst:%d vmid:%d perm:%d\n",
			 i, dst_vmids[i].vmid, dst_vmids[i].perm);
	}

	ret = qcom_scm_assign_mem(page, PAGE_SIZE, &src_vmid_list, dst_vmids, nr_acl_entries + 1);
	if (ret)
		pr_err("hyp assign for %llu address of size %lx rc:%d\n",
		       page, PAGE_SIZE, ret);
	kfree(dst_vmids);
	return ret;
}

static int kiumd_io_pgtable_hyp_unassign_page(u32 *vmid, u64 page, u32 nr_acl_entries)
{
	int ret;
	u64 src_vmid_list = BIT(QCOM_SCM_VMID_HLOS);

	struct qcom_scm_vmperm dst_vmids[] = { {QCOM_SCM_VMID_HLOS,
						QCOM_SCM_PERM_RWX } };
	for (int i = 0; i < nr_acl_entries ; i++) {
		src_vmid_list |= BIT(vmid[i]);
		pr_debug("Hyp unassign page for dst:%d vmid:%d\n",
			 i, vmid[i]);
	}

	ret = qcom_scm_assign_mem(page, PAGE_SIZE, &src_vmid_list,
				  dst_vmids, ARRAY_SIZE(dst_vmids));
	if (ret)
		pr_err("hyp unassign failed %llu address of size %lx rc:%d\n",
		       page, PAGE_SIZE, ret);
	return ret;
}

static int kiumd_hyp_unassign_sg(struct sg_table *sgt, int *source_vm_list,
				 int source_nelems, bool clear_page_private)
{
	u64 src_vmid_list = 0;
	struct qcom_scm_vmperm dst_vmids[] = { {QCOM_SCM_VMID_HLOS,
						QCOM_SCM_PERM_RWX } };
	struct scatterlist *sg;
	int ret, i;

	if (source_nelems <= 0)
		return -EINVAL;

	if (!sgt)
		return -EINVAL;

	if (!sgt->sgl)
		return -EINVAL;

	sg = sgt->sgl;

	for (int j = 0; j < source_nelems ; j++) {
		src_vmid_list |= BIT(source_vm_list[j]);
		pr_debug("Hyp unassign sg for dst:%d vmid:%d\n",
			 j, source_vm_list[j]);
	}

	do {
		pr_debug("%s: memory ownership transfer start\n", __func__);
		ret = qcom_scm_assign_mem(page_to_phys(sg_page(sg)), sg->length, &src_vmid_list,
					  dst_vmids, ARRAY_SIZE(dst_vmids));
		if (ret) {
			pr_err("Hyp unassign failed %llu address of size %x rc:%d\n",
			       page_to_phys(sg_page(sg)), sg->length, ret);
			goto out;
		}
		pr_debug("%s: memory ownership transfer end:%d\n", __func__, ret);
		sg = sg_next(sg);
	} while (sg);

	if (clear_page_private)
		for_each_sg(sgt->sgl, sg, sgt->nents, i)
			ClearPagePrivate(sg_page(sg));
out:
	return ret;
}

static int kiumd_hyp_assign_sg(struct sg_table *sgt, int *dest_vm_list,
			       int dest_nelems, bool set_page_private, int *dest_perms)
{
	u64 src_vmid_list = BIT(QCOM_SCM_VMID_HLOS);
	int ret;
	struct qcom_scm_vmperm *dst_vmids;
	struct scatterlist *sg;

	if (dest_nelems <= 0) {
		pr_err("%s: dest_nelems invalid\n", __func__);
		return -EINVAL;
	}

	if (!sgt)
		return -EINVAL;

	sg = sgt->sgl;
	if (!sg)
		return -EINVAL;

	dst_vmids = kcalloc(dest_nelems, sizeof(struct qcom_scm_vmperm), GFP_KERNEL);
	if (!dst_vmids)
		return -ENOMEM;

	for (int i = 0; i < dest_nelems; i++) {
		dst_vmids[i].vmid = dest_vm_list[i];
		dst_vmids[i].perm = dest_perms[i];
		pr_debug("Hyp assign sg for dst:%d vmid:%d perm:%d\n",
			 i, dst_vmids[i].vmid, dst_vmids[i].perm);
	}

	do {
		pr_debug("Assign call initiated\n");
		ret = qcom_scm_assign_mem(page_to_phys(sg_page(sg)), sg->length, &src_vmid_list,
					  dst_vmids, dest_nelems);
		if (ret) {
			pr_err("failed qcom_assign for assigning %llu address of size %x rc:%d\n",
			       page_to_phys(sg_page(sg)), sg->length, ret);
			goto err;
		}
		pr_debug("Assign call success:%d\n", ret);
		sg = sg_next(sg);
	} while (sg);
	pr_debug("%s success\n", __func__);
err:
	kfree(dst_vmids);
	return ret;
}

int kiumd_acl_to_vmid_perms_list(unsigned int nr_acl_entries, const void __user *acl_entries,
				 int **dst_vmids, int **dst_perms)
{
	int ret, i, *vmids, *perms;
	struct kiumd_acl_entry entry;

	if (!nr_acl_entries || !acl_entries) {
		pr_err("%s:%d Invalid params entries:%d\n", __func__, __LINE__, nr_acl_entries);
		return -EINVAL;
	}

	if (nr_acl_entries > MAX_KIUMD_ACL_ENTRIES) {
		pr_err("%s:%d Invalid params\n", __func__, __LINE__);
		return -EINVAL;
	}

	vmids = kmalloc_array(nr_acl_entries, sizeof(*vmids), GFP_KERNEL);
	if (!vmids)
		return -ENOMEM;

	perms = kmalloc_array(nr_acl_entries, sizeof(*perms), GFP_KERNEL);
	if (!perms) {
		kfree(vmids);
		return -ENOMEM;
	}

	for (i = 0; i < nr_acl_entries; i++) {
		ret = copy_struct_from_user(&entry, sizeof(entry),
					    acl_entries + (sizeof(entry) * i),
					    sizeof(entry));
		if (ret < 0) {
			pr_err("%s:%d Invalid params\n", __func__, __LINE__);
			goto out;
		}

		vmids[i] = entry.vmid;
		perms[i] = entry.perms;
		pr_debug("%d vmid:%d perms:%d\n", i, vmids[i], perms[i]);
		if (vmids[i] < 0 || perms[i] < 0 ||
		    vmids[i] > KIUMD_MAX_VMID ||
		    perms[i] > KIUMD_MAX_PERMS) {
			ret = -EINVAL;
			goto out;
		}
	}

	*dst_vmids = vmids;
	*dst_perms = perms;
	return ret;

out:
	kfree(perms);
	kfree(vmids);
	return ret;
}

int kiumd_get_pgd(struct vfio_device *vfio_dev, u64 *pgd)
{
	struct iommu_domain *iommu_dom;
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable *pgtable;

	if (!pgd) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		return -EINVAL;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:%d Failed to get IOMMU DOMAIN VFIO\n", __func__, __LINE__);
		return -EINVAL;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_dom || !smmu_dom->pgtbl_ops) {
		pr_err("%s:%d failed to get smmu_dom\n", __func__, __LINE__);
		return -EINVAL;
	}

	pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if (!pgtable) {
		pr_err("%s:%d failed to get pgtabl ops\n", __func__, __LINE__);
		return -EINVAL;
	}

	*pgd = pgtable->cfg.arm_lpae_s1_cfg.ttbr;

	return 0;
}

int kiumd_dmabuf_vfio_secure_map(char __user *arg, struct file *fp)
{
	struct kiumd_user kiusr;
	struct vfio_device *vfio_dev = NULL;
	struct vfio_device_file *df;
	struct file *file = NULL;
	struct dma_buf *kiumd_dmabuf = NULL;
	struct dma_buf_attachment *dmabufattach = NULL;
	struct sg_table *sgt = NULL;
	int kiumd_dma_direction;
	u64 pgd = 0;
	int ret = 0;
	int *vmids, *perms;
	struct kiumd_ctx *kiumd_ctx = NULL;
	struct smmu_map_data *smap = NULL;

	pr_debug(" %s: Entering.....\n", __func__);
	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user))) {
		pr_err("%s:%d bad params from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	if (kiusr.vfio_fd < 0) {
		pr_err("%s:%d invalid fd from user\n", __func__, __LINE__);
		return -EBADF;
	}

	file = fget(kiusr.vfio_fd);
	if (!file) {
		pr_err("%s:%d failed to get file from vfio fd\n", __func__, __LINE__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:%d vfio_dev is NULL\n", __func__, __LINE__);
		ret = -EINVAL;
		goto close_file;
	}

	if (!vfio_dev->dev) {
		ret = -EINVAL;
		pr_err("%s:%d invalid device\n", __func__, __LINE__);
		goto close_file;
	}

	ret = kiumd_get_pgd(vfio_dev, &pgd);
	if (ret) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		goto close_file;
	}

	pr_debug("Kiumd VM page table ttbr:%llx\n", pgd);

	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		ret = !kiumd_dmabuf ? -EINVAL : PTR_ERR(kiumd_dmabuf);
		goto close_file;
	}

	ret = kiumd_acl_to_vmid_perms_list(kiusr.mem_parcel.nr_acl_entries,
					   (void *)kiusr.mem_parcel.acl_list, &vmids, &perms);
	if (ret) {
		pr_err("%s:%d Invalid params\n", __func__, __LINE__);
		goto close_file;
	}

	dmabufattach = dma_buf_attach(kiumd_dmabuf, vfio_dev->dev);
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

	/* Grant Page table read access to peripheral VM*/
	ret = kiumd_io_pgtable_hyp_assign_page(vmids, pgd, kiusr.mem_parcel.nr_acl_entries);
	if (ret < 0) {
		pr_err("%s:%d ownership transfer error:%d\n", __func__, __LINE__, ret);
		goto detach;
	}
	pr_debug("Pgtable ownership transfer success\n");

	ret = kiumd_hyp_assign_sg(sgt, vmids, kiusr.mem_parcel.nr_acl_entries, true, perms);
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

	if (!fp) {
		pr_err("%s:file ptr returns NULL\n", __func__);
		ret = -EINVAL;
		goto close_file;
	}
	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (!kiumd_ctx) {
		pr_err("%s:kiumd ctx is NULL \n", __func__);
		ret = -EINVAL;
		goto close_file;
	}

	smap = kzalloc(sizeof(struct smmu_map_data), GFP_KERNEL);
	if (!smap) {
		pr_err("%s:No memory for smap \n", __func__);
		ret = -ENOMEM;
		goto close_file;
	}
	smap->dmabufattach = (long)dmabufattach;
	smap->sgt_ptr = (long)sgt;
	smap->dmabuf_ptr = (long)kiumd_dmabuf;
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
	fput(file);
	pr_debug("returning from ioctl ret:%d\n", ret);
	return ret;
unmap:
	dma_buf_unmap_attachment_unlocked(dmabufattach, (struct sg_table *)sgt,
				 DMA_BIDIRECTIONAL);
hyp_unassign_sg:
	kiumd_hyp_unassign_sg((struct sg_table *)sgt, vmids,
			      kiusr.mem_parcel.nr_acl_entries, true);
hyp_unassign_table:
	kiumd_io_pgtable_hyp_unassign_page(vmids, pgd, kiusr.mem_parcel.nr_acl_entries);
detach:
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);
free_mem:
	kfree(vmids);
	kfree(perms);
close_file:
	fput(file);
	return ret;
}

int kiumd_dmabuf_vfio_secure_unmap(char __user *arg, struct file *fp)
{
	struct vfio_device *vfio_dev = NULL;
	struct vfio_device_file *df;
	struct file *file = NULL;
	u64 pgd;
	int ret = 0;
	struct kiumd_user kiusr;
	struct dma_buf_attachment *dmabufattach = NULL;
	struct dma_buf *kiumd_dmabuf = NULL;
	int *vmids, *perms;
	struct smmu_map_data *smap;
	bool found = false;
	struct kiumd_ctx *kiumd_ctx = NULL;

	if (!fp) {
		pr_err("%s:file ptr returns NULL\n", __func__);
		return -EINVAL;
	}
	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (!kiumd_ctx) {
		pr_err("%s:kiumd ctx is NULL\n", __func__);
		return -EINVAL;
	}

	pr_debug("%s entering\n", __func__);

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}
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
	if (kiusr.vfio_fd < 0) {
		pr_err("%s:%d invalid vfio fd\n", __func__, __LINE__);
		return -EBADF;
	}

	file = fget(kiusr.vfio_fd);
	if (!file) {
		pr_err("%s:%d failed to get file from vfio fd\n", __func__, __LINE__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:%d vfio_dev is NULL\n", __func__, __LINE__);
		ret = -EINVAL;
		goto close_file;
	}

	if (!vfio_dev->dev) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		ret = -EINVAL;
		goto close_file;
	}

	ret = kiumd_get_pgd(vfio_dev, &pgd);
	if (ret) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		goto close_file;
	}

	/*Input param error checking for ACL happens in kiumd_acl_to_vmid_perms*/
	ret = kiumd_acl_to_vmid_perms_list(kiusr.mem_parcel.nr_acl_entries,
					   (void *)kiusr.mem_parcel.acl_list, &vmids, &perms);
	if (ret) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		goto close_file;
	}

	dmabufattach = (struct dma_buf_attachment *)smap->dmabufattach;
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

	dma_buf_unmap_attachment_unlocked(dmabufattach, (struct sg_table *)smap->sgt_ptr,
				 DMA_BIDIRECTIONAL);

	ret = kiumd_hyp_unassign_sg((struct sg_table *)smap->sgt_ptr, vmids,
				    kiusr.mem_parcel.nr_acl_entries, true);
	if (ret < 0) {
		pr_err("%s:%d memory ownership transfer error:%d\n", __func__, __LINE__, ret);
		goto free_mem;
	}

	ret = kiumd_io_pgtable_hyp_unassign_page(vmids, pgd, kiusr.mem_parcel.nr_acl_entries);
	if (ret < 0) {
		pr_err("%s:%d memory ownership transfer error:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto free_mem;
	}

	pr_debug("memory ownership transfer success\n");

	kiumd_dmabuf = (struct dma_buf *)smap->dmabuf_ptr;
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
close_file:
	fput(file);

	return ret;
}

static int kiumd_mmio_smmu_map(char __user *arg, struct file *fp)
{
	struct kiumd_smmu_mmio_map kiusr;
	int ret;
	struct vfio_device *vfio_dev;
	struct kiumd_iommu_dma_cookie *cookie;
	char *reg_name;
	u64 addr, size;
	dma_addr_t dma_addr;
	struct kiumd_ctx *kiumd_ctx = NULL;
	struct smmu_map_data *smap = NULL;
	struct kiumd_smmu_mmio_ctx *mmio_ctx;
	struct resource *res;
	int iter, retval = 0;

	if (!fp) {
		pr_err("%s:file ptr returns NULL\n", __func__);
		return -EINVAL;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (!kiumd_ctx) {
		pr_err("%s:kiumd ctx is NULL\n", __func__);
		return -EINVAL;
	}

	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	vfio_dev = kiumd_get_vfio_device(kiusr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:%d invalid vfio device fd\n", __func__, __LINE__);
		return -EINVAL;
	}

	cookie = kiumd_get_dma_cookie(vfio_dev);
	if (!cookie) {
		pr_err("%s:cookie not found\n", __func__);
		return -EINVAL;
	}
	spin_lock(&kiumd_ctx->smmu_lock);
	if (!hash_empty(kiumd_ctx->smmu_table)) {
		hash_for_each(kiumd_ctx->smmu_table, iter, smap, node) {
			if (!smap->context)
				continue;
			pr_debug("mmio ctx%p iova:%llx size:%lx\n", smap->context,
				 ((struct kiumd_smmu_mmio_ctx *)smap->context)->iova,
				 ((struct kiumd_smmu_mmio_ctx *)smap->context)->size);
			if (kiusr.fixed_iova &&
			    ((struct kiumd_smmu_mmio_ctx *)smap->context)->iova == kiusr.iova) {
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

	if (!dev_is_platform(vfio_dev->dev)) {
		pr_err("%s:%d not platform device\n", __func__, __LINE__);
		goto reg_free;
	}

	res = platform_get_resource_byname(to_platform_device(vfio_dev->dev), IORESOURCE_MEM,
					   reg_name);
	if (!res) {
		ret = -EINVAL;
		pr_err("%s:%d resource error\n", __func__, __LINE__);
		goto reg_free;
	}
	addr = res->start;
	size = resource_size(res);

	if (kiusr.fixed_iova) {
		mutex_lock(&cookie->mutex);
		ret = kiumd_set_dma_cookie(cookie, IOMMU_DMA_MSI_COOKIE, kiusr.iova);
		if (ret) {
			mutex_unlock(&cookie->mutex);
			goto reg_free;
		}
	}

	dma_addr = dma_map_resource(vfio_dev->dev, addr, size, 0, 0);
	ret = dma_mapping_error(vfio_dev->dev, dma_addr);
	if (kiusr.fixed_iova) {
		retval = kiumd_set_dma_cookie(cookie, IOMMU_DMA_IOVA_COOKIE, 0);
		mutex_unlock(&cookie->mutex);
	}

	if (ret || retval) {
		pr_err("%s:Failed to map with error: %d\n", __func__, ret);
		goto reg_free;
	}

	kiusr.iova = dma_addr;

	spin_lock(&kiumd_ctx->smmu_lock);
	smap->id = kiumd_ctx->id++;
	kiusr.id = smap->id;
	hash_add(kiumd_ctx->smmu_table, &smap->node, smap->id);
	mmio_ctx->iova = dma_addr;
	mmio_ctx->size = size;
	mmio_ctx->dev = vfio_dev->dev;
	smap->context = mmio_ctx;
	spin_unlock(&kiumd_ctx->smmu_lock);

	pr_debug("%s:%s mapped pa:%llx size:%llx user iova:%llx dma_addr:%llx id:%d\n",
		 __func__, reg_name, addr, size, kiusr.iova, dma_addr, kiusr.id);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("kiumd:error in copying data:%d\n", ret);
		ret = -EFAULT;
		goto smap_del;
	}

	kfree(reg_name);
	return ret;
smap_del:
	dma_unmap_resource(vfio_dev->dev, dma_addr, size, 0, 0);
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

static int kiumd_mmio_smmu_unmap(char __user *arg, struct file *fp)
{
	struct kiumd_smmu_mmio_map kiusr;
	int ret = 0;
	struct kiumd_smmu_mmio_ctx *mmio_ctx;
	struct kiumd_ctx *kiumd_ctx = NULL;
	struct smmu_map_data *smap = NULL;
	bool found = false;

	if (!fp) {
		pr_err("%s:file ptr returns NULL\n", __func__);
		return -EINVAL;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	if (!kiumd_ctx) {
		pr_err("%s:kiumd ctx is NULL\n", __func__);
		return -EINVAL;
	}

	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	if (kiusr.id < 0) {
		pr_err("%s:id passed from user should be positive value\n", __func__);
		return -EFAULT;
	}

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

	mmio_ctx = (struct kiumd_smmu_mmio_ctx *)smap->context;
	if (!mmio_ctx) {
		pr_err("%s:invalid context:%d\n", __func__, __LINE__);
		return -EINVAL;
	}

	pr_debug("%s:mapping found:%d mmio_ctx:%p iova:%llx size:%lx\n",
		 __func__, kiusr.id, mmio_ctx, mmio_ctx->iova, mmio_ctx->size);

	dma_unmap_resource(mmio_ctx->dev, mmio_ctx->iova, mmio_ctx->size, 0, 0);

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	kfree(smap->context);
	kfree(smap);
	spin_unlock(&kiumd_ctx->smmu_lock);

	return ret;
}

static int kiumd_open(struct inode *inode, struct file *filp)
{
	struct kiumd_ctx *kictx = NULL;

	kictx = kzalloc(sizeof(struct kiumd_ctx), GFP_KERNEL);
	if (!kictx)
		return -ENOMEM;
	kictx->id = 0;
	hash_init(kictx->smmu_table);
	spin_lock_init(&kictx->smmu_lock);
	filp->private_data = kictx;

	return 0;
}

static int kiumd_close(struct inode *inode, struct file *filp)
{
	struct kiumd_ctx *ki_ctx = (struct kiumd_ctx *)filp->private_data;
	struct smmu_map_data *smap;
	int iter;

	if (!ki_ctx) {
		pr_err("%s:kiumd ctx is NULL\n", __func__);
		return -EINVAL;
	}
	spin_lock(&ki_ctx->smmu_lock);
	if (!hash_empty(ki_ctx->smmu_table)) {
		hash_for_each(ki_ctx->smmu_table, iter, smap, node) {
			if (smap->context) {
				struct kiumd_smmu_mmio_ctx *mmio_ctx = smap->context;

				pr_debug("Free mmio ctx%p iova:%llx size:%lx\n",
					 mmio_ctx, mmio_ctx->iova, mmio_ctx->size);
				dma_unmap_resource(mmio_ctx->dev, mmio_ctx->iova, mmio_ctx->size, 0, 0);
				hash_del(&smap->node);
				kfree(smap->context);
				kfree(smap);
			}
		}
	}
	spin_unlock(&ki_ctx->smmu_lock);
	kfree(ki_ctx);

	return 0;
}

static long kiumd_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	char __user *argp = (char __user *)arg;
	int err;

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
	case KIUMD_SET_USER_CONTEXT:
		err = kiumd_perprocess_set_user_context(argp);
		break;
	case KIUMD_PER_PROCESS_ALLOC:
		err = kiumd_perprocess_pt_alloc(argp);
		break;
	case KIUMD_PER_PROCESS_SET:
		err = kiumd_perprocess_pgtble_set(argp);
		break;
	case KIUMD_PER_PROCESS_FREE:
		err = kiumd_perprocess_pgtble_free(argp);
		break;
	case KIUMD_FD_DMABUF_HANDLE:
		err = kiumd_fd_dmabuf_handler(argp);
		break;
	case KIUMD_CUSTOM_IOVA_INIT:
		err = kiumd_dmabuf_custom_iova_init(argp);
		break;
	case KIUMD_GLOBAL_PT_SET:
		err = kiumd_global_pgtble_set(argp);
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
	default:
		err = -ENOTTY;
		break;
	}

	return err;
}

static const struct file_operations kiumd_fops = {
	.open = kiumd_open,
	.unlocked_ioctl = kiumd_ioctl,
	.compat_ioctl = kiumd_ioctl,
	.release = kiumd_close,
};

static int kiumd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int err;
	const char *name, *devname;
	struct miscdevice *miscdev;

	miscdev = kzalloc(sizeof(struct miscdevice), GFP_KERNEL);
	if (!miscdev)
		return -ENOMEM;

	if (!of_property_read_string(np, "qcom,dev-name", &name))
		devname = devm_kstrdup(dev, name, GFP_KERNEL);
	else
		devname = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);

	miscdev->minor = MISC_DYNAMIC_MINOR;
	miscdev->name = devname;
	miscdev->fops = &kiumd_fops;
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

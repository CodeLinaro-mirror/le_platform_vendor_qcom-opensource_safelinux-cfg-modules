// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/adreno-smmu-priv.h>
#include <linux/kiumd_common.h>
#include <linux/pm_runtime.h>

#define CREATE_TRACE_POINTS
#define TRACE_SAFELINUX_COMMON

#include "safelinux_modules_trace.h"

DECLARE_BITMAP(global_map, KGSL_PT_MEM_PAGES);
static DEFINE_SPINLOCK(global_map_lock);

#ifdef CONFIG_DMABUF_DEBUG
/*
 * For debug builds there is a mangling done in the regular buffer
 * map path, so iommu_map_sg is expecting a mangled physical address of
 * the buffer
 */

/**
 * @Brief: This function facilitates to
 * modify the page links for each of
 * scatter gather table
 *
 * Parameters:
 * @sg_table: pointer for scatter gather table
 *
 * Returns void
 */
static void kiumd_mangle_sg_table(struct sg_table *sg_table)
{
	int i;
	struct scatterlist *sg;

	for_each_sgtable_sg(sg_table, sg, i) {
		if (sg)
			sg->page_link ^= ~0xffUL;
	}
}
#else /* CONFIG_DMABUF_DEBUG */
static void kiumd_mangle_sg_table(struct sg_table *sg_table)
{
}
#endif /* CONFIG_DMABUF_DEBUG */

/**
 * kiumd_iommu_get_dma_domain - This function find the iommu group for given
 * vfio device and then return default iommu domain for that group
 *
 * Parameters:
 * @dev: device
 *
 * Return: struct iommu_domain * upon successand NULL on failure
 */
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

/**
 * kiumd_get_dma_cookie - Provide the dma cookie for given vfio device
 *
 * Parameters:
 * @dev: device
 *
 * Return:  struct kiumd_iommu_dma_cookie *(cookie) upon success and NULL on failure
 */
struct kiumd_iommu_dma_cookie *kiumd_get_dma_cookie(struct device *dev)
{
	struct kiumd_iommu_dma_cookie *cookie;
	struct iommu_domain *domain;

	domain = kiumd_iommu_get_dma_domain(dev);
	if (!domain) {
		pr_err("%s:iommu domain is NULL for %s\n", __func__, dev_name(dev));
		return NULL;
	}

	cookie = (struct kiumd_iommu_dma_cookie *)domain->iova_cookie;
	if (!cookie) {
		pr_err("%s:cookie not found\n", __func__);
		return NULL;
	}

	return cookie;
}

/**
 * kiumd_set_dma_cookie - Set dma cookie type and iova to given cookie type and iova
 *
 * Parameters:
 * @*cookie: iommu dma cookie
 * @type: iommu_dma_cookie_type
 * @iova: iova address
 *
 * Return:  0 upon success and -EINVAL on failure
 */
int kiumd_set_dma_cookie(struct kiumd_iommu_dma_cookie *cookie,
			 enum iommu_dma_cookie_type type,
			 dma_addr_t iova)
{
	if (!cookie) {
		pr_err("%s:Unable to set cookie\n", __func__);
		return -EINVAL;
	}

	cookie->type = type;
	cookie->msi_iova = iova;

	return 0;
}

/**
 * kiumd_set_dma_cookie_unlocked - Calls the api to set dma cookie.
 * Cookie mutex should not be held while calling the function.
 *
 * Parameters:
 * @*cookie: iommu dma cookie
 * @type: iommu_dma_cookie_type
 * @iova: iova address
 *
 * Return: 0 upon success and -EINVAL on failure
 */
int kiumd_set_dma_cookie_unlocked(struct kiumd_iommu_dma_cookie *cookie,
				  enum iommu_dma_cookie_type type,
				  dma_addr_t iova)
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

/**
 * kiumd_get_smmu_domain - This function facilitates to set the iommu domain
 *
 * Parameters:
 * @dev: device
 *
 * Return: iommu_dom upon success and NULL
 * on failure
 */
struct arm_smmu_domain *kiumd_get_smmu_domain(struct device *dev)
{
	struct arm_smmu_domain *smmu_domain;
	struct iommu_domain *iommu_dom;

	iommu_dom = kiumd_iommu_get_dma_domain(dev);
	if (!iommu_dom) {
		pr_err("%s:IOMMU domain is NULL\n", __func__);
		return NULL;
	}
	smmu_domain = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_domain) {
		pr_err("%s:SMMU domain is NULL\n", __func__);
		return NULL;
	}

	return smmu_domain;
}

int kiumd_iommu_custom_iova_init(struct device *dev)
{
	struct kiumd_iommu_dma_cookie *cookie;
	struct iommu_resv_region *region;
	struct iommu_domain *domain;
	struct iova_domain *iovad;
	unsigned long lo, hi;
	LIST_HEAD(resrvd);

	//Print a warning and continue.
	if (dma_set_max_seg_size(dev, (unsigned int) DMA_BIT_MASK(32)))
		pr_err("%s:WARNING: max_segment size not set.\n", __func__);

	domain = kiumd_iommu_get_dma_domain(dev);
	if (!domain) {
		pr_err("%s:dma_domain is invalid.\n", __func__);
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

	return 0;
}

/**
 * kiumd_get_pgtable_entry - Searche for a pagetable entry in
 * the given context's hash table based on the provided index
 *
 * Parameters:
 * @kiumd_ctx: Pointer to the kiumd context
 * @idx: Index of the pagetable entry to retrieve
 * @is_process: Flag indicating if the context is process-specific
 *
 * Return: Pointer to the found pagetable entry, or NULL if not found.
 */
struct pgtable_map *kiumd_get_pgtable_entry(struct kiumd_ctx *kiumd_ctx,
					    unsigned long idx)
{
	struct kiumd_kgsl_context *kgsl_context;
	struct pgtable_map *pgtble_ctx;
	bool found = false;

	kgsl_context = kiumd_ctx->kgsl_context;
	spin_lock(&kgsl_context->kgsl_hash_lock);
	hash_for_each_possible(kiumd_ctx->kgsl_page_table, pgtble_ctx, node, idx) {
		if (pgtble_ctx->idx == idx) {
			found = true;
			break;
		}
	}

	spin_unlock(&kgsl_context->kgsl_hash_lock);
	if (!found) {
		pr_err("%s:%d wrong id:%lu passed by client to get enrty in hash map\n",
			__func__, __LINE__, idx);
		return NULL;
	}

	return pgtble_ctx;
}

bool check_ptselect(struct kiumd_user *kiusr)
{
	return ((kiusr->ptselect == KGSL_GLOBAL_PT)
		|| (kiusr->ptselect == KGSL_PER_PROCESS_PT)
		|| (kiusr->ptselect == KGSL_DEFAULT_PT));
}

static void free_iommu_addr_entry(struct kmem_cache *addr_cache, struct iommu_addr_entry *entry)
{
	kmem_cache_free(addr_cache, entry);
}

void free_iova_range(struct kmem_cache *addr_cache,
		     struct pgtable_map *map, unsigned long iova)
{
	struct rb_node *node = map->rbtree.rb_node;
	struct iommu_addr_entry *entry;

	guard(mutex)(&map->pgctx_lock);
	while (node) {
		entry = rb_entry(node, struct iommu_addr_entry, rbnode);

		if (iova < entry->base_addr)
			node = node->rb_left;
		else if (iova > entry->base_addr)
			node = node->rb_right;
		else {
			rb_erase(&entry->rbnode, &map->rbtree);
			free_iommu_addr_entry(addr_cache, entry);
			return;
		}
	}

}

/**
 * free_allocated_iova - Free an allocated IOVA range from a pagetable entry
 * based on index
 *
 * Parameters:
 * @kiumd_ctx: Pointer to the kiumd context
 * @iova: IOVA address to free
 * @is_process: Flag indicating if the context is process-specific
 *
 * Return: void
 */
void free_allocated_iova(struct kmem_cache *addr_cache,
			struct kiumd_ctx *kiumd_ctx, unsigned long iova)
{
	struct pgtable_map *pgtble_ctx;

	pgtble_ctx = kiumd_ctx->pgtable_ctx;
	free_iova_range(addr_cache, pgtble_ctx, iova);
}

struct iommu_addr_entry *alloc_iommu_addr_entry(struct kmem_cache *addr_cache,
						unsigned long base_addr,
						unsigned long size)
{
	struct iommu_addr_entry *entry = kmem_cache_alloc(addr_cache, GFP_KERNEL);

	if (!entry) {
		pr_err("%s:%d failed to create entry for addr: %lx, size: %lu)\n",
		       __func__, __LINE__, base_addr, size);
		return NULL;
	}

	entry->base_addr = base_addr;
	entry->size = size;
	return entry;
}

static int insert_iova(struct pgtable_map *pgtable_ctx,
		       struct iommu_addr_entry *entry)
{
	struct rb_node **new = &(pgtable_ctx->rbtree.rb_node);
	struct rb_node *parent = NULL;

	while (*new) {
		struct iommu_addr_entry *this = rb_entry(*new, struct iommu_addr_entry, rbnode);

		parent = *new;

		if (entry->base_addr < this->base_addr)
			new = &((*new)->rb_left);
		else if (entry->base_addr > this->base_addr)
			new = &((*new)->rb_right);
		else
			return -EEXIST;
	}

	rb_link_node(&entry->rbnode, parent, new);
	rb_insert_color(&entry->rbnode, &pgtable_ctx->rbtree);

	return 0;
}

int kiumd_configure_dma_cookie(struct device *dev,
			       enum iommu_dma_cookie_type cookie_type,
			       dma_addr_t dma_addr)
{
	struct kiumd_iommu_dma_cookie *cookie;
	int ret;

	cookie = kiumd_get_dma_cookie(dev);
	if (!cookie)
		return -EINVAL;

	ret = kiumd_set_dma_cookie_unlocked(cookie, cookie_type, dma_addr);
	if (ret) {
		pr_err("%s %d failed to set cookie\n", __func__, __LINE__);
		return -EINVAL;
	}

	return 0;
}

static struct iova_domain *kiumd_get_iova_domain(struct device *dev)
{
	struct kiumd_iommu_dma_cookie *cookie;
	struct iommu_domain *domain;
	struct iova_domain *iovad;

	domain = kiumd_iommu_get_dma_domain(dev);
	if (!domain) {
		pr_err("%s:dma_domain is invalid\n", __func__);
		return NULL;
	}

	cookie = (struct kiumd_iommu_dma_cookie *)domain->iova_cookie;
	iovad = &cookie->iovad;

	return iovad;
}

unsigned long get_shift_from_dt(struct device *dev)
{
	struct device_node *node;
	u32 shift = 0;

	node = dev->of_node;
	if (!of_property_read_u32(node, "qcom,iova-align-shift-max", &shift))
		return shift;

	return DEFAULT_IOVA_ALIGN_SHIFT_MAX;
}


static unsigned long limit_align_shift(struct device *dev, unsigned long shift,
				       unsigned long max_shift)
{
	unsigned long max_align_shift, final_shift = 0;
	struct iova_domain *iovad;

	iovad = kiumd_get_iova_domain(dev);
	if (iovad) {
		max_align_shift = max_shift + PAGE_SHIFT - iova_shift(iovad);
		final_shift = min_t(unsigned long, max_align_shift, shift);
	}

	return final_shift;
}

static unsigned long align_iova(struct device *dev, unsigned long start_iova,
				unsigned long size, unsigned long max_shift)
{
	unsigned long align_mask, final_shift;

	final_shift = limit_align_shift(dev, fls_long(size-1), max_shift);
	if (!final_shift)
		return ULONG_MAX;

	align_mask = (1UL << final_shift) - 1;

	if (start_iova % size != 0)
		start_iova = (start_iova + align_mask) & ~align_mask;

	return start_iova;
}

/**
 * alloc_iova_range - Allocate an IOVA range from the page table context
 * @addr_cache: kmem cache for rbtree entries/pagetable entries
 * @dev: Device requesting the IOVA
 * @ptable_ctx: Page table context
 * @smap: Mapping metadata
 * @max_shift: Alignment shift
 * @fixed_iova: Fixed IOVA address (if applicable)
 * @is_fix_map: Whether fixed mapping is requested
 *
 * Returns allocated IOVA address on success or negative error code.
 */
unsigned long alloc_iova_range(struct kmem_cache *addr_cache, struct device *dev,
		struct pgtable_map *ptable_ctx,	struct smmu_map_data *smap,
		unsigned long max_shift, unsigned long fixed_iova, bool is_fix_map)
{
	struct iommu_addr_entry *new_entry;
	struct iommu_addr_entry *entry;
	unsigned long result = 0;
	unsigned long start_iova;
	size_t size = smap->size;
	struct rb_node *node;

	guard(mutex)(&ptable_ctx->pgctx_lock);
	/* fix allocation path */
	if (is_fix_map) {
		start_iova = fixed_iova;
		if (start_iova + size > ptable_ctx->end_iova)
			return result;

		node = rb_first(&ptable_ctx->rbtree);
		while (node) {
			entry = rb_entry(node, struct iommu_addr_entry, rbnode);
			if ((start_iova < entry->base_addr + entry->size) &&
							(start_iova + size > entry->base_addr)) {
				return result;
			}
			node = rb_next(node);
		}

		goto dynamic_alloc;
	}

	/* Dynamic allocation path */
	start_iova = align_iova(dev, ptable_ctx->start_iova, size, max_shift);
	if (start_iova == ULONG_MAX)
		return result;

	node = rb_first(&ptable_ctx->rbtree);
	while (node) {
		entry = rb_entry(node, struct iommu_addr_entry, rbnode);
		if (start_iova + size <= entry->base_addr &&
						start_iova + size <= ptable_ctx->end_iova)
			goto dynamic_alloc;

		start_iova = entry->base_addr + entry->size;
		start_iova = align_iova(dev, start_iova, size, max_shift);
		if (start_iova == ULONG_MAX)
			return result;

		node = rb_next(node);
	}

	if (start_iova + size > ptable_ctx->end_iova)
		return result;

dynamic_alloc:
	new_entry = alloc_iommu_addr_entry(addr_cache, start_iova, size);
	if (!new_entry)
		return result;

	if (insert_iova(ptable_ctx, new_entry) != 0)
		return result;

	result = start_iova;
	return result;
}

/**
 * check_pgtable_context - Validate the pagetable context
 *
 * Parameters:
 * @vfio_fd: File descriptor for the VFIO device
 * @pgtable_ctx: Pointer to the pagetable context to validate
 *
 * Return: true if the pagetable context is valid, false otherwise.
 */
bool check_pgtable_context(struct device *dev, struct pgtable_map *pgtable_ctx)
{
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable *pgtable;
	unsigned long ttbr0;

	smmu_dom = kiumd_get_smmu_domain(dev);
	if (!smmu_dom) {
		pr_err("%s:%d invalid smmu_dom\n", __func__, __LINE__);
		return false;
	}

	pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if (!pgtable) {
		pr_err("%s:%d invalid pagetable\n", __func__, __LINE__);
		return false;
	}

	ttbr0 = pgtable->cfg.arm_lpae_s1_cfg.ttbr;

	if (ttbr0 != pgtable_ctx->ttbr0_addr) {
		pr_err("%s:%d mismatch pt_id and pgtable context, probably wrong id from UMD\n",
		       __func__, __LINE__);
		return false;
	}

	return true;
}

/**
 * kiumd_smmuv2_write_context_bank - Write smmu context bank for given index
 * bank register for given context bank
 *
 * Parameters:
 * @*smmu: pointer for arm smmu device information
 * @idx: index for context bank
 *
 * Returns void
 */
static void kiumd_smmuv2_write_context_bank(struct arm_smmu_device *smmu,
					    int idx)
{
	struct arm_smmu_cb *cb = &smmu->cbs[idx];
	struct arm_smmu_cfg *cfg = cb->cfg;
	bool stage1;
	u32 reg;

	trace_kiumd_smmuv2_write_context_bank_start(idx, cfg->cbndx, cfg->cbar);

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

	trace_kiumd_smmuv2_write_context_bank_end(idx);
}

/**
 * kiumd_smmuv2_set_ttbr0cfg - Configure TTBR0 settings for the ARM SMMU
 * for a specific vfio device(as of now used by GPU)
 *
 * Parameters:
 * @smmu_domain: Pointer to the SMMU domain structure
 * @pgtbl_cfg: Pointer to the page table configuration
 *
 * Return: 0 on success, negative error code on failure
 */
static int kiumd_smmuv2_set_ttbr0_cfg(struct arm_smmu_domain *smmu_domain,
				      const struct io_pgtable_cfg *pgtbl_cfg)
{

	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_cb *cb = &smmu_domain->smmu->cbs[cfg->cbndx];
	u32 tcr = cb->tcr[0];

	if (!(cb->tcr[0] & ARM_SMMU_TCR_EPD0)) {
		pr_err("TTBR0 translation is already enabled\n");
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

/**
 * kiumd_smmuv2_set_ttbr1_cfg - Configure TTBR1 settings for the ARM SMMU
 * for a specific vfio device(as of now used by LPAC).
 *
 * Parameters:
 * @smmu_domain: Pointer to the SMMU domain structure
 * @pgtbl_cfg: Pointer to the page table configuration
 *
 * Return: 0 on success, negative error code on failure
 */
static void kiumd_smmuv2_set_ttbr1_cfg(struct arm_smmu_domain *smmu_domain,
				      const struct io_pgtable_cfg *pgtbl_cfg)
{
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_cb *cb = &smmu_domain->smmu->cbs[cfg->cbndx];
	u32 tcr = cb->tcr[0];

	tcr |= arm_smmu_lpae_tcr(pgtbl_cfg);
	tcr &= ~(ARM_SMMU_TCR_EPD0 | ARM_SMMU_TCR_EPD1);

	cb->tcr[0] = tcr;
	cb->ttbr[1] = pgtbl_cfg->arm_lpae_s1_cfg.ttbr;
	cb->ttbr[1] |= FIELD_PREP(ARM_SMMU_TTBRn_ASID, cb->cfg->asid);

	kiumd_smmuv2_write_context_bank(smmu_domain->smmu, cb->cfg->cbndx);
}


/**
 * kiumd_perprocess_set_ttbr1_context - Configure TTTBR1 settings for the
 * ARM SMMU for a specific vfio device(as of now used by LPAC).
 *
 * Parameters:
 * @:iommu_dom: iommu_domain
 *
 * Return: 0 on success, negative error code on failure
 */
int kiumd_set_pgtble_ttbr1_context(struct iommu_domain *iommu_dom)
{
	struct io_pgtable_ops *pgtable_ops;
	struct arm_smmu_domain *smmu_dom;
	struct arm_smmu_cfg *smmu_cfg;
	struct io_pgtable *pagetable;
	struct io_pgtable_cfg cfg;
	struct arm_smmu_cb *cb;

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_dom || !smmu_dom->pgtbl_ops) {
		pr_err("%s: smmu domain/pagetable ops is invalid\n", __func__);
		return -EINVAL;
	}

	smmu_cfg = &smmu_dom->cfg;
	cb = &smmu_dom->smmu->cbs[smmu_cfg->cbndx];
	if (!(cb->tcr[0] & ARM_SMMU_TCR_EPD1)) {
		/** Not an error. During kgsl relaunch, we dont have to reprogram
		 *  TTBR1 if its already enabled.
		 */
		pr_err("%s: TTBR1 is already enabled for the device\n", __func__);
		return 0;
	}

	pagetable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if (!pagetable) {
		pr_err("%s: pagetable is NULL\n", __func__);
		return -EINVAL;
	}

	memcpy(&cfg, &pagetable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks |= IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;

	if (cfg.quirks & IO_PGTABLE_QUIRK_ARM_TTBR1) {
		iommu_dom->geometry.aperture_start = ~0UL << cfg.ias;
		iommu_dom->geometry.aperture_end = ~0UL;
	}

	pgtable_ops = alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!pgtable_ops) {
		pr_err("%s: failed to allocate pagetable ops\n", __func__);
		return -EINVAL;
	}

	kiumd_smmuv2_set_ttbr1_cfg(smmu_dom, &cfg);
	smmu_dom->pgtbl_ops = pgtable_ops;

	return 0;
}

/**
 * kiumd_perprocess_set_ttbr0_context - Configure TTTBR0 settings for a
 * specific vfio device(as of now used by GPU) ARM SMMU for the specified
 * device.
 *
 * Parameters:
 * @iommu_dom: iommu_domain
 * @kiumd_ctx: kiumd context associated with the device
 *
 * Return: 0 on success, negative error code on failure
 */
int kiumd_set_pgtble_ttbr0_context(struct iommu_domain *iommu_dom,
				   struct kiumd_ctx *kiumd_ctx)
{
	struct io_pgtable_ops *pgtable_ops;
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable *pgtable;
	struct io_pgtable_cfg cfg;
	int ret;

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if ((!smmu_dom) || (!(smmu_dom->pgtbl_ops))) {
		pr_err("%s:smmu domain/pagetable ops is invalid\n", __func__);
		return -EINVAL;
	}

	if (!kiumd_ctx->pgtable) {
		kiumd_ctx->pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
		if (!kiumd_ctx->pgtable) {
			pr_err("%s:pagetable is NULL\n", __func__);
			return -EINVAL;
		}
	}

	pgtable = kiumd_ctx->pgtable;
	if (!pgtable) {
		pr_err("%s: pgtable is null\n", __func__);
		return -EINVAL;
	}

	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	/*Allocate a default pagetable for TTBR0 in case per process allocation fails*/
	pgtable_ops = alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!pgtable_ops) {
		pr_err("%s:failed to allocate pagetable ops.\n", __func__);
		return -ENOMEM;
	}

	kiumd_smmuv2_set_ttbr0_cfg(smmu_dom, &cfg);
	ret = qcom_scm_kgsl_set_smmu_aperture(smmu_dom->cfg.cbndx);
	if (ret == -EBUSY)
		ret = qcom_scm_kgsl_set_smmu_aperture(smmu_dom->cfg.cbndx);

	if (ret) {
		pr_err("%s:Setting smmu aperture error: %d\n", __func__, ret);
		free_io_pgtable_ops(pgtable_ops);
		return ret;
	}

	return 0;
}

int kiumd_set_dma_addr_ranges(struct kiumd_ctx *kiumd_ctx, struct device *dev)
{
	const __be32 *addr_range;
	u64 start_addr, end_addr;
	struct device_node *np;
	int len;

	np = dev->of_node;
	if (!np) {
		dev_err(dev, "Device tree node not found\n");
		return -EINVAL;
	}

	addr_range = of_get_property(np, "qcom,iommu-dma-addr-range", &len);
	if (!addr_range)
		return 0; /*This is not an error, not every device need to have this property set*/

	if (len < (2 * sizeof(u32))) {
		dev_err(dev, "qcom,iommu-dma-addr-range property length is invalid\n");
		return -EINVAL;
	}

	start_addr = of_read_number(addr_range, 2);
	end_addr = of_read_number(addr_range + 2, 2);
	if (end_addr < start_addr) {
		dev_err(dev, "invalid address specified in the device tree\n");
		return -EINVAL;
	}

	kiumd_ctx->pt_start_iova = start_addr;
	kiumd_ctx->pt_end_iova = end_addr;

	return 0;
}

/**
 * clear_map_iova - This function facilitates to clear the global map based of
 * given iova and ptselect
 *
 * Parameters:
 * @kiumd_ctz: kiumd context attached with the GPU device
 * @smap: smap info containing iova data for the mapping being cleared
 *
 * Return: nothing
 */
void clear_kgsl_map_iova(struct kmem_cache *addr_cache,
			struct kiumd_ctx *kiumd_ctx, struct smmu_map_data *smap)
{
	struct pgtable_map *pgtble_ctx;
	u64 bit, size, iova;
	int ptselect, idx;

	size = smap->size;
	iova = smap->kgsl_ctx.iova;
	idx = smap->kgsl_ctx.pt_id;
	ptselect = smap->kgsl_ctx.ptselect;

	if (ptselect == KGSL_GLOBAL_PT) {
		bit = (iova & ~KGSL_GLOBAL_PT_BASE_IOVA) >> PAGE_SHIFT;
		spin_lock(&global_map_lock);
		bitmap_clear(global_map, bit, size >> PAGE_SHIFT);
		spin_unlock(&global_map_lock);
	} else {
		pgtble_ctx = kiumd_get_pgtable_entry(kiumd_ctx, idx);
		free_iova_range(addr_cache, pgtble_ctx, iova);
	}

}

void clean_map(struct kmem_cache *addr_cache, struct kiumd_ctx *kiumd_ctx,
		struct smmu_map_data *smap)
{
	guard(mutex)(&kiumd_ctx->map_lock);
	if (smap->is_iova_zero)
		kiumd_dmabuf_zero_unmap(smap);
	else if (smap->is_priv_map)
		kiumd_dmabuf_priv_unmap(smap);
	else
		kiumd_dmabuf_unmap(smap);

	if (smap->is_kgsl_ctx)
		(void)clear_kgsl_map_iova(addr_cache, kiumd_ctx, smap);

	if (smap->iova_rb)
		(void)free_allocated_iova(addr_cache, kiumd_ctx, smap->iova_rb);

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	spin_unlock(&kiumd_ctx->smmu_lock);
}


/**
 * get_map_offset - This function facilitates to get the offset of the global
 * map based of given size and  ptselect
 *
 * Parameters:
 * @size: u64 size
 * @ptselect: type of pagetable per process or global
 *
 * Returns map offset
 */
static u64 get_map_offset_global(u64 size)
{
	static u64 last_offset_global;
	unsigned long *map = global_map;
	u64 bit, size_in_pages, iova;

	if (size < PAGE_SIZE) {
		pr_err("%s: Invalid size: 0x%llx\n", __func__, size);
		return 0;
	}

	size_in_pages = size >> PAGE_SHIFT;
	spin_lock(&global_map_lock);
	bit = bitmap_find_next_zero_area(map, KGSL_PT_MEM_PAGES,
					 last_offset_global, size_in_pages, 0);
	if (unlikely(bit >= KGSL_PT_MEM_PAGES)) {
		//Reset from IOVA_ZERO
		bit = bitmap_find_next_zero_area(map, KGSL_PT_MEM_PAGES,
						 IOVA_ZERO, size_in_pages, 0);
		if (unlikely(bit >= KGSL_PT_MEM_PAGES)) {
			spin_unlock(&global_map_lock);
			return 0;
		}
	}

	bitmap_set(map, bit, size_in_pages);
	last_offset_global = (bit + size_in_pages) % KGSL_PT_MEM_PAGES;
	spin_unlock(&global_map_lock);
	iova = bit << PAGE_SHIFT;

	/*
	 * The iova we found is relative to zero iova
	 * We need to shift it relative to start global
	 * IOVA
	 */
	iova |= KGSL_GLOBAL_PT_BASE_IOVA;
	return iova;
}

/**
 * find_available_region_in_range - Search for an available IOVA region within
 * a given VA range in RB tree
 *
 * Parameters:
 * @ptable_ctx: Pointer to the page table mapping context
 * @search_start: Start of the IOVA search range (inclusive)
 * @search_end: End of the IOVA search range (exclusive)
 * @size: Size of the region to allocate
 *
 * Return: Starting address of the available region, or 0 if none found.
 */
static unsigned long find_available_region_in_range(struct pgtable_map *ptable_ctx,
						   unsigned long search_start,
						   unsigned long search_end,
						   unsigned long size)
{
	struct rb_node *node = rb_first(&ptable_ctx->rbtree);
	unsigned long available_start = search_start;
	struct iommu_addr_entry *entry;

	while (node) {
		entry = rb_entry(node, struct iommu_addr_entry, rbnode);
		if (available_start + size <= entry->base_addr &&
			available_start + size <= search_end) {
			return available_start;
		}

		if (entry->base_addr + entry->size > available_start)
			available_start = entry->base_addr + entry->size;

		if (available_start >= search_end)
			break;
		node = rb_next(node);
	}

	// Final gap check after last node
	if (available_start + size <= search_end)
		return available_start;

	return 0; // No available region found
}

/**
 * alloc_iova_range_contiguous - Allocate a contiguous IOVA range
 *
 * Parameters:
 * @ptable_ctx: Pointer to the pagetable context
 * @size: Size of the IOVA range to allocate
 *
 * Return: The starting address of the allocated IOVA range, or 0 if no suitable
 * range is found.
 */
static unsigned long alloc_iova_range_contiguous(struct kmem_cache *addr_cache,
						struct pgtable_map *ptable_ctx,
						unsigned long size)
{
	struct iommu_addr_entry *new_entry = NULL;
	unsigned long available_start = 0;
	unsigned long last_allocated_end;

	spin_lock(&ptable_ctx->kgsl_rbtree_lock);
	last_allocated_end = ptable_ctx->last_allocated_end;

	// First pass: search from last_allocated_end to end_iova
	available_start = find_available_region_in_range(ptable_ctx,
							last_allocated_end,
							ptable_ctx->end_iova,
							size);
	// Second pass: wrap around if needed
	if (!available_start) {
		available_start = find_available_region_in_range(ptable_ctx,
								ptable_ctx->start_iova,
								last_allocated_end,
								size);
	}

	spin_unlock(&ptable_ctx->kgsl_rbtree_lock);
	// If no region found, return failure
	if (!available_start)
		return 0;

	new_entry = alloc_iommu_addr_entry(addr_cache,
					   available_start, size);
	if (!new_entry)
		return 0;

	spin_lock(&ptable_ctx->kgsl_rbtree_lock);
	insert_iova(ptable_ctx, new_entry);
	ptable_ctx->last_allocated_end = available_start + size;
	spin_unlock(&ptable_ctx->kgsl_rbtree_lock);
	return available_start;
}

/**
 * get_pgtble_and_alloc_iova - Retrieve pagetable entry and allocate IOVA range
 *
 * Parameters:
 * @vfio_fd: File descriptor for the VFIO device
 * @dev: device
 * @kiumd_ctx: Pointer to the kiumd context
 * @size: Size of the IOVA range to allocate
 * @idx: Index of the pagetable entry to retrieve
 *
 * Return: The starting address of the allocated IOVA range, or 0 if any step fails.
 */
static uint64_t get_pgtble_and_alloc_iova(struct kmem_cache *addr_cache,
					  struct device *dev,
					  struct kiumd_ctx *kiumd_ctx,
					  u64 size, unsigned int idx)
{
	struct pgtable_map *pgtble_ctx;
	uint64_t addr;

	pgtble_ctx = kiumd_get_pgtable_entry(kiumd_ctx, idx);
	if (!pgtble_ctx) {
		pr_err("%s:%d Invalid id for hash table: id: %d\n", __func__,
			__LINE__, idx);
		return 0;
	}

	if (!check_pgtable_context(dev, pgtble_ctx)) {
		pr_err("%s:%d check_pgtable_context failed\n", __func__, __LINE__);
		return 0;
	}

	addr = alloc_iova_range_contiguous(addr_cache, pgtble_ctx, size);
	if (!addr) {
		pr_err("%s:%d IOVA memory limit reached\n", __func__, __LINE__);
		return 0;
	}

	return addr;
}

int set_kgsl_map_iova(struct kmem_cache *addr_cache, struct kiumd_ctx *kiumd_ctx,
			struct kiumd_user kiusr, struct smmu_map_data *smap)
{
	struct device *dev = smap->dev;
	u64 size = smap->size;
	u64 iova;
	int ret;

	if (kiusr.ptselect == KGSL_DEFAULT_PT)
		smap->is_kgsl_map = true;

	if (!KIUSR_IS_KGSL(kiusr))
		return 0;

	if (kiusr.ptselect == KGSL_GLOBAL_PT)
		iova = get_map_offset_global(size);
	else
		iova = get_pgtble_and_alloc_iova(addr_cache, dev, kiumd_ctx, size,
						 kiusr.pt_id);

	if (!iova) {
		dev_err(dev, "Failed to get valid iova for the GPU device\n");
		return -ENOSPC;
	}

	smap->is_fixed_map = true;
	smap->is_kgsl_map = true;
	smap->is_kgsl_ctx = true;
	smap->kgsl_ctx.ptselect = kiusr.ptselect;
	smap->kgsl_ctx.pt_id = kiusr.pt_id;
	smap->kgsl_ctx.iova = iova;

	ret = kiumd_configure_dma_cookie(dev, IOMMU_DMA_MSI_COOKIE, iova);
	if (ret) {
		dev_err(dev, "Failed to set cookie for the GPU device\n");
		(void)clear_kgsl_map_iova(addr_cache, kiumd_ctx, smap);
		return -EINVAL;
	}

	return 0;
}

void add_to_smmu_table(struct kiumd_ctx *ctx, struct smmu_map_data *map_data)
{
	spin_lock(&ctx->smmu_lock);
	map_data->id = ctx->id++;
	hash_add(ctx->smmu_table, &map_data->node, map_data->id);
	spin_unlock(&ctx->smmu_lock);
}

int set_allocated_iova(struct device *dev, unsigned long iova)
{
	struct kiumd_iommu_dma_cookie *cookie;
	int ret;

	cookie = kiumd_get_dma_cookie(dev);
	if (!cookie) {
		pr_err("%s failed to get cookie\n", __func__);
		return -EINVAL;
	}

	ret = kiumd_set_dma_cookie_unlocked(cookie, IOMMU_DMA_MSI_COOKIE, iova);
	if (ret)
		pr_err("%s failed to set cookie\n", __func__);

	return ret;
}

int init_and_allocate_iova(struct kmem_cache *addr_cache, struct device *dev,
			   struct kiumd_ctx *kiumd_ctx,
			   struct smmu_map_data *smap, unsigned long max_shift,
			   unsigned long fixed_iova, bool is_fixed_map)
{
	unsigned long iova;
	int ret;

	if (!kiumd_ctx->pgtable_ctx) {
		guard(mutex)(&kiumd_ctx->managed_rbtree_lock);
		if (!kiumd_ctx->pgtable_ctx) {
			kiumd_ctx->pgtable_ctx = kzalloc(sizeof(struct pgtable_map), GFP_KERNEL);
			if (!kiumd_ctx->pgtable_ctx)
				return -ENOMEM;

			kiumd_ctx->pgtable_ctx->rbtree = RB_ROOT;
			kiumd_ctx->pgtable_ctx->start_iova = kiumd_ctx->pt_start_iova;
			kiumd_ctx->pgtable_ctx->end_iova = kiumd_ctx->pt_end_iova;
			mutex_init(&kiumd_ctx->pgtable_ctx->pgctx_lock);
		}
	}

	iova = alloc_iova_range(addr_cache, dev, kiumd_ctx->pgtable_ctx, smap,
				max_shift, fixed_iova, is_fixed_map);
	if (!iova) {
		pr_err("%s: Failed to allocate iova.\n", __func__);
		return -ENOMEM;
	}

	smap->is_fixed_map = true;
	smap->iova_rb  = iova;
	ret = kiumd_configure_dma_cookie(dev, IOMMU_DMA_MSI_COOKIE, iova);
	if (ret < 0) {
		pr_err("%s: Failed to set the iova.\n", __func__);
		free_allocated_iova(addr_cache, kiumd_ctx, iova);
		return -ENOMEM;
	}

	return 0;
}

u64 kiumd_get_dmabuf_size(int dmabuf_fd)
{
	struct dma_buf *kiumd_dmabuf;
	u64 size;

	kiumd_dmabuf = dma_buf_get(dmabuf_fd);
	if (IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:dma_buf_get failed with error: %ld\n",
		       __func__, PTR_ERR(kiumd_dmabuf));

		return 0;
	}
	size = kiumd_dmabuf->size;
	dma_buf_put(kiumd_dmabuf);
	return size;
}

/**
 * @Brief: This function facilitates to check the fixed iova mapping  by
 * checking the cookie type for the device.
 *
 * Parameters:
 * @dev: pointer for device structure
 *
 * Returns true/false
 */
bool is_fixed_mapping(struct device *dev)
{
	struct kiumd_iommu_dma_cookie *cookie;

	cookie = kiumd_get_dma_cookie(dev);
	if (cookie) {
		if (cookie->type == IOMMU_DMA_MSI_COOKIE)
			return true;
	}

	return false;
}

struct smmu_map_data *allocate_init_smap(struct kiumd_user kiusr,
					 struct device *dev, u64 size)
{
	struct smmu_map_data *smap;

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap)
		return NULL;

	smap->dev = dev;
	smap->size = size;
	smap->dma_dir = kiusr.dma_direction == DMA_TO_DEVICE ?
			DMA_TO_DEVICE : DMA_BIDIRECTIONAL;

	smap->is_priv_map = KIUSR_IS_PRIV(kiusr);
	smap->is_iova_zero = KIUSR_IS_IOVA_ZERO(kiusr);
	smap->is_fixed_map = is_fixed_mapping(smap->dev);

	return smap;
}

struct smmu_map_data *allocate_init_smap_mmio(struct  kiumd_smmu_mmio_map kiusr,
					      struct resource *res,
					      struct device *dev)
{
	struct kiumd_smmu_mmio_ctx *mmio_ctx;
	struct smmu_map_data *smap;

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap)
		return NULL;

	mmio_ctx = kzalloc(sizeof(*mmio_ctx), GFP_KERNEL);
	if (!mmio_ctx) {
		kfree(smap);
		return NULL;
	}

	mmio_ctx->size = resource_size(res);
	mmio_ctx->addr = res->start;
	smap->context = mmio_ctx;
	smap->dev = dev;
	smap->is_fixed_map = kiusr.iova && kiusr.fixed_iova;
	return smap;
}

static int kiumd_iommu_zero_map(struct smmu_map_data *smap)
{
	struct iommu_domain *iommu_dom;
	u64 mapped_size;
	int prot;

	if (smap->is_priv_map)
		prot = IOMMU_CACHE | IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV;
	else
		prot = IOMMU_CACHE | IOMMU_READ | IOMMU_WRITE;

	iommu_dom = kiumd_iommu_get_dma_domain(smap->dev);
	if (!iommu_dom)
		return -EINVAL;

	kiumd_mangle_sg_table(smap->sgt_ptr);
	mapped_size = iommu_map_sg(iommu_dom, IOVA_ZERO, smap->sgt_ptr->sgl,
				   smap->sgt_ptr->orig_nents, prot, GFP_ATOMIC);

	if (smap->size != mapped_size) {
		dev_err(smap->dev, "%s:iommu_map_sg failed\n", __func__);
		return -EFAULT;
	}

	return 0;
}

int kiumd_dmabuf_zero_map(struct smmu_map_data *smap)
{
	struct dma_buf_attachment *dmabufattach;
	struct sg_table *sgt;
	int ret;

	dmabufattach = dma_buf_attach(smap->dmabuf_ptr, smap->staging_dev);
	if (IS_ERR(dmabufattach)) {
		dev_err(smap->dev, "%s:attach failed error:%ld\n",
			__func__, PTR_ERR(dmabufattach));
		return PTR_ERR(dmabufattach);
	}

	smap->dmabufattach = dmabufattach;
	sgt = dma_buf_map_attachment_unlocked(dmabufattach, smap->dma_dir);
	if (IS_ERR(sgt)) {
		dev_err(smap->dev, "%s:map failed error:%ld\n",
			__func__, PTR_ERR(sgt));
		ret = PTR_ERR(sgt);
		goto fail_attach;
	}

	smap->sgt_ptr = sgt;
	/*
	 * For mapping at 0x0 we create a mapping using
	 * dma_buf_map_attachment_unlocked and then take the sg list
	 * and map it at iova - 0x0
	 */
	ret = kiumd_iommu_zero_map(smap);
	if (ret)
		goto fail_attachment;

	return 0;

fail_attachment:
	dma_buf_unmap_attachment_unlocked(dmabufattach, sgt,
					  smap->dma_dir);
fail_attach:
	dma_buf_detach(smap->dmabuf_ptr, dmabufattach);
	return ret;
}

int kiumd_dmabuf_priv_map(struct smmu_map_data *smap)
{
	struct kiumd_dma_heap_attachment *dmaheapattachment;
	struct dma_buf_attachment *dmabufattach;
	struct sg_table *sgt;
	int ret;

	dmabufattach = dma_buf_attach(smap->dmabuf_ptr, smap->dev);
	if (IS_ERR(dmabufattach)) {
		dev_err(smap->dev, "%s:attach failed error:%ld\n",
			__func__, PTR_ERR(dmabufattach));
		return PTR_ERR(dmabufattach);
	}
	smap->dmabufattach = dmabufattach;

	dmaheapattachment = (struct kiumd_dma_heap_attachment *)dmabufattach->priv;
	sgt = dmaheapattachment->table;
	if (!sgt) {
		dev_err(smap->dev, "%s:sglist is NULL\n", __func__);
		ret = -EINVAL;
		goto fail_attach;
	}

	ret = dma_map_sgtable(smap->dev, sgt, smap->dma_dir,
			      DMA_ATTR_PRIVILEGED | DMA_ATTR_SKIP_CPU_SYNC);
	if (ret) {
		dev_err(smap->dev, "%s:dma_map_sgtable fail error:%d\n",
			__func__, ret);
		goto fail_attach;
	}
	smap->sgt_ptr = sgt;

	return 0;

fail_attach:
	dma_buf_detach(smap->dmabuf_ptr, dmabufattach);
	return ret;
}

int kiumd_dmabuf_map(struct smmu_map_data *smap)
{
	struct dma_buf_attachment *dmabufattach;
	struct sg_table *sgt;
	int ret;

	dmabufattach = dma_buf_attach(smap->dmabuf_ptr, smap->dev);
	if (IS_ERR(dmabufattach)) {
		dev_err(smap->dev, "%s:attach failed error:%ld\n",
			__func__, PTR_ERR(dmabufattach));
		return PTR_ERR(dmabufattach);
	}
	smap->dmabufattach = dmabufattach;

	sgt = dma_buf_map_attachment_unlocked(dmabufattach, smap->dma_dir);
	if (IS_ERR(sgt)) {
		dev_err(smap->dev, "%s:map failed error:%ld\n",
			__func__, PTR_ERR(sgt));
		ret = PTR_ERR(sgt);
		goto fail_attach;
	}
	smap->sgt_ptr = sgt;
	return 0;

fail_attach:
	dma_buf_detach(smap->dmabuf_ptr, dmabufattach);
	return ret;
}

int kiumd_mmio_map(struct smmu_map_data *smap)
{
	smap->context->iova = dma_map_resource(smap->dev,
			      smap->context->addr, smap->context->size, 0, 0);
	return dma_mapping_error(smap->dev, smap->context->iova);

}

void release_map_data(struct kiumd_ctx *kiumd_ctx, struct smmu_map_data *smap)
{
	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	kfree(smap);
	spin_unlock(&kiumd_ctx->smmu_lock);
}


void kiumd_dmabuf_zero_unmap(struct smmu_map_data *smap)
{
	struct iommu_domain *iommu_dom;
	u64 unmapped_size;

	iommu_dom = kiumd_iommu_get_dma_domain(smap->dev);
	if (!iommu_dom)
		return;
	unmapped_size = iommu_unmap(iommu_dom, IOVA_ZERO, smap->size);
	if (unmapped_size != smap->size) {
		dev_err(smap->dev, "%s:iommu_unmap unmapped %llu\n",
			__func__, unmapped_size);
	}

	dma_buf_detach(smap->dmabuf_ptr, smap->dmabufattach);
	dma_buf_put(smap->dmabuf_ptr);
}

void kiumd_dmabuf_priv_unmap(struct smmu_map_data *smap)
{

	dma_unmap_sgtable(smap->dev, smap->sgt_ptr, smap->dma_dir,
			  DMA_ATTR_PRIVILEGED);
	dma_buf_detach(smap->dmabuf_ptr, smap->dmabufattach);
	dma_buf_put(smap->dmabuf_ptr);
}

void kiumd_dmabuf_unmap(struct smmu_map_data *smap)
{
	dma_buf_unmap_attachment_unlocked(smap->dmabufattach, smap->sgt_ptr,
					  smap->dma_dir);
	dma_buf_detach(smap->dmabuf_ptr, smap->dmabufattach);
	dma_buf_put(smap->dmabuf_ptr);
}

void kiumd_mmio_unmap(struct smmu_map_data *smap)
{
	dma_unmap_resource(smap->dev, smap->context->iova,
			   smap->context->size, 0, 0);
	kfree(smap->context);
}

unsigned long get_hash_key(struct device *dev)
{
	unsigned long hash_id;

	hash_id = (unsigned long) kiumd_iommu_get_dma_domain(dev);
	if (!hash_id) {
		pr_err("%s:invalid domain\n", __func__);
		return -EINVAL;
	}

	return hash_id;
}

/**
 * @kiumd_io_pgtable_hyp_assign_page - This function facilitates to hyp-assign
 * the page for given vmid
 *
 * Parameters:
 * @vmid: user space argument pointer
 * @page: Page
 * @nr_acl_entries: Number of acl entries for scm assign
 *
 * Return: 0 upon success and error codes on failure
 */
int kiumd_io_pgtable_hyp_assign_page(u32 *vmid, u64 page, u32 nr_acl_entries)
{
	struct qcom_scm_vmperm *dst_vmids;
	int ret, i;
	u64 src_vmid_list[2] = {0};

	src_vmid_list[0] = BIT(QCOM_SCM_VMID_HLOS);
	trace_kiumd_io_pgtable_hyp_assign_page_start(vmid, page, nr_acl_entries);

	dst_vmids = kcalloc((nr_acl_entries + 1), sizeof(*dst_vmids),
			    GFP_KERNEL);
	if (!dst_vmids)
		return -ENOMEM;

	i = 0;
	dst_vmids[i].vmid = QCOM_SCM_VMID_HLOS;
	dst_vmids[i].perm = QCOM_SCM_PERM_RW;
	pr_debug("Hyp assign page for dst:%d vmid:%d perm:%d total vmids:%d\n",
		 i, dst_vmids[i].vmid, dst_vmids[i].perm, nr_acl_entries + 1);

	for (i = 1; i < nr_acl_entries + 1; i++) {
		dst_vmids[i].vmid = vmid[i - 1];
		dst_vmids[i].perm = QCOM_SCM_PERM_READ;
		pr_debug("Hyp assign page for dst:%d vmid:%d perm:%d\n",
			 i, dst_vmids[i].vmid, dst_vmids[i].perm);
	}

	ret = qcom_scm_assign_mem(page, PAGE_SIZE, &src_vmid_list[0],
		dst_vmids, nr_acl_entries + 1);
	if (ret)
		pr_err("hyp assign for %llu address of size %lx rc:%d\n",
		       page, PAGE_SIZE, ret);
	kfree(dst_vmids);

	trace_kiumd_io_pgtable_hyp_assign_page_end(page, nr_acl_entries);
	return ret;
}

/**
 * kiumd_io_pgtable_hyp_unassign_page - This function facilitates to
 * hyp-unassign the page for given vmid
 *
 * Parameters:
 * @vmid: user space argument pointer
 * @page: Page
 * @nr_acl_entries: Number of acl entries for scm assign
 *
 * Return: 0 upon success and error codes on failure
 */
int kiumd_io_pgtable_hyp_unassign_page(u32 *vmid, u64 page, u32 nr_acl_entries)
{
	int ret;
	u64 src_vmid_list[2] = {0};

	src_vmid_list[0] = BIT(QCOM_SCM_VMID_HLOS);

	trace_kiumd_io_pgtable_hyp_unassign_page_start(vmid, page, nr_acl_entries);

	struct qcom_scm_vmperm dst_vmids[] = { {
			QCOM_SCM_VMID_HLOS,
			QCOM_SCM_PERM_RWX
		}
	};
	for (int i = 0; i < nr_acl_entries ; i++) {
		qcom_scm_set_vmid_by_word(&src_vmid_list[0], vmid[i]);
		pr_debug("Hyp unassign page for dst:%d vmid:%d\n",
			 i, vmid[i]);
	}

	ret = qcom_scm_assign_mem(page, PAGE_SIZE, &src_vmid_list[0],
				  dst_vmids, ARRAY_SIZE(dst_vmids));
	if (ret)
		pr_err("hyp unassign failed %llu address of size %lx rc:%d\n",
		       page, PAGE_SIZE, ret);

	trace_kiumd_io_pgtable_hyp_unassign_page_end(page, nr_acl_entries);
	return ret;
}

/**
 * kiumd_hyp_unassign_sg - This function facilitates to transfer memory
 * ownership for given sg for source vm list
 *
 * Parameters:
 * @sgt: user space argument pointer
 * @source_vm_list: Page
 * @source_nelems: Number of acl entries for scm assign
 * @clear_page_private: boolean flag to check sg private page clearance
 *
 * Return: 0 upon success and error codes on failure
 */
int kiumd_hyp_unassign_sg(struct sg_table *sgt, int *source_vm_list,
			  int source_nelems, bool clear_page_private)
{
	struct qcom_scm_vmperm dst_vmids[] = { {
			QCOM_SCM_VMID_HLOS,
			QCOM_SCM_PERM_RWX
		}
	};
	struct scatterlist *sg;
	u64 src_vmid_list[2] = {0};
	u64 src_vmid_list_copy[2] = {0};
	int ret, i;

	if (source_nelems <= 0)
		return -EINVAL;

	if (!sgt)
		return -EINVAL;

	if (!sgt->sgl)
		return -EINVAL;

	trace_kiumd_hyp_unassign_sg_start(sgt);
	sg = sgt->sgl;

	for (int j = 0; j < source_nelems ; j++) {
		qcom_scm_set_vmid_by_word(&src_vmid_list[0], source_vm_list[j]);
		pr_debug("Hyp unassign sg for dst:%d vmid:%d\n",
			 j, source_vm_list[j]);
	}
	src_vmid_list_copy[0] = src_vmid_list[0];
	do {
		src_vmid_list[0] = src_vmid_list_copy[0];
		pr_debug("%s: memory ownership transfer start src vmid:%llx\n",
			__func__, src_vmid_list[0]);
		ret = qcom_scm_assign_mem(page_to_phys(sg_page(sg)), sg->length,
		&src_vmid_list[0], dst_vmids,
		ARRAY_SIZE(dst_vmids));
		if (ret) {
			pr_err("Hyp unassign failed %llu address of size %x rc:%d\n",
			       page_to_phys(sg_page(sg)), sg->length, ret);
			goto out;
		}
		pr_debug("%s: memory ownership transfer end:%d\n", __func__, ret);
		sg = sg_next(sg);
	} while (sg);

	if (clear_page_private)
		for (i = 0, sg = sgt->sgl; i < sgt->nents && sg; i++, sg = sg_next(sg))
			ClearPagePrivate(sg_page(sg));

	if (i != sgt->nents)
		pr_warn("%s: sg list shorter than nents (%d < %d)\n",
		       __func__, i, sgt->nents);

	trace_kiumd_hyp_unassign_sg_end(sgt);
out:
	return ret;
}

/**
 * kiumd_hyp_assign_sg - This function facilitates to hyp-assign
 * given sg for destination vm list
 *
 * Parameters:
 * @sgt: scatter gather table ptr
 * @dest_vm_list: destination vm list
 * @dest_nelems: Number of entries for destination
 * @set_page_private: page flag
 * @dest_perms: Destination permission
 *
 * Return: 0 upon success and error codes on failure
 */
int kiumd_hyp_assign_sg(struct sg_table *sgt, int *dest_vm_list,
			int dest_nelems, bool set_page_private,
			int *dest_perms)
{
	struct qcom_scm_vmperm *dst_vmids;
	struct scatterlist *sg;
	u64 src_vmid_list[2] = {0};
	int ret;

	src_vmid_list[0] = BIT(QCOM_SCM_VMID_HLOS);
	if (dest_nelems <= 0) {
		pr_err("%s: dest_nelems invalid\n", __func__);
		return -EINVAL;
	}

	if (!sgt)
		return -EINVAL;

	trace_kiumd_hyp_assign_sg_start(sgt);
	sg = sgt->sgl;
	if (!sg)
		return -EINVAL;

	dst_vmids = kcalloc(dest_nelems, sizeof(*dst_vmids), GFP_KERNEL);
	if (!dst_vmids)
		return -ENOMEM;

	for (int i = 0; i < dest_nelems; i++) {
		dst_vmids[i].vmid = dest_vm_list[i];
		dst_vmids[i].perm = dest_perms[i];
		pr_debug("Hyp assign sg for dst:%d vmid:%d perm:%d\n",
			 i, dst_vmids[i].vmid, dst_vmids[i].perm);
	}

	do {
		src_vmid_list[0] = BIT(QCOM_SCM_VMID_HLOS);
		pr_debug("Assign call initiated :%llx\n", src_vmid_list[0]);
		ret = qcom_scm_assign_mem(page_to_phys(sg_page(sg)), sg->length, &src_vmid_list[0],
					  dst_vmids, dest_nelems);
		if (ret) {
			pr_err("failed qcom_assign for assigning %llx address of size %x rc:%d\n",
			       page_to_phys(sg_page(sg)), sg->length, ret);
			goto err;
		}
		pr_debug("Assign call success:%d\n", ret);
		sg = sg_next(sg);
	} while (sg);
	pr_debug("%s success\n", __func__);
err:
	kfree(dst_vmids);

	trace_kiumd_hyp_assign_sg_end(sgt);
	return ret;
}

/**
 * kiumd_acl_to_vmid_perms_list - This function facilitates to set the
 * destination vmids and permissions to given vmids and permissions.
 *
 * Parameters:
 * @nr_acl_entries: Number of acl entries for scm assign
 * @acl_entries: acl entries for scm assign
 * @dst_vmids: pointer to destination vmid
 * @dest_perms: Destination permission
 *
 * Return: 0 upon success and error codes on failure
 */
int kiumd_acl_to_vmid_perms_list(unsigned int nr_acl_entries,
				 const void __user *acl_entries,
				 int **dst_vmids, int **dst_perms)
{
	struct kiumd_acl_entry entry;
	int ret, i, *vmids, *perms;

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

/**
 * kiumd_get_pgd - Facilitates to get the page table global directory by
 * getting iommu_domain, smmu_domain and pagetable
 *
 * Parameters:
 * @dev: device
 * @pgd: page global directory pointer
 *
 * Return: 0 upon success and error codes on failure
 */
int kiumd_get_pgd(struct device *dev, u64 *pgd)
{
	struct arm_smmu_domain *smmu_dom;
	struct iommu_domain *iommu_dom;
	struct io_pgtable *pgtable;

	if (!pgd) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		return -EINVAL;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(dev);
	if (!iommu_dom) {
		pr_err("%s:%d Failed to get IOMMU DOMAIN\n",
		       __func__, __LINE__);
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

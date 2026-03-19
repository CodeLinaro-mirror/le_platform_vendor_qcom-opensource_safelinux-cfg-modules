// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/adreno-smmu-priv.h>
#include <linux/dma-buf.h>
#include <linux/dma-direction.h>
#include <linux/eventfd.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/hashtable.h>
#include <linux/iommu.h>
#include <linux/iova.h>
#include <linux/io-pgtable.h>
#include <linux/iommu_iova_map.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <uapi/misc/kiumd.h>
#include <linux/kiumd_common.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/string.h>
#include <linux/xarray.h>
#include <linux/pm_runtime.h>

#define IS_PGTABLE_SET(kiusr) \
	((kiusr.ptselect == KGSL_GLOBAL_PT) || (kiusr.ptselect == KGSL_PER_PROCESS_PT))

#define GET_KGSL_DATA(file) \
	container_of((file)->private_data, struct umd_kgsl_data, mdev)

struct irq_context {
	int hwirq;
	struct eventfd_ctx *eventfd_ctx;
	bool irq_masked;
	char *irq_name;
};

struct kgsl_fault_info {
	u32 fsr;
	spinlock_t lock;
	unsigned long fault_count;
	unsigned long pgtable_id;
	unsigned long iova;
	struct device_attribute attr_info;
	struct kernfs_node *fs_node;
};

struct umd_kgsl_data {
	struct resource *resources;
	int num_regs;
	struct platform_device *pdev;
	struct device *dev;
	struct miscdevice mdev;
	struct kiumd_ctx *kiumdctx;
	struct irq_context *irq_ctx;
	int num_irqs;
	struct kgsl_fault_info *fault_info;
};

static struct kmem_cache *kgsl_addr_cache;

/**
 * umd_kgsl_register_eventfd - Register an eventfd for a given IRQ
 * @kgsl_data: Pointer to KGSL driver context
 * @eventfd: Eventfd file descriptor
 * @irq_index: Index of the IRQ to associate with the eventfd
 *
 * Associates an eventfd with a hardware IRQ and enables the IRQ.
 *
 * Return: 0 on success, negative error code on failure.
 */

static int umd_kgsl_register_eventfd(struct umd_kgsl_data *kgsl_data, int eventfd, int irq_index)
{
	struct irq_context *ctx;

	if (irq_index >= kgsl_data->num_irqs)
		return -EINVAL;

	ctx = &kgsl_data->irq_ctx[irq_index];
	ctx->eventfd_ctx = eventfd_ctx_fdget(eventfd);
	if (IS_ERR(ctx->eventfd_ctx)) {
		dev_err(kgsl_data->dev, "Failed to get eventfd context\n");
		return PTR_ERR(ctx->eventfd_ctx);
	}

	enable_irq(ctx->hwirq);
	dev_info(kgsl_data->dev, "Eventfd registered successfully\n");
	return 0;
}

/**
 * umd_kgsl_unmask_interrupt - Unmask a masked IRQ
 * @kgsl_data: Pointer to KGSL driver context
 * @irq_index: Index of the IRQ to unmask
 *
 * Enables the IRQ if it was previously masked.
 *
 * Return: 0 on success, -EINVAL if the index is invalid.
 */

static int umd_kgsl_unmask_interrupt(struct umd_kgsl_data *kgsl_data, int irq_index)
{
	struct irq_context *ctx;

	if (irq_index >= kgsl_data->num_irqs)
		return -EINVAL;

	ctx = &kgsl_data->irq_ctx[irq_index];
	if (ctx->irq_masked) {
		ctx->irq_masked = false;
		enable_irq(ctx->hwirq);
	}

	return 0;
}

/**
 * umd_kgsl_irq_handler - IRQ handler for KGSL device
 * @irq: IRQ number
 * @dev_id: Pointer to IRQ context structure
 *
 * Disables the IRQ, marks it as masked, and signals the associated eventfd.
 *
 * Return: IRQ_HANDLED
 */

static irqreturn_t umd_kgsl_irq_handler(int irq, void *dev_id)
{
	struct irq_context *ctx = (struct irq_context *)dev_id;

	disable_irq_nosync(ctx->hwirq);
	ctx->irq_masked = true;

	if (ctx->eventfd_ctx)
		eventfd_signal(ctx->eventfd_ctx, 1);

	return IRQ_HANDLED;
}

struct pgtable_map *kgsl_get_pgtable_entry(struct kiumd_kgsl_context *kgsl_context,
					   struct kiumd_ctx *kiumd_ctx,
					   unsigned long idx)
{
	struct pgtable_map *pgtble_ctx;
	bool found = false;

	spin_lock(&kgsl_context->kgsl_hash_lock);
	hash_for_each_possible(kiumd_ctx->kgsl_page_table, pgtble_ctx, node, idx) {
		if (pgtble_ctx->idx == idx) {
			found = true;
			break;
		}
	}

	spin_unlock(&kgsl_context->kgsl_hash_lock);
	if (!found) {
		pr_err("%s:%d id not found in hash table\n", __func__, __LINE__);
		return NULL;
	}

	return pgtble_ctx;
}

static void release_pgtbl_context(struct kiumd_kgsl_context *kgsl_context,
					struct pgtable_map *pgtbl_ctx)
{
	spin_lock(&kgsl_context->kgsl_hash_lock);
	hash_del(&pgtbl_ctx->node);
	spin_unlock(&kgsl_context->kgsl_hash_lock);
	kfree(pgtbl_ctx);
}


/**
 * umd_kgsl_process_pt_alloc - Allocate a per-process page table
 * @arg: Pointer to user-space argument structure
 * @kgsl_data: Pointer to the KGSL device-specific context
 *
 * This function handles the allocation of a per-process page table
 * for a user-space client. It performs the following steps:
 * - Copies user input from user space
 * - Retrieves the SMMU domain and validates page table operations
 * - Configures and allocates a new IOMMU page table
 * - Initializes a pgtable_map structure to track the new mapping
 * - Assigns a unique page table ID and stores it in a hash table
 *
 * Return: 0 on success, -EINVAL or -EFAULT on failure
 */

static int umd_kgsl_process_pt_alloc(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct kiumd_kgsl_context *kgsl_context;
	struct io_pgtable_ops *pgtable_ops;
	struct arm_smmu_domain *smmu_dom;
	struct pgtable_map *pgtbl_ctx;
	struct kiumd_ctx *kiumd_ctx;
	struct io_pgtable *pgtable;
	struct io_pgtable_cfg cfg;
	struct kiumd_user kiusr;
	int ret;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	smmu_dom = kiumd_get_smmu_domain(kgsl_data->dev);
	if ((!smmu_dom) || (!(smmu_dom->pgtbl_ops))) {
		pr_err("%s:smmu domain/pagetable ops is invalid\n", __func__);
		return -EINVAL;
	}

	kiumd_ctx = kgsl_data->kiumdctx;
	pgtable = kiumd_ctx->pgtable;
	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	kiusr.asid = smmu_dom->cfg.asid;
	pgtable_ops = alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!pgtable_ops) {
		dev_err(kgsl_data->dev, "%s:failed to allocate pagetable ops\n", __func__);
		return -EINVAL;
	}

	kiusr.ttbr0 = cfg.arm_lpae_s1_cfg.ttbr;

	kgsl_context = kiumd_ctx->kgsl_context;
	if (kgsl_context->kgsl_pt_id == UINT_MAX) {
		dev_err(kgsl_data->dev, "%s:%d integer overflow in pt_id.\n", __func__, __LINE__);
		ret = -EINVAL;
		goto free_pgtable_ops;
	}

	pgtbl_ctx = kzalloc(sizeof(struct pgtable_map), GFP_KERNEL);
	if (!pgtbl_ctx) {
		ret = -EINVAL;
		goto free_pgtable_ops;
	}

	spin_lock_init(&pgtbl_ctx->kgsl_rbtree_lock);
	pgtbl_ctx->rbtree = RB_ROOT;
	pgtbl_ctx->ttbr0_addr = kiusr.ttbr0;
	pgtbl_ctx->start_iova = kgsl_context->kgsl_start_iova;
	pgtbl_ctx->end_iova = kgsl_context->kgsl_end_iova;
	pgtbl_ctx->pgtable_ops = pgtable_ops;
	pgtbl_ctx->last_allocated_end = pgtbl_ctx->start_iova;
	spin_lock(&kgsl_context->kgsl_hash_lock);
	pgtbl_ctx->idx = kgsl_context->kgsl_pt_id++;
	hash_add(kiumd_ctx->kgsl_page_table, &pgtbl_ctx->node, pgtbl_ctx->idx);
	spin_unlock(&kgsl_context->kgsl_hash_lock);

	kiusr.pt_id = pgtbl_ctx->idx;

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		dev_err(kgsl_data->dev, "%s: copy_to_user failed...\n", __func__);
		ret = -EINVAL;
		goto free_pt_context;
	}

	return 0;

free_pt_context:
	release_pgtbl_context(kgsl_context, pgtbl_ctx);

free_pgtable_ops:
	free_io_pgtable_ops(pgtable_ops);

	return ret;
}

/**
 * kiumd_smmuv2_write_context_bank - Configure SMMU context bank registers
 * @smmu: Pointer to the SMMU device
 * @idx: Context bank index
 *
 * Programs the context bank registers for the given index.
 */

static void kiumd_smmuv2_write_context_bank(struct arm_smmu_device *smmu, int idx)
{
	struct arm_smmu_cb *cb = &smmu->cbs[idx];
	struct arm_smmu_cfg *cfg = cb->cfg;
	bool stage1;
	u32 reg;

	stage1 = cfg->cbar != CBAR_TYPE_S2_TRANS;

	reg = (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64) ? ARM_SMMU_CBA2R_VA64 : 0;

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


/**
 * kiumd_smmuv2_set_ttbr1_cfg - Configure TTBR1 settings for the ARM SMMU
 * for a specific vfio device(as of now used by LPAC).
 * @smmu_domain: Pointer to the SMMU domain structure
 * @pgtbl_cfg: Pointer to the page table configuration
 *
 * This function enables TTBR1 translation in the SMMU and updates the
 * registers for efficient address translation.
 *
 * Return: 0 on success, negative error code on failure
 */

static int kiumd_smmuv2_set_ttbr1_cfg(struct arm_smmu_domain *smmu_domain,
						const struct io_pgtable_cfg *pgtbl_cfg)
{
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_cb *cb = &smmu_domain->smmu->cbs[cfg->cbndx];
	u32 tcr = cb->tcr[0];

	if (!(cb->tcr[0] & ARM_SMMU_TCR_EPD1)) {
		pr_err("TTBR1 translation is already enabled\n");
		return -EINVAL;
	}

	tcr |= arm_smmu_lpae_tcr(pgtbl_cfg);
	tcr &= ~(ARM_SMMU_TCR_EPD0 | ARM_SMMU_TCR_EPD1);

	cb->tcr[0] = tcr;
	cb->ttbr[1] = pgtbl_cfg->arm_lpae_s1_cfg.ttbr;
	cb->ttbr[1] |= FIELD_PREP(ARM_SMMU_TTBRn_ASID, cb->cfg->asid);

	kiumd_smmuv2_write_context_bank(smmu_domain->smmu, cb->cfg->cbndx);

	return 0;
}


/**
 * kiumd_perprocess_set_ttbr1_context - Configure TTTBR1 settings for the
 * ARM SMMU for a specific vfio device(as of now used by LPAC).
 * @arg: User-provided argument pointer
 *
 * This function allocates a pagetable and invokes the function to program
 * TTBR1 for the specified VFIO device's SMMU domain. It also configures
 * the aperture for the specified vfio device.
 *
 * Return: 0 on success, negative error code on failure
 */

static int umd_kgsl_pgtble_set_ttbr1_context(struct arm_smmu_domain *smmu_dom,
					     struct iommu_domain *iommu_dom)
{
	struct io_pgtable_ops *pgtable_ops;
	struct io_pgtable *pagetable;
	struct io_pgtable_cfg cfg;

	pagetable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if (!pagetable) {
		pr_err("%s: pagetable is NULL\n", __func__);
		return -EINVAL;
	}

	memcpy(&cfg, &pagetable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks |= IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;

	iommu_dom->geometry.aperture_start = ~0UL << cfg.ias;
	iommu_dom->geometry.aperture_end = ~0UL;

	pgtable_ops = alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!pgtable_ops) {
		pr_err("%s: failed to allocate pagetable ops\n", __func__);
		return -EINVAL;
	}

	smmu_dom->pgtbl_ops = pgtable_ops;
	if (kiumd_smmuv2_set_ttbr1_cfg(smmu_dom, &cfg) < 0) {
		pr_err("%s: failed to set TTBR1 cfg\n", __func__);
		free_io_pgtable_ops(pgtable_ops);
		return -EINVAL;
	}

	return 0;
}


/**
 * kiumd_perprocess_set_ttbr1_context - Configure TTTBR0 settings
 * for a specific vfio device(as of now used by GPU)
 * ARM SMMU for the specified device.
 * @arg: User-provided argument pointer
 *
 * This function allocates a pagetable and invokes the function to program
 * TTBR0 for the specified VFIO device's SMMU domain. It also configures
 * the aperture for the specified vfio device through an scm call.
 *
 * Return: 0 on success, negative error code on failure
 */

static int umd_kgsl_pgtble_set_ttbr0_context(struct device *dev,
					     struct arm_smmu_domain *smmu_dom,
					     struct kiumd_ctx *kiumd_ctx)
{
	struct adreno_smmu_priv *kgsl_priv;
	struct io_pgtable_ops *pgtable_ops;
	struct io_pgtable *pgtable;
	struct io_pgtable_cfg cfg;
	int ret;

	if (!kiumd_ctx->pgtable) {
		kiumd_ctx->pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
		if (!kiumd_ctx->pgtable) {
			pr_err("%s:pagetable is NULL\n", __func__);
			return -EINVAL;
		}
	}

	pgtable = kiumd_ctx->pgtable;
	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	/*Allocate a default pagetable for TTBR0 in case per process allocation fails*/
	pgtable_ops = alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!pgtable_ops) {
		pr_err("%s:failed to allocate pagetable ops.\n", __func__);
		return -ENOMEM;
	}

	kgsl_priv = dev_get_drvdata(dev);
	kgsl_priv->set_ttbr0_cfg(smmu_dom, &cfg);
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



/**
 * kiumd_set_pgtbl_context - Set the page table context for an SMMU device.
 * @arg: User-provided pointer to a struct kiumd_smmu_user containing context information
 *
 * This function sets the page table context for an SMMU device based on user-provided
 * information. It validates the VFIO file descriptor, retrieves the VFIO device,
 * and obtains the IOMMU domain. Depending on the context flags, it configures the
 * appropriate page table settings.
 *
 * Return:
 *   0 on success, negative error code on failure.
 */

static int umd_kgsl_set_pgtbl_context(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct arm_smmu_domain *smmu_dom;
	struct kiumd_smmu_user pgtbl_ctx;
	struct iommu_domain *iommu_dom;
	struct kiumd_ctx *kiumd_ctx;
	int ret;

	if (copy_from_user(&pgtbl_ctx, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	iommu_dom = kiumd_iommu_get_dma_domain(kgsl_data->dev);
	if (!iommu_dom) {
		pr_err("%s: iommu domain is NULL\n", __func__);
		return -EINVAL;
	}

	smmu_dom = kiumd_get_smmu_domain(kgsl_data->dev);
	if ((!smmu_dom) || (!(smmu_dom->pgtbl_ops))) {
		pr_err("%s:smmu domain/pagetable ops is invalid for: %d\n",
							__func__, pgtbl_ctx.flags);
		return -EINVAL;
	}

	switch (pgtbl_ctx.flags) {
	case KIUMD_SMMU_SET_TTBR0_CONFIG:
		kiumd_ctx = kgsl_data->kiumdctx;
		ret = umd_kgsl_pgtble_set_ttbr0_context(kgsl_data->dev, smmu_dom, kiumd_ctx);
		break;
	case KIUMD_SMMU_SET_TTBR1_CONFIG:
		ret = umd_kgsl_pgtble_set_ttbr1_context(smmu_dom, iommu_dom);
		break;
	default:
		pr_err("%s: Invalid flags: %d\n", __func__, pgtbl_ctx.flags);
		ret = -ENOTTY;
		break;
	}

	return ret;
}

/**
 * umd_kgsl_global_pgtble_set - Set global page table operations
 * @kgsl_data: Pointer to KGSL driver-specific context data
 *
 * This function initializes and sets the global page table operations
 * for the GPU driver. It retrieves the SMMU domain associated with the
 * device and ensures that the page table is allocated and valid. If the
 * page table is not already initialized, it is created from the existing
 * SMMU page table operations. The resulting page table operations are
 * then assigned to the SMMU domain.
 *
 * Return: 0 on success, error number on failure
 */

static int umd_kgsl_global_pgtble_set(struct umd_kgsl_data *kgsl_data)
{
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable_ops *pgtbl_ops;
	struct kiumd_ctx *kiumd_ctx;
	struct io_pgtable *pgtable;

	smmu_dom = kiumd_get_smmu_domain(kgsl_data->dev);
	if (!smmu_dom) {
		pr_err("%s:SMMU domain is NULL\n", __func__);
		return -ENOMEM;
	}

	kiumd_ctx = kgsl_data->kiumdctx;
	if (!kiumd_ctx->pgtable) {
		kiumd_ctx->pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
		if (!kiumd_ctx->pgtable) {
			dev_err(kgsl_data->dev, "%s:pagetable is NULL\n", __func__);
			return -EINVAL;
		}
	}

	pgtable = kiumd_ctx->pgtable;
	pgtbl_ops = (struct io_pgtable_ops *) (&pgtable->ops);
	if (!pgtbl_ops) {
		dev_err(kgsl_data->dev, "%s:pagetable ops is NULL\n", __func__);
		return -ENOMEM;
	}

	smmu_dom->pgtbl_ops = pgtbl_ops;

	return 0;
}

/**
 * umd_kgsl_perprocess_pgtble_set - Set per-process page table operations
 * @arg: Pointer to user-space structure containing page table ID
 * @kgsl_data: Pointer to KGSL driver-specific context data
 *
 * This function sets the page table operations for a specific process
 * context in the GPU driver. It copies the user-provided data from
 * user space, retrieves the corresponding SMMU domain and page table
 * context, and assigns the page table operations to the SMMU domain.
 *
 * Return: 0 on success, -EFAULT if user data copy fails,
 *         -EINVAL if the SMMU domain, page table context, or
 *         page table operations are invalid.
 */

static int umd_kgsl_perprocess_pgtble_set(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct io_pgtable_ops *pgtable_ops;
	struct arm_smmu_domain *smmu_dom;
	struct pgtable_map *pgtble_ctx;
	struct kiumd_ctx *kiumd_ctx;
	struct kiumd_user kiusr;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	kiumd_ctx = kgsl_data->kiumdctx;
	smmu_dom = kiumd_get_smmu_domain(kgsl_data->dev);
	if (!smmu_dom) {
		pr_err("%s:invalid SMMU domain\n", __func__);
		return -EINVAL;
	}

	pgtble_ctx = kgsl_get_pgtable_entry(kiumd_ctx->kgsl_context, kgsl_data->kiumdctx,
										kiusr.pt_id);
	if (!pgtble_ctx) {
		dev_err(kgsl_data->dev, "%s:%d Invalid id for hash table: id: %d\n",
								__func__, __LINE__, kiusr.pt_id);
		return -EINVAL;
	}

	pgtable_ops = pgtble_ctx->pgtable_ops;
	if (!pgtable_ops) {
		dev_err(kgsl_data->dev, "%s:invalid pagetable ops\n", __func__);
		return -EINVAL;
	}

	smmu_dom->pgtbl_ops = pgtable_ops;

	return 0;
}

/**
 * umd_kgsl_process_pgtble_free - Free per-process page table operations
 * @arg: Pointer to user-space argument structure
 * @kgsl_data: Pointer to the KGSL device-specific context
 *
 * This function retrieves the page table context associated with the
 * provided page table ID and frees the associated IOMMU page table
 * operations. It performs validation on the KGSL context, the page
 * table entry, and the page table operations before freeing resources.
 *
 * Return: 0 on success, or a negative errno on failure
 */

static int umd_kgsl_process_pgtble_free(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct kiumd_kgsl_context *kgsl_context;
	struct io_pgtable_ops *pgtbl_ops;
	struct pgtable_map *pgtble_ctx;
	struct kiumd_ctx *kiumd_ctx;
	struct kiumd_user kiusr;
	struct device *dev;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	kiumd_ctx = kgsl_data->kiumdctx;
	kgsl_context = kiumd_ctx->kgsl_context;
	if (!kgsl_context) {
		dev_err(kgsl_data->dev, "%s:kgsl context invalid\n", __func__);
		return -EINVAL;
	}

	pgtble_ctx = kgsl_get_pgtable_entry(kgsl_context, kiumd_ctx, kiusr.pt_id);
	if (!pgtble_ctx) {
		dev_err(kgsl_data->dev, "%s:%d Invalid id for hash table: id: %d\n",
								__func__, __LINE__, kiusr.pt_id);
		return -EINVAL;
	}

	dev = kgsl_data->dev;
	if (!check_pgtable_context(dev, pgtble_ctx)) {
		dev_err(kgsl_data->dev, "%s:%d check_pgtable_context failed\n", __func__, __LINE__);
		return -EINVAL;
	}

	pgtbl_ops = pgtble_ctx->pgtable_ops;
	if (!pgtbl_ops) {
		dev_err(kgsl_data->dev, "%s:pagegetable ops is NULL\n", __func__);
		return -EINVAL;
	}

	free_io_pgtable_ops(pgtbl_ops);
	release_pgtbl_context(kgsl_context, pgtble_ctx);

	return 0;
}


/**
 * umd_kgsl_manage_runtime_pm - Handle runtime PM for GPU
 *
 * Parameters:
 * @arg: User pointer to struct kiumd_user with VFIO fd and PM state
 *
 * Return: 0 on success or a negative error code on failure.
 */

static int umd_kgsl_manage_runtime_pm(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct kiumd_user kiusr;
	struct device *dev;
	int ret;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	dev = kgsl_data->dev;
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


static int umd_kgsl_plat_dev_init(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct irqinfo_user irquser;

	if (kiumd_iommu_custom_iova_init(kgsl_data->dev)) {
		dev_err(kgsl_data->dev, "%s: failed to initialize custom iova\n", __func__);
		return -EINVAL;
	}

	irquser.num_irqs = kgsl_data->num_irqs;
	if (copy_to_user(arg, &irquser, sizeof(irquser)))
		return -EFAULT;

	return 0;
}

static int umd_platform_fixed_iova_control(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct kiumd_iova iovausr;
	int cookie_type, ret;
	dma_addr_t iova_usr = 0;

	if (copy_from_user(&iovausr, arg, sizeof(struct kiumd_iova)))
		return -EFAULT;

	if (iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_CLEAR)
		cookie_type = IOMMU_DMA_IOVA_COOKIE;
	else
		cookie_type = IOMMU_DMA_MSI_COOKIE;

	if (iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_SET) {
		cookie_type = IOMMU_DMA_MSI_COOKIE;
		iova_usr = iovausr.iova;
	}

	ret = kiumd_configure_dma_cookie(kgsl_data->dev, cookie_type, iova_usr);
	if (ret)
		pr_err("%s failed to set fixed iova\n", __func__);

	return ret;
}

static int umd_kgsl_mmio_smmu_map(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct kiumd_smmu_mmio_ctx *mmio_ctx;
	struct kiumd_smmu_mmio_map kiusr;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct resource *res;
	int retval = 0;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_smmu_mmio_map)))
		return -EFAULT;

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap)
		return -ENOMEM;

	mmio_ctx = kzalloc(sizeof(*mmio_ctx), GFP_KERNEL);
	if (!mmio_ctx) {
		retval = -ENOMEM;
		goto free_smap;
	}

	if (kgsl_data->num_regs < 0 || kiusr.reg_idx >= kgsl_data->num_regs) {
		pr_err("%s:%d invalid reg index from userspace: %d\n", __func__,
								__LINE__, kiusr.reg_idx);
		retval = -EINVAL;
		goto free_mmio_ctx;
	}

	res = &kgsl_data->resources[kiusr.reg_idx];
	if (!res) {
		pr_err("%s:%d resource error\n", __func__, __LINE__);
		retval = -EINVAL;
		goto free_mmio_ctx;
	}

	retval = kiumd_mmio_iommu_map(&kiusr, kgsl_data->dev, mmio_ctx, res);
	if (retval) {
		dev_err(kgsl_data->dev,
			"%s:mmio_iommu map failed for reg id: %d, fixed iova: %llx\n",
							__func__, kiusr.reg_idx, kiusr.iova);
		retval = -EINVAL;
		goto free_mmio_ctx;
	}

	kiumd_ctx = kgsl_data->kiumdctx;
	spin_lock(&kiumd_ctx->smmu_lock);
	smap->id = kiumd_ctx->id++;
	kiusr.id = smap->id;
	hash_add(kiumd_ctx->smmu_table, &smap->node, smap->id);
	smap->context = mmio_ctx;
	spin_unlock(&kiumd_ctx->smmu_lock);

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("kiumd:error in copying data\n");
		retval = -EFAULT;
		goto release_map;
	}

	return 0;

release_map:
	release_map_data(kiumd_ctx, smap);
	kfree(mmio_ctx);
	return retval;

free_mmio_ctx:
	kfree(mmio_ctx);

free_smap:
	kfree(smap);

	return retval;
}

static int umd_kgsl_dmabuf_map(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct kiumd_kgsl_context *kgsl_context;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	struct device *dev;
	int size, ret = 0;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	dev = kgsl_data->dev;
	kiumd_ctx = kgsl_data->kiumdctx;

	kgsl_context = kiumd_ctx->kgsl_context;
	if (!kgsl_context)
		return -EINVAL;

	size = kiumd_get_dmabuf_size(kiusr.dma_buf_fd);
	if (!size)
		return -EINVAL;

	smap = allocate_init_smap(kiusr, dev, size);
	if (!smap)
		return -ENOMEM;

	smap->dmabuf_ptr = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(smap->dmabuf_ptr)) {
		ret = -EBADF;
		goto smap_free;
	}

	ret = set_kgsl_map_iova(kgsl_addr_cache, kiumd_ctx, kiusr, smap);
	if (ret) {
		ret = -EINVAL;
		goto dmabuf_put;
	}

	if (KIUSR_IS_IOVA_ZERO(kiusr))
		ret = kiumd_dmabuf_zero_map(smap);
	else if (KIUSR_IS_PRIV(kiusr))
		ret = kiumd_dmabuf_priv_map(smap);
	else
		ret = kiumd_dmabuf_map(smap);

	if (ret)
		goto dmabuf_put;

	add_to_smmu_table(kiumd_ctx, smap);

	kiusr.id = smap->id;
	if (smap->is_iova_zero)
		kiusr.dma_addr = (unsigned long) IOVA_ZERO;
	else
		kiusr.dma_addr = (unsigned long) sg_dma_address(smap->sgt_ptr->sgl);

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		ret = -EFAULT;
		goto clean_map_res;
	}

	return 0;

clean_map_res:
	clean_map(kgsl_addr_cache, kiumd_ctx, smap);
dmabuf_put:
	dma_buf_put(smap->dmabuf_ptr);
smap_free:
	kfree(smap);

	return ret;
}

static int umd_kgsl_dmabuf_unmap(char __user *arg, struct umd_kgsl_data *kgsl_data)
{
	struct smmu_map_data *smap __free(kfree) = NULL;
	struct iommu_domain *iommu_dom;
	struct kiumd_ctx *kiumd_ctx;
	struct kiumd_user kiusr;
	unsigned long dma_addr;
	bool found = false;
	struct device *dev;
	int ret;

	dev = kgsl_data->dev;
	kiumd_ctx = kgsl_data->kiumdctx;

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

	dma_addr = smap->is_iova_zero ?
			IOVA_ZERO : sg_dma_address(smap->sgt_ptr->sgl);

	if (smap->is_fixed_map) {
		ret = kiumd_configure_dma_cookie(smap->dev, IOMMU_DMA_MSI_COOKIE,
									dma_addr);
		if (ret)
			return -ENODEV;
	}

	if (smap->is_kgsl_ctx) {
		ret = clear_kgsl_map_iova(kgsl_addr_cache, kiumd_ctx, smap);
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

static long umd_kgsl_ioctl(struct file *file, unsigned int cmd, unsigned long argp)
{
	struct umd_kgsl_data *kgsl_data;
	int eventfd, irq_index, ret = 0;
	struct irqinfo_user irquser;
	char __user *arg = (char __user *)argp;

	kgsl_data = GET_KGSL_DATA(file);
	switch (cmd) {

	case IOCTL_PLAT_DEV_INIT:
		ret = umd_kgsl_plat_dev_init(arg, kgsl_data);
		if (ret)
			return ret;
		break;

	case IOCTL_REGISTER_EVENTFD:
		if (copy_from_user(&irquser, (int __user *)argp, sizeof(struct irqinfo_user)))
			return -EFAULT;

		irq_index = irquser.irq_index;
		eventfd = irquser.event_fd;

		ret = umd_kgsl_register_eventfd(kgsl_data, eventfd, irq_index);
		if (ret)
			return ret;
		break;

	case IOCTL_UNMASK_INTERRUPT:
		irq_index = (int) argp;
		ret = umd_kgsl_unmask_interrupt(kgsl_data, irq_index);
		if (ret)
			return ret;
		break;

	case IOCTL_DMABUF_MAP:
		ret = umd_kgsl_dmabuf_map(arg, kgsl_data);
		if (ret)
			return ret;
		break;

	case IOCTL_DMABUF_UNMAP:
		ret = umd_kgsl_dmabuf_unmap(arg, kgsl_data);
		if (ret)
			return ret;
		break;

	case IOCTL_SMMU_MMIO_MAP:
		ret = umd_kgsl_mmio_smmu_map(arg, kgsl_data);
		if (ret)
			return ret;
		break;

	case IOCTL_FIXED_IOVA_CTRL:
		ret = umd_platform_fixed_iova_control(arg, kgsl_data);
		if (ret)
			return ret;

		break;

	case IOCTL_SET_PGTBL_CONTEXT:
		ret = umd_kgsl_set_pgtbl_context(arg, kgsl_data);
		if (ret)
			return ret;

		break;

	case IOCTL_PROCESS_PGTBL_SET:
		ret = umd_kgsl_perprocess_pgtble_set(arg, kgsl_data);
		if (ret)
			return ret;

		break;

	case IOCTL_PER_PROCESS_ALLOC:
		ret = umd_kgsl_process_pt_alloc(arg, kgsl_data);
		if (ret)
			return ret;

		break;

	case IOCTL_GLOBAL_PGTABLE_SET:
		ret = umd_kgsl_global_pgtble_set(kgsl_data);
		if (ret)
			return ret;

		break;

	case IOCTL_PROCESS_PGTABLE_FREE:
		ret = umd_kgsl_process_pgtble_free(arg, kgsl_data);
		if (ret)
			return ret;

		break;

	case IOCTL_MANAGE_RUNTIME_PM:
		ret = umd_kgsl_manage_runtime_pm(arg, kgsl_data);
		if (ret)
			return ret;

		break;

	default:
		ret = -EINVAL;
	}

	return ret;
}

static int umd_kgsl_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pgoff = vma->vm_pgoff;
	struct umd_kgsl_data *kgsl_data;
	struct resource *res;

	kgsl_data = GET_KGSL_DATA(filp);
	if (pgoff >= kgsl_data->num_regs)
		return -EINVAL;

	res = &kgsl_data->resources[pgoff];
	if (size > resource_size(res))
		return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (remap_pfn_range(vma, vma->vm_start, res->start >> PAGE_SHIFT, size, vma->vm_page_prot))
		return -EAGAIN;

	pr_info("Mapped register index %lu, address: 0x%llx, size: 0x%lx\n", pgoff,
							(unsigned long long)res->start, size);
	return 0;
}

static ssize_t fault_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct kgsl_fault_info *fault_info;
	unsigned long irq_flags;
	unsigned long fault_count;
	unsigned long pgtable_id;
	unsigned long iova;
	u32 fsr;

	fault_info = container_of(attr, struct kgsl_fault_info, attr_info);
	spin_lock_irqsave(&fault_info->lock, irq_flags);
	fsr = fault_info->fsr;
	iova = fault_info->iova;
	pgtable_id = fault_info->pgtable_id;
	fault_count = fault_info->fault_count;
	spin_unlock_irqrestore(&fault_info->lock, irq_flags);

	return sysfs_emit(buf, "fsr=0x%x iova=0x%lx pgtable_id=%lu fault_count=%lu\n",
			  fsr, iova, pgtable_id, fault_count);
}

unsigned long kgsl_get_pgtable_identifier(struct kiumd_ctx *kiumd_ctx, u64 ttbr0)
{
	struct kiumd_kgsl_context *kgsl_context;
	unsigned long bkt, ret = -EINVAL;
	struct pgtable_map *pgtbl_ctx;

	kgsl_context = kiumd_ctx->kgsl_context;
	if (!kgsl_context)
		return ret;

	spin_lock(&kgsl_context->kgsl_hash_lock);
	hash_for_each(kiumd_ctx->kgsl_page_table, bkt, pgtbl_ctx, node) {
		if (pgtbl_ctx->ttbr0_addr == ttbr0) {
			ret = pgtbl_ctx->idx;
			break;
		}
	}

	spin_unlock(&kgsl_context->kgsl_hash_lock);

	return ret;
}

static int umd_kgsl_iommu_fault_handler(struct iommu_domain *domain, struct device *dev,
							unsigned long iova, int flags, void *token)
{
	struct umd_kgsl_data *kgsl_data = token;
	unsigned long irq_flags, ttbr0, pt_id;
	struct arm_smmu_domain *smmu_domain;
	struct kgsl_fault_info *fault_info;
	struct kernfs_node *fs_node;
	struct arm_smmu_cfg *cfg;
	u32 fsr;

	if (!kgsl_data || !kgsl_data->fault_info)
		return -EINVAL;

	smmu_domain = container_of(domain, struct arm_smmu_domain, domain);
	cfg = &smmu_domain->cfg;
	ttbr0 = arm_smmu_cb_readq(smmu_domain->smmu, cfg->cbndx, ARM_SMMU_CB_TTBR0);

	pt_id = kgsl_get_pgtable_identifier(kgsl_data->kiumdctx, ttbr0);
	fsr = arm_smmu_cb_read(smmu_domain->smmu, cfg->cbndx, ARM_SMMU_CB_FSR);
	fault_info = kgsl_data->fault_info;
	spin_lock_irqsave(&fault_info->lock, irq_flags);
	fault_info->iova = iova;
	fault_info->fsr = fsr;
	fault_info->pgtable_id = pt_id;
	fault_info->fault_count++;
	fs_node = fault_info->fs_node;
	spin_unlock_irqrestore(&fault_info->lock, irq_flags);

	if (fault_info->fs_node)
		sysfs_notify_dirent(fs_node);
	else
		dev_err(dev, "IOMMU fault: sysfs notify failed\n");

	dev_warn(dev, "IOMMU fault: iova=0x%lx flags=0x%x fsr=0x%x pgtableid: %ld\n",
				iova, flags, fsr, pt_id);

	return 0;
}

static int umd_kgsl_init_fault_handler(struct umd_kgsl_data *kgsl_data)
{
	struct kgsl_fault_info *fault_info = kgsl_data->fault_info;
	struct iommu_domain *domain;
	int ret;

	domain = kiumd_iommu_get_dma_domain(kgsl_data->dev);
	if (!domain) {
		dev_err(kgsl_data->dev, "No IOMMU domain attached\n");
		return -ENODEV;
	}

	iommu_set_fault_handler(domain, umd_kgsl_iommu_fault_handler, kgsl_data);

	spin_lock_init(&fault_info->lock);
	fault_info->attr_info.attr.name = "fault_info";
	fault_info->attr_info.attr.mode = 0444;
	fault_info->attr_info.show = fault_info_show;
	fault_info->attr_info.store = NULL;
	fault_info->fsr = 0;
	fault_info->iova = 0;
	fault_info->pgtable_id = 0;
	fault_info->fault_count = 0;

	ret = device_create_file(kgsl_data->dev, &fault_info->attr_info);
	if (ret) {
		dev_err(kgsl_data->dev, "failed to create sysfs file in /sys/kernel/\n");
		return ret;
	}

	fault_info->fs_node = sysfs_get_dirent(kgsl_data->dev->kobj.sd, "fault_info");
	if (!fault_info->fs_node) {
		device_remove_file(kgsl_data->dev, &fault_info->attr_info);
		return -ENOENT;
	}

	return 0;
}

static int umd_kgsl_open(struct inode *inode, struct file *file)
{
	struct umd_kgsl_data *kgsl_data = GET_KGSL_DATA(file);

	dev_info(kgsl_data->dev, "Device %s opened successfully with minor: %d\n",
						kgsl_data->mdev.name, MINOR(inode->i_rdev));
	return 0;
}

static int umd_kgsl_release(struct inode *inode, struct file *file)
{
	struct umd_kgsl_data *kgsl_data;
	struct irq_context *ctx;
	int i;

	kgsl_data = GET_KGSL_DATA(file);
	for (i = 0; i < kgsl_data->num_irqs; i++) {
		ctx = &kgsl_data->irq_ctx[i];
		if (ctx->eventfd_ctx) {
			eventfd_ctx_put(ctx->eventfd_ctx);
			ctx->eventfd_ctx = NULL;
		}
	}

	return 0;
}


static const struct file_operations umd_kgsl_fops = {
	.owner = THIS_MODULE,
	.open = umd_kgsl_open,
	.unlocked_ioctl = umd_kgsl_ioctl,
	.release = umd_kgsl_release,
	.mmap = umd_kgsl_mmap,
};

static int populate_resources(struct platform_device *pdev, struct umd_kgsl_data *kgsl_data)
{
	struct device_node *np = pdev->dev.of_node;
	int len, i, na, ns, num_regions;
	struct resource *resources;
	const char *reg_name;
	const __be32 *addrp;

	na = of_n_addr_cells(np);
	ns = of_n_size_cells(np);

	if (na <= 0 || ns <= 0) {
		dev_err(&pdev->dev, "Invalid #address-cells or #size-cells\n");
		return -EINVAL;
	}

	addrp = of_get_property(np, "reg", &len);
	if (!addrp) {
		dev_err(&pdev->dev, "No 'reg' property found\n");
		return -EINVAL;
	}

	num_regions = len / ((na + ns) * sizeof(u32));
	if (num_regions <= 0) {
		dev_err(&pdev->dev, "Invalid 'reg' property length\n");
		return -EINVAL;
	}

	resources = devm_kzalloc(&pdev->dev, num_regions * sizeof(*resources), GFP_KERNEL);
	if (!resources)
		return -ENOMEM;

	for (i = 0; i < num_regions; i++) {
		u64 address, size;

		address = of_read_number(addrp, na);
		addrp += na;
		size = of_read_number(addrp, ns);
		addrp += ns;

		if (of_property_read_string_index(np, "reg-names", i, &reg_name)) {
			dev_err(&pdev->dev, "Failed to get reg-names for resource %d\n", i);
			return -EINVAL;
		}

		resources[i].start = address;
		resources[i].end = address + size - 1;
		resources[i].flags = IORESOURCE_MEM;
		resources[i].name = reg_name;

		dev_info(&pdev->dev, "Resource %d: start = 0x%llx, size = 0x%llx, name = %s\n",
				i, (unsigned long long)address, (unsigned long long)size, reg_name);
	}

	pdev->resource = resources;
	pdev->num_resources = num_regions;

	kgsl_data->resources = resources;
	kgsl_data->num_regs = num_regions;
	return 0;
}

static int allocate_kgsl_data_memory(struct umd_kgsl_data *kgsl_data,
					struct platform_device *child_pdev,
					int num_irqs)
{
	struct device *dev = &child_pdev->dev;

	if (num_irqs > 0) {
		kgsl_data->irq_ctx = devm_kzalloc(dev,
						sizeof(struct irq_context) * num_irqs, GFP_KERNEL);
		if (!kgsl_data->irq_ctx)
			return -ENOMEM;
	}

	kgsl_data->fault_info = devm_kzalloc(dev, sizeof(struct kgsl_fault_info), GFP_KERNEL);
	if (!kgsl_data->fault_info)
		return -ENOMEM;

	kgsl_data->kiumdctx = devm_kzalloc(dev, sizeof(struct kiumd_ctx), GFP_KERNEL);
	if (!kgsl_data->kiumdctx)
		return -ENOMEM;

	kgsl_data->kiumdctx->kgsl_context = devm_kzalloc(dev,
						sizeof(struct kiumd_kgsl_context), GFP_KERNEL);
	if (!kgsl_data->kiumdctx->kgsl_context)
		return -ENOMEM;

	return 0;
}


static int umd_kgsl_init(struct device *parent_dev, struct device_node *child_np)
{
	struct kiumd_kgsl_context *kgsl_context;
	struct platform_device *child_pdev;
	struct adreno_smmu_priv *kgsl_priv;
	struct umd_kgsl_data *kgsl_data;
	const char *dev_name_from_dt;
	struct kiumd_ctx *kiumd_ctx;
	int ret, i, num_irqs;

	child_pdev = of_platform_device_create(child_np, NULL, parent_dev);
	if (!child_pdev)
		return dev_err_probe(parent_dev, -ENODEV, "Failed to create child pdev\n");

	kgsl_data = devm_kzalloc(&child_pdev->dev, sizeof(*kgsl_data), GFP_KERNEL);
	if (!kgsl_data)
		return -ENOMEM;

	kgsl_data->pdev = child_pdev;
	kgsl_data->dev = &child_pdev->dev;

	ret = populate_resources(child_pdev, kgsl_data);
	if (ret)
		return dev_err_probe(&child_pdev->dev, ret, "Failed to populate resources\n");

	num_irqs = of_property_count_strings(child_np, "interrupt-names");
	if (num_irqs > 0)
		kgsl_data->num_irqs = num_irqs;
	else
		kgsl_data->num_irqs = 0;

	ret = allocate_kgsl_data_memory(kgsl_data, child_pdev, num_irqs);
	if (ret)
		return dev_err_probe(&child_pdev->dev, ret, "Memory allocation failed\n");


	kiumd_ctx = kgsl_data->kiumdctx;
	kgsl_context = kiumd_ctx->kgsl_context;

	kgsl_context->kgsl_start_iova = KGSL_PER_PROCESS_PT_BASE_IOVA;
	kgsl_context->kgsl_end_iova   = KGSL_PER_PROCESS_PT_END_IOVA;

	spin_lock_init(&kiumd_ctx->smmu_lock);
	spin_lock_init(&kgsl_context->kgsl_hash_lock);

	for (i = 0; i < num_irqs; i++) {
		struct irq_context *irqc = &kgsl_data->irq_ctx[i];

		irqc->hwirq = platform_get_irq(child_pdev, i);
		if (irqc->hwirq < 0) {
			dev_err(&child_pdev->dev, "Failed to get interrupt %d\n", i);
			return irqc->hwirq;
		}

		if (of_property_read_string_index(child_np, "interrupt-names",
							i, (const char **)&irqc->irq_name))
			return dev_err_probe(&child_pdev->dev, -EINVAL,
						"Failed to get interrupt name for irq %d\n", i);

		ret = devm_request_irq(&child_pdev->dev, irqc->hwirq, umd_kgsl_irq_handler,
				   IRQF_NO_AUTOEN, irqc->irq_name, irqc);
		if (ret)
			return dev_err_probe(&child_pdev->dev, ret,
				"Failed to register interrupt handler for IRQ %d\n", irqc->hwirq);
	}

	if (of_property_read_string(child_np, "name", &dev_name_from_dt))
		return dev_err_probe(&child_pdev->dev, -EINVAL, "No 'name' property in DT\n");

	kgsl_data->mdev.minor = MISC_DYNAMIC_MINOR;
	kgsl_data->mdev.name = devm_kasprintf(&child_pdev->dev, GFP_KERNEL, "rgs/%s",
									dev_name_from_dt);
	kgsl_data->mdev.fops = &umd_kgsl_fops;
	kgsl_data->mdev.parent = parent_dev;

	kgsl_priv = devm_kzalloc(&child_pdev->dev, sizeof(*kgsl_priv), GFP_KERNEL);
	if (!kgsl_priv)
		return -ENOMEM;

	dev_set_drvdata(&child_pdev->dev, kgsl_priv);
	if (of_device_is_compatible(child_np, "qcom,adreno-device"))
		ret = dma_set_coherent_mask(&child_pdev->dev, DMA_BIT_MASK(64));
	else
		ret = dma_set_coherent_mask(&child_pdev->dev, DMA_BIT_MASK(32));

	if (ret)
		return dev_err_probe(&child_pdev->dev, ret,
				"Failed to set coherent DMA mask for child device\n");

	ret = of_dma_configure(&child_pdev->dev, child_np, true);
	if (ret)
		return dev_err_probe(&child_pdev->dev, ret,
					"Failed to configure DMA for child device\n");

	ret = umd_kgsl_init_fault_handler(kgsl_data);
	if (ret)
		return dev_err_probe(&child_pdev->dev, ret,
						"Failed to init fault handler\n");

	dev_info(&child_pdev->dev, "Child device initialized with resources and interrupts\n");
	return 0;
}


static int gpu_parent_probe(struct platform_device *pdev)
{
	struct device_node *child_np;
	int ret;

	for_each_available_child_of_node(pdev->dev.of_node, child_np) {
		if (of_device_is_compatible(child_np, "qcom,gpu-umd-platform")) {
			ret = umd_kgsl_init(&pdev->dev, child_np);
			if (ret) {
				of_node_put(child_np);
				return dev_err_probe(&pdev->dev, ret,
						"Failed to initialize child device with error\n");
			}
		}
	}

	kgsl_addr_cache = kmem_cache_create("kgsl_iommu_addr_cache",
						sizeof(struct iommu_addr_entry), 0, 0, NULL);
	if (!kgsl_addr_cache)
		return dev_err_probe(&pdev->dev, -ENOMEM, "kiumd kmem cache creation failed\n");

	return 0;
}

static int gpu_parent_remove(struct platform_device *pdev)
{
	kmem_cache_destroy(kgsl_addr_cache);

	dev_info(&pdev->dev, "gpu_parent device removed\n");
	return 0;
}

static const struct of_device_id gpu_parent_of_match[] = {
	{ .compatible = "gpu-parent-device", },
	{},
};
MODULE_DEVICE_TABLE(of, gpu_parent_of_match);

static struct platform_driver gpu_parent_driver = {
	.driver = {
		.name = "gpu_parent",
		.of_match_table = gpu_parent_of_match,
	},
	.probe = gpu_parent_probe,
	.remove = gpu_parent_remove,
};

module_platform_driver(gpu_parent_driver);

MODULE_IMPORT_NS(DMA_BUF);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KGSL platform driver for interrupts, memory, iommu and DMA configuration");

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/completion.h>
#include <linux/dev_printk.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/types.h>

#define CREATE_TRACE_POINTS
#include "arm-smmu-qcom-fusa.h"

#define FUSA_TCU_ERROR_INJECT_REGISTER	0x18
#define FUSA_QTC_INTSTS_REGISTER	0x8

#define FUSA_TCU_IRQ_SET_REGISTER	0x10
#define FUSA_TCU_ERR_MASK_SAIL		0x8
#define FUSA_TCU_WRN_MASK_SAIL		0xC
#define FUSA_TBU_ERROR_INJECT_REGISTER	0x30
#define FUSA_TBU_IRQ_SET_REGISTER	0x20
#define FUSA_TBU_ERR_MASK_SAIL		0x10
#define FUSA_TBU_WRN_MASK_SAIL		0x18

#define TCU_SRAM_PARITY_OFFSET		0x3
#define TBU_SRAM_PARITY_OFFSET		0x2
#define CSR_PARITY_OFFSET		0x1
#define ARR_2D_OFFSET			0x0

#define PA_ERR				0x4
#define VA_ERR				0x3
#define TBU_TCU_LINK_ERR		0x2
#define WRBUF_ERR			0x1
#define WRBUF_WRN			0x0

#define FUSA_TCU500_NUM_ERR		0x4
#define FUSA_TBU500_NUM_ERR		0x5

#define FUSA_INTSTS_PFB_2D_ARR		0x1
#define FUSA_INTSTS_CSR_PARITY		0x2
#define FUSA_INTSTS_TBU_SRAM_PARITY	0x4
#define FUSA_INTSTS_TCU_SRAM_PARITY	0x8

#define FUSA_INTSTS_WRBUF_WARN		0x1
#define FUSA_INTSTS_WRBUF_ERR		0x2
#define FUSA_INTSTS_TBU_TCU_ERR		0x4
#define FUSA_INTSTS_VA_ERR		0x8
#define FUSA_INTSTS_PA_ERR		0x10

#define FUSA_INTSTS_TBU_TCU_ERR_CLR	0x0

#define FUSA_TBU500_OFFSET		0x4000

#define FUSA_INTSTS_SPURIOUS		0x0
#define FUSA_WARNING			0x1
#define FUSA_ERROR			0x2

#define BUFFER_SZ			64
#define USECASE_SWITCH_TIMEOUT_MSECS	(500)

static bool smmu_fusa_inj_only_one = true;
module_param(smmu_fusa_inj_only_one, bool, 0644);

static bool smmu_fusa_only_one_client = true;
module_param(smmu_fusa_only_one_client, bool, 0644);

#define QSMMU_F_TBU500			BIT(1)
#define QSMMU_F_QTB500			BIT(1) /* QTB500 uses same fault path as TBU500 */
#define QSMMU_F_QTB600			BIT(2)

struct qsmmu_fusa_match_data {
	u32 offset;
	u32 flags;
	bool enable_fault_injection;
};

struct qcom_smmu_safety_fault {
	char *fault_source;
	u64 fault_code;
#ifdef CONFIG_DEBUG_FS
	bool fault_src_ready;
	bool fault_code_ready;
#endif
};

struct qsmmu_fusa_test {
	/* for usecase under test */
	struct device *test_dev;
	struct iommu_domain *domain;
	/* Protects test_dev */
	struct mutex state_lock;
	/* For waiting for child probe to complete */
	struct completion probe_wait;
};

struct qsmmu_fusa {
	struct device *dev;
	const char *smmu_name;
	u32 num_clients;
	u32 client_offset;
	u32 client_fusa_reg;
	size_t client_fusa_sz;
	void __iomem *tcu_fusa_base;
	void __iomem *client_fusa_base;
	struct dentry *qcom_smmu_dir;
	struct qcom_smmu_safety_fault hw_fault;
	spinlock_t lock;
	struct qsmmu_fusa_test *tdev;
	bool tbu_fault_injection;
#ifdef CONFIG_DEBUG_FS
	wait_queue_head_t wq;
#endif
	const struct qsmmu_fusa_match_data *md;
};

#ifdef CONFIG_DEBUG_FS
static ssize_t fault_source_read(struct file *filep,
                                 char __user *userbuf, size_t count,
                                 loff_t *ppos)
{
	struct qsmmu_fusa *qsmmu_fusa = filep->private_data;
	char buf[BUFFER_SZ];
	u32 len;

	len = scnprintf(buf, sizeof(buf), "%s\n", qsmmu_fusa->hw_fault.fault_source);

	return simple_read_from_buffer(userbuf, count, ppos, buf, len);
}

static ssize_t fault_code_read(struct file *filep, char __user *userbuf,
                               size_t count, loff_t *ppos)
{
	struct qsmmu_fusa *qsmmu_fusa = filep->private_data;
	char buf[BUFFER_SZ];
	u32 len;

	len = scnprintf(buf, sizeof(buf), "0x%llx\n", qsmmu_fusa->hw_fault.fault_code);

	return simple_read_from_buffer(userbuf, count, ppos, buf, len);
}

static __poll_t fault_src_notify(struct file *filep, poll_table *wait)
{
	struct qsmmu_fusa *qsmmu_fusa = filep->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(filep, &qsmmu_fusa->wq, wait);

	spin_lock_irqsave(&qsmmu_fusa->lock, flags);
	if (qsmmu_fusa->hw_fault.fault_src_ready) {
		qsmmu_fusa->hw_fault.fault_src_ready = false;
		mask = EPOLLIN | EPOLLRDNORM;
	}
	spin_unlock_irqrestore(&qsmmu_fusa->lock, flags);

	return mask;
}

static __poll_t fault_code_notify(struct file *filep, poll_table *wait)
{
	struct qsmmu_fusa *qsmmu_fusa = filep->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(filep, &qsmmu_fusa->wq, wait);

	spin_lock_irqsave(&qsmmu_fusa->lock, flags);
	if (qsmmu_fusa->hw_fault.fault_code_ready) {
		qsmmu_fusa->hw_fault.fault_code_ready = false;
		mask = EPOLLIN | EPOLLRDNORM;
	}
	spin_unlock_irqrestore(&qsmmu_fusa->lock, flags);

	return mask;
}

static const struct file_operations fault_source_ops = {
	.open = simple_open,
	.read = fault_source_read,
	.poll = fault_src_notify,
};

static const struct file_operations fault_code_ops = {
	.open = simple_open,
	.read = fault_code_read,
	.poll = fault_code_notify,
};

static void qcom_smmu_create_debug_dir(struct qsmmu_fusa *qsmmu_fusa)
{
	char file_name[BUFFER_SZ];

	qsmmu_fusa->qcom_smmu_dir = debugfs_lookup("smmu_hw_faults", NULL);

	if (!qsmmu_fusa->qcom_smmu_dir)
		qsmmu_fusa->qcom_smmu_dir = debugfs_create_dir("smmu_hw_faults", NULL);

	if (IS_ERR(qsmmu_fusa->qcom_smmu_dir))
		return;

	/* Create file to o/p SMMU FuSa fault source */
	scnprintf(file_name, sizeof(file_name), "%s.qcom_smmu_fault_src",
	          qsmmu_fusa->smmu_name);
	debugfs_create_file(file_name, 0444, qsmmu_fusa->qcom_smmu_dir, qsmmu_fusa,
	                    &fault_source_ops);

	/* Create file to o/p SMMU FuSa fault code */
	scnprintf(file_name, sizeof(file_name), "%s.qcom_smmu_fault_code",
	          qsmmu_fusa->smmu_name);
	debugfs_create_file(file_name, 0444, qsmmu_fusa->qcom_smmu_dir, qsmmu_fusa,
	                    &fault_code_ops);
}
#endif

static u64 check_tcu_fault(u64 fisr, u32 num_clients, u8 *severity)
{
	if ((fisr & GENMASK((num_clients + FUSA_TCU500_NUM_ERR) - 1, 0))) {
		if (fisr == FUSA_INTSTS_TCU_SRAM_PARITY ||
		    fisr == FUSA_INTSTS_TBU_SRAM_PARITY)
			*severity = FUSA_WARNING;
		else
			*severity = FUSA_ERROR;

		return fisr;
	}

	*severity = FUSA_WARNING;
	return FUSA_INTSTS_SPURIOUS;
}

static u64 check_qtc_fault(u64 fisr, u8 *severity)
{
	if (fisr & GENMASK_ULL(34, 28)) {
		*severity = FUSA_WARNING;
		return fisr;
	} else if (fisr & (GENMASK(27, 0) | BIT_ULL(35))) {
		*severity = FUSA_ERROR;
		return fisr;
	}

	*severity = FUSA_WARNING;
	return FUSA_INTSTS_SPURIOUS;
}

static u32 check_tbu_fault(u32 fisr, u8 *severity)
{
	if ((fisr & GENMASK(FUSA_TBU500_NUM_ERR - 1, 0))) {
		if (fisr == FUSA_INTSTS_WRBUF_WARN)
			*severity = FUSA_WARNING;
		else
			*severity = FUSA_ERROR;

		return fisr;
	}

	*severity = FUSA_WARNING;
	return FUSA_INTSTS_SPURIOUS;
}

static irqreturn_t qcom_smmu_tcu_fault(int irq, void *dev)
{
	struct qsmmu_fusa *qsmmu_fusa = dev;
	struct irq_data *irq_data;
	struct irq_desc *desc;
	unsigned long flags;
	u8 severity;
	u64 fisr = 0;

	if (qsmmu_fusa->md->flags == QSMMU_F_QTB600) {
		fisr = readq(qsmmu_fusa->tcu_fusa_base + FUSA_QTC_INTSTS_REGISTER);
		writeq(fisr, qsmmu_fusa->tcu_fusa_base + FUSA_QTC_INTSTS_REGISTER);
	} else {
		fisr = readl(qsmmu_fusa->tcu_fusa_base);
		writel(fisr, qsmmu_fusa->tcu_fusa_base);
	}

	irq_data = irq_get_irq_data(irq);
	if (unlikely(!irq_data))
		return IRQ_NONE;

	desc = irq_data_to_desc(irq_data);
	if (unlikely(!desc))
		return IRQ_NONE;

	if (unlikely(!desc->action || !desc->action->name))
		return IRQ_NONE;

	spin_lock_irqsave(&qsmmu_fusa->lock, flags);
	scnprintf(qsmmu_fusa->hw_fault.fault_source, BUFFER_SZ, "%s",
	          desc->action->name);
#ifdef CONFIG_DEBUG_FS
	qsmmu_fusa->hw_fault.fault_src_ready = true;
	qsmmu_fusa->hw_fault.fault_code_ready = true;
#endif

	if (qsmmu_fusa->md->flags == QSMMU_F_QTB600)
		qsmmu_fusa->hw_fault.fault_code = check_qtc_fault(fisr,
								  &severity);
	else
		qsmmu_fusa->hw_fault.fault_code = check_tcu_fault(fisr,
								  qsmmu_fusa->num_clients,
								  &severity);

	spin_unlock_irqrestore(&qsmmu_fusa->lock, flags);
	trace_smmu_hwirq(dev_name(qsmmu_fusa->dev), qsmmu_fusa->hw_fault.fault_source,
			 qsmmu_fusa->hw_fault.fault_code, severity);

#ifdef CONFIG_DEBUG_FS
	wake_up_interruptible_all(&qsmmu_fusa->wq);
#endif

	return qsmmu_fusa->hw_fault.fault_code ? IRQ_HANDLED : IRQ_NONE;
}

static irqreturn_t qcom_smmu_client_fault(int irq, void *dev)
{
	struct qsmmu_fusa *qsmmu_fusa = dev;
	void __iomem *client_status_reg;
	struct irq_data *irq_data;
	struct irq_desc *desc;
	unsigned long flags;
	char *client_name;
	u32 fisr, client_index, ret;
	u8 severity;

	irq_data = irq_get_irq_data(irq);
	if (unlikely(!irq_data))
		return IRQ_NONE;

	desc = irq_data_to_desc(irq_data);
	if (unlikely(!desc))
		return IRQ_NONE;

	if (unlikely(!desc->action || !desc->action->name))
		return IRQ_NONE;

	client_name = strnstr(desc->action->name, "CLIENT", strlen(desc->action->name));
	if (!client_name)
		goto err;

	ret = kstrtouint(client_name + strlen("CLIENT"), 0, &client_index);

	if (ret)
		goto err;

	client_status_reg = (qsmmu_fusa->client_fusa_base +
	                     (client_index * qsmmu_fusa->client_offset) +
	                     qsmmu_fusa->client_fusa_reg);

	fisr = readl(client_status_reg);
	writel(fisr, client_status_reg);

	if (fisr == FUSA_INTSTS_TBU_TCU_ERR)
		writel(FUSA_INTSTS_TBU_TCU_ERR_CLR, qsmmu_fusa->tcu_fusa_base +
		       FUSA_TCU_ERROR_INJECT_REGISTER);

	spin_lock_irqsave(&qsmmu_fusa->lock, flags);
	scnprintf(qsmmu_fusa->hw_fault.fault_source, BUFFER_SZ, "%s",
	          desc->action->name);
	qsmmu_fusa->hw_fault.fault_code = check_tbu_fault(fisr, &severity);
#ifdef CONFIG_DEBUG_FS
	qsmmu_fusa->hw_fault.fault_src_ready = true;
	qsmmu_fusa->hw_fault.fault_code_ready = true;
#endif
	spin_unlock_irqrestore(&qsmmu_fusa->lock, flags);
	trace_smmu_hwirq(dev_name(qsmmu_fusa->dev), qsmmu_fusa->hw_fault.fault_source,
	                 qsmmu_fusa->hw_fault.fault_code, severity);
#ifdef CONFIG_DEBUG_FS
	wake_up_interruptible_all(&qsmmu_fusa->wq);
#endif

	return qsmmu_fusa->hw_fault.fault_code ? IRQ_HANDLED : IRQ_NONE;
err:
	return IRQ_NONE;
}

static int qcom_smmu_hw_irq_setup(struct qsmmu_fusa *qsmmu_fusa)
{
	struct platform_device *pdev = to_platform_device(qsmmu_fusa->dev);
	char *fault_source;
	u32 num_irqs;
	int irq, i, ret;

	num_irqs = platform_irq_count(pdev);
	// Note: Keeping this check until "qcom,num-clients"
	//       DT property is mainlined for all Lemans/Monaco DTs
	if (!qsmmu_fusa->num_clients)
		qsmmu_fusa->num_clients = num_irqs - 1;
	if (num_irqs < 1) {
		dev_err(qsmmu_fusa->dev, "No H/W IRQs specified in DT. Exiting...\n");
		return -EINVAL;
	}

	/* Register Fault Handler for TCU */
	irq = platform_get_irq(pdev, 0);
	fault_source = devm_kasprintf(qsmmu_fusa->dev,
	                              GFP_KERNEL,
	                              "%s.SMMU",
	                              qsmmu_fusa->smmu_name);

	if (!fault_source)
		return -ENOMEM;

	ret = devm_request_threaded_irq(qsmmu_fusa->dev,
	                                irq, NULL,
	                                qcom_smmu_tcu_fault,
	                                IRQF_ONESHOT |
	                                IRQF_SHARED,
	                                fault_source,
	                                qsmmu_fusa);

	if (ret) {
		dev_err(qsmmu_fusa->dev, "Failed to register SMMU TCU FuSa interrupt (%d)\n",
		        irq);
		goto err_irq;
	}

	/* Register Fault Handler for TCU Clients */
	for (i = 1; i < num_irqs; i++) {
		irq = platform_get_irq(pdev, i);
		fault_source = devm_kasprintf(qsmmu_fusa->dev,
		                              GFP_KERNEL,
		                              "%s.CLIENT%u",
		                              qsmmu_fusa->smmu_name,
		                              i);

		if (!fault_source)
			return -ENOMEM;

		ret = devm_request_threaded_irq(qsmmu_fusa->dev,
		                                irq, NULL,
		                                qcom_smmu_client_fault,
		                                IRQF_ONESHOT |
		                                IRQF_SHARED,
		                                fault_source,
		                                qsmmu_fusa);

		if (ret) {
			dev_err(qsmmu_fusa->dev, "Failed to register SMMU Client FuSa interrupt (%d)\n",
			        irq);
			goto err_irq;
		}
	}

	return 0;
err_irq:
	return ret;
}

static void fusa_tcu_mask_sail_err_register(struct qsmmu_fusa *qsmmu_fusa, u32 offset)
{
	u32 tcu_fusa_sail_errmask;

	tcu_fusa_sail_errmask = readl_relaxed(qsmmu_fusa->tcu_fusa_base + FUSA_TCU_ERR_MASK_SAIL);
	tcu_fusa_sail_errmask |= offset;
	writel(tcu_fusa_sail_errmask, qsmmu_fusa->tcu_fusa_base + FUSA_TCU_ERR_MASK_SAIL);
}

static void fusa_tcu_unmask_sail_err_register(struct qsmmu_fusa *qsmmu_fusa, u32 offset)
{
	u32 tcu_fusa_sail_errmask;

	tcu_fusa_sail_errmask = readl_relaxed(qsmmu_fusa->tcu_fusa_base + FUSA_TCU_ERR_MASK_SAIL);
	tcu_fusa_sail_errmask &= ~offset;
	writel(tcu_fusa_sail_errmask, qsmmu_fusa->tcu_fusa_base + FUSA_TCU_ERR_MASK_SAIL);
}

static void fusa_tcu_mask_sail_wrn_register(struct qsmmu_fusa *qsmmu_fusa, u32 offset)
{
	u32 tcu_fusa_sail_wrnmask;

	tcu_fusa_sail_wrnmask = readl_relaxed(qsmmu_fusa->tcu_fusa_base + FUSA_TCU_WRN_MASK_SAIL);
	tcu_fusa_sail_wrnmask |= offset;
	writel(tcu_fusa_sail_wrnmask, qsmmu_fusa->tcu_fusa_base + FUSA_TCU_WRN_MASK_SAIL);
}

static void fusa_tcu_unmask_sail_wrn_register(struct qsmmu_fusa *qsmmu_fusa, u32 offset)
{
	u32 tcu_fusa_sail_wrnmask;

	tcu_fusa_sail_wrnmask = readl_relaxed(qsmmu_fusa->tcu_fusa_base + FUSA_TCU_WRN_MASK_SAIL);
	tcu_fusa_sail_wrnmask &= ~offset;
	writel(tcu_fusa_sail_wrnmask, qsmmu_fusa->tcu_fusa_base + FUSA_TCU_WRN_MASK_SAIL);
}

static void inject_tcu_fault(struct qsmmu_fusa *qsmmu_fusa, u32 fault_code, bool inject_enable)
{
	if (inject_enable)
		writel(fault_code, qsmmu_fusa->tcu_fusa_base + FUSA_TCU_ERROR_INJECT_REGISTER);
	else
		writel(fault_code, qsmmu_fusa->tcu_fusa_base + FUSA_TCU_IRQ_SET_REGISTER);
}

static void clear_tcu_fusa_fault(struct qsmmu_fusa *qsmmu_fusa, u32 fault_code)
{
	writel(fault_code, qsmmu_fusa->tcu_fusa_base);
}

static int check_for_injected_tcu_fault(struct qsmmu_fusa *qsmmu_fusa, u32 fault_code)
{
	u32 tcu_fusa_intrsts = 0;

	tcu_fusa_intrsts = readl(qsmmu_fusa->tcu_fusa_base);

	if (tcu_fusa_intrsts & fault_code)
		return 0;
	return -EINVAL;
}

static int check_tcu_fault_injection(struct qsmmu_fusa *qsmmu_fusa,
				     u32 fault_code, bool inject_enable)
{
	int res;

	inject_tcu_fault(qsmmu_fusa, fault_code, inject_enable);

	res = check_for_injected_tcu_fault(qsmmu_fusa, fault_code);
	if (res)
		dev_err(qsmmu_fusa->dev, "TCU FuSa error injection failed\n");

	writel(0, qsmmu_fusa->tcu_fusa_base + FUSA_TCU_ERROR_INJECT_REGISTER);
	writel(0, qsmmu_fusa->tcu_fusa_base + FUSA_TCU_IRQ_SET_REGISTER);
	clear_tcu_fusa_fault(qsmmu_fusa, fault_code);

	return res;
}

static int check_tcu_tcu_sram_injection(struct qsmmu_fusa *qsmmu_fusa, bool inject_enable)
{
	u32 offset;
	int res;

	offset = qsmmu_fusa->num_clients + TCU_SRAM_PARITY_OFFSET;
	if (offset < 31U) {
		offset = BIT(offset);
		fusa_tcu_mask_sail_wrn_register(qsmmu_fusa, offset);
		res = check_tcu_fault_injection(qsmmu_fusa, offset, inject_enable);
		fusa_tcu_unmask_sail_wrn_register(qsmmu_fusa, offset);
	} else {
		res = -EINVAL;
	}

	return res;
}

/* TCU-TBU SRAM parity irq doesn't work when injected from this driver, hence skipping
 * static int check_tcu_tbu_sram_injection(struct qsmmu_fusa *qsmmu_fusa, bool inject_enable)
 * {
 *	u32 offset;
 *	int res;
 *
 *	offset = qsmmu_fusa->num_clients + TBU_SRAM_PARITY_OFFSET;
 *	if (offset < 31U) {
 *		offset = BIT(offset);
 *		fusa_tcu_mask_sail_wrn_register(qsmmu_fusa, offset);
 *		res = check_tcu_fault_injection(qsmmu_fusa, offset, inject_enable);
 *		fusa_tcu_unmask_sail_wrn_register(qsmmu_fusa, offset);
 *	} else {
 *		res = -EINVAL;
 *	}
 *
 *	return res;
 * }
 */

static int check_tcu_csr_parity_injection(struct qsmmu_fusa *qsmmu_fusa, bool inject_enable)
{
	u32 offset;
	int res;

	offset = qsmmu_fusa->num_clients + CSR_PARITY_OFFSET;
	if (offset < 31U) {
		offset = BIT(offset);
		fusa_tcu_mask_sail_err_register(qsmmu_fusa, offset);
		res = check_tcu_fault_injection(qsmmu_fusa, offset, inject_enable);
		fusa_tcu_unmask_sail_err_register(qsmmu_fusa, offset);
	} else {
		res = -EINVAL;
	}

	return res;
}

static int check_tcu_pfb_2d_arr_injection(struct qsmmu_fusa *qsmmu_fusa, bool inject_enable)
{
	u32 offset;
	int res;

	offset = qsmmu_fusa->num_clients + ARR_2D_OFFSET;
	if (offset < 31U) {
		offset = BIT(offset);
		fusa_tcu_mask_sail_err_register(qsmmu_fusa, offset);
		res = check_tcu_fault_injection(qsmmu_fusa, offset, inject_enable);
		fusa_tcu_unmask_sail_err_register(qsmmu_fusa, offset);
	} else {
		res = -EINVAL;
	}

	return res;
}

static int fusa_fault_injection_tcu_test(struct qsmmu_fusa *qsmmu_fusa)
{
	int result = 0;

	result = check_tcu_tcu_sram_injection(qsmmu_fusa, true);
	if (result) {
		dev_err(qsmmu_fusa->dev, "tcu_tcu_sram_injection failed\n");
		return result;
	}

	/* Optimize boot KPI by running only one test to verify
	 * FuSa H/W IRQ trigger working
	 */
	if (unlikely(smmu_fusa_inj_only_one))
		return result;

/* TCU-TBU SRAM parity irq doesn't work when injected from this driver, hence skipping
 *	result = check_tcu_tbu_sram_injection(qsmmu_fusa, true);
 *	if (result) {
 *		dev_err(qsmmu_fusa->dev, "tcu_tbu_sram_injection failed\n");
 *		return result;
 *	}
 */
	result = check_tcu_csr_parity_injection(qsmmu_fusa, true);
	if (result) {
		dev_err(qsmmu_fusa->dev, "tcu_csr_parity_injection failed\n");
		return result;
	}

	result = check_tcu_pfb_2d_arr_injection(qsmmu_fusa, false);
	if (result) {
		dev_err(qsmmu_fusa->dev, "tcu_sram_injection failed\n");
		return result;
	}

	return result;
}

static void fusa_tbu_mask_sail_err_register(void __iomem *client_base, u32 offset)
{
	u32 tbu_fusa_sail_errmask;

	tbu_fusa_sail_errmask = readl_relaxed(client_base + FUSA_TBU_ERR_MASK_SAIL);
	tbu_fusa_sail_errmask |= offset;
	writel(tbu_fusa_sail_errmask, client_base + FUSA_TBU_ERR_MASK_SAIL);
}

static void fusa_tbu_unmask_sail_err_register(void __iomem *client_base, u32 offset)
{
	u32 tbu_fusa_sail_errmask;

	tbu_fusa_sail_errmask = readl_relaxed(client_base + FUSA_TBU_ERR_MASK_SAIL);
	tbu_fusa_sail_errmask &= ~offset;
	writel(tbu_fusa_sail_errmask, client_base + FUSA_TBU_ERR_MASK_SAIL);
}

static void fusa_tbu_mask_sail_wrn_register(void __iomem *client_base, u32 offset)
{
	u32 tbu_fusa_sail_wrnmask;

	tbu_fusa_sail_wrnmask = readl_relaxed(client_base + FUSA_TBU_WRN_MASK_SAIL);
	tbu_fusa_sail_wrnmask |= offset;
	writel(tbu_fusa_sail_wrnmask, client_base + FUSA_TBU_WRN_MASK_SAIL);
}

static void fusa_tbu_unmask_sail_wrn_register(void __iomem *client_base, u32 offset)
{
	u32 tbu_fusa_sail_wrnmask;

	tbu_fusa_sail_wrnmask = readl_relaxed(client_base + FUSA_TBU_WRN_MASK_SAIL);
	tbu_fusa_sail_wrnmask &= ~offset;
	writel(tbu_fusa_sail_wrnmask, client_base + FUSA_TBU_WRN_MASK_SAIL);
}

static void inject_tbu_fault(void __iomem *client_base, u32 fault_code)
{
	writel(fault_code, client_base + FUSA_TBU_IRQ_SET_REGISTER);
}

static void clear_tbu_fusa_fault(void __iomem *client_base, u32 fault_code)
{
	writel(fault_code, client_base);
}

static int check_for_injected_tbu_fault(void __iomem *client_base, u32 fault_code)
{
	u32 tbu_fusa_intrsts = 0;

	tbu_fusa_intrsts = readl_relaxed(client_base);
	if (tbu_fusa_intrsts & fault_code)
		return 0;

	return -EINVAL;
}

static int check_tbu_fault_injection(struct qsmmu_fusa *qsmmu_fusa,
				     void __iomem *client_base,
				     u32 fault_code)
{
	int res;

	inject_tbu_fault(client_base, fault_code);

	res = check_for_injected_tbu_fault(client_base, fault_code);
	if (res) {
		dev_err(qsmmu_fusa->dev, "TBU FuSa error injection failed\n");
	} else {
		writel(0, client_base + FUSA_TBU_IRQ_SET_REGISTER);
		clear_tbu_fusa_fault(client_base, fault_code);
	}

	return res;
}

static int check_tbu_pa_err_injection(struct qsmmu_fusa *qsmmu_fusa, void __iomem *client_base)
{
	u32 offset;
	int res;

	offset = BIT(PA_ERR);
	fusa_tbu_mask_sail_err_register(client_base, offset);
	res = check_tbu_fault_injection(qsmmu_fusa, client_base, offset);
	fusa_tbu_unmask_sail_err_register(client_base, offset);

	return res;

}
static int check_tbu_va_err_injection(struct qsmmu_fusa *qsmmu_fusa, void __iomem *client_base)
{
	u32 offset;
	int res;

	offset = BIT(VA_ERR);
	fusa_tbu_mask_sail_err_register(client_base, offset);
	res = check_tbu_fault_injection(qsmmu_fusa, client_base, offset);
	fusa_tbu_unmask_sail_err_register(client_base, offset);

	return res;
}
static int check_tbu_wrbuf_err_injection(struct qsmmu_fusa *qsmmu_fusa,
					 void __iomem *client_base)
{
	u32 offset;
	int res;

	offset = BIT(WRBUF_ERR);
	fusa_tbu_mask_sail_err_register(client_base, offset);
	res = check_tbu_fault_injection(qsmmu_fusa, client_base, offset);
	fusa_tbu_unmask_sail_err_register(client_base, offset);

	return res;
}

static int check_tbu_wrbuf_wrn_injection(struct qsmmu_fusa *qsmmu_fusa,
					 void __iomem *client_base)
{
	u32 offset;
	int res;

	offset = BIT(WRBUF_WRN);
	fusa_tbu_mask_sail_wrn_register(client_base, offset);
	res = check_tbu_fault_injection(qsmmu_fusa, client_base, offset);
	fusa_tbu_unmask_sail_wrn_register(client_base, offset);

	return res;
}

static int check_tbu_tcu_link_err_injection(struct qsmmu_fusa *qsmmu_fusa,
					    void __iomem *client_base)
{
	u32 offset;
	int res;

	offset = BIT(TBU_TCU_LINK_ERR);
	fusa_tbu_mask_sail_err_register(client_base, offset);
	res = check_tbu_fault_injection(qsmmu_fusa, client_base, offset);
	fusa_tbu_unmask_sail_err_register(client_base, offset);

	return res;
}

static int fusa_fault_injection_tbu_test(struct qsmmu_fusa *qsmmu_fusa, u32 client_id)
{
	void __iomem *client_base;
	int result = 0;

	if (client_id >= qsmmu_fusa->num_clients) {
		dev_err(qsmmu_fusa->dev, "Invalid client_id %u (max: %u)\n",
			client_id, qsmmu_fusa->num_clients - 1);
		return -EINVAL;
	}

	client_base = (qsmmu_fusa->client_fusa_base +
		      ((client_id + 1) * qsmmu_fusa->client_offset) +
		      qsmmu_fusa->client_fusa_reg);

	result = check_tbu_pa_err_injection(qsmmu_fusa, client_base);
	if (result) {
		dev_err(qsmmu_fusa->dev, "tbu_pa_err_injection failed\n");
		return result;
	}

	/* Optimize boot KPI by running only one test to verify
	 * FuSa H/W IRQ trigger working
	 */
	if (unlikely(smmu_fusa_inj_only_one))
		return result;

	result = check_tbu_va_err_injection(qsmmu_fusa, client_base);
	if (result) {
		dev_err(qsmmu_fusa->dev, "tbu_va_err_injection failed\n");
		return result;
	}

	result = check_tbu_wrbuf_err_injection(qsmmu_fusa, client_base);
	if (result) {
		dev_err(qsmmu_fusa->dev, "tbu_wrbuf_err_injection failed\n");
		return result;
	}

	result = check_tbu_wrbuf_wrn_injection(qsmmu_fusa, client_base);
	if (result) {
		dev_err(qsmmu_fusa->dev, "tbu_wrbuf_wrn_injection failed\n");
		return result;
	}

	result = check_tbu_tcu_link_err_injection(qsmmu_fusa, client_base);
	if (result) {
		dev_err(qsmmu_fusa->dev, "tbu_tcu_link_err_injection failed\n");
		return result;
	}

	return result;
}

static int dma_map_test(struct device *dev, dma_addr_t *iova, void **dma_buffer)
{
	size_t size = SZ_4K;

	/* Make sure we can allocate and use a buffer */
	*dma_buffer = kmalloc(size, GFP_KERNEL);
	if (!*dma_buffer) {
		dev_err(dev, "dma_map failed\n");
		return -ENOMEM;
	}
	memset(*dma_buffer, 0xa5, size);
	*iova = dma_map_single(dev, *dma_buffer, size, DMA_TO_DEVICE);

	if (dma_mapping_error(dev, *iova)) {
		dev_err(dev, "dma_map failed\n");
		kfree(*dma_buffer);
		*dma_buffer = NULL;
		return -ENOMEM;
	}

	return 0;
}

static int qsmmu_fusa_test(struct qsmmu_fusa *qsmmu_fusa)
{
	struct platform_device *test_pdev;
	struct device_node *child;
	bool dma_mapped = false;
	void *dma_buffer = NULL;
	bool timedout = false;
	dma_addr_t iova = 0;
	u32 client_id = 0;
	int ret = 0;


	/* Find num of TBUs */
	for_each_child_of_node(qsmmu_fusa->dev->of_node, child) {
		reinit_completion(&qsmmu_fusa->tdev->probe_wait);

		/*
		 * Create platform_device for each TBU entry
		 * This will create a iommu group mapping
		 */
		test_pdev = of_platform_device_create(child, NULL, qsmmu_fusa->dev);
		if (!test_pdev) {
			dev_err(qsmmu_fusa->dev, "Creating platform device failed\n");
			of_node_put(child);
			return -EINVAL;
		}

		/*
		 * Wait for child device's probe function to be called.
		 * Its very unlikely to be asynchonrous...
		 */
		ret = wait_for_completion_interruptible_timeout(&qsmmu_fusa->tdev->probe_wait,
						msecs_to_jiffies(USECASE_SWITCH_TIMEOUT_MSECS));
		if (ret <= 0) {
			dev_err(qsmmu_fusa->dev, "Timed out waiting for test device probe\n");
			if (ret == 0)
				timedout = true;
			goto out;
		}
		timedout = false;

		if (!iommu_get_domain_for_dev(&test_pdev->dev))
			dev_notice(qsmmu_fusa->dev, "Oops, usecase not associated with iommu\n");

		/* map dummy dma data */
		ret = dma_map_test(&test_pdev->dev, &iova, &dma_buffer);
		if (ret)
			goto out;
		dma_mapped = true;

		/* Inject and test faults for TBU */
		mutex_lock(&qsmmu_fusa->tdev->state_lock);
		qsmmu_fusa->tdev->test_dev = &test_pdev->dev;
		mutex_unlock(&qsmmu_fusa->tdev->state_lock);
		ret = fusa_fault_injection_tcu_test(qsmmu_fusa);
		if (!ret && qsmmu_fusa->tbu_fault_injection) {
			int tbu_ret = fusa_fault_injection_tbu_test(qsmmu_fusa, client_id);

			if (tbu_ret)
				dev_warn(qsmmu_fusa->dev,
					 "FuSa TBU client %u injection failed (%d) - subsystem gated? skipping\n",
					 client_id, tbu_ret);
		}

out:
		/* unmap dma data */
		if (dma_mapped) {
			dma_unmap_single(&test_pdev->dev, iova, SZ_4K, DMA_TO_DEVICE);
			kfree(dma_buffer);
			dma_buffer = NULL;
			dma_mapped = false;
		}

		/*
		 * Destroy platform_device for each TBU entry
		 * Clear test_dev pointer before destroying device
		 */
		mutex_lock(&qsmmu_fusa->tdev->state_lock);
		qsmmu_fusa->tdev->test_dev = NULL;
		mutex_unlock(&qsmmu_fusa->tdev->state_lock);
		if (test_pdev)
			of_platform_device_destroy(&test_pdev->dev, NULL);

		if (ret || timedout) {
			dev_err(qsmmu_fusa->dev, "qsmmu_fusa fault injection test failed. Exiting ...\n");
			of_node_put(child);
			return ret ? ret : -ETIMEDOUT;
		}

		client_id++;
		if (qsmmu_fusa->tbu_fault_injection && smmu_fusa_only_one_client) {
			of_node_put(child);
			break;
		}
	}

	return ret;
}

static int qsmmu_fusa_test_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qsmmu_fusa *qsmmu_fusa;

	if (!dev->parent)
		return -EINVAL;

	qsmmu_fusa = dev_get_drvdata(dev->parent);
	if (!qsmmu_fusa)
		return -EINVAL;

	if (qsmmu_fusa->tdev)
		complete(&qsmmu_fusa->tdev->probe_wait);

	return 0;
}

static int qsmmu_fusa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct qsmmu_fusa *qsmmu_fusa;
	struct resource *tcu_res, client_res;
	resource_size_t tcu_sz;
	int ret = 0;

	qsmmu_fusa = devm_kzalloc(dev, sizeof(*qsmmu_fusa), GFP_KERNEL);
	if (!qsmmu_fusa)
		return -ENOMEM;

	qsmmu_fusa->tdev = devm_kzalloc(dev, sizeof(struct qsmmu_fusa_test), GFP_KERNEL);
	if (!qsmmu_fusa->tdev)
		return -ENOMEM;

	init_completion(&qsmmu_fusa->tdev->probe_wait);
	mutex_init(&qsmmu_fusa->tdev->state_lock);

	qsmmu_fusa->dev = dev;
	qsmmu_fusa->smmu_name = dev_name(dev);

	if (of_property_read_u32(np, "qcom,num_clients", &qsmmu_fusa->num_clients)) {
		// Note: Not making mandatory until "qcom,num-clients"
		//       DT property is mainlined for all Lemans/Monaco DTs
		dev_notice(dev, "missing mandatory \"qcom,num_clients\" property\n");
		//return -EINVAL;
	}

	tcu_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!tcu_res)
		return -ENODEV;

	tcu_sz = resource_size(tcu_res);
	qsmmu_fusa->tcu_fusa_base = devm_ioremap(dev, tcu_res->start, tcu_sz);
	if (!qsmmu_fusa->tcu_fusa_base) {
		dev_err(dev, "Can't map SMMU FUSA @%pa\n", &tcu_res->start);
		return -ENOMEM;
	}

	qsmmu_fusa->md = of_device_get_match_data(&pdev->dev);
	if (!qsmmu_fusa->md)
		return -ENODEV;

	if (qsmmu_fusa->md->flags == QSMMU_F_TBU500) {
		if (!of_property_read_u32(np, "qcom,client_offset",
					  &qsmmu_fusa->client_offset)) {
			ret = of_address_to_resource(np, 1, &client_res);

			if (ret) {
				dev_err(dev, "Failed to parse client fusa memory region\n");
				return ret;
			}

			qsmmu_fusa->client_fusa_sz = resource_size(&client_res);
			qsmmu_fusa->client_fusa_base = devm_ioremap(qsmmu_fusa->dev,
								    client_res.start,
								    qsmmu_fusa->client_fusa_sz);

			if (!qsmmu_fusa->client_fusa_base)
				return -ENOMEM;

			qsmmu_fusa->tbu_fault_injection = true;
		}
	}

	qsmmu_fusa->client_fusa_reg = qsmmu_fusa->md->offset;

	platform_set_drvdata(pdev, qsmmu_fusa);

	if (qsmmu_fusa->md->enable_fault_injection) {
		ret = qsmmu_fusa_test(qsmmu_fusa);
		if (ret) {
			/* If the fault-injection test fails this module should
			 * trigger a panic as occurrence of this failure signifies
			 * a compromise in the system safety. Hence, we should not
			 * continue system boot post failure of SMMU FuSa fault
			 * injection test
			 *
			 * #ToDo : will be enabled once verified on all active platforms
			 *
			 * msleep(100);
			 * panic();
			 * return ret;
			 */
			dev_notice(dev, "qsmmu_fusa_test failed : %d\n", ret);
		}
	}

	qsmmu_fusa->hw_fault.fault_source = devm_kzalloc(qsmmu_fusa->dev,
							 BUFFER_SZ, GFP_KERNEL);

	if (!qsmmu_fusa->hw_fault.fault_source)
		return -ENOMEM;

	spin_lock_init(&qsmmu_fusa->lock);
#ifdef CONFIG_DEBUG_FS
	init_waitqueue_head(&qsmmu_fusa->wq);
	qcom_smmu_create_debug_dir(qsmmu_fusa);
#endif
	ret = qcom_smmu_hw_irq_setup(qsmmu_fusa);

	return ret;
}

static void qsmmu_fusa_remove(struct platform_device *pdev)
{
#ifdef CONFIG_DEBUG_FS
	struct qsmmu_fusa *qsmmu_fusa = platform_get_drvdata(pdev);
	char file_name[BUFFER_SZ];

	if (IS_ERR_OR_NULL(qsmmu_fusa->qcom_smmu_dir))
		return;

	scnprintf(file_name, sizeof(file_name), "%s.qcom_smmu_fault_src",
						qsmmu_fusa->smmu_name);
	debugfs_lookup_and_remove(file_name, qsmmu_fusa->qcom_smmu_dir);
	scnprintf(file_name, sizeof(file_name), "%s.qcom_smmu_fault_code",
						qsmmu_fusa->smmu_name);
	debugfs_lookup_and_remove(file_name, qsmmu_fusa->qcom_smmu_dir);

	qsmmu_fusa->qcom_smmu_dir = NULL;
#endif
}

static const struct qsmmu_fusa_match_data md_qsmmu_tbu500 = {
	.offset = FUSA_TBU500_OFFSET,
	.flags  = QSMMU_F_TBU500,
	.enable_fault_injection = true,
};

static const struct qsmmu_fusa_match_data md_qsmmu_qtb500 = {
	.offset = 0,
	.flags  = QSMMU_F_QTB500,
	.enable_fault_injection = false,
};

static const struct qsmmu_fusa_match_data md_qsmmu_qtb600 = {
	.offset = 0,
	.flags  = QSMMU_F_QTB600,
	.enable_fault_injection = false,
};

static const struct of_device_id qsmmu_fusa_of_match[] = {
	{.compatible = "qcom,tbu500-fusa", .data = &md_qsmmu_tbu500},
	{.compatible = "qcom,qtb500-fusa", .data = &md_qsmmu_qtb500},
	{.compatible = "qcom,qtb600-fusa", .data = &md_qsmmu_qtb600},
	{}
};

static struct platform_driver qsmmu_fusa_driver = {
	.driver = {
		.name = "qsmmu-fusa",
		.of_match_table = qsmmu_fusa_of_match,
	},
	.probe = qsmmu_fusa_probe,
	.remove_new = qsmmu_fusa_remove,
};

static const struct of_device_id qsmmu_fusa_test_of_match[] = {
	{.compatible = "qcom,tbu500-fusa-test"},
	{.compatible = "qcom,qtb500-fusa-test"},
	{}
};

static struct platform_driver qsmmu_fusa_test_driver = {
	.driver = {
		   .name = "qcom,qsmmu-fusa-test",
		   .of_match_table = qsmmu_fusa_test_of_match,
		   },
	.probe = qsmmu_fusa_test_probe,
};

static int qcom_fusa_init(void)
{
	int ret;

	ret = platform_driver_register(&qsmmu_fusa_test_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&qsmmu_fusa_driver);
	if (ret)
		platform_driver_unregister(&qsmmu_fusa_test_driver);
	return ret;
}

static void qcom_fusa_exit(void)
{
#ifdef CONFIG_DEBUG_FS
	struct dentry *dir;
#endif

	platform_driver_unregister(&qsmmu_fusa_test_driver);
	platform_driver_unregister(&qsmmu_fusa_driver);
#ifdef CONFIG_DEBUG_FS
	dir = debugfs_lookup("smmu_hw_faults", NULL);
	debugfs_remove_recursive(dir);
	if (dir)
		dput(dir);
#endif
}

module_init(qcom_fusa_init);
module_exit(qcom_fusa_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies Inc. SMMU Functional Safety driver");

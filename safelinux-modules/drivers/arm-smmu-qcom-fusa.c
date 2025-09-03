// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/dev_printk.h>
#include <linux/debugfs.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#define CREATE_TRACE_POINTS
#include "arm-smmu-qcom-fusa.h"

#define FUSA_TCU_ERROR_INJECT_REGISTER	0x18

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

#define FUSA_INTSTS_SPURIOUS		0x0
#define FUSA_INTSTS_TBU_TCU_ERR_CLR	0x0

#define FUSA_TBU500_OFFSET		0x4000

#define FUSA_WARNING			0x0
#define FUSA_ERROR			0x1

#define BUFFER_SZ			64

struct qcom_smmu_safety_fault {
	char *fault_source;
	u32 fault_code;
#ifdef CONFIG_DEBUG_FS
	bool fault_src_ready;
	bool fault_code_ready;
#endif
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
#ifdef CONFIG_DEBUG_FS
	wait_queue_head_t wq;
#endif
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

	len = scnprintf(buf, sizeof(buf), "0x%x\n", qsmmu_fusa->hw_fault.fault_code);

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

static int check_tcu_fault(u32 fisr, u32 num_clients, u8 *severity)
{
	if ((fisr & GENMASK(num_clients + FUSA_TCU500_NUM_ERR, 0))) {
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

static int check_tbu_fault(u32 fisr, u8 *severity)
{
	if ((fisr & GENMASK(FUSA_TBU500_NUM_ERR, 0))) {
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
	struct irq_desc *desc;
	unsigned long flags;
	u8 severity;
	u32 fisr;

	fisr = readl(qsmmu_fusa->tcu_fusa_base);
	writel(fisr, qsmmu_fusa->tcu_fusa_base);

	desc = irq_data_to_desc(irq_get_irq_data(irq));
	if (unlikely(!desc))
		return IRQ_NONE;

	spin_lock_irqsave(&qsmmu_fusa->lock, flags);
	scnprintf(qsmmu_fusa->hw_fault.fault_source, BUFFER_SZ, "%s",
	          desc->action->name);
	qsmmu_fusa->hw_fault.fault_code = check_tcu_fault(fisr, qsmmu_fusa->num_clients,
	                                  &severity);
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
}

static irqreturn_t qcom_smmu_client_fault(int irq, void *dev)
{
	struct qsmmu_fusa *qsmmu_fusa = dev;
	void __iomem *client_status_reg;
	struct irq_desc *desc;
	unsigned long flags;
	char *client_name;
	u32 fisr, client_index, ret;
	u8 severity;

	desc = irq_data_to_desc(irq_get_irq_data(irq));
	if (unlikely(!desc))
		return IRQ_NONE;

	client_name = strnstr(desc->action->name, "CLIENT", strlen(desc->action->name));
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
	qsmmu_fusa->num_clients = num_irqs - 1;

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

static int qsmmu_fusa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct qsmmu_fusa *qsmmu_fusa;
	struct resource *tcu_res, client_res;
	resource_size_t tcu_sz;
	int ret;

	qsmmu_fusa = devm_kzalloc(dev, sizeof(*qsmmu_fusa), GFP_KERNEL);
	if (!qsmmu_fusa)
		return -ENOMEM;

	qsmmu_fusa->dev = dev;
	qsmmu_fusa->smmu_name = dev_name(dev);

	tcu_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!tcu_res)
		return -ENODEV;

	tcu_sz = resource_size(tcu_res);
	qsmmu_fusa->tcu_fusa_base = devm_ioremap(dev, tcu_res->start, tcu_sz);
	if (!qsmmu_fusa->tcu_fusa_base) {
		dev_err(dev, "Can't map SMMU FUSA @%pa\n", &tcu_res->start);
		return PTR_ERR(qsmmu_fusa->tcu_fusa_base);
	}

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

		qsmmu_fusa->client_fusa_reg = (u32)(uintptr_t)of_device_get_match_data(dev);
	}

	qsmmu_fusa->hw_fault.fault_source = devm_kzalloc(qsmmu_fusa->dev,
	                                    PAGE_SIZE, GFP_KERNEL);

	if (!qsmmu_fusa->hw_fault.fault_source)
		return -ENOMEM;

	spin_lock_init(&qsmmu_fusa->lock);
	ret = qcom_smmu_hw_irq_setup(qsmmu_fusa);
#ifdef CONFIG_DEBUG_FS
	qcom_smmu_create_debug_dir(qsmmu_fusa);
	init_waitqueue_head(&qsmmu_fusa->wq);
#endif
	platform_set_drvdata(pdev, qsmmu_fusa);

	return ret;
}

static void qsmmu_fusa_remove(struct platform_device *pdev)
{
#ifdef CONFIG_DEBUG_FS
	struct qsmmu_fusa *qsmmu_fusa = platform_get_drvdata(pdev);

	debugfs_remove_recursive(qsmmu_fusa->qcom_smmu_dir);
	qsmmu_fusa->qcom_smmu_dir = NULL;
#endif
}

static const struct of_device_id qsmmu_fusa_of_match[] = {
	{.compatible = "qcom,smmu500-tbu-fusa", .data = (void *) FUSA_TBU500_OFFSET},
	{.compatible = "qcom,smmu500-qtb-fusa"},
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

module_platform_driver(qsmmu_fusa_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies Inc. SMMU Functional Safety driver");

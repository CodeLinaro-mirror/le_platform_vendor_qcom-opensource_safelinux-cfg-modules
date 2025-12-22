// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/debugfs.h>
#include <linux/dev_printk.h>
#include <linux/interrupt.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/iopoll.h>

#define CREATE_TRACE_POINTS
#include "pinctrl_fusa_trace.h"

/* TLMM FUSA reg value for error injection */
#define FUSA_STATUS_REG_CDP_BIT		BIT(13)

/* TLMM FUSA reg value for error state reset */
#define FUSA_STATUS_REG_CMP_BIT		BIT(14)

#define BUF_SZ	32

struct tlmm_fusa {
	struct device *dev;
	/* FuSa overall error status register */
	void __iomem *err_status;
	/* Advanced High-performance Hardware Reset Error Status */
	void __iomem *ahb_hreset_status;
	/* Power-on-reset Asynchronous Reset */
	void __iomem *por_ares_status;
	spinlock_t lock;
	int irq;
	/* Advanced High-performance Hardware Reset Bus Error Count */
	u32 ahb_hreset_err_cnt;
	/* Power-on-reset Asynchronous Reset Count */
	u32 por_ares_err_cnt;
	/* Spurious Error Count */
	u32 spurious_err_cnt;
	u32 por_ares_bitmask;
	bool ahb_hreset_ready;
	bool por_ares_ready;
	bool spurious_ready;
#ifdef CONFIG_DEBUG_FS
	wait_queue_head_t wq;
#endif
};

enum tlmm_fusa_event_type {
	AHB_HRESET,
	POR_ARES,
	SPURIOUS
};

struct tlmm_fusa_event {
	enum tlmm_fusa_event_type type;
	u32 count;
	void __iomem *reset_reg;
};

#ifdef CONFIG_DEBUG_FS
static struct dentry *debugfs_dir;
static ssize_t ahb_hreset_err_read(struct file *file, char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct tlmm_fusa *pinctrl_fusa = file->private_data;
	char buf[BUF_SZ];
	int len;

	len = scnprintf(buf, sizeof(buf), "%u\n",
			pinctrl_fusa->ahb_hreset_err_cnt);
	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static ssize_t por_ares_err_read(struct file *file, char __user *user_buf,
				 size_t count, loff_t *ppos)
{
	struct tlmm_fusa *pinctrl_fusa = file->private_data;
	char buf[BUF_SZ];
	int len;

	len = scnprintf(buf, sizeof(buf), "%u\n",
			pinctrl_fusa->por_ares_err_cnt);
	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static ssize_t spurious_err_read(struct file *file, char __user *user_buf,
				 size_t count, loff_t *ppos)
{
	struct tlmm_fusa *pinctrl_fusa = file->private_data;
	char buf[BUF_SZ];
	int len;

	len = scnprintf(buf, sizeof(buf), "%u\n",
			pinctrl_fusa->spurious_err_cnt);
	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static __poll_t ahb_hreset_err_notify(struct file *filep, poll_table *wait)
{
	struct tlmm_fusa *pinctrl_fusa = filep->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(filep, &pinctrl_fusa->wq, wait);
	spin_lock_irqsave(&pinctrl_fusa->lock, flags);
	if (pinctrl_fusa->ahb_hreset_ready) {
		pinctrl_fusa->ahb_hreset_ready = false;
		mask = EPOLLIN;
	}

	spin_unlock_irqrestore(&pinctrl_fusa->lock, flags);
	return mask;
}

static __poll_t por_ares_err_notify(struct file *filep, poll_table *wait)
{
	struct tlmm_fusa *pinctrl_fusa = filep->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(filep, &pinctrl_fusa->wq, wait);
	spin_lock_irqsave(&pinctrl_fusa->lock, flags);
	if (pinctrl_fusa->por_ares_ready) {
		pinctrl_fusa->por_ares_ready = false;
		mask = EPOLLIN;
	}

	spin_unlock_irqrestore(&pinctrl_fusa->lock, flags);
	return mask;
}

static __poll_t spurious_err_notify(struct file *filep, poll_table *wait)
{
	struct tlmm_fusa *pinctrl_fusa = filep->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(filep, &pinctrl_fusa->wq, wait);
	spin_lock_irqsave(&pinctrl_fusa->lock, flags);
	if (pinctrl_fusa->spurious_ready) {
		pinctrl_fusa->spurious_ready = false;
		mask = EPOLLIN;
	}

	spin_unlock_irqrestore(&pinctrl_fusa->lock, flags);
	return mask;
}

static const struct file_operations ahb_hreset_err_fops = {
	.owner = THIS_MODULE,
	.read = ahb_hreset_err_read,
	.open = simple_open,
	.poll = ahb_hreset_err_notify,
};

static const struct file_operations por_ares_err_fops = {
	.owner = THIS_MODULE,
	.read = por_ares_err_read,
	.open = simple_open,
	.poll = por_ares_err_notify,
};

static const struct file_operations spurious_err_fops = {
	.owner = THIS_MODULE,
	.read = spurious_err_read,
	.open = simple_open,
	.poll = spurious_err_notify,
};

static int tlmm_fusa_create_debugfs_entries(struct tlmm_fusa *pinctrl_fusa)
{
	debugfs_dir = debugfs_lookup("tlmm_hw_safety", NULL);
	if (!debugfs_dir) {
		debugfs_dir = debugfs_create_dir("tlmm_hw_safety", NULL);
		if (IS_ERR_OR_NULL(debugfs_dir)) {
			dev_err(pinctrl_fusa->dev,
				"Failed to create new debugfs directory\n");
			return IS_ERR(debugfs_dir);
		}
	}

	debugfs_create_file("ahb_hreset_errors", 0444, debugfs_dir,
				pinctrl_fusa, &ahb_hreset_err_fops);
	debugfs_create_file("por_ares_errors", 0444, debugfs_dir,
				pinctrl_fusa, &por_ares_err_fops);
	debugfs_create_file("spurious_errors", 0444, debugfs_dir,
				pinctrl_fusa, &spurious_err_fops);
	return 0;
}
#endif

static irqreturn_t pinctrl_fusa_irq(int irq, void *data)
{
	struct tlmm_fusa *pinctrl_fusa = data;
	int ret = IRQ_HANDLED;
	unsigned long flags;
	u32 err_status = readl_relaxed(pinctrl_fusa->err_status);
	struct tlmm_fusa_event event;

	spin_lock_irqsave(&pinctrl_fusa->lock, flags);
	if (!err_status) {
		/* Spurious Error Case */
		pinctrl_fusa->spurious_ready = true;
		pinctrl_fusa->spurious_err_cnt++;
		event.type = SPURIOUS;
		event.count = pinctrl_fusa->spurious_err_cnt;
		ret = IRQ_NONE;
	} else if ((err_status & pinctrl_fusa->por_ares_bitmask)) {
		/* POR_ARES Error Case */
		pinctrl_fusa->por_ares_ready = true;
		pinctrl_fusa->por_ares_err_cnt++;
		event.type = POR_ARES;
		event.count = pinctrl_fusa->por_ares_err_cnt;
		event.reset_reg = pinctrl_fusa->por_ares_status;
	} else {
		/* AHB_HRESET Error Case */
		pinctrl_fusa->ahb_hreset_ready = true;
		pinctrl_fusa->ahb_hreset_err_cnt++;
		event.type = AHB_HRESET;
		event.count = pinctrl_fusa->ahb_hreset_err_cnt;
		event.reset_reg = pinctrl_fusa->ahb_hreset_status;
	}

	spin_unlock_irqrestore(&pinctrl_fusa->lock, flags);
	if (event.type != SPURIOUS)
		writel_relaxed(FUSA_STATUS_REG_CMP_BIT, event.reset_reg);

	trace_tlmm_hw_safety_event(event.type == SPURIOUS ? "SPURIOUS" :
			event.type == POR_ARES ? "POR_ARES" :
			"AHB_HRESET", event.count);

#ifdef CONFIG_DEBUG_FS
	wake_up_interruptible(&pinctrl_fusa->wq);
#endif
	return ret;
}

#if IS_ENABLED(CONFIG_PINCTRL_SA8797P)
/* Boot-time TLMM FUSA Parity Error Validation */
static void tlmm_fusa_validate_parity_errors(struct tlmm_fusa *pinctrl_fusa)
{
	u32 err_status;
	int ret;

	/* Trigger AHB HRESET error */
	writel(FUSA_STATUS_REG_CDP_BIT, pinctrl_fusa->ahb_hreset_status);
	ret = readl_poll_timeout(pinctrl_fusa->err_status, err_status,
			(err_status && !(err_status & pinctrl_fusa->por_ares_bitmask)), 1, 1000);
	if (ret) {
		/* Give 100 ms delay for logging infra to come up to be able to capture the panic */
		msleep(100);
		panic("TLMM FUSA: AHB_HRESET parity error validation failed!");
	}

	writel_relaxed(FUSA_STATUS_REG_CMP_BIT, pinctrl_fusa->ahb_hreset_status);
	/* Trigger POR ARES error */
	writel(FUSA_STATUS_REG_CDP_BIT, pinctrl_fusa->por_ares_status);
	ret = readl_poll_timeout(pinctrl_fusa->err_status, err_status,
			(err_status & pinctrl_fusa->por_ares_bitmask), 1, 1000);
	if (ret) {
		/* Give 100 ms delay for logging infra to come up to be able to capture the panic */
		msleep(100);
		panic("TLMM FUSA: POR_ARES parity error validation failed!");
	}

	writel_relaxed(FUSA_STATUS_REG_CMP_BIT, pinctrl_fusa->por_ares_status);
}
#endif

static int tlmm_fusa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tlmm_fusa *pinctrl_fusa;
	struct resource *res;
	int ret;
	u32 por_ares_bit;

	pinctrl_fusa = devm_kzalloc(dev, sizeof(*pinctrl_fusa), GFP_KERNEL);
	if (!pinctrl_fusa)
		return -ENOMEM;

	pinctrl_fusa->dev = dev;
	spin_lock_init(&pinctrl_fusa->lock);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"error_status");
	if (!res) {
		dev_err(dev, "Failed to get resource\n");
		return -ENODEV;
	}

	pinctrl_fusa->err_status = devm_ioremap(dev, res->start,
						resource_size(res));
	if (!pinctrl_fusa->err_status) {
		dev_err(dev, "Couldn't ioremap error_status register\n");
		return -ENOMEM;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"ahb_hreset_status");
	if (!res) {
		dev_err(dev, "Failed to get resource\n");
		return -ENODEV;
	}

	pinctrl_fusa->ahb_hreset_status = devm_ioremap(dev, res->start,
							resource_size(res));
	if (!pinctrl_fusa->ahb_hreset_status) {
		dev_err(dev, "Couldn't ioremap ahb_hreset_status register\n");
		return -ENOMEM;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"por_ares_status");
	if (!res) {
		dev_err(dev, "Failed to get resource\n");
		return -ENODEV;
	}

	pinctrl_fusa->por_ares_status = devm_ioremap(dev, res->start,
							resource_size(res));
	if (!pinctrl_fusa->por_ares_status) {
		dev_err(dev, "Couldn't ioremap por_ares_status register\n");
		return -ENOMEM;
	}

	ret = of_property_read_u32(dev->of_node, "qcom,por-ares-reset-bit",
							&por_ares_bit);
	if (ret) {
		dev_err(dev,
			"Failed to read qcom,por-ares-reset-bit property\n");
		return ret;
	}

	pinctrl_fusa->por_ares_bitmask = BIT(por_ares_bit);
	pinctrl_fusa->irq = platform_get_irq(pdev, 0);
	if (pinctrl_fusa->irq < 0) {
		dev_err(dev, "Failed to get irq\n");
		return pinctrl_fusa->irq;
	}

#if IS_ENABLED(CONFIG_PINCTRL_SA8797P)
	tlmm_fusa_validate_parity_errors(pinctrl_fusa);
#endif
	ret = devm_request_irq(dev, pinctrl_fusa->irq, pinctrl_fusa_irq,
			       IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND,
			       "pinctrl_fusa", pinctrl_fusa);
	if (ret) {
		dev_err(dev, "Failed to register TLMM FUSA irq: %d\n", ret);
		return ret;
	}

	dev_set_drvdata(dev, pinctrl_fusa);
#ifdef CONFIG_DEBUG_FS
	ret = tlmm_fusa_create_debugfs_entries(pinctrl_fusa);
	init_waitqueue_head(&pinctrl_fusa->wq);
#endif
	return ret;
}

static void tlmm_fusa_remove(struct platform_device *pdev)
{
#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(debugfs_dir);
	debugfs_dir = NULL;
#endif
}

static const struct of_device_id tlmm_fusa_of_match[] = {
	{ .compatible = "qcom,tlmm-fusa", },
	{ },
};

static struct platform_driver pinctrl_fusa_driver = {
	.driver = {
		.name = "tlmm-fusa",
		.of_match_table = tlmm_fusa_of_match,
	},
	.probe = tlmm_fusa_probe,
	.remove_new = tlmm_fusa_remove,
};

module_platform_driver(pinctrl_fusa_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies Inc. TLMM FUSA driver");

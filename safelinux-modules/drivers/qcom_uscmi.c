/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/errno.h>
#include <linux/pm_runtime.h>
#include <linux/pm_opp.h>
#include <linux/reset.h>
#include <uapi/misc/qcom_uscmi.h>

struct qcom_uscmi_dev {
	struct miscdevice miscdev;
	struct device *dev;
	const char *name;
};

#define miscdev_to_data(d) container_of(d, struct qcom_uscmi_dev, miscdev)

static int dev_pm_opp_set_level(struct device *dev,
				unsigned int level)
{
	struct dev_pm_opp *opp = dev_pm_opp_find_level_exact(dev, level);
	int ret = 0;

	if (IS_ERR(opp))
		return PTR_ERR(opp);

	ret = dev_pm_opp_set_opp(dev, opp);
	dev_pm_opp_put(opp);

	return ret;
}

static int qcom_uscmi_open(struct inode *inode, struct file *filp)
{
	/* do nothing */
	return 0;
}

static int qcom_uscmi_release(struct inode *inode, struct file *filp)
{
	/* do nothing */
        return 0;
}

static int do_performance_operation(scmi_oper_ioctl_t *req,
				    struct qcom_uscmi_dev *uscmi)
{
	struct device *dev = uscmi->dev;
	int ret = 0;

	if (req->proto != SCMI_PROTO_PERFORMANCE)
		return -EINVAL;

	if (dev_pm_opp_get_opp_count(dev) <= 0)
		return -EINVAL;

	switch(req->oper) {
	  case SCMI_PRF_LVL_SET:
		ret = dev_pm_opp_set_level(dev, req->level);
		break;

	  default:
		dev_warn(dev, "performance operation(%d) not supported\n", req->oper);
		ret = -EINVAL;
	}

	if (ret)
		dev_err(dev, "performance operation(%d) failed with err=%d\n", req->oper, ret);

	return ret;
}

static int do_reset_operation(scmi_oper_ioctl_t *req,
			      struct qcom_uscmi_dev *uscmi)
{
	struct device *dev = uscmi->dev;
	const char *id = strlen(req->reset_id) ? req->reset_id : NULL;
	struct reset_control *rstc;
	int ret = 0;

	if (req->proto != SCMI_PROTO_RESET)
		return -EINVAL;

	rstc = devm_reset_control_get_optional(dev, id);
	if (IS_ERR_OR_NULL(rstc))
		return -ENODEV;

	switch(req->oper) {
	  case SCMI_RST_ASSERT:
		ret = reset_control_assert(rstc);
		break;
	  case SCMI_RST_DEASSERT:
		ret = reset_control_deassert(rstc);
		break;
	  case SCMI_RST_RESET:
		ret = reset_control_reset(rstc);
		break;
	  default:
		ret = -EINVAL;
		break;
	}

	reset_control_put(rstc);

	if (ret)
		dev_err(dev, "reset operation(%d) failed with err=%d\n", req->oper, ret);

	return ret;
}

static long qcom_uscmi_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct qcom_uscmi_dev *uscmi = miscdev_to_data(file->private_data);
	char __user *argp = (char __user *)arg;
	scmi_oper_ioctl_t req;
	int err = 0;

	err = copy_from_user(&req, argp, sizeof(req));
	if (err)
		return -EFAULT;

	switch (cmd) {
	  case SCMI_IOCTL_PRF:
		err = do_performance_operation(&req, uscmi);
		break;

	  case SCMI_IOCTL_RST:
		err = do_reset_operation(&req, uscmi);
		break;

	  default:
		err = -ENOTTY;
		break;
	}

	return err;
}

static const struct file_operations qcom_uscmi_fops = {
	.owner = THIS_MODULE,
	.open = qcom_uscmi_open,
	.release = qcom_uscmi_release,
	.unlocked_ioctl = qcom_uscmi_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int qcom_uscmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct qcom_uscmi_dev *uscmi;
	const char *name;
	int err;

	uscmi = devm_kzalloc(dev, sizeof(*uscmi), GFP_KERNEL);
	if (!uscmi)
		return -ENOMEM;

	uscmi->dev = dev;

	if (!of_property_read_string(np, "qcom,dev-name", &name))
		uscmi->name = devm_kstrdup(dev, name, GFP_KERNEL);
	else
		uscmi->name = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);

	uscmi->miscdev.minor = MISC_DYNAMIC_MINOR;
	uscmi->miscdev.name = uscmi->name;
	uscmi->miscdev.fops = &qcom_uscmi_fops;

	err = misc_register(&uscmi->miscdev);
	if (err) {
		dev_err(dev, "misc_register failed(ret=%d)\n", err);
		return err;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_forbid(dev);

	dev_info(dev, "/dev/%s node created\n", uscmi->name);

	return 0;
}

static int qcom_uscmi_runtime_nop(struct device *dev)
{
	/* do nothing */
	return 0;
}

static const struct of_device_id qcom_uscmi_match_table[] = {
	{ .compatible = "qcom,uscmi", },
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_uscmi_match_table);

static const struct dev_pm_ops qcom_uscmi_pm_ops = {
	.runtime_suspend = qcom_uscmi_runtime_nop,
	.runtime_resume = qcom_uscmi_runtime_nop,
};

static struct platform_driver qcom_uscmi_driver = {
	.driver = {
		.name = "qcom-uscmi",
		.pm = &qcom_uscmi_pm_ops,
		.of_match_table = qcom_uscmi_match_table,
	},
	.probe = qcom_uscmi_probe,
};
module_platform_driver(qcom_uscmi_driver);

MODULE_DESCRIPTION("Qualcomm userspace scmi interface driver");
MODULE_LICENSE("GPL v2");

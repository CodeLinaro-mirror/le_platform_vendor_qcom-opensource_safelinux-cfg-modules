/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
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
#include <linux/pm_domain.h>
#include <linux/mutex.h>
#include <uapi/misc/qcom_uscmi.h>

struct qcom_uscmi_dev {
	struct miscdevice miscdev;
	struct device *dev;
	const char *name;
	struct mutex uscmi_lock;
	struct dev_pm_domain_list *pd_list;
};

#define miscdev_to_data(d) container_of(d, struct qcom_uscmi_dev, miscdev)

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

static struct device * get_pd_dev(struct qcom_uscmi_dev *uscmi, const char *name, int *idx)
{
	int index = -EINVAL;

	if (!uscmi->pd_list) { /* single domain */
		*idx = 0;
		return uscmi->dev;
	}

	if (strlen(name))
		index = of_property_match_string(uscmi->dev->of_node, "power-domain-names",
						 name);

	if (index < 0) {
		dev_err(uscmi->dev, "power domain %s not found\n", name);
		return NULL;
	}

	*idx = index;
	return uscmi->pd_list->pd_devs[index];
}

static int is_genpd_on(struct device *dev)
{
	struct generic_pm_domain *genpd = pd_to_genpd(dev->pm_domain);

	return (genpd->status == GENPD_STATE_ON);
}

static int do_power_operation(scmi_oper_ioctl_t *req,
			      struct qcom_uscmi_dev *uscmi)
{
	struct device *dev;
	int ret = 0;
	int index;

	dev = get_pd_dev(uscmi, req->name, &index);
	if (!dev)
		return -EINVAL;

	if (req->proto != SCMI_PROTO_POWER)
		return -EINVAL;

	if (!dev->pm_domain)
		return -EINVAL;

	switch(req->oper) {
	  case SCMI_PWR_OFF:
		  if (is_genpd_on(dev)) {
			ret = pm_runtime_put_sync(dev);
		  }
		  break;
	  case SCMI_PWR_ON:
		  if (!is_genpd_on(dev)) {
			ret = pm_runtime_resume_and_get(dev);
		  }
		  break;

	  default:
		dev_warn(dev, "power operation(%d) not supported\n", req->oper);
		ret = -EINVAL;
	}

	if (ret < 0)
		dev_err(dev, "power operation(%d) failed with err=%d\n", req->oper, ret);

	return ret >= 0 ? 0 : ret;
}

static int dev_pm_opp_apply_level(struct device *dev, unsigned int level)
{
	struct dev_pm_opp *opp = dev_pm_opp_find_level_exact(dev, level);
	int ret = 0;

	if (IS_ERR(opp))
		return -EINVAL;

	ret = dev_pm_opp_set_opp(dev, opp);
	dev_pm_opp_put(opp);

	return ret;
}

static int do_performance_operation(scmi_oper_ioctl_t *req,
				    struct qcom_uscmi_dev *uscmi)
{
	struct device *dev;
	int ret = 0;
	int index;

	dev = get_pd_dev(uscmi, req->name, &index);
	if (!dev)
		return -EINVAL;

	if (req->proto != SCMI_PROTO_PERFORMANCE)
		return -EINVAL;

	if (dev_pm_opp_get_opp_count(dev) <= 0)
		return -EINVAL;

	switch(req->oper) {
	  case SCMI_PRF_LVL_SET:
		ret = dev_pm_opp_apply_level(dev, req->level);
		break;

	  default:
		dev_warn(dev, "performance operation(%d) not supported\n", req->oper);
		ret = -EINVAL;
	}

	if (!ret && uscmi->pd_list) {
		/* multiple domains don't get turned on automatically hence
		 * need to do refcount increment here to get the set level
		 * request take effect.
		 */
		ret = pm_runtime_resume_and_get(dev);
		if (ret >= 0)
			ret = pm_runtime_put_sync(dev);
	}

	if (ret)
		dev_err(dev, "performance operation(%d) failed with err=%d\n", req->oper, ret);

	return ret;
}

static int do_reset_operation(scmi_oper_ioctl_t *req,
			      struct qcom_uscmi_dev *uscmi)
{
	struct device *dev = uscmi->dev;
	const char *id = strlen(req->name) ? req->name : NULL;
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

	guard(mutex)(&uscmi->uscmi_lock);
	switch (cmd) {
	  case SCMI_IOCTL_PRF:
		err = do_performance_operation(&req, uscmi);
		break;

	  case SCMI_IOCTL_RST:
		err = do_reset_operation(&req, uscmi);
		break;

	  case SCMI_IOCTL_PWR:
		err = do_power_operation(&req, uscmi);
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

	if (!dev->pm_domain) {
		/* multiple domains used */
		err = dev_pm_domain_attach_list(dev, NULL, &uscmi->pd_list);
		if (err < 0) {
			dev_err(dev, "multi domain attach failed(ret=%d)\n", err);
			return err;
		}
	}

	if (!of_property_read_string(np, "qcom,dev-name", &name))
		uscmi->name = devm_kstrdup(dev, name, GFP_KERNEL);
	else
		uscmi->name = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);

	mutex_init(&uscmi->uscmi_lock);
	uscmi->miscdev.minor = MISC_DYNAMIC_MINOR;
	uscmi->miscdev.name = uscmi->name;
	uscmi->miscdev.fops = &qcom_uscmi_fops;

	err = misc_register(&uscmi->miscdev);
	if (err) {
		dev_err(dev, "misc_register failed(ret=%d)\n", err);
		mutex_destroy(&uscmi->uscmi_lock);
		dev_pm_domain_detach_list(uscmi->pd_list);
		return err;
	}

	if (!uscmi->pd_list) {
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);
	}

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
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = qcom_uscmi_probe,
};
module_platform_driver(qcom_uscmi_driver);

MODULE_DESCRIPTION("Qualcomm userspace scmi interface driver");
MODULE_LICENSE("GPL v2");

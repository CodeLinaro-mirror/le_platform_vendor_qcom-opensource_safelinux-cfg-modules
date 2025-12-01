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
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <uapi/misc/qcom_uscmi.h>

#define CREATE_TRACE_POINTS
#include "qcom_uscmi_trace.h"

struct qcom_uscmi_dev {
	struct miscdevice miscdev;
	struct device *dev;
	const char *name;
	struct mutex uscmi_lock;
	struct dev_pm_domain_list *pd_list;
	u32 num_pds;
	bool skip_system_pm;
};

#define miscdev_to_data(d) container_of(d, struct qcom_uscmi_dev, miscdev)

/**
 * qcom_uscmi_open - Open handler for USCMI device
 * @inode: inode structure
 * @filp: file structure
 *
 * Return: 0 on success
 */
static int qcom_uscmi_open(struct inode *inode, struct file *filp)
{
	/* No initialization needed */
	return 0;
}

/**
 * qcom_uscmi_release - Release handler for USCMI device
 * @inode: inode structure
 * @filp: file structure
 *
 * Return: 0 on success
 */
static int qcom_uscmi_release(struct inode *inode, struct file *filp)
{
	/* No cleanup needed */
	return 0;
}

/**
 * get_pd_dev - Get power domain device by name
 * @uscmi: USCMI device structure
 * @name: Power domain name to find
 * @idx: Output parameter for domain index
 *
 * Return: Device pointer on success, NULL on failure
 */
static struct device * get_pd_dev(struct qcom_uscmi_dev *uscmi, char *name, int *idx)
{
	int index;
	size_t len;

	if (!uscmi->pd_list) { /* single domain */
		*idx = 0;
		return uscmi->dev;
	}

	len = strnlen(name, NAME_LEN);
	if (!len || len == NAME_LEN)
		return NULL;

	name[len] = '\0';
	index = of_property_match_string(uscmi->dev->of_node, "power-domain-names",
						 name);

	if (index < 0) {
		dev_err(uscmi->dev, "power domain %s not found\n", name);
		return NULL;
	}

	*idx = index;
	return uscmi->pd_list->pd_devs[index];
}

/**
 * is_genpd_on - Check if generic power domain is on
 * @dev: Device structure
 *
 * Return: 1 if domain is on, 0 otherwise
 */
static int is_genpd_on(struct device *dev)
{
	struct generic_pm_domain *genpd;

	if (!dev || !dev->pm_domain)
		return 0;

	genpd = pd_to_genpd(dev->pm_domain);

	return (genpd->status == GENPD_STATE_ON);
}

/**
 * is_genpd_always_on - Check if generic power domain is always on
 * @dev: Device structure
 *
 * Return: 1 if domain is always on, 0 otherwise
 */
static int is_genpd_always_on(struct device *dev)
{
	struct generic_pm_domain *genpd;

	if (!dev || !dev->pm_domain)
		return 0;

	genpd = pd_to_genpd(dev->pm_domain);
	dev_dbg(dev, "domain is %s\n", genpd->flags & GENPD_FLAG_ALWAYS_ON ?
		"always on" : "not always on");

	return (genpd->flags & GENPD_FLAG_ALWAYS_ON);
}

/**
 * qcom_uscmi_all_pds_off - Check if all power domains are off
 * @uscmi: USCMI device structure
 *
 * Return: true if all non-always-on domains are off, false otherwise
 */
static bool qcom_uscmi_all_pds_off(struct qcom_uscmi_dev *uscmi)
{
	int i;

	if (!uscmi->num_pds || !uscmi->pd_list || !uscmi->pd_list->pd_devs)
		return true;

	for (i = 0; i < uscmi->num_pds; i++) {
		if (unlikely(!uscmi->pd_list->pd_devs[i]))
			continue;

		/* If any non-DVFS domain is ON, we're not ready to detach */
		if (is_genpd_always_on(uscmi->pd_list->pd_devs[i]))
			continue;

		if (is_genpd_on(uscmi->pd_list->pd_devs[i]))
			return false;
	}

	dev_dbg(uscmi->dev, "all domains are off\n");
	return true;
}

/**
 * qcom_uscmi_attach_domains - Attach power domains if needed
 * @uscmi: USCMI device structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int qcom_uscmi_attach_domains(struct qcom_uscmi_dev *uscmi)
{
	int num_pds;
	ktime_t start_time, end_time;
	s64 duration_us = 0;

	if (!uscmi->skip_system_pm || uscmi->num_pds)
		return 0;

	start_time = ktime_get();
	num_pds = dev_pm_domain_attach_list(uscmi->dev, NULL, &uscmi->pd_list);
	if (num_pds < 0) {
		dev_err(uscmi->dev, "multi domain attach failed (ret=%d)\n", num_pds);
		trace_qcom_uscmi_pd_attach(dev_name(uscmi->dev), num_pds, 0, 0);
		return num_pds;
	}

	dev_dbg(uscmi->dev, "multi domain attach success (num_pds=%d)\n", num_pds);
	uscmi->num_pds = num_pds;
	end_time = ktime_get();
	duration_us = ktime_to_us(ktime_sub(end_time, start_time));
	trace_qcom_uscmi_pd_attach(dev_name(uscmi->dev), num_pds, 0, duration_us);

	return 0;
}

/**
 * qcom_uscmi_detach_domains - Detach power domains if all are off
 * @uscmi: USCMI device structure
 * @operation: Operation type for logging
 * @force: Force detach even if domains are on (e.g., on error)
 */
static void qcom_uscmi_detach_domains(struct qcom_uscmi_dev *uscmi,
				      int operation, bool force)
{
	ktime_t start_time, end_time;
	s64 duration_us = 0;

	if (!uscmi->skip_system_pm || !uscmi->num_pds)
		return;

	start_time = ktime_get();
	if (force || qcom_uscmi_all_pds_off(uscmi)) {
		if (uscmi->pd_list)
			dev_pm_domain_detach_list(uscmi->pd_list);
		dev_dbg(uscmi->dev, "detached domains after operation %d force:%d\n",
			operation, force);
		uscmi->pd_list = NULL;
		uscmi->num_pds = 0;
		end_time = ktime_get();
		duration_us = ktime_to_us(ktime_sub(end_time, start_time));
		trace_qcom_uscmi_pd_detach(dev_name(uscmi->dev), operation, force, duration_us);
	} else {
		dev_dbg(uscmi->dev, "detach skipped after operation %d, some domains are ON\n",
			operation);
	}
}

/**
 * do_power_operation - Execute power domain operation
 * @req: SCMI operation request
 * @uscmi: USCMI device structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int do_power_operation(scmi_oper_ioctl_t *req,
			      struct qcom_uscmi_dev *uscmi)
{
	struct device *dev;
	int ret = 0;
	int index;
	ktime_t start_time, end_time;
	u64 duration_us;

	start_time = ktime_get();

	ret = qcom_uscmi_attach_domains(uscmi);
	if (ret)
		goto out_detach;

	dev = get_pd_dev(uscmi, req->name, &index);
	if (!dev) {
		ret = -EINVAL;
		goto out_detach;
	}

	if (req->proto != SCMI_PROTO_POWER) {
		ret = -EINVAL;
		goto out_detach;
	}

	if (!dev->pm_domain) {
		ret = -EINVAL;
		goto out_detach;
	}

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
		dev_warn(uscmi->dev, "power operation(%d) not supported\n", req->oper);
		ret = -EINVAL;
	}

out_detach:
	if (ret < 0)
		dev_err(uscmi->dev, "power operation(%d) failed with err=%d\n", req->oper, ret);

	end_time = ktime_get();
	duration_us = ktime_to_us(ktime_sub(end_time, start_time));

	trace_qcom_uscmi_power_op(req->name, req->oper, ret, duration_us);
	qcom_uscmi_detach_domains(uscmi, req->oper, ret < 0);

	return ret >= 0 ? 0 : ret;
}

/**
 * dev_pm_opp_apply_level - Apply OPP level to device
 * @dev: Device structure
 * @level: OPP level to apply
 *
 * Return: 0 on success, negative error code on failure
 */
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

/**
 * do_performance_operation - Execute performance operation
 * @req: SCMI operation request
 * @uscmi: USCMI device structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int do_performance_operation(scmi_oper_ioctl_t *req,
				    struct qcom_uscmi_dev *uscmi)
{
	struct device *dev;
	int ret = 0;
	int index;
	ktime_t start_time, end_time;
	u64 duration_us;

	start_time = ktime_get();

	ret = qcom_uscmi_attach_domains(uscmi);
	if (ret)
		goto out_detach;

	dev = get_pd_dev(uscmi, req->name, &index);
	if (!dev) {
		ret = -EINVAL;
		goto out_detach;
	}

	if (req->proto != SCMI_PROTO_PERFORMANCE) {
		ret = -EINVAL;
		goto out_detach;
	}

	if (dev_pm_opp_get_opp_count(dev) <= 0) {
		ret =  -EINVAL;
		goto out_detach;
	}

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

out_detach:
	if (ret < 0)
		dev_err(uscmi->dev, "performance operation %d failed with error %d\n",
			req->oper, ret);

	end_time = ktime_get();
	duration_us = ktime_to_us(ktime_sub(end_time, start_time));

	trace_qcom_uscmi_perf_op(req->name, req->level, ret, duration_us);
	qcom_uscmi_detach_domains(uscmi, req->oper, ret < 0);

	return ret;
}

/**
 * do_reset_operation - Execute reset operation
 * @req: SCMI operation request
 * @uscmi: USCMI device structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int do_reset_operation(scmi_oper_ioctl_t *req,
			      struct qcom_uscmi_dev *uscmi)
{
	struct device *dev = uscmi->dev;
	const char *id;
	struct reset_control *rstc;
	int ret = 0;
	size_t len;
	ktime_t start_time, end_time;
	u64 duration_us;

	start_time = ktime_get();

	if (req->proto != SCMI_PROTO_RESET) {
		ret = -EINVAL;
		goto out;
	}

	len = strnlen(req->name, NAME_LEN);
	if (!len || len == NAME_LEN) {
		ret = -EINVAL;
		goto out;
	}

	req->name[len] = '\0';
	id = req->name;
	rstc = devm_reset_control_get_optional(dev, id);
	if (IS_ERR_OR_NULL(rstc)) {
		ret = -ENODEV;
		goto out;
	}

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
out:
	if (ret)
		dev_err(dev, "reset operation(%d) failed with err=%d\n", req->oper, ret);

	end_time = ktime_get();
	duration_us = ktime_to_us(ktime_sub(end_time, start_time));

	trace_qcom_uscmi_reset_op(req->name, req->oper, ret, duration_us);
	return ret;
}

/**
 * qcom_uscmi_ioctl - IOCTL handler for USCMI device
 * @file: File structure
 * @cmd: IOCTL command
 * @arg: IOCTL argument
 *
 * Return: 0 on success, negative error code on failure
 */
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

/**
 * qcom_uscmi_probe - Probe function for USCMI driver
 * @pdev: Platform device structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int qcom_uscmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct qcom_uscmi_dev *uscmi;
	const char *name;
	int err, num_pds = 0;

	uscmi = devm_kzalloc(dev, sizeof(*uscmi), GFP_KERNEL);
	if (!uscmi)
		return -ENOMEM;

	uscmi->dev = dev;

	if (!dev->pm_domain) {
		/* multiple domains used */
		num_pds = dev_pm_domain_attach_list(dev, NULL, &uscmi->pd_list);
		if (num_pds < 0) {
			dev_err(dev, "multi domain attach failed(ret=%d)\n", num_pds);
			return num_pds;
		}
		uscmi->num_pds = num_pds;
	} else {
		uscmi->num_pds = 1;
	}

	if (!of_property_read_string(np, "qcom,dev-name", &name))
		uscmi->name = devm_kstrdup(dev, name, GFP_KERNEL);
	else
		uscmi->name = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);

	if (of_property_read_bool(uscmi->dev->of_node, "qcom,no-suspend")) {
		dev_info(uscmi->dev, "skip system pm cbs\n");
		uscmi->skip_system_pm = true;
	}

	mutex_init(&uscmi->uscmi_lock);
	uscmi->miscdev.minor = MISC_DYNAMIC_MINOR;
	uscmi->miscdev.name = uscmi->name;
	uscmi->miscdev.fops = &qcom_uscmi_fops;

	err = misc_register(&uscmi->miscdev);
	if (err) {
		dev_err(dev, "misc_register failed(ret=%d)\n", err);
		mutex_destroy(&uscmi->uscmi_lock);
		if (uscmi->pd_list)
			dev_pm_domain_detach_list(uscmi->pd_list);
		return err;
	}

	if (!uscmi->pd_list) {
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);
	}

	pm_runtime_forbid(dev);

	dev_info(dev, "/dev/%s node created\n", uscmi->name);

	platform_set_drvdata(pdev, uscmi);
	return 0;
}

/**
 * qcom_uscmi_runtime_nop - No-op runtime PM callback
 * @dev: Device structure
 *
 * Return: 0
 */
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

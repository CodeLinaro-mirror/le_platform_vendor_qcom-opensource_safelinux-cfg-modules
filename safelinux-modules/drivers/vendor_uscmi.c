/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/qcom_scmi_vendor.h>
#include <linux/scmi_protocol.h>
#include <uapi/misc/vendor_uscmi.h>
#define CREATE_TRACE_POINTS
#include "vendor_uscmi_trace.h"

extern struct bus_type scmi_bus_type;

struct qcom_vendor_uscmi_dev {
	struct miscdevice miscdev;
	struct device *dev;
	const char *name;
	const struct qcom_scmi_vendor_ops *ops;
	struct scmi_protocol_handle *ph;
	struct scmi_device *sdev;
	u64 algo_str;
};

#define miscdev_to_priv(d) container_of(d, struct qcom_vendor_uscmi_dev, miscdev)

static int scmi_vendor_open(struct inode *inode, struct file *filp)
{
	/* do nothing */
	return 0;
}

static int scmi_vendor_release(struct inode *inode, struct file *filp)
{
	/* do nothing */
	return 0;
}

static long scmi_vendor_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	char __user *argp = (char __user *)arg;
	scmi_vendor_msg_t kmsg;
	void *payload;
	int ret = 0;
	u64 size;
	struct qcom_vendor_uscmi_dev *uscmi = miscdev_to_priv(file->private_data);

	if (!uscmi) {
		pr_err("%s invalid device file\n", __func__);
		return -EINVAL;
	}

	ret = copy_from_user(&kmsg, argp, sizeof(kmsg));
	if (ret)
		goto err;

	if (cmd == GET_PARAM)
		size = kmsg.tx_size > kmsg.rx_size ? kmsg.tx_size : kmsg.rx_size;
	else
		size = kmsg.tx_size;

	payload = kzalloc(size, GFP_KERNEL);
	if (!payload) {
		ret = -ENOMEM;
		goto err;
        }

	ret = copy_from_user(payload, kmsg.msg, kmsg.tx_size);
	if (ret) {
		kfree(payload);
		goto err;
	}

	switch (cmd) {
	case SET_PARAM:
		ret = uscmi->ops->set_param(uscmi->ph, payload, uscmi->algo_str,
					    kmsg.param_id, kmsg.tx_size);
		break;
	case GET_PARAM:
		ret = uscmi->ops->get_param(uscmi->ph, payload, uscmi->algo_str,
					    kmsg.param_id, kmsg.tx_size, kmsg.rx_size);
		if (!ret)
			ret = copy_to_user(kmsg.msg, payload, kmsg.rx_size);
		break;
	case START_ACTIVITY:
		ret = uscmi->ops->start_activity(uscmi->ph, payload,
						 uscmi->algo_str, kmsg.param_id,
						 kmsg.tx_size);
		break;
	case STOP_ACTIVITY:
		ret = uscmi->ops->stop_activity(uscmi->ph, payload,
						uscmi->algo_str, kmsg.param_id,
						kmsg.tx_size);
		break;

	default:
		ret = -ENOTTY;
		break;
	}

	kfree(payload);
err:
	trace_vendor_uscmi_ioctl(cmd, size, ret, uscmi->algo_str, kmsg.param_id);
	return ret;
}

static const struct file_operations scmi_vendor_fops = {
	.owner = THIS_MODULE,
	.open = scmi_vendor_open,
	.release = scmi_vendor_release,
	.unlocked_ioctl = scmi_vendor_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int uscmi_vendor_probe(struct platform_device *pdev)
{
	int ret;
	const char *name;
	struct device *dev = &pdev->dev, *scmi_dev;
	struct device_node *np, *scmi_node;
	struct qcom_vendor_uscmi_dev *uscmi;
	struct miscdevice miscdev;

	uscmi = devm_kzalloc(dev, sizeof(*uscmi), GFP_KERNEL);
	if (!uscmi)
		return -ENOMEM;

	uscmi->dev = dev;
	np = dev->of_node;

	if (!of_property_read_string(np, "qcom,dev-name", &name))
		uscmi->name = devm_kstrdup(dev, name, GFP_KERNEL);
	else
		uscmi->name = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);

	ret = of_property_read_u64(np, "qcom,algo", &uscmi->algo_str);
	if (ret) {
		dev_err(dev, "qcom algo string not found %s\n", __func__);
		return ret;
	}

	scmi_node = of_parse_phandle(np, "qcom,vendor", 0);
	if (!scmi_node) {
		dev_err(dev, "qcom vendor prop not found %s\n", __func__);
		return -ENODEV;
	}

	scmi_dev = bus_find_device_by_of_node(&scmi_bus_type, scmi_node);
	if (!scmi_dev) {
		dev_err(dev, "scmi dev not found %s\n", __func__);
		return -ENODEV;
	}

	dev_dbg(dev, "device found: %s\n", dev_name(scmi_dev));
	of_node_put(scmi_node);

	uscmi->sdev = dev_get_drvdata(scmi_dev);
	if (!uscmi->sdev) {
		pr_info("drv data not set %s\n", __func__);
		return -EINVAL;
	}

	uscmi->ops = uscmi->sdev->handle->devm_protocol_get(uscmi->sdev,
			QCOM_SCMI_VENDOR_PROTOCOL, &uscmi->ph);
	if (IS_ERR(uscmi->ops)) {
		ret = PTR_ERR(uscmi->ops);
		uscmi->ops = NULL;
		dev_err(dev, "Error getting vendor protocol ops: %d\n", ret);
		return ret;
	}

	uscmi->miscdev.minor = MISC_DYNAMIC_MINOR;
	uscmi->miscdev.name = uscmi->name;
	uscmi->miscdev.fops = &scmi_vendor_fops;

	ret = misc_register(&uscmi->miscdev);
	if (ret) {
		dev_err(dev, "misc_register failed(ret=%d)\n", ret);
		return ret;
	}

	dev_info(dev, "/dev/%s node created\n", uscmi->name);

	return ret;
}

static const struct of_device_id  uscmi_vendor_match_table[] = {
	{ .compatible = "qcom,uscmi-vendor" },
	{ },
};
MODULE_DEVICE_TABLE(of, uscmi_vendor_match_table);

static struct platform_driver uscmi_vendor_drv = {
	.driver = {
		.name = "qcom-uscmi-vendor",
		.of_match_table = uscmi_vendor_match_table,
		.suppress_bind_attrs = true,
	},
	.probe		= uscmi_vendor_probe,
};
module_platform_driver(uscmi_vendor_drv);

MODULE_DESCRIPTION("QCOM SCMI vendor uscmi driver");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only
 /* Copyright (C) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/reset-controller.h>
#include <linux/arm-smccc.h>
#include <linux/miscdevice.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/cdev.h>
#include <uapi/misc/scm_user_intf.h>

#define MAX_SCM_USER_INTFS      32 /*maximum number of user intf devices*/

static dev_t scm_user_intf_devt;

static int  scm_id;

struct scm_hand_shake {
       unsigned int svc;
       unsigned int cmd;
       unsigned int arginfo;
       unsigned int args_buffer[MAX_QCOM_SCM_ARGS];
       unsigned int ret;
       unsigned int arg_type;
       unsigned int qcom_scm_res[MAX_QCOM_SCM_RETS];
};

struct scm_dev_data {
	struct platform_device *scm_pdev;
	struct device dev;
	struct cdev cdev;
};

static int qcom_scm_open(struct inode *inode, struct file *filp)
{
	struct scm_dev_data *dev_data = container_of(inode->i_cdev, struct scm_dev_data,
                                       cdev);

	filp->private_data = dev_data;
	return 0;
}

static long scm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct scm_hand_shake scm_data;
	void __user *ip = (void __user *)arg;
	struct qcom_scm_desc desc;
	int id, i, no_of_args;
	struct qcom_scm_res res;
	struct scm_dev_data *dev_data = (struct scm_dev_data *)file->private_data;
	struct platform_device *scm_pdev = dev_data->scm_pdev;

	if (cmd != SCM_HAND_SHAKE_IOCTL)
		return -EFAULT;

	if (copy_from_user(&scm_data, ip, sizeof(struct scm_hand_shake)))
		return -EFAULT;

	no_of_args = scm_data.arginfo & 0x0F;

	if (no_of_args > MAX_QCOM_SCM_ARGS)
		return -EINVAL;

	switch(scm_data.svc) {
		case QCOM_SCM_SVC_BOOT:
		case QCOM_SCM_SVC_PIL:
		case QCOM_SCM_SVC_IO:
		case QCOM_SCM_SVC_INFO:
		case QCOM_SCM_SVC_MP:
		case QCOM_SCM_SVC_OCMEM:
		case QCOM_SCM_SVC_ES:
		case QCOM_SCM_SVC_HDCP:
		case QCOM_SCM_SVC_LMH:
		case QCOM_SCM_SVC_SMMU_PROGRAM:
		{
			desc.svc = scm_data.svc;
			desc.cmd = scm_data.cmd;
			desc.owner = ARM_SMCCC_OWNER_SIP;
			desc.arginfo = scm_data.arginfo;

			for (id = 0; id < no_of_args; id++)
				desc.args[id] = scm_data.args_buffer[id];

			scm_data.ret =  qcom_scm_call(&scm_pdev->dev, &desc, &res);
			dev_info(&dev_data->dev, "scm ioctl - ret: %d\n",  scm_data.ret);

			for (i = 0; i < MAX_QCOM_SCM_RETS; i++)
				scm_data.qcom_scm_res[i] = res.result[i];

			if (copy_to_user(ip, &scm_data, sizeof(struct scm_hand_shake)))
				return -EFAULT;
		}
		break;

		default:
			dev_err(&dev_data->dev, "Unsupported scm service:"
					"%d and command:%d  \n",
					scm_data.svc, scm_data.cmd);
			return -EINVAL;
	}
	return 0;
}

static const struct file_operations qcom_scm_fops = {
	.open = qcom_scm_open,
	.unlocked_ioctl = scm_ioctl,
};

static int qcom_scm_intf_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const char *scm_dev_name = of_get_property(np, "scm_dev", NULL);
	struct platform_device *scm_pdev;
	struct scm_dev_data *dev_data;

	if (!scm_dev_name)
		return -EINVAL;

	np = of_find_node_by_name(NULL, scm_dev_name);
	if (!np)
		return -ENODEV;

	scm_pdev = of_find_device_by_node(np);
	if (!scm_pdev)
		return -ENODEV;

	dev_data = devm_kzalloc(dev, sizeof(struct scm_dev_data), GFP_KERNEL);
	if (!dev_data)
		return -ENOMEM;

	device_initialize(&dev_data->dev);
	dev_data->dev.devt = MKDEV(MAJOR(scm_user_intf_devt), scm_id);
        dev_data->dev.parent = &pdev->dev;
        dev_set_name(&dev_data->dev, "scm_dev%d", scm_id++);
	dev_data->scm_pdev = scm_pdev;

	cdev_init(&dev_data->cdev, &qcom_scm_fops);

	err = cdev_device_add(&dev_data->cdev, &dev_data->dev);
	if (err){
		dev_err(&pdev->dev, "failed to register cdev. err %d\n", err);
		return err;
	}

	return 0;
}

static const struct of_device_id qcom_scm_intf_dt_match[] = {
	{ .compatible = "qcom,scm-user-intf",
        },
	{}
};

MODULE_DEVICE_TABLE(of, qcom_scm_intf_dt_match);

static struct platform_driver qcom_scm_intf_driver = {
        .driver = {
                .name   = "qcom_scm_intf",
                .of_match_table = qcom_scm_intf_dt_match,
                .suppress_bind_attrs = true,
        },
        .probe = qcom_scm_intf_probe,
};

static int __init qcom_scm_intf_init(void)
{
	int err;

	err = alloc_chrdev_region(&scm_user_intf_devt, 0, MAX_SCM_USER_INTFS,
				"qcom_scm_intf");
	if (err < 0) {
		pr_err("qcom_scm_intf: unable to allocate char dev region\n");
		return err;
	}

	return platform_driver_register(&qcom_scm_intf_driver);
}

device_initcall(qcom_scm_intf_init);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. user interface SCM driver");
MODULE_LICENSE("GPL v2");

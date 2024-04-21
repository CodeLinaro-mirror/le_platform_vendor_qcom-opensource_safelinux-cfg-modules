// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, 2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/firmware/qcom/qcom_scm.h>

enum qcom_download_mode {
	QCOM_DOWNLOAD_FULLDUMP  = 0x10,
	QCOM_DOWNLOAD_MINIDUMP  = 0x20,
	QCOM_DOWNLOAD_BOTHDUMP  = (QCOM_DOWNLOAD_FULLDUMP | QCOM_DOWNLOAD_MINIDUMP),
};

static enum qcom_download_mode dump_mode = QCOM_DOWNLOAD_FULLDUMP;
static u64 dload_mode_addr;

static int get_dump_mode(int *mode)
{
	int ret;

	ret = qcom_scm_io_readl(dload_mode_addr, mode);
	if (ret)
		pr_err("failed to get dump mode, ret = %d\n", ret);

	return ret;
}

static int set_dump_mode(enum qcom_download_mode mode)
{
	int ret;

	ret = qcom_scm_io_writel(dload_mode_addr, mode);
	if (ret)
		pr_err("failed to set dump mode as '0x%x', ret = %d\n", mode, ret);
	else
		dump_mode = mode;
	return ret;
}

struct reset_attribute {
	struct attribute        attr;
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct attribute *attr,
			const char *buf, size_t count);
};
#define to_reset_attr(_attr) \
	container_of(_attr, struct reset_attribute, attr)

static ssize_t attr_show(struct kobject *kobj, struct attribute *attr,
			 char *buf)
{
	struct reset_attribute *reset_attr = to_reset_attr(attr);
	ssize_t ret = -EIO;

	if (reset_attr->show)
		ret = reset_attr->show(kobj, attr, buf);
	return ret;
}

static ssize_t attr_store(struct kobject *kobj, struct attribute *attr,
			  const char *buf, size_t count)
{
	struct reset_attribute *reset_attr = to_reset_attr(attr);
	ssize_t ret = -EIO;

	if (reset_attr->store)
		ret = reset_attr->store(kobj, attr, buf, count);
	return ret;
}

static const struct sysfs_ops reset_sysfs_ops = {
	.show   = attr_show,
	.store  = attr_store,
};

static struct kobj_type qcom_dload_mode_kobj_type = {
	.sysfs_ops      = &reset_sysfs_ops,
};

static ssize_t dload_mode_show(struct kobject *kobj, struct attribute *this,
			       char *buf)
{
	const char *mode;

	switch ((unsigned int)dump_mode) {
	case QCOM_DOWNLOAD_FULLDUMP:
		mode = "full";
		break;
	case QCOM_DOWNLOAD_MINIDUMP:
		mode = "mini";
		break;
	case QCOM_DOWNLOAD_BOTHDUMP:
		mode = "both";
		break;
	default:
		mode = "unknown";
		break;
	}
	return scnprintf(buf, PAGE_SIZE, "DLOAD dump type: %s\n", mode);
}

static ssize_t dload_mode_store(struct kobject *kobj, struct attribute *this,
				const char *buf, size_t count)
{
	enum qcom_download_mode mode;

	if (sysfs_streq(buf, "full")) {
		mode = QCOM_DOWNLOAD_FULLDUMP;
	} else if (sysfs_streq(buf, "mini")) {
		mode = QCOM_DOWNLOAD_MINIDUMP;
	} else if (sysfs_streq(buf, "both")) {
		mode = QCOM_DOWNLOAD_BOTHDUMP;
	} else {
		pr_err("Invalid dump mode request...\n");
		pr_err("Supported modes are : 'full', 'mini', or 'both'\n");
		return -EINVAL;
	}

	return set_dump_mode(mode) ? : count;
}

static struct reset_attribute attr_dload_mode = __ATTR_RW(dload_mode);

static int qcom_scm_find_dload_mode_address(struct device_node *np, u64 *addr)
{
	struct device_node *tcsr;
	struct resource res;
	u32 offset;
	int ret = -1;

	tcsr = of_parse_phandle(np, "qcom,dload-mode", 0);
	if (!tcsr)
		return ret;

	ret = of_address_to_resource(tcsr, 0, &res);
	of_node_put(tcsr);
	if (ret)
		return ret;

	ret = of_property_read_u32_index(np, "qcom,dload-mode", 1, &offset);
	if (ret < 0)
		return ret;

	*addr = res.start + offset;
	return 0;
}

static int qcom_dload_mode_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	static struct kobject kobj;
	struct device_node *scm_dev;
	int ret, temp;

	scm_dev = of_find_node_by_name(NULL, "qcom_scm");
	if (!scm_dev) {
		dev_err(dev, "Unable to find 'qcom_scm' node!\n");
		return -ENODEV;
	}
	of_node_put(scm_dev);

	ret = qcom_scm_find_dload_mode_address(scm_dev, &dload_mode_addr);
	if (ret < 0) {
		dev_err(dev, "Unable to find dload_mode_address!\n");
		return ret;
	}

	ret = kobject_init_and_add(&kobj, &qcom_dload_mode_kobj_type,
				   kernel_kobj, "dload");
	if (ret) {
		dev_err(dev, "Error in creation kobject_add!\n");
		kobject_put(&kobj);
		return ret;
	}

	ret = sysfs_create_file(&kobj, &attr_dload_mode.attr);
	if (ret) {
		dev_err(dev, "Error in creation sysfs_create_group!\n");
		kobject_del(&kobj);
		return ret;
	}
	dump_mode = get_dump_mode(&temp) ? dump_mode : temp;

	return 0;
}

static const struct of_device_id of_qcom_dload_mode_match[] = {
	{.compatible = "qcom,dload-mode", },
	{}
};
MODULE_DEVICE_TABLE(of, of_qcom_dload_mode_match);

static struct platform_driver qcom_dload_mode_driver = {
	.probe = qcom_dload_mode_probe,
	.driver = {
		.name = "qcom-dload-mode",
		.of_match_table = of_qcom_dload_mode_match,
	},
};

module_platform_driver(qcom_dload_mode_driver);
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Download Mode Driver");
MODULE_LICENSE("GPL v2");

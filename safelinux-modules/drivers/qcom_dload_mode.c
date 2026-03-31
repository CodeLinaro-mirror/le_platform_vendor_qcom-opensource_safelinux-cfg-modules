// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, 2021 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/firmware/qcom/qcom_scm.h>

/*
 * TZ dump capability query (SMC 0x02000310):
 *
 * Response 1 (dumps_allowed):
 *   0 = No dumps allowed
 *   1 = SEC_DMP_ALLOWED  - secure full dump (superset, also permits minidump)
 *   2 = NSEC_DMP_ALLOWED - non-secure full dump (does NOT permit minidump)
 *
 * Response 3 (md_flags) for newer SoC family (bit31=1):
 *   bit1  = MD_APPS_NSEC_SUBSYS_ENABLED - minidump gate
 *   bit31 = TARGET_FAMILY - set for newer SoC family
 *
 * Response 3 (md_flags) for previous SoC family (bit31=0):
 *   bit5 = SEC_DBG_ENABLE_APPS_ENCRYPTED_MINI_DUMPS
 */

/* md_flags bits for newer SoC family (bit31=1) */
#define MD_TARGET_FAMILY_BIT		BIT(31)
#define MD_APPS_NSEC_SUBSYS_ENABLED_BIT	BIT(1)

/* md_flags bits for previous SoC family (bit31=0) */
#define APDP_APPS_MINI_DUMPS_BIT        BIT(5)

/* dumps_allowed values */
#define SEC_DMP_ALLOWED     1
#define NSEC_DMP_ALLOWED    2

enum qcom_download_mode {
	QCOM_DOWNLOAD_NODUMP    = 0x00,
	QCOM_DOWNLOAD_FULLDUMP  = 0x10,
	QCOM_DOWNLOAD_MINIDUMP  = 0x20,
	QCOM_DOWNLOAD_BOTHDUMP  = (QCOM_DOWNLOAD_FULLDUMP | QCOM_DOWNLOAD_MINIDUMP),
};

enum qcom_download_dest {
	QCOM_DOWNLOAD_DEST_UNKNOWN = -1,
	QCOM_DOWNLOAD_DEST_QPST = 0,
	QCOM_DOWNLOAD_DEST_EMMC = 2,
};

static enum qcom_download_mode dump_mode = QCOM_DOWNLOAD_FULLDUMP;
static u64 dload_mode_addr;
static void __iomem *dload_dest_addr;

static int get_dump_mode(int *mode)
{
	int ret;

	ret = qcom_scm_io_readl(dload_mode_addr, mode);
	if (ret)
		pr_err("dload: failed to read TCSR: addr=0x%llx ret=%d\n",
		       dload_mode_addr, ret);

	return ret;
}

static int set_dump_mode(enum qcom_download_mode mode)
{
	int ret, readback;

	ret = qcom_scm_io_writel(dload_mode_addr, mode);
	if (ret) {
		pr_err("dload: write failed: mode=0x%x ret=%d\n",
		       mode, ret);
		return ret;
	}

	/*
	 * qcom_scm_io_writel() returns 0 if the SCM call transport succeeded,
	 * but TZ/APDP policy may silently accept the call while not honoring
	 * the write (or writing a different value). A readback is required to
	 * confirm the value was actually accepted.
	 */
	ret = get_dump_mode(&readback);
	if (ret)
		return ret;

	if ((enum qcom_download_mode)readback != mode) {
		pr_err("dload: TZ/APDP rejected mode 0x%x, read back 0x%x\n",
		       mode, readback);
		return -EIO;
	}

	dump_mode = mode;
	return 0;
}

struct qcom_dload_caps {
	u32 dumps_allowed;
	u32 md_flags;
};

static int qcom_dload_query_caps(struct qcom_dload_caps *caps)
{
	u32 dload_cookie;

	return qcom_scm_get_dump_mode_caps(&caps->dumps_allowed,
					   &dload_cookie,
					   &caps->md_flags);
}

/*
 * Select and apply the best dump mode based on TZ policy.
 *
 * For newer SoC family (bit31=1):
 *   full dump: dumps_allowed == SEC_DMP_ALLOWED or NSEC_DMP_ALLOWED
 *   mini dump: dumps_allowed == SEC_DMP_ALLOWED (superset)
 *              OR md_flags & MD_APPS_NSEC_SUBSYS_ENABLED (bit1)
 *
 * For previous SoC family (bit31=0):
 *   full dump: dumps_allowed == SEC_DMP_ALLOWED or NSEC_DMP_ALLOWED
 *   mini dump: dumps_allowed == SEC_DMP_ALLOWED (superset)
 *              OR md_flags & APDP_APPS_MINI_DUMPS_BIT (bit5)
 *
 * No dump: write NODUMP (0x00) to TCSR
 *
 * Preference: FULLDUMP > MINIDUMP > NODUMP
 * BOTHDUMP (0x30) is not selected here; callers can set it via sysfs
 * if the platform supports simultaneous full and mini dump collection.
 *
 * Returns the result of set_dump_mode() so the caller can handle failures.
 */
static int qcom_dload_apply_best_mode(const struct qcom_dload_caps *caps)
{
	bool new_family = !!(caps->md_flags & MD_TARGET_FAMILY_BIT);
	bool full_ok, mini_ok;

	full_ok = (caps->dumps_allowed == SEC_DMP_ALLOWED ||
		   caps->dumps_allowed == NSEC_DMP_ALLOWED);

	if (new_family)
		mini_ok = (caps->dumps_allowed == SEC_DMP_ALLOWED) ||
			  !!(caps->md_flags & MD_APPS_NSEC_SUBSYS_ENABLED_BIT);
	else
		mini_ok = (caps->dumps_allowed == SEC_DMP_ALLOWED) ||
			  !!(caps->md_flags & APDP_APPS_MINI_DUMPS_BIT);

	if (full_ok)
		return set_dump_mode(QCOM_DOWNLOAD_FULLDUMP);
	else if (mini_ok)
		return set_dump_mode(QCOM_DOWNLOAD_MINIDUMP);
	else
		return set_dump_mode(QCOM_DOWNLOAD_NODUMP);
}

static void set_download_dest(enum qcom_download_dest dest)
{
	if (dload_dest_addr)
		__raw_writel(dest, dload_dest_addr);
}

static enum qcom_download_dest get_download_dest(void)
{
	if (dload_dest_addr)
		return __raw_readl(dload_dest_addr);
	else
		return QCOM_DOWNLOAD_DEST_UNKNOWN;
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

static struct kobj_type qcom_dload_kobj_type = {
	.sysfs_ops      = &reset_sysfs_ops,
};

static ssize_t dload_mode_show(struct kobject *kobj, struct attribute *this,
			       char *buf)
{
	const char *mode;
	int val;

	if (get_dump_mode(&val))
		return scnprintf(buf, PAGE_SIZE, "DLOAD dump type: unknown\n");

	switch ((unsigned int)val) {
	case QCOM_DOWNLOAD_NODUMP:
		mode = "off";
		break;
	case QCOM_DOWNLOAD_FULLDUMP:
		mode = "full";
		break;
	case QCOM_DOWNLOAD_MINIDUMP:
		mode = "mini";
		break;
	case QCOM_DOWNLOAD_BOTHDUMP:
		mode = "full,mini";
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

	if (sysfs_streq(buf, "off")) {
		mode = QCOM_DOWNLOAD_NODUMP;
	} else if (sysfs_streq(buf, "full")) {
		mode = QCOM_DOWNLOAD_FULLDUMP;
	} else if (sysfs_streq(buf, "mini")) {
		mode = QCOM_DOWNLOAD_MINIDUMP;
	} else if (sysfs_streq(buf, "full,mini")) {
		mode = QCOM_DOWNLOAD_BOTHDUMP;
	} else {
		pr_err("dload: invalid mode '%.*s'. Supported: off, full, mini, full,mini\n",
		       (int)count, buf);
		return -EINVAL;
	}

	return set_dump_mode(mode) ? : count;
}
static struct reset_attribute attr_dload_mode = __ATTR_RW(dload_mode);

static ssize_t emmc_dload_show(struct kobject *kobj,
			       struct attribute *this,
			       char *buf)
{
	if (!dload_dest_addr)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			get_download_dest() == QCOM_DOWNLOAD_DEST_EMMC);
}

static ssize_t emmc_dload_store(struct kobject *kobj,
				struct attribute *this,
				const char *buf, size_t count)
{
	int ret;
	bool enabled;

	if (!dload_dest_addr)
		return -ENODEV;

	ret = kstrtobool(buf, &enabled);
	if (ret < 0)
		return ret;

	if (enabled)
		set_download_dest(QCOM_DOWNLOAD_DEST_EMMC);
	else
		set_download_dest(QCOM_DOWNLOAD_DEST_QPST);

	return count;
}
static struct reset_attribute attr_emmc_dload = __ATTR_RW(emmc_dload);

static struct attribute *qcom_dload_attrs[] = {
	&attr_emmc_dload.attr,
	&attr_dload_mode.attr,
	NULL
};

static struct attribute_group qcom_dload_attr_group = {
	.attrs = qcom_dload_attrs,
};

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

static void __iomem *map_prop_mem(const char *propname)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, propname);
	void __iomem *addr;

	if (!np) {
		pr_err("Unable to find DT property: %s\n", propname);
		return NULL;
	}

	addr = of_iomap(np, 0);
	of_node_put(np);
	if (!addr)
		pr_err("Unable to map memory for DT property: %s\n", propname);

	return addr;
}

static int qcom_dload_remove(struct platform_device *pdev)
{
	if (dload_dest_addr)
		iounmap(dload_dest_addr);

	return 0;
}

static int qcom_dload_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	static struct kobject kobj;
	struct device_node *scm_dev;
	struct qcom_dload_caps caps = {0};
	int ret;

	scm_dev = of_find_node_by_name(NULL, "qcom_scm");
	if (!scm_dev) {
		dev_err(dev, "Unable to find 'qcom_scm' node!\n");
		return -ENODEV;
	}

	ret = qcom_scm_find_dload_mode_address(scm_dev, &dload_mode_addr);
	of_node_put(scm_dev);
	if (ret < 0) {
		dev_err(dev, "Unable to find dload_mode_address!\n");
		return ret;
	}

	ret = kobject_init_and_add(&kobj, &qcom_dload_kobj_type,
				   kernel_kobj, "dload");
	if (ret) {
		dev_err(dev, "Error in creation kobject_add!\n");
		kobject_put(&kobj);
		return ret;
	}

	ret = sysfs_create_group(&kobj, &qcom_dload_attr_group);
	if (ret) {
		dev_err(dev, "Error in creation sysfs_create_group!\n");
		kobject_del(&kobj);
		kobject_put(&kobj);
		return ret;
	}

	dload_dest_addr = map_prop_mem("qcom,msm-imem-dload-type");
	if (!dload_dest_addr)
		dev_err(dev, "Failed to map dload destination address!!\n");

	ret = qcom_dload_query_caps(&caps);
	if (ret) {
		dev_warn(dev, "dload: caps query failed (ret=%d), using default FULLDUMP\n",
			 ret);
		ret = set_dump_mode(dump_mode);
		if (ret) {
			dev_err(dev, "dload: default dump mode rejected by TZ\n");
			goto err_cleanup;
		}
	} else {
		ret = qcom_dload_apply_best_mode(&caps);
		if (ret) {
			dev_err(dev, "dload: failed to apply best mode: ret=%d\n",
				ret);
			goto err_cleanup;
		}
	}

	return 0;

err_cleanup:
	if (dload_dest_addr) {
		iounmap(dload_dest_addr);
		dload_dest_addr = NULL;
	}
	sysfs_remove_group(&kobj, &qcom_dload_attr_group);
	kobject_del(&kobj);
	kobject_put(&kobj);
	return ret;
}

static int qcom_dload_resume(struct device *dev)
{
	enum qcom_download_mode mode = dump_mode;
	int ret;

	switch (mode) {
	case QCOM_DOWNLOAD_NODUMP:
	case QCOM_DOWNLOAD_FULLDUMP:
	case QCOM_DOWNLOAD_MINIDUMP:
	case QCOM_DOWNLOAD_BOTHDUMP:
		ret = set_dump_mode(mode);
		if (ret) {
			dev_err(dev, "dload: resume: restore failed, ret=%d\n",
				ret);
			return ret;
		}
		break;
	default:
		dev_warn(dev, "dload: resume: skipped, invalid mode=0x%x\n",
			 mode);
		break;
	}

	return 0;
}

static const struct dev_pm_ops qcom_dload_pm_ops = {
	.resume  = qcom_dload_resume,
};

static const struct of_device_id of_qcom_dload_match[] = {
	{.compatible = "qcom,dload-mode", },
	{}
};
MODULE_DEVICE_TABLE(of, of_qcom_dload_match);

static struct platform_driver qcom_dload_driver = {
	.probe = qcom_dload_probe,
	.remove = qcom_dload_remove,
	.driver = {
		.name = "qcom-dload-mode",
		.of_match_table = of_qcom_dload_match,
		.pm = &qcom_dload_pm_ops,
	},
};

module_platform_driver(qcom_dload_driver);
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Download Mode Driver");
MODULE_LICENSE("GPL v2");

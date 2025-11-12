// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#define pr_fmt(fmt)	"[qcom_l3cc:%s:%d] " fmt, __func__, __LINE__
#define dev_fmt pr_fmt

#include <asm/barrier.h>
#include <linux/bitops.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/types.h>
#include <uapi/misc/qcom_l3_cache_config.h>


#define CLUSTER_THREAD_SID_EL1		sys_reg(3, 0, 15, 4, 0)
#define CLUSTER_ACP_SID_EL1			sys_reg(3, 0, 15, 4, 1)
#define CLUSTER_STASH_SID_EL1		sys_reg(3, 0, 15, 4, 2)

/* Cluster Partition Control Register */
#define CLUSTER_PCR_EL1				sys_reg(3, 0, 15, 4, 3)

#define CLUSTER_BUS_QOS_EL1			sys_reg(3, 0, 15, 4, 4)

#define BITS_PER_SID					(4u)
#define CLUSTER_ACP_SID_EL1_MASK		GENMASK(3, 0)
#define CLUSTER_STASH_SID_EL1_MASK		GENMASK(3, 0)
#define CLUSTER_THREAD_SID_EL1_MASK		GENMASK(3, 0)
#define L3_WAYS_SID_MASK(sid)			GENMASK((sid*BITS_PER_SID)+3, (sid*BITS_PER_SID))
#define MAX_CACHE_WAYS_CONFIG			0x1111

struct qcom_l3cc_drv {
	u32 consumer_cookie;
	const char *consumer;
	const char *name;
	struct mutex update_lock;
	int nr_cpus;
	int nr_clusters;
	int max_cpu;
	struct device *dev;
	struct miscdevice mdev;
};

#define mdev_to_drv(d) container_of(d, struct qcom_l3cc_drv, mdev)

static bool qcom_l3cc_is_valid_consumer(u32 cookie, struct qcom_l3cc_drv *drv)
{
	bool is_valid = false;

	if (!drv->consumer || !drv->consumer_cookie) {
		dev_err(drv->dev, "Invalid cookie used for registering\n");
		return false;
	}

	mutex_lock(&drv->update_lock);
	is_valid = (cookie == drv->consumer_cookie) ? true : false;
	mutex_unlock(&drv->update_lock);

	return is_valid;
}

static void set_cpu_sid_config_on_cpu(void *info)
{
	struct qcom_l3cc_cpu_sid_config *cdata = info;
	u64 regval = 0;

	regval = read_sysreg_s(CLUSTER_THREAD_SID_EL1);
	regval &= ~CLUSTER_THREAD_SID_EL1_MASK;
	regval |= FIELD_PREP(CLUSTER_THREAD_SID_EL1_MASK, cdata->config.sid);
	write_sysreg_s(regval, CLUSTER_THREAD_SID_EL1);

	/* to synchronize cache settings*/
	isb();
}

static void get_cpu_sid_config_on_cpu(void *info)
{
	struct qcom_l3cc_cpu_sid_config *cdata = info;

	cdata->config.sid = CLUSTER_THREAD_SID_EL1_MASK &
						read_sysreg_s(CLUSTER_THREAD_SID_EL1);
}

static int qcom_l3cc_update_cpu_sid_config(void __user *req, struct qcom_l3cc_drv *drv)
{
	struct qcom_l3cc_cpu_sid_config cdata;
	int ret = 0;
	int cpu;

	ret = copy_from_user(&cdata, req, sizeof(cdata));
	if (ret) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	if (cdata.cl >= (drv->nr_clusters)) {
		dev_err(drv->dev, "Invalid number of clusters:%d\n", cdata.cl);
		return -EINVAL;
	}

	if (!qcom_l3cc_is_valid_consumer(cdata.cookie, drv)) {
		dev_err(drv->dev, "Invalid service consumer\n");
		return -EINVAL;
	}

	if (cdata.config.cpu >= drv->max_cpu) {
		dev_err(drv->dev, "Invalid cpu:%d\n", cdata.config.cpu);
		return -EINVAL;
	}

	cpu = cdata.config.cpu;

	mutex_lock(&drv->update_lock);
	if (cdata.op) {
		if (cdata.config.sid >= L3_CACHE_SCHEME_ID_MAX) {
			dev_err(drv->dev, "Invalid sid:%llu\n", cdata.config.sid);
			mutex_unlock(&drv->update_lock);
			return -EINVAL;
		}
		smp_call_function_single(cpu, set_cpu_sid_config_on_cpu,
									&cdata, true);
	} else {
		smp_call_function_single(cpu, get_cpu_sid_config_on_cpu,
									&cdata, true);
	}
	mutex_unlock(&drv->update_lock);

	if (copy_to_user(req, &cdata, sizeof(cdata))) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	return 0;
}

static void set_cluster_l3_wg_for_sid(void *info)
{
	struct qcom_l3cc_wg_config *cdata = info;
	u64 regval = 0;

	regval = read_sysreg_s(CLUSTER_PCR_EL1);
	regval &= ~L3_WAYS_SID_MASK(cdata->sid);
	regval |= (cdata->cache_ways << (cdata->sid*(BITS_PER_SID)));

	write_sysreg_s(regval, CLUSTER_PCR_EL1);

	/* to synchronize cache settings*/
	isb();
}

static int qcom_l3cc_set_cluster_wg(void __user *req, struct qcom_l3cc_drv *drv)
{
	struct qcom_l3cc_wg_config cdata;
	int ret = 0;
	int cpu;

	ret = copy_from_user(&cdata, req, sizeof(cdata));
	if (ret) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	if ((cdata.cl >= (drv->nr_clusters)) ||
			(cdata.sid >= L3_CACHE_SCHEME_ID_MAX) ||
		(cdata.cache_ways > MAX_CACHE_WAYS_CONFIG)) {
		dev_err(drv->dev, "Invalid params\n");
		return -EINVAL;
	}

	if (!qcom_l3cc_is_valid_consumer(cdata.cookie, drv)) {
		dev_err(drv->dev, "Invalid service consumer\n");
		return -EINVAL;
	}

	cpu = (cdata.cl * drv->nr_cpus);

	mutex_lock(&drv->update_lock);
	smp_call_function_single(cpu, set_cluster_l3_wg_for_sid,
							&cdata, true);
	mutex_unlock(&drv->update_lock);

	if (copy_to_user(req, &cdata, sizeof(cdata))) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	return 0;
}

static void set_cluster_config_on_cpu(void *info)
{
	struct qcom_l3cc_cl_config *cdata = info;
	u64 regval = 0;

	regval = read_sysreg_s(CLUSTER_ACP_SID_EL1);
	regval &= ~CLUSTER_ACP_SID_EL1_MASK;
	regval |= FIELD_PREP(CLUSTER_ACP_SID_EL1_MASK, cdata->config.acp_sid);
	write_sysreg_s(regval, CLUSTER_ACP_SID_EL1);

	regval = read_sysreg_s(CLUSTER_STASH_SID_EL1);
	regval &= ~CLUSTER_STASH_SID_EL1_MASK;
	regval |= FIELD_PREP(CLUSTER_STASH_SID_EL1_MASK, cdata->config.stash_sid);
	write_sysreg_s(regval, CLUSTER_STASH_SID_EL1);

	write_sysreg_s(cdata->config.cpcr, CLUSTER_PCR_EL1);

	/* to synchronize cache settings*/
	isb();
}

static void get_cluster_config_on_cpu(void *info)
{
	struct qcom_l3cc_cl_config *cdata = info;

	cdata->config.acp_sid = read_sysreg_s(CLUSTER_ACP_SID_EL1) &
								CLUSTER_ACP_SID_EL1_MASK;
	cdata->config.stash_sid = read_sysreg_s(CLUSTER_STASH_SID_EL1) &
								CLUSTER_STASH_SID_EL1_MASK;
	cdata->config.cpcr = read_sysreg_s(CLUSTER_PCR_EL1);
}

static int qcom_l3cc_update_cluster_config(void __user *req, struct qcom_l3cc_drv *drv)
{
	struct qcom_l3cc_cl_config cdata;
	int ret = 0;
	int cpu;

	ret = copy_from_user(&cdata, req, sizeof(cdata));
	if (ret) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	if (cdata.cl >= (drv->nr_clusters)) {
		dev_err(drv->dev, "Invalid cluster: %d\n", cdata.cl);
		return -EINVAL;
	}

	if (!qcom_l3cc_is_valid_consumer(cdata.cookie, drv)) {
		dev_err(drv->dev, "Invalid service cookie\n");
		return -EINVAL;
	}

	cpu = (cdata.cl * drv->nr_cpus);

	mutex_lock(&drv->update_lock);
	if (cdata.op) {
		if ((cdata.config.acp_sid >= L3_CACHE_SCHEME_ID_MAX) ||
		(cdata.config.stash_sid >= L3_CACHE_SCHEME_ID_MAX)) {
			dev_err(drv->dev, "Invalid params\n");
			mutex_unlock(&drv->update_lock);
			return -EINVAL;
		}
		smp_call_function_single(cpu, set_cluster_config_on_cpu, &cdata, true);
	} else {
		smp_call_function_single(cpu, get_cluster_config_on_cpu, &cdata, true);
	}
	mutex_unlock(&drv->update_lock);

	if (copy_to_user(req, &cdata, sizeof(cdata))) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	return 0;
}

static void set_cluster_sid_qos(void *info)
{
	struct qcom_l3cc_cl_qos_config *cdata = info;
	u64 regval = 0;

	regval = read_sysreg_s(CLUSTER_BUS_QOS_EL1);
	regval &= ~cdata->mask;
	regval |= cdata->rvalue;
	write_sysreg_s(regval, CLUSTER_BUS_QOS_EL1);

	/* to synchronize cache settings*/
	isb();

}

static void get_cluster_sid_qos(void *info)
{
	struct qcom_l3cc_cl_qos_config *cdata = info;

	cdata->rvalue = read_sysreg_s(CLUSTER_BUS_QOS_EL1);
}

static int qcom_l3cc_update_cluster_sid_qos(void __user *req, struct qcom_l3cc_drv *drv)
{
	struct qcom_l3cc_cl_qos_config cdata;
	int ret = 0;
	int cpu;

	ret = copy_from_user(&cdata, req, sizeof(cdata));
	if (ret) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	if (cdata.cl >= (drv->nr_clusters)) {
		dev_err(drv->dev, "Invalid cluster: %d\n", cdata.cl);
		return -EINVAL;
	}

	if (!qcom_l3cc_is_valid_consumer(cdata.cookie, drv)) {
		dev_err(drv->dev, "Invalid cookie\n");
		return -EINVAL;
	}

	cpu = (cdata.cl * drv->nr_cpus);

	mutex_lock(&drv->update_lock);
	if (cdata.op)
		smp_call_function_single(cpu, set_cluster_sid_qos, &cdata, true);
	else
		smp_call_function_single(cpu, get_cluster_sid_qos, &cdata, true);
	mutex_unlock(&drv->update_lock);

	if (copy_to_user(req, &cdata, sizeof(cdata))) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	return 0;
}

static int qcom_l3cc_register(void __user *req, struct qcom_l3cc_drv *drv)
{
	struct qcom_l3cc_regdata rdata;
	int ret = 0;
	u32 cookie;
	char consumer[256];

	ret = copy_from_user(&rdata, req, sizeof(rdata));
	if (ret) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	mutex_lock(&drv->update_lock);
	if (!rdata.name || drv->consumer || drv->consumer_cookie) {
		dev_err(drv->dev, "Invalid service request\n");
		mutex_unlock(&drv->update_lock);
		return -EINVAL;
	}

	if (strncpy_from_user(consumer, rdata.name, sizeof(consumer) - 1) < 0) {
		mutex_unlock(&drv->update_lock);
		return -EFAULT;
	}
	consumer[sizeof(consumer) - 1] = '\0';

	drv->consumer = kstrdup(consumer, GFP_KERNEL);
	if (!drv->consumer) {
		mutex_unlock(&drv->update_lock);
		return -ENOMEM;
	}

	get_random_bytes(&cookie, sizeof(cookie));
	drv->consumer_cookie = cookie;
	mutex_unlock(&drv->update_lock);
	rdata.cookie = cookie;

	if (copy_to_user(req, &rdata, sizeof(rdata))) {
		dev_err(drv->dev, "failed to copy data\n");
		mutex_lock(&drv->update_lock);
		kfree(drv->consumer);
		drv->consumer = NULL;
		drv->consumer_cookie = 0;
		mutex_unlock(&drv->update_lock);
		return -EFAULT;
	}

	return 0;
}

static int qcom_l3cc_unregister(void __user *req, struct qcom_l3cc_drv *drv)
{
	struct qcom_l3cc_regdata rdata;
	int ret = 0;

	ret = copy_from_user(&rdata, req, sizeof(rdata));
	if (ret) {
		dev_err(drv->dev, "failed to copy data\n");
		return -EFAULT;
	}

	if (!qcom_l3cc_is_valid_consumer(rdata.cookie, drv)) {
		dev_err(drv->dev, "invalid service consumer\n");
		return -EINVAL;
	}

	mutex_lock(&drv->update_lock);
	if (drv->consumer)
		kfree(drv->consumer);
	drv->consumer = NULL;
	drv->consumer_cookie = 0;
	mutex_unlock(&drv->update_lock);

	return 0;
}

static long qcom_l3cc_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	char __user *req = (char __user *)arg;
	struct qcom_l3cc_drv *drv = mdev_to_drv(file->private_data);
	int ret = 0;

	switch (cmd) {
	case QCOM_L3CC_IOCTL_REGISTER:
		ret = qcom_l3cc_register(req, drv);
		break;

	case QCOM_L3CC_IOCTL_UNREGISTER:
		ret = qcom_l3cc_unregister(req, drv);
		break;

	case QCOM_L3CC_IOCTL_UPDATE_CL_CONFIG:
		ret = qcom_l3cc_update_cluster_config(req, drv);
		break;

	case QCOM_L3CC_IOCTL_UPDATE_CPU_SID:
		ret = qcom_l3cc_update_cpu_sid_config(req, drv);
		break;

	case QCOM_L3CC_IOCTL_SET_CL_WG:
		ret = qcom_l3cc_set_cluster_wg(req, drv);
		break;

	case QCOM_L3CC_IOCTL_UPDATE_CL_SID_QOS:
		ret = qcom_l3cc_update_cluster_sid_qos(req, drv);
		break;


	default:
		ret = -ENOENT;
		break;
	}

	return ret;
}

static int qcom_l3cc_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int qcom_l3cc_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations qcom_l3cc_fops = {
	.owner = THIS_MODULE,
	.open = qcom_l3cc_open,
	.release = qcom_l3cc_release,
	.unlocked_ioctl = qcom_l3cc_ioctl,
};

static int qcom_l3cc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct qcom_l3cc_drv *drv;
	int ret = 0;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->dev = dev;
	if (of_property_read_u32(np, "nr_cpus", &drv->nr_cpus)) {
		ret = -EINVAL;
		return dev_err_probe(&pdev->dev, ret,
				"failed to find nr_cpus field\n");
	}

	if (of_property_read_u32(np, "nr_clusters", &drv->nr_clusters)) {
		ret = -EINVAL;
		return dev_err_probe(&pdev->dev, ret,
					"failed to find nr_clusters field\n");

	}

	dev_set_drvdata(&pdev->dev, drv);
	mutex_init(&drv->update_lock);

	drv->max_cpu = drv->nr_cpus * drv->nr_clusters;
	drv->name = "qcom_l3_cache_config_device";
	drv->mdev.minor = MISC_DYNAMIC_MINOR;
	drv->mdev.name = drv->name;
	drv->mdev.fops = &qcom_l3cc_fops;

	ret = misc_register(&drv->mdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
							"registration failed\n");

	return 0;
}

static int qcom_l3cc_remove(struct platform_device *pdev)
{
	struct qcom_l3cc_drv *drv = dev_get_drvdata(&pdev->dev);

	misc_deregister(&drv->mdev);

	return 0;
}

static const struct of_device_id qcom_l3cc_table[] = {
	{.compatible = "qcom,l3-cache-config"},
	{}
};

MODULE_DEVICE_TABLE(of, qcom_l3cc_table);

static struct platform_driver qcom_l3cc_driver = {
	.driver = {
		.name = "qcom-l3-cache-config",
		.of_match_table = qcom_l3cc_table,
	},
	.probe = qcom_l3cc_probe,
	.remove = qcom_l3cc_remove,
};

module_platform_driver(qcom_l3cc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm L3 cache config driver");

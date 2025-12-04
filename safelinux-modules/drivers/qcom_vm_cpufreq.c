// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/scmi_protocol.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <uapi/misc/qcom_vm_cpufreq.h>

#define CREATE_TRACE_POINTS
#include "qcom_vm_cpufreq_trace.h"

struct vm_cpufreq_cluster;
struct vm_cpufreq_client_ctx;

/* Default transition latency if SCMI doesn't provide one (in microseconds) */
#define DEFAULT_TRANSITION_LATENCY_US 10000

/*
 * rate_limit - enable transition-latency rate limiting (default: off).
 *
 * When true, vm_cpufreq_set_level() rejects SCMI level_set calls that arrive
 * sooner than transition_latency_us after the previous successful call,
 * returning -EAGAIN to the caller.
 *
 * This parameter is read-only after module load; set it at load time:
 *
 *   modprobe qcom_vm_cpufreq rate_limit=1
 */
static bool rate_limit;
module_param(rate_limit, bool, 0444);
MODULE_PARM_DESC(rate_limit,
		 "Reject level_set calls within transition_latency_us of the previous call (default: 0)");

/* Client data structure - simplified to single performance level request */
struct vm_cpufreq_client {
	u32 level;
};

/* Cluster device structure */
struct vm_cpufreq_cluster {
	struct kref ref;
	struct miscdevice miscdev;
	int domain;
	/* ph: Protected by srcu during access, set to NULL during removal */
	struct scmi_protocol_handle *ph;
	const struct scmi_perf_proto_ops *perf_ops;
	struct cpumask cpus;
	struct device *dev;		/* Parent device */
	struct list_head list;		/* For global driver list */
	struct mutex lock;

	struct list_head client_list;
	u32 agg_applied_level;

	/* Rate limiting for level changes */
	ktime_t last_freq_change;
	u32 transition_latency_us;

	unsigned long *freq_table;
	u32 num_levels;
	u32 thermal_cap_khz;
	/* Policy kobject for cpufreq hierarchy */
	struct kobject *policy_kobj;
	struct srcu_struct hw_srcu;
};

/* Driver Context to safely track clusters for cleanup */
struct vm_cpufreq_context {
	struct list_head clusters;
};

/* wrapper tying file to cluster + client */
struct vm_cpufreq_client_ctx {
	struct vm_cpufreq_cluster *cluster;
	struct vm_cpufreq_client client;
	struct list_head node;
};

/* vm_policy_node - wrapper embedding a kobject for the policy directory. */
struct vm_policy_node {
	struct kobject kobj;
	struct vm_cpufreq_cluster *cluster;
};

/* Global cpufreq kobject for policy hierarchy - No lock needed */
static struct kobject *cpufreq_global_kobj;

/* Cleanup helpers */
static inline void vm_cpufreq_client_ctx_cleanup(struct vm_cpufreq_client_ctx *ctx_ptr)
{
	kfree(ctx_ptr);
}

DEFINE_FREE(vm_cpufreq_client_ctx, struct vm_cpufreq_client_ctx *,
	    vm_cpufreq_client_ctx_cleanup(_T))

static void vm_cpufreq_cluster_release_mem(struct kref *kref)
{
	struct vm_cpufreq_cluster *cluster =
		container_of(kref, struct vm_cpufreq_cluster, ref);

	cleanup_srcu_struct(&cluster->hw_srcu);
	kfree(cluster->freq_table);
	mutex_destroy(&cluster->lock);
	kfree(cluster->miscdev.name);
	kfree(cluster);
}

/*
 * Defines automatic cleanup for vm_cpufreq_cluster pointers.
 * This enables the use of __free(vm_cpufreq_cluster) to automatically
 * drop the kref when the variable goes out of scope.
 */
DEFINE_FREE(vm_cpufreq_cluster, struct vm_cpufreq_cluster *,
	if (_T)
		kref_put(&_T->ref, vm_cpufreq_cluster_release_mem))


/* Get domain ID for a CPU (from DT) */
static int vm_cpufreq_parse_cpu_domain(int cpu)
{
	struct device *cpu_dev;
	struct of_phandle_args domain_id;
	struct device_node *np __free(device_node) = NULL;
	int ret;

	if (!cpu_possible(cpu))
		return -EINVAL;

	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev)
		return -ENODEV;

	np = of_node_get(cpu_dev->of_node);
	if (!np) {
		dev_dbg(cpu_dev, "No device node for CPU %d\n", cpu);
		return -ENODEV;
	}

	ret = of_parse_phandle_with_args(np, "clocks", "#clock-cells", 0,
					 &domain_id);
	if (ret) {
		ret = of_parse_phandle_with_args(np, "power-domains",
						 "#power-domain-cells", 0,
						 &domain_id);
		if (ret)
			return -EINVAL;
	}

	of_node_put(domain_id.np);
	return domain_id.args[0];
}

/* Helper: Convert index to frequency using Array - O(1) */
static int vm_cpufreq_index_to_freq(struct vm_cpufreq_cluster *cluster,
				    u32 index, unsigned long *freq_khz)
{
	if (!cluster || !freq_khz || index >= cluster->num_levels)
		return -EINVAL;

	*freq_khz = cluster->freq_table[index];
	return 0;
}

/* Helper: Convert frequency to index - Linear scan */
static int vm_cpufreq_freq_to_index(struct vm_cpufreq_cluster *cluster,
				    unsigned long freq_khz, u32 *index)
{
	u32 i;

	if (!cluster || !index)
		return -EINVAL;

	for (i = 0; i < cluster->num_levels; i++) {
		if (cluster->freq_table[i] == freq_khz) {
			*index = i;
			return 0;
		}
	}

	return -EINVAL;
}

/* Get performance level via SCMI */
static int vm_cpufreq_get_level(struct vm_cpufreq_cluster *cluster, u32 *level)
{
	int ret;
	ktime_t start_time, end_time;
	s64 duration_us;
	int srcu_idx;

	if (!level)
		return -EINVAL;

	/*
	 * Acquire SRCU lock to protect against concurrent driver removal
	 * (which nulls cluster->ph).
	 */
	srcu_idx = srcu_read_lock(&cluster->hw_srcu);

	if (!READ_ONCE(cluster->ph)) {
		ret = -ENODEV;
		goto out;
	}

	if (!cluster->perf_ops->level_get) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	start_time = ktime_get();
	ret = cluster->perf_ops->level_get(cluster->ph, cluster->domain,
					   level, false);
	end_time = ktime_get();
	duration_us = ktime_us_delta(end_time, start_time);

	if (!ret)
		trace_vm_cpufreq_level_get(cluster->domain, *level, ret,
					   duration_us);
out:
	srcu_read_unlock(&cluster->hw_srcu, srcu_idx);
	return ret;
}

/*
 * Helper: Get current frequency in kHz.
 *
 * Design note: in this driver the SCMI performance "level" is a frequency
 * value in kHz, not an opaque index.  level_set() is always called with a
 * kHz value (see vm_cpufreq_set_level()), so level_get() returns a kHz
 * value directly.  No index-to-frequency conversion is required here.
 * handle_level_get() converts the returned kHz value to a user-visible
 * index via vm_cpufreq_freq_to_index() because the ioctl ABI exposes
 * indices, not raw frequencies.
 */
static int vm_cpufreq_get_freq(struct vm_cpufreq_cluster *cluster, u64 *freq_khz)
{
	u32 scmi_level;
	int ret;

	if (!freq_khz)
		return -EINVAL;

	ret = vm_cpufreq_get_level(cluster, &scmi_level);
	if (ret)
		return ret;

	*freq_khz = scmi_level;
	return 0;
}

/* Set performance level via SCMI - caller must hold cluster->lock */
static int vm_cpufreq_set_level(struct vm_cpufreq_cluster *cluster, u32 level)
{
	ktime_t start_time, end_time;
	s64 duration_us;
	int ret;
	int srcu_idx;

	lockdep_assert_held(&cluster->lock);

	/*
	 * Acquire SRCU lock to protect against concurrent driver removal.
	 * This is necessary because 'cluster_release' (file close) calls this
	 * without holding SRCU.
	 */
	srcu_idx = srcu_read_lock(&cluster->hw_srcu);

	if (!READ_ONCE(cluster->ph)) {
		ret = -ENODEV;
		goto out;
	}

	if (rate_limit && cluster->last_freq_change != 0) {
		ktime_t now = ktime_get();
		s64 elapsed_us = ktime_us_delta(now, cluster->last_freq_change);

		if (elapsed_us < cluster->transition_latency_us) {
			dev_dbg(cluster->dev,
				"Rate limit: rejecting level change (%lld us since last, need %u us)\n",
				elapsed_us, cluster->transition_latency_us);
			ret = -EAGAIN;
			goto out;
		}
	}

	start_time = ktime_get();
	ret = cluster->perf_ops->level_set(cluster->ph, cluster->domain,
					   level, false);
	end_time = ktime_get();
	duration_us = ktime_us_delta(end_time, start_time);

	trace_vm_cpufreq_level_set(cluster->domain, level, ret, duration_us);

	if (!ret) {
		cluster->agg_applied_level = level;
		cluster->last_freq_change = end_time;
	}

out:
	srcu_read_unlock(&cluster->hw_srcu, srcu_idx);
	return ret;
}

/* Recompute aggregated request and apply if needed */
static int vm_cpufreq_recompute_and_update(struct vm_cpufreq_cluster *cluster)
{
	struct vm_cpufreq_client_ctx *ctx;
	u32 new_target = 0;
	bool has_clients = false;

	lockdep_assert_held(&cluster->lock);

	list_for_each_entry(ctx, &cluster->client_list, node) {
		has_clients = true;
		if (ctx->client.level > new_target)
			new_target = ctx->client.level;
	}

	if (!has_clients) {
		/*
		 * No clients remain.  Reset the cached level to 0 so the next
		 * client that arrives will always issue a fresh SCMI call
		 * (new_target != agg_applied_level).  We intentionally do NOT
		 * push a level=0 to the hardware here; the hardware stays at
		 * the last programmed frequency until a new client requests a
		 * change.  If explicit hardware reset is required, add a
		 * vm_cpufreq_set_level(cluster, lowest_level) call here.
		 */
		cluster->agg_applied_level = 0;
		return 0;
	}

	new_target = min_t(u32, new_target, cluster->thermal_cap_khz);
	if (new_target == cluster->agg_applied_level)
		return 0;

	return vm_cpufreq_set_level(cluster, new_target);
}

/* Initialize frequency table from OPP table */
static int vm_cpufreq_init_freq_table(struct vm_cpufreq_cluster *cluster)
{
	struct device *dev = cluster->miscdev.this_device;
	struct dev_pm_opp *opp;
	unsigned long freq_hz = 0;
	int count, i;

	if (!dev)
		return -ENODEV;

	count = dev_pm_opp_get_opp_count(dev);
	if (count < 0) {
		dev_err(cluster->dev, "Failed to get OPP count: %d\n", count);
		return count;
	}
	if (count == 0) {
		dev_err(cluster->dev, "No frequencies found in OPP table\n");
		return -ENODEV;
	}

	cluster->freq_table = kcalloc(count, sizeof(*cluster->freq_table), GFP_KERNEL);
	if (!cluster->freq_table)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		opp = dev_pm_opp_find_freq_ceil(dev, &freq_hz);
		if (IS_ERR(opp)) {
			/* Free now; set NULL to prevent double-free in release_mem */
			kfree(cluster->freq_table);
			cluster->freq_table = NULL;
			return PTR_ERR(opp);
		}
		dev_pm_opp_put(opp);


		cluster->freq_table[i] = freq_hz / 1000;
		freq_hz++;
	}

	/* Loop runs exactly 'count' times; num_levels == count on success */
	cluster->num_levels = i;
	return 0;
}

/* File operations */
static int cluster_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *mdev = filp->private_data;
	struct vm_cpufreq_cluster *cluster =
		container_of(mdev, struct vm_cpufreq_cluster, miscdev);
	struct vm_cpufreq_client_ctx *ctx __free(vm_cpufreq_client_ctx) = NULL;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->cluster = cluster;
	ctx->client.level = 0;
	INIT_LIST_HEAD(&ctx->node);

	guard(mutex)(&cluster->lock);

	if (!READ_ONCE(cluster->ph))
		return -ENODEV;

	kref_get(&cluster->ref);
	list_add_tail(&ctx->node, &cluster->client_list);

	filp->private_data = no_free_ptr(ctx);
	return 0;
}

static int cluster_release(struct inode *inode, struct file *filp)
{
	struct vm_cpufreq_client_ctx *ctx __free(vm_cpufreq_client_ctx) =
		filp->private_data;
	struct vm_cpufreq_cluster *cluster;
	struct vm_cpufreq_cluster *cluster_ref __free(vm_cpufreq_cluster) = NULL;

	if (!ctx)
		return 0;

	cluster = ctx->cluster;
	cluster_ref = cluster; /* Hold ref to put it at end of function */

	scoped_guard(mutex, &cluster->lock) {
		list_del(&ctx->node);
		vm_cpufreq_recompute_and_update(cluster);
	}

	filp->private_data = NULL;
	return 0;
}

static int handle_level_set(struct vm_cpufreq_cluster *cluster,
			     struct vm_cpufreq_client_ctx *ctx,
			     char __user *argp)
{
	struct cpu_perf_level_req req;
	unsigned long freq_khz;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.level >= cluster->num_levels)
		return -EINVAL;

	ret = vm_cpufreq_index_to_freq(cluster, req.level, &freq_khz);
	if (ret)
		return ret;

	/*
	 * Reject zero and values that would truncate when stored in the u32
	 * client.level field.  On 64-bit kernels ULONG_MAX >> 1 is far above
	 * U32_MAX and would silently truncate any frequency above ~4.3 GHz.
	 */
	if (unlikely(freq_khz == 0 || freq_khz > U32_MAX))
		return -EINVAL;

	guard(mutex)(&cluster->lock);
	ctx->client.level = freq_khz;
	return vm_cpufreq_recompute_and_update(cluster);
}

static int handle_level_get(struct vm_cpufreq_cluster *cluster,
			     char __user *argp)
{
	struct cpu_perf_level_req req = {0};
	u32 freq_khz;
	int ret;

	ret = vm_cpufreq_get_level(cluster, &freq_khz);
	if (ret)
		return ret;

	ret = vm_cpufreq_freq_to_index(cluster, freq_khz, &req.level);
	if (ret)
		return ret;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static int handle_get_available_levels(struct vm_cpufreq_cluster *cluster,
				       char __user *argp)
{
	struct cpu_perf_levels_available avail = {0};
	u32 i;

	/*
	 * Cap num_levels to MAX_PERF_LEVELS so that userspace cannot read
	 * avail.num_levels and then index avail.levels[] beyond its bounds.
	 */
	avail.num_levels = min_t(u32, cluster->num_levels, (u32)MAX_PERF_LEVELS);
	for (i = 0; i < avail.num_levels; i++)
		avail.levels[i] = i;

	if (copy_to_user(argp, &avail, sizeof(avail)))
		return -EFAULT;

	return 0;
}

static long cluster_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct vm_cpufreq_client_ctx *ctx = file->private_data;
	struct vm_cpufreq_cluster *cluster;
	char __user *argp = (char __user *)arg;
	int ret = -EINVAL;
	int srcu_idx;

	/* Defensive: private_data is set in cluster_open; guard against races */
	if (!ctx)
		return -EINVAL;

	cluster = ctx->cluster;
	if (!cluster)
		return -EINVAL;

	if (_IOC_TYPE(cmd) != VM_CPUFREQ_IOC_MAGIC)
		return -ENOTTY;

	/*
	 * Outer SRCU lock protects 'ph' for the basic check
	 * and keeps the device "alive" logic consistent.
	 */
	srcu_idx = srcu_read_lock(&cluster->hw_srcu);

	if (!READ_ONCE(cluster->ph)) {
		srcu_read_unlock(&cluster->hw_srcu, srcu_idx);
		return -ENODEV;
	}

	switch (cmd) {
	case CPU_PERF_LEVEL_SET:
		if (_IOC_SIZE(cmd) != sizeof(struct cpu_perf_level_req)) {
			ret = -EINVAL;
			goto out;
		}
		ret = handle_level_set(cluster, ctx, argp);
		break;
	case CPU_PERF_LEVEL_GET:
		if (_IOC_SIZE(cmd) != sizeof(struct cpu_perf_level_req)) {
			ret = -EINVAL;
			goto out;
		}
		ret = handle_level_get(cluster, argp);
		break;
	case CPU_PERF_LEVELS_GET_AVAILABLE:
		if (_IOC_SIZE(cmd) != sizeof(struct cpu_perf_levels_available)) {
			ret = -EINVAL;
			goto out;
		}
		ret = handle_get_available_levels(cluster, argp);
		break;
	default:
		ret = -ENOTTY;
		break;
	}

out:
	srcu_read_unlock(&cluster->hw_srcu, srcu_idx);
	return ret;
}

static const struct file_operations cluster_fops = {
	.owner          = THIS_MODULE,
	.open           = cluster_open,
	.release        = cluster_release,
	.unlocked_ioctl = cluster_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* Sysfs Implementation */

struct vm_cpufreq_attr {
	struct attribute attr;
	ssize_t (*show)(struct vm_cpufreq_cluster *cluster, char *buf);
};

#define VM_CPUFREQ_ATTR_RO(_name) \
	static struct vm_cpufreq_attr vm_cpufreq_attr_##_name = { \
		.attr = { .name = #_name, .mode = 0444 }, \
		.show = vm_cpufreq_##_name##_show, \
	}

static ssize_t vm_cpufreq_scaling_cur_freq_show(
				struct vm_cpufreq_cluster *cluster, char *buf)
{
	u64 freq_khz;
	int ret;

	ret = vm_cpufreq_get_freq(cluster, &freq_khz);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%llu\n", freq_khz);
}
VM_CPUFREQ_ATTR_RO(scaling_cur_freq);

static ssize_t vm_cpufreq_scaling_available_frequencies_show(
				struct vm_cpufreq_cluster *cluster, char *buf)
{
	unsigned long freq_khz;
	int count = 0;	/* int matches sysfs_emit_at()'s 'int at' parameter */
	u32 index;
	int ret;

	if (cluster->num_levels == 0)
		return sysfs_emit(buf, "\n");

	for (index = 0; index < cluster->num_levels; index++) {
		ret = vm_cpufreq_index_to_freq(cluster, index, &freq_khz);
		if (ret)
			continue;

		ret = sysfs_emit_at(buf, count, "%lu ", freq_khz);
		if (ret < 0)
			break;
		count += ret;
	}

	if (count > 0 && buf[count - 1] == ' ')
		buf[count - 1] = '\n';
	else if (count == 0)
		return sysfs_emit(buf, "\n");

	return count;
}
VM_CPUFREQ_ATTR_RO(scaling_available_frequencies);

static ssize_t vm_cpufreq_affected_cpus_show(
				struct vm_cpufreq_cluster *cluster, char *buf)
{
	ssize_t i = 0;
	unsigned int cpu;

	for_each_cpu(cpu, &cluster->cpus) {
		i += sysfs_emit_at(buf, i, "%u ", cpu);
		if (i >= (PAGE_SIZE - 5))
			break;
	}

	/* Strip trailing space and terminate with newline */
	i--;
	i += sysfs_emit_at(buf, i, "\n");
	return i;
}
VM_CPUFREQ_ATTR_RO(affected_cpus);

static struct attribute *vm_cpufreq_attrs[] = {
	&vm_cpufreq_attr_scaling_cur_freq.attr,
	&vm_cpufreq_attr_scaling_available_frequencies.attr,
	&vm_cpufreq_attr_affected_cpus.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vm_cpufreq);

static ssize_t vm_cpufreq_attr_show(struct kobject *kobj,
				    struct attribute *attr, char *buf)
{
	struct vm_policy_node *node =
		container_of(kobj, struct vm_policy_node, kobj);
	struct vm_cpufreq_attr *vattr =
		container_of(attr, struct vm_cpufreq_attr, attr);

	if (!vattr->show)
		return -EIO;

	return vattr->show(node->cluster, buf);
}

static const struct sysfs_ops vm_cpufreq_sysfs_ops = {
	.show = vm_cpufreq_attr_show,
};

static void vm_policy_release(struct kobject *kobj)
{
	struct vm_policy_node *node =
		container_of(kobj, struct vm_policy_node, kobj);

	/*
	 * Release the reference to the cluster held by the policy node.
	 * This ensures the cluster memory is not freed while sysfs is active.
	 */
	kref_put(&node->cluster->ref, vm_cpufreq_cluster_release_mem);
	kfree(node);
}

static const struct kobj_type vm_policy_ktype = {
	.release        = vm_policy_release,
	.sysfs_ops      = &vm_cpufreq_sysfs_ops,
	.default_groups = vm_cpufreq_groups,
};

static void vm_cpufreq_remove_policy_structure(struct vm_cpufreq_cluster *cluster)
{
	int cpu;
	struct device *cpu_dev;

	if (!cluster->policy_kobj)
		return;

	for_each_cpu(cpu, &cluster->cpus) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;
		sysfs_remove_link(&cpu_dev->kobj, "cpufreq");
	}

	/* Decrement refcount of kobj; this eventually calls vm_policy_release */
	kobject_put(cluster->policy_kobj);
	cluster->policy_kobj = NULL;
}

static int vm_cpufreq_create_policy_structure(struct vm_cpufreq_cluster *cluster)
{
	struct vm_policy_node *node;
	char name[16];
	int cpu, ret;
	struct device *cpu_dev;

	if (!cpufreq_global_kobj)
		return -ENODEV;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	/*
	 * Take a reference to the cluster for the policy node.
	 * This prevents the cluster from being freed while sysfs is active.
	 */
	kref_get(&cluster->ref);
	node->cluster = cluster;

	cpu = cpumask_first(&cluster->cpus);
	snprintf(name, sizeof(name), "policy%d", cpu);

	ret = kobject_init_and_add(&node->kobj, &vm_policy_ktype,
				   cpufreq_global_kobj, "%s", name);
	if (ret) {
		/* kobject_put calls release, which drops the cluster ref */
		kobject_put(&node->kobj);
		return ret;
	}

	cluster->policy_kobj = &node->kobj;

	for_each_cpu(cpu, &cluster->cpus) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;

		ret = sysfs_create_link(&cpu_dev->kobj, cluster->policy_kobj,
					"cpufreq");
		if (ret && ret != -EEXIST) {
			/*
			 * vm_cpufreq_remove_policy_structure() iterates the
			 * full cpumask and calls sysfs_remove_link() for every
			 * CPU, including those whose symlinks were never created.
			 * sysfs_remove_link() is a no-op on non-existent entries,
			 * so all previously created symlinks are cleaned up and
			 * no resource leak occurs.
			 */
			vm_cpufreq_remove_policy_structure(cluster);
			return ret;
		}
	}

	return 0;
}

static void vm_cpufreq_build_cpumask(struct vm_cpufreq_cluster *cluster,
				     int domain_id)
{
	int cpu;

	cpumask_clear(&cluster->cpus);
	for_each_possible_cpu(cpu) {
		if (vm_cpufreq_parse_cpu_domain(cpu) == domain_id)
			cpumask_set_cpu(cpu, &cluster->cpus);
	}
}

/* This is called via devm action on driver remove/failure */
static void vm_cpufreq_release_all(void *data)
{
	struct vm_cpufreq_context *ctx = data;
	struct vm_cpufreq_cluster *cluster, *tmp;

	list_for_each_entry_safe(cluster, tmp, &ctx->clusters, list) {
		list_del(&cluster->list);

		WRITE_ONCE(cluster->ph, NULL);
		synchronize_srcu(&cluster->hw_srcu);

		vm_cpufreq_remove_policy_structure(cluster);

		if (cluster->miscdev.this_device)
			dev_pm_opp_remove_all_dynamic(cluster->miscdev.this_device);

		/*
		 * misc_deregister works even if registration failed (if zeroed),
		 * but our logic ensures we only list_add after success.
		 */
		misc_deregister(&cluster->miscdev);

		kref_put(&cluster->ref, vm_cpufreq_cluster_release_mem);
	}
}

static int vm_cpufreq_process_domain(struct scmi_device *sdev,
				     struct scmi_protocol_handle *ph,
				     const struct scmi_perf_proto_ops *perf_ops,
				     int domain_id,
				     struct vm_cpufreq_context *ctx)
{
	struct vm_cpufreq_cluster *cluster;
	int ret;

	cluster = kzalloc(sizeof(*cluster), GFP_KERNEL);
	if (!cluster)
		return -ENOMEM;

	kref_init(&cluster->ref);
	cluster->domain = domain_id;
	cluster->ph = ph;
	cluster->perf_ops = perf_ops;
	cluster->dev = &sdev->dev;
	cluster->thermal_cap_khz = U32_MAX;
	mutex_init(&cluster->lock);
	INIT_LIST_HEAD(&cluster->client_list);
	INIT_LIST_HEAD(&cluster->list);

	ret = init_srcu_struct(&cluster->hw_srcu);
	if (ret) {
		mutex_destroy(&cluster->lock);
		kfree(cluster);
		return ret;
	}

	ret = perf_ops->transition_latency_get(ph, domain_id);
	cluster->transition_latency_us = (ret > 0) ? ret : DEFAULT_TRANSITION_LATENCY_US;

	vm_cpufreq_build_cpumask(cluster, domain_id);

	/*
	 * Skip domains that have no associated CPUs.  cpumask_first() on an
	 * empty mask returns nr_cpu_ids, which would produce a nonsensical
	 * "policy<nr_cpu_ids>" sysfs directory.
	 */
	if (cpumask_empty(&cluster->cpus)) {
		dev_dbg(&sdev->dev, "Domain %d: no CPUs, skipping\n", domain_id);
		kref_put(&cluster->ref, vm_cpufreq_cluster_release_mem);
		return 0;
	}

	cluster->miscdev.minor = MISC_DYNAMIC_MINOR;
	cluster->miscdev.name = kasprintf(GFP_KERNEL, "cpu-freq-domain%d", domain_id);
	if (!cluster->miscdev.name) {
		kref_put(&cluster->ref, vm_cpufreq_cluster_release_mem);
		return -ENOMEM;
	}
	cluster->miscdev.fops = &cluster_fops;
	cluster->miscdev.parent = &sdev->dev;

	/*
	 * misc_register must happen before OPPs so that 'this_device' is
	 * populated.  Cleanup from here on uses goto err_dereg.
	 */
	ret = misc_register(&cluster->miscdev);
	if (ret) {
		kref_put(&cluster->ref, vm_cpufreq_cluster_release_mem);
		return ret;
	}

	ret = perf_ops->device_opps_add(ph, cluster->miscdev.this_device, domain_id);
	if (ret)
		goto err_dereg;

	ret = vm_cpufreq_init_freq_table(cluster);
	if (ret)
		goto err_remove_opps;

	/*
	 * The sysfs policy structure is a convenience interface only.
	 * A failure here (e.g. cpufreq_global_kobj not available) must not
	 * prevent the misc device ioctl path from functioning.
	 */
	ret = vm_cpufreq_create_policy_structure(cluster);
	if (ret)
		dev_warn(&sdev->dev,
			 "Domain %d: sysfs policy structure unavailable: %d\n",
			 domain_id, ret);

	/*
	 * Success: commit to global list.
	 * Only now will vm_cpufreq_release_all() see this cluster.
	 */
	list_add(&cluster->list, &ctx->clusters);

	dev_info(&sdev->dev, "Registered domain %d with %d CPUs\n",
		 domain_id, cpumask_weight(&cluster->cpus));

	return 0;

err_remove_opps:
	dev_pm_opp_remove_all_dynamic(cluster->miscdev.this_device);
err_dereg:
	WRITE_ONCE(cluster->ph, NULL);
	synchronize_srcu(&cluster->hw_srcu);
	misc_deregister(&cluster->miscdev);
	kref_put(&cluster->ref, vm_cpufreq_cluster_release_mem);
	return ret;
}

/* Check if SCMI device is used by CPUs via clock phandle */
static bool vm_cpufreq_scmi_dev_used_by_cpus(struct device *scmi_dev)
{
	struct device_node *scmi_np = dev_of_node(scmi_dev);
	struct device *cpu_dev;
	int cpu;

	if (!scmi_np)
		return false;

	for_each_possible_cpu(cpu) {
		struct device_node *np __free(device_node) = NULL;

		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;
		np = of_parse_phandle(dev_of_node(cpu_dev), "clocks", 0);
		if (np && np == scmi_np)
			return true;
	}

	return false;
}

static int vm_cpufreq_probe(struct scmi_device *sdev)
{
	int ret;
	struct device *dev = &sdev->dev;
	const struct scmi_handle *handle = sdev->handle;
	const struct scmi_perf_proto_ops *perf_ops;
	struct scmi_protocol_handle *ph;
	struct vm_cpufreq_context *ctx;
	int num_domains, i;

	if (!handle || !vm_cpufreq_scmi_dev_used_by_cpus(dev))
		return -ENODEV;

	perf_ops = handle->devm_protocol_get(sdev, SCMI_PROTOCOL_PERF, &ph);
	if (IS_ERR(perf_ops))
		return PTR_ERR(perf_ops);

	num_domains = perf_ops->num_domains_get(ph);
	if (num_domains < 0)
		return num_domains;
	if (num_domains == 0)
		return -ENODEV;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctx->clusters);

	/* Register cleanup for the list of clusters */
	ret = devm_add_action_or_reset(dev, vm_cpufreq_release_all, ctx);
	if (ret)
		return ret;

	for (i = 0; i < num_domains; i++) {
		ret = vm_cpufreq_process_domain(sdev, ph, perf_ops, i, ctx);
		/* Continue even if one domain fails, but warn */
		if (ret)
			dev_warn(dev, "Failed to process domain %d: %d\n", i, ret);
	}

	if (list_empty(&ctx->clusters))
		return -ENODEV;

	return 0;
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_PERF, "vm-cpufreq" },
	{ },
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver vm_cpufreq_drv = {
	.name     = "vm-cpufreq",
	.probe    = vm_cpufreq_probe,
	.id_table = scmi_id_table,
};

static int __init vm_cpufreq_init(void)
{
	struct device *cpu_dev;
	int ret;

	/* Create global kobject for userspace compatibility */
	cpu_dev = get_cpu_device(0);
	if (cpu_dev && cpu_dev->kobj.parent) {
		cpufreq_global_kobj = kobject_create_and_add("cpufreq",
							     cpu_dev->kobj.parent);
		if (!cpufreq_global_kobj)
			pr_warn("vm_cpufreq: Failed to create global cpufreq kobject; sysfs policy dirs unavailable\n");
	}

	ret = scmi_register(&vm_cpufreq_drv);
	if (ret && cpufreq_global_kobj) {
		kobject_put(cpufreq_global_kobj);
		cpufreq_global_kobj = NULL;
	}

	return ret;
}
module_init(vm_cpufreq_init);

static void __exit vm_cpufreq_exit(void)
{
	scmi_unregister(&vm_cpufreq_drv);

	if (cpufreq_global_kobj) {
		kobject_put(cpufreq_global_kobj);
		cpufreq_global_kobj = NULL;
	}
}
module_exit(vm_cpufreq_exit);

MODULE_DESCRIPTION("QCOM VM CPU Frequency Driver");
MODULE_LICENSE("GPL");

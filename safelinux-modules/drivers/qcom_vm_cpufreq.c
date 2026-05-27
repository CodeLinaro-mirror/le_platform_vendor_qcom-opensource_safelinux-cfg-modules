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
#include <linux/overflow.h>
#include <linux/pm_opp.h>
#include <linux/scmi_protocol.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/thermal.h>
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
 * Per-cluster cap on concurrent open() handles. Each open allocates a
 * vm_cpufreq_client_ctx and adds an entry to client_list; aggregation is O(N)
 * over that list. Without a cap, a misbehaving guest can drive arbitrary
 * memory growth and CPU work per ioctl. 256 is well above any plausible
 * legitimate use (one or a handful of governors per VM).
 */
#define VM_CPUFREQ_MAX_CLIENTS 256

/*
 * rate_limit - enable transition-latency rate limiting (default: off).
 *
 * When true, vm_cpufreq_apply_freq_khz() rejects SCMI freq_set calls that arrive
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
		 "Reject freq_set calls within transition_latency_us of the previous call (default: 0)");

/*
 * Per-client request, in kHz. Named freq_khz (not "level") to avoid
 * confusion with the SCMI perf-protocol "level" concept and with the
 * UAPI's level-index field; this driver always operates in the kHz
 * domain via perf_ops->freq_set / freq_get.
 */
struct vm_cpufreq_client {
	u32 freq_khz;
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
	/* Number of entries in client_list (capped at VM_CPUFREQ_MAX_CLIENTS) */
	u32 client_count;
	/*
	 * Last freq we asked SCMI to apply, in kHz. Initialized to U32_MAX
	 * as a "nothing applied yet" sentinel: with that value, the first
	 * recompute_and_update with no active client requests falls back to
	 * U32_MAX, then min_t clamps to thermal_cap_khz, so a thermal cap
	 * fired before any client SETs is enforced by pushing the cap
	 * itself. Real kHz values replace the sentinel after the first apply.
	 */
	u32 agg_applied_freq_khz;

	/* Rate limiting for frequency changes */
	ktime_t last_freq_change;
	u32 transition_latency_us;

	u32 *freq_table;
	u32 num_levels;
	u32 thermal_cap_khz;
	struct device *opp_dev;
	/* Policy kobject for cpufreq hierarchy */
	struct kobject *policy_kobj;
	/*
	 * CPUs whose /sys/devices/system/cpu/cpuN/cpufreq symlink we
	 * created. Only these are torn down on remove, so we never delete a
	 * symlink owned by another cpufreq driver.
	 */
	struct cpumask policy_link_cpus;
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

DEFINE_FREE(vm_cpufreq_client_ctx, struct vm_cpufreq_client_ctx *, kfree(_T))

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
	struct device_node *np __free(device_node) = NULL;
	struct of_phandle_args domain_id;
	struct device *cpu_dev;
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
		int perf_idx;

		/*
		 * A CPU node may have multiple power domains (performance,
		 * memory, power, etc.).  Find the one named "perf" rather than
		 * blindly using index 0.  Matches scmi-cpufreq.c behaviour.
		 */
		perf_idx = of_property_match_string(np, "power-domain-names",
						    "perf");
		if (perf_idx < 0)
			return -EINVAL;

		ret = of_parse_phandle_with_args(np, "power-domains",
						 "#power-domain-cells", perf_idx,
						 &domain_id);
		if (ret)
			return -EINVAL;
	}

	of_node_put(domain_id.np);
	return domain_id.args[0];
}

/* Helper: Convert index to frequency using Array - O(1) */
static int vm_cpufreq_index_to_freq(struct vm_cpufreq_cluster *cluster,
				    u32 index, u32 *freq_khz)
{
	if (index >= cluster->num_levels)
		return -EINVAL;

	*freq_khz = cluster->freq_table[index];
	return 0;
}

/* Helper: Convert frequency to index - Linear scan */
static int vm_cpufreq_freq_to_index(struct vm_cpufreq_cluster *cluster,
				    u32 freq_khz, u32 *index)
{
	u32 i;

	for (i = 0; i < cluster->num_levels; i++) {
		if (cluster->freq_table[i] == freq_khz) {
			*index = i;
			return 0;
		}
	}

	return -EINVAL;
}

/* Query current frequency from SCMI, in kHz. */
static int vm_cpufreq_query_freq_khz(struct vm_cpufreq_cluster *cluster, u32 *freq_khz)
{
	ktime_t start_time, end_time;
	unsigned long freq_hz;
	int ret, srcu_idx;
	s64 duration_us;

	/*
	 * Acquire SRCU lock to protect against concurrent driver removal
	 * (which nulls cluster->ph).
	 */
	srcu_idx = srcu_read_lock(&cluster->hw_srcu);

	if (!READ_ONCE(cluster->ph)) {
		ret = -ENODEV;
		goto out;
	}

	if (!cluster->perf_ops->freq_get) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	start_time = ktime_get();
	ret = cluster->perf_ops->freq_get(cluster->ph, cluster->domain,
					  &freq_hz, false);
	end_time = ktime_get();
	duration_us = ktime_us_delta(end_time, start_time);

	if (!ret)
		*freq_khz = (u32)(freq_hz / 1000);

	/* trace field "level" carries kHz; tracepoint name kept for ABI stability */
	trace_vm_cpufreq_level_get(cluster->domain, ret ? 0 : *freq_khz, ret,
				   duration_us);
out:
	srcu_read_unlock(&cluster->hw_srcu, srcu_idx);
	return ret;
}

/* Apply requested frequency (kHz) via SCMI - caller must hold cluster->lock. */
static int vm_cpufreq_apply_freq_khz(struct vm_cpufreq_cluster *cluster, u32 freq_khz,
				     bool urgent)
{
	ktime_t start_time, end_time;
	unsigned long rate_hz;
	int ret, srcu_idx;
	s64 duration_us;

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

	if (!cluster->perf_ops->freq_set) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (!urgent && rate_limit && cluster->last_freq_change != 0) {
		s64 elapsed_us = ktime_us_delta(ktime_get(), cluster->last_freq_change);

		if (elapsed_us < cluster->transition_latency_us) {
			dev_dbg(cluster->dev,
				"Rate limit: rejecting freq change (%lld us since last, need %u us)\n",
				elapsed_us, cluster->transition_latency_us);
			ret = -EAGAIN;
			trace_vm_cpufreq_level_set(cluster->domain, freq_khz, ret, 0);
			goto out;
		}
	}

	/*
	 * Convert kHz to Hz before handing to SCMI. The multiplication is
	 * done in unsigned long width to match freq_set's parameter type.
	 * On 32-bit kernels this can overflow for freq_khz > ULONG_MAX/1000
	 * (~4.29 GHz); detect that explicitly and surface -ERANGE rather
	 * than passing a wrapped rate to firmware.
	 */
	if (check_mul_overflow((unsigned long)freq_khz, 1000UL, &rate_hz)) {
		dev_err(cluster->dev,
			"freq %u kHz would overflow unsigned long Hz\n", freq_khz);
		ret = -ERANGE;
		trace_vm_cpufreq_level_set(cluster->domain, freq_khz, ret, 0);
		goto out;
	}

	start_time = ktime_get();
	ret = cluster->perf_ops->freq_set(cluster->ph, cluster->domain,
					  rate_hz, false);
	end_time = ktime_get();
	duration_us = ktime_us_delta(end_time, start_time);

	trace_vm_cpufreq_level_set(cluster->domain, freq_khz, ret, duration_us);

	if (!ret) {
		cluster->agg_applied_freq_khz = freq_khz;
		cluster->last_freq_change = end_time;
	}

out:
	srcu_read_unlock(&cluster->hw_srcu, srcu_idx);
	return ret;
}

/* Recompute aggregated request and apply if needed */
static int vm_cpufreq_recompute_and_update(struct vm_cpufreq_cluster *cluster,
					   bool urgent)
{
	struct vm_cpufreq_client_ctx *ctx;
	u32 new_target;

	lockdep_assert_held(&cluster->lock);

	/*
	 * Defensive bail-out. recompute_and_update is called from:
	 *   - handle_level_set (via cluster_ioctl, which gates on
	 *     freq_table+num_levels — so init has completed by the time we
	 *     get here),
	 *   - vm_cpufreq_cdev_set_state (cdev is created only after
	 *     init_freq_table succeeds, so init has completed).
	 * Both paths imply freq_table and num_levels are populated. Keep
	 * the check as belt-and-suspenders against a future refactor that
	 * adds another caller path.
	 */
	if (!cluster->freq_table || !cluster->num_levels)
		return 0;

	new_target = 0;
	list_for_each_entry(ctx, &cluster->client_list, node)
		if (ctx->client.freq_khz > new_target)
			new_target = ctx->client.freq_khz;

	/*
	 * No active client request: fall back to agg_applied_freq_khz, which
	 * is U32_MAX before any apply has happened and a real kHz value
	 * thereafter. The U32_MAX sentinel ensures the first thermal cdev
	 * fire with no clients pushes the cap itself: min(U32_MAX, cap) =
	 * cap. After any apply, the fallback is whatever was last applied,
	 * so subsequent cdev events without clients re-converge against the
	 * same value (equality check below short-circuits when the cap
	 * doesn't actually change the target).
	 *
	 * Userspace-governor semantics: cluster_release does NOT call
	 * recompute_and_update, so a closing client cannot trigger a drop
	 * here. Firmware stays at the last applied freq until the next
	 * SET or cdev event re-aggregates.
	 */
	new_target = new_target ?: cluster->agg_applied_freq_khz;

	new_target = min_t(u32, new_target, cluster->thermal_cap_khz);

	if (new_target == cluster->agg_applied_freq_khz)
		return 0;

	return vm_cpufreq_apply_freq_khz(cluster, new_target, urgent);
}

/* Initialize frequency table from OPP table */
static int vm_cpufreq_init_freq_table(struct vm_cpufreq_cluster *cluster)
{
	struct device *dev = cluster->opp_dev;
	unsigned long freq_hz = 0;
	struct dev_pm_opp *opp;
	int count, i = 0;

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

	cluster->freq_table = kcalloc(count, sizeof(*cluster->freq_table),
				      GFP_KERNEL);
	if (!cluster->freq_table)
		return -ENOMEM;

	opp = dev_pm_opp_find_freq_ceil(dev, &freq_hz);
	while (!IS_ERR(opp) && i < count) {
		freq_hz = dev_pm_opp_get_freq(opp);
		cluster->freq_table[i++] = (u32)(freq_hz / 1000);
		freq_hz++;
		dev_pm_opp_put(opp);
		opp = dev_pm_opp_find_freq_ceil(dev, &freq_hz);
	}

	if (!IS_ERR(opp)) {
		dev_pm_opp_put(opp);
	} else if (PTR_ERR(opp) != -ERANGE) {
		int err = PTR_ERR(opp);

		dev_err(cluster->dev,
			"init_freq_table: dev_pm_opp_find_freq_ceil failed: %d (after %d entries)\n",
			err, i);
		kfree(cluster->freq_table);
		cluster->freq_table = NULL;
		return err;
	}

	cluster->num_levels = i;
	if (cluster->num_levels == 0) {
		dev_err(cluster->dev, "OPP table emptied during init (TOCTOU)\n");
		kfree(cluster->freq_table);
		cluster->freq_table = NULL;
		return -ENODEV;
	}

	/*
	 * Detect Hz->kHz truncation collisions. SCMI-supplied OPPs are
	 * MHz-grain in practice, but if firmware ever advertises two OPPs
	 * whose Hz values fall in the same kHz bucket, freq_table ends up
	 * with duplicates and freq_to_index() becomes ambiguous. Warn so
	 * the underlying firmware bug is visible rather than silently
	 * latent. OPPs are appended in ascending order, so adjacency check
	 * suffices.
	 */
	for (i = 1; i < cluster->num_levels; i++) {
		if (cluster->freq_table[i] == cluster->freq_table[i - 1]) {
			dev_warn_once(cluster->dev,
				"duplicate kHz bucket at index %d (%u kHz); Hz->kHz collision in OPP table\n",
				i, cluster->freq_table[i]);
			break;
		}
	}

	return 0;
}

/* =========================================================================
 * CPU Cooling Device Implementation
 * =========================================================================
 */

struct vm_cpufreq_cdev_device {
	struct list_head node;
	struct thermal_cooling_device *cdev;
	struct vm_cpufreq_cluster *cluster;
	int cpu;
	unsigned long cur_state;
	unsigned long max_state;
	char cdev_name[THERMAL_NAME_LENGTH];
};

/* Global list of cooling devices */
static LIST_HEAD(vm_cdev_list);
static DEFINE_MUTEX(vm_cdev_list_lock);

static int vm_cpufreq_cdev_set_state(struct thermal_cooling_device *cdev,
					unsigned long state)
{
	struct vm_cpufreq_cdev_device *cdev_data = cdev->devdata;
	struct vm_cpufreq_cluster *cluster = cdev_data->cluster;

	if (state > cdev_data->max_state)
		return -EINVAL;
	if (state == cdev_data->cur_state)
		return 0;

	cdev_data->cur_state = state;

	dev_dbg(cluster->dev, "Thermal cdev:%s state:%lu (max_state:%lu)\n",
		cdev->type, state, cdev_data->max_state);
	guard(mutex)(&cluster->lock);
	cluster->thermal_cap_khz = cluster->freq_table[cdev_data->max_state - state];

	/*
	 * Immediately re-apply the aggregation so that any active clients
	 * are throttled without waiting for their next ioctl.
	 */
	return vm_cpufreq_recompute_and_update(cluster, true);
}

static int vm_cpufreq_cdev_get_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct vm_cpufreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->cur_state;
	return 0;
}

static int vm_cpufreq_cdev_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct vm_cpufreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->max_state;
	return 0;
}

static struct thermal_cooling_device_ops vm_cpufreq_cdev_ops = {
	.set_cur_state = vm_cpufreq_cdev_set_state,
	.get_cur_state = vm_cpufreq_cdev_get_state,
	.get_max_state = vm_cpufreq_cdev_get_max_state,
};

/* Create cooling device for a cluster */
static int vm_cpufreq_create_cooling_device(struct vm_cpufreq_cluster *cluster)
{
	struct vm_cpufreq_cdev_device *cdev_data;
	struct device_node *np;
	int cpu, ret = 0;

	if (!cluster->freq_table || cluster->num_levels == 0)
		return -EINVAL;

	cpu = cpumask_first(&cluster->cpus);
	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	cdev_data = kzalloc(sizeof(*cdev_data), GFP_KERNEL);
	if (!cdev_data)
		return -ENOMEM;

	cdev_data->cluster = cluster;
	cdev_data->cpu = cpu;
	cdev_data->max_state = cluster->num_levels - 1;
	cdev_data->cur_state = 0;

	snprintf(cdev_data->cdev_name, sizeof(cdev_data->cdev_name),
		 "vm-thermal-cpufreq%d", cluster->domain);

	/*
	 * Register cooling device with device tree support.
	 * This links the cooling device to the CPU's device tree node,
	 * allowing thermal zones to reference it as <&cpu4 0 15>.
	 */
	np = of_cpu_device_node_get(cpu);
	cdev_data->cdev = thermal_of_cooling_device_register(np,
							     cdev_data->cdev_name,
							     cdev_data,
							     &vm_cpufreq_cdev_ops);
	of_node_put(np);

	if (IS_ERR(cdev_data->cdev)) {
		ret = PTR_ERR(cdev_data->cdev);
		dev_err(cluster->dev, "Failed to register cooling device %s: %d\n",
			cdev_data->cdev_name, ret);
		kfree(cdev_data);
		return ret;
	}

	/* Add to global list */
	mutex_lock(&vm_cdev_list_lock);
	list_add(&cdev_data->node, &vm_cdev_list);
	mutex_unlock(&vm_cdev_list_lock);

	dev_info(cluster->dev, "Registered cooling device %s for domain %d (CPU %d)\n",
		 cdev_data->cdev_name, cluster->domain, cpu);

	return 0;
}

/* Remove cooling device for a cluster */
static void vm_cpufreq_remove_cooling_device(struct vm_cpufreq_cluster *cluster)
{
	struct vm_cpufreq_cdev_device *cdev_data, *tmp;

	mutex_lock(&vm_cdev_list_lock);
	list_for_each_entry_safe(cdev_data, tmp, &vm_cdev_list, node) {
		if (cdev_data->cluster == cluster) {
			list_del(&cdev_data->node);
			thermal_cooling_device_unregister(cdev_data->cdev);
			kfree(cdev_data);
			break;
		}
	}
	mutex_unlock(&vm_cdev_list_lock);
}

/* File operations */
/*
 * cluster_open is invoked by the misc framework's open path, which holds
 * the framework's internal mutex across the entire f_op->open() call
 * (see drivers/char/misc.c). vm_cpufreq_release_all() reaches kref_put()
 * only after the misc device is unregistered, which acquires the same
 * framework mutex — so the two are serialized: while we are inside
 * cluster_open the cluster cannot reach refcount 0 and be freed under us.
 *
 * Within that outer barrier, cluster->lock then orders the ph read here
 * against release_all's WRITE_ONCE(ph, NULL): if release_all wrote NULL
 * first, we observe it and bail with -ENODEV; otherwise we acquire the
 * client kref before dropping the lock, keeping the cluster alive past
 * the framework mutex release.
 *
 * If this code is ever ported off the misc framework, the framework-mutex
 * half of the contract has to be replaced explicitly.
 */
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
	INIT_LIST_HEAD(&ctx->node);

	guard(mutex)(&cluster->lock);

	if (!READ_ONCE(cluster->ph))
		return -ENODEV;

	/*
	 * Cap concurrent clients to bound memory and per-ioctl aggregation
	 * cost. See VM_CPUFREQ_MAX_CLIENTS for rationale.
	 */
	if (cluster->client_count >= VM_CPUFREQ_MAX_CLIENTS)
		return -EBUSY;

	kref_get(&cluster->ref);
	list_add_tail(&ctx->node, &cluster->client_list);
	cluster->client_count++;

	filp->private_data = no_free_ptr(ctx);
	return 0;
}

static int cluster_release(struct inode *inode, struct file *filp)
{
	struct vm_cpufreq_cluster *cluster_ref __free(vm_cpufreq_cluster) = NULL;
	struct vm_cpufreq_client_ctx *ctx __free(vm_cpufreq_client_ctx) =
		filp->private_data;
	struct vm_cpufreq_cluster *cluster;

	cluster = ctx->cluster;
	cluster_ref = cluster; /* Hold ref to put it at end of function */

	scoped_guard(mutex, &cluster->lock) {
		list_del(&ctx->node);
		WARN_ON(!cluster->client_count);
		cluster->client_count--;
		/*
		 * Userspace-governor semantics: a closing client withdraws
		 * its vote from the aggregation list (the list_del above)
		 * but does not push firmware to a new level. Firmware stays
		 * at the last applied freq until the next SET ioctl or
		 * thermal cdev event triggers a fresh recompute. Userspace
		 * that wants firmware to drop on exit must explicitly SET
		 * a lower level before closing — same convention as
		 * cpufreq's userspace governor (sticky last value).
		 */
	}

	filp->private_data = NULL;
	return 0;
}

static int handle_level_set(struct vm_cpufreq_cluster *cluster,
			     struct vm_cpufreq_client_ctx *ctx,
			     char __user *argp)
{
	struct cpu_perf_level_req req;
	u32 freq_khz;
	u32 old_freq_khz;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = vm_cpufreq_index_to_freq(cluster, req.level, &freq_khz);
	if (ret)
		return ret;

	if (unlikely(freq_khz == 0))
		return -EINVAL;

	guard(mutex)(&cluster->lock);
	old_freq_khz = ctx->client.freq_khz;
	ctx->client.freq_khz = freq_khz;
	ret = vm_cpufreq_recompute_and_update(cluster, false);
	if (ret)
		ctx->client.freq_khz = old_freq_khz;

	return ret;
}

static int handle_level_get(struct vm_cpufreq_cluster *cluster,
			     char __user *argp)
{
	struct cpu_perf_level_req req = {0};
	u32 freq_khz;
	int ret;

	ret = vm_cpufreq_query_freq_khz(cluster, &freq_khz);
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
	 * num_levels and freq_table[] are post-init immutable. Sysfs and
	 * ioctl readers reach this code only after init_freq_table has
	 * completed: the cluster_ioctl entry guard returns -ENODEV during
	 * the brief window between misc-device registration and
	 * init_freq_table completing; the policy_kobj used by sysfs is
	 * created after init_freq_table.
	 *
	 * Cap num_levels to MAX_PERF_LEVELS so that userspace cannot read
	 * avail.num_levels and then index avail.levels[] beyond its bounds.
	 */
	avail.num_levels = min_t(u32, cluster->num_levels, (u32)MAX_PERF_LEVELS);
	for (i = 0; i < avail.num_levels; i++) {
		avail.levels[i] = i;
		avail.freq_khz[i] = cluster->freq_table[i];
	}

	if (copy_to_user(argp, &avail, sizeof(avail)))
		return -EFAULT;

	return 0;
}

static int handle_get_transition_latency(struct vm_cpufreq_cluster *cluster,
					 char __user *argp)
{
	struct cpu_perf_transition_latency lat = {0};

	lat.latency_us = cluster->transition_latency_us;

	if (copy_to_user(argp, &lat, sizeof(lat)))
		return -EFAULT;

	return 0;
}

static long cluster_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct vm_cpufreq_client_ctx *ctx = file->private_data;
	char __user *argp = (char __user *)arg;
	struct vm_cpufreq_cluster *cluster;
	int ret = -ENOTTY, srcu_idx;

	cluster = ctx->cluster;

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

	if (!cluster->freq_table || !cluster->num_levels) {
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
	case CPU_PERF_TRANSITION_LATENCY_GET:
		if (_IOC_SIZE(cmd) != sizeof(struct cpu_perf_transition_latency)) {
			ret = -EINVAL;
			goto out;
		}
		ret = handle_get_transition_latency(cluster, argp);
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
	u32 freq_khz;
	int ret;

	ret = vm_cpufreq_query_freq_khz(cluster, &freq_khz);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", freq_khz);
}
VM_CPUFREQ_ATTR_RO(scaling_cur_freq);

static ssize_t vm_cpufreq_scaling_available_frequencies_show(
				struct vm_cpufreq_cluster *cluster, char *buf)
{
	u32 freq_khz;
	int count = 0, ret;	/* int matches sysfs_emit_at()'s 'int at' parameter */
	u32 index;

	/*
	 * Sysfs reaches this function via policy_kobj, which is only
	 * created after misc-device registration and init_freq_table
	 * complete. freq_table and num_levels are stable thereafter; no
	 * extra ordering needed.
	 */
	if (!cluster->freq_table || !cluster->num_levels)
		return sysfs_emit(buf, "\n");

	for (index = 0; index < cluster->num_levels; index++) {
		if (vm_cpufreq_index_to_freq(cluster, index, &freq_khz))
			break;
		ret = sysfs_emit_at(buf, count, "%u ", freq_khz);
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
	unsigned int cpu;
	ssize_t i = 0;

	for_each_cpu(cpu, &cluster->cpus) {
		int ret = sysfs_emit_at(buf, i, "%u ", cpu);

		if (ret < 0)
			break;
		i += ret;
		if (i >= (PAGE_SIZE - 5))
			break;
	}

	if (i == 0)
		return sysfs_emit(buf, "\n");

	/* Strip trailing space and terminate with newline */
	i--;
	i += sysfs_emit_at(buf, i, "\n");
	return i;
}
VM_CPUFREQ_ATTR_RO(affected_cpus);

static ssize_t vm_cpufreq_cpuinfo_transition_latency_show(
				struct vm_cpufreq_cluster *cluster, char *buf)
{
	return sysfs_emit(buf, "%u\n", cluster->transition_latency_us);
}
VM_CPUFREQ_ATTR_RO(cpuinfo_transition_latency);

static struct attribute *vm_cpufreq_attrs[] = {
	&vm_cpufreq_attr_scaling_cur_freq.attr,
	&vm_cpufreq_attr_scaling_available_frequencies.attr,
	&vm_cpufreq_attr_affected_cpus.attr,
	&vm_cpufreq_attr_cpuinfo_transition_latency.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vm_cpufreq);

static ssize_t vm_cpufreq_attr_show(struct kobject *kobj,
				    struct attribute *attr, char *buf)
{
	struct vm_cpufreq_attr *vattr =
		container_of(attr, struct vm_cpufreq_attr, attr);
	struct vm_policy_node *node =
		container_of(kobj, struct vm_policy_node, kobj);

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
	struct device *cpu_dev;
	int cpu;

	if (!cluster->policy_kobj)
		return;

	/*
	 * Only tear down the symlinks we actually created. If sysfs_create_link
	 * returned -EEXIST during create (because another cpufreq driver had
	 * already populated cpuN/cpufreq), we never tracked that CPU and must
	 * not delete the symlink owned by the other driver.
	 */
	for_each_cpu(cpu, &cluster->policy_link_cpus) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;
		sysfs_remove_link(&cpu_dev->kobj, "cpufreq");
	}
	cpumask_clear(&cluster->policy_link_cpus);

	/* Decrement refcount of kobj; this eventually calls vm_policy_release */
	kobject_put(cluster->policy_kobj);
	cluster->policy_kobj = NULL;
}

static int vm_cpufreq_create_policy_structure(struct vm_cpufreq_cluster *cluster)
{
	struct vm_policy_node *node;
	struct device *cpu_dev;
	char name[16];
	int cpu, ret;

	if (!cpufreq_global_kobj)
		return -ENODEV;

	cpumask_clear(&cluster->policy_link_cpus);

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
		if (ret == -EEXIST) {
			/*
			 * Another cpufreq driver already owns this symlink.
			 * Leave it alone and skip tracking; on remove we will
			 * not touch it.
			 */
			continue;
		}
		if (ret) {
			vm_cpufreq_remove_policy_structure(cluster);
			return ret;
		}
		cpumask_set_cpu(cpu, &cluster->policy_link_cpus);
	}

	return 0;
}

static void vm_cpufreq_build_cpumask(struct vm_cpufreq_cluster *cluster,
				     int domain_id)
{
	int cpu, ret;

	cpumask_clear(&cluster->cpus);
	for_each_possible_cpu(cpu) {
		ret = vm_cpufreq_parse_cpu_domain(cpu);
		if (ret < 0) {
			dev_dbg(cluster->dev,
				"CPU %d: failed to parse domain: %d\n", cpu, ret);
			continue;
		}
		if (ret == domain_id)
			cpumask_set_cpu(cpu, &cluster->cpus);
	}
}

/* This is called via devm action on driver remove/failure */
static void vm_cpufreq_release_all(void *data)
{
	struct vm_cpufreq_cluster *cluster, *tmp;
	struct vm_cpufreq_context *ctx = data;

	list_for_each_entry_safe(cluster, tmp, &ctx->clusters, list) {
		list_del(&cluster->list);

		scoped_guard(mutex, &cluster->lock)
			WRITE_ONCE(cluster->ph, NULL);
		synchronize_srcu(&cluster->hw_srcu);

		/*
		 * vm_cpufreq_remove_cooling_device is a synchronization point
		 * with in-flight thermal cdev callbacks: it calls
		 * thermal_cooling_device_unregister, which synchronously waits
		 * for any running set_cur_state callback to finish and then
		 * frees cdev_data. After it returns, no callback can re-enter
		 * the driver via this cluster's cdev. The kref_put below must
		 * therefore not be reordered before this call — otherwise an
		 * in-flight callback could observe a freed cluster.
		 */
		vm_cpufreq_remove_cooling_device(cluster);

		vm_cpufreq_remove_policy_structure(cluster);
		dev_pm_opp_remove_all_dynamic(cluster->opp_dev);

		/*
		 * Unregistering the misc device is the synchronization point
		 * with in-flight cluster_open: it acquires the misc framework
		 * mutex which the framework's open path holds across
		 * f_op->open(). After it returns, no cluster_open call is in
		 * progress and no new one can start, so the kref_put below
		 * cannot race with a cluster_open observing a half-torn-down
		 * cluster. Do not reorder kref_put before this call.
		 *
		 * The unregister works even if registration failed (if zeroed),
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
	struct device *cpu_dev;
	int cpu, ret;

	cluster = kzalloc(sizeof(*cluster), GFP_KERNEL);
	if (!cluster)
		return -ENOMEM;

	kref_init(&cluster->ref);
	cluster->domain = domain_id;
	cluster->ph = ph;
	cluster->perf_ops = perf_ops;
	cluster->dev = &sdev->dev;
	cluster->thermal_cap_khz = U32_MAX;
	cluster->agg_applied_freq_khz = U32_MAX;
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
	cluster->transition_latency_us = (ret > 0) ?
		(u32)(ret / 1000) : DEFAULT_TRANSITION_LATENCY_US;
	dev_info(&sdev->dev, "Domain %d: transition_latency=%u us%s\n",
		 domain_id, cluster->transition_latency_us,
		 (ret > 0) ? " (from SCMI)" : " (default fallback)");

	vm_cpufreq_build_cpumask(cluster, domain_id);

	if (cpumask_empty(&cluster->cpus)) {
		dev_dbg(&sdev->dev, "Domain %d: no CPUs, skipping\n", domain_id);
		kref_put(&cluster->ref, vm_cpufreq_cluster_release_mem);
		return 0;
	}

	cpu = cpumask_first(&cluster->cpus);
	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev) {
		dev_err(&sdev->dev, "Domain %d: get_cpu_device(%d) failed\n",
			domain_id, cpu);
		ret = -ENODEV;
		goto err_free_cluster;
	}

	ret = perf_ops->device_opps_add(ph, cpu_dev, domain_id);
	if (ret) {
		dev_err(&sdev->dev,
			"Domain %d: device_opps_add(cpu_dev=%s) failed: %d\n",
			domain_id, dev_name(cpu_dev), ret);
		goto err_free_cluster;
	}
	cluster->opp_dev = cpu_dev;
	dev_info(&sdev->dev, "Domain %d: OPPs on cpu_dev=%s (no init window)\n",
		 domain_id, dev_name(cpu_dev));

	ret = vm_cpufreq_init_freq_table(cluster);
	if (ret) {
		dev_err(&sdev->dev,
			"Domain %d: init_freq_table failed: %d\n",
			domain_id, ret);
		goto err_remove_opps;
	}

	cluster->miscdev.minor = MISC_DYNAMIC_MINOR;
	cluster->miscdev.name = kasprintf(GFP_KERNEL, "cpu-freq-domain%d", domain_id);
	if (!cluster->miscdev.name) {
		ret = -ENOMEM;
		goto err_remove_opps;
	}
	cluster->miscdev.fops = &cluster_fops;
	cluster->miscdev.parent = &sdev->dev;

	ret = misc_register(&cluster->miscdev);
	if (ret) {
		dev_err(&sdev->dev,
			"Domain %d: misc device registration failed: %d\n",
			domain_id, ret);
		goto err_remove_opps;
	}

	ret = vm_cpufreq_create_policy_structure(cluster);
	if (ret)
		dev_warn(&sdev->dev,
			 "Domain %d: sysfs policy structure unavailable: %d\n",
			 domain_id, ret);

	ret = vm_cpufreq_create_cooling_device(cluster);
	if (ret)
		dev_warn(&sdev->dev,
			 "Domain %d: cooling device creation failed: %d\n",
			 domain_id, ret);

	list_add(&cluster->list, &ctx->clusters);

	dev_info(&sdev->dev, "Registered domain %d with %d CPUs (opp_dev=%s)\n",
		 domain_id, cpumask_weight(&cluster->cpus),
		 dev_name(cluster->opp_dev));

	return 0;

err_remove_opps:
	dev_pm_opp_remove_all_dynamic(cluster->opp_dev);
err_free_cluster:
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
		struct device_node *np2 __free(device_node) = NULL;

		int perf_idx;

		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;
		np = of_parse_phandle(dev_of_node(cpu_dev), "clocks", 0);
		if (np && np == scmi_np)
			return true;
		perf_idx = of_property_match_string(dev_of_node(cpu_dev),
						    "power-domain-names", "perf");
		if (perf_idx < 0)
			continue;
		np2 = of_parse_phandle(dev_of_node(cpu_dev), "power-domains",
				       perf_idx);
		if (np2 && np2 == scmi_np)
			return true;
	}

	return false;
}

static int vm_cpufreq_probe(struct scmi_device *sdev)
{
	const struct scmi_handle *handle = sdev->handle;
	const struct scmi_perf_proto_ops *perf_ops;
	struct scmi_protocol_handle *ph;
	struct device *dev = &sdev->dev;
	struct vm_cpufreq_context *ctx;
	int num_domains, ret, i;

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
		/*
		 * -EPROBE_DEFER from a domain means a downstream dependency
		 * isn't ready yet. Propagate so the kernel retries the whole
		 * probe; devm cleanup will tear down any clusters built so far.
		 */
		if (ret == -EPROBE_DEFER)
			return ret;
		/* Other failures: warn and continue with remaining domains. */
		if (ret)
			dev_warn(dev, "Failed to process domain %d: %d\n", i, ret);
	}

	if (list_empty(&ctx->clusters)) {
		dev_warn(dev, "No domains with CPUs found; check DT clock/power-domain bindings\n");
		return -ENODEV;
	}

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

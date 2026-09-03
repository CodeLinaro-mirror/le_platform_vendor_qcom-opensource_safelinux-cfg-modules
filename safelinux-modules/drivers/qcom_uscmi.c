/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/seq_file.h>
#include <uapi/misc/qcom_uscmi.h>

#define CREATE_TRACE_POINTS
#include "qcom_uscmi_trace.h"

#define MAX_CLIENTS 10000
#define DEFAULT_CLIENT_LIMIT 100

/**
 * struct uscmi_stats - Driver statistics structure
 * @power_on_count: Total number of power-on operations
 * @power_off_count: Total number of power-off operations
 * @perf_set_count: Total number of performance level set operations
 * @reset_count: Total number of reset operations
 * @client_count: Current number of active clients
 * @max_clients: Maximum number of concurrent clients reached
 * @error_count: Total number of operation errors
 *
 * Tracks various statistics for debugging and monitoring driver behavior.
 * All counters are atomic to allow lock-free updates.
 */
struct uscmi_stats {
	atomic64_t power_on_count;
	atomic64_t power_off_count;
	atomic64_t perf_set_count;
	atomic64_t rst_assert_count;
	atomic64_t rst_deassert_count;
	atomic64_t rst_reset_count;
	atomic_t client_count;
	atomic_t max_clients;
	atomic64_t error_count;
};

/**
 * struct qcom_uscmi_client - Client connection structure
 * @uscmi: Pointer to parent USCMI device
 * @node: List node for client list
 * @pwr_votes: Per-domain power vote counts (atomic)
 * @prf_votes: Per-domain performance level votes (-1 = no vote)
 * @comm: Client process name for debugging
 * @pid: Client process ID for debugging
 *
 * Represents a single client connection to the USCMI device. Each client
 * maintains independent vote counts for power and performance operations.
 * Members are ordered for optimal struct packing.
 */
struct qcom_uscmi_client {
	struct qcom_uscmi_dev *uscmi;
	atomic_t *pwr_votes;
	int *prf_votes;
	struct list_head node;
	char comm[TASK_COMM_LEN];
	pid_t pid;
};

/**
 * struct qcom_uscmi_dev - Main driver structure
 * @dev: Platform device pointer
 * @miscdev: Miscellaneous device for /dev node
 * @name: Device name
 * @dev_lock: Lock for serializing all operations and protecting device state
 * @clients: List of clients
 * @pd_list: List of power domains (contains pd_devs array)
 * @resets: Array of reset control pointers
 * @reset_names: Array of reset domain names
 * @domain_names: Array of domain names (for debugging)
 * @debugfs_dir: debugfs root directory
 * @stats: Statistics for debugging
 * @domain_count: Number of power domains
 * @reset_count: Number of reset domains
 * @client_limit: Maximum number of clients allowed
 * @domains_attached: Whether power domains are currently attached
 * @pm_domains_on_demand: Whether to use on-demand power domain attachment
 *
 * This is the main structure for the USCMI driver, containing all the
 * necessary information to manage power domains, reset domains, and clients.
 */
struct qcom_uscmi_dev {
	/* Core device info */
	struct device *dev;
	struct miscdevice miscdev;
	const char *name;

	/* Locks */
	struct mutex dev_lock;

	/* Client management */
	struct list_head clients;

	/* Power domain management */
	struct dev_pm_domain_list *pd_list;
	const char **domain_names;
	int domain_count;
	bool domains_attached;
	bool pm_domains_on_demand;

	/* Reset control management */
	struct reset_control **resets;
	const char **reset_names;
	int reset_count;

	/* Debugging and statistics */
	struct dentry *debugfs_dir;
	struct uscmi_stats stats;
	u32 client_limit;
};

#define miscdev_to_data(d) container_of(d, struct qcom_uscmi_dev, miscdev)

/**
 * qcom_uscmi_client_free - Free client and all nested allocations
 * @cl: Pointer to client structure
 *
 * Custom cleanup function for use with __free() attribute.
 * Frees the client structure and all its nested allocations.
 */
static void qcom_uscmi_client_free(struct qcom_uscmi_client *cl)
{
	if (cl) {
		kfree(cl->pwr_votes);
		kfree(cl->prf_votes);
		kfree(cl);
	}
}

DEFINE_FREE(qcom_uscmi_client, struct qcom_uscmi_client *,
	if (_T)
		qcom_uscmi_client_free(_T))

/**
 * get_pd_dev - Get power domain device by name
 * @uscmi: USCMI device structure
 * @name: Power domain name to find
 * @idx: Pointer to store the domain index
 *
 * This function assumes that uscmi->domain_count is at least 1 when handling
 * the single domain case (!pd_list). Callers must ensure that domain_count > 0
 * before calling this function to avoid potential out-of-bounds access with
 * the returned index.
 *
 * Return: Device pointer on success, NULL on failure
 */
static struct device *get_pd_dev(struct qcom_uscmi_dev *uscmi, char *name,
				 int *idx)
{
	int index = 0;
	size_t len;

	if (!uscmi->pd_list) { /* single domain */
		*idx = index;
		return uscmi->dev;
	}

	len = strnlen(name, NAME_LEN);
	if (!len || len == NAME_LEN)
		return NULL;

	name[len] = '\0';
	index = of_property_match_string(uscmi->dev->of_node,
					 "power-domain-names", name);

	if (index < 0) {
		dev_err(uscmi->dev, "power domain %s not found\n", name);
		return NULL;
	}

	if (!uscmi->pd_list->pd_devs || index >= uscmi->pd_list->num_pds) {
		dev_err(uscmi->dev, "Invalid domain index %d (max: %d)\n",
			index, uscmi->pd_list->num_pds);
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
	dev_dbg(dev, "domain is %s\n",
		genpd->flags & GENPD_FLAG_ALWAYS_ON ? "always on" :
						      "not always on");

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

	if (!uscmi->pd_list || !uscmi->pd_list->pd_devs)
		return true;

	for (i = 0; i < uscmi->pd_list->num_pds; i++) {
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
 * Attach power domains dynamically when pm_domains_on_demand is enabled.
 * This function is always called under ioctl mutex lock.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qcom_uscmi_attach_domains(struct qcom_uscmi_dev *uscmi)
{
	int num_pds;
	ktime_t start_time, end_time;
	s64 duration_us;

	/**
	 * skip attach if domains are already attached or dynamic attach/detach
	 * is not supported
	 */

	if (!uscmi->pm_domains_on_demand || uscmi->domains_attached)
		return 0;

	/*
	 * Control should never reach here if the device does not have any PM
	 *
	 */
	start_time = ktime_get();
	num_pds = dev_pm_domain_attach_list(uscmi->dev, NULL, &uscmi->pd_list);
	if (num_pds <= 0) {
		dev_err(uscmi->dev, "multi domain attach failed (ret=%d)%s\n",
			num_pds,
			!num_pds ? " (no power domains attached)" : "");
		return num_pds ? num_pds : -ENODEV;
	}

	dev_dbg(uscmi->dev, "multi domain attach success (num_pds=%d)\n",
		num_pds);

	uscmi->domains_attached = true;
	end_time = ktime_get();
	duration_us = ktime_to_us(ktime_sub(end_time, start_time));
	trace_qcom_uscmi_pd_attach(dev_name(uscmi->dev), num_pds, 0,
				   duration_us);

	return 0;
}

/**
 * qcom_uscmi_detach_domains - Detach power domains if all are off
 * @uscmi: USCMI device structure
 * @operation: Operation type for logging
 * @force: Force detach even if domains are on (e.g., on error)
 *
 * Detach power domains when all non-always-on domains are off or when
 * forced. This function is always called under ioctl mutex lock.
 */
static void qcom_uscmi_detach_domains(struct qcom_uscmi_dev *uscmi,
				      int operation, bool force)
{
	ktime_t start_time, end_time;
	s64 duration_us;

	/**
	 * skip detach if domains are not attached or dynamic attach/detach
	 * is not supported
	 */
	if (!uscmi->pm_domains_on_demand || !uscmi->domains_attached)
		return;

	start_time = ktime_get();
	if (force || qcom_uscmi_all_pds_off(uscmi)) {
		if (uscmi->pd_list)
			dev_pm_domain_detach_list(uscmi->pd_list);
		dev_dbg(uscmi->dev,
			"detached domains after operation %d force:%d\n",
			operation, force);
		uscmi->domains_attached = false;
		uscmi->pd_list = NULL;

		end_time = ktime_get();
		duration_us = ktime_to_us(ktime_sub(end_time, start_time));
		trace_qcom_uscmi_pd_detach(dev_name(uscmi->dev), operation,
					   force, duration_us);
	} else {
		dev_dbg(uscmi->dev,
			"detach skipped after operation %d, some domains are ON\n",
			operation);
	}
}

/**
 * __do_power_vote - Core power vote operation (internal)
 * @pd: Power domain device
 * @cl: Client structure
 * @idx: Domain index
 * @vote_on: true to vote ON, false to vote OFF
 *
 * Must be called with dev_lock held.
 * Does NOT handle attach/detach or tracing.
 *
 * Return: 0 on success, negative error code on failure
 */
static int __do_power_vote(struct device *pd, struct qcom_uscmi_client *cl,
			   int idx, bool vote_on)
{
	int ret, old_vote;

	if (!pd || !pd->pm_domain)
		return -EINVAL;

	if (vote_on) {
		ret = pm_runtime_resume_and_get(pd);
		if (ret < 0) {
			dev_err(pd, "Power on failed (err=%d)\n", ret);
			return ret;
		}
		old_vote = atomic_fetch_inc(&cl->pwr_votes[idx]);
		dev_dbg(pd, "[client %s:%d] power refcount incremented to %d\n",
			cl->comm, cl->pid, old_vote + 1);
	} else {
		old_vote = atomic_fetch_dec(&cl->pwr_votes[idx]);
		if (old_vote <= 0) {
			/* Restore the counter */
			atomic_inc(&cl->pwr_votes[idx]);
			dev_warn(pd,
					"[client %s:%d] power refcount already 0\n",
					cl->comm, cl->pid);
			return -EINVAL;
		}

		dev_dbg(pd, "[client %s:%d] power refcount decremented to %d\n",
				cl->comm, cl->pid, old_vote - 1);

		ret = pm_runtime_put_sync(pd);
		if (ret < 0) {
			pm_runtime_get_noresume(pd);
			atomic_inc(&cl->pwr_votes[idx]);
			dev_err(pd, "Power off failed (err=%d)\n", ret);
			return ret;
		}
	}

	return 0;
}

/**
 * do_power_operation - Handle power IOCTL operations
 * @req: IOCTL request structure
 * @uscmi: USCMI device structure
 * @cl: Client structure
 *
 * IOCTL handler wrapper that adds attach/detach, tracing, and validation.
 *
 * Return: 0 on success, negative error code on failure
 */
static int do_power_operation(scmi_oper_ioctl_t *req,
			      struct qcom_uscmi_dev *uscmi,
			      struct qcom_uscmi_client *cl)
{
	struct device *pd = NULL;
	int ret = 0, idx = 0;
	ktime_t start_time, end_time;
	u64 duration_us;
	bool vote_on;

	start_time = ktime_get();

	/* Check if client has power vote arrays allocated */
	if (!cl->pwr_votes) {
		dev_err(uscmi->dev,
			"No power domains configured for this client\n");
		return -EINVAL;
	}

	ret = qcom_uscmi_attach_domains(uscmi);
	if (ret)
		goto out_detach;

	pd = get_pd_dev(uscmi, req->name, &idx);
	if (!pd || req->proto != SCMI_PROTO_POWER) {
		ret = -EINVAL;
		goto out_detach;
	}

	/* Validate domain index */
	if (idx < 0 || idx >= uscmi->domain_count) {
		dev_err(uscmi->dev, "Invalid domain index %d (max: %d)\n", idx,
			uscmi->domain_count);
		ret = -EINVAL;
		goto out_detach;
	}

	switch (req->oper) {
	case SCMI_PWR_ON:
		vote_on = true;
		atomic64_inc(&uscmi->stats.power_on_count);
		break;
	case SCMI_PWR_OFF:
		vote_on = false;
		atomic64_inc(&uscmi->stats.power_off_count);
		break;
	default:
		dev_warn(pd, "power operation(%d) not supported\n", req->oper);
		ret = -EINVAL;
		goto out_detach;
	}

	/* Call core operation */
	ret = __do_power_vote(pd, cl, idx, vote_on);

out_detach:
	if (ret < 0) {
		atomic64_inc(&uscmi->stats.error_count);
		dev_err(pd != NULL ? pd : uscmi->dev,
			"power operation(%d) failed with err=%d\n", req->oper,
			ret);
	}

	end_time = ktime_get();
	duration_us = ktime_to_us(ktime_sub(end_time, start_time));

	trace_qcom_uscmi_power_op(req->name[0] ? req->name : "power", req->oper,
				  ret, duration_us);
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
	struct dev_pm_opp *opp;
	int ret;

	opp = dev_pm_opp_find_level_exact(dev, level);
	if (IS_ERR(opp))
		return -EINVAL;

	ret = dev_pm_opp_set_opp(dev, opp);
	dev_pm_opp_put(opp);

	return ret;
}

/**
 * recalculate_perf_level - Recalculate aggregated performance level for a
 * domain
 * @uscmi: Driver structure
 * @domain_idx: Index of the domain to recalculate (must be valid, caller
 * ensures this)
 *
 * This function recalculates the aggregated performance level for a domain
 * based on the votes from all clients. It finds the maximum performance level
 * requested by any client.
 *
 * LOCKING: Must be called with dev_lock held to protect client list iteration.
 * PRECONDITION: domain_idx must be valid (0 <= domain_idx < domain_count).
 *               All callers validate this before calling.
 *
 * Return: The new aggregated performance level, or -1 if no votes
 */
static int recalculate_perf_level(struct qcom_uscmi_dev *uscmi, int domain_idx)
{
	struct qcom_uscmi_client *cl;
	int max_level = -1;

	/* Defensive bounds check */
	if (WARN_ON(domain_idx < 0 || domain_idx >= uscmi->domain_count))
		return -1;

	/* Check if client list is empty - dev_lock must be held by caller */
	if (list_empty(&uscmi->clients))
		return -1;

	/* Find the highest performance level among all clients */
	list_for_each_entry(cl, &uscmi->clients, node) {
		if (cl->prf_votes && cl->prf_votes[domain_idx] >= 0 &&
		    cl->prf_votes[domain_idx] > max_level) {
			max_level = cl->prf_votes[domain_idx];
		}
	}

	return max_level;
}

/**
 * __do_perf_vote - Core performance vote operation (internal)
 * @pd: Power domain device
 * @uscmi: USCMI device structure
 * @cl: Client structure
 * @idx: Domain index
 * @level: Performance level to set (-1 to clear vote)
 *
 * Must be called with dev_lock held.
 * Does NOT handle attach/detach or tracing.
 * Assumes domain is already powered on if setting a level.
 *
 * Return: 0 on success, negative error code on failure
 */
static int __do_perf_vote(struct device *pd, struct qcom_uscmi_dev *uscmi,
			  struct qcom_uscmi_client *cl, int idx, int level)
{
	int ret, max_level, prev_level, def_level = 0;
	struct dev_pm_opp *def_opp;

	if (!pd)
		return -EINVAL;

	if (dev_pm_opp_get_opp_count(pd) <= 0)
		return -EINVAL;

	prev_level = cl->prf_votes[idx];

	/* Update client vote */
	cl->prf_votes[idx] = level;

	if (level >= 0) {
		dev_dbg(pd, "[client %s:%d] perf level set to %d\n", cl->comm,
			cl->pid, level);
	} else {
		dev_dbg(pd, "[client %s:%d] perf vote cleared\n", cl->comm,
			cl->pid);
	}

	/* Recalculate aggregated level */
	max_level = recalculate_perf_level(uscmi, idx);

	if (max_level < 0) {
		def_opp = dev_pm_opp_find_level_ceil(pd, &def_level);
		if (IS_ERR(def_opp)) {
			cl->prf_votes[idx] = prev_level;
			dev_err(pd, "failed to find default opp level\n");
			return -ENODEV;
		}
		max_level = def_level;
		dev_pm_opp_put(def_opp);
	}

	dev_dbg(pd, "[agg] max perf level updated to %d\n", max_level);

	/* Apply aggregated level - caller ensures domain is active */
	ret = dev_pm_opp_apply_level(pd, max_level);
	if (ret < 0) {
		cl->prf_votes[idx] = prev_level;
		dev_err(pd, "Failed to apply OPP level %d: %d\n", max_level,
			ret);
		return ret;
	}

	return 0;
}

/**
 * do_performance_operation - Handle performance IOCTL operations
 * @req: SCMI operation request structure
 * @uscmi: USCMI device structure
 * @cl: Client structure
 *
 * IOCTL handler wrapper that adds attach/detach, tracing, and validation.
 *
 * Return: 0 on success, negative error code on failure
 */
static int do_performance_operation(scmi_oper_ioctl_t *req,
				    struct qcom_uscmi_dev *uscmi,
				    struct qcom_uscmi_client *cl)
{
	struct device *pd = NULL;
	int ret, idx;
	ktime_t start_time, end_time;
	u64 duration_us;

	start_time = ktime_get();

	/* Check if client has performance vote arrays allocated */
	if (!cl->prf_votes) {
		dev_err(uscmi->dev,
			"No power domains configured for this client\n");
		return -EINVAL;
	}

	ret = qcom_uscmi_attach_domains(uscmi);
	if (ret)
		goto out_detach;

	pd = get_pd_dev(uscmi, req->name, &idx);
	if (!pd || req->proto != SCMI_PROTO_PERFORMANCE) {
		ret = -EINVAL;
		goto out_detach;
	}

	/* Validate domain index */
	if (idx < 0 || idx >= uscmi->domain_count) {
		dev_err(uscmi->dev, "Invalid domain index %d (max: %d)\n", idx,
			uscmi->domain_count);
		ret = -EINVAL;
		goto out_detach;
	}

	if (req->oper != SCMI_PRF_LVL_SET) {
		dev_warn(pd, "performance operation(%d) not supported\n",
			 req->oper);
		ret = -EINVAL;
		goto out_detach;
	}

	/* Power on domain for OPP operation */
	ret = pm_runtime_resume_and_get(pd);
	if (ret < 0) {
		dev_err(pd, "Failed to power on domain: %d\n", ret);
		goto out_detach;
	}

	/* Set performance level */
	ret = __do_perf_vote(pd, uscmi, cl, idx, req->level);

	/* Power off domain */
	pm_runtime_put_sync(pd);

	/* Update statistics */
	if (ret == 0)
		atomic64_inc(&uscmi->stats.perf_set_count);

out_detach:
	if (ret < 0) {
		atomic64_inc(&uscmi->stats.error_count);
		dev_err(pd != NULL ? pd : uscmi->dev,
			"perf operation(%d) failed with err=%d\n", req->oper,
			ret);
	}

	end_time = ktime_get();
	duration_us = ktime_to_us(ktime_sub(end_time, start_time));

	trace_qcom_uscmi_perf_op(req->name[0] ? req->name : "perf", req->level,
				 ret, duration_us);
	qcom_uscmi_detach_domains(uscmi, req->oper, ret < 0);

	return ret;
}

/**
 * do_reset_operation - Execute reset operation
 * @req: SCMI operation request structure
 * @uscmi: USCMI device structure
 *
 * Handle reset domain operations (assert/deassert/reset).
 *
 * Return: 0 on success, negative error code on failure
 */
static int do_reset_operation(scmi_oper_ioctl_t *req,
			      struct qcom_uscmi_dev *uscmi)
{
	struct reset_control *rstc = NULL;
	int ret, i;
	size_t len;
	ktime_t start_time, end_time;
	u64 duration_us;

	start_time = ktime_get();
	if (!uscmi->reset_count || !uscmi->reset_names || !uscmi->resets) {
		ret = -ENODEV;
		goto err;
	}

	if (req->proto != SCMI_PROTO_RESET) {
		ret = -EINVAL;
		goto err;
	}

	len = strnlen(req->name, NAME_LEN);
	if (!len || len == NAME_LEN) {
		ret = -EINVAL;
		goto err;
	}

	req->name[len] = '\0';
	/* Find the reset control by name */
	for (i = 0; i < uscmi->reset_count; i++) {
		if (strcmp(uscmi->reset_names[i], req->name) == 0) {
			rstc = uscmi->resets[i];
			break;
		}
	}

	if (!rstc) {
		dev_err(uscmi->dev, "reset domain %s not found\n", req->name);
		ret = -ENODEV;
		goto err;
	}

	switch (req->oper) {
	case SCMI_RST_ASSERT:
		ret = reset_control_assert(rstc);
		if (ret == 0)
			atomic64_inc(&uscmi->stats.rst_assert_count);
		break;
	case SCMI_RST_DEASSERT:
		ret = reset_control_deassert(rstc);
		if (ret == 0)
			atomic64_inc(&uscmi->stats.rst_deassert_count);
		break;
	case SCMI_RST_RESET:
		ret = reset_control_reset(rstc);
		if (ret == 0)
			atomic64_inc(&uscmi->stats.rst_reset_count);
		break;
	default:
		ret = -EINVAL;
		break;
	}
err:
	if (ret < 0) {
		atomic64_inc(&uscmi->stats.error_count);
		dev_err(uscmi->dev, "reset operation(%d) failed with err=%d\n",
			req->oper, ret);
	}

	end_time = ktime_get();
	duration_us = ktime_to_us(ktime_sub(end_time, start_time));

	trace_qcom_uscmi_reset_op(req->name[0] ? req->name : "reset", req->oper,
				  ret, duration_us);
	return ret;
}

/**
 * allocate_client_votes - Allocate and initialize vote arrays for a client
 * @cl: Client structure
 * @domain_count: Number of domains
 *
 * Return: 0 on success, negative error code on failure
 */
static int allocate_client_votes(struct qcom_uscmi_client *cl, int domain_count)
{
	int i;

	if (!domain_count)
		return 0;

	cl->pwr_votes = kcalloc(domain_count, sizeof(atomic_t), GFP_KERNEL);
	if (!cl->pwr_votes)
		return -ENOMEM;

	cl->prf_votes = kcalloc(domain_count, sizeof(int), GFP_KERNEL);
	if (!cl->prf_votes)
		return -ENOMEM;

	/* Initialize performance votes to -1 (no vote) */
	for (i = 0; i < domain_count; i++)
		cl->prf_votes[i] = -1;

	return 0;
}

/**
 * add_client_to_list - Add client to device list and update statistics
 * @uscmi: USCMI device structure
 * @cl: Client structure
 *
 * Must be called with dev_lock held.
 */
static void add_client_to_list(struct qcom_uscmi_dev *uscmi,
			       struct qcom_uscmi_client *cl)
{
	int client_count;

	list_add(&cl->node, &uscmi->clients);
	client_count = atomic_inc_return(&uscmi->stats.client_count);

	/* Track maximum concurrent clients */
	if (client_count > atomic_read(&uscmi->stats.max_clients))
		atomic_set(&uscmi->stats.max_clients, client_count);
}

/**
 * qcom_uscmi_open - Open a new client connection
 * @inode: Inode structure
 * @filp: File structure
 *
 * This function creates a new client and initializes its state.
 * It also enforces client limits to prevent resource exhaustion.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qcom_uscmi_open(struct inode *inode, struct file *filp)
{
	struct qcom_uscmi_dev *uscmi = miscdev_to_data(filp->private_data);
	struct qcom_uscmi_client *cl __free(qcom_uscmi_client) = NULL;
	int ret, client_count;
	u32 client_limit;

	if (!uscmi)
		return -EINVAL;

	/* Hold lock for entire operation */
	mutex_lock(&uscmi->dev_lock);

	/* Check client limit BEFORE allocating anything */
	client_limit = READ_ONCE(uscmi->client_limit);
	if (!client_limit)
		client_limit = MAX_CLIENTS;
	client_limit = clamp_t(u32, client_limit, 1, MAX_CLIENTS);

	client_count = atomic_read(&uscmi->stats.client_count);
	if (client_count >= client_limit) {
		mutex_unlock(&uscmi->dev_lock);
		dev_err(uscmi->dev,
			"Client limit reached (%d clients), rejecting connection\n",
			client_count);
		return -EUSERS;
	}

	/* Allocate and initialize client structure */
	cl = kzalloc(sizeof(*cl), GFP_KERNEL);
	if (!cl) {
		mutex_unlock(&uscmi->dev_lock);
		return -ENOMEM;
	}

	cl->uscmi = uscmi;
	get_task_comm(cl->comm, current);
	cl->pid = task_pid_nr(current);

	/* Allocate vote arrays */
	ret = allocate_client_votes(cl, uscmi->domain_count);
	if (ret) {
		mutex_unlock(&uscmi->dev_lock);
		return ret;
	}

	/* Add to list and update statistics */
	add_client_to_list(uscmi, cl);

	mutex_unlock(&uscmi->dev_lock);

	dev_info(uscmi->dev, "fd opened by %s:%d with %d domain(s)\n", cl->comm,
		 cl->pid, uscmi->domain_count);
	filp->private_data = no_free_ptr(cl);
	return 0;
}

/**
 * cleanup_client_domain - Clean up votes for a single domain
 * @uscmi: USCMI device structure
 * @cl: Client structure
 * @pd: Power domain device
 * @idx: Domain index
 *
 * Must be called with dev_lock held.
 */
static void cleanup_client_domain(struct qcom_uscmi_dev *uscmi,
				  struct qcom_uscmi_client *cl,
				  struct device *pd, int idx)
{
	int old_vote, ret;

	if (!pd)
		return;

	/* Clear performance vote first (while powered) */
	if (cl->prf_votes[idx] >= 0) {
		dev_dbg(pd, "fd closing, clearing perf vote\n");

		/* Power on domain for OPP operation */
		ret = pm_runtime_resume_and_get(pd);
		if (ret < 0)
			dev_err(pd, "Failed to power on domain: %d\n", ret);

		__do_perf_vote(pd, uscmi, cl, idx, -1);

		/* Power off domain */
		if (ret >= 0)
			pm_runtime_put_sync(pd);
	}

	/* Release power votes (may power off) */
	old_vote = atomic_read(&cl->pwr_votes[idx]);
	if (old_vote > 0) {
		dev_dbg(pd, "fd closing with power refcount=%d, releasing\n",
			old_vote);
		while (old_vote > 0) {
			ret = __do_power_vote(pd, cl, idx, false);
			if (ret < 0)
				dev_err(pd, "[client %s:%d] failed to release power vote: %d\n",
					cl->comm, cl->pid, ret);
			old_vote--;
		}
	}
}

/**
 * cleanup_client_votes - Clean up all votes for a client
 * @uscmi: USCMI device structure
 * @cl: Client structure
 *
 * Must be called with dev_lock held.
 *
 * Return: 0 on success, negative error code on failure
 */
static int cleanup_client_votes(struct qcom_uscmi_dev *uscmi,
				struct qcom_uscmi_client *cl)
{
	struct device *pd;
	int i, ret;

	/* Attach domains if needed */
	ret = qcom_uscmi_attach_domains(uscmi);
	if (ret < 0) {
		/*
		 * If we cannot attach domains, we cannot safely access the
		 * power domain devices to decrease their reference counts. We
		 * must abort cleanup to prevent operating on invalid devices,
		 * even though this leaks hardware votes.
		 */
		dev_err(uscmi->dev,
			"Failed to attach domains for cleanup: %d\n", ret);
		return ret;
	}

	/* Clean up each domain */
	for (i = 0; i < uscmi->domain_count; i++) {
		if (uscmi->pd_list && uscmi->pd_list->pd_devs)
			pd = uscmi->pd_list->pd_devs[i];
		else
			pd = uscmi->dev; /* Single domain case */

		cleanup_client_domain(uscmi, cl, pd, i);
	}

	/* Detach domains if all are off */
	qcom_uscmi_detach_domains(uscmi, -1, false);

	return 0;
}

/**
 * qcom_uscmi_release - Release client connection
 * @inode: Inode structure
 * @filp: File structure
 *
 * Return: 0 on success
 */
static int qcom_uscmi_release(struct inode *inode, struct file *filp)
{
	struct qcom_uscmi_client *cl = filp->private_data;
	struct qcom_uscmi_dev *uscmi;
	struct device *dev;
	int ret = 0;

	if (!cl)
		return 0;

	uscmi = cl->uscmi;
	if (!uscmi)
		return 0;
	dev = uscmi->dev;

	mutex_lock(&uscmi->dev_lock);

	/* Clean up votes if client has domains */
	if (uscmi->domain_count && cl->pwr_votes && cl->prf_votes) {
		ret = cleanup_client_votes(uscmi, cl);
		if (ret < 0)
			dev_err(dev,
				"[client %s:%d] failed to clean up votes: %d\n",
				cl->comm, cl->pid, ret);
	}

	/* Remove from list and update count */
	list_del(&cl->node);
	atomic_dec(&uscmi->stats.client_count);

	mutex_unlock(&uscmi->dev_lock);

	/* Free client resources using custom cleanup function */
	qcom_uscmi_client_free(cl);

	filp->private_data = NULL;

	dev_dbg(dev, "fd closed and cleaned up\n");

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
	struct qcom_uscmi_client *cl = file->private_data;
	struct qcom_uscmi_dev *uscmi = cl->uscmi;
	char __user *argp = (char __user *)arg;
	scmi_oper_ioctl_t req;
	int err = 0;

	err = copy_from_user(&req, argp, sizeof(req));
	if (err)
		return -EFAULT;

	/* Serialize all operations at the device level */
	mutex_lock(&uscmi->dev_lock);

	switch (cmd) {
	case SCMI_IOCTL_PRF:
		err = do_performance_operation(&req, uscmi, cl);
		break;

	case SCMI_IOCTL_RST:
		err = do_reset_operation(&req, uscmi);
		break;

	case SCMI_IOCTL_PWR:
		err = do_power_operation(&req, uscmi, cl);
		break;

	default:
		err = -ENOTTY;
		break;
	}

	mutex_unlock(&uscmi->dev_lock);
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
 * uscmi_stats_show - Show driver statistics in debugfs
 * @s: Sequence file
 * @unused: Unused
 *
 * This function displays comprehensive statistics about the driver,
 * including operation counts and client details.
 *
 * Return: 0 on success
 */
static int uscmi_stats_show(struct seq_file *s, void *unused)
{
	struct qcom_uscmi_dev *uscmi = s->private;
	struct qcom_uscmi_client *cl;
	int i;

	/* Read atomic values once to avoid repeated atomic operations */
	long long power_on_count = atomic64_read(&uscmi->stats.power_on_count);
	long long power_off_count =
		atomic64_read(&uscmi->stats.power_off_count);
	long long perf_set_count = atomic64_read(&uscmi->stats.perf_set_count);
	long long rst_assert_count =
		atomic64_read(&uscmi->stats.rst_assert_count);
	long long rst_deassert_count =
		atomic64_read(&uscmi->stats.rst_deassert_count);
	long long rst_reset_count =
		atomic64_read(&uscmi->stats.rst_reset_count);
	long long error_count = atomic64_read(&uscmi->stats.error_count);
	int client_count = atomic_read(&uscmi->stats.client_count);
	int max_clients = atomic_read(&uscmi->stats.max_clients);

	seq_puts(s, "USCMI Driver Statistics:\n");
	seq_printf(s, "  Power-on operations: %lld\n", power_on_count);
	seq_printf(s, "  Power-off operations: %lld\n", power_off_count);
	seq_printf(s, "  Performance set operations: %lld\n", perf_set_count);
	seq_printf(s, "  Reset assert operations: %lld\n", rst_assert_count);
	seq_printf(s, "  Reset deassert operations: %lld\n",
		   rst_deassert_count);
	seq_printf(s, "  Reset operations: %lld\n", rst_reset_count);
	seq_printf(s, "  Error count: %lld\n", error_count);
	seq_printf(s, "  Current clients: %d\n", client_count);
	seq_printf(s, "  Maximum clients: %d\n", max_clients);
	seq_printf(s, "  Client limit: %u\n", READ_ONCE(uscmi->client_limit));

	seq_puts(s, "\nDomain Status:\n");
	/* Try to acquire lock, skip if busy to avoid deadlock */
	if (!mutex_trylock(&uscmi->dev_lock)) {
		seq_puts(s, "  (Device busy, domain status unavailable)\n");
		return 0;
	}
	for (i = 0; i < uscmi->domain_count; i++) {
		const char *name =
			uscmi->domain_names ? uscmi->domain_names[i] : NULL;
		int active_count = 0;
		int agg_perf_level = -1;

		/* Calculate active clients and aggregated perf level */
		list_for_each_entry(cl, &uscmi->clients, node) {
			if (cl->pwr_votes && atomic_read(&cl->pwr_votes[i]) > 0)
				active_count++;

			/* Find max performance level */
			if (cl->prf_votes && cl->prf_votes[i] >= 0 &&
			    cl->prf_votes[i] > agg_perf_level)
				agg_perf_level = cl->prf_votes[i];
		}

		seq_printf(s, "  Domain %d (%s):\n", i,
			   name ? name : "unnamed");
		seq_printf(s, "    Active clients: %d\n", active_count);
		seq_printf(s, "    Aggregated perf level: %d\n",
			   agg_perf_level);
	}

	seq_puts(s, "\nActive Clients:\n");
	list_for_each_entry(cl, &uscmi->clients, node) {
		seq_printf(s, "  Client %s (PID: %d):\n", cl->comm, cl->pid);

		if (cl->pwr_votes && cl->prf_votes) {
			for (i = 0; i < uscmi->domain_count; i++) {
				seq_printf(s,
					   "    Domain %d: Power votes=%d, Perf level=%d\n",
					   i, atomic_read(&cl->pwr_votes[i]),
					   cl->prf_votes[i]);
			}
		}
	}
	mutex_unlock(&uscmi->dev_lock);

	return 0;
}

static int uscmi_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, uscmi_stats_show, inode->i_private);
}

static const struct file_operations uscmi_stats_fops = {
	.open = uscmi_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Global debugfs root directory for all USCMI devices */
static struct dentry *qcom_uscmi_root;

/**
 * uscmi_debugfs_init - Initialize debugfs interface
 * @uscmi: Driver structure
 *
 * This function creates the debugfs directory and files for the driver.
 * It provides detailed statistics and configuration options.
 * Also initializes statistics counters, default configuration, and caches
 * domain names from device tree for debugging purposes.
 */
static void uscmi_debugfs_init(struct qcom_uscmi_dev *uscmi)
{
	struct device *dev = uscmi->dev;
	struct device_node *np = dev->of_node;
	struct dentry *stats_file, *dir;
	int i, num_pd_names;

	/* Set default configuration (stats already zero-initialized by kzalloc)
	 */
	uscmi->client_limit = DEFAULT_CLIENT_LIMIT;

	/* Cache domain names if available (for debugging) */
	if (uscmi->domain_count) {
		num_pd_names =
			of_property_count_strings(np, "power-domain-names");
		if (num_pd_names > 0 && num_pd_names == uscmi->domain_count) {
			uscmi->domain_names = devm_kcalloc(
				dev, uscmi->domain_count,
				sizeof(*uscmi->domain_names), GFP_KERNEL);
			if (uscmi->domain_names) {
				for (i = 0; i < uscmi->domain_count; i++) {
					of_property_read_string_index(
						np, "power-domain-names", i,
						&uscmi->domain_names[i]);
				}
			}
		}
	}

	/* Create device-specific subdirectory under global root */
	if (!qcom_uscmi_root) {
		dev_warn(dev, "Debugfs root not available\n");
		return;
	}

	uscmi->debugfs_dir = debugfs_create_dir(uscmi->name, qcom_uscmi_root);

	if (IS_ERR_OR_NULL(uscmi->debugfs_dir)) {
		uscmi->debugfs_dir = NULL;
		return;
	}

	/* Create stats file */
	stats_file = debugfs_create_file("stats", 0444, uscmi->debugfs_dir,
					 uscmi, &uscmi_stats_fops);
	if (IS_ERR_OR_NULL(stats_file))
		dev_warn(uscmi->dev, "Failed to create debugfs stats file\n");

	/* Create config directory */
	dir = debugfs_create_dir("config", uscmi->debugfs_dir);
	if (!IS_ERR_OR_NULL(dir)) {
		/*
		 * Add configuration options.
		 * Note: client_limit will be validated and clamped in
		 * qcom_uscmi_open() to prevent setting it to unsafe values (0
		 * or > 10000).
		 */
		debugfs_create_u32("client_limit", 0644, dir,
				   &uscmi->client_limit);
	}
}

static void uscmi_debugfs_exit(struct qcom_uscmi_dev *uscmi)
{
	debugfs_remove_recursive(uscmi->debugfs_dir);
	uscmi->debugfs_dir = NULL;
}

/**
 * uscmi_setup_power_domains - Attach and configure power domains
 * @uscmi: USCMI device structure
 *
 * Handles power domain attachment for both single and multiple domain cases.
 *
 * Return: 0 on success, negative error code on failure
 */
static int uscmi_setup_power_domains(struct qcom_uscmi_dev *uscmi)
{
	struct device *dev = uscmi->dev;
	int err;

	/* Get domain count from device tree */
	uscmi->domain_count = of_count_phandle_with_args(
		dev->of_node, "power-domains", "#power-domain-cells");
	if (uscmi->domain_count < 0)
		uscmi->domain_count = 0;

	/* Check if on-demand attachment is requested */
	uscmi->pm_domains_on_demand =
		of_property_read_bool(dev->of_node, "qcom,no-suspend");

	/* Attach domains now if not on-demand */
	if (!uscmi->pm_domains_on_demand && uscmi->domain_count > 1) {
		err = dev_pm_domain_attach_list(dev, NULL, &uscmi->pd_list);
		if (err < 0) {
			dev_err(dev, "multi domain attach failed(ret=%d)\n",
				err);
			return err;
		}
		uscmi->domains_attached = true;
		dev_info(dev, "Attached %d power domain(s)\n",
			 uscmi->domain_count);
	} else if (uscmi->pm_domains_on_demand) {
		uscmi->domains_attached = false;
		dev_info(
			dev,
			"On-demand mode: %d domain(s) will be attached later\n",
			uscmi->domain_count);
	}

	return 0;
}

/**
 * uscmi_setup_device_name - Setup device name from DT or node name
 * @uscmi: USCMI device structure
 *
 * Reads device name from "qcom,dev-name" property or uses node name.
 *
 * Return: 0 on success, negative error code on failure
 */
static int uscmi_setup_device_name(struct qcom_uscmi_dev *uscmi)
{
	struct device *dev = uscmi->dev;
	struct device_node *np = dev->of_node;
	const char *name;

	if (!of_property_read_string(np, "qcom,dev-name", &name))
		uscmi->name = devm_kstrdup(dev, name, GFP_KERNEL);
	else
		uscmi->name = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);

	if (!uscmi->name)
		return -ENOMEM;

	return 0;
}

/**
 * uscmi_register_miscdev - Register miscellaneous device
 * @uscmi: USCMI device structure
 *
 * Registers the miscdevice and sets up runtime PM if needed.
 *
 * Return: 0 on success, negative error code on failure
 */
static int uscmi_register_miscdev(struct qcom_uscmi_dev *uscmi)
{
	struct device *dev = uscmi->dev;
	int err;

	uscmi->miscdev.minor = MISC_DYNAMIC_MINOR;
	uscmi->miscdev.name = uscmi->name;
	uscmi->miscdev.fops = &qcom_uscmi_fops;
	uscmi->miscdev.parent = dev;

	err = misc_register(&uscmi->miscdev);
	if (err) {
		dev_err(dev, "%s: failed: %d\n", __func__, err);
		return err;
	}

	/* Only set up runtime PM if we have power domains */
	if (dev->pm_domain) {
		err = devm_pm_runtime_set_active_enabled(dev);
		if (err) {
			misc_deregister(&uscmi->miscdev);
			return err;
		}
	}
	pm_runtime_forbid(dev);
	pm_runtime_put_noidle(dev);

	return 0;
}

/**
 * uscmi_init_reset_controls - Initialize reset controls
 * @uscmi: USCMI device structure
 *
 * Gets all reset controls from device tree and stores them for later use.
 *
 * Return: 0 on success, negative error code on failure
 */
static int uscmi_init_reset_controls(struct qcom_uscmi_dev *uscmi)
{
	struct device *dev = uscmi->dev;
	struct device_node *np = dev->of_node;
	int i, num_resets, ret;

	num_resets = of_property_count_strings(np, "reset-names");
	if (num_resets <= 0)
		return 0;

	uscmi->reset_count = num_resets;

	/* Allocate arrays for reset controls and names */
	uscmi->reset_names = devm_kcalloc(
		dev, num_resets, sizeof(*uscmi->reset_names), GFP_KERNEL);
	if (!uscmi->reset_names)
		return -ENOMEM;

	uscmi->resets = devm_kcalloc(dev, num_resets, sizeof(*uscmi->resets),
				     GFP_KERNEL);
	if (!uscmi->resets)
		return -ENOMEM;

	/* Get all reset controls */
	for (i = 0; i < num_resets; i++) {
		const char *name;

		ret = of_property_read_string_index(np, "reset-names", i,
						    &name);
		if (ret) {
			dev_err(dev, "Failed to read reset-names[%d]: %d\n", i,
				ret);
			return ret;
		}

		uscmi->reset_names[i] = devm_kstrdup(dev, name, GFP_KERNEL);
		if (!uscmi->reset_names[i])
			return -ENOMEM;

		uscmi->resets[i] = of_reset_control_get(np, name);
		if (IS_ERR(uscmi->resets[i])) {
			ret = PTR_ERR(uscmi->resets[i]);
			if (ret == -ENOENT) {
				uscmi->resets[i] = NULL;
				dev_info(dev,
					 "Reset control %s not available (optional)\n",
					 name);
			} else {
				dev_err(dev,
					"Failed to get reset control %s: %d\n",
					name, ret);
				return ret;
			}
		}

		dev_dbg(dev, "Acquired reset control: %s\n", name);
	}

	return 0;
}

/**
 * qcom_uscmi_probe - Probe function for USCMI driver
 * @pdev: Platform device
 *
 * Main probe function that orchestrates device initialization by calling
 * helper functions for each major initialization step.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qcom_uscmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_uscmi_dev *uscmi;
	int err;

	/* Allocate main device structure - use kzalloc, not devm */
	uscmi = devm_kzalloc(dev, sizeof(*uscmi), GFP_KERNEL);
	if (!uscmi)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate uscmi dev\n");

	/* Initialize device structure */
	uscmi->dev = dev;
	mutex_init(&uscmi->dev_lock);
	INIT_LIST_HEAD(&uscmi->clients);

	/* Setup device name */
	err = uscmi_setup_device_name(uscmi);
	if (err)
		return dev_err_probe(dev, err, "device_name setup failed\n");

	err = uscmi_init_reset_controls(uscmi);
	if (err)
		return dev_err_probe(dev, err, "reset setup failed\n");

	/* Setup power domains */
	err = uscmi_setup_power_domains(uscmi);
	if (err)
		return dev_err_probe(dev, err, "power setup failed\n");

	/* Register miscdevice and setup PM */
	err = uscmi_register_miscdev(uscmi);
	if (err)
		return dev_err_probe(dev, err, "device registration failed\n");

	/* Initialize debugfs */
	uscmi_debugfs_init(uscmi);

	/* Save driver data */
	platform_set_drvdata(pdev, uscmi);

	/* Log successful initialization */
	dev_info(dev,
		 "/dev/%s node created with %d power domain(s) and %d reset domain(s)\n",
		 uscmi->name, uscmi->domain_count, uscmi->reset_count);
	return 0;
}

static int qcom_uscmi_remove(struct platform_device *pdev)
{
	struct qcom_uscmi_dev *uscmi = platform_get_drvdata(pdev);
	int i;

	if (!uscmi)
		return 0;

	/* Remove debugfs entries first */
	uscmi_debugfs_exit(uscmi);

	/*
	 * Unregister miscdevice to prevent new open() calls.
	 * suppress_bind_attrs = true guarantees no open fds exist at
	 * this point, so it is safe to free all resources immediately.
	 */
	misc_deregister(&uscmi->miscdev);

	dev_info(uscmi->dev, "/dev/%s node removed\n", uscmi->name);

	/* Detach power domains if still attached */
	if (uscmi->pd_list)
		dev_pm_domain_detach_list(uscmi->pd_list);

	/* Release reset controls */
	if (uscmi->resets) {
		for (i = 0; i < uscmi->reset_count; i++) {
			if (uscmi->resets[i])
				reset_control_put(uscmi->resets[i]);
		}
	}

	mutex_destroy(&uscmi->dev_lock);
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
	{
		.compatible = "qcom,uscmi",
	},
	{},
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
		.suppress_bind_attrs = true,
	},
	.probe = qcom_uscmi_probe,
	.remove = qcom_uscmi_remove,
};

static int __init qcom_uscmi_init(void)
{
	/* Create global debugfs root directory */
	qcom_uscmi_root = debugfs_create_dir("qcom_uscmi", NULL);
	if (IS_ERR_OR_NULL(qcom_uscmi_root)) {
		pr_warn("qcom_uscmi: Failed to create debugfs root directory\n");
		qcom_uscmi_root = NULL;
	}

	return platform_driver_register(&qcom_uscmi_driver);
}
module_init(qcom_uscmi_init);

static void __exit qcom_uscmi_exit(void)
{
	/* Remove global debugfs root directory */
	debugfs_remove_recursive(qcom_uscmi_root);

	platform_driver_unregister(&qcom_uscmi_driver);
}
module_exit(qcom_uscmi_exit);

MODULE_DESCRIPTION("Qualcomm userspace scmi interface driver");
MODULE_LICENSE("GPL v2");

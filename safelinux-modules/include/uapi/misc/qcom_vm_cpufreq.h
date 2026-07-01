/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#ifndef _QCOM_VM_CPUFREQ_H_
#define _QCOM_VM_CPUFREQ_H_

#include <linux/types.h>
#include <linux/ioctl.h>

/* CPU performance level control IOCTLs */
struct cpu_perf_level_req {
	__u32 level;  /* Performance level */
};

/* Structure for querying available performance levels */
#define MAX_PERF_LEVELS 32
struct cpu_perf_levels_available {
	__u32 num_levels;                  /* Number of available levels */
	__u32 levels[MAX_PERF_LEVELS];     /* Array of available perf level indices */
	__u32 freq_khz[MAX_PERF_LEVELS];   /* Frequency in kHz for each level */
};

/* Structure for querying transition latency */
struct cpu_perf_transition_latency {
	__u32 latency_us;  /* Transition latency in microseconds */
};

#define VM_CPUFREQ_IOC_MAGIC 0xBB

/*
 * Common return codes (in addition to per-ioctl errors below):
 *   -ENODEV  driver torn down or initialization not yet complete
 *   -EFAULT  argp pointer not accessible from userspace
 *   -EINVAL  argument size mismatch with the ioctl definition
 *   -ENOTTY  unknown command for this device
 */

/*
 * CPU_PERF_LEVEL_SET — request a performance level by index into the table
 * returned by CPU_PERF_LEVELS_GET_AVAILABLE.
 *
 * Additional return codes:
 *   -EINVAL      level index out of range, or selected level maps to 0 kHz
 *   -EAGAIN      rate-limited: the rate_limit module parameter is enabled
 *                and the previous successful SCMI freq_set was within
 *                transition_latency_us. Userspace should retry after that
 *                interval (queryable via CPU_PERF_TRANSITION_LATENCY_GET).
 *   -ERANGE      computed Hz value would overflow unsigned long
 *                (theoretical; unreachable with current OPP-derived inputs).
 *   -EOPNOTSUPP  SCMI server does not implement freq_set.
 */
#define CPU_PERF_LEVEL_SET \
	_IOW(VM_CPUFREQ_IOC_MAGIC, 1, struct cpu_perf_level_req)

/*
 * CPU_PERF_LEVEL_GET — read firmware's current performance level, returned
 * as an index into the CPU_PERF_LEVELS_GET_AVAILABLE table.
 *
 * Additional return codes:
 *   -EINVAL      firmware reported a frequency not present in the
 *                advertised OPP table (firmware/SCMI inconsistency)
 *   -EOPNOTSUPP  SCMI server does not implement freq_get
 */
#define CPU_PERF_LEVEL_GET \
	_IOR(VM_CPUFREQ_IOC_MAGIC, 2, struct cpu_perf_level_req)

/*
 * CPU_PERF_LEVELS_GET_AVAILABLE — fetch the per-domain table of
 * (level index, frequency in kHz) pairs. levels[i] is always i; freq_khz[i]
 * is the kHz value firmware advertises for that level. Only the first
 * num_levels entries are valid.
 */

#define CPU_PERF_LEVELS_GET_AVAILABLE \
	_IOR(VM_CPUFREQ_IOC_MAGIC, 3, struct cpu_perf_levels_available)

/*
 * CPU_PERF_TRANSITION_LATENCY_GET — read the per-domain transition latency
 * in microseconds. This is the minimum interval rate_limit (when enabled)
 * enforces between successful CPU_PERF_LEVEL_SET calls.
 */
#define CPU_PERF_TRANSITION_LATENCY_GET \
	_IOR(VM_CPUFREQ_IOC_MAGIC, 4, struct cpu_perf_transition_latency)

#endif /* _QCOM_VM_CPUFREQ_H_ */

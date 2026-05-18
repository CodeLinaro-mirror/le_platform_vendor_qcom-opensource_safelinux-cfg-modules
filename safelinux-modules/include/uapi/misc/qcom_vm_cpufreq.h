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
	__u32 num_levels;               /* Number of available levels */
	__u32 levels[MAX_PERF_LEVELS];  /* Array of Available perf levels */
};

#define VM_CPUFREQ_IOC_MAGIC 0xBB
#define CPU_PERF_LEVEL_SET \
	_IOW(VM_CPUFREQ_IOC_MAGIC, 1, struct cpu_perf_level_req)
#define CPU_PERF_LEVEL_GET \
	_IOWR(VM_CPUFREQ_IOC_MAGIC, 2, struct cpu_perf_level_req)
#define CPU_PERF_LEVELS_GET_AVAILABLE \
	_IOR(VM_CPUFREQ_IOC_MAGIC, 3, struct cpu_perf_levels_available)

#endif /* _QCOM_VM_CPUFREQ_H_ */

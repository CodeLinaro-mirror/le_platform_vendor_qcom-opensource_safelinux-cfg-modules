/* SPDX-License-Identifier: GPL-2.0-only */
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#undef TRACE_SYSTEM
#define TRACE_SYSTEM qcom_vm_cpufreq

#if !defined(_TRACE_QCOM_VM_CPUFREQ_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_QCOM_VM_CPUFREQ_H

#include <linux/tracepoint.h>

TRACE_EVENT(vm_cpufreq_level_set,
	TP_PROTO(int domain, u32 level, int ret, s64 duration_us),
	TP_ARGS(domain, level, ret, duration_us),

	TP_STRUCT__entry(
		__field(int, domain)
		__field(u32, level)
		__field(int, ret)
		__field(s64, duration_us)
	),

	TP_fast_assign(
		__entry->domain = domain;
		__entry->level = level;
		__entry->ret = ret;
		__entry->duration_us = duration_us;
	),

	TP_printk("domain=%d level=%u ret=%d duration=%lld us",
		  __entry->domain, __entry->level, __entry->ret, __entry->duration_us)
);

TRACE_EVENT(vm_cpufreq_level_get,
	TP_PROTO(int domain, u32 level, int ret, s64 duration_us),
	TP_ARGS(domain, level, ret, duration_us),

	TP_STRUCT__entry(
		__field(int, domain)
		__field(u32, level)
		__field(int, ret)
		__field(s64, duration_us)
	),

	TP_fast_assign(
		__entry->domain = domain;
		__entry->level = level;
		__entry->ret = ret;
		__entry->duration_us = duration_us;
	),

	TP_printk("domain=%d level=%u ret=%d duration=%lld us",
		  __entry->domain, __entry->level, __entry->ret, __entry->duration_us)
);

#endif /* _TRACE_QCOM_VM_CPUFREQ_H */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE qcom_vm_cpufreq_trace

/* This part must be outside protection */
#include <trace/define_trace.h>

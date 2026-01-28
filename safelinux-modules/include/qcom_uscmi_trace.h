/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM qcom_uscmi

#if !defined(_QCOM_USCMI_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _QCOM_USCMI_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(qcom_uscmi_power_op,
	TP_PROTO(const char *name, int operation, int ret, u64 duration_us),
	TP_ARGS(name, operation, ret, duration_us),

	TP_STRUCT__entry(
		__string(domain_name, name)
		__field(int, operation)
		__field(int, ret)
		__field(u64, duration_us)
	),

	TP_fast_assign(
		__assign_str(domain_name, name);
		__entry->operation = operation;
		__entry->ret = ret;
		__entry->duration_us = duration_us;
	),

	TP_printk("domain=%s op=%d ret=%d duration=%llu us",
		__get_str(domain_name), __entry->operation, __entry->ret,
		__entry->duration_us)
);

TRACE_EVENT(qcom_uscmi_perf_op,
	TP_PROTO(const char *name, unsigned int level, int ret, u64 duration_us),
	TP_ARGS(name, level, ret, duration_us),

	TP_STRUCT__entry(
		__string(domain_name, name)
		__field(unsigned int, level)
		__field(int, ret)
		__field(u64, duration_us)
	),

	TP_fast_assign(
		__assign_str(domain_name, name);
		__entry->level = level;
		__entry->ret = ret;
		__entry->duration_us = duration_us;
	),

	TP_printk("domain=%s level=%u ret=%d duration=%llu us",
		__get_str(domain_name), __entry->level, __entry->ret,
		__entry->duration_us)
);

TRACE_EVENT(qcom_uscmi_reset_op,
	TP_PROTO(const char *name, int operation, int ret, u64 duration_us),
	TP_ARGS(name, operation, ret, duration_us),

	TP_STRUCT__entry(
		__string(reset_name, name)
		__field(int, operation)
		__field(int, ret)
		__field(u64, duration_us)
	),

	TP_fast_assign(
		__assign_str(reset_name, name);
		__entry->operation = operation;
		__entry->ret = ret;
		__entry->duration_us = duration_us;
	),

	TP_printk("reset=%s op=%d ret=%d duration=%llu us",
		__get_str(reset_name), __entry->operation, __entry->ret,
		__entry->duration_us)
);

TRACE_EVENT(qcom_uscmi_pd_attach,
	TP_PROTO(const char *dev_name, int num_pds, int ret, u64 duration_us),
	TP_ARGS(dev_name, num_pds, ret, duration_us),

	TP_STRUCT__entry(
		__string(device, dev_name)
		__field(int, num_pds)
		__field(int, ret)
		__field(u64, duration_us)
	),

	TP_fast_assign(
		__assign_str(device, dev_name);
		__entry->num_pds = num_pds;
		__entry->ret = ret;
		__entry->duration_us = duration_us;
	),

	TP_printk("device=%s num_pds=%d ret=%d duration=%llu us",
		__get_str(device), __entry->num_pds, __entry->ret,
		__entry->duration_us)
);

TRACE_EVENT(qcom_uscmi_pd_detach,
	TP_PROTO(const char *dev_name, int operation, bool force, u64 duration_us),
	TP_ARGS(dev_name, operation, force, duration_us),

	TP_STRUCT__entry(
		__string(device, dev_name)
		__field(int, operation)
		__field(bool, force)
		__field(u64, duration_us)
	),

	TP_fast_assign(
		__assign_str(device, dev_name);
		__entry->operation = operation;
		__entry->force = force;
		__entry->duration_us = duration_us;
	),

	TP_printk("device=%s op=%d force=%d duration=%llu us",
		__get_str(device), __entry->operation, __entry->force,
		__entry->duration_us)
);

TRACE_EVENT(qcom_uscmi_genpd_state,
	TP_PROTO(const char *domain, bool is_on, bool always_on),
	TP_ARGS(domain, is_on, always_on),

	TP_STRUCT__entry(
		__string(domain_name, domain)
		__field(bool, is_on)
		__field(bool, always_on)
	),

	TP_fast_assign(
		__assign_str(domain_name, domain);
		__entry->is_on = is_on;
		__entry->always_on = always_on;
	),

	TP_printk("domain=%s is_on=%d always_on=%d",
		__get_str(domain_name), __entry->is_on, __entry->always_on)
);

#endif /* _QCOM_USCMI_TRACE_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE qcom_uscmi_trace
#include <trace/define_trace.h>

/* SPDX-License-Identifier: GPL-2.0-only*/
/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM vendor_uscmi

#if !defined(TRACE_VENDOR_USCMI) || defined(TRACE_HEADER_MULTI_READ)
#define TRACE_VENDOR_USCMI
#include <linux/tracepoint.h>

TRACE_EVENT(vendor_uscmi_ioctl,

	TP_PROTO(int cmd, __u64 size, int ret, __u64 algo, int param_id),

	TP_ARGS(cmd, size, ret, algo, param_id),

	TP_STRUCT__entry(
		__field(int, cmd)
		__field(__u64, size)
		__field(int, ret)
		__field(__u64, algo)
		__field(int, param_id)
	),

	TP_fast_assign(
		__entry->cmd = cmd;
		__entry->size = size;
		__entry->ret = ret;
		__entry->algo = algo;
		__entry->param_id = param_id;
	),

	TP_printk("cmd=%d, size=%ul, ret=%d algo:%ul param_id:%d", __entry->cmd,
		  __entry->size, __entry->ret, __entry->algo, __entry->param_id)
);

#endif /* _TRACE_VENDOR_USCMI */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE vendor_uscmi_trace

#include <trace/define_trace.h>


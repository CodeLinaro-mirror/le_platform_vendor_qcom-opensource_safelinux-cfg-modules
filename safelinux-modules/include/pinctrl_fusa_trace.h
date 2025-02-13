/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM tlmm_hw_safety

#if !defined(_TRACE_TLMM_HW_SAFETY_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TLMM_HW_SAFETY_H

#include <linux/tracepoint.h>

TRACE_EVENT(tlmm_hw_safety_event,
	TP_PROTO(const char *interrupt_type, u32 count),
	TP_ARGS(interrupt_type, count),
	TP_STRUCT__entry(
		__string(interrupt_type, interrupt_type)
		__field(u32, count)
	),
	TP_fast_assign(
		__assign_str(interrupt_type, interrupt_type);
		__entry->count = count;
	),
	TP_printk("Pinctrl FUSA Error: %s, count: %u", __get_str(interrupt_type), __entry->count)
);

#endif /* _TRACE_TLMM_HW_SAFETY_H */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE pinctrl_fusa_trace
#include <trace/define_trace.h>

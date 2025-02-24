/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM smmu

#if !defined(_TRACE_ARM_SMMU_QCOM_FUSA_H) | defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_ARM_SMMU_QCOM_FUSA_H

#include <linux/tracepoint.h>
#include <linux/aer.h>

#define FUSA_FATAL	0x1
/*
 * QCOM SMMU Trace event
 *
 * These events are generated when QCOM SMMU hardware detects a corrected
 * or uncorrected warning/error on TCU/TBU client. The event report has
 * the following structure:
 *
 * char * dev_name -	The name of the TCU/TBU
 * u32 status -		Either the correctable or uncorrectable register
 *			indicating what error or errors have been seen
 * u8 severity -	error severity 0:NONFATAL 1:FATAL
 */

TRACE_EVENT(smmu_hwirq,
	TP_PROTO(const char *dev_name,
		 const char *fault_source,
		 const u32 fault_code,
		 const u8 severity),

	TP_ARGS(dev_name, fault_source, fault_code, severity),

	TP_STRUCT__entry(
		__string(dev_name, dev_name)
		__string(fault_source, fault_source)
		__field(u32, fault_code)
		__field(u8, severity)
	),

	TP_fast_assign(
		__assign_str(dev_name, dev_name);
		__assign_str(fault_source, fault_source);
		__entry->fault_code	= fault_code;
		__entry->severity	= severity;
	),

	TP_printk("%s SMMU ERROR: fault_source=%s fault_code=0x%x severity=%s\n",
			__get_str(dev_name),
			__get_str(fault_source),
			__entry->fault_code,
			__entry->severity == FUSA_FATAL ?
				"Fatal, uncorrectable" : "Non-fatal, correctable")
);
#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE arm-smmu-qcom-fusa
#include <trace/define_trace.h>

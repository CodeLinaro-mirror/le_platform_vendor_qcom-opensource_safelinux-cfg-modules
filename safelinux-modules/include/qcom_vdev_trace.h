/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM qcom_vdev

#if !defined(_QCOM_VDEV_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _QCOM_VDEV_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(qcom_vdev_register_eventfd,
	TP_PROTO(const char *name, int irq, int eventfd, u32 irq_index),
	TP_ARGS(name, irq, eventfd, irq_index),

	TP_STRUCT__entry(
		__string(dev_name, name)
		__field(int, irq)
		__field(int, eventfd)
		__field(u32, irq_index)
	),

	TP_fast_assign(
		__assign_str(dev_name, name);
		__entry->irq = irq;
		__entry->eventfd = eventfd;
		__entry->irq_index = irq_index;
	),

	TP_printk("device name=%s irq=%d eventfd=%d irq_index=%d",
		__get_str(dev_name), __entry->irq, __entry->eventfd,
		__entry->irq_index)
);

TRACE_EVENT(qcom_vdev_mask_interrupt,
	TP_PROTO(const char *name, u32 irq_index, int irq, u32 flags, bool mask),
	TP_ARGS(name, irq_index, irq, flags, mask),

	TP_STRUCT__entry(
		__string(dev_name, name)
		__field(u32, irq_index)
		__field(int, irq)
		__field(u32, flags)
		__field(bool, mask)
	),

	TP_fast_assign(
		__assign_str(dev_name, name);
		__entry->irq_index = irq_index;
		__entry->irq = irq;
		__entry->flags = flags;
		__entry->mask = mask;
	),

	TP_printk("device name=%s irq_index=%d irq=%d flags=%d mask=%d",
		__get_str(dev_name), __entry->irq_index, __entry->irq, __entry->flags,
		__entry->mask)
);

TRACE_EVENT(qcom_vdev_unmask_interrupt,
	TP_PROTO(const char *name, u32 irq_index, int irq, u32 flags, bool mask),
	TP_ARGS(name, irq_index, irq, flags, mask),

	TP_STRUCT__entry(
		__string(dev_name, name)
		__field(u32, irq_index)
		__field(int, irq)
		__field(u32, flags)
		__field(bool, mask)
	),

	TP_fast_assign(
		__assign_str(dev_name, name);
		__entry->irq_index = irq_index;
		__entry->irq = irq;
		__entry->flags = flags;
		__entry->mask = mask;
	),

	TP_printk("device name=%s irq_index=%d irq=%d flags=%d mask=%d",
		__get_str(dev_name), __entry->irq_index, __entry->irq, __entry->flags,
		__entry->mask)
);

TRACE_EVENT(qcom_vdev_automasked_irq_handler,
	TP_PROTO(const char *name, int hwirq, int irq, int ret),
	TP_ARGS(name, hwirq, irq, ret),

	TP_STRUCT__entry(
		__string(dev_name, name)
		__field(int, hwirq)
		__field(int, irq)
		__field(int, ret)
	),

	TP_fast_assign(
		__assign_str(dev_name, name);
		__entry->hwirq = hwirq;
		__entry->irq = irq;
		__entry->ret = ret;
	),

	TP_printk("device name=%s hwirq=%d irq=%d ret=%d", __get_str(dev_name),
		 __entry->hwirq, __entry->irq, __entry->ret)
);

TRACE_EVENT(qcom_vdev_irq_handler,
	TP_PROTO(const char *name, int hwirq, int irq),
	TP_ARGS(name, hwirq, irq),

	TP_STRUCT__entry(
		__string(dev_name, name)
		__field(int, hwirq)
		__field(int, irq)
	),

	TP_fast_assign(
		__assign_str(dev_name, name);
		__entry->hwirq = hwirq;
		__entry->irq = irq;
	),

	TP_printk("device name=%s hwirq=%d irq=%d", __get_str(dev_name),
		 __entry->hwirq, __entry->irq)
);

TRACE_EVENT(qcom_vdev_mem_regions_init,
	TP_PROTO(const char *dev_name, int rindex, u64 addr, u32 size),
	TP_ARGS(dev_name, rindex, addr, size),

	TP_STRUCT__entry(
		__string(device, dev_name)
		__field(int, rindex)
		__field(u64, addr)
		__field(u32, size)
	),

	TP_fast_assign(
		__assign_str(device, dev_name);
		__entry->rindex = rindex;
		__entry->addr = addr;
		__entry->size = size;
	),

	TP_printk("device=%s rindex=%d addr=0x%llx size=%d",
		__get_str(device), __entry->rindex, __entry->addr,
		__entry->size)
);

TRACE_EVENT(qcom_vdev_regions_init,
	TP_PROTO(const char *dev_name, int rindex, u64 start, u32 size),
	TP_ARGS(dev_name, rindex, start, size),

	TP_STRUCT__entry(
		__string(device, dev_name)
		__field(int, rindex)
		__field(u64, start)
		__field(u32, size)
	),

	TP_fast_assign(
		__assign_str(device, dev_name);
		__entry->rindex = rindex;
		__entry->start = start;
		__entry->size = size;
	),

	TP_printk("device=%s rindex=%d start=0x%llx size=%d",
		__get_str(device), __entry->rindex, __entry->start,
		__entry->size)
);

#endif /* _QCOM_VDEV_TRACE_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE qcom_vdev_trace
#include <trace/define_trace.h>

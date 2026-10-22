/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _QCOM_SYSTEM_HEAP_H
#define _QCOM_SYSTEM_HEAP_H

#include <linux/dma-heap.h>
#include <linux/err.h>
#include "qcom_sg_ops.h"
#include "qcom_dynamic_page_pool.h"

struct qcom_system_heap {
	int uncached;
	struct dynamic_page_pool **pool_list;
};

void qcom_system_heap_create(const char *name, const char *system_alias, bool uncached);
void qcom_system_heap_free(struct qcom_sg_buffer *buffer);
struct page *qcom_sys_heap_alloc_largest_available(struct dynamic_page_pool **pools,
						   unsigned long size,
						   unsigned int max_order,
						   bool movable);
int system_qcom_sg_buffer_alloc(struct dma_heap *heap,
				struct qcom_sg_buffer *buffer,
				unsigned long len,
				bool movable);

struct dynamic_page_pool **qcom_system_heap_get_pools(void);
struct device *qcom_system_heap_get_dev(void);

#endif /* _QCOM_SYSTEM_HEAP_H */

/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/iommu.h>
#include <linux/dma-mapping.h>
#include <linux/iova.h>


struct device_node *qcom_iommu_group_parse_phandle(struct device *dev);

int qcom_iommu_generate_dma_regions(struct device *dev,
			    struct list_head *head);

void qcom_iommu_generate_resv_regions(struct device *dev,
			struct list_head *list);

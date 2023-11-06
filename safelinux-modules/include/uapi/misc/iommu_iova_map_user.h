/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IOMMU_IOVA_MAP_USER_H__
#define __IOMMU_IOVA_MAP_USER_H__

#include <linux/types.h>


#define KIUMD_SMMU_MAP_BUF	 _IOWR('R', 14, struct vfio_device_iommu)

struct vfio_device_iommu{
	int *dev;
	int *group;
};
#endif

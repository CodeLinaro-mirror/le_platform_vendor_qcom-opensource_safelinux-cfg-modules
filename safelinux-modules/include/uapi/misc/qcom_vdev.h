/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __QCOM_VDEV_H__
#define __QCOM_VDEV_H__

#include <linux/types.h>

struct vdev_device_info {
	__u32 argsz;
	__u32 flags;
	__u32 num_regions; /* Max region index + 1 */
	__u32 num_reserved_regions;
	__u32 num_irqs; /* Max IRQ index + 1 */
	__u32 pad;
};

struct vdev_region_info {
	__u32 argsz;
	__u32 flags;
	__u32 index; /* Region index */
	__u32 cap_offset; /* Offset within info struct of first cap */
	__aligned_u64 size; /* Region size (bytes) */
	__aligned_u64 offset; /* Region offset from start of device fd */
};

struct irq_user {
	__u32 irq_index;
	int event_fd;
};

#define VDEV_IOCTL_MAGIC 'V'
#define IOCTL_VDEV_GET_INFO _IOWR(VDEV_IOCTL_MAGIC, 1, struct vdev_device_info)
#define IOCTL_VDEV_GET_REGION_INFO _IOWR(VDEV_IOCTL_MAGIC, 2, struct vdev_region_info)
#define IOCTL_VDEV_REGISTER_EVENTFD _IOW(VDEV_IOCTL_MAGIC, 3, struct irq_user)
#define IOCTL_VDEV_UNMASK_INTERRUPT _IOW(VDEV_IOCTL_MAGIC, 4, struct irq_user)
#define IOCTL_VDEV_MASK_INTERRUPT _IOW(VDEV_IOCTL_MAGIC, 5, struct irq_user)

#endif /* __QCOM_VDEV_H__ */

/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef __VENDOR_USCMI_H__
#define __VENDOR_USCMI_H__

#include <linux/types.h>

typedef struct scmi_vendor_msg {
	__u32 param_id;
	__u32 tx_size;
	__u32 rx_size;
	void *msg;
} scmi_vendor_msg_t;

#define SCMI_VENDOR_IOCTL_MAGIC	   0xBA
#define SET_PARAM			_IOW(SCMI_VENDOR_IOCTL_MAGIC, 0, struct scmi_vendor_msg)
#define GET_PARAM			_IOWR(SCMI_VENDOR_IOCTL_MAGIC, 1, struct scmi_vendor_msg)
#define START_ACTIVITY		   _IOW(SCMI_VENDOR_IOCTL_MAGIC, 2, struct scmi_vendor_msg)
#define STOP_ACTIVITY		    _IOW(SCMI_VENDOR_IOCTL_MAGIC, 3, struct scmi_vendor_msg)

#endif /* __VENDOR_USCMI_H__ */


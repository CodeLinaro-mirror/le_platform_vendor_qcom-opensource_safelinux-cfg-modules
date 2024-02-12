/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef __QCOM_USCMI_H__
#define __QCOM_USCMI_H__

#include <linux/types.h>

#define NAME_LEN 32

typedef struct scmi_oper_ioctl {
	/* protocol used */
	__u32 proto;
	/* operation to be performed for the protocol */
	__u32 oper;
	/* level to be set(only valid for performance protocol */
	__u32 level;
	__u32 reserved;
	/* name can be the reset name or power domain name as mentioned in DT */
	char name[NAME_LEN];
} scmi_oper_ioctl_t;


#define SCMI_IOCTL_MAGIC 0xB9
#define SCMI_IOCTL_PRF  _IOWR(SCMI_IOCTL_MAGIC, 0, struct scmi_oper_ioctl)
#define SCMI_IOCTL_RST  _IOWR(SCMI_IOCTL_MAGIC, 1, struct scmi_oper_ioctl)
#define SCMI_IOCTL_PWR  _IOWR(SCMI_IOCTL_MAGIC, 2, struct scmi_oper_ioctl)

/* SCMI performance protocol operations */
typedef enum {
	SCMI_PRF_LVL_SET,
} scmi_prf_oper_t;

/* SCMI reset protocol operations */
typedef enum {
	SCMI_RST_ASSERT,
	SCMI_RST_DEASSERT,
	SCMI_RST_RESET,
} scmi_rst_oper_t;

typedef enum {
	SCMI_PWR_OFF,
	SCMI_PWR_ON,
} scmi_pwr_oper_t;

typedef enum {
	SCMI_PROTO_PERFORMANCE,
	SCMI_PROTO_RESET,
	SCMI_PROTO_POWER,
} scmi_proto_t;

#endif /* __QCOM_USCMI_H__ */


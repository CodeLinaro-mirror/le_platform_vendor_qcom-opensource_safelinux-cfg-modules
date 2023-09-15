/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef __QCOM_USCMI_H__
#define __QCOM_USCMI_H__

#include <linux/types.h>

#define RESET_ID_LEN 32

typedef struct scmi_oper_ioctl {
	__u32 proto;
	__u32 oper;
	__u32 level;
	__u32 reserved;
	char reset_id[RESET_ID_LEN];
} scmi_oper_ioctl_t;


#define SCMI_IOCTL_MAGIC 0xB9
#define SCMI_IOCTL_PRF  _IOWR(SCMI_IOCTL_MAGIC, 0, struct scmi_oper_ioctl)
#define SCMI_IOCTL_RST  _IOWR(SCMI_IOCTL_MAGIC, 1, struct scmi_oper_ioctl)

/* SCMI performance protocol operations */
typedef enum {
	SCMI_PRF_LVL_SET,
} scmi_prf_oper_t;

/* SCMI reset protocol operations */
typedef enum {
	SCMI_RST_ASSERT,
	SCMI_RST_DEASSERT,
	SCMI_RST_RESET,
}scmi_rst_oper_t;

typedef enum {
	SCMI_PROTO_PERFORMANCE,
	SCMI_PROTO_RESET,
} scmi_proto_t;

#endif /* __QCOM_USCMI_H__ */


/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 * Copyright (C) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/types.h>

#define SCM_HAND_SHAKE_IOCTL       _IOWR('R', 10, struct scm_hand_shake)

#define QCOM_SCM_INFO_GET_FEAT_VERSION_CMD      0x03
#define QCOM_SCM_INFO_BW_PROF_ID                0x07

#define QCOM_SCM_MP_SHM_BRIDGE_ENABLE		0x1c
#define QCOM_SCM_MP_SHM_BRIDGE_DELETE		0x1d
#define QCOM_SCM_MP_SHM_BRIDGE_CREATE		0x1e

#define QCOM_SCM_SVC_SHE                        0x21
#define QCOM_SCM_SHE_ID                         0x1
#define QCOM_SCM_SVC_SAFETY                     0x23
#define QCOM_SCM_SAFETY_ENABLE_FFI_ID           0x1

#define QCOM_SCM_MP_CP_SMMU_APERTURE_ID         0x1b
#define QCOM_SCM_CP_APERTURE_REG                0x0

#define MAX_QCOM_SCM_RESULT                     3
#define MAX_QCOM_SCM_IN                         10

struct scm_hand_shake {
	__u32 svc;
	__u32 cmd;
	__u32 arginfo;
	__u64 args_buffer[MAX_QCOM_SCM_IN];
	__u32 ret;
	__u32 arg_type;
	__u64 qcom_scm_res[MAX_QCOM_SCM_RESULT];
};

int qcom_scm_kgsl_set_smmu_aperture(unsigned int num_context_bank);

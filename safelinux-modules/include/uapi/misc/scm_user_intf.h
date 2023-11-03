// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/* Copyright (C) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define SCM_HAND_SHAKE_IOCTL       _IOWR('R', 10, struct scm_hand_shake)

#define MAX_QCOM_SCM_RETS 3

#define MAX_QCOM_SCM_IN 10

struct scm_hand_shake {
       unsigned int svc;
       unsigned int cmd;
       unsigned int arginfo;
       unsigned int args_buffer[MAX_QCOM_SCM_IN];
       unsigned int ret;
       unsigned int arg_type;
       unsigned int qcom_scm_res[MAX_QCOM_SCM_RETS];
};

int qcom_scm_kgsl_set_smmu_aperture(unsigned int num_context_bank);

// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/* Copyright (C) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define SCM_HAND_SHAKE_IOCTL       _IOWR('R', 10, struct scm_hand_shake)

int qcom_scm_kgsl_set_smmu_aperture(unsigned int num_context_bank);

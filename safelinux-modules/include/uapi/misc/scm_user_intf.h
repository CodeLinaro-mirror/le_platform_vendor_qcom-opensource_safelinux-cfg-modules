/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 * Copyright (C) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/types.h>
#include <linux/version.h>

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

#define QCOM_SCM_SVC_SMC_INVOKE                 0x6
#define QCOM_SCM_SMCINVOKE                      0x2
#define QCOM_SCM_SMCINVOKE_CB_RESP              0x1

#define QCOM_SCM_SVC_CAMERA                     0x18
#define QCOM_SCM_CAMERA_UPDATE_CAMNOC_QOS       0x0A

#define TZ_SVC_BW_PROF_ID			0x07

#define MAX_QCOM_SCM_RESULT                     3
#define MAX_QCOM_SCM_IN                         10

struct scm_hand_shake {
	__u32 svc;
	__u32 cmd;
	__u32 owner;
	__u32 arginfo;
	__u64 args_buffer[MAX_QCOM_SCM_IN];
	__u32 ret;
	__u32 arg_type;
	__u64 qcom_scm_res[MAX_QCOM_SCM_RESULT];
};

#if (LINUX_VERSION_CODE ==  KERNEL_VERSION(5, 14, 0))
enum qcom_scm_arg_types {
        QCOM_SCM_VAL,
        QCOM_SCM_RO,
        QCOM_SCM_RW,
        QCOM_SCM_BUFVAL,
};

#define QCOM_SCM_ARGS_IMPL(num, a, b, c, d, e, f, g, h, i, j, ...) (\
                           (((a) & 0x3) << 4) | \
                           (((b) & 0x3) << 6) | \
                           (((c) & 0x3) << 8) | \
                           (((d) & 0x3) << 10) | \
                           (((e) & 0x3) << 12) | \
                           (((f) & 0x3) << 14) | \
                           (((g) & 0x3) << 16) | \
                           (((h) & 0x3) << 18) | \
                           (((i) & 0x3) << 20) | \
                           (((j) & 0x3) << 22) | \
                           ((num) & 0xf))

#define QCOM_SCM_ARGS(...) QCOM_SCM_ARGS_IMPL(__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

int qcom_scm_kgsl_set_smmu_aperture(unsigned int num_context_bank);


extern int qcom_scm_ddrbw_profiler(uint64_t in_buf,
    size_t in_buf_size, uint64_t out_buf, size_t out_buf_size);
#endif

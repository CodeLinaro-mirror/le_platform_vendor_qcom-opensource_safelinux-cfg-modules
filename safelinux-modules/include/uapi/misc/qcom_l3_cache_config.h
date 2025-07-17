/* SPDX-License-Identifier: GPL-2.0-only with Linux-syscall-note
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QCOM_L3CC_H__
#define __QCOM_L3CC_H__

#include <linux/types.h>
#include <linux/ioctl.h>

typedef enum {
	CLUSTER_ID0,
	CLUSTER_ID1,
} cluster_idx;

typedef enum {
	CPU_ID0 = 0,
	CPU_ID1,
	CPU_ID2,
	CPU_ID3,
} cpu_idx;

typedef enum {
	GET_CONFIG = 0,
	SET_CONFIG,
} op_type;

typedef enum {
	L3_CACHE_SCHEME_ID0 = 0,
	L3_CACHE_SCHEME_ID1,
	L3_CACHE_SCHEME_ID2,
	L3_CACHE_SCHEME_ID3,
	L3_CACHE_SCHEME_ID4,
	L3_CACHE_SCHEME_ID5,
	L3_CACHE_SCHEME_ID6,
	L3_CACHE_SCHEME_ID7,
	L3_CACHE_SCHEME_ID_MAX,
} l3_cache_scheme_idx;

/**
 * struct qcom_l3cc_cluster_config_data - L3 cache configuration data for cluster
 * @stash_sid: Cluster stash scheme ID register
 * @acp_sid: Cluster ACP scheme ID register
 * @cpcr: Cluster partition control register
 */
typedef struct qcom_l3cc_cluster_config_data {
	__u64 stash_sid;
	__u64 acp_sid;
	__u64 cpcr;
} cluster_config_t;

/**
 * struct qcom_l3cc_cpu_sid_config_data - CPU L3 Scheme ID data
 * @cpu: CPU for which the intended L3 scheme ID is applied
 * @sid: L3 Scheme ID requested for CPU
 */
typedef struct qcom_l3cc_cpu_sid_config_data {
	cpu_idx cpu;
	__u64 sid;
} cpu_sid_config_t;

/**
 * struct qcom_l3cc_cl_config - L3 cache config data for a Cluster
 * @cookie: Cookie for service request
 * @cl: Cluster for configuration
 * @config: Cluster L3 cache config data
 * @op: Operation type
 */
struct qcom_l3cc_cl_config {
	__u32 cookie;
	cluster_idx cl;
	cluster_config_t config;
	op_type op;
};

/**
 * struct qcom_l3cc_cpu_sid_config - L3 cache config data for a cpu
 * @cookie: Cookie for service request
 * @cl: Cluster for configuration
 * @config: CPU L3 scheme id configuration data
 * @op: Operation type
 */
struct qcom_l3cc_cpu_sid_config {
	__u32 cookie;
	cluster_idx cl;
	cpu_sid_config_t config;
	op_type op;
};

/**
 * struct qcom_l3cc_wg_config - L3 cache config way group configuration
 * @cookie: Cookie for service request
 * @cl: Cluster for configuration
 * @sid: CPU L3 scheme id for which the way group settings are imposed
 * @cache_ways: Cache ways reserved for the schemeid
 */
struct qcom_l3cc_wg_config {
	__u32 cookie;
	cluster_idx cl;
	__u64 sid;
	__u64 cache_ways;
};

/**
 * struct qcom_l3cc_cl_qos_config - L3 cache QOS settings for scheme id
 * @cookie: Cookie for service request
 * @cl: Cluster for configuration
 * @qos: Qos priority for the schemeid
 * @op: Operation type
 */
struct qcom_l3cc_cl_qos_config {
	__u32 cookie;
	cluster_idx cl;
	__u32 qos;
	op_type op;
};

/**
 * struct qcom_l3cc_regdata - registration data for service consumer
 * @cookie: Cookie for service request
 * @name: Name of service consumer
 */
struct qcom_l3cc_regdata {
	__u32 cookie;
	const char *name;
};

#define QCOM_L3CC_IOCTL_MAGIC 0x99
#define QCOM_L3CC_IOCTL_REGISTER			\
_IOW(QCOM_L3CC_IOCTL_MAGIC, 0, struct qcom_l3cc_regdata)
#define QCOM_L3CC_IOCTL_UNREGISTER			\
_IOW(QCOM_L3CC_IOCTL_MAGIC, 1, struct qcom_l3cc_regdata)
#define QCOM_L3CC_IOCTL_UPDATE_CL_CONFIG			\
_IOW(QCOM_L3CC_IOCTL_MAGIC, 2, struct qcom_l3cc_cl_config)
#define QCOM_L3CC_IOCTL_UPDATE_CPU_SID			\
_IOW(QCOM_L3CC_IOCTL_MAGIC, 3, struct qcom_l3cc_cpu_sid_config)
#define QCOM_L3CC_IOCTL_SET_CL_WG			\
_IOW(QCOM_L3CC_IOCTL_MAGIC, 4, struct qcom_l3cc_wg_config)
#define QCOM_L3CC_IOCTL_UPDATE_CL_SID_QOS			\
_IOW(QCOM_L3CC_IOCTL_MAGIC, 5, struct qcom_l3cc_cl_qos_config)

#endif

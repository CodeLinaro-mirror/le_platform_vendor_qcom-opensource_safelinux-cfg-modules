/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef __KIUMD_H__
#define __KIUMD_H__

#include <linux/types.h>
#include <linux/unistd.h>

#define KIUMD_SMMU_MAP_BUF		_IOWR('R', 10, struct kiumd_user)
#define KIUMD_SMMU_UNMAP_BUF	_IOWR('R', 11, struct kiumd_user)
#define KIUMD_IOVA_MAP_CTRL             _IOWR('R', 14, struct kiumd_user)
#define KIUMD_SET_USER_CONTEXT         _IOWR('R', 15, struct kiumd_smmu_user)
#define KIUMD_PER_PROCESS_ALLOC 	_IOWR('R', 16, struct kiumd_smmu_user)
#define KIUMD_PER_PROCESS_SET         _IOWR('R', 17, struct kiumd_smmu_user)
#define KIUMD_PER_PROCESS_FREE        _IOWR('R', 18, struct kiumd_smmu_user)
#define KIUMD_FD_DMABUF_HANDLE          _IOWR('R', 19, struct kiumd_user)
#define KIUMD_CUSTOM_IOVA_INIT          _IOWR('R', 20, struct kiumd_user)
#define KIUMD_GLOBAL_PT_SET            _IOWR('R', 21, struct kiumd_smmu_user)
#define KIUMD_SMMU_SECURE_MAP           _IOWR('R', 22, struct kiumd_user)
#define KIUMD_SMMU_SECURE_UNMAP         _IOWR('R', 23, struct kiumd_user)

#define IOMMU_NOEXEC    (1 << 3)
#define IOMMU_MMIO      (1 << 4)
#define IOMMU_PRIV      (1 << 5)
#define DMA_ATTR_PRIVILEGED	(1UL << 9)

#define HANDLE_TO_FD    -1
#define FD_TO_HANDLE    -2
#define CLOSE_HANDLE    -3

#define KGSL_GLOBAL_PT 1
#define KGSL_PER_PROCESS_PT 2
#define KGSL_DEFAULT_PT  3

/*
 * Choosing an arbitrary value other than 0 and 1
 * to avoid setting this condition, if user doesnt calloc
 * or pre-initialize the values
 * */
#define FIXED_IOVA_AT_ZERO 5

enum kiumd_iova_addr_type {
	KGSL_SMMU_GLOBALPT_FIXED_ADDR_SET,
	KGSL_SMMU_GLOBALPT_FIXED_ADDR_CLEAR,
};

struct kiumd_iova {
	int vfio_fd;
	enum kiumd_iova_addr_type iova_flag;
	__u64 iova;
};

struct kiumd_smmu_user {
	int vfio_fd;
	long pgtbl_ops_ptr;
	__u64 ttbr0;
	__u16 asid;
};

struct kiumd_acl_entry {
	__u32 vmid;
	__u32 perms;
};

struct kiumd_mem_parcel {
	__u32 nr_acl_entries;
	struct kimud_acl_entry *acl_list;
};

struct kiumd_user {
	int vfio_fd;
	int dma_buf_fd;
	int heap_fd;
	int flag;
	unsigned long dma_addr;
	int buf_token;
	int dma_attr;
	int dma_direction;
	int ptselect;
	__u32 handle;
	int is_iova_zero;
	struct kiumd_mem_parcel mem_parcel;
	int id;
};

#endif /* __KIUMD_H__ */

/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __KIUMD_H__
#define __KIUMD_H__

#include <linux/types.h>
#include <linux/unistd.h>

#define KIUMD_SMMU_MAP_BUF		_IOWR('R', 10, struct kiumd_user)
#define KIUMD_SMMU_UNMAP_BUF	        _IOWR('R', 11, struct kiumd_user_unmap)
#define KIUMD_IOVA_MAP_CTRL             _IOWR('R', 14, struct kiumd_user)
#define KIUMD_SET_PGTBL_CONTEXT          _IOWR('R', 15, struct kiumd_smmu_user)
#define KIUMD_PER_PROCESS_ALLOC 	_IOWR('R', 16, struct kiumd_smmu_user)
#define KIUMD_PER_PROCESS_SET           _IOWR('R', 17, struct kiumd_smmu_user)
#define KIUMD_PER_PROCESS_FREE          _IOWR('R', 18, struct kiumd_smmu_user)
#define KIUMD_CUSTOM_IOVA_INIT          _IOWR('R', 20, struct kiumd_user)
#define KIUMD_GLOBAL_PT_SET             _IOWR('R', 21, struct kiumd_smmu_user)
#define KIUMD_SMMU_MMIO_MAP		_IOWR('R', 24, struct kiumd_smmu_mmio_map)
#define KIUMD_SMMU_MMIO_UNMAP		_IOWR('R', 25, struct kiumd_user_unmap)
#define KIUMD_SMMU_FAULT_HANDLE_REGISTER _IOWR('R', 26, struct kiumd_user)
#define KIUMD_SMMU_FAULT_HANDLE_DEREGISTER _IOWR('R', 27, struct kiumd_user)
#define KIUMD_VFIO_CTX_INIT	        _IOWR('R', 28, struct kiumd_dev_mem_info)
#define KIUMD_SMMU_MANAGED_IOVA_MAP	_IOWR('R', 29, struct kiumd_user)
#define KIUMD_SMMU_MANAGED_IOVA_UNMAP	_IOWR('R', 30, struct kiumd_user_unmap)
#define KIUMD_SMMU_ASSIGN_BUF		_IOWR('R', 31, struct kiumd_user)
#define KIUMD_SMMU_UNASSIGN_BUF		_IOWR('R', 32, struct kiumd_user)
#define KIUMD_MANAGE_RUNTIME_PM        _IOWR('R', 33, struct kiumd_user)
#define KIUMD_VFIO_CTX_GET_DATA	        _IOWR('R', 34, struct kiumd_dev_mem_info)

#define IOCTL_REGISTER_EVENTFD _IOW('E', 1, int)
#define IOCTL_UNMASK_INTERRUPT _IO('E', 2)
#define IOCTL_REGISTER_EVENTFD_INDEX _IOW('E', 3, int[2])
#define IOCTL_DMABUF_MAP _IOWR('R', 4, struct kiumd_user)
#define IOCTL_SET_PGTBL_CONTEXT _IOWR('R', 5, struct kiumd_smmu_user)
#define IOCTL_PER_PROCESS_ALLOC _IOWR('R', 6, struct kiumd_smmu_user)
#define IOCTL_PROCESS_PGTBL_SET _IOWR('R', 7, struct kiumd_smmu_user)
#define IOCTL_PLAT_DEV_INIT _IOWR('R', 8, struct kiumd_smmu_user)
#define IOCTL_DMABUF_UNMAP _IOWR('R', 9, struct kiumd_user)
#define IOCTL_GLOBAL_PGTABLE_SET _IOWR('R', 10, struct kiumd_smmu_user)
#define IOCTL_PROCESS_PGTABLE_FREE _IOWR('R', 11, struct kiumd_smmu_user)
#define IOCTL_FIXED_IOVA_CTRL _IOWR('R', 12, struct kiumd_user)
#define IOCTL_DMABUF_FIXED_MAP _IOWR('R', 13, struct kiumd_user)
#define IOCTL_DMABUF_FIXED_UNMAP _IOWR('R', 14, struct kiumd_user)
#define IOCTL_SMMU_MMIO_MAP _IOWR('R', 15, struct kiumd_smmu_mmio_map)
#define IOCTL_MANAGE_RUNTIME_PM _IOWR('R', 16, struct kiumd_user)

#define IOMMU_NOEXEC    (1 << 3)
#define IOMMU_MMIO      (1 << 4)
#define IOMMU_PRIV      (1 << 5)
#define DMA_ATTR_PRIVILEGED	(1UL << 9)

#define KGSL_GLOBAL_PT 1
#define KGSL_PER_PROCESS_PT 2
#define KGSL_DEFAULT_PT  3

/*
 * Choosing an arbitrary value other than 0 and 1
 * to avoid setting this condition, if user doesnt calloc
 * or pre-initialize the values
 * */
#define FIXED_IOVA_AT_ZERO 5

#define KIUMD_SMMU_SET_TTBR0_CONFIG    11
#define KIUMD_SMMU_SET_TTBR1_CONFIG    12

#define KIUMD_IOVA_SIZE_ALIGNED 6
#define KIUMD_IOVA_PAGE_ALIGNED 7

#define DEV_PWR_OFF 1
#define DEV_PWR_ON 2

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
	__u32 flags;
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
	int is_iova_zero;
	struct kiumd_mem_parcel mem_parcel;
	int id;
	unsigned int hyp_id;
	int pid;
	__u32 pt_id;
	__u64 pgtbl_ops_ptr;
	__u64 ttbr0;
	__u16 asid;
	__u32 flags;
	__u32 pm_state;
	int is_fix_map;
};

struct kiumd_smmu_mmio_map {
	__u8 fixed_iova;
	int vfio_fd;
	int id;
	__u64 iova;
	__u64 reg_len;
	char *reg_name;
	int reg_idx;
};

struct kiumd_user_unmap {
	__u32 id;
};

struct kiumd_mem_info {
	__u64 offset;
	__u64 size;
};

struct kiumd_dev_mem_info {
	int vfio_fd;
	__u32 num_regions;
	struct kiumd_mem_info *mem_info;
};

struct irqinfo_user {
	int irq_index;
	int event_fd;
	int num_irqs;
};

#endif /* __KIUMD_H__ */

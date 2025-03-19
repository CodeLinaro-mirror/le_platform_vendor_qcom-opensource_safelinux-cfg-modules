/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM safelinux

#if !defined(TRACE_SAFELINUX_MODULES) || defined(TRACE_HEADER_MULTI_READ)
#define TRACE_SAFELINUX_MODULES
#include <linux/tracepoint.h>

TRACE_EVENT(kiumd_smmuv2_write_context_bank_start,

	TP_PROTO(int idx, int cbndx, int cbar),

	TP_ARGS(idx, cbndx, cbar),

	TP_STRUCT__entry(
		__field(int, idx)
		__field(int, cbndx)
		__field(int, cbar)
	),

	TP_fast_assign(
		__entry->idx = idx;
		__entry->cbndx = cbndx;
		__entry->cbar = cbar;
	),

	TP_printk("idx=%d, cbndx=%d, cbar=%d", __entry->idx, __entry->cbndx, __entry->cbar)
);

TRACE_EVENT(kiumd_smmuv2_write_context_bank_end,

	TP_PROTO(int idx),

	TP_ARGS(idx),

	TP_STRUCT__entry(
		__field(int, idx)
	),

	TP_fast_assign(
		__entry->idx = idx;
	),

	TP_printk("idx=%d", __entry->idx)
);


TRACE_EVENT(kiumd_perprocess_pt_alloc_start,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_perprocess_pt_alloc_end,

	TP_PROTO(int vfio_fd, long pgtbl_ops_ptr, __u64 ttbr0, __u16 asid),

	TP_ARGS(vfio_fd, pgtbl_ops_ptr, ttbr0, asid),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(long, pgtbl_ops_ptr)
		__field(__u64, ttbr0)
		__field(__u16, asid)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->pgtbl_ops_ptr = pgtbl_ops_ptr;
		__entry->ttbr0 = ttbr0;
		__entry->asid = asid;
	),

	TP_printk("vfio_fd=%d, pgtbl_ops_ptr=0x%x, ttbr0=0x%x, asid=%u", __entry->vfio_fd, __entry->pgtbl_ops_ptr, __entry->ttbr0, __entry->asid)
);

TRACE_EVENT(kiumd_global_pgtble_set_start,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_global_pgtble_set_end,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_perprocess_pgtble_set_start,

	TP_PROTO(int vfio_fd, long pgtbl_ops_ptr, __u64 ttbr0, __u16 asid),

	TP_ARGS(vfio_fd, pgtbl_ops_ptr, ttbr0, asid),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(long, pgtbl_ops_ptr)
		__field(__u64, ttbr0)
		__field(__u16, asid)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->pgtbl_ops_ptr = pgtbl_ops_ptr;
		__entry->ttbr0 = ttbr0;
		__entry->asid = asid;
	),

	TP_printk("vfio_fd=%d, pgtbl_ops_ptr=0x%x, ttbr0=0x%x, asid=%u", __entry->vfio_fd, __entry->pgtbl_ops_ptr, __entry->ttbr0, __entry->asid)
);

TRACE_EVENT(kiumd_perprocess_pgtble_set_end,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_perprocess_pgtble_free_start,

	TP_PROTO(int vfio_fd, long pgtbl_ops_ptr, __u64 ttbr0, __u16 asid),

	TP_ARGS(vfio_fd, pgtbl_ops_ptr, ttbr0, asid),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(long, pgtbl_ops_ptr)
		__field(__u64, ttbr0)
		__field(__u16, asid)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->pgtbl_ops_ptr = pgtbl_ops_ptr;
		__entry->ttbr0 = ttbr0;
		__entry->asid = asid;
	),

	TP_printk("vfio_fd=%d, pgtbl_ops_ptr=0x%x, ttbr0=0x%x, asid=%u", __entry->vfio_fd, __entry->pgtbl_ops_ptr, __entry->ttbr0, __entry->asid)
);

TRACE_EVENT(kiumd_perprocess_pgtble_free_end,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_dmabuf_custom_iova_init_start,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_dmabuf_custom_iova_init_end,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_dmabuf_vfio_map_start,

	TP_PROTO(char * device_name, int vfio_fd, int dma_buf_fd, int dma_attr, int dma_direction, int ptselect,
		 int is_iova_zero, u64 size, void *kiumd_ctx),

	TP_ARGS(device_name, vfio_fd, dma_buf_fd, dma_attr, dma_direction, ptselect, is_iova_zero, size, kiumd_ctx),

	TP_STRUCT__entry(
		__string(str, device_name)
		__field(int, vfio_fd)
		__field(int, dma_buf_fd)
		__field(int, dma_attr)
		__field(int, dma_direction)
		__field(int, ptselect)
		__field(int, is_iova_zero)
		__field(u64, size)
		__field(void*, kiumd_ctx)
	),

	TP_fast_assign(
		__assign_str(str, device_name);
		__entry->vfio_fd = vfio_fd;
		__entry->dma_buf_fd = dma_buf_fd;
		__entry->dma_attr = dma_attr;
		__entry->dma_direction = dma_direction;
		__entry->ptselect = ptselect;
		__entry->is_iova_zero = is_iova_zero;
		__entry->size = size;
		__entry->kiumd_ctx = kiumd_ctx;
	),

	TP_printk("device_name=%s, vfio_fd=%d, dma_buf_fd=%d, dma_attr=%d, dma_direction=%d, ptselect=%d, is_iova_zero=%d, size=%lu, kiumd_ctx=%p",
		   __get_str(str), __entry->vfio_fd, __entry->dma_buf_fd, __entry->dma_attr, __entry->dma_direction,
		   __entry->ptselect, __entry->is_iova_zero, __entry->size, __entry->kiumd_ctx)
);

TRACE_EVENT(kiumd_dmabuf_vfio_map_end,

	TP_PROTO(int vfio_fd, int id, unsigned long dma_addr),

	TP_ARGS(vfio_fd, id, dma_addr),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(int, id)
		__field(unsigned long, dma_addr)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->id = id;
		__entry->dma_addr = dma_addr;
	),

	TP_printk("vfio_fd=%d,id=%d, dma_addr=0x%x", __entry->vfio_fd,  __entry->id, __entry->dma_addr)
);

TRACE_EVENT(kiumd_dmabuf_vfio_unmap_start,

	TP_PROTO(char * device_name, int vfio_fd, int dma_buf_fd, unsigned long dma_addr, int dma_attr, int dma_direction,
	         int ptselect, int is_iova_zero, size_t size, void *kiumd_ctx),

	TP_ARGS(device_name, vfio_fd, dma_buf_fd, dma_addr, dma_attr, dma_direction, ptselect, is_iova_zero, size, kiumd_ctx),

	TP_STRUCT__entry(
		__string(str, device_name)
		__field(int, vfio_fd)
		__field(int, dma_buf_fd)
		__field(unsigned long, dma_addr)
		__field(int, dma_attr)
		__field(int, dma_direction)
		__field(int, ptselect)
		__field(int, is_iova_zero)
		__field(size_t, size)
		__field(void *, kiumd_ctx)
	),

	TP_fast_assign(
		__assign_str(str, device_name);
		__entry->vfio_fd = vfio_fd;
		__entry->dma_buf_fd = dma_buf_fd;
		__entry->dma_addr = dma_addr;
		__entry->dma_attr = dma_attr;
		__entry->dma_direction = dma_direction;
		__entry->ptselect = ptselect;
		__entry->is_iova_zero = is_iova_zero;
		__entry->size = size;
		__entry->kiumd_ctx = kiumd_ctx;
	),

	TP_printk("device_name=%s, vfio_fd=%d, dma_buf_fd=%d, dma_addr=0x%x, dma_attr=%d, dma_direction=%d, ptselect=%d, is_iova_zero=%d size=%zu kiumd_ctx=%p",
		   __get_str(str), __entry->vfio_fd, __entry->dma_buf_fd, __entry->dma_addr, __entry->dma_attr, __entry->dma_direction,
		   __entry->ptselect, __entry->is_iova_zero, __entry->size, __entry->kiumd_ctx)
);

TRACE_EVENT(kiumd_dmabuf_vfio_unmap_end,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_iova_ctrl_start,

	TP_PROTO(int vfio_fd, int iova_flag, unsigned long iova),

	TP_ARGS(vfio_fd, iova_flag, iova),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(int, iova_flag)
		__field(unsigned long, iova)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->iova_flag = iova_flag;
		__entry->iova = iova;
	),

	TP_printk("vfio_fd=%d, iova_flag=%d, iova=0x%x", __entry->vfio_fd, __entry->iova_flag, __entry->iova)
);

TRACE_EVENT(kiumd_iova_ctrl_end,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_fd_dmabuf_handler_start,

	TP_PROTO(int handle, int dma_buf_fd),

	TP_ARGS(handle, dma_buf_fd),

	TP_STRUCT__entry(
		__field(int, handle)
		__field(int, dma_buf_fd)
	),

	TP_fast_assign(
		__entry->handle = handle;
		__entry->dma_buf_fd = dma_buf_fd;
	),

	TP_printk("handle=%d, dma_buf_fd=%d", __entry->handle, __entry->dma_buf_fd)
);

TRACE_EVENT(kiumd_fd_dmabuf_handler_fd_to_handle,

	TP_PROTO(int dma_buf_fd, int handle, int handle_refcount),

	TP_ARGS(dma_buf_fd, handle, handle_refcount),

	TP_STRUCT__entry(
		__field(int, dma_buf_fd)
		__field(int, handle)
		__field(int, handle_refcount)
	),

	TP_fast_assign(
		__entry->dma_buf_fd = dma_buf_fd;
		__entry->handle = handle;
		__entry->handle_refcount = handle_refcount;
	),

	TP_printk("dma_buf_fd=%d, handle=%d, handle_refcount=%d",
		__entry->dma_buf_fd, __entry->handle, __entry->handle_refcount)
);

TRACE_EVENT(kiumd_fd_dmabuf_handler_handle_to_fd,

	TP_PROTO(int handle, int dma_buf_fd, int handle_refcount),

	TP_ARGS(handle, dma_buf_fd, handle_refcount),

	TP_STRUCT__entry(
		__field(int, handle)
		__field(int, dma_buf_fd)
		__field(int, handle_refcount)
	),

	TP_fast_assign(
		__entry->handle = handle;
		__entry->dma_buf_fd = dma_buf_fd;
		__entry->handle_refcount = handle_refcount;
	),

	TP_printk("handle=%d, dma_buf_fd=%d, handle_refcount=%d",
		__entry->handle, __entry->dma_buf_fd, __entry->handle_refcount)
);

TRACE_EVENT(kiumd_fd_dmabuf_handler_end,

	TP_PROTO(int handle, int dma_buf_fd),

	TP_ARGS(handle, dma_buf_fd),

	TP_STRUCT__entry(
		__field(int, handle)
		__field(int, dma_buf_fd)
	),

	TP_fast_assign(
		__entry->handle = handle;
		__entry->dma_buf_fd = dma_buf_fd;
	),

	TP_printk("handle=%d, dma_buf_fd=%d", __entry->handle, __entry->dma_buf_fd)
);

TRACE_EVENT(kiumd_io_pgtable_hyp_assign_page_start,

	TP_PROTO(u32 *vmid, u64 page, u32 nr_acl_entries),

	TP_ARGS(vmid, page, nr_acl_entries),

	TP_STRUCT__entry(
		__dynamic_array(u32, vmid_arr, nr_acl_entries)
		__field(u32, page)
		__field(u32, nr_acl_entries)
	),

	TP_fast_assign(
		unsigned int i;
		u32 *vmid_ptr = __get_dynamic_array(vmid_arr);
		__entry->page = page;
		__entry->nr_acl_entries = nr_acl_entries;
		for(i = 0; i < __entry->nr_acl_entries; i++) {
			vmid_ptr[i] = vmid[i];
		}
	),

	TP_printk("vmid=%s, page=%llu, nr_acl_entries=%d", __print_array(__get_dynamic_array(vmid_arr),
		 __entry->nr_acl_entries, sizeof(u32)), __entry->page, __entry->nr_acl_entries)
);

TRACE_EVENT(kiumd_io_pgtable_hyp_assign_page_end,

	TP_PROTO(u64 page, u32 nr_acl_entries),

	TP_ARGS(page, nr_acl_entries),

	TP_STRUCT__entry(
		__field(u32, page)
		__field(u32, nr_acl_entries)
	),

	TP_fast_assign(
		__entry->page = page;
		__entry->nr_acl_entries = nr_acl_entries;
	),

	TP_printk("page=%%llu, nr_acl_entries=%d", __entry->page, __entry->nr_acl_entries)
);

TRACE_EVENT(kiumd_io_pgtable_hyp_unassign_page_start,

	TP_PROTO(u32 *vmid, u64 page, u32 nr_acl_entries),

	TP_ARGS(vmid, page, nr_acl_entries),

	TP_STRUCT__entry(
		__dynamic_array(u32, vmid_arr, nr_acl_entries)
		__field(u32, page)
		__field(u32, nr_acl_entries)
	),

	TP_fast_assign(
		unsigned int i;
		u32 *vmid_ptr = __get_dynamic_array(vmid_arr);
		__entry->page = page;
		__entry->nr_acl_entries = nr_acl_entries;
		for(i = 0; i < __entry->nr_acl_entries; i++) {
			vmid_ptr[i] = vmid[i];
		}
	),

	TP_printk("vmid=%s, page=%llu, nr_acl_entries=%d", __print_array(__get_dynamic_array(vmid_arr),
		__entry->nr_acl_entries, sizeof(u32)), __entry->page, __entry->nr_acl_entries)
);

TRACE_EVENT(kiumd_io_pgtable_hyp_unassign_page_end,

	TP_PROTO(u64 page, u32 nr_acl_entries),

	TP_ARGS(page, nr_acl_entries),

	TP_STRUCT__entry(
		__field(u32, page)
		__field(u32, nr_acl_entries)
	),

	TP_fast_assign(
		__entry->page = page;
		__entry->nr_acl_entries = nr_acl_entries;
	),

	TP_printk("page=%llu, nr_acl_entries=%d", __entry->page, __entry->nr_acl_entries)
);

TRACE_EVENT(kiumd_hyp_unassign_sg_start,

	TP_PROTO(struct sg_table *sgt),

	TP_ARGS(sgt),

	TP_STRUCT__entry(
		__field(unsigned int, nents)
	),

	TP_fast_assign(
		__entry->nents = sgt->nents;
	),

	TP_printk("nents=%d", __entry->nents)
);

TRACE_EVENT(kiumd_hyp_unassign_sg_end,

	TP_PROTO(struct sg_table *sgt),

	TP_ARGS(sgt),

	TP_STRUCT__entry(
		__field(unsigned int, nents)
	),

	TP_fast_assign(
		__entry->nents = sgt->nents;
	),

	TP_printk("nents=%d", __entry->nents)
);

TRACE_EVENT(kiumd_hyp_assign_sg_start,

	TP_PROTO(struct sg_table *sgt),

	TP_ARGS(sgt),

	TP_STRUCT__entry(
		__field(unsigned int, nents)
	),

	TP_fast_assign(
		__entry->nents = sgt->nents;
	),

	TP_printk("nents=%d", __entry->nents)
);

TRACE_EVENT(kiumd_hyp_assign_sg_end,

	TP_PROTO(struct sg_table *sgt),

	TP_ARGS(sgt),

	TP_STRUCT__entry(
		__field(unsigned int, nents)
	),

	TP_fast_assign(
		__entry->nents = sgt->nents;
	),

	TP_printk("nents=%d", __entry->nents)
);

TRACE_EVENT(kiumd_dmabuf_vfio_secure_map_start,

	TP_PROTO(int vfio_fd, int dma_buf_fd, size_t size),

	TP_ARGS(vfio_fd, dma_buf_fd, size),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(int, dma_buf_fd)
		__field(int, size)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->dma_buf_fd = dma_buf_fd;
		__entry->size = size;
	),

	TP_printk("vfio_fd=%d, dma_buf_fd=%d, size=%zu", __entry->vfio_fd, __entry->dma_buf_fd, __entry->size)
);

TRACE_EVENT(kiumd_dmabuf_vfio_secure_map_end,

	TP_PROTO(int vfio_fd, int id,unsigned long dma_addr),

	TP_ARGS(vfio_fd, id, dma_addr),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(int, id)
		__field(unsigned long, dma_addr)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->id = id;
		__entry->dma_addr = dma_addr;
	),

	TP_printk("vfio_fd=%d, id=%d, dma_addr=0x%x", __entry->vfio_fd, __entry->id, __entry->dma_addr)
);

TRACE_EVENT(kiumd_dmabuf_vfio_secure_unmap_start,

	TP_PROTO(int vfio_fd, int dma_buf_fd, int ptselect, unsigned long dma_addr, int id),

	TP_ARGS(vfio_fd, dma_buf_fd, ptselect, dma_addr, id),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(int, dma_buf_fd)
		__field(int, ptselect)
		__field(unsigned long, dma_addr)
		__field(int, id)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->dma_buf_fd = dma_buf_fd;
		__entry->ptselect = ptselect;
		__entry->dma_addr = dma_addr;
		__entry->id = id;
	),

	TP_printk("vfio_fd=%d, dma_buf_fd=%d, ptselect=%d, dma_addr=0x%x, id=%d", __entry->vfio_fd, __entry->dma_buf_fd, __entry->ptselect, __entry->dma_addr, __entry->id)
);

TRACE_EVENT(kiumd_dmabuf_vfio_secure_unmap_end,

	TP_PROTO(int vfio_fd, int dma_buf_fd),

	TP_ARGS(vfio_fd, dma_buf_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(int, dma_buf_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->dma_buf_fd = dma_buf_fd;
	),

	TP_printk("vfio_fd=%d, dma_buf_fd=%d", __entry->vfio_fd, __entry->dma_buf_fd)
);

TRACE_EVENT(kiumd_mmio_smmu_map_start,

	TP_PROTO(int vfio_fd, __u8 fixed_iova, __u64 iova),

	TP_ARGS(vfio_fd, fixed_iova, iova),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(__u8, fixed_iova)
		__field(__u64, iova)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
		__entry->fixed_iova = fixed_iova;
		__entry->iova = iova;
	),

	TP_printk("vfio_fd=%d, fixed_iova=%u, iova=0x%x", __entry->vfio_fd, __entry->fixed_iova, __entry->iova)
);

TRACE_EVENT(kiumd_mmio_smmu_map_end,

	TP_PROTO(char * reg_name, int vfio_fd, int id, __u64 iova, __u64 reg_len),

	TP_ARGS(reg_name, vfio_fd, id, iova, reg_len),


	TP_STRUCT__entry(
		__string(str, reg_name)
		__field(int, vfio_fd)
		__field(int, id)
		__field(__u64, iova)
		__field(__u64, reg_len)
	),

	TP_fast_assign(
		__assign_str(str, reg_name);
		__entry->vfio_fd = vfio_fd;
		__entry->id = id;
		__entry->iova = iova;
		__entry->reg_len = reg_len;
	),

	TP_printk("reg_name=%s, vfio_fd=%d, id=%d, iova=%llx, reg_len=%llx", __get_str(str), __entry->vfio_fd, __entry->id, __entry->iova, __entry->reg_len)
);

TRACE_EVENT(kiumd_mmio_smmu_unmap_start,

	TP_PROTO(int vfio_fd, __u64 iova, int id),

	TP_ARGS(vfio_fd, iova, id),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
		__field(__u64, iova)
		__field(int, id)
	),

	TP_fast_assign(
		__entry->vfio_fd =  vfio_fd;
		__entry->iova = iova;
		__entry->id = id;
	),

	TP_printk("vfio_fd=%d, iova=%llx, id=%d", __entry->vfio_fd, __entry->iova, __entry->id)
);

TRACE_EVENT(kiumd_mmio_smmu_unmap_end,

	TP_PROTO(int id),

	TP_ARGS(id),

	TP_STRUCT__entry(
		__field(int, id)
	),

	TP_fast_assign(
		__entry->id = id;
	),

	TP_printk("id=%d", __entry->id)
);

TRACE_EVENT(kiumd_vfio_ctx_init_start,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

TRACE_EVENT(kiumd_vfio_ctx_init_end,

	TP_PROTO(int vfio_fd),

	TP_ARGS(vfio_fd),

	TP_STRUCT__entry(
		__field(int, vfio_fd)
	),

	TP_fast_assign(
		__entry->vfio_fd = vfio_fd;
	),

	TP_printk("vfio_fd=%d", __entry->vfio_fd)
);

#endif /* _TRACE_SAFELINUX_MODULES_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE safelinux_modules_trace

#include <trace/define_trace.h>


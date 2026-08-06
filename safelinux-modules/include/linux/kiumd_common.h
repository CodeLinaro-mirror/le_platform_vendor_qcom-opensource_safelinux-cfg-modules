// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2007-2008 Advanced Micro Devices, Inc.
 * Author: Joerg Roedel <jroedel@suse.de>
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __KIUMD_COMMON_H__
#define __KIUMD_COMMON_H__

#include <linux/dma-buf.h>
#include <linux/dma-direction.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/iommu_iova_map.h>
#include <linux/iova.h>
#include <linux/kernel.h>
#include <uapi/misc/kiumd.h>
#include <uapi/misc/scm_user_intf.h>
#if (KERNEL_VERSION(5, 14, 0) != LINUX_VERSION_CODE)
#include "arm-smmu/arm-smmu.h"
#else
#include "arm-smmu.h"
#endif

#define SMMU_MAPTABLE_SIZE		(10)
#define MAX_KIUMD_ACL_ENTRIES		(64)
#define KIUMD_MAX_VMID			(64)
#define KIUMD_MAX_PERMS			(8)

//KGSL related global variables
//No.of pages for 6GB IOVA space with 4K page size//
#define KGSL_PT_MEM_PAGES		(0x180000)
#define KGSL_GLOBAL_PT_BASE_IOVA	(0xFFFFFF8000000000)
#define KGSL_PER_PROCESS_PT_BASE_IOVA	(0x60000000)
#define KGSL_PER_PROCESS_PT_END_IOVA	(0x240000000)

#define KIUSR_IS_KGSL(kiusr)		(kiusr.ptselect == KGSL_GLOBAL_PT ||  \
					 kiusr.ptselect == KGSL_PER_PROCESS_PT)

#define KIUSR_IS_PRIV(kiusr)		(kiusr.dma_attr == DMA_ATTR_PRIVILEGED)
#define KIUSR_IS_IOVA_ZERO(kiusr)	(kiusr.is_iova_zero == FIXED_IOVA_AT_ZERO)

#define IOVA_ZERO			((dma_addr_t)0)
#define KIUMD_MAX_REG_NAME_LEN		(100)
#define KIUMD_INDEX_OFFSET		(40)

#define KIUMD_32BIT_START_IOVA		(0x1000)
#define KIUMD_32BIT_END_IOVA		(0xFFFFFFFF)
#define DEFAULT_IOVA_ALIGN_SHIFT_MAX	(12)

struct smmu_device_obj {
	struct kobject *kobj;
	int smmu_fsr;
	int smmu_iova;
	int flag;
	struct smmu_device_obj *next;
};

struct kiumd_smmu_mmio_ctx {
	resource_size_t addr;
	dma_addr_t iova;
	size_t size;
	struct device *dev;
};

struct kiumd_smmu_kgsl_ctx {
	u64 iova;
	int pt_id;
	int ptselect;
};

enum iommu_dma_cookie_type {
	IOMMU_DMA_IOVA_COOKIE,
	IOMMU_DMA_MSI_COOKIE,
};

/*
 * This is a redefinition of kernel struct - struct dma_heap_attachment
 * to obtain the sgtable to map buffers with specific attributes
 * using dma_map_sgtable
 */

struct kiumd_dma_heap_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
	bool mapped;
};

/*
 * This is a redefinition of kernel struct - struct iommu_dma_cookie
 * We are using the iova cookie to set the IOVA, when device want to
 * map a buffer at a specific IOVA
 */

struct kiumd_iommu_dma_cookie {
	enum iommu_dma_cookie_type      type;
	union {
		struct {
			struct iova_domain      iovad;
			struct iova_fq __percpu *fq;    /* Flush queue */
			atomic64_t              fq_flush_start_cnt;
			atomic64_t              fq_flush_finish_cnt;
			struct timer_list       fq_timer;
			atomic_t                fq_timer_on;
		};
		dma_addr_t              msi_iova;
	};
	struct list_head                msi_page_list;
	struct iommu_domain             *fq_domain;
	struct mutex			mutex;
};

/*
 * This is a kernel redefinition of the struct - iommu_group, to obtain
 * iommu domain from default domain, the iommu domain from iommu group is
 * a blocking domain with the latest update from vfio frameworks on linux6.1
 * and shouldn't be used
 */

struct kiumd_iommu_group {
	struct kobject kobj;
	struct kobject *devices_kobj;
	struct list_head devices;
	struct xarray pasid_array;
	struct mutex mutex;
	void *iommu_data;
	void (*iommu_data_release)(void *iommu_data);
	char *name;
	int id;
	struct iommu_domain *default_domain;
	struct iommu_domain *blocking_domain;
	struct iommu_domain *domain;
	struct list_head entry;
	unsigned int owner_cnt;
	void *owner;
};

static void _tlb_flush_all(void *cookie)
{
}

static void _tlb_flush_walk(unsigned long iova, size_t size,
			    size_t granule, void *cookie)
{
}

static void _tlb_add_page(struct iommu_iotlb_gather *gather,
			  unsigned long iova, size_t granule,
			  void *cookie)
{
}

static const struct iommu_flush_ops kgsl_iopgtbl_tlb_ops = {
	.tlb_flush_all = _tlb_flush_all,
	.tlb_flush_walk = _tlb_flush_walk,
	.tlb_add_page = _tlb_add_page,
};

/**
 * struct kiumd_secure_map_context: Structure for secure
 * map context.
 * @vmids: vmids for subsystem which need to do hyp assign
 * @perms: permissions
 *
 */
struct kiumd_secure_map_context {
	u64 nr_acl_entries;
	int *vmids;
	int *perms;
};


/**
 * struct kiumd_reserved_mem_area: Structure for reserved
 * memory area.
 * @size: size of reserved memory area
 * @base: start address of reserved memory area
 *
 */
struct kiumd_reserved_mem {
	int num_regions;
	struct mutex resmem_lock;
	struct kiumd_resmem_area {
		size_t size;
		u64    base;
	} *area;
};

/**
 * struct pgtable_map: to store each pagetable iova details for GPU
 * @idx: pagetable id for each GPU process
 * @start_iova: Starting iova for GPU
 * @end_iova: Ending iova for GPU
 * @rbtree: Red-black tree root for managing iova ranges
 * @ttbr0_addr: TTBR0 address for GPU
 * @pgtbl_ops_ptr: Pointer to pagetable operations for GPU
 * @last_allocated_end: Last allocated end address for GPU
 * @kgsl_rbtree_lock: lock for pagetable add/retrieve
 */
struct pgtable_map {
	unsigned int idx;
	unsigned long start_iova;
	unsigned long end_iova;
	struct rb_root rbtree;
	struct mutex pgctx_lock;
//Used only by GPU
	unsigned long ttbr0_addr;
	unsigned long pgtbl_ops_ptr;
	unsigned long last_allocated_end;
	spinlock_t kgsl_rbtree_lock;
	struct io_pgtable_ops *pgtable_ops;
};

struct kiumd_ctx {
	struct device *staging_dev;
	struct mutex map_lock;
	struct xarray kiumd_xa_smap;
	struct xarray kiumd_xa_hyp;
	struct xarray kiumd_xa_kgsl_pt;
	u32 pt_id;
	struct hlist_node pgtable_map;
	spinlock_t pt_lock;
	struct kiumd_reserved_mem resmem;
	struct mutex managed_rbtree_lock;
	unsigned long pt_start_iova;
	unsigned long pt_end_iova;
	unsigned long max_shift;
	struct io_pgtable *pgtable;
	struct pgtable_map *pgtable_ctx;
};

struct iommu_addr_entry {
	unsigned long base_addr;
	unsigned long size;
	struct rb_node rbnode;
};

struct smmu_map_data {
	int id;			// Map ID

	struct device *dev;	//Device details
	struct device *staging_dev;

	struct {		// Dma buf details
		struct sg_table *sgt_ptr;
		struct dma_buf *dmabuf_ptr;
		struct dma_buf_attachment *dmabufattach;
		int dma_dir;
	};

	struct kiumd_smmu_mmio_ctx *context;	//MMIO details

	struct {		//map type details
		bool is_iova_zero;
		bool is_priv_map;
		bool is_fixed_map;
	};

	struct {		// KGSL details
		bool is_kgsl_map;
		bool is_kgsl_ctx;
		struct kiumd_smmu_kgsl_ctx kgsl_ctx;
		int ptselect;
	};

	u64 size;		//Map res size
	unsigned long iova_rb;  // iova/dma address
	struct hlist_node node;
};

/**
 * struct  hyp_map_data: Structure for hashtable data for hyp assign/unassign
 * @id: id to hyp assign/unassign entries in hashtable
 * @sgt_ptr: sgt pointer value
 * @dmabuf_ptr: dma buf pointer for map operations
 * @dmabufattach: dmabufattach value
 * @node: hlist_node
 * @secure_ctx: secure ctx pointer for strore/retrieve hyp data
 */
struct hyp_map_data {
	unsigned int id;
	struct sg_table *sgt_ptr;
	struct dma_buf *dmabuf_ptr;
	struct dma_buf_attachment *dmabufattach;
	struct kiumd_secure_map_context *secure_ctx;
	struct hlist_node node;
};

struct iommu_domain *kiumd_iommu_get_dma_domain(struct device *dev);

int kiumd_set_pgtble_ttbr0_context(struct iommu_domain *iommu_dom,
				   struct kiumd_ctx *kiumd_ctx);

int kiumd_set_pgtble_ttbr1_context(struct iommu_domain *iommu_dom);

bool check_pgtable_context(struct device *dev, struct pgtable_map *pgtable_ctx);

struct iommu_domain *kiumd_iommu_get_dma_domain(struct device *dev);

unsigned long get_shift_from_dt(struct device *dev);

int kiumd_set_dma_addr_ranges(struct kiumd_ctx *kiumd_ctx, struct device *dev);

void clear_kgsl_map_iova(struct kmem_cache *addr_cache,
			struct kiumd_ctx *kiumd_ctx, struct smmu_map_data *smap);

int set_kgsl_map_iova(struct kmem_cache *addr_cache, struct kiumd_ctx *kiumd_ctx,
				struct kiumd_user kiusr, struct smmu_map_data *smap);

unsigned long get_hash_key(struct device *dev);


unsigned long alloc_iova_range(struct kmem_cache *addr_cache, struct device *dev,
				struct pgtable_map *ptable_ctx, struct smmu_map_data *smap,
				unsigned long max_shift, unsigned long fixed_iova, bool is_fix_map);

int add_smap(struct kiumd_ctx *ctx, struct smmu_map_data *smap);

struct smmu_map_data *remove_smap(struct kiumd_ctx *ctx, int id);

u64 kiumd_get_dmabuf_size(int dmabuf_fd);

bool is_fixed_mapping(struct device *dev);

struct smmu_map_data *allocate_init_smap(struct kiumd_user kiusr,
					 struct device *dev, u64 size);

struct smmu_map_data *allocate_init_smap_mmio(struct  kiumd_smmu_mmio_map kiusr,
					      struct resource *res,
					      struct device *dev);

int kiumd_dmabuf_zero_map(struct smmu_map_data *smap);

int kiumd_dmabuf_priv_map(struct smmu_map_data *smap);

int kiumd_dmabuf_map(struct smmu_map_data *smap);

int kiumd_mmio_map(struct smmu_map_data *smap);

void kiumd_dmabuf_zero_unmap(struct smmu_map_data *smap);

void kiumd_dmabuf_priv_unmap(struct smmu_map_data *smap);

void kiumd_dmabuf_unmap(struct smmu_map_data *smap);

void kiumd_mmio_unmap(struct smmu_map_data *smap);

void free_allocated_iova(struct kmem_cache *addr_cache, struct kiumd_ctx *kiumd_ctx,
			 unsigned long iova);

int init_and_allocate_iova(struct kmem_cache *addr_cache, struct device *dev,
			   struct kiumd_ctx *kiumd_ctx,
			   struct smmu_map_data *smap, unsigned long max_shift,
			   unsigned long fixed_iova, bool is_fixed_map);

int kiumd_configure_dma_cookie(struct device *dev,
			       enum iommu_dma_cookie_type cookie_type,
			       dma_addr_t dma_addr);

bool check_ptselect(struct kiumd_user *kiusr);
struct kiumd_iommu_dma_cookie *kiumd_get_dma_cookie(struct device *dev);

int kiumd_set_dma_cookie_unlocked(struct kiumd_iommu_dma_cookie *cookie,
				  enum iommu_dma_cookie_type type,
				  dma_addr_t iova);

struct arm_smmu_domain *kiumd_get_smmu_domain(struct device *dev);

int kiumd_acl_to_vmid_perms_list(unsigned int nr_acl_entries,
				 const void __user *acl_entries,
				 int **dst_vmids, int **dst_perms);

int kiumd_hyp_assign_sg(struct sg_table *sgt, int *dest_vm_list,
			int dest_nelems, bool set_page_private,
			int *dest_perms);

int kiumd_hyp_unassign_sg(struct sg_table *sgt, int *source_vm_list,
			  int source_nelems, bool clear_page_private);

int kiumd_get_pgd(struct device *dev, u64 *pgd);

int kiumd_io_pgtable_hyp_assign_page(u32 *vmid, u64 page, u32 nr_acl_entries);

int kiumd_io_pgtable_hyp_unassign_page(u32 *vmid, u64 page, u32 nr_acl_entries);

int kiumd_set_dma_cookie(struct kiumd_iommu_dma_cookie *cookie,
			 enum iommu_dma_cookie_type type,
			 dma_addr_t iova);
int set_allocated_iova(struct device *dev, unsigned long iova);

void free_iova_range(struct kmem_cache *addr_cache,
		     struct pgtable_map *map, unsigned long iova);

void clean_map(struct kmem_cache *addr_cache, struct kiumd_ctx *kiumd_ctx,
						struct smmu_map_data *smap);

void release_map_data(struct kiumd_ctx *kiumd_ctx,
					struct smmu_map_data *smap);
int kiumd_iommu_custom_iova_init(struct device *dev);

struct iommu_addr_entry *alloc_iommu_addr_entry(struct kmem_cache *addr_cache,
						unsigned long base_addr,
						unsigned long size);
#endif /* __KIUMD_COMMON_H__ */

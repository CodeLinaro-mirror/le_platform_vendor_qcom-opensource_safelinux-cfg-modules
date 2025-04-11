// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/adreno-smmu-priv.h>
#include <linux/dma-buf.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/hashtable.h>
#include <linux/iommu_iova_map.h>
#include <linux/iova.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <uapi/misc/kiumd.h>
#include <uapi/misc/scm_user_intf.h>

#define CREATE_TRACE_POINTS
#include "safelinux_modules_trace.h"

#if (LINUX_VERSION_CODE != KERNEL_VERSION(5, 14, 0))
#include "arm-smmu/arm-smmu.h"
#else
#include "arm-smmu.h"
#endif
#include "vfio.h"

#ifdef CONFIG_SAFELINUX_KERNEL
#define kiumd_set_dma_max_seg_size(_dev, _size)				\
({									\
	dma_set_max_seg_size(_dev, _size);				\
})
#else
#define kiumd_set_dma_max_seg_size(_dev, _size)				\
({									\
	ret = dma_set_max_seg_size(_dev, _size);			\
	if (ret)							\
		pr_warn("%s: max_segment size not set.\n", __func__);	\
})
#endif

#define IOVA_ZERO	((dma_addr_t)0)
static struct kobject *smmu_obj;
static struct kobject *device_obj;

struct smmu_device_obj {
	struct kobject *kobj;
	int smmu_fsr;
	int smmu_iova;
	int flag;
	struct smmu_device_obj *next;
};

struct smmu_device_obj *head;


/*No.of pages for 6GB IOVA space with 4K page size*/
#define KGSL_PT_MEM_PAGES 0x180000
static DECLARE_BITMAP(global_map, KGSL_PT_MEM_PAGES);
static DECLARE_BITMAP(perprocess_map, KGSL_PT_MEM_PAGES);

#define KGSL_GLOBAL_PT_BASE_IOVA 0xFFFFFF8000000000
#define KGSL_PER_PROCESS_PT_BASE_IOVA 0x60000000
#define KGSL_PER_PROCESS_PT_END_IOVA 0x240000000

#define SMMU_MAPTABLE_SIZE (10)

#define MAX_KIUMD_ACL_ENTRIES 64
#define KIUMD_MAX_VMID        64
#define KIUMD_MAX_PERMS       8

#define KIUMD_MAX_REG_NAME_LEN (100)
#define KIUMD_INDEX_OFFSET     (40)

#define KIUMD_32BIT_START_IOVA 0x1000
#define KIUMD_32BIT_END_IOVA 0xFFFFFFFF

struct kiumd_smmu_mmio_ctx {
	struct device *dev;
	dma_addr_t iova;
	size_t size;
};

struct dmabuf_fd {
	struct dma_buf *kiumd_dmabuf; //Value
	uint32_t token;  //Key
	struct list_head *next;
};

enum iommu_dma_cookie_type {
	IOMMU_DMA_IOVA_COOKIE,
	IOMMU_DMA_MSI_COOKIE,
};

struct dma_buf_handle {
	unsigned long dmabuf;
	atomic_t handle_refcount;
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
struct kiumd_reserved_mem_area {
	size_t size;
	u64    base;
};

/**
 * struct kiumd_kgsl_context: to store GPU context
 * @kgsl_pt_id: pagetable id
 * @kgsl_pt_lock: lock for pagetable add/retrieve
 * @kgsl_start_iova: Starting iova for GPU
 * @kgsl_end_iova Ending iova for GPU
 */

struct kiumd_kgsl_context {
	u32 kgsl_pt_id;
	spinlock_t kgsl_pt_lock;
	unsigned long kgsl_start_iova;
	unsigned long kgsl_end_iova;
};

/**
 * struct pgtable_map: to store each pagetable iova details for GPU
 * @idx: pagetable id for each GPU process
 * @start_iova: Starting iova for GPU
 * @end_iova: Ending iova for GPU
 * @rbtree: Red-black tree root for managing iova ranges
 * @node: Node for linking in a hash list
 * @ttbr0_addr: TTBR0 address for GPU
 * @pgtbl_ops_ptr: Pointer to pagetable operations for GPU
 * @last_allocated_end: Last allocated end address for GPU
 * @is_contiguous: Flag indicating if the iova range is contiguous
 */
struct pgtable_map {
	unsigned long idx;
	unsigned long start_iova;
	unsigned long end_iova;
	struct rb_root rbtree;
	struct hlist_node node;
//Used only by GPU
	unsigned long ttbr0_addr;
	unsigned long pgtbl_ops_ptr;
	unsigned long last_allocated_end;
	bool is_contiguous;
};


/**
 * struct kiumd_ctx: Structure for kiumd_ctx .
 * @id: id to map/unmap entries in hashtable
 * @smmu_map_data: structurefor hashtable data
 * @smmu_lock: Lock for map/unmap operations
 * @hyp_lock: Lock for hyp assign operations
 * @smmu_table: Hashtable to hold entries based on id
 * @reserved_mem_area: pointer to hold reserved memory area
 * @hyp_idx: id for hyp_map_data node entries in hashtable
 * @num_reserved_regions : number of reserved memory areas
 *
 */

struct kiumd_ctx {
	int id;
	u32 pt_id;
	bool is_initialized;
	struct hlist_node smmu_map_data;
	struct hlist_node pgtable_map;
	unsigned int hyp_idx;
	struct hlist_node hyp_map_data;
	spinlock_t smmu_lock;
	spinlock_t pt_lock;
	DECLARE_HASHTABLE(smmu_table, SMMU_MAPTABLE_SIZE);
	DECLARE_HASHTABLE(hyp_table, SMMU_MAPTABLE_SIZE);
	struct kiumd_reserved_mem_area *res_mem_area;
	int num_reserved_regions;
	struct xarray kiumd_xa;
	struct mutex kiumd_xa_mutex;
	struct mutex hyp_lock;
	unsigned long pt_start_iova;
	unsigned long pt_end_iova;
	DECLARE_HASHTABLE(page_table, SMMU_MAPTABLE_SIZE);
	DECLARE_HASHTABLE(kgsl_page_table, SMMU_MAPTABLE_SIZE);
	struct kiumd_kgsl_context *kgsl_context;
	unsigned long max_shift;
	struct io_pgtable *pgtable;
};

struct iommu_addr_entry {
	unsigned long base_addr;
	unsigned long size;
	struct rb_node rbnode;
};

static struct kmem_cache *iommu_addr_cache;

/**
 * struct  smmu_map_data: Structure for hashtable data .
 * @id: id to map/unmap entries in hashtable
 * @sgt_ptr: sgt pointer value
 * @dmabuf_ptr: dma buf pointer for map operations
 * @dmabufattach: dmabufattach value
 * @node: hlist_node
 *
 */

struct smmu_map_data {
	int id;
	long sgt_ptr;
	long dmabuf_ptr;
	long dmabufattach;
	int dma_dir;
	int ptselect;
	int is_iova_zero;
	bool is_fixed_map;
	struct vfio_device *vfio_dev;
	void *context;
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
	long sgt_ptr;
	long dmabuf_ptr;
	long dmabufattach;
	struct kiumd_secure_map_context *secure_ctx;
	struct hlist_node node;
};

/**
 * @Brief: This function find the
 * iommu group for given vfio device
 * and then return default iommu
 * domain for that group.
 *
 * Parameters:
 * @dev: vfio device
 *
 * Returns struct iommu_domain * upon success
 * and NULL on failure
 */
struct iommu_domain *kiumd_iommu_get_dma_domain(struct device *dev)
{
	struct kiumd_iommu_group *iommu_group;

	iommu_group = (struct kiumd_iommu_group *) dev->iommu_group;
	if (!iommu_group) {
		dev_err(dev, "%s:iommu group is invalid\n", __func__);
		return NULL;
	}

	return iommu_group->default_domain;
}

/**
 * @Brief: This function provide the vfio device
 * pointer for the given file descriptor
 *
 * Parameters:
 * @fd: file descriptor
 *
 * Returns vfio device * upon success and NULL
 * on failure
 */
static struct vfio_device *kiumd_get_vfio_device(int fd)
{
	struct vfio_device *vfio_dev = NULL;
	struct vfio_device_file *df;
	struct file *file;

	if (fd < 0)
		return NULL;

	file = fget(fd);
	if (!file)
		return NULL;

	if (!vfio_file_is_valid(file))
		goto close_file;

	df = (struct vfio_device_file *)file->private_data;
	if (!df)
		goto close_file;

	vfio_dev = (struct vfio_device *)df->device;

close_file:
	fput(file);
	return vfio_dev;
}

/**
 * @Brief: This function provide the dma cookie
 * for given vfio device
 *
 * Parameters:
 * @vfio_dev: vfio device
 *
 * Returns  struct kiumd_iommu_dma_cookie
 * *(cookie) upon success and NULL on failure
 */
static struct kiumd_iommu_dma_cookie *kiumd_get_dma_cookie(struct vfio_device
							   *vfio_dev)
{
	struct kiumd_iommu_dma_cookie *cookie;
	struct iommu_domain *domain;

	if (!vfio_dev) {
		pr_err("%s:vfio dev is NULL\n", __func__);
		return NULL;
	}

	if (!vfio_dev->dev) {
		pr_err("%s:vfio dev is NULL\n", __func__);
		return NULL;
	}

	domain = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!domain) {
		pr_err("%s:iommu domain is NULL for %s\n", __func__, dev_name(vfio_dev->dev));
		return NULL;
	}

	cookie = (struct kiumd_iommu_dma_cookie *)domain->iova_cookie;
	if (!cookie) {
		pr_err("%s:cookie not found\n", __func__);
		return NULL;
	}

	return cookie;
}

/**
 * @Brief: This function set dma cookie type and iova to given
 * cookie type and iova.
 *
 * Parameters:
 * @*cookie: iommu dma cookie
 * @type: iommu_dma_cookie_type
 * @iova: iova address
 *
 * Returns  0 upon success and -EINVAL on failure
 */
static int kiumd_set_dma_cookie(struct kiumd_iommu_dma_cookie *cookie,
				enum iommu_dma_cookie_type type,
				dma_addr_t iova)
{
	if (!cookie) {
		pr_err("%s:Unable to set cookie\n", __func__);
		return -EINVAL;
	}

	cookie->type = type;
	cookie->msi_iova = iova;

	return 0;
}

/**
 * @Brief: This function call the api to set dma cookie
 *
 * Parameters:
 * @*cookie: iommu dma cookie
 * @type: iommu_dma_cookie_type
 * @iova: iova address
 *
 * Returns  0 upon success and -EINVAL on failure
 */
static int kiumd_set_dma_cookie_unlocked(struct kiumd_iommu_dma_cookie *cookie,
					 enum iommu_dma_cookie_type type,
					 dma_addr_t iova)
{
	int ret;

	if (!cookie) {
		pr_err("%s:Unable to set cookie\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&cookie->mutex);
	ret = kiumd_set_dma_cookie(cookie, type, iova);
	mutex_unlock(&cookie->mutex);
	return ret;
}

/**
 * @Brief: This function facilitates to
 * get the iommu domain for given vfio device
 *
 * Parameters:
 * @vfio_fd: file descriptor for vfio device
 *
 * Returns  iommu_dom upon success and NULL
 * on failure
 */
struct iommu_domain *kiumd_get_iommu_domain(int vfio_fd)
{
	struct vfio_device *vfio_dev;
	struct iommu_domain *domain;

	vfio_dev = kiumd_get_vfio_device(vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		return NULL;
	}
	domain = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!domain) {
		pr_err("%s:iommu domain is NULL\n", __func__);
		return NULL;
	}

	return domain;
}

/**
 * @Brief: This function facilitates to
 * set the iommu domain
 *
 * Parameters:
 * @vfio_fd: file descriptor for vfio device
 *
 * Returns  iommu_dom upon success and NULL
 * on failure
 */
struct arm_smmu_domain *kiumd_get_smmu_domain(int vfio_fd)
{
	struct arm_smmu_domain *smmu_domain;
	struct iommu_domain *iommu_dom;

	iommu_dom = kiumd_get_iommu_domain(vfio_fd);
	if (!iommu_dom) {
		pr_err("%s:IOMMU domain is NULL\n", __func__);
		return NULL;
	}
	smmu_domain = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_domain) {
		pr_err("%s:SMMU domain is NULL\n", __func__);
		return NULL;
	}

	return smmu_domain;
}

/**
 * kiumd_get_pgtable_entry - Retrieve a pagetable entry from the context
 * @kiumd_ctx: Pointer to the kiumd context
 * @idx: Index of the pagetable entry to retrieve
 * @is_process: Flag indicating if the context is process-specific
 *
 * This function searches for a pagetable entry in the given context's
 * hash table based on the provided index. If the context is process-specific,
 * it locks the process-specific pagetable lock, searches the hash table, and
 * unlocks the lock. If the context is not process-specific, it performs the
 * same operations on the general pagetable lock and hash table.
 *
 * Return: Pointer to the found pagetable entry, or NULL if not found.
 */
struct pgtable_map *kiumd_get_pgtable_entry(struct kiumd_ctx *kiumd_ctx,
					    unsigned long idx, bool is_process)
{
	struct kiumd_kgsl_context *kgsl_context;
	struct pgtable_map *pgtble_ctx;
	bool found = false;

	if (is_process) {
		kgsl_context = kiumd_ctx->kgsl_context;
		spin_lock(&kgsl_context->kgsl_pt_lock);
		hash_for_each_possible(kiumd_ctx->kgsl_page_table, pgtble_ctx, node, idx) {
			if (pgtble_ctx->idx == idx) {
				found = true;
				break;
			}
		}

		spin_unlock(&kgsl_context->kgsl_pt_lock);
		if (!found) {
			pr_err("%s:%d id not found in hash table\n", __func__, __LINE__);
			return NULL;
		}
	} else {
		spin_lock(&kiumd_ctx->pt_lock);
		hash_for_each_possible(kiumd_ctx->page_table, pgtble_ctx, node, idx) {
			if (pgtble_ctx->idx == idx) {
				found = true;
				break;
			}
		}

		spin_unlock(&kiumd_ctx->pt_lock);
		if (!found) {
			pr_err("%s:%d id not found in hash table\n", __func__, __LINE__);
			return NULL;
		}
	}

	return pgtble_ctx;
}

static void free_iommu_addr_entry(struct iommu_addr_entry *entry)
{
	kmem_cache_free(iommu_addr_cache, entry);
}

static int free_iova_range(struct pgtable_map *map, unsigned long iova)
{
	struct rb_node *node = map->rbtree.rb_node;
	struct iommu_addr_entry *entry;

	while (node) {
		entry = rb_entry(node, struct iommu_addr_entry, rbnode);

		if (iova < entry->base_addr)
			node = node->rb_left;
		else if (iova > entry->base_addr)
			node = node->rb_right;
		else {
			rb_erase(&entry->rbnode, &map->rbtree);
			free_iommu_addr_entry(entry);
			return 0;
		}
	}

	return -EINVAL; // IOVA not found
}

/**
 * free_allocated_iova - Free an allocated IOVA range
 * @kiumd_ctx: Pointer to the kiumd context
 * @iova: IOVA address to free
 * @idx: Index of the pagetable entry
 * @is_process: Flag indicating if the context is process-specific
 *
 * This function retrieves a pagetable entry from the kiumd context based on the
 * provided index and process flag. If the pagetable entry is found, it frees the
 * specified IOVA range. If the pagetable entry is not found, it logs an error and
 * returns -EINVAL.
 *
 * Return: 0 on success, -EINVAL if the pagetable entry is not found.
 */
int free_allocated_iova(struct kiumd_ctx *kiumd_ctx, unsigned long iova,
			unsigned long idx, bool is_process)
{
	struct pgtable_map *pgtble_ctx;

	pgtble_ctx = kiumd_get_pgtable_entry(kiumd_ctx, idx, is_process);
	if (!pgtble_ctx) {
		pr_err("%s:invalid id for hash table: id: %d\n", __func__, idx);
		return -EINVAL;
	}

	return free_iova_range(pgtble_ctx, iova);
}

static struct iommu_addr_entry *alloc_iommu_addr_entry(unsigned long base_addr,
						       unsigned long size)
{
	struct iommu_addr_entry *entry = kmem_cache_alloc(iommu_addr_cache, GFP_KERNEL);

	if (!entry) {
		pr_err("%s:%d failed to create entry for addr: %lx, size: %lu)\n",
		       __func__, __LINE__, base_addr, size);
		return NULL;
	}

	entry->base_addr = base_addr;
	entry->size = size;
	return entry;
}

static int insert_iova(struct pgtable_map *pgtable_ctx,
		       struct iommu_addr_entry *entry)
{
	struct rb_node **new = &(pgtable_ctx->rbtree.rb_node);
	struct rb_node *parent = NULL;

	while (*new) {
		struct iommu_addr_entry *this = rb_entry(*new, struct iommu_addr_entry, rbnode);

		parent = *new;

		if (entry->base_addr < this->base_addr)
			new = &((*new)->rb_left);
		else if (entry->base_addr > this->base_addr)
			new = &((*new)->rb_right);
		else
			return -EEXIST;
	}

	rb_link_node(&entry->rbnode, parent, new);
	rb_insert_color(&entry->rbnode, &pgtable_ctx->rbtree);

	return 0;
}

int kiumd_configure_dma_cookie(struct vfio_device *vfio_dev,
			       enum iommu_dma_cookie_type cookie_type,
			       dma_addr_t dma_addr)
{
	struct kiumd_iommu_dma_cookie *cookie;
	int ret;

	cookie = kiumd_get_dma_cookie(vfio_dev);
	if (!cookie) {
		pr_err("%s failed to get cookie\n", __func__);
		return -EINVAL;
	}

	ret = kiumd_set_dma_cookie_unlocked(cookie, cookie_type, dma_addr);
	if (ret) {
		pr_err("%s %d failed to set cookie\n", __func__, __LINE__);
		return -EINVAL;
	}

	return 0;
}

struct iova_domain *kiumd_get_iova_domain(struct device *dev)
{
	struct kiumd_iommu_dma_cookie *cookie;
	struct iommu_domain *domain;
	struct iova_domain *iovad;

	domain = kiumd_iommu_get_dma_domain(dev);
	if (!domain) {
		pr_err("%s:dma_domain is invalid\n", __func__);
		return -EINVAL;
	}

	cookie = (struct kiumd_iommu_dma_cookie *)domain->iova_cookie;
	iovad = &cookie->iovad;

	return iovad;
}

unsigned long get_shift_from_dt(struct device *dev)
{
	struct device_node *node;
	u32 shift = 0;

	node = dev->of_node;
	if (!of_property_read_u32(node, "qcom,iova-align-shift-max", &shift))
		return shift;

	return 0;
}


static unsigned long limit_align_shift(struct device *dev, unsigned long shift,
				       unsigned long max_shift)
{
	unsigned long max_align_shift, final_shift;
	struct iova_domain *iovad;

	iovad = kiumd_get_iova_domain(dev);
	max_align_shift = max_shift + PAGE_SHIFT - iova_shift(iovad);

	final_shift = min_t(unsigned long, max_align_shift, shift);

	return final_shift;
}

static unsigned long align_iova(struct device *dev, unsigned long start_iova,
				unsigned long size, unsigned long max_shift)
{
	unsigned long align_mask, final_shift;

	final_shift = limit_align_shift(dev, fls_long(size-1), max_shift);
	align_mask = (1UL << final_shift) - 1;

	if (start_iova % size != 0)
		start_iova = (start_iova + align_mask) & ~align_mask;

	return start_iova;
}

static unsigned long alloc_iova_range(struct vfio_device *vfio_dev,
				      struct pgtable_map *ptable_ctx,
				      unsigned long size,
				      unsigned long max_shift)
{
	struct rb_node *node = rb_first(&ptable_ctx->rbtree);
	unsigned long start_iova = ptable_ctx->start_iova;
	struct iommu_addr_entry *new_entry;
	struct iommu_addr_entry *entry;

	start_iova = align_iova(vfio_dev->dev, start_iova, size, max_shift);

	while (node) {
		entry = rb_entry(node, struct iommu_addr_entry, rbnode);

		if (start_iova + size < entry->base_addr) {
			if (start_iova + size <= ptable_ctx->end_iova) {
				new_entry = alloc_iommu_addr_entry(start_iova, size);

				if (!new_entry)
					return 0;

				insert_iova(ptable_ctx, new_entry);
				return start_iova;
			}

			return 0;
		}

		start_iova = entry->base_addr + entry->size;
		start_iova = align_iova(vfio_dev->dev, start_iova, size, max_shift);
		node = rb_next(node);
	}

	if (start_iova + size <= ptable_ctx->end_iova) {
		new_entry = alloc_iommu_addr_entry(start_iova, size);
		if (!new_entry)
			return 0;

		insert_iova(ptable_ctx, new_entry);
		return start_iova;
	}

	return 0;
}

/**
 * check_pgtable_context - Validate the pagetable context.
 * @vfio_fd: File descriptor for the VFIO device
 * @pgtable_ctx: Pointer to the pagetable context to validate
 *
 * This function retrieves the SMMU domain associated with the given VFIO file
 * descriptor and checks if the pagetable context matches the TTBR0 address
 * in the SMMU domain's pagetable configuration. If the SMMU domain or pagetable
 * is invalid, or if there is a mismatch in the TTBR0 address, the function
 * logs an error and returns false.
 *
 * Return: true if the pagetable context is valid, false otherwise.
 */
bool check_pgtable_context(int vfio_fd, struct pgtable_map *pgtable_ctx)
{
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable *pgtable;
	unsigned long ttbr0;

	smmu_dom = kiumd_get_smmu_domain(vfio_fd);
	if (!smmu_dom) {
		pr_err("%s:%d invalid smmu_dom\n", __func__, __LINE__);
		return false;
	}

	pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if (!pgtable) {
		pr_err("%s:%d invalid pagetable\n", __func__, __LINE__);
		return false;
	}

	ttbr0 = pgtable->cfg.arm_lpae_s1_cfg.ttbr;

	if (ttbr0 != pgtable_ctx->ttbr0_addr) {
		pr_err("%s:%d mismatch pt_id and pgtable context, probably wrong id from UMD\n",
		       __func__, __LINE__);
		return false;
	}

	return true;
}

/**
 * Brief: This function write smmu context bank for given
 * index bank register for given context bank.
 *
 * Parameters:
 * @*smmu: pointer for arm smmu device information
 * @idx: index for context bank
 *
 * Returns void
 */
static void kiumd_smmuv2_write_context_bank(struct arm_smmu_device *smmu,
					    int idx)
{
	struct arm_smmu_cb *cb = &smmu->cbs[idx];
	struct arm_smmu_cfg *cfg = cb->cfg;
	bool stage1;
	u32 reg;

	trace_kiumd_smmuv2_write_context_bank_start(idx, cfg->cbndx, cfg->cbar);

	stage1 = cfg->cbar != CBAR_TYPE_S2_TRANS;

	if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64)
		reg = ARM_SMMU_CBA2R_VA64;
	else
		reg = 0;

	arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBA2R(idx), reg);
	reg = FIELD_PREP(ARM_SMMU_CBAR_TYPE, cfg->cbar);

	if (stage1) {
		reg |= FIELD_PREP(ARM_SMMU_CBAR_S1_BPSHCFG,
			ARM_SMMU_CBAR_S1_BPSHCFG_NSH) |
			FIELD_PREP(ARM_SMMU_CBAR_S1_MEMATTR,
				ARM_SMMU_CBAR_S1_MEMATTR_WB);
	} else if (!(smmu->features & ARM_SMMU_FEAT_VMID16)) {
		/* 8-bit VMIDs live in CBAR */
		reg |= FIELD_PREP(ARM_SMMU_CBAR_VMID, cfg->vmid);
	}

	arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBAR(idx), reg);

	if (stage1)
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TCR2, cb->tcr[1]);

	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TCR, cb->tcr[0]);

	arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_TTBR0, cb->ttbr[0]);

	if (stage1)
		arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_TTBR1, cb->ttbr[1]);

	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_S1_MAIR0, cb->mair[0]);
	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_S1_MAIR1, cb->mair[1]);

	reg = ARM_SMMU_SCTLR_CFIE | ARM_SMMU_SCTLR_CFRE | ARM_SMMU_SCTLR_AFE |
	      ARM_SMMU_SCTLR_TRE | ARM_SMMU_SCTLR_M;

	reg |= ARM_SMMU_SCTLR_S1_ASIDPNE;

	smmu->impl->write_sctlr(smmu, idx, reg);

	trace_kiumd_smmuv2_write_context_bank_end(idx);
}

/**
 * kiumd_smmuv2_set_ttbr0cfg - Configure TTBR0 settings for the ARM SMMU
 * for a specific vfio device(as of now used by GPU)
 * @smmu_domain: Pointer to the SMMU domain structure
 * @pgtbl_cfg: Pointer to the page table configuration
 *
 * This function enables TTBR0 translation in the SMMU and updates the
 * registers for efficient address translation.
 *
 * Return: 0 on success, negative error code on failure
 */
static int kiumd_smmuv2_set_ttbr0_cfg(struct arm_smmu_domain *smmu_domain,
				      const struct io_pgtable_cfg *pgtbl_cfg)
{

	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_cb *cb = &smmu_domain->smmu->cbs[cfg->cbndx];
	u32 tcr = cb->tcr[0];

	if (!(cb->tcr[0] & ARM_SMMU_TCR_EPD0)) {
		pr_err("TTBR0 translation is already enabled");
		return -EINVAL;
	}

	tcr |= arm_smmu_lpae_tcr(pgtbl_cfg);
	tcr &= ~(ARM_SMMU_TCR_EPD0 | ARM_SMMU_TCR_EPD1);

	cb->tcr[0] = tcr;
	cb->ttbr[0] = pgtbl_cfg->arm_lpae_s1_cfg.ttbr;
	cb->ttbr[0] |= FIELD_PREP(ARM_SMMU_TTBRn_ASID, cb->cfg->asid);

	kiumd_smmuv2_write_context_bank(smmu_domain->smmu, cb->cfg->cbndx);

	return 0;
}

/**
 * kiumd_smmuv2_set_ttbr1_cfg - Configure TTBR1 settings for the ARM SMMU
 * for a specific vfio device(as of now used by LPAC).
 * @smmu_domain: Pointer to the SMMU domain structure
 * @pgtbl_cfg: Pointer to the page table configuration
 *
 * This function enables TTBR1 translation in the SMMU and updates the
 * registers for efficient address translation.
 *
 * Return: 0 on success, negative error code on failure
 */

static void kiumd_smmuv2_set_ttbr1_cfg(struct arm_smmu_domain *smmu_domain,
				      const struct io_pgtable_cfg *pgtbl_cfg)
{
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_cb *cb = &smmu_domain->smmu->cbs[cfg->cbndx];
	u32 tcr = cb->tcr[0];

	tcr |= arm_smmu_lpae_tcr(pgtbl_cfg);
	tcr &= ~(ARM_SMMU_TCR_EPD0 | ARM_SMMU_TCR_EPD1);

	cb->tcr[0] = tcr;
	cb->ttbr[1] = pgtbl_cfg->arm_lpae_s1_cfg.ttbr;
	cb->ttbr[1] |= FIELD_PREP(ARM_SMMU_TTBRn_ASID, cb->cfg->asid);

	kiumd_smmuv2_write_context_bank(smmu_domain->smmu, cb->cfg->cbndx);

	return;
}


/**
 * kiumd_perprocess_set_ttbr1_context - Configure TTTBR1 settings for the
 * ARM SMMU for a specific vfio device(as of now used by LPAC).
 * @arg: User-provided argument pointer
 *
 * This function allocates a pagetable and invokes the function to program
 * TTBR1 for the specified VFIO device's SMMU domain. It also configures
 * the aperture for the specified vfio device.
 *
 * Return: 0 on success, negative error code on failure
 */

static int kiumd_set_pgtble_ttbr1_context(struct iommu_domain *iommu_dom)
{
	struct io_pgtable_ops *pgtable_ops;
	struct arm_smmu_domain *smmu_dom;
	struct arm_smmu_cfg *smmu_cfg;
	struct io_pgtable *pagetable;
	struct io_pgtable_cfg cfg;
	struct arm_smmu_cb *cb;

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_dom || !smmu_dom->pgtbl_ops) {
		pr_err("%s: smmu domain/pagetable ops is invalid\n", __func__);
		return -EINVAL;
	}

	smmu_cfg = &smmu_dom->cfg;
	cb = &smmu_dom->smmu->cbs[smmu_cfg->cbndx];
	if (!(cb->tcr[0] & ARM_SMMU_TCR_EPD1)) {
		/** Not an error. During kgsl relaunch, we dont have to reprogram
		*  TTBR1 if its already enabled.*/
		pr_err("%s: TTBR1 is already enabled for the device\n", __func__);
		return 0;
	}

	pagetable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if (!pagetable) {
		pr_err("%s: pagetable is NULL\n", __func__);
		return -EINVAL;
	}

	memcpy(&cfg, &pagetable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks |= IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;

	if (cfg.quirks & IO_PGTABLE_QUIRK_ARM_TTBR1) {
		iommu_dom->geometry.aperture_start = ~0UL << cfg.ias;
		iommu_dom->geometry.aperture_end = ~0UL;
	}

	pgtable_ops = alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!pgtable_ops) {
		pr_err("%s: failed to allocate pagetable ops\n", __func__);
		return -EINVAL;
	}

	kiumd_smmuv2_set_ttbr1_cfg(smmu_dom, &cfg);
	smmu_dom->pgtbl_ops = pgtable_ops;

	return 0;
}

/**
 * kiumd_perprocess_set_ttbr1_context - Configure TTTBR0 settings
 * for a specific vfio device(as of now used by GPU)
 * ARM SMMU for the specified device.
 * @arg: User-provided argument pointer
 *
 * This function allocates a pagetable and invokes the function to program
 * TTBR0 for the specified VFIO device's SMMU domain. It also configures
 * the aperture for the specified vfio device through an scm call.
 *
 * Return: 0 on success, negative error code on failure
 */
static int kiumd_set_pgtble_ttbr0_context(struct iommu_domain *iommu_dom,
					  struct kiumd_ctx *kiumd_ctx)
{
	struct io_pgtable_ops *pgtable_ops;
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable *pgtable;
	struct io_pgtable_cfg cfg;
	int ret;

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if ((!smmu_dom) || (!(smmu_dom->pgtbl_ops))) {
		pr_err("%s:smmu domain/pagetable ops is invalid\n", __func__);
		return -EINVAL;
	}

	if (!kiumd_ctx->pgtable) {
		kiumd_ctx->pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
		if (!kiumd_ctx->pgtable) {
			pr_err("%s:pagetable is NULL\n", __func__);
			return -EINVAL;
		}
	}

	pgtable = kiumd_ctx->pgtable;
	if (!pgtable) {
		pr_err("%s: pgtable is null\n", __func__);
		return -EINVAL;
	}

	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	/*Allocate a default pagetable for TTBR0 in case per process allocation fails*/
	pgtable_ops = alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!pgtable_ops) {
		pr_err("%s:failed to allocate pagetable ops.\n", __func__);
		return -ENOMEM;
	}

	kiumd_smmuv2_set_ttbr0_cfg(smmu_dom, &cfg);
	ret = qcom_scm_kgsl_set_smmu_aperture(smmu_dom->cfg.cbndx);
	if (ret == -EBUSY)
		ret = qcom_scm_kgsl_set_smmu_aperture(smmu_dom->cfg.cbndx);

	if (ret) {
		pr_err("%s:Setting smmu aperture error: %d\n", __func__, ret);
		free_io_pgtable_ops(pgtable_ops);
		return ret;
	}

	return 0;
}

/**
 * kiumd_set_pgtbl_context - Set the page table context for an SMMU device.
 * @arg: User-provided pointer to a struct kiumd_user containing context information
 *
 * This function sets the page table context for an SMMU device based on user-provided
 * information. It validates the VFIO file descriptor, retrieves the VFIO device,
 * and obtains the IOMMU domain. Depending on the context flags, it configures the
 * appropriate page table settings.
 *
 * Return:
 *   0 on success, negative error code on failure.
 */
static int kiumd_set_pgtbl_context(char __user *arg, struct file *fp)
{
	struct iommu_domain *iommu_dom;
	struct vfio_device *vfio_dev;
	struct kiumd_user pgtbl_ctx;
	struct kiumd_ctx *kiumd_ctx;

	int ret;

	if (copy_from_user(&pgtbl_ctx, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	vfio_dev = kiumd_get_vfio_device(pgtbl_ctx.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s: vfio_dev is NULL\n", __func__);
		return -EINVAL;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s: iommu domain is NULL\n", __func__);
		return -EINVAL;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	switch (pgtbl_ctx.flags) {
	case KIUMD_SMMU_SET_TTBR0_CONFIG:
		ret = kiumd_set_pgtble_ttbr0_context(iommu_dom, kiumd_ctx);
		break;
	case KIUMD_SMMU_SET_TTBR1_CONFIG:
		ret = kiumd_set_pgtble_ttbr1_context(iommu_dom);
		break;
	default:
		pr_err("%s: Invalid flags: %d\n", __func__, pgtbl_ctx.flags);
		ret = -ENOTTY;
		break;
	}

	return ret;
}

/**
 * @Brief: This function call the api to provides
 * per process page table allocation
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Returns  0 upon success and -EINVAL on failure
 */
int kiumd_perprocess_pt_alloc(char __user *arg, struct file *fp)
{
	struct kiumd_kgsl_context *kgsl_context;
	struct arm_smmu_domain *smmu_dom;
	struct iommu_domain *iommu_dom;
	struct pgtable_map *pgtbl_ctx;
	struct vfio_device *vfio_dev;
	struct kiumd_ctx *kiumd_ctx;
	struct vfio_device_file *df;
	struct io_pgtable *pgtable;
	struct io_pgtable_cfg cfg;
	struct kiumd_user kiusr;

	trace_kiumd_perprocess_pt_alloc_start(kiusr.vfio_fd);


	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	vfio_dev = kiumd_get_vfio_device(kiusr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:%d vfio_dev is NULL\n", __func__, __LINE__);
		return -EINVAL;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:%d iommu domain is NULL\n", __func__, __LINE__);
		return -EINVAL;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if ((!smmu_dom) || (!(smmu_dom->pgtbl_ops))) {
		pr_err("%s:%d smmu domain/pagetable ops is invalid\n", __func__, __LINE__);
		return -EINVAL;
	}

	pgtable = kiumd_ctx->pgtable;
	if (!pgtable) {
		pr_err("%s: pgtable is null\n", __func__);
		return -EINVAL;
	}

	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	kiusr.asid = smmu_dom->cfg.asid;
	kiusr.pgtbl_ops_ptr = (long)alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if (!(kiusr.pgtbl_ops_ptr)) {
		pr_err("%s:%d failed to allocate pagetable ops\n", __func__, __LINE__);
		return -EINVAL;
	}

	kiusr.ttbr0 = cfg.arm_lpae_s1_cfg.ttbr;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	kgsl_context = kiumd_ctx->kgsl_context;
	if (kgsl_context->kgsl_pt_id == UINT_MAX) {
		pr_err("%s:%d integer overflow in pt_id.\n", __func__, __LINE__);
		return -EINVAL;
	}

	pgtbl_ctx = kzalloc(sizeof(*pgtbl_ctx), GFP_KERNEL);
	if (!pgtbl_ctx)
		return -EINVAL;

	pgtbl_ctx->rbtree = RB_ROOT;
	pgtbl_ctx->ttbr0_addr = kiusr.ttbr0;
	pgtbl_ctx->start_iova = kiumd_ctx->kgsl_context->kgsl_start_iova;
	pgtbl_ctx->end_iova = kiumd_ctx->kgsl_context->kgsl_end_iova;
	pgtbl_ctx->pgtbl_ops_ptr = kiusr.pgtbl_ops_ptr;
	pgtbl_ctx->last_allocated_end = 0;
	pgtbl_ctx->is_contiguous = true;
	spin_lock(&kgsl_context->kgsl_pt_lock);
	pgtbl_ctx->idx = kgsl_context->kgsl_pt_id++;
	hash_add(kiumd_ctx->kgsl_page_table, &pgtbl_ctx->node, pgtbl_ctx->idx);
	spin_unlock(&kgsl_context->kgsl_pt_lock);

	kiusr.pt_id = pgtbl_ctx->idx;

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s:%d copy_to_user failed...\n", __func__, __LINE__);
		return -EFAULT;
	}
	trace_kiumd_perprocess_pt_alloc_end(kiusr.vfio_fd, kiusr.pgtbl_ops_ptr,
					    kiusr.ttbr0, kiusr.asid);

	return 0;
}

/**
 * @Brief: This function call the api to
 * provides global page table allocation
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Returns  0 upon success and error codes
 * on failure
 */
int kiumd_global_pgtble_set(char __user *arg, struct file *fp)
{

	struct io_pgtable_ops *ki_pgtbl_ops;
	struct arm_smmu_domain *smmu_dom;
	struct kiumd_user kismmu_pproc;
	struct iommu_domain *iommu_dom;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct kiumd_ctx *kiumd_ctx;
	struct io_pgtable *pgtable;
	struct file *file;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	trace_kiumd_global_pgtble_set_start(kismmu_pproc.vfio_fd);

	if (kismmu_pproc.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kismmu_pproc.vfio_fd);
	if (!file) {
		pr_err("%s:failed to get file from vfio fd\n", __func__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		fput(file);
		return -ENOTTY;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:IOMMU domain is NULL\n", __func__);
		fput(file);
		return -ENOMEM;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_dom) {
		pr_err("%s:SMMU domain is NULL\n", __func__);
		fput(file);
		return -ENOMEM;
	}

	if (!kiumd_ctx->pgtable) {
		kiumd_ctx->pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
		if (!kiumd_ctx->pgtable) {
			pr_err("%s:pagetable is NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}
	}

	pgtable = kiumd_ctx->pgtable;
	ki_pgtbl_ops = (struct io_pgtable_ops *) (&pgtable->ops);
	if (!ki_pgtbl_ops) {
		pr_err("%s:pagetable ops is NULL\n", __func__);
		fput(file);
		return -ENOMEM;
	}

	smmu_dom->pgtbl_ops = ki_pgtbl_ops;
	fput(file);

	trace_kiumd_global_pgtble_set_end(kismmu_pproc.vfio_fd);

	return 0;
}

/**
 * @Brief: This function call the api to set
 * the per process page table ops
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Returns  0 upon success and error codes
 * on failure
 */
int kiumd_perprocess_pgtble_set(char __user *arg)
{
	struct io_pgtable_ops *ki_pgtbl_ops;
	struct arm_smmu_domain *smmu_dom;
	struct kiumd_user kismmu_pproc;
	struct iommu_domain *iommu_dom;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct file *file;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	trace_kiumd_perprocess_pgtble_set_start(kismmu_pproc.vfio_fd,
						kismmu_pproc.pgtbl_ops_ptr,
						kismmu_pproc.ttbr0,
						kismmu_pproc.asid);

	if (kismmu_pproc.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kismmu_pproc.vfio_fd);
	if (!file) {
		pr_err("%s:failed to get file from vfio fd\n", __func__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:IOMMU domain is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_dom) {
		pr_err("%s:SMMU domain is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	ki_pgtbl_ops = (struct io_pgtable_ops *)kismmu_pproc.pgtbl_ops_ptr;
	if (!ki_pgtbl_ops) {
		pr_err("%s:pagetable ops is NULL\n", __func__);
		return -EINVAL;
	}

	smmu_dom->pgtbl_ops = ki_pgtbl_ops;
	fput(file);

	trace_kiumd_perprocess_pgtble_set_end(kismmu_pproc.vfio_fd);

	return 0;
}

/**
 * @Brief: This function call the api to free
 * the per process page table ops
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Returns  0 upon success and error codes
 * on failure
 */
int kiumd_perprocess_pgtble_free(char __user *arg, struct file *fp)
{
	struct kiumd_user kiusr;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct io_pgtable_ops *pgtable_ops;
	struct pgtable_map *pgtble_ctx;
	struct kiumd_ctx *kiumd_ctx;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	trace_kiumd_perprocess_pgtble_free_start(kiusr.vfio_fd,
						 kiusr.pgtbl_ops_ptr,
						 kiusr.ttbr0, kiusr.asid);

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	vfio_dev = kiumd_get_vfio_device(kiusr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:%d vfio_dev is NULL\n", __func__, __LINE__);
		return -EINVAL;
	}

	pgtble_ctx = kiumd_get_pgtable_entry(kiumd_ctx, kiusr.pt_id, true);
	if (!pgtble_ctx) {
		pr_err("%s:%d Invalid id for hash table: id: %d\n",
		       __func__, __LINE__, kiusr.pt_id);
		return -EINVAL;
	}

	if (!check_pgtable_context(kiusr.vfio_fd, pgtble_ctx)) {
		pr_err("%s:%d check_pgtable_context failed\n", __func__, __LINE__);
		return -EINVAL;
	}

	/*
	 * TODO: unmap the remaining iovas and buffers in rbtree, if any,
	 * for that ttbr should be pointing to this pagetable
	 */
	pgtable_ops = (struct io_pgtable_ops *)pgtble_ctx->pgtbl_ops_ptr;
	if (!pgtable_ops) {
		pr_err("%s:%d pagegetable ops is NULL\n", __func__, __LINE__);
		return -EINVAL;
	}

	free_io_pgtable_ops(pgtable_ops);

	trace_kiumd_perprocess_pgtble_free_end(kiusr.vfio_fd);

	return 0;
}

int kiumd_set_dma_addr_ranges(struct kiumd_ctx *kiumd_ctx, struct device *dev)
{
	const __be32 *addr_range;
	u64 start_addr, end_addr;
	struct device_node *np;
	int len;

	np = dev->of_node;
	if (!np) {
		dev_err(dev, "Device tree node not found\n");
		return -EINVAL;
	}

	addr_range = of_get_property(np, "qcom,iommu-dma-addr-range", &len);
	if (!addr_range)
		return 0; /*This is not an error, not every device need to have this property set*/

	if (len < (2 * sizeof(u32))) {
		dev_err(dev, "qcom,iommu-dma-addr-range property length is invalid\n");
		return -EINVAL;
	}

	start_addr = of_read_number(addr_range, 2);
	end_addr = of_read_number(addr_range + 2, 2);
	if (end_addr < start_addr) {
		dev_err(dev, "invalid address specified in the device tree\n");
		return -EINVAL;
	}

	kiumd_ctx->pt_start_iova = start_addr;
	kiumd_ctx->pt_end_iova = end_addr;

	return 0;
}

/**
 * @Brief: This function is to map the IOVAs in a predefined address range. The
 * IOVA address range should be specified in the device tree using the attribute
 * qcom,iommu-dma-addr-pool The function is called via IOCTL
 * interface and input is provided via struct kiumd_user from the user space.
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Returns  0 upon success and error codes on failure
 */
int kiumd_dmabuf_custom_iova_init(char __user *arg, struct file *fp)
{
	struct kiumd_iommu_dma_cookie *cookie;
	struct iommu_resv_region *region;
	struct vfio_device *vfio_dev;
	struct vfio_device_file *df;
	struct iommu_domain *domain;
	struct kiumd_ctx *kiumd_ctx;
	struct iova_domain *iovad;
	struct kiumd_user kiusr;
	unsigned long lo, hi;
	struct file *file;
	LIST_HEAD(resrvd);
	int ret;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	trace_kiumd_dmabuf_custom_iova_init_start(kiusr.vfio_fd);

	if (kiusr.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kiusr.vfio_fd);
	if (!file) {
		pr_err("%s:Invalid vfio fd\n", __func__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev)  {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	/*Get the maximum shift from DT for managed_iova_map api to
	 * determine alignment for large buffers
	 */
	kiumd_ctx->max_shift = get_shift_from_dt(vfio_dev->dev);

	kiumd_set_dma_max_seg_size(vfio_dev->dev, (unsigned int) DMA_BIT_MASK(32));

	ret = kiumd_set_dma_addr_ranges(kiumd_ctx, vfio_dev->dev);
	if (ret) {
		pr_err("%s:set dma addr ranges failed for %s\n", __func__,
		       dev_name(vfio_dev->dev));
		fput(file);
		return -EINVAL;
	}

	domain = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!domain) {
		pr_err("%s:dma_domain is invalid\n", __func__);
		fput(file);
		return -EINVAL;
	}

	cookie = (struct kiumd_iommu_dma_cookie *)domain->iova_cookie;
	iovad = &cookie->iovad;

	qcom_iommu_generate_resv_regions(vfio_dev->dev, &resrvd);
	list_for_each_entry(region, &resrvd, list) {
		lo = iova_pfn(iovad, region->start);
		hi = iova_pfn(iovad, region->start + region->length - 1);
		reserve_iova(iovad, lo, hi);
	}

	fput(file);
	trace_kiumd_dmabuf_custom_iova_init_end(kiusr.vfio_fd);
	return 0;
}

/**
 * @Brief: This function facilitates to
 * clear the global map based of
 * given iova and ptselect
 *
 * Parameters:
 * @iova: u64 virtual address for page table
 * @size: u64 size
 * @ptselect: type of pagetable per
 * process or global
 *
 * Returns nothing
 */
int clear_map_iova(struct kiumd_ctx *kiumd_ctx, u64 iova, u64 size,
		   int ptselect, unsigned int idx)
{
	int ret = 0;
	u64 bit;

	if (ptselect == KGSL_GLOBAL_PT) {
		bit = (iova & ~KGSL_GLOBAL_PT_BASE_IOVA) >> PAGE_SHIFT;
		bitmap_clear(global_map, bit, size >> PAGE_SHIFT);
	} else if (ptselect == KGSL_PER_PROCESS_PT) {
		ret = free_allocated_iova(kiumd_ctx, iova, idx, true);
		if (ret) {
			pr_err("%s:%d unable to free iova\n", __func__, __LINE__);
			return ret;
		}
	}

	return ret;
}

/**
 * @Brief: This function facilitates to
 * get the offset of the global map
 * based of given size and  ptselect
 *
 * Parameters:
 * @size: u64 size
 * @ptselect: type of pagetable per
 * process or global
 *
 * Returns map offset
 */
s64 get_map_offset(u64 size, int ptselect)
{
	static u64 last_offset_global;
	static u64 last_offset_perprocess;
	u64 *last_offset;
	u64 bit, offset;
	unsigned long *map;

	if (ptselect != KGSL_GLOBAL_PT && ptselect != KGSL_PER_PROCESS_PT) {
		pr_err("%s: Invalid ptselect: %d\n", __func__, ptselect);
		return (s64) -EINVAL;
	}

	last_offset = (ptselect == KGSL_GLOBAL_PT) ? &last_offset_global :
						     &last_offset_perprocess;
	map = (ptselect == KGSL_GLOBAL_PT) ? global_map : perprocess_map;
	if (map == NULL) {
		pr_err("%s: Bitmap map is NULL for ptselect: %d\n", __func__, ptselect);
		return (s64) -EFAULT;
	}

	if (size == 0 || (size >> PAGE_SHIFT) == 0) {
		pr_err("%s: Invalid size: 0x%llx, for ptselect: %d\n",
		       __func__, size, ptselect);
		return (s64) -EINVAL;
	}

	bit = bitmap_find_next_zero_area(map, KGSL_PT_MEM_PAGES, *last_offset,
					 size >> PAGE_SHIFT, 0);

	if (bit + (size >> PAGE_SHIFT) >= KGSL_PT_MEM_PAGES) {
		bit = bitmap_find_next_zero_area(map, KGSL_PT_MEM_PAGES, 0,
						 size >> PAGE_SHIFT, 0);
		if (bit >= KGSL_PT_MEM_PAGES) {
			pr_err("%s: No free area in bitmap for size: 0x%llx, ptselect: %d\n",
			       __func__, size, ptselect);
			return (s64) -ENOSPC;
		}
	}

	bitmap_set(map, bit, size >> PAGE_SHIFT);
	offset = bit << PAGE_SHIFT;

	*last_offset = (bit + (size >> PAGE_SHIFT)) % KGSL_PT_MEM_PAGES;

	return (s64) offset;
}

/**
 * @Brief: This function facilitates to
 * set the offset of the global map
 * based of given size and  ptselect
 *
 * Parameters:
 * @offset: offset for global map
 * @vfio_dev: vfio device *
 * @ptselect: type of pagetable per
 * process or global
 *
 * Returns errno in failure or 0 in case
 * of success
 */
int set_map_iova(u64 offset, struct vfio_device *vfio_dev, int ptselect)
{
	struct kiumd_iommu_dma_cookie *cookie;
	dma_addr_t iova = offset;
	int ret;

	if (ptselect == KGSL_GLOBAL_PT)
		iova = KGSL_GLOBAL_PT_BASE_IOVA + offset;
	else if (ptselect == KGSL_PER_PROCESS_PT)
		iova = offset;
	else {
		pr_err("%s invalid ptselect\n", __func__);
		return -EINVAL;
	}

	cookie = kiumd_get_dma_cookie(vfio_dev);
	if (!cookie) {
		pr_err("%s failed to get cookie\n", __func__);
		return -EINVAL;
	}

	ret = kiumd_set_dma_cookie_unlocked(cookie, IOMMU_DMA_MSI_COOKIE, iova);
	if (ret)
		pr_err("%s failed to set cookie\n", __func__);

	return ret;
}

/**
 * @Brief: This function facilitates to
 * modify the page links for each of
 * scatter gather table
 *
 * Parameters:
 * @sg_table: pointer for scatter gather table
 *
 * Returns void
 */
static void kiumd_mangle_sg_table(struct sg_table *sg_table)
{
	struct scatterlist *sg;
	int i;

	for_each_sgtable_sg(sg_table, sg, i) {
		if (sg)
			sg->page_link ^= ~0xffUL;
	}
}

/**
 * @Brief: This function facilitates to check the fixed iova mapping  by
 * checking the cookie type for the vfio device.
 *
 * Parameters:
 * @vfio_dev: pointer for vfio device structure
 *
 * Returns true/false
 */
bool is_fixed_mapping(struct vfio_device *vfio_dev)
{
	struct kiumd_iommu_dma_cookie *cookie;

	cookie = kiumd_get_dma_cookie(vfio_dev);
	if (cookie) {
		if (cookie->type == IOMMU_DMA_MSI_COOKIE)
			return true;
	}

	return false;
}
/**
 * alloc_iova_range_contiguous - Allocate a contiguous IOVA range
 * @ptable_ctx: Pointer to the pagetable context
 * @size: Size of the IOVA range to allocate
 *
 * This function attempts to allocate a contiguous IOVA range of the specified
 * size within the given pagetable context. It first tries to allocate from the
 * end of the last allocated block if the context is marked as contiguous. If
 * that fails, it resets the last allocated end and searches for available holes
 * in the IOVA space. If a suitable range is found, it allocates and inserts the
 * new IOVA entry into the pagetable context.
 *
 * Return: The starting address of the allocated IOVA range, or 0 if no suitable
 * range is found.
 */
static unsigned long alloc_iova_range_contiguous(struct pgtable_map
						 *ptable_ctx,
						 unsigned long size)
{
	struct iommu_addr_entry *new_entry, *entry;
	unsigned long available_start;
	bool found_hole;

	struct rb_node *node = rb_first(&ptable_ctx->rbtree);
	unsigned long last_allocated_end = ptable_ctx->last_allocated_end;

	if (last_allocated_end == 0)
		last_allocated_end = ptable_ctx->start_iova;

	// Try to allocate from the end of the last allocated block
	if ((last_allocated_end + size <= ptable_ctx->end_iova) &&
	    ptable_ctx->is_contiguous) {
		new_entry = alloc_iommu_addr_entry(last_allocated_end, size);

		if (!new_entry) {
			pr_err("%s:%d failed to create new entry for iova: %lx with size: %lu\n",
			       __func__, __LINE__, last_allocated_end, size);
			return 0;
		}


		insert_iova(ptable_ctx, new_entry);
		ptable_ctx->last_allocated_end = last_allocated_end + size;
		return last_allocated_end;
	}

	// Reset alloacted end
	if (last_allocated_end + size > ptable_ctx->end_iova) {
		ptable_ctx->last_allocated_end = ptable_ctx->start_iova;
		ptable_ctx->is_contiguous = false;
		last_allocated_end = ptable_ctx->start_iova;
	}

	available_start = last_allocated_end;
	found_hole = false;

	// If no contiguous space is available, systematically search for holes
	while (node) {
		entry = rb_entry(node, struct iommu_addr_entry, rbnode);

		if ((available_start + size <= entry->base_addr) &&
		    (available_start >= ptable_ctx->start_iova)) {
			if (available_start >= last_allocated_end) {
				found_hole = true;
				break;
			}
		}

		available_start = entry->base_addr + entry->size;
		node = rb_next(node);
	}

	if (!found_hole && (available_start + size <= ptable_ctx->end_iova) &&
	    (available_start >= ptable_ctx->start_iova))
		found_hole = true;

	// Finally, check for space after the last allocated block
	if (found_hole) {
		new_entry = alloc_iommu_addr_entry(available_start, size);

		if (!new_entry) {
			pr_err("%s:%d failed to create entry for addr: %lx with size: %lu\n",
			       __func__, __LINE__, last_allocated_end, size);
			return 0;
		}

		insert_iova(ptable_ctx, new_entry);
		ptable_ctx->last_allocated_end = available_start + size;
		return available_start;
	}

	return 0; // No suitable IOVA found
}

/**
 * get_pgtble_and_alloc_iova - Retrieve pagetable entry and allocate IOVA range
 * @vfio_fd: File descriptor for the VFIO device
 * @kiumd_ctx: Pointer to the kiumd context
 * @size: Size of the IOVA range to allocate
 * @idx: Index of the pagetable entry to retrieve
 *
 * This function retrieves a pagetable entry from the kiumd context based on the
 * provided index. It then checks the validity of the pagetable context against
 * the SMMU domain associated with the VFIO file descriptor. If the context is
 * valid, it attempts to allocate a contiguous IOVA range of the specified size.
 * If any step fails, it logs an error and returns 0.
 *
 * Return: The starting address of the allocated IOVA range, or 0 if any step fails.
 */
static uint64_t get_pgtble_and_alloc_iova(int vfio_fd,
					  struct kiumd_ctx *kiumd_ctx,
					  u64 size,
					  unsigned int idx)
{
	struct pgtable_map *pgtble_ctx;
	uint64_t addr;

	pgtble_ctx = kiumd_get_pgtable_entry(kiumd_ctx, idx, true);
	if (!pgtble_ctx) {
		pr_err("%s:%d Invalid id for hash table: id: %d\n", __func__, __LINE__, idx);
		return 0;
	}

	if (!check_pgtable_context(vfio_fd, pgtble_ctx)) {
		pr_err("%s:%d check_pgtable_context failed\n", __func__, __LINE__);
		return 0;
	}

	addr = alloc_iova_range_contiguous(pgtble_ctx, size);
	if (!addr) {
		pr_err("%s:%d IOVA memory limit reached\n", __func__, __LINE__);
		return 0;
	}

	return addr;
}

void add_to_smmu_table(struct kiumd_ctx *ctx, struct smmu_map_data *map_data)
{
	spin_lock(&ctx->smmu_lock);
	map_data->id = ctx->id++;
	hash_add(ctx->smmu_table, &map_data->node, map_data->id);
	spin_unlock(&ctx->smmu_lock);
}

int set_allocated_iova(struct vfio_device *vfio_dev, unsigned long iova)
{
	struct kiumd_iommu_dma_cookie *cookie;
	int ret;

	cookie = kiumd_get_dma_cookie(vfio_dev);
	if (!cookie) {
		pr_err("%s failed to get cookie\n", __func__);
		return -EINVAL;
	}

	ret = kiumd_set_dma_cookie_unlocked(cookie, IOMMU_DMA_MSI_COOKIE, iova);
	if (ret)
		pr_err("%s failed to set cookie\n", __func__);

	return ret;
}

static int init_and_allocate_iova(struct vfio_device *vfio_dev,
				  struct kiumd_ctx *kiumd_ctx,
				  unsigned long idx, unsigned int size,
				  unsigned long max_shift)
{
	struct pgtable_map *pgtable_ctx;
	unsigned long iova;
	int ret;

	if (!kiumd_ctx->is_initialized) {

		pgtable_ctx = kzalloc(sizeof(*pgtable_ctx), GFP_KERNEL);
		if (!pgtable_ctx)
			return -ENOMEM;

		pgtable_ctx->rbtree = RB_ROOT;
		pgtable_ctx->idx = idx;
		pgtable_ctx->start_iova = kiumd_ctx->pt_start_iova;
		pgtable_ctx->end_iova = kiumd_ctx->pt_end_iova;

		spin_lock(&kiumd_ctx->pt_lock);
		hash_add(kiumd_ctx->page_table, &pgtable_ctx->node, idx);
		spin_unlock(&kiumd_ctx->pt_lock);

		kiumd_ctx->is_initialized = true;
	} else {
		pgtable_ctx = kiumd_get_pgtable_entry(kiumd_ctx, idx, false);
		if (!pgtable_ctx) {
			pr_err("%s: Failed to find pgtable_map for device: %d", __func__, idx);
			return -EINVAL;
		}
	}

	iova = alloc_iova_range(vfio_dev, pgtable_ctx, size, max_shift);
	if (!iova) {
		pr_err("%s: Failed to allocate iova.\n", __func__);
		return -ENOMEM;
	}

	ret = set_allocated_iova(vfio_dev, iova);
	if (ret < 0) {
		pr_err("%s: Failed to set the iova.\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

unsigned long get_hash_key(int vfio_fd)
{
	struct vfio_device *vfio_dev;
	unsigned long hash_id;

	if (vfio_fd < 0) {
		pr_err("%s:%d invalid fd from user\n", __func__, __LINE__);
		return -EBADF;
	}

	vfio_dev = kiumd_get_vfio_device(vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:invalid vfio_fd from user\n", __func__);
		return -EINVAL;
	}

	hash_id = (unsigned long) kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!hash_id) {
		pr_err("%s:invalid domain\n", __func__);
		return -EINVAL;
	}

	return hash_id;
}


int kiumd_dmabuf_managed_iova_map(char __user *arg, struct file *fp)
{
	struct dma_buf_attachment *dmabufattach;
	struct vfio_device *vfio_dev;
	struct dma_buf *kiumd_dmabuf;
	int kiumd_dma_direction, ret;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	unsigned long max_shift;
	unsigned long hash_id;
	struct sg_table *sgt;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	vfio_dev = kiumd_get_vfio_device(kiusr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s: invalid vfio device fd\n", __func__);
		return -EINVAL;
	}

	hash_id = get_hash_key(kiusr.vfio_fd);
	if (!hash_id) {
		pr_err("%s:invalid domain\n", __func__);
		return -EINVAL;
	}

	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:dma_buf_get failed with error: %ld, for device: %s\n",
		       __func__, PTR_ERR(kiumd_dmabuf), dev_name(vfio_dev->dev));
		ret = (kiumd_dmabuf == NULL ? -EINVAL : PTR_ERR(kiumd_dmabuf));
		return ret;
	}

	if (kiumd_ctx->max_shift) {
		ret = init_and_allocate_iova(vfio_dev, kiumd_ctx, hash_id, kiumd_dmabuf->size,
					     kiumd_ctx->max_shift);
		if (ret) {
			pr_err("%s: failed to allocate iova for: %s, ret: %d\n",
			       __func__, dev_name(vfio_dev->dev), ret);
			return ret;
		}
	}

	if (!(vfio_dev->dev)) {
		pr_err("%s:vfio_dev->dev is NULL\n", __func__);
		return -ENODEV;
	}

	dmabufattach = dma_buf_attach(kiumd_dmabuf, vfio_dev->dev);
	if (IS_ERR(dmabufattach)) {
		pr_err("%s:dmabufattach failed with error: %ld, for device: %s\n",
		       __func__, PTR_ERR(dmabufattach), dev_name(vfio_dev->dev));
		ret = PTR_ERR(dmabufattach);
		goto fail_put;
	}

	if (IS_ERR_OR_NULL(dmabufattach->dmabuf)) {
		pr_err("%s:dmabuf is NULL\n", __func__);
		return -EINVAL;
	}

	if ((kiusr.dma_direction < DMA_BIDIRECTIONAL)
	    || (kiusr.dma_direction > DMA_NONE)) {
		pr_err("%s:Invalid DMA direction: %d\n", __func__, kiusr.dma_direction);
		ret = -EINVAL;
		goto fail_detach;
	}

	if (kiusr.dma_direction == DMA_TO_DEVICE)
		kiumd_dma_direction = kiusr.dma_direction;
	else
		kiumd_dma_direction = DMA_BIDIRECTIONAL;

	sgt = dma_buf_map_attachment_unlocked(dmabufattach, kiumd_dma_direction);
	if (IS_ERR_OR_NULL(sgt)) {
		pr_err("%s: mapping failed with error: %ld, for device: %s\n",
		       __func__, PTR_ERR(sgt), dev_name(vfio_dev->dev));
		ret = (sgt == NULL ? -EINVAL : PTR_ERR(sgt));
		goto fail_detach;
	}

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap) {
		ret = -ENOMEM;
		goto fail_detach;
	}

	smap->dmabufattach = (long)dmabufattach;
	smap->sgt_ptr = (long)sgt;
	smap->dmabuf_ptr = (long)kiumd_dmabuf;
	add_to_smmu_table(kiumd_ctx, smap);

	kiusr.id = smap->id;
	kiusr.dma_addr = (unsigned long) sg_dma_address(sgt->sgl);

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed...\n", __func__);
		dma_buf_unmap_attachment_unlocked(dmabufattach, sgt,
						  kiumd_dma_direction);
		ret = -EFAULT;
		goto fail_detach;
	}

	return 0;

fail_detach:
	dma_buf_detach(kiumd_dmabuf, dmabufattach);

fail_put:
	dma_buf_put(kiumd_dmabuf);

	return ret;
}

int kiumd_dmabuf_managed_iova_unmap(char __user *arg, struct file *fp)
{
	struct dma_buf_attachment *dmabufattach;
	struct iommu_domain *iommu_dom;
	int kiumd_dma_direction, ret;
	struct dma_buf *kiumd_dmabuf;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	unsigned long hash_id;
	bool found = false;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	if (kiusr.id < 0) {
		pr_err("%s:id passed from user should be positive value\n", __func__);
		return -EFAULT;
	}

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_for_each_possible(kiumd_ctx->smmu_table, smap, node, kiusr.id) {
		if (smap->id == kiusr.id) {
			found = true;
			break;
		}
	}

	if (!found) {
		spin_unlock(&kiumd_ctx->smmu_lock);
		return -ENOENT;
	}

	spin_unlock(&kiumd_ctx->smmu_lock);

	kiumd_dmabuf = (struct dma_buf *)smap->dmabuf_ptr;
	if (!kiumd_dmabuf) {
		pr_err("%s:kiumd_dmabuf is NULL\n", __func__);
		return -EINVAL;
	}

	hash_id = get_hash_key(kiusr.vfio_fd);
	if (hash_id < 0) {
		pr_err("%s:invalid domain\n", __func__);
		return -EINVAL;
	}

	if (kiumd_ctx->max_shift) {
		ret = free_allocated_iova(kiumd_ctx, kiusr.dma_addr, hash_id,
					  false);
		if (ret) {
			pr_err("%s:unable to free iova\n", __func__);
			return ret;
		}
	}

	dmabufattach = (struct dma_buf_attachment *)smap->dmabufattach;
	if (!dmabufattach) {
		pr_err("%s:dmabufattach is NULL\n", __func__);
		return -EINVAL;
	}

	if (kiusr.dma_direction == 1)
		kiumd_dma_direction = kiusr.dma_direction;
	else
		kiumd_dma_direction = 0;

	if (!smap->sgt_ptr) {
		pr_err("%s: smap->sgt_ptr is NULL\n", __func__);
		return -EINVAL;
	}

	dma_buf_unmap_attachment_unlocked(dmabufattach,
					  (struct sg_table *)smap->sgt_ptr,
					  kiumd_dma_direction);

	iommu_dom = kiumd_get_iommu_domain(kiusr.vfio_fd);
	if (!iommu_dom) {
		pr_err("%s: iommu_dom is NULL\n", __func__);
		return -EINVAL;
	}

	iommu_flush_iotlb_all(iommu_dom);

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	kfree(smap);
	spin_unlock(&kiumd_ctx->smmu_lock);
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);

	return 0;
}

/**
 * kiumd_dmabuf_vfio_map(char __user *arg, struct file *fp)
 *
 * This function facilitates the mapping of a DMA-BUF based buffer to a SMMU
 * backed device represented via a vfio_device.
 *
 * The function is called via IOCTL interface and input is provided via struct
 * kiumd_user from the user space.
 *
 * return value is errno or 0 in case of successful mapping
 */
int kiumd_dmabuf_vfio_map(char __user *arg, struct file *fp)
{
	struct kiumd_dma_heap_attachment *dmaheapattachment;
	struct dma_buf_attachment *dmabufattach;
	int kiumd_dma_direction, ret, prot;
	struct iommu_domain *iommu_dom;
	struct dma_buf *kiumd_dmabuf;
	struct vfio_device *vfio_dev;
	struct kiumd_ctx *kiumd_ctx;
	struct vfio_device_file *df;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	struct sg_table *sgt;
	struct file *file;
	s64 offset;
	u64 size;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap)
		return -ENOMEM;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	if (kiusr.vfio_fd < 0) {
		pr_err("%s: Invalid fd from user\n", __func__);
		return -EBADF;
	}

	file = fget(kiusr.vfio_fd);
	if (!file) {
		pr_err("%s:failed to get file from vfio fd\n", __func__);
		return -EBADF;
	}
	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (vfio_dev == NULL) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		ret = -EINVAL;
		goto fail_fput;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;
	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:dma_buf_get failed with error: %ld, for device: %s\n",
		       __func__, PTR_ERR(kiumd_dmabuf), vfio_dev->dev->kobj.name);
		ret = (kiumd_dmabuf == NULL ? -EINVAL : PTR_ERR(kiumd_dmabuf));
		goto fail_fput;
	}

	trace_kiumd_dmabuf_vfio_map_start(dev_name(vfio_dev->dev),
					  kiusr.vfio_fd, kiusr.dma_buf_fd,
					  kiusr.dma_attr, kiusr.dma_direction,
					  kiusr.ptselect, kiusr.is_iova_zero,
					  kiumd_dmabuf->size, kiumd_ctx);

	if ((kiusr.ptselect == KGSL_GLOBAL_PT) ||
	    (kiusr.ptselect == KGSL_PER_PROCESS_PT)) {
		size = kiumd_dmabuf->size;
		if (kiusr.ptselect == KGSL_GLOBAL_PT) {
			offset = get_map_offset(size, kiusr.ptselect);
			if (offset < 0) {
				pr_err("%s:%d failed to get offset\n",
				       __func__, __LINE__);
				ret = offset;
				goto fail_put;
			}

			ret = set_map_iova((u64)offset, vfio_dev, kiusr.ptselect);
			if (ret < 0) {
				pr_err("%s:%d failed to set offset\n",
				       __func__, __LINE__);
				goto fail_put;
			}
		} else {
			offset = get_pgtble_and_alloc_iova(kiusr.vfio_fd,
							   kiumd_ctx, size,
							   kiusr.pt_id);
			if (!offset) {
				pr_err("%s:%d get_pgtble_and_alloc_iova failed\n", __func__, __LINE__);
				ret = -EINVAL;
				goto fail_put;
			}
			ret = set_map_iova(offset, vfio_dev, kiusr.ptselect);
			if (ret < 0) {
				pr_err("%s:%d failed to set offset\n", __func__, __LINE__);
				goto fail_put;
			}
		}
	}

	if (!(vfio_dev->dev)) {
		pr_err("%s:vfio_dev->dev is NULL\n", __func__);
		ret = -ENODEV;
		goto fail_put;
	}

	dmabufattach = dma_buf_attach(kiumd_dmabuf, vfio_dev->dev);
	if (IS_ERR(dmabufattach)) {
		pr_err("%s:dmabufattach failed with error: %ld, for device: %s\n",
		       __func__, PTR_ERR(dmabufattach), vfio_dev->dev->kobj.name);
		ret = PTR_ERR(dmabufattach);
		goto fail_put;
	}

	if ((kiusr.dma_direction < DMA_BIDIRECTIONAL) ||
	    (kiusr.dma_direction > DMA_NONE)) {
		pr_err("%s:Invalid DMA direction: %d\n", __func__, kiusr.dma_direction);
		ret = -EINVAL;
		goto fail_detach;
	}

	if (kiusr.dma_direction == DMA_TO_DEVICE)
		kiumd_dma_direction = kiusr.dma_direction;
	else
		kiumd_dma_direction = DMA_BIDIRECTIONAL;

	if ((kiusr.dma_attr == DMA_ATTR_PRIVILEGED) &&
	    (kiusr.is_iova_zero != FIXED_IOVA_AT_ZERO)) {
		if (!(dmabufattach->priv)) {
			pr_err("%s:dmabufattach-priv is NULL\n", __func__);
			ret = -EINVAL;
			goto fail_detach;
		}

		dmaheapattachment = (struct kiumd_dma_heap_attachment *)dmabufattach->priv;
		if (!dmaheapattachment) {
			pr_err("%s:dmaheapattachment is NULL\n", __func__);
			ret = -EINVAL;
			goto fail_detach;
		}

		sgt = dmaheapattachment->table;
		if (!sgt) {
			pr_err("%s:sglist is NULL\n", __func__);
			ret = -EINVAL;
			goto fail_detach;
		}

		ret = dma_map_sgtable(vfio_dev->dev, sgt, kiumd_dma_direction,
				      DMA_ATTR_PRIVILEGED |
				      DMA_ATTR_SKIP_CPU_SYNC);
		if (ret) {
			pr_err("%s:dma_map_sgtable failed with err: %d, for device: %s\n",
			       __func__, ret, vfio_dev->dev->kobj.name);
			goto fail_detach;
		}
	} else {

		if (IS_ERR_OR_NULL(dmabufattach->dmabuf)) {
			pr_err("%s:dmabuf is NULL\n", __func__);
			return -EINVAL;
		}

		sgt = dma_buf_map_attachment_unlocked(dmabufattach, kiumd_dma_direction);
		if (IS_ERR_OR_NULL(sgt)) {
			pr_err("%s: mapping failed with error: %ld, for device: %s\n",
			       __func__, PTR_ERR(sgt), vfio_dev->dev->kobj.name);
			ret = (sgt == NULL ? -EINVAL : PTR_ERR(sgt));
			goto fail_detach;
		}

		if (kiusr.is_iova_zero == FIXED_IOVA_AT_ZERO) {
			if (kiusr.dma_attr == DMA_ATTR_PRIVILEGED)
				prot = IOMMU_CACHE | IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV;
			else
				prot = IOMMU_CACHE | IOMMU_READ | IOMMU_WRITE;

			iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
			if (!iommu_dom) {
				pr_err("%s:IOMMU domain is NULL\n", __func__);
				ret = -EINVAL;
				goto fail_fput;
			}

#ifdef CONFIG_DMABUF_DEBUG
			/*
			 * For debug builds there is a mangling done in the regular buffer map path,
			 * so iommu_map_sg is expecting a mangled physical address of the buffer
			 */

			kiumd_mangle_sg_table(sgt);
#endif

			/*
			 * For mapping at 0x0 we create a mapping using
			 * dma_buf_map_attachment_unlocked and then take the sg list
			 * and map it at iova - 0x0
			 */

			size = iommu_map_sg(iommu_dom, IOVA_ZERO, sgt->sgl,
					    sgt->orig_nents, prot, GFP_ATOMIC);
			if (size < 0) {
				pr_err("%s:iommu_map_sg failed\n", __func__);
				dma_buf_unmap_attachment_unlocked(dmabufattach, sgt, kiumd_dma_direction);
				ret = -EFAULT;
				goto fail_detach;
			}
		}
	}

	if (is_fixed_mapping(vfio_dev))
		smap->is_fixed_map = true;
	else
		smap->is_fixed_map = false;

	smap->dmabufattach = (long)dmabufattach;
	smap->sgt_ptr = (long)sgt;
	smap->dmabuf_ptr = (long)kiumd_dmabuf;
	smap->dma_dir = kiumd_dma_direction;
	smap->ptselect = kiusr.ptselect;
	smap->is_iova_zero = kiusr.is_iova_zero;
	smap->vfio_dev = vfio_dev;
	spin_lock(&kiumd_ctx->smmu_lock);
	smap->id = kiumd_ctx->id++;
	hash_add(kiumd_ctx->smmu_table, &smap->node, smap->id);
	spin_unlock(&kiumd_ctx->smmu_lock);

	kiusr.id = smap->id;
	if (kiusr.is_iova_zero == FIXED_IOVA_AT_ZERO)
		kiusr.dma_addr = (unsigned long) IOVA_ZERO;
	else
		kiusr.dma_addr = (unsigned long) sg_dma_address(sgt->sgl);

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed...\n", __func__);
		dma_buf_unmap_attachment_unlocked(dmabufattach, sgt,
						  kiumd_dma_direction);
		ret = -EFAULT;
		goto fail_detach;
	}
	trace_kiumd_dmabuf_vfio_map_end(kiusr.vfio_fd, kiusr.id, kiusr.dma_addr);
	fput(file);
	return 0;

fail_detach:
	dma_buf_detach(kiumd_dmabuf, dmabufattach);

fail_put:
	dma_buf_put(kiumd_dmabuf);

fail_fput:
	fput(file);

	return ret;
}

/**
 * Brief: This function facilitates the unmap the buffer mapped to SMMU backed device
 * also decrements the dma_buf kref count.
 *
 * Parameters:
 * @arg: user space argument pointer
 * @fp: file pointer for kiumd_ctx
 *
 * return errno or 0 in case of success
 */
int kiumd_dmabuf_vfio_unmap(char __user *arg, struct file *fp)
{

	struct kiumd_dma_heap_attachment *dmaheapattachment;
	struct dma_buf_attachment *dmabufattach;
	struct iommu_domain *iommu_dom;
	struct dma_buf *kiumd_dmabuf;
	struct vfio_device *vfio_dev;
	int kiumd_dma_direction, ret;
	struct vfio_device_file *df;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct sg_table *sgtable;
	struct kiumd_user kiusr;
	struct file *file;
	bool found = false;
	u64 size;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	if (kiusr.id < 0) {
		pr_err("%s:id passed from user should be positive value\n", __func__);
		return -EFAULT;
	}

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_for_each_possible(kiumd_ctx->smmu_table, smap, node, kiusr.id) {
		if (smap->id == kiusr.id) {
			found = true;
			break;
		}
	}

	if (!found) {
		spin_unlock(&kiumd_ctx->smmu_lock);
		return -ENOENT;
	}

	spin_unlock(&kiumd_ctx->smmu_lock);
	kiumd_dmabuf = (struct dma_buf *)smap->dmabuf_ptr;
	if (!kiumd_dmabuf) {
		pr_err("%s:kiumd_dmabuf is NULL\n", __func__);
		return -EINVAL;
	}

	file = fget(kiusr.vfio_fd);
	if (!file) {
		pr_err("%s:fget returns NULL\n", __func__);
		return -EINVAL;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:vfio dev returns NULL\n", __func__);
		fput(file);
		return -EINVAL;
	}

	trace_kiumd_dmabuf_vfio_unmap_start(dev_name(vfio_dev->dev),
					    kiusr.vfio_fd, kiusr.dma_buf_fd,
					    kiusr.dma_addr, kiusr.dma_attr,
					    kiusr.dma_direction,
					    kiusr.ptselect, kiusr.is_iova_zero,
					    kiumd_dmabuf->size, kiumd_ctx);

	dmabufattach = (struct dma_buf_attachment *)smap->dmabufattach;
	if (!dmabufattach) {
		pr_err("%s:dmabufattach is NULL\n", __func__);
		return -EINVAL;
	}

	if (kiusr.ptselect == KGSL_GLOBAL_PT ||
	    kiusr.ptselect == KGSL_PER_PROCESS_PT) {
		ret = kiumd_configure_dma_cookie(vfio_dev,
						 IOMMU_DMA_MSI_COOKIE,
						 kiusr.dma_addr);
		if (ret) {
			pr_err("%s %d failed to configure cookie\n",
			       __func__, __LINE__);
			return -EINVAL;
		}

		ret = clear_map_iova(kiumd_ctx, kiusr.dma_addr,
				     kiumd_dmabuf->size, kiusr.ptselect,
				     kiusr.pt_id);
		if (ret) {
			pr_err("%s:clear_map_iova failed\n", __func__);
			return -EINVAL;
		}
	}

	if (kiusr.dma_direction == 1)
		kiumd_dma_direction = kiusr.dma_direction;
	else
		kiumd_dma_direction = 0;

	if (smap->is_fixed_map) {
		ret = kiumd_configure_dma_cookie(vfio_dev,
						 IOMMU_DMA_MSI_COOKIE,
						 kiusr.dma_addr);
		if (ret) {
			pr_err("%s %d failed to configure cookie\n",
			       __func__, __LINE__);
			return -EINVAL;
		}
	}

	if ((kiusr.dma_attr == DMA_ATTR_PRIVILEGED) &&
	    (kiusr.is_iova_zero != FIXED_IOVA_AT_ZERO)) {
		if (!(dmabufattach->priv)) {
			pr_err("%s:dmabufattach-priv is NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}

		dmaheapattachment = (struct kiumd_dma_heap_attachment *)dmabufattach->priv;
		if (!dmaheapattachment) {
			pr_err("%s:dmaheapattachment is NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}

		sgtable = dmaheapattachment->table;
		if (!sgtable) {
			pr_err("%s:sglist is NULL\n", __func__);
			fput(file);
			return -EINVAL;
		}

		dma_unmap_sgtable(vfio_dev->dev, sgtable, kiumd_dma_direction,
				  DMA_ATTR_PRIVILEGED);
	} else {
		if (!smap->sgt_ptr) {
			pr_err("%s: smap->sgt_ptr is NULL\n", __func__);
			return -EINVAL;
		}
		dma_buf_unmap_attachment_unlocked(dmabufattach,
						  (struct sg_table *)smap->sgt_ptr,
						  kiumd_dma_direction);
		if (kiusr.is_iova_zero == FIXED_IOVA_AT_ZERO) {
			iommu_dom = kiumd_get_iommu_domain(kiusr.vfio_fd);
			if (!iommu_dom) {
				pr_err("%s:iommu_dom is NULL\n", __func__);
				return -EINVAL;
			}

			size = iommu_unmap(iommu_dom, IOVA_ZERO, kiumd_dmabuf->size);
			if (size != kiumd_dmabuf->size) {
				pr_err("%s:iommu_unmap failed\n", __func__);
				return -EINVAL;
			}
		}
	}

	if (smap->is_fixed_map) {
		ret = kiumd_configure_dma_cookie(vfio_dev,
						 IOMMU_DMA_IOVA_COOKIE,
						 kiusr.dma_addr);
		if (ret) {
			pr_err("%s %d failed to configure cookie\n",
			       __func__, __LINE__);
			return -EINVAL;
		}
	}

	if (kiusr.ptselect == KGSL_GLOBAL_PT ||
	    kiusr.ptselect == KGSL_PER_PROCESS_PT ||
	    kiusr.ptselect == KGSL_DEFAULT_PT) {
		iommu_dom = kiumd_get_iommu_domain(kiusr.vfio_fd);
		if (!iommu_dom) {
			pr_err("%s:iommu_dom is NULL\n", __func__);
			return -EINVAL;
		}
		iommu_flush_iotlb_all(iommu_dom);
	}

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	kfree(smap);
	spin_unlock(&kiumd_ctx->smmu_lock);
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);
	fput(file);

	trace_kiumd_dmabuf_vfio_unmap_end(kiusr.vfio_fd);

	return 0;
}

/**
 * @Brief: This function facilitates to
 * set the cookie type based on flag
 * passed from user space and then set
 * cookie in dma.The function is called
 * via IOCTL interface and input is
 * provided via struct kiumd_user
 * from the user space.
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Returns  0 upon success and error codes on failure
 */
int kiumd_iova_ctrl(char __user *arg)
{
	struct kiumd_iommu_dma_cookie *cookie;
	dma_addr_t iova_usr = IOVA_ZERO;
	struct vfio_device *vfio_dev;
	struct kiumd_iova iovausr;
	int cookie_type, ret;

	if (copy_from_user(&iovausr, arg, sizeof(struct kiumd_iova)))
		return -EFAULT;

	trace_kiumd_iova_ctrl_start(iovausr.vfio_fd, iovausr.iova_flag, iovausr.iova);

	if (iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_CLEAR)
		cookie_type = 0;
	else
		cookie_type = 1;

	if (iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_SET) {
		cookie_type = 1;
		iova_usr = iovausr.iova;
	}

	vfio_dev = kiumd_get_vfio_device(iovausr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s failed to get vfio device\n", __func__);
		return -EINVAL;
	}

	cookie = kiumd_get_dma_cookie(vfio_dev);
	if (!cookie) {
		pr_err("%s failed to get cookie\n", __func__);
		return -EINVAL;
	}

	ret = kiumd_set_dma_cookie_unlocked(cookie, cookie_type, iova_usr);
	if (ret)
		pr_err("%s failed to set cookie\n", __func__);

	trace_kiumd_iova_ctrl_end(iovausr.vfio_fd);

	return ret;
}

/**
 * @Brief: This function facilitates to
 * get the handle to a fd,fd to a handle
 * or closing the handle based on user space
 * arguments. API uses xarray to store
 * and retrieve handles.The function
 * is called via IOCTL interface
 * and input is provided via struct
 * kiumd_user from the user space.
 *
 * Parameters:
 * @arg: user space argument pointer
 *
 * Returns  handle/fd upon success and error codes on failure
 */
int kiumd_fd_dmabuf_handler(char __user *arg, struct file *fp)
{
	struct dma_buf_handle *dmabuf_xarray_entry;
	struct dma_buf_handle *dmabuf_handle;
	bool handle_available = false;
	struct dma_buf *kiumd_dmabuf;
	struct kiumd_ctx *kiumd_ctx;
	struct kiumd_user kiusr;
	unsigned long xa_index;
	unsigned long dmabuf;
	uint32_t local_id;
	void *xa_entry;
	void *ret;
	int err;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user))) {
		pr_err("%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	trace_kiumd_fd_dmabuf_handler_start(kiusr.handle, kiusr.dma_buf_fd);

	/* FD to Handle */
	if (kiusr.handle == FD_TO_HANDLE) {

		if (kiusr.dma_buf_fd < 0) {
			pr_err("%s: dma_buf_fd is invalid\n", __func__);
			return -EBADF;
		}

		mutex_lock(&kiumd_ctx->kiumd_xa_mutex);
		/* Retrieve struct dma_buf from FD*/
		dmabuf  = (unsigned long) dma_buf_get(kiusr.dma_buf_fd);
		if (((struct dma_buf *) dmabuf) == NULL) {
			pr_err("%s: dma_buf_get returns NULL\n", __func__);
			mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
			return -EINVAL;
		}

		/* Check if handle for buffer already exists, RCU lock acquired*/
		xa_for_each(&kiumd_ctx->kiumd_xa, xa_index, xa_entry) {
			dmabuf_xarray_entry = (struct dma_buf_handle *) xa_entry;
			if (dmabuf_xarray_entry->dmabuf == dmabuf) {
				handle_available = true;
				local_id = xa_index;
				atomic_inc(&dmabuf_xarray_entry->handle_refcount);
				break; //Handle found, exit the loop
			}
		}

		/* If Handle does not exist, allocate xa_array entry*/
		if (!handle_available) {
			dmabuf_handle = kzalloc(sizeof(*dmabuf_handle), GFP_KERNEL);
			if (!dmabuf_handle) {
				dma_buf_put((struct dma_buf *) dmabuf);
				mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
				return -ENOMEM;
			}
			dmabuf_handle->dmabuf = dmabuf;
			atomic_set(&dmabuf_handle->handle_refcount, 1);
			err = xa_alloc(&kiumd_ctx->kiumd_xa, &local_id,
				       (void *)dmabuf_handle, xa_limit_32b,
				       GFP_KERNEL);
			if (err < 0) {
				pr_err("%s:xarray alloc failure %d\n", __func__, err);
				dma_buf_put((struct dma_buf *) dmabuf);
				kfree(dmabuf_handle);
				mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
				return err;
			}
		} else {
			dmabuf_handle = xa_load(&kiumd_ctx->kiumd_xa, local_id);
			if (!dmabuf_handle) {
				pr_err("%s: dmabuf_handle is NULL\n", __func__);
				mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
				return -EINVAL;
			}
		}

		kiumd_dmabuf = (struct dma_buf *) dmabuf;
		kiusr.handle = local_id;
		mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
		trace_kiumd_fd_dmabuf_handler_fd_to_handle(kiusr.dma_buf_fd,
							   kiusr.handle,
							   atomic_read(&dmabuf_handle->handle_refcount));
	} else if (kiusr.dma_buf_fd == HANDLE_TO_FD) { /* Handle to FD */
		if (kiusr.handle < 0) {
			pr_err("%s: dmabuf handle is invalid\n", __func__);
			return -EINVAL;
		}
		mutex_lock(&kiumd_ctx->kiumd_xa_mutex);
		local_id = kiusr.handle;
		dmabuf_handle = xa_load(&kiumd_ctx->kiumd_xa, local_id);
		if (!dmabuf_handle) {
			pr_err("%s: dmabuf_handle is NULL\n", __func__);
			mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
			return -EINVAL;
		}
		if (!IS_ERR_OR_NULL((struct dma_buf *) dmabuf_handle->dmabuf)) {
			get_dma_buf((struct dma_buf *) dmabuf_handle->dmabuf);
			kiusr.dma_buf_fd = dma_buf_fd((struct dma_buf *) dmabuf_handle->dmabuf,
						      (O_CLOEXEC));
		}
		if (kiusr.dma_buf_fd < 0) {
			pr_err("%s:dma_buf_fd failed\n", __func__);
			mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
			return -EBADF;
		}

		mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
		trace_kiumd_fd_dmabuf_handler_handle_to_fd(kiusr.dma_buf_fd,
							   kiusr.handle,
							   atomic_read(&dmabuf_handle->handle_refcount));

	} else if (kiusr.dma_buf_fd == CLOSE_HANDLE) {  /* Close Handle */
		if (kiusr.handle < 0) {
			pr_err("%s: Invalid dma buf handle.\n", __func__);
			return -EINVAL;
		}

		mutex_lock(&kiumd_ctx->kiumd_xa_mutex);
		local_id = (int32_t)kiusr.handle;

		dmabuf_handle = xa_load(&kiumd_ctx->kiumd_xa, local_id);
		if (!dmabuf_handle) {
			pr_err("%s:Entry not available in xarray\n", __func__);
			mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
			return -EINVAL;
		}

		kiumd_dmabuf = ((struct dma_buf *)dmabuf_handle->dmabuf);
		if (atomic_dec_and_test(&dmabuf_handle->handle_refcount)) {

			if (!IS_ERR_OR_NULL(kiumd_dmabuf))
				dma_buf_put(kiumd_dmabuf);
			xa_erase(&kiumd_ctx->kiumd_xa, local_id);
			if (!dmabuf_handle) {
				kfree(dmabuf_handle);
				dmabuf_handle = NULL;
			}
		} else {
			if (!IS_ERR_OR_NULL(kiumd_dmabuf))
				dma_buf_put(kiumd_dmabuf);
		}
		kiusr.dma_buf_fd = 0;
		mutex_unlock(&kiumd_ctx->kiumd_xa_mutex);
	}

	trace_kiumd_fd_dmabuf_handler_end(kiusr.handle, kiusr.dma_buf_fd);

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed...\n", __func__);
		return -EFAULT;
	}

	return 0;
}

/**
 * @Brief: This function facilitates to hyp-assign the page for given vmid
 *
 * Parameters:
 * @vmid: user space argument pointer
 * @page: Page
 * @nr_acl_entries: Number of acl entries for scm assign
 *
 * Returns  0 upon success and error codes on failure
 */
static int kiumd_io_pgtable_hyp_assign_page(u32 *vmid, u64 page,
					    u32 nr_acl_entries)
{
	u64 src_vmid_list = BIT(QCOM_SCM_VMID_HLOS);
	struct qcom_scm_vmperm *dst_vmids;
	int ret, i;

	trace_kiumd_io_pgtable_hyp_assign_page_start(vmid, page, nr_acl_entries);

	dst_vmids = kcalloc((nr_acl_entries + 1), sizeof(*dst_vmids),
			    GFP_KERNEL);
	if (!dst_vmids)
		return -ENOMEM;

	i = 0;
	dst_vmids[i].vmid = QCOM_SCM_VMID_HLOS;
	dst_vmids[i].perm = QCOM_SCM_PERM_RW;
	pr_debug("Hyp assign page for dst:%d vmid:%d perm:%d total vmids:%d\n",
		 i, dst_vmids[i].vmid, dst_vmids[i].perm, nr_acl_entries + 1);

	for (i = 1; i < nr_acl_entries + 1; i++) {
		dst_vmids[i].vmid = vmid[i - 1];
		dst_vmids[i].perm = QCOM_SCM_PERM_READ;
		pr_debug("Hyp assign page for dst:%d vmid:%d perm:%d\n",
			 i, dst_vmids[i].vmid, dst_vmids[i].perm);
	}

	ret = qcom_scm_assign_mem(page, PAGE_SIZE, &src_vmid_list, dst_vmids,
				  nr_acl_entries + 1);
	if (ret)
		pr_err("hyp assign for %llu address of size %lx rc:%d\n",
		       page, PAGE_SIZE, ret);
	kfree(dst_vmids);

	trace_kiumd_io_pgtable_hyp_assign_page_end(page, nr_acl_entries);
	return ret;
}

/**
 * @Brief: This function facilitates to hyp-unassign
 * the page for given vmid
 *
 * Parameters:
 * @vmid: user space argument pointer
 * @page: Page
 * @nr_acl_entries: Number of acl entries for scm assign
 *
 * Returns  0 upon success and error codes on failure
 */
static int kiumd_io_pgtable_hyp_unassign_page(u32 *vmid, u64 page,
		u32 nr_acl_entries)
{
	u64 src_vmid_list = BIT(QCOM_SCM_VMID_HLOS);
	int ret;

	trace_kiumd_io_pgtable_hyp_unassign_page_start(vmid, page, nr_acl_entries);

	struct qcom_scm_vmperm dst_vmids[] = { {
			QCOM_SCM_VMID_HLOS,
			QCOM_SCM_PERM_RWX
		}
	};
	for (int i = 0; i < nr_acl_entries ; i++) {
		src_vmid_list |= BIT(vmid[i]);
		pr_debug("Hyp unassign page for dst:%d vmid:%d\n",
			 i, vmid[i]);
	}

	ret = qcom_scm_assign_mem(page, PAGE_SIZE, &src_vmid_list,
				  dst_vmids, ARRAY_SIZE(dst_vmids));
	if (ret)
		pr_err("hyp unassign failed %llu address of size %lx rc:%d\n",
		       page, PAGE_SIZE, ret);

	trace_kiumd_io_pgtable_hyp_unassign_page_end(page, nr_acl_entries);
	return ret;
}

/**
 * @Brief: This function facilitates to
 * transfer memory ownership for
 * given sg for source vm list
 *
 * Parameters:
 * @sgt: user space argument pointer
 * @source_vm_list: Page
 * @source_nelems: Number of acl entries for scm assign
 * @clear_page_private: boolean flag to check sg private
 * page clearance
 *
 * Returns  0 upon success and error codes on failure
 */
static int kiumd_hyp_unassign_sg(struct sg_table *sgt, int *source_vm_list,
								 int source_nelems, bool clear_page_private)
{
	u64 src_vmid_list = 0, src_vmid_list_copy;
	struct qcom_scm_vmperm dst_vmids[] = { {
			QCOM_SCM_VMID_HLOS,
			QCOM_SCM_PERM_RWX
		}
	};
	struct scatterlist *sg;
	int ret, i;

	if (source_nelems <= 0)
		return -EINVAL;

	if (!sgt)
		return -EINVAL;

	if (!sgt->sgl)
		return -EINVAL;

	trace_kiumd_hyp_unassign_sg_start(sgt);
	sg = sgt->sgl;

	for (int j = 0; j < source_nelems ; j++) {
		src_vmid_list |= BIT(source_vm_list[j]);
		pr_debug("Hyp unassign sg for dst:%d vmid:%d\n",
			 j, source_vm_list[j]);
	}
	src_vmid_list_copy = src_vmid_list;
	do {
		src_vmid_list = src_vmid_list_copy;
		pr_debug("%s: memory ownership transfer start src vmid:%llx\n",
			 __func__, src_vmid_list);
		ret = qcom_scm_assign_mem(page_to_phys(sg_page(sg)), sg->length,
					  &src_vmid_list, dst_vmids,
					  ARRAY_SIZE(dst_vmids));
		if (ret) {
			pr_err("Hyp unassign failed %llu address of size %x rc:%d\n",
			       page_to_phys(sg_page(sg)), sg->length, ret);
			goto out;
		}
		pr_debug("%s: memory ownership transfer end:%d\n", __func__, ret);
		sg = sg_next(sg);
	} while (sg);

	if (clear_page_private)
		for_each_sg(sgt->sgl, sg, sgt->nents, i) {
			if (sg)
				ClearPagePrivate(sg_page(sg));
		}

	trace_kiumd_hyp_unassign_sg_end(sgt);
out:
	return ret;
}

/**
 * @Brief: This function facilitates to hyp-assign
 * given sg for destination vm list
 *
 * Parameters:
 * @sgt: scatter gather table ptr
 * @dest_vm_list: destination vm list
 * @dest_nelems: Number of entries for destination
 * @set_page_private: page flag
 * @dest_perms: Destination permission
 *
 * Returns  0 upon success and error codes on failure
 */
static int kiumd_hyp_assign_sg(struct sg_table *sgt, int *dest_vm_list,
			       int dest_nelems, bool set_page_private,
			       int *dest_perms)
{
	struct qcom_scm_vmperm *dst_vmids;
	struct scatterlist *sg;
	u64 src_vmid_list;
	int ret;

	if (dest_nelems <= 0) {
		pr_err("%s: dest_nelems invalid\n", __func__);
		return -EINVAL;
	}

	if (!sgt)
		return -EINVAL;

	trace_kiumd_hyp_assign_sg_start(sgt);
	sg = sgt->sgl;
	if (!sg)
		return -EINVAL;

	dst_vmids = kcalloc(dest_nelems, sizeof(*dst_vmids), GFP_KERNEL);
	if (!dst_vmids)
		return -ENOMEM;

	for (int i = 0; i < dest_nelems; i++) {
		dst_vmids[i].vmid = dest_vm_list[i];
		dst_vmids[i].perm = dest_perms[i];
		pr_debug("Hyp assign sg for dst:%d vmid:%d perm:%d\n",
			 i, dst_vmids[i].vmid, dst_vmids[i].perm);
	}

	do {
		src_vmid_list = BIT(QCOM_SCM_VMID_HLOS);
		pr_debug("Assign call initiated :%llx\n", src_vmid_list);
		ret = qcom_scm_assign_mem(page_to_phys(sg_page(sg)), sg->length, &src_vmid_list,
					  dst_vmids, dest_nelems);
		if (ret) {
			pr_err("failed qcom_assign for assigning %llx address of size %x rc:%d\n",
			       page_to_phys(sg_page(sg)), sg->length, ret);
			goto err;
		}
		pr_debug("Assign call success:%d\n", ret);
		sg = sg_next(sg);
	} while (sg);
	pr_debug("%s success\n", __func__);
err:
	kfree(dst_vmids);

	trace_kiumd_hyp_assign_sg_end(sgt);
	return ret;
}

/**
 * @Brief: This function facilitates to set the
 * destination vmids and permissions to given
 * vmids and permissions.
 *
 * Parameters:
 * @nr_acl_entries: Number of acl entries for scm assign
 * @acl_entries: acl entries for scm assign
 * @dst_vmids: pointer to destination vmid
 * @dest_perms: Destination permission
 *
 * Returns  0 upon success and error codes on failure
 */
int kiumd_acl_to_vmid_perms_list(unsigned int nr_acl_entries,
				 const void __user *acl_entries,
				 int **dst_vmids, int **dst_perms)
{
	struct kiumd_acl_entry entry;
	int ret, i, *vmids, *perms;

	if (!nr_acl_entries || !acl_entries) {
		pr_err("%s:%d Invalid params entries:%d\n", __func__, __LINE__, nr_acl_entries);
		return -EINVAL;
	}

	if (nr_acl_entries > MAX_KIUMD_ACL_ENTRIES) {
		pr_err("%s:%d Invalid params\n", __func__, __LINE__);
		return -EINVAL;
	}

	vmids = kmalloc_array(nr_acl_entries, sizeof(*vmids), GFP_KERNEL);
	if (!vmids)
		return -ENOMEM;

	perms = kmalloc_array(nr_acl_entries, sizeof(*perms), GFP_KERNEL);
	if (!perms) {
		kfree(vmids);
		return -ENOMEM;
	}

	for (i = 0; i < nr_acl_entries; i++) {
		ret = copy_struct_from_user(&entry, sizeof(entry),
					    acl_entries + (sizeof(entry) * i),
					    sizeof(entry));
		if (ret < 0) {
			pr_err("%s:%d Invalid params\n", __func__, __LINE__);
			goto out;
		}

		vmids[i] = entry.vmid;
		perms[i] = entry.perms;
		pr_debug("%d vmid:%d perms:%d\n", i, vmids[i], perms[i]);
		if (vmids[i] < 0 || perms[i] < 0 ||
		    vmids[i] > KIUMD_MAX_VMID ||
		    perms[i] > KIUMD_MAX_PERMS) {
			ret = -EINVAL;
			goto out;
		}
	}

	*dst_vmids = vmids;
	*dst_perms = perms;
	return ret;

out:
	kfree(perms);
	kfree(vmids);
	return ret;
}

/**
 * @Brief: This function facilitates to get the page
 * table global directory by getting iommu_domain,
 * smmu_domain and pagetable
 *
 * Parameters:
 * @vfio_fd: file descriptor for vfio device
 * @pgd: page global directory pointer
 *
 * Returns  0 upon success and error codes on failure
 */
int kiumd_get_pgd(struct vfio_device *vfio_dev, u64 *pgd)
{
	struct arm_smmu_domain *smmu_dom;
	struct iommu_domain *iommu_dom;
	struct io_pgtable *pgtable;

	if (!pgd) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		return -EINVAL;
	}

	iommu_dom = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!iommu_dom) {
		pr_err("%s:%d Failed to get IOMMU DOMAIN VFIO\n",
		       __func__, __LINE__);
		return -EINVAL;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if (!smmu_dom || !smmu_dom->pgtbl_ops) {
		pr_err("%s:%d failed to get smmu_dom\n", __func__, __LINE__);
		return -EINVAL;
	}

	pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if (!pgtable) {
		pr_err("%s:%d failed to get pgtabl ops\n", __func__, __LINE__);
		return -EINVAL;
	}

	*pgd = pgtable->cfg.arm_lpae_s1_cfg.ttbr;

	return 0;
}

/**
 * @Brief: This function facilitates the hyp assigning  of a system heap
 * allocated dmabuffer to a SMMU backed device represented via a vfio_device.
 * The function is called via IOCTL interface and input is provided via struct
 * kiumd_user from the user space.
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * return value is errno in failure cases or 0 in case of successful mapping
 */
int kiumd_dmabuf_assign_buf(char __user *arg, struct file *fp)
{
	struct dma_buf_attachment *dmabufattach;
	struct kiumd_secure_map_context *map_ctx;
	struct vfio_device *vfio_dev;
	struct dma_buf *kiumd_dmabuf;
	struct kiumd_ctx *kiumd_ctx;
	struct hyp_map_data *smap;
	struct kiumd_user kiusr;
	struct sg_table *sgt;
	int *vmids, *perms;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	vfio_dev = kiumd_get_vfio_device(kiusr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:%d invalid vfio device fd\n", __func__, __LINE__);
		return -EINVAL;
	}

	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		ret = !kiumd_dmabuf ? -EINVAL : PTR_ERR(kiumd_dmabuf);
		return ret;
	}

	ret = kiumd_acl_to_vmid_perms_list(kiusr.mem_parcel.nr_acl_entries,
					   (void *)kiusr.mem_parcel.acl_list,
					   &vmids, &perms);
	if (ret) {
		pr_err("%s:%d Invalid params\n", __func__, __LINE__);
		return ret;
	}

	dmabufattach = dma_buf_attach(kiumd_dmabuf, vfio_dev->dev);
	if (IS_ERR(dmabufattach)) {
		pr_err("%s:%d dmabufattach is invalid\n", __func__, __LINE__);
		ret = PTR_ERR(dmabufattach);
		goto free_mem;
	}

	if (!dmabufattach->priv) {
		ret = -EINVAL;
		pr_err("%s:%d dma heap attachment is NULL\n", __func__, __LINE__);
		goto detach;
	}

	sgt = ((struct kiumd_dma_heap_attachment *)(dmabufattach->priv))->table;
	if (!sgt) {
		pr_err("%s:%d sgt is NULL\n", __func__, __LINE__);
		ret = -EINVAL;
		goto detach;
	}

	if (!sgt->sgl) {
		ret = -EINVAL;
		pr_err("%s:%d sgl is NULL\n", __func__, __LINE__);
		goto detach;
	}

	pr_debug("%s:sgt from attachment:%p %llx\n", __func__, sgt, sg_phys(sgt->sgl));

	ret = kiumd_hyp_assign_sg(sgt, vmids, kiusr.mem_parcel.nr_acl_entries,
				  true, perms);
	if (ret < 0) {
		pr_err("%s:%d ownership transfer error\n", __func__, __LINE__);
		goto detach;
	}

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap) {
		ret = -ENOMEM;
		goto hyp_unassign_sg;
	}
	smap->dmabufattach = (long)dmabufattach;
	smap->sgt_ptr = (long)sgt;
	smap->dmabuf_ptr = (long)kiumd_dmabuf;

	mutex_lock(&kiumd_ctx->hyp_lock);
	smap->id = kiumd_ctx->hyp_idx++;
	hash_add(kiumd_ctx->hyp_table, &smap->node, smap->id);

	map_ctx = kzalloc(sizeof(*map_ctx), GFP_KERNEL);
	if (!map_ctx) {
		ret = -ENOMEM;
		mutex_unlock(&kiumd_ctx->hyp_lock);
		goto hyp_unassign_sg;
	}
	map_ctx->nr_acl_entries = kiusr.mem_parcel.nr_acl_entries;
	map_ctx->vmids = vmids;
	map_ctx->perms = perms;

	smap->secure_ctx = map_ctx;

	mutex_unlock(&kiumd_ctx->hyp_lock);

	pr_debug("maping attachment sgt:%lx attachmemt:%lx dmabuf:%lx, id: %d\n",
		 smap->sgt_ptr, smap->dmabufattach, smap->dmabuf_ptr, smap->id);
	kiusr.hyp_id = smap->id;
	kiusr.dma_addr = sg_dma_address(sgt->sgl);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s:%d copy_to_user failed...\n", __func__, __LINE__);
		ret = -EFAULT;
		goto hyp_unassign_sg;
	}

	pr_debug("returning from ioctl ret:%d dma add:%x\n", ret, kiusr.dma_addr);
	return 0;
hyp_unassign_sg:
	kiumd_hyp_unassign_sg((struct sg_table *)sgt, vmids,
			      kiusr.mem_parcel.nr_acl_entries, true);
detach:
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);
free_mem:
	kfree(vmids);
	kfree(perms);
	return ret;
}

/**
 * @Brief: This function facilitates the hyp unassigning  of a system heap
 * allocated dmabuffer to a SMMU backed device represented via a vfio_device.
 * The function is called via IOCTL interface and input is provided via struct
 * kiumd_user from the user space.
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * return value is errno in failure cases or 0 in case of successful mapping
 */
int kiumd_dmabuf_unassign_buf(char __user *arg, struct file *fp)
{
	struct kiumd_secure_map_context *map_ctx;
	struct dma_buf_attachment *dmabufattach;
	struct dma_buf *kiumd_dmabuf;
	struct kiumd_ctx *kiumd_ctx;
	struct hyp_map_data *smap;
	struct kiumd_user kiusr;
	int *vmids, *perms;
	bool found = false;
	int ret;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	if (kiusr.hyp_id < 0) {
		pr_err("%s:id passed from user should be positive value\n", __func__);
		return -EFAULT;
	}

	mutex_lock(&kiumd_ctx->hyp_lock);
	hash_for_each_possible(kiumd_ctx->hyp_table, smap, node, kiusr.hyp_id) {
		if (smap->id == kiusr.hyp_id) {
			found = true;
			break;
		}
	}

	hash_del(&smap->node);
	mutex_unlock(&kiumd_ctx->hyp_lock);
	if (!found) {
		pr_err("%s:Id not found id: %d\n", __func__, kiusr.hyp_id);
		return -ENOENT;
	}

	map_ctx = smap->secure_ctx;
	if (!map_ctx) {
		pr_err("%s:Invalid ctx\n", __func__);
		return -EINVAL;
	}

	vmids = map_ctx->vmids;
	perms = map_ctx->perms;

	if (!smap->sgt_ptr) {
		ret = -EINVAL;
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		goto err;
	}

	ret = kiumd_hyp_unassign_sg((struct sg_table *)smap->sgt_ptr, vmids,
				    map_ctx->nr_acl_entries, true);
	if (ret < 0) {
		pr_err("%s:%d memory ownership transfer error:%d\n", __func__, __LINE__, ret);
		goto err;
	}


	kfree(vmids);
	kfree(perms);

	dmabufattach = (struct dma_buf_attachment *)smap->dmabufattach;
	if (!dmabufattach) {
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto err;
	}
	pr_debug("kiumd secure unmap:sgt from attachment:%p\n",
		 ((struct kiumd_dma_heap_attachment *)(dmabufattach->priv))->table);

	kiumd_dmabuf = (struct dma_buf *)smap->dmabuf_ptr;
	if (!kiumd_dmabuf) {
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto err;
	}
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);

	mutex_lock(&kiumd_ctx->hyp_lock);
	kfree(smap->secure_ctx);
	kfree(smap);
	mutex_unlock(&kiumd_ctx->hyp_lock);
	pr_debug("%s: Hyp unassign done\n", __func__);
err:
	return ret;
}

int kiumd_dmabuf_vfio_secure_map(char __user *arg, struct file *fp)
{
	struct dma_buf_attachment *dmabufattach;
	struct vfio_device *vfio_dev;
	struct dma_buf *kiumd_dmabuf;
	struct vfio_device_file *df;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	int kiumd_dma_direction;
	struct sg_table *sgt;
	int *vmids, *perms;
	struct file *file;
	u64 pgd;
	int ret;

	pr_debug(" %s: Entering.....\n", __func__);
	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user))) {
		pr_err("%s:%d bad params from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	if (kiusr.vfio_fd < 0) {
		pr_err("%s:%d invalid fd from user\n", __func__, __LINE__);
		return -EBADF;
	}

	file = fget(kiusr.vfio_fd);
	if (!file) {
		pr_err("%s:%d failed to get file from vfio fd\n",
		       __func__, __LINE__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:%d vfio_dev is NULL\n", __func__, __LINE__);
		ret = -EINVAL;
		goto close_file;
	}

	if (!vfio_dev->dev) {
		ret = -EINVAL;
		pr_err("%s:%d invalid device\n", __func__, __LINE__);
		goto close_file;
	}

	ret = kiumd_get_pgd(vfio_dev, &pgd);
	if (ret) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		goto close_file;
	}

	pr_debug("Kiumd VM page table ttbr:%llx\n", pgd);

	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	if (IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		ret = !kiumd_dmabuf ? -EINVAL : PTR_ERR(kiumd_dmabuf);
		goto close_file;
	}

	trace_kiumd_dmabuf_vfio_secure_map_start(kiusr.vfio_fd,
						 kiusr.dma_buf_fd,
						 kiumd_dmabuf->size);

	ret = kiumd_acl_to_vmid_perms_list(kiusr.mem_parcel.nr_acl_entries,
					   (void *)kiusr.mem_parcel.acl_list, &vmids, &perms);
	if (ret) {
		pr_err("%s:%d Invalid params\n", __func__, __LINE__);
		goto close_file;
	}

	dmabufattach = dma_buf_attach(kiumd_dmabuf, vfio_dev->dev);
	if (IS_ERR(dmabufattach)) {
		pr_err("%s:%d dmabufattach is invalid\n", __func__, __LINE__);
		ret = PTR_ERR(dmabufattach);
		goto free_mem;
	}

	if (!dmabufattach->priv) {
		ret = -EINVAL;
		pr_err("%s:%d dma heap attachment is NULL\n", __func__, __LINE__);
		goto detach;
	}

	sgt = ((struct kiumd_dma_heap_attachment *)(dmabufattach->priv))->table;
	if (!sgt) {
		pr_err("%s:%d sgt is NULL\n", __func__, __LINE__);
		ret = -EINVAL;
		goto detach;
	}

	if (!sgt->sgl) {
		ret = -EINVAL;
		pr_err("%s:%d sgl is NULL\n", __func__, __LINE__);
		goto detach;
	}

	pr_debug("%s:sgt from attachment:%p %llx\n", __func__, sgt, sg_phys(sgt->sgl));

	/* Grant Page table read access to peripheral VM*/
	ret = kiumd_io_pgtable_hyp_assign_page(vmids, pgd,
					       kiusr.mem_parcel.nr_acl_entries);
	if (ret < 0) {
		pr_err("%s:%d ownership transfer error:%d\n", __func__, __LINE__, ret);
		goto detach;
	}
	pr_debug("Pgtable ownership transfer success\n");

	ret = kiumd_hyp_assign_sg(sgt, vmids, kiusr.mem_parcel.nr_acl_entries,
				  true, perms);
	if (ret < 0) {
		pr_err("%s:%d ownership transfer error\n", __func__, __LINE__);
		goto hyp_unassign_table;
	}

	if (kiusr.dma_direction == 1)
		kiumd_dma_direction = kiusr.dma_direction;
	else
		kiumd_dma_direction = 0;

	sgt = dma_buf_map_attachment_unlocked(dmabufattach, kiumd_dma_direction);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		pr_err("%s:%d sgt is invalid\n", __func__, __LINE__);
		goto hyp_unassign_sg;
	}

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap) {
		ret = -ENOMEM;
		goto close_file;
	}
	smap->dmabufattach = (long)dmabufattach;
	smap->sgt_ptr = (long)sgt;
	smap->dmabuf_ptr = (long)kiumd_dmabuf;
	spin_lock(&kiumd_ctx->smmu_lock);
	smap->id = kiumd_ctx->id++;
	hash_add(kiumd_ctx->smmu_table, &smap->node, smap->id);
	spin_unlock(&kiumd_ctx->smmu_lock);

	kiusr.id = smap->id;
	kiusr.dma_addr = sg_dma_address(sgt->sgl);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s:%d copy_to_user failed...\n", __func__, __LINE__);
		ret = -EFAULT;
		goto unmap;
	}

	kfree(vmids);
	kfree(perms);
	fput(file);
	pr_debug("returning from ioctl ret:%d\n", ret);
	trace_kiumd_dmabuf_vfio_secure_map_end(kiusr.vfio_fd, kiusr.id, kiusr.dma_addr);
	return 0;
unmap:
	dma_buf_unmap_attachment_unlocked(dmabufattach, (struct sg_table *)sgt,
					  DMA_BIDIRECTIONAL);
hyp_unassign_sg:
	kiumd_hyp_unassign_sg((struct sg_table *)sgt, vmids,
			      kiusr.mem_parcel.nr_acl_entries, true);
hyp_unassign_table:
	kiumd_io_pgtable_hyp_unassign_page(vmids, pgd, kiusr.mem_parcel.nr_acl_entries);
detach:
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);
free_mem:
	kfree(vmids);
	kfree(perms);
close_file:
	fput(file);
	return ret;
}

/**
 * @Brief: This function facilitates the
 * secure unmapping of a DMA-BUF based
 * buffer to a SMMU backed device
 * represented via a vfio_device.
 * The function is called via IOCTL
 * interface and input is provided via
 * struct kiumd_user from the user space.
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * return value is errno in failure cases
 * or 0 in case of successful mapping
 */
int kiumd_dmabuf_vfio_secure_unmap(char __user *arg, struct file *fp)
{
	struct dma_buf_attachment *dmabufattach;
	struct dma_buf *kiumd_dmabuf;
	struct vfio_device *vfio_dev;
	struct kiumd_ctx *kiumd_ctx;
	struct vfio_device_file *df;
	struct smmu_map_data *smap;
	struct kiumd_user kiusr;
	int *vmids, *perms;
	bool found = false;
	struct file *file;
	u64 pgd;
	int ret;

	pr_debug("%s entering\n", __func__);
	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}
	trace_kiumd_dmabuf_vfio_secure_unmap_start(kiusr.vfio_fd,
						   kiusr.dma_buf_fd,
						   kiusr.ptselect,
						   kiusr.dma_addr,
						   kiusr.id);
	if (kiusr.id < 0) {
		pr_err("%s:id passed from user should be positive value\n", __func__);
		return -EFAULT;
	}

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_for_each_possible(kiumd_ctx->smmu_table, smap, node, kiusr.id) {
		if (smap->id == kiusr.id) {
			found = true;
			break;
		}
	}

	if (!found) {
		spin_unlock(&kiumd_ctx->smmu_lock);
		return -ENOENT;
	}
	spin_unlock(&kiumd_ctx->smmu_lock);
	if (kiusr.vfio_fd < 0) {
		pr_err("%s:%d invalid vfio fd\n", __func__, __LINE__);
		return -EBADF;
	}

	file = fget(kiusr.vfio_fd);
	if (!file) {
		pr_err("%s:%d failed to get file from vfio fd\n", __func__, __LINE__);
		return -EBADF;
	}

	df = (struct vfio_device_file *)file->private_data;
	vfio_dev = (struct vfio_device *)df->device;
	if (!vfio_dev) {
		pr_err("%s:%d vfio_dev is NULL\n", __func__, __LINE__);
		ret = -EINVAL;
		goto close_file;
	}

	if (!vfio_dev->dev) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		ret = -EINVAL;
		goto close_file;
	}

	ret = kiumd_get_pgd(vfio_dev, &pgd);
	if (ret) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		goto close_file;
	}

	/*Input param error checking for ACL happens in kiumd_acl_to_vmid_perms*/
	ret = kiumd_acl_to_vmid_perms_list(kiusr.mem_parcel.nr_acl_entries,
					   (void *)kiusr.mem_parcel.acl_list,
					   &vmids, &perms);
	if (ret) {
		pr_err("%s:%d invalid params\n", __func__, __LINE__);
		goto close_file;
	}

	dmabufattach = (struct dma_buf_attachment *)smap->dmabufattach;
	if (!dmabufattach) {
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto free_mem;
	}

	if (!smap->sgt_ptr) {
		ret = -EINVAL;
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		goto free_mem;
	}

	dma_buf_unmap_attachment_unlocked(dmabufattach,
					  (struct sg_table *)smap->sgt_ptr,
					  DMA_BIDIRECTIONAL);

	ret = kiumd_hyp_unassign_sg((struct sg_table *)smap->sgt_ptr, vmids,
				    kiusr.mem_parcel.nr_acl_entries, true);
	if (ret < 0) {
		pr_err("%s:%d memory ownership transfer error:%d\n", __func__, __LINE__, ret);
		goto free_mem;
	}

	ret = kiumd_io_pgtable_hyp_unassign_page(vmids, pgd,
						 kiusr.mem_parcel.nr_acl_entries);
	if (ret < 0) {
		pr_err("%s:%d memory ownership transfer error:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto free_mem;
	}

	pr_debug("memory ownership transfer success\n");

	kiumd_dmabuf = (struct dma_buf *)smap->dmabuf_ptr;
	if (!kiumd_dmabuf) {
		pr_err("%s:%d invalid params:%d\n", __func__, __LINE__, ret);
		ret = -EINVAL;
		goto free_mem;
	}
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	kfree(smap);
	spin_unlock(&kiumd_ctx->smmu_lock);
free_mem:
	kfree(vmids);
	kfree(perms);
close_file:
	fput(file);

	trace_kiumd_dmabuf_vfio_secure_unmap_end(kiusr.vfio_fd, kiusr.dma_buf_fd);
	return ret;
}

/**
 * @Brief: This function facilitates to get the faulty iova FSR value
 * when user reads the sysfs node as a result of smmu faults.
 *
 * Parameters:
 * @kobj: kernel object pointer of device
 * @attr: kernel object attribute
 * @buf: user space buffer
 *
 * return value is number of bytes successfully written
 */

static ssize_t fsr_iova_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	struct smmu_device_obj *temp = head;
	int local_fsr, local_iova;

	while (temp != NULL && kobj != NULL) {
		if (!strcmp(temp->kobj->name, kobj->name)) {
			local_fsr = temp->smmu_fsr;
			local_iova = temp->smmu_iova;
			temp->smmu_fsr = 0;
			temp->smmu_iova = 0;
			temp->flag = 0;
			return snprintf(buf, PAGE_SIZE, "0x%x:0x%x\n",
					local_fsr, local_iova);
		}
		temp = temp->next;
	}

	return 0;
}

static struct kobj_attribute fsr_iova_attribute = {
	.attr = {
		.name = "fsr_iova",
		.mode = 0644,
	},
	.show = fsr_iova_show,
};

/**
 * @Brief: This function should be used by IOMMU users which want to be notified
 * whenever an IOMMU fault happens.
 *
 * Parameters:
 * @iommu_domain: iommu domain
 * @dev: device structure
 * @iova: fault address
 * @flags: flags
 * @token: user data
 *
 * The fault handler itself should return 0 on success, and an appropriate
 * error code otherwise.
 */
static int kiumd_smmu_fault_handler(struct iommu_domain *iomm_domain,
				    struct device *dev, unsigned long iova,
				    int flags, void *token)
{
	struct smmu_device_obj *temp = head;
	struct arm_smmu_domain *smmu_domain;
	char *name = (char *)token;
	struct arm_smmu_cfg *cfg;
	int retval = 0;

	smmu_domain = container_of(iomm_domain, struct arm_smmu_domain, domain);
	if ((!smmu_domain) || (!(smmu_domain->pgtbl_ops))) {
		pr_err("%s:smmu domain/pagetable ops is invalid\n", __func__);
		return -EINVAL;
	}

	cfg = &smmu_domain->cfg;
	while (temp != NULL) {
		if (!strcmp(temp->kobj->name, name) && !(temp->flag)) {
			temp->smmu_fsr = arm_smmu_cb_read(smmu_domain->smmu,
							  cfg->cbndx,
							  ARM_SMMU_CB_FSR);
			temp->smmu_iova = iova;
			temp->flag = 1;
			sysfs_notify(temp->kobj, NULL, "fsr_iova");
			return 0;
		}
		temp = temp->next;
	}

	return 0;
}

/**
 * @Brief: This function should be used by IOMMU users which want to deregister
 * an IOMMU fault handler.
 *
 * Parameters:
 * @arg: user data
 *
 * return 0 on success, or an appropriate
 * error code otherwise.
 */
static int kiumd_smmu_fault_handler_deregister(char __user *arg)
{
	struct smmu_device_obj *prev = NULL, *temp = head;
	struct vfio_device *vfio_dev;
	struct kiumd_user kiusr;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	vfio_dev = kiumd_get_vfio_device(kiusr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		return -ENOTTY;
	}

	if (temp != NULL &&
	    !strcmp(temp->kobj->name, vfio_dev->dev->kobj.name)) {
		head = head->next;
		temp->next = NULL;
		kobject_put(temp->kobj);
		kfree(temp);
	} else {
		while (temp != NULL &&
		       strcmp(temp->kobj->name,
			      vfio_dev->dev->kobj.name)) {
			prev = temp;
			temp = temp->next;
		}
		if (temp == NULL) {
			pr_err("%s:vfio device is not present in list to deregister\n");
			return -ENOENT;
		}
		if (prev)
			prev->next = temp->next;
		temp->next = NULL;
		kobject_put(temp->kobj);
		kfree(temp);
	}

	return 0;
}

/**
 * @Brief: This function should be used by IOMMU users which want to register
 * an IOMMU fault handler.
 *
 * Parameters:
 * @arg: user data
 *
 * return 0 on success, or an appropriate
 * error code otherwise.
 */
static int kiumd_smmu_fault_handler_register(char __user *arg)
{
	struct vfio_device *vfio_dev;
	struct smmu_device_obj *temp;
	struct iommu_domain *domain;
	struct smmu_device_obj *ptr;
	struct kiumd_user kiusr;
	int ret;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	vfio_dev = kiumd_get_vfio_device(kiusr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:vfio_dev is NULL\n", __func__);
		return -ENOTTY;
	}

	domain = kiumd_iommu_get_dma_domain(vfio_dev->dev);
	if (!domain) {
		pr_err("%s:iommu domain is NULL\n", __func__);
		return -EINVAL;
	}

	device_obj = kobject_create_and_add(vfio_dev->dev->kobj.name, smmu_obj);
	if (!device_obj) {
		pr_err("/sys/kernel/smmu_fault/%s creation failed\n",
		       vfio_dev->dev->kobj.name);
		return -ENOMEM;
	}

	ret = sysfs_create_file(device_obj, &(fsr_iova_attribute.attr));
	if (ret) {
		pr_err("failed to create sysfs file in /sys/kernel/\n");
		return ret;
	}

	ptr = kzalloc(sizeof(*ptr), GFP_KERNEL);
	if (ptr == NULL)
		return -ENOMEM;

	ptr->kobj = device_obj;
	ptr->flag = 0;
	ptr->next = NULL;

	if (head == NULL) {
		head = ptr;
	} else {
		temp = head;
		while (temp->next != NULL)
			temp = temp->next;
		temp->next = ptr;
	}

	iommu_set_fault_handler(domain, kiumd_smmu_fault_handler, device_obj->name);

	return 0;
}


/**
 * @Brief: This function facilitates to
 * map mmio region into smmu at fixed
 * IOVA.The function is called via IOCTL
 * interface and input is provided via struct
 * kiumd_user from the user space.
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * return value is errno in failure cases
 * or 0 in case of successful mapping
 */
static int kiumd_mmio_smmu_map(char __user *arg, struct file *fp)
{
	struct kiumd_iommu_dma_cookie *cookie;
	struct kiumd_smmu_mmio_ctx *mmio_ctx;
	struct kiumd_smmu_mmio_map kiusr;
	struct vfio_device *vfio_dev;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	int ret, iter, retval = 0;
	struct resource *res;
	dma_addr_t dma_addr;
	char *reg_name;
	u64 addr, size;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	trace_kiumd_mmio_smmu_map_start(kiusr.vfio_fd, kiusr.fixed_iova, kiusr.iova);

	vfio_dev = kiumd_get_vfio_device(kiusr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:%d invalid vfio device fd\n", __func__, __LINE__);
		return -EINVAL;
	}

	cookie = kiumd_get_dma_cookie(vfio_dev);
	if (!cookie) {
		pr_err("%s:cookie not found\n", __func__);
		return -EINVAL;
	}
	spin_lock(&kiumd_ctx->smmu_lock);
	if (!hash_empty(kiumd_ctx->smmu_table)) {
		hash_for_each(kiumd_ctx->smmu_table, iter, smap, node) {
			if (!smap->context)
				continue;
			pr_debug("mmio ctx%p iova:%llx size:%lx\n", smap->context,
				 ((struct kiumd_smmu_mmio_ctx *)smap->context)->iova,
				 ((struct kiumd_smmu_mmio_ctx *)smap->context)->size);
			if (kiusr.fixed_iova &&
			    ((struct kiumd_smmu_mmio_ctx *)smap->context)->iova == kiusr.iova) {
				spin_unlock(&kiumd_ctx->smmu_lock);
				pr_err("%s:error IOVA:%llx exists..\n", __func__, kiusr.iova);
				return -EINVAL;
			}
		}
	}
	spin_unlock(&kiumd_ctx->smmu_lock);

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap)
		return -ENOMEM;

	mmio_ctx = kzalloc(sizeof(*mmio_ctx), GFP_KERNEL);
	if (!mmio_ctx) {
		kfree(smap);
		return -ENOMEM;
	}

	reg_name = strndup_user(kiusr.reg_name, KIUMD_MAX_REG_NAME_LEN);
	if (IS_ERR(reg_name)) {
		pr_err("%s:%d invalid str\n", __func__, __LINE__);
		ret = -EINVAL;
		goto smap_free;
	}

	if (!dev_is_platform(vfio_dev->dev)) {
		pr_err("%s:%d not platform device\n", __func__, __LINE__);
		ret = -EINVAL;
		goto reg_free;
	}

	res = platform_get_resource_byname(to_platform_device(vfio_dev->dev),
					   IORESOURCE_MEM, reg_name);
	if (!res) {
		ret = -EINVAL;
		pr_err("%s:%d resource error\n", __func__, __LINE__);
		goto reg_free;
	}
	addr = res->start;
	size = resource_size(res);

	if (kiusr.fixed_iova) {
		mutex_lock(&cookie->mutex);
		ret = kiumd_set_dma_cookie(cookie, IOMMU_DMA_MSI_COOKIE, kiusr.iova);
		if (ret) {
			mutex_unlock(&cookie->mutex);
			ret = -EFAULT;
			goto reg_free;
		}
	}

	dma_addr = dma_map_resource(vfio_dev->dev, addr, size, 0, 0);
	ret = dma_mapping_error(vfio_dev->dev, dma_addr);
	if (kiusr.fixed_iova) {
		retval = kiumd_set_dma_cookie(cookie, IOMMU_DMA_IOVA_COOKIE, 0);
		mutex_unlock(&cookie->mutex);
	}

	if (ret || retval) {
		pr_err("%s:Failed to map with error: %d\n", __func__, ret);
		ret = -EINVAL;
		goto reg_free;
	}

	kiusr.iova = dma_addr;
	kiusr.reg_len = size;

	spin_lock(&kiumd_ctx->smmu_lock);
	smap->id = kiumd_ctx->id++;
	kiusr.id = smap->id;
	hash_add(kiumd_ctx->smmu_table, &smap->node, smap->id);
	mmio_ctx->iova = dma_addr;
	mmio_ctx->size = size;
	mmio_ctx->dev = vfio_dev->dev;
	smap->context = mmio_ctx;
	spin_unlock(&kiumd_ctx->smmu_lock);

	pr_debug("%s:%s mapped pa:%llx size:%llx user iova:%llx dma_addr:%llx id:%d\n",
		 __func__, reg_name, addr, size, kiusr.iova, dma_addr, kiusr.id);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("kiumd:error in copying data:%d\n", ret);
		ret = -EFAULT;
		goto smap_del;
	}

	trace_kiumd_mmio_smmu_map_end(reg_name, kiusr.vfio_fd,  kiusr.id, kiusr.iova,
				      kiusr.reg_len);
	kfree(reg_name);
	return 0;
smap_del:
	dma_unmap_resource(vfio_dev->dev, dma_addr, size, 0, 0);
	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	spin_unlock(&kiumd_ctx->smmu_lock);
reg_free:
	kfree(reg_name);
smap_free:
	kfree(smap->context);
	kfree(smap);
	return ret;
}

/**
 * @Brief: This function facilitates to
 * unmap mmio region into smmu at fixed
 * IOVA.The function is called via IOCTL
 * interface and input is provided via
 * struct kiumd_user from the user space.
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * return value is errno in failure cases
 * or 0 in case of successful mapping
 */
static int kiumd_mmio_smmu_unmap(char __user *arg, struct file *fp)
{
	struct kiumd_smmu_mmio_ctx *mmio_ctx;
	struct kiumd_smmu_mmio_map kiusr;
	struct kiumd_ctx *kiumd_ctx;
	struct smmu_map_data *smap;
	bool found = false;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EFAULT;
	}

	trace_kiumd_mmio_smmu_unmap_start(kiusr.vfio_fd, kiusr.iova, kiusr.id);

	if (kiusr.id < 0) {
		pr_err("%s:id passed from user should be positive value\n",
		       __func__);
		return -EFAULT;
	}

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_for_each_possible(kiumd_ctx->smmu_table, smap, node, kiusr.id) {
		if (smap->id == kiusr.id && smap->context) {
			found = true;
			break;
		}
	}
	spin_unlock(&kiumd_ctx->smmu_lock);

	if (!found) {
		pr_err("%s:smmu id not found: %d\n", __func__, kiusr.id);
		return -ENOENT;
	}

	mmio_ctx = (struct kiumd_smmu_mmio_ctx *)smap->context;
	if (!mmio_ctx) {
		pr_err("%s:invalid context:%d\n", __func__, __LINE__);
		return -EINVAL;
	}

	pr_debug("%s:mapping found:%d mmio_ctx:%p iova:%llx size:%lx\n",
		 __func__, kiusr.id, mmio_ctx, mmio_ctx->iova, mmio_ctx->size);

	dma_unmap_resource(mmio_ctx->dev, mmio_ctx->iova, mmio_ctx->size,
			   0, 0);

	spin_lock(&kiumd_ctx->smmu_lock);
	hash_del(&smap->node);
	kfree(smap->context);
	kfree(smap);
	spin_unlock(&kiumd_ctx->smmu_lock);

	trace_kiumd_mmio_smmu_unmap_end(kiusr.id);

	return 0;
}

/**
 * @Brief: This function facilitates to
 * initialize the locks, hash tables and
 * allocate the memory for device ctx.
 *
 * Parameters:
 * @inode: inode ptr for device file
 * @filp: file ptr for device context
 *
 * return value is errno in failure cases
 * or 0 in case of successful mapping
 */
static int kiumd_open(struct inode *inode, struct file *filp)
{
	struct kiumd_ctx *kictx;

	kictx = kzalloc(sizeof(*kictx), GFP_KERNEL);
	if (!kictx)
		return -ENOMEM;

	kictx->kgsl_context = kzalloc(sizeof(*kictx->kgsl_context), GFP_KERNEL);
	if (!kictx->kgsl_context)
		return -ENOMEM;

	kictx->id = 0;
	kictx->pt_start_iova = KIUMD_32BIT_START_IOVA;
	kictx->pt_end_iova = KIUMD_32BIT_END_IOVA;
	kictx->is_initialized = false;
	kictx->hyp_idx = 0;
	kictx->kgsl_context->kgsl_start_iova = KGSL_PER_PROCESS_PT_BASE_IOVA;
	kictx->kgsl_context->kgsl_end_iova = KGSL_PER_PROCESS_PT_END_IOVA;
	hash_init(kictx->smmu_table);
	hash_init(kictx->hyp_table);
	spin_lock_init(&kictx->smmu_lock);
	xa_init(&kictx->kiumd_xa);
	mutex_init(&kictx->kiumd_xa_mutex);
	mutex_init(&kictx->hyp_lock);
	xa_init_flags(&kictx->kiumd_xa, XA_FLAGS_ALLOC);
	filp->private_data = kictx;

	return 0;
}

/**
 * @Brief: This function facilitates to
 * free the locks, hash tables and
 * free the memory for device ctx.
 *
 * Parameters:
 * @inode: inode ptr for device file
 * @filp: file ptr for device context
 *
 * return value is errno in failure cases
 * or 0 in case of success
 */
static int kiumd_close(struct inode *inode, struct file *filp)
{
	struct kiumd_ctx *ki_ctx = (struct kiumd_ctx *)filp->private_data;
	struct dma_buf_handle *dmabuf_handle;
	struct iommu_domain *iommu_dom;
	struct dma_buf *kiumd_dmabuf;
	struct smmu_map_data *smap;
	unsigned long xa_index;
	struct hlist_node *tmp;
	int count, ret, iter;
	void *xa_entry;


	if (!hash_empty(ki_ctx->smmu_table)) {
		hash_for_each_safe(ki_ctx->smmu_table, iter, tmp, smap, node) {
			if (smap->context) {
				struct kiumd_smmu_mmio_ctx *mmio_ctx = smap->context;

				pr_debug("Free mmio ctx%p iova:%llx size:%lx\n",
					 mmio_ctx, mmio_ctx->iova, mmio_ctx->size);
				dma_unmap_resource(mmio_ctx->dev, mmio_ctx->iova, mmio_ctx->size, 0, 0);
				kfree(smap->context);
			}

			if (smap->dmabuf_ptr) {
				kiumd_dmabuf = (struct dma_buf *)smap->dmabuf_ptr;

				pr_debug("kiumd_debug: Driver close : unmap sgt_ptr:%llx\n",
					 smap->sgt_ptr);

				if (smap->ptselect == KGSL_GLOBAL_PT ||
				    smap->ptselect == KGSL_PER_PROCESS_PT ||
				    smap->ptselect == KGSL_DEFAULT_PT)
					continue;

				if (smap->is_fixed_map) {
					ret = kiumd_configure_dma_cookie(smap->vfio_dev, IOMMU_DMA_MSI_COOKIE, smap->dmabuf_ptr);
					if (ret) {
						pr_err("%s %d failed to configure cookie\n", __func__, __LINE__);
						break;
					}
				}

				if (smap->dmabufattach && smap->sgt_ptr)
					dma_buf_unmap_attachment_unlocked((struct dma_buf_attachment *)
									  smap->dmabufattach,
									  (struct sg_table *)smap->sgt_ptr,
									  smap->dma_dir);

				pr_debug("kiumd_debug: unmap dmabufatach:%llx\n", smap->dmabufattach);
				if (smap->is_iova_zero == FIXED_IOVA_AT_ZERO) {
					iommu_dom = kiumd_iommu_get_dma_domain(smap->vfio_dev->dev);
					if (!iommu_dom) {
						pr_err("%s:IOMMU domain is NULL\n", __func__);
						continue;
					}

					pr_debug("kiumd_debug: unmap iommu_dom:%llx, size: %d\n",
						 iommu_dom,
						 kiumd_dmabuf->size);
					ret = iommu_unmap(iommu_dom, 0,
							  kiumd_dmabuf->size);
					if (ret != kiumd_dmabuf->size) {
						pr_err("%s:iommu_unmap failed\n", __func__);
						continue;
					}
				}

                                if (smap->is_fixed_map) {
					ret = kiumd_configure_dma_cookie(smap->vfio_dev, IOMMU_DMA_IOVA_COOKIE, smap->dmabuf_ptr);
					if (ret) {
						pr_err("%s %d failed to configure cookie\n", __func__, __LINE__);
						break;
					}
				}

				dma_buf_detach(kiumd_dmabuf, smap->dmabufattach);
				dma_buf_put(kiumd_dmabuf);
				pr_debug("kiumd_debug: unmap done\n");
			}

			hash_del(&smap->node);
			kfree(smap);
		}
	}

	kfree(ki_ctx->res_mem_area);
	mutex_lock(&ki_ctx->kiumd_xa_mutex);
	if (!xa_empty(&ki_ctx->kiumd_xa)) {
		xa_for_each(&ki_ctx->kiumd_xa, xa_index, xa_entry) {
			dmabuf_handle = (struct dma_buf_handle *) xa_entry;
			if (!dmabuf_handle)
				continue;

			kiumd_dmabuf = ((struct dma_buf *)dmabuf_handle->dmabuf);
			count = atomic_read(&dmabuf_handle->handle_refcount);
			if (!IS_ERR_OR_NULL(kiumd_dmabuf)) {
				while (count > 0) {
					dma_buf_put(kiumd_dmabuf);
					count--;
				}
			}
			kfree(dmabuf_handle);
		}
		xa_destroy(&ki_ctx->kiumd_xa);
	}
	mutex_unlock(&ki_ctx->kiumd_xa_mutex);

	kfree(ki_ctx);

	return 0;
}

/**
 * @Brief: This function facilitates to
 * initialise the context of the device.
 * Currently it reads the Device tree to check if the
 * device has any reserved regions and stores the information
 * of reserved memory regions internally.
 *
 * Parameters:
 * @arg: User space argument ptr
 * @fp: file ptr for device context
 *
 * return value is errno in failure cases
 * or 0 in case of success
 */
static int kiumd_vfio_ctx_init(char __user *arg, struct file *fp)
{
	struct kiumd_dev_mem_info kiusr;
	struct vfio_device *vfio_dev;
	struct kiumd_ctx *kiumd_ctx;
	struct device_node *mem_np;
	struct reserved_mem *rmem;
	struct device_node *np;

	kiumd_ctx = (struct kiumd_ctx *)fp->private_data;

	if (copy_from_user(&kiusr, arg, sizeof(kiusr))) {
		pr_err("%s:%d invalid args from user\n", __func__, __LINE__);
		return -EINVAL;
	}

	trace_kiumd_vfio_ctx_init_start(kiusr.vfio_fd);

	vfio_dev = kiumd_get_vfio_device(kiusr.vfio_fd);
	if (!vfio_dev) {
		pr_err("%s:%d invalid vfio device fd\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (!vfio_dev->dev)
		return -EINVAL;

	np = dev_of_node(vfio_dev->dev);
	if (!np) {
		pr_err("%s:No memory-region specified\n", __func__);
		return -EINVAL;
	}

	kiumd_ctx->num_reserved_regions = of_property_count_elems_of_size(np,
									  "memory-region",
									  sizeof(phandle));
	if (kiumd_ctx->num_reserved_regions <= 0)
		return -EINVAL;

	kiumd_ctx->res_mem_area = kcalloc((kiumd_ctx->num_reserved_regions + 1),
					  sizeof(*kiumd_ctx->res_mem_area),
					  GFP_KERNEL);
	if (!kiumd_ctx->res_mem_area)
		return -ENOMEM;

	kiusr.num_regions = kiumd_ctx->num_reserved_regions;
	for (u64 i = 0; i < kiusr.num_regions; i++) {
		mem_np = of_parse_phandle(vfio_dev->dev->of_node, "memory-region", i);
		if (!mem_np) {
			pr_debug("%s:cant find phandle\n", __func__);
			continue;
		}

		rmem = of_reserved_mem_lookup(mem_np);
		if (!rmem) {
			of_node_put(mem_np);
			pr_err("%s:No memory address assigned to the reserved region\n", __func__);
			kfree(kiumd_ctx->res_mem_area);
			return -EINVAL;
		}

		of_node_put(mem_np);
		kiumd_ctx->res_mem_area[i].size = rmem->size;
		kiumd_ctx->res_mem_area[i].base = rmem->base;
		kiusr.mem_info[i].size = rmem->size;
		kiusr.mem_info[i].offset = i << KIUMD_INDEX_OFFSET;
		pr_debug("%s:base:%lx size:%lx offset:%lx\n", __func__,
			 kiumd_ctx->res_mem_area[i].base,
			 kiusr.mem_info[i].size, kiusr.mem_info[i].offset);
	}

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		kfree(kiumd_ctx->res_mem_area);
		kiumd_ctx->res_mem_area = NULL;
		pr_err("%s:error in copying vfio ctx data for reserved memory\n",
		       __func__);
		return -EFAULT;
	}
	trace_kiumd_vfio_ctx_init_end(kiusr.vfio_fd);
	return 0;
}

/**
 * @Brief: This function facilitates to call
 * the different ioctls based on
 * commands received from user space.
 *
 * Parameters:
 * @file: file ptr for device file
 * @cmd: ioctl cmd
 * @arg: argument
 *
 * return value is errno in failure cases
 * or 0 in case of success
 */
static long kiumd_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	char __user *argp = (char __user *)arg;
	int err;

	switch (cmd) {
	case KIUMD_SMMU_MAP_BUF:
		err = kiumd_dmabuf_vfio_map(argp, file);
		break;
	case KIUMD_SMMU_UNMAP_BUF:
		err = kiumd_dmabuf_vfio_unmap(argp, file);
		break;
	case KIUMD_IOVA_MAP_CTRL:
		err = kiumd_iova_ctrl(argp);
		break;
	case KIUMD_SET_PGTBL_CONTEXT:
		err = kiumd_set_pgtbl_context(argp, file);
		break;
	case KIUMD_PER_PROCESS_ALLOC:
		err = kiumd_perprocess_pt_alloc(argp, file);
		break;
	case KIUMD_PER_PROCESS_SET:
		err = kiumd_perprocess_pgtble_set(argp);
		break;
	case KIUMD_PER_PROCESS_FREE:
		err = kiumd_perprocess_pgtble_free(argp, file);
		break;
	case KIUMD_FD_DMABUF_HANDLE:
		err = kiumd_fd_dmabuf_handler(argp, file);
		break;
	case KIUMD_CUSTOM_IOVA_INIT:
		err = kiumd_dmabuf_custom_iova_init(argp, file);
		break;
	case KIUMD_GLOBAL_PT_SET:
		err = kiumd_global_pgtble_set(argp, file);
		break;
	case KIUMD_SMMU_SECURE_MAP:
		err = kiumd_dmabuf_vfio_secure_map(argp, file);
		break;
	case KIUMD_SMMU_SECURE_UNMAP:
		err = kiumd_dmabuf_vfio_secure_unmap(argp, file);
		break;
	case KIUMD_SMMU_MMIO_MAP:
		err = kiumd_mmio_smmu_map(argp, file);
		break;
	case KIUMD_SMMU_MMIO_UNMAP:
		err = kiumd_mmio_smmu_unmap(argp, file);
		break;
	case KIUMD_SMMU_FAULT_HANDLE_REGISTER:
		err = kiumd_smmu_fault_handler_register(argp);
		break;
	case KIUMD_SMMU_FAULT_HANDLE_DEREGISTER:
		err = kiumd_smmu_fault_handler_deregister(argp);
		break;
	case KIUMD_VFIO_CTX_INIT:
		err = kiumd_vfio_ctx_init(argp, file);
		break;
	case KIUMD_SMMU_MANAGED_IOVA_MAP:
		err = kiumd_dmabuf_managed_iova_map(argp, file);
		break;
	case KIUMD_SMMU_MANAGED_IOVA_UNMAP:
		err = kiumd_dmabuf_managed_iova_unmap(argp, file);
		break;
	case KIUMD_SMMU_ASSIGN_BUF:
		err = kiumd_dmabuf_assign_buf(argp, file);
		break;
	case KIUMD_SMMU_UNASSIGN_BUF:
		err = kiumd_dmabuf_unassign_buf(argp, file);
		break;
	default:
		err = -ENOTTY;
		break;
	}

	return err;
}

/**
 * @Brief: This function facilitates to
 * map a reserved DDR region as normal memory
 * in user space
 *
 * Parameters:
 * @file: file ptr
 * @vma : pointer to struct vma
 *
 * return value is errno in failure cases
 * or 0 in case of success
 */
static int kiumd_mmap(struct file *file, struct vm_area_struct *vma)
{
	u64 req_len, index, pgoff;
	struct kiumd_ctx *ki_ctx;

	if (!file) {
		pr_err("%s:file ptr returns NULL\n", __func__);
		return -EINVAL;
	}

	ki_ctx = (struct kiumd_ctx *)file->private_data;
	if (!ki_ctx) {
		pr_err("%s:kiumd ctx is NULL\n", __func__);
		return -EINVAL;
	}

	if (!ki_ctx->res_mem_area) {
		pr_err("%s:No reserved mem areas\n", __func__);
		return -EINVAL;
	}

	if (vma->vm_end < vma->vm_start) {
		pr_err("%s:Invalid vm start and end\n", __func__);
		return -EINVAL;
	}

	index = vma->vm_pgoff >> (KIUMD_INDEX_OFFSET - PAGE_SHIFT);
	if (ki_ctx->num_reserved_regions <= index) {
		pr_err("%s:Invalid index:%lx\n", __func__, index);
		return -EINVAL;
	}

	pr_debug("%s:index:%lx\n", __func__, index);
	if (ki_ctx->res_mem_area[index].base & ~PAGE_MASK) {
		pr_err("%s:Unalligned base address\n", __func__);
		return -EINVAL;
	}

	req_len = vma->vm_end - vma->vm_start;

	vma->vm_pgoff = vma->vm_pgoff &
			((1ULL << (KIUMD_INDEX_OFFSET - PAGE_SHIFT)) - 1);

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	pr_debug("%s:res mem start:%llx End:%llx size:%lx vma start:%lx vma end:%lx size:%lx offset:%lx\n",
		 __func__, ki_ctx->res_mem_area[index].base,
		 ki_ctx->res_mem_area[index].base + ki_ctx->res_mem_area[index].size,
		 ki_ctx->res_mem_area[index].size, vma->vm_start,
		 vma->vm_end, vma->vm_end - vma->vm_start, vma->vm_pgoff);

	return remap_pfn_range(vma, vma->vm_start,
			       ki_ctx->res_mem_area[index].base >> PAGE_SHIFT,
			       req_len, vma->vm_page_prot);
}

static const struct file_operations kiumd_fops = {
	.open = kiumd_open,
	.unlocked_ioctl = kiumd_ioctl,
	.compat_ioctl = kiumd_ioctl,
	.release = kiumd_close,
	.mmap = kiumd_mmap,
};

/**
 * @Brief: This function facilitates to
 * register the kiumd driver as a
 * miscellaneous device to kernel driver
 * framework
 *
 * Parameters:
 * @pdev: platform device ptr
 *
 * return value is errno in failure cases
 * or 0 in case of success
 */

static int kiumd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const char *name, *devname;
	struct miscdevice *miscdev;
	struct device_node *np;
	int err;

	np = dev->of_node;
	miscdev = kzalloc(sizeof(*miscdev), GFP_KERNEL);
	if (!miscdev)
		return -ENOMEM;

	if (!of_property_read_string(np, "qcom,dev-name", &name))
		devname = devm_kstrdup(dev, name, GFP_KERNEL);
	else
		devname = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);

	iommu_addr_cache = kmem_cache_create("iommu_addr_cache",
					     sizeof(struct iommu_addr_entry),
					     0, 0, NULL);
	if (!iommu_addr_cache) {
		pr_err("kiumd kmem cache creation failure\n");
		return -ENOMEM;
	}

	miscdev->minor = MISC_DYNAMIC_MINOR;
	miscdev->name = devname;
	miscdev->fops = &kiumd_fops;
	err = misc_register(miscdev);
	if (err) {
		pr_err("kiumd misc device %s creation failure\n", devname);
		return err;
	}
	smmu_obj = kobject_create_and_add("smmu_faults", kernel_kobj);
	if (!smmu_obj) {
		pr_err("smmu_faults kernel object creation failure\n");
		return -ENOMEM;
	}
	return 0;
}

static const struct of_device_id kiumd_match_table[] = {
	{ .compatible = "qcom,kiumd", },
	{ },
};
MODULE_DEVICE_TABLE(of, kiumd_match_table);

static struct platform_driver kiumd_driver = {
	.driver = {
		.name = "kiumd",
		.of_match_table = kiumd_match_table,
	},
	.probe = kiumd_probe,
};
module_platform_driver(kiumd_driver);

MODULE_IMPORT_NS(DMA_BUF);
MODULE_DESCRIPTION("KIUMD");
MODULE_LICENSE("GPL v2");

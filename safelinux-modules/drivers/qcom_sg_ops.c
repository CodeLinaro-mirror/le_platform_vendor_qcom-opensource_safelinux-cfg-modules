// SPDX-License-Identifier: GPL-2.0-only
/*
 * mmap() algorithm taken from drivers/staging/android/ion/ion_heap.c as
 * of commit a3ec289e74b4 ("arm-smmu: Fix missing qsmmuv500 callback")
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019, 2020 Linaro Ltd.
 *
 * Portions based off of Andrew Davis' SRAM heap:
 * Copyright (C) 2019 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 *
 * These ops were base of the ops in drivers/dma-buf/heaps/system-heap.c from
 * https://lore.kernel.org/lkml/20201017013255.43568-2-john.stultz@linaro.org/
 *
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/of.h>
#include <linux/dma-map-ops.h>
#include "qcom_sg_ops.h"

static struct sg_table *dup_sg_table(struct sg_table *table)
{
	struct sg_table *new_table;
	int ret, i;
	struct scatterlist *sg, *new_sg;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, table->orig_nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	new_sg = new_table->sgl;
	for_each_sgtable_sg(table, sg, i) {
		sg_set_page(new_sg, sg_page(sg), sg->length, sg->offset);
		new_sg = sg_next(new_sg);
	}

	return new_table;
}

int qcom_sg_attach(struct dma_buf *dmabuf,
		   struct dma_buf_attachment *attachment)
{
	struct qcom_sg_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;
	struct sg_table *table;

	pr_debug("qcom_sg_attach\n");
	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = dup_sg_table(&buffer->sg_table);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->table = table;
	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->list);
	a->mapped = false;

	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

void qcom_sg_detach(struct dma_buf *dmabuf,
		    struct dma_buf_attachment *attachment)
{
	struct qcom_sg_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a = attachment->priv;

	pr_debug("qcom_sg_dettach\n");
	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);

	sg_free_table(a->table);
	kfree(a->table);
	kfree(a);
}

struct sg_table *qcom_sg_map_dma_buf(struct dma_buf_attachment *attachment,
				     enum dma_data_direction direction)
{
	struct dma_heap_attachment *a = attachment->priv;
	struct sg_table *table = a->table;
	struct qcom_sg_buffer *buffer;

	unsigned long attrs = 0;
	int ret;

	pr_debug("qcom_sg_map_dma_buf\n");
	buffer = attachment->dmabuf->priv;

	mutex_lock(&buffer->lock);

	if (!a->mapped) {
		ret = dma_map_sgtable(attachment->dev, table, direction, attrs);
	} else {
		dev_err(attachment->dev, "Error: Dma-buf is already mapped!\n");
		ret = -EBUSY;
	}

	if (ret) {
		table = ERR_PTR(ret);
		goto err_map_sgtable;
	}

	a->mapped = true;
	mutex_unlock(&buffer->lock);
	return table;

err_map_sgtable:

	mutex_unlock(&buffer->lock);
	return table;
}

void qcom_sg_unmap_dma_buf(struct dma_buf_attachment *attachment,
			   struct sg_table *table,
			   enum dma_data_direction direction)
{
	struct dma_heap_attachment *a = attachment->priv;
	struct qcom_sg_buffer *buffer;

	unsigned long attrs = 0;
	pr_debug("qcom_sg_unmap_dma_buf\n");

	buffer = attachment->dmabuf->priv;

	mutex_lock(&buffer->lock);

	a->mapped = false;

	dma_unmap_sgtable(attachment->dev, table, direction, attrs);

	mutex_unlock(&buffer->lock);
}

int qcom_sg_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
				     enum dma_data_direction direction)
{
	struct qcom_sg_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;

	pr_debug("qcom_sg_begin_cpu_access\n");

	mutex_lock(&buffer->lock);

	if (buffer->vmap_cnt)
		invalidate_kernel_vmap_range(buffer->vaddr, buffer->len);

	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->mapped)
			continue;
		dma_sync_sgtable_for_cpu(a->dev, a->table, direction);
	}

	mutex_unlock(&buffer->lock);

	return 0;
}

int qcom_sg_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
				   enum dma_data_direction direction)
{
	struct qcom_sg_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;
	pr_debug("qcom_sg_end_cpu_access\n");

	mutex_lock(&buffer->lock);

	if (buffer->vmap_cnt)
		flush_kernel_vmap_range(buffer->vaddr, buffer->len);

	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->mapped)
			continue;
		dma_sync_sgtable_for_device(a->dev, a->table, direction);
	}

	mutex_unlock(&buffer->lock);

	return 0;
}


int qcom_sg_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct qcom_sg_buffer *buffer = dmabuf->priv;
	struct sg_table *table = &buffer->sg_table;
	struct scatterlist *sg;
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	int ret;
	int i;
	pr_debug("qcom_sg_mmap\n");

	for_each_sg(table->sgl, sg, table->nents, i) {
		struct page *page = sg_page(sg);
		unsigned long remainder = vma->vm_end - addr;
		unsigned long len = sg->length;

		if (offset >= sg->length) {
			offset -= sg->length;
			continue;
		} else if (offset) {
			page += offset / PAGE_SIZE;
			len = sg->length - offset;
			offset = 0;
		}
		len = min(len, remainder);
		ret = remap_pfn_range(vma, addr, page_to_pfn(page), len,
				      vma->vm_page_prot);

		addr += len;
		if (addr >= vma->vm_end)
			return 0;
	}
	return 0;
}

void *qcom_sg_do_vmap(struct qcom_sg_buffer *buffer)
{
	struct sg_table *table = &buffer->sg_table;
	int npages = PAGE_ALIGN(buffer->len) / PAGE_SIZE;
	struct page **pages = vmalloc(sizeof(struct page *) * npages);
	struct page **tmp = pages;
	struct sg_page_iter piter;
	pgprot_t pgprot = PAGE_KERNEL;
	void *vaddr;

	if (!pages)
		return ERR_PTR(-ENOMEM);

	for_each_sgtable_page(table, &piter, 0) {
		WARN_ON(tmp - pages >= npages);
		*tmp++ = sg_page_iter_page(&piter);
	}

	vaddr = vmap(pages, npages, VM_MAP, pgprot);
	vfree(pages);

	if (!vaddr)
		return ERR_PTR(-ENOMEM);

	return vaddr;
}

int qcom_sg_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct qcom_sg_buffer *buffer = dmabuf->priv;
	void *vaddr;
	int ret = 0;

	pr_debug("qcom_sg_vmap\n");
	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt) {
		buffer->vmap_cnt++;
		iosys_map_set_vaddr(map, buffer->vaddr);
		goto out;
	}

	vaddr = qcom_sg_do_vmap(buffer);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		goto out;
	}

	buffer->vaddr = vaddr;
	buffer->vmap_cnt++;
	iosys_map_set_vaddr(map, buffer->vaddr);
out:
	mutex_unlock(&buffer->lock);

	return ret;
}

void qcom_sg_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct qcom_sg_buffer *buffer = dmabuf->priv;

	pr_debug("qcom_sg_vunmap\n");
	mutex_lock(&buffer->lock);
	if (!--buffer->vmap_cnt) {
		vunmap(buffer->vaddr);
		buffer->vaddr = NULL;
	}

	mutex_unlock(&buffer->lock);
	iosys_map_clear(map);
}

void qcom_sg_release(struct dma_buf *dmabuf)
{
	struct qcom_sg_buffer *buffer = dmabuf->priv;
	pr_debug("qcom_sg_release\n");
	buffer->free(buffer);
}

struct dma_buf_ops qcom_sg_buf_ops = {
		.attach = qcom_sg_attach,
		.detach = qcom_sg_detach,
		.map_dma_buf = qcom_sg_map_dma_buf,
		.unmap_dma_buf = qcom_sg_unmap_dma_buf,
		.begin_cpu_access = qcom_sg_dma_buf_begin_cpu_access,
		.end_cpu_access = qcom_sg_dma_buf_end_cpu_access,
		.mmap = qcom_sg_mmap,
		.vmap = qcom_sg_vmap,
		.vunmap = qcom_sg_vunmap,
		.release = qcom_sg_release,
};



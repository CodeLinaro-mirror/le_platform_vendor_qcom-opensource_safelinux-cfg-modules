// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <uapi/misc/dmabuf_share.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/xarray.h>

#define CREATE_TRACE_POINTS
#include "safelinux_modules_trace.h"

/**
 * struct dma_buf_handle - Structure to hold a dma_buf handle
 * @dmabuf: The dma_buf associated with the handle
 * @ref: The reference count for the handle
 *
 * This structure is used to store information about a dma_buf handle. It
 * contains the dma_buf associated with the handle and a reference count to
 * track the number of users of the handle.
 */
struct dma_buf_handle {
	struct dma_buf *dmabuf;
	atomic_t ref;
};

/**
 * struct dmabuf_ctx - Structure to hold the dma_buf context
 * @xarr: The xarray to store dma_buf file descriptors
 * @lock: The mutex to protect access to the xarray
 *
 * This structure is used to store the dma_buf context. It contains an xarray
 * to store dma_buf file descriptors and a mutex to protect access to the
 * xarray.
 */
struct dmabuf_ctx {
	struct xarray xarr;
	struct mutex lock;
};

/**
 * dmabuf_fd_to_handle - Convert a dma_buf fd to a handle
 * @dusr: user space argument
 * @dmabuf_ctx: dmabuf context
 *
 * Returns 0 on success, negative error code on failure
 */
static int dmabuf_fd_to_handle(struct dmabuf_user *dusr,
				struct dmabuf_ctx *dmabuf_ctx)
{
	struct dma_buf_handle *dmabuf_handle = NULL;
	uint32_t local_id = 0;
	struct dma_buf *dmabuf;
	unsigned long xa_index;
	void *xa_entry;
	int ret = 0;

	mutex_lock(&dmabuf_ctx->lock);
	dmabuf  =  dma_buf_get(dusr->dma_buf_fd);
	if (IS_ERR_OR_NULL(dmabuf)) {
		pr_err("dma_buf_get returns NULL\n");
		ret = PTR_ERR(dmabuf);
		goto unlock_and_return;
	}

	xa_for_each(&dmabuf_ctx->xarr, xa_index, xa_entry) {
		dmabuf_handle = (struct dma_buf_handle *) xa_entry;
		if (dmabuf_handle->dmabuf == dmabuf) {
			local_id = xa_index;
			atomic_inc(&dmabuf_handle->ref);
			goto handle_found;
		}
	}

	dmabuf_handle = kzalloc(sizeof(struct dma_buf_handle), GFP_KERNEL);
	if (!dmabuf_handle) {
		ret = -ENOMEM;
		goto put_dmabuf;
	}

	dmabuf_handle->dmabuf = dmabuf;
	atomic_set(&dmabuf_handle->ref, 1);
	ret = xa_alloc(&dmabuf_ctx->xarr, &local_id, (void *)dmabuf_handle,
						XA_LIMIT(1, UINT_MAX), GFP_KERNEL);
	if (ret < 0) {
		pr_err("xarray alloc failure %d\n", ret);
		kfree(dmabuf_handle);
		goto put_dmabuf;
	}

handle_found:
	dusr->handle = local_id;
	trace_kiumd_fd_dmabuf_handler_fd_to_handle(dusr->dma_buf_fd, dusr->handle,
						   atomic_read(&dmabuf_handle->ref));
	goto unlock_and_return;

put_dmabuf:
	dma_buf_put(dmabuf);
unlock_and_return:
	mutex_unlock(&dmabuf_ctx->lock);
	return ret;
}

/**
 * dmabuf_handle_to_fd - Convert a handle to a dma_buf fd
 * @dusr: user space argument
 * @dmabuf_ctx: dmabuf context
 *
 * Returns 0 on success, negative error code on failure
 */
static int dmabuf_handle_to_fd(struct dmabuf_user *dusr,
				struct dmabuf_ctx *dmabuf_ctx)
{
	struct dma_buf_handle *dmabuf_handle = NULL;
	uint32_t local_id;
	int ret = 0;

	local_id = dusr->handle;
	mutex_lock(&dmabuf_ctx->lock);
	dmabuf_handle = xa_load(&dmabuf_ctx->xarr, local_id);

	if (!dmabuf_handle) {
		pr_err("Failed to load dmabuf_handle for local_id %d\n", local_id);
		ret = -ENOENT;
		goto unlock_and_return;
	}

	if (IS_ERR_OR_NULL(dmabuf_handle->dmabuf)) {
		pr_err("dmabuf_handle is invalid\n");
		ret = -EINVAL;
		goto unlock_and_return;
	}

	get_dma_buf(dmabuf_handle->dmabuf);
	dusr->dma_buf_fd = dma_buf_fd(dmabuf_handle->dmabuf, (O_CLOEXEC));

	if (dusr->dma_buf_fd < 0) {
		pr_err("dma_buf_fd descriptor error fd %d\n", dusr->dma_buf_fd);
		dma_buf_put(dmabuf_handle->dmabuf);
		ret = -EBADF;
		goto unlock_and_return;
	}

unlock_and_return:
	mutex_unlock(&dmabuf_ctx->lock);
	return ret;
}

/**
 * dmabuf_close_handle - Close a handle
 * @dusr: user space argument
 * @dmabuf_ctx: dmabuf context
 *
 * Returns 0 on success, negative error code on failure
 */
static int dmabuf_close_handle(struct dmabuf_user *dusr,
				struct dmabuf_ctx *dmabuf_ctx)
{
	struct dma_buf_handle *dmabuf_handle = NULL;
	struct dma_buf *fdhandle_dmabuf = NULL;
	uint32_t local_id;
	int ret = 0;

	local_id = (uint32_t)dusr->handle;
	mutex_lock(&dmabuf_ctx->lock);
	dmabuf_handle = xa_load(&dmabuf_ctx->xarr, local_id);

	if (!dmabuf_handle) {
		pr_err("Entry not available in xarray\n");
		ret = -EINVAL;
		goto unlock_and_return;
	}

	fdhandle_dmabuf = dmabuf_handle->dmabuf;
	if (IS_ERR_OR_NULL(fdhandle_dmabuf)) {
		pr_err("Invalid dma buf handle.\n");
		ret = -EINVAL;
		goto unlock_and_return;
	}

	dma_buf_put(fdhandle_dmabuf);
	if (atomic_dec_and_test(&dmabuf_handle->ref)) {
		xa_erase(&dmabuf_ctx->xarr, local_id);
		kfree(dmabuf_handle);
	}

unlock_and_return:
	mutex_unlock(&dmabuf_ctx->lock);
	return ret;
}

static int dmabuf_open(struct inode *inode, struct file *filp)
{
	struct dmabuf_ctx *dmactx;

	dmactx = kzalloc(sizeof(struct dmabuf_ctx), GFP_KERNEL);
	if (!dmactx)
		return -ENOMEM;

	mutex_init(&dmactx->lock);
	xa_init_flags(&dmactx->xarr, XA_FLAGS_ALLOC);
	filp->private_data = dmactx;
	return 0;
}

static long dmabuf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dmabuf_ctx *dmabuf_ctx = (struct dmabuf_ctx *)file->private_data;
	struct dmabuf_user dusr;
	int ret;

	if (copy_from_user(&dusr, (struct dmabuf_user *)arg,
					sizeof(struct dmabuf_user))) {
		pr_err("failed to get data from user\n");
		return -EFAULT;
	}

	trace_kiumd_fd_dmabuf_handler_start(dusr.handle, dusr.dma_buf_fd);

	if (cmd == DMABUF_FD_TO_HANDLE)
		ret = dmabuf_fd_to_handle(&dusr, dmabuf_ctx);
	else if (cmd == DMABUF_HANDLE_TO_FD)
		ret = dmabuf_handle_to_fd(&dusr, dmabuf_ctx);
	else if (cmd == DMABUF_CLOSE_HANDLE)
		ret = dmabuf_close_handle(&dusr, dmabuf_ctx);
	else
		ret = -EINVAL;

	trace_kiumd_fd_dmabuf_handler_end(dusr.handle, dusr.dma_buf_fd, ret);

	if (ret == 0 && copy_to_user((struct dmabuf_user *)arg, &dusr, sizeof(dusr))) {
		pr_err("failed to send data to user\n");
		ret = -EFAULT;
	}

	return ret;
}

static int dmabuf_release(struct inode *inode, struct file *filp)
{
	struct dmabuf_ctx *dma_ctx = (struct dmabuf_ctx *)filp->private_data;
	struct dma_buf_handle *dmabuf_handle;
	unsigned long xa_index;
	struct dma_buf *dmabuf;
	void *xa_entry;
	int count;

	mutex_lock(&dma_ctx->lock);
	xa_for_each(&dma_ctx->xarr, xa_index, xa_entry) {
		dmabuf_handle = (struct dma_buf_handle *) xa_entry;

		if (!dmabuf_handle)
			continue;

		dmabuf = dmabuf_handle->dmabuf;
		count = atomic_read(&dmabuf_handle->ref);

		if (!IS_ERR_OR_NULL(dmabuf)) {
			while (count > 0) {
				dma_buf_put(dmabuf);
				count--;
			}
		}

		kfree(dmabuf_handle);
	}

	xa_destroy(&dma_ctx->xarr);
	mutex_unlock(&dma_ctx->lock);
	kfree(dma_ctx);
	return 0;
}

/* File operations structure*/
static const struct file_operations dmaheap_fops = {
	.owner = THIS_MODULE,
	.open = dmabuf_open,
	.unlocked_ioctl = dmabuf_ioctl,
	.release = dmabuf_release,
};

/*Miscellaneous device structure*/
static struct miscdevice dmaheap_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRIVER_NAME,
	.fops = &dmaheap_fops,
};

/*Module initialization*/
static int __init dmaheap_init(void)
{
	int ret;

	//Register misc device
	ret = misc_register(&dmaheap_misc);
	if (ret)
		pr_err("Failed to register misc device: %d\n", ret);

	return ret;
}

/*Module cleanup*/
static void __exit dmaheap_exit(void)
{
	// Deregister misc device
	misc_deregister(&dmaheap_misc);
}

/*Register module initialization and cleanup functions*/
module_init(dmaheap_init);
module_exit(dmaheap_exit);

MODULE_IMPORT_NS(DMA_BUF);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Platform buffer share driver");

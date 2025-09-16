/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __DMABUF_SHARE_H__
#define __DMABUF_SHARE_H__

#include <linux/types.h>
#include <linux/unistd.h>

#define DMABUF_HANDLE_TO_FD    _IOWR('R', 19, struct dmabuf_user)
#define DMABUF_FD_TO_HANDLE    _IOWR('R', 20, struct dmabuf_user)
#define DMABUF_CLOSE_HANDLE    _IOWR('R', 21, struct dmabuf_user)
#define DRIVER_NAME "dmabuf_share"
#define pr_fmt(fmt) "%s:%s: " fmt, DRIVER_NAME, __func__
#define dev_fmt(fmt) "%s:%s: " fmt, DRIVER_NAME, __func__

struct dmabuf_user {
	int dma_buf_fd;  /* DMA buffer file descriptor */
	__u32 handle;    /* Handle representing the DMA buffer */
	int pid;         /* Process ID for cross-process sharing */
};

#endif /* __DMABUF_SHARE_H__ */

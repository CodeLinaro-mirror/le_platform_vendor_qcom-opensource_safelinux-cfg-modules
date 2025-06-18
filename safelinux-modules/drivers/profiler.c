// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018, 2020-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#define pr_fmt(fmt) "PROFILER: %s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/of_platform.h>
#include <uapi/misc/scm_user_intf.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/version.h>

#include "profiler.h"
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 33))
#include <linux/firmware/qcom/qcom_scm_addon.h>
#endif
#define PROFILER_DEV			"profiler"

static struct class *driver_class;
static dev_t profiler_device_no;
struct platform_device *ddr_pdev;

static struct reg_offset offset_reg_values;
static struct device_param_init dev_params;
static bool bw_profiling_disabled;

struct profiler_control {
	struct device *pdev;
	struct cdev cdev;
	struct mutex lock;
	struct qcom_tzmem_pool *tzmem_pool;
	void __iomem *mmnoc_base;
	void __iomem *llcc_mmap_base[LLCC_CHANNELS];
	void __iomem *gemnoc_mmap_base[GEMNOC_CHANNELS_NUM];
};

struct conf_data {
	int num_llcc_channels;
	int num_gemnoc_metrics;
	int num_hf_metrics;
	int num_sf_metrics;
	uint32_t llcc_offset;
	uint32_t cabo_offset;
	uint32_t mmnoc_hf_offset;
	uint32_t mmnoc_sf_offset;
	int gemnoc_channels;
};

static struct profiler_control *profiler;

static int bw_profiling_command(const void *req)
{
	int      ret = 0;
	uint32_t qseos_cmd_id = 0;
	struct tz_bw_svc_resp *rsp = NULL;
	size_t req_size = 0, rsp_size = 0;

	if (!req) {
		pr_err("Invalid request buffer pointer\n");
		return -EINVAL;
	}
	rsp = &((struct tz_bw_svc_buf *)req)->bwresp;
	if (!rsp) {
		pr_err("Invalid response buffer pointer\n");
		return -EINVAL;
	}
	rsp_size = sizeof(struct tz_bw_svc_resp);
	req_size = ((struct tz_bw_svc_buf *)req)->req_size;

	qseos_cmd_id = *(uint32_t *)req;

	void *req_ptr __free(qcom_tzmem) = qcom_tzmem_alloc(profiler->tzmem_pool,
							PAGE_ALIGN(req_size + rsp_size),
							GFP_KERNEL);
	if (!req_ptr)
		return -ENOMEM;

	memcpy(req_ptr, req, req_size);

	switch (qseos_cmd_id) {
	case TZ_BW_SVC_START_ID:
	case TZ_BW_SVC_GET_ID:
	case TZ_BW_SVC_STOP_ID:
		/* Send the command to TZ */
		ret = qcom_scm_ddrbw_profiler(qcom_tzmem_to_phys(req_ptr), req_size,
				qcom_tzmem_to_phys(req_ptr) + req_size, rsp_size);
		break;
	default:
		pr_err("cmd_id %d is not supported.\n",
			   qseos_cmd_id);
		ret = -EINVAL;
	} /*end of switch (qsee_cmd_id)  */

	memcpy(rsp, (char *)req_ptr + req_size, rsp_size);

	/* Verify cmd id and Check that request succeeded.*/
	if ((rsp->status != E_BW_SUCCESS) ||
		(qseos_cmd_id != rsp->cmd_id)) {
		ret = -1;
		pr_err("Status: %d,Cmd: %d\n",
			rsp->status,
			rsp->cmd_id);
	}
	return ret;
}

static int bw_profiling_start(struct tz_bw_svc_buf *bwbuf)
{
	bwbuf->bwreq.start_req.cmd_id = TZ_BW_SVC_START_ID;
	bwbuf->bwreq.start_req.version = TZ_BW_SVC_VERSION;
	bwbuf->req_size = sizeof(struct tz_bw_svc_start_req);
	return bw_profiling_command(bwbuf);
}


static int bw_profiling_get(void __user *argp, struct tz_bw_svc_buf *bwbuf)
{
	int ret = 0;

	const int bufsize = sizeof(struct profiler_bw_cntrs_req_m)
							- sizeof(uint32_t);
	struct profiler_bw_cntrs_req_m cnt_buf;

	memset(&cnt_buf, 0, sizeof(cnt_buf));

	void *tzmem_disabled_ptr __free(qcom_tzmem) = qcom_tzmem_alloc(profiler->tzmem_pool,
							bufsize,
							GFP_KERNEL);
	if (!tzmem_disabled_ptr)
		return -ENOMEM;

	/* Populate request data */
	bwbuf->bwreq.get_req.cmd_id = TZ_BW_SVC_GET_ID;
	bwbuf->bwreq.get_req.buf_ptr = qcom_tzmem_to_phys(tzmem_disabled_ptr);
	bwbuf->bwreq.get_req.buf_size = bufsize;
	bwbuf->req_size = sizeof(struct tz_bw_svc_get_req);

	ret = bw_profiling_command(bwbuf);
	if (ret) {
		pr_err("bw_profiling_command failed\n");
		return ret;
	}

	memcpy(&cnt_buf, tzmem_disabled_ptr, bufsize);
	if (copy_to_user(argp, &cnt_buf, sizeof(struct profiler_bw_cntrs_req_m)))
		pr_err("copy_to_user failed\n");

	return ret;
}

static int bw_profiling_per_ip_get(void __user *argp, struct tz_bw_svc_buf *bwbuf)
{
	int ret = 0;
	int ch, gc;
	const int bufsize = sizeof(struct profiler_bw_cntrs_req)
							- sizeof(uint32_t);
	struct profiler_bw_cntrs_req cnt_buf;

	void *tzmem_ptr __free(qcom_tzmem) = qcom_tzmem_alloc(profiler->tzmem_pool,
							      bufsize,
							      GFP_KERNEL);
	if (!tzmem_ptr)
		return -ENOMEM;

	/* Populate request data */
	bwbuf->bwreq.get_req.cmd_id = TZ_BW_SVC_GET_ID;
	bwbuf->bwreq.get_req.buf_ptr = qcom_tzmem_to_phys(tzmem_ptr);
	bwbuf->bwreq.get_req.buf_size = bufsize;
	bwbuf->bwreq.get_req.type = 0;
	bwbuf->req_size = sizeof(struct tz_bw_svc_get_req);

	ret = bw_profiling_command(bwbuf);
	if (ret) {
		pr_err("bw_profiling_command failed\n");
		return ret;
	}

	memset(&cnt_buf, 0, sizeof(cnt_buf));

	for (ch = 0; ch < dev_params.num_llcc_channels; ch++) {
		cnt_buf.llcc_values[ch*2] = readl(profiler->llcc_mmap_base[ch]
						+ offset_reg_values.llcc_offset[ch*2]);
		cnt_buf.llcc_values[ch*2 + 1] = readl(profiler->llcc_mmap_base[ch]
						+ offset_reg_values.llcc_offset[ch*2 + 1]);
		cnt_buf.cabo_values[ch*2] = readl(profiler->llcc_mmap_base[ch]
						+ offset_reg_values.cabo_offset[ch*2]);
		cnt_buf.cabo_values[ch*2 + 1] = readl(profiler->llcc_mmap_base[ch]
						+ offset_reg_values.cabo_offset[ch*2+1]);
	}

	for (ch = 0; ch < dev_params.gemnoc_channels; ch++) {
		for (gc = 0; gc < dev_params.num_gemnoc_metrics; gc++) {
			int index = ch * dev_params.num_gemnoc_metrics + gc;

			cnt_buf.gemnoc_values[index] = readl(profiler->gemnoc_mmap_base[ch]
						+ offset_reg_values.gemnoc_offset[index]);
		}
	}

	/* Populate request data */
	bwbuf->bwreq.get_req.cmd_id = TZ_BW_SVC_GET_ID;
	bwbuf->bwreq.get_req.buf_ptr = qcom_tzmem_to_phys(tzmem_ptr);
	bwbuf->bwreq.get_req.buf_size = bufsize;
	bwbuf->bwreq.get_req.type = 1;
	bwbuf->req_size = sizeof(struct tz_bw_svc_get_req);

	ret = bw_profiling_command(bwbuf);
	if (ret) {
		pr_err("bw_profiling_command failed\n");
		return ret;
	}

	if (copy_to_user(argp, &cnt_buf, sizeof(struct profiler_bw_cntrs_req)))
		pr_err("copy_to_user failed\n");

	return ret;
}

static int bw_profiling_stop(struct tz_bw_svc_buf *bwbuf)
{
	bwbuf->bwreq.stop_req.cmd_id = TZ_BW_SVC_STOP_ID;
	bwbuf->req_size = sizeof(struct tz_bw_svc_stop_req);
	return bw_profiling_command(bwbuf);
}

static int profiler_get_bw_info(void __user *argp)
{
	int ret = 0;
	struct tz_bw_svc_buf *bwbuf = NULL;
	struct profiler_bw_cntrs_req cnt_buf;
	struct profiler_bw_cntrs_req_m cnt_buf_m;

	if (bw_profiling_disabled) {
		ret = copy_from_user(&cnt_buf_m, argp,
				sizeof(struct profiler_bw_cntrs_req_m));
	} else {
		ret = copy_from_user(&cnt_buf, argp,
				sizeof(struct profiler_bw_cntrs_req));
	}

	if (ret)
		return ret;
	/* Allocate memory for request */
	bwbuf = kzalloc(sizeof(struct tz_bw_svc_buf), GFP_KERNEL);
	if (bwbuf == NULL)
		return -ENOMEM;


	if (!bw_profiling_disabled) {
		bwbuf->bwreq.start_req.bwEnableFlags = cnt_buf.bwEnableFlags;
		switch (cnt_buf.cmd) {
		case TZ_BW_SVC_START_ID:
			ret = bw_profiling_start(bwbuf);
			if (ret)
				pr_err("bw_profiling_start Failed with ret: %d\n", ret);
			break;
		case TZ_BW_SVC_GET_ID:
			ret = bw_profiling_per_ip_get(argp, bwbuf);
			if (ret)
				pr_err("bw_profiling_get Failed with ret: %d\n", ret);
			break;
		case TZ_BW_SVC_STOP_ID:
			ret = bw_profiling_stop(bwbuf);
			if (ret)
				pr_err("bw_profiling_stop Failed with ret: %d\n", ret);
			break;
		default:
			pr_err("Invalid IOCTL: 0x%x\n", cnt_buf.cmd);
			ret = -EINVAL;
		}
	} else {
		switch (cnt_buf_m.cmd) {
		case TZ_BW_SVC_START_ID:
			ret = bw_profiling_start(bwbuf);
			if (ret)
				pr_err("bw_profiling_start Failed with ret: %d\n", ret);
			break;
		case TZ_BW_SVC_GET_ID:
			ret = bw_profiling_get(argp, bwbuf);
			if (ret)
				pr_err("bw_profiling_get Failed with ret: %d\n", ret);
			break;
		case TZ_BW_SVC_STOP_ID:
			ret = bw_profiling_stop(bwbuf);
			if (ret)
				pr_err("bw_profiling_stop Failed with ret: %d\n", ret);
			break;
		default:
			pr_err("Invalid IOCTL: 0x%x\n", cnt_buf_m.cmd);
			ret = -EINVAL;
		}
	}
	/* Free memory for command */
	if (bwbuf != NULL) {
		kfree(bwbuf);
		bwbuf = NULL;
	}
	return ret;
}

static int profiler_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	int lock_status = mutex_trylock(&profiler->lock);

	if (lock_status == 1) {
		file->private_data = profiler;
	} else
		return -EBUSY;

	return ret;
}

static long profiler_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	void __user *argp = (void __user *) arg;

	if (!profiler) {
		pr_err("Invalid/uninitialized device handle\n");
		return -EINVAL;
	}

	switch (cmd) {
	case PROFILER_IOCTL_GET_BW_INFO:
		bw_profiling_disabled = false;
		ret = profiler_get_bw_info(argp);
		if (ret)
			pr_err("failed get system bandwidth info: %d\n", ret);
		break;

	case PROFILER_IOCTL_GET_BW_INFO_BC:
		bw_profiling_disabled = true;
		ret = profiler_get_bw_info(argp);
		if (ret)
			pr_err("failed get system bandwidth info: %d\n", ret);
		break;

	default:
		pr_err("Invalid IOCTL: 0x%x\n", cmd);
		return -EINVAL;
	}
	return ret;
}

static int profiler_release(struct inode *inode, struct file *file)
{
	struct tz_bw_svc_buf *bwbuf = NULL;
	int ret = 0;

	pr_info("profiler release\n");

	mutex_unlock(&profiler->lock);

	bwbuf = kzalloc(sizeof(struct tz_bw_svc_buf), GFP_KERNEL);

	if (bwbuf == NULL)
		return -ENOMEM;

	ret = bw_profiling_stop(bwbuf);

	if (ret)
		pr_err("bw_profiling_stop Failed with ret: %d\n", ret);

	return 0;
}

static int profiler_info_init(struct conf_data *desc, struct platform_device *pdev)
{
	dev_params.num_llcc_channels = desc->num_llcc_channels;
	dev_params.num_gemnoc_metrics = desc->num_gemnoc_metrics;
	dev_params.num_hf_metrics = desc->num_hf_metrics;
	dev_params.num_sf_metrics = desc->num_sf_metrics;
	dev_params.gemnoc_channels = desc->gemnoc_channels;

	for (int i = 0; i < desc->num_llcc_channels; i++) {
		profiler->llcc_mmap_base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(profiler->llcc_mmap_base[i]))
			return dev_err_probe(&pdev->dev, PTR_ERR(profiler->llcc_mmap_base[i]),
						"Failed to ioremap llcc registers\n");
	}

	for (int i = 0; i < desc->gemnoc_channels; i++) {
		profiler->gemnoc_mmap_base[i] =
			devm_platform_ioremap_resource(pdev,
						(desc->num_llcc_channels + i));
		if (IS_ERR(profiler->gemnoc_mmap_base[i]))
			return dev_err_probe(&pdev->dev, PTR_ERR(profiler->gemnoc_mmap_base[i]),
						"Failed to ioremap gemnoc registers\n");
	}


	for (int i = 0; i < desc->num_llcc_channels; i++) {
		offset_reg_values.llcc_offset[i*2] = desc->llcc_offset
							+ LLCC_CAB0_COUNTER_OFFSET(0);
		offset_reg_values.llcc_offset[i*2 + 1] = desc->llcc_offset
							+ LLCC_CAB0_COUNTER_OFFSET(1);
		offset_reg_values.cabo_offset[i*2] = desc->cabo_offset
							+ LLCC_CAB0_COUNTER_OFFSET(9);//rd
		offset_reg_values.cabo_offset[i*2 + 1] = desc->cabo_offset
							+ LLCC_CAB0_COUNTER_OFFSET(11);//wr
	}

	for (int i = 0; i < desc->gemnoc_channels; i++) {
		for (int j = 0; j < desc->num_gemnoc_metrics; j++) {
			offset_reg_values.gemnoc_offset[i * desc->num_gemnoc_metrics + j]
							= GEMNOC_OFFSET(j);
		}
	}
	for (int hf = 0; hf < desc->num_hf_metrics; hf++)
		offset_reg_values.mmnoc_hf_offset[hf] = desc->mmnoc_hf_offset + MMNOC_OFFSETS(hf);

	for (int sf = 0; sf < desc->num_sf_metrics; sf++)
		offset_reg_values.mmnoc_sf_offset[sf] = desc->mmnoc_sf_offset + MMNOC_OFFSETS(sf);

	return 0;
}
static const struct file_operations profiler_fops = {
	.owner = THIS_MODULE,
	.open = profiler_open,
	.unlocked_ioctl = profiler_ioctl,
#ifdef CONFIG_COMPAT
	 .compat_ioctl = profiler_ioctl,
#endif
	.release = profiler_release
};

static int bwprofiler_probe(struct platform_device *pdev)
{
	int rc;
	struct device *class_dev;
	struct conf_data *desc;
	struct qcom_tzmem_pool_config pool_config;
	struct qcom_tzmem_pool *tzmem_pool;

	profiler = devm_kzalloc(&pdev->dev, sizeof(*profiler), GFP_KERNEL);

	if (!profiler)
		return -ENOMEM;

	mutex_init(&profiler->lock);

	memset(&pool_config, 0, sizeof(pool_config));
	pool_config.initial_size = 0;
	pool_config.policy = QCOM_TZMEM_POLICY_ON_DEMAND;
	pool_config.max_size = SZ_256K;

	profiler->tzmem_pool = qcom_tzmem_pool_new(&pool_config);
	if (IS_ERR(profiler->tzmem_pool)) {
		return dev_err_probe(&pdev->dev, "profiler: Failed to create qcom_tzmem_pool %d\n",
				PTR_ERR(profiler->tzmem_pool));
	}

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return dev_err_probe(&pdev->dev, -ENOMEM, "failed to probe chip info\n");

	rc = profiler_info_init(desc, pdev);
	if (rc < 0)
		return dev_err_probe(&pdev->dev, "%s: init chip info failed %d\n", __func__, rc);

	rc = alloc_chrdev_region(&profiler_device_no, 0, 1, PROFILER_DEV);
	if (rc < 0) {
		pr_err("alloc_chrdev_region failed %d\n", rc);
		return rc;
	}

	driver_class = class_create( PROFILER_DEV);
	if (IS_ERR(driver_class)) {
		rc = -ENOMEM;
		pr_err("class_create failed %d\n", rc);
		goto exit_unreg_chrdev_region;
	}

	class_dev = device_create(driver_class, NULL, profiler_device_no, NULL,
			PROFILER_DEV);
	if (IS_ERR(class_dev)) {
		pr_err("class_device_create failed %d\n", rc);
		rc = -ENOMEM;
		goto exit_destroy_class;
	}

	cdev_init(&profiler->cdev, &profiler_fops);
	profiler->cdev.owner = THIS_MODULE;

	rc = cdev_add(&profiler->cdev, MKDEV(MAJOR(profiler_device_no), 0), 1);
	if (rc < 0) {
		pr_err("%s: cdev_add failed %d\n", __func__, rc);
		goto exit_destroy_device;
	}

	profiler->pdev = class_dev;

	return 0;

exit_destroy_device:
	device_destroy(driver_class, profiler_device_no);
exit_destroy_class:
	class_destroy(driver_class);
exit_unreg_chrdev_region:
	unregister_chrdev_region(profiler_device_no, 1);

	return rc;
}

static int bwprofiler_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct conf_data sa8797_ddr_info = {
	.num_llcc_channels = 16,
	.num_gemnoc_metrics = 8,
	.num_hf_metrics = 16,
	.num_sf_metrics = 12,
	.llcc_offset = 0x69010,
	.cabo_offset = 0x10B0A0,
	.mmnoc_hf_offset = 0x46140,
	.mmnoc_sf_offset = 0x9140,
	.gemnoc_channels = 8,
};

static const struct conf_data sa8775_ddr_info = {
	.num_llcc_channels = 6,
	.num_gemnoc_metrics = 8,
	.num_hf_metrics = 16,
	.num_sf_metrics = 12,
	.llcc_offset = 0x36060,
	.cabo_offset = 0xAB0A0,
	.mmnoc_hf_offset = 0x4140,
	.mmnoc_sf_offset = 0x24140,
	.gemnoc_channels = 6,
};

static const struct conf_data sa7255_ddr_info = {
	.num_llcc_channels = 4,
	.num_gemnoc_metrics = 8,
	.num_hf_metrics = 16,
	.num_sf_metrics = 12,
	.llcc_offset = 0x36060,
	.cabo_offset = 0xAB0A0,
	.mmnoc_hf_offset = 0x5140,
	.mmnoc_sf_offset = 0x25140,
	.gemnoc_channels = 4,
};
static const struct of_device_id bwprofiler_of_match[] = {
	{ .compatible = "qcom,sa8797_ddr_bwprofiler", .data = &sa8797_ddr_info},
	{ .compatible = "qcom,sa8775_ddr_bwprofiler", .data = &sa8775_ddr_info},
	{ .compatible = "qcom,sa8255_ddr_bwprofiler", .data = &sa8775_ddr_info},
	{ .compatible = "qcom,sa8620_ddr_bwprofiler", .data = &sa7255_ddr_info},
	{ .compatible = "qcom,sa7255_ddr_bwprofiler", .data = &sa7255_ddr_info},
	{},
};

static struct platform_driver bwprofiler_driver = {
		.probe = bwprofiler_probe,
		.remove	= bwprofiler_remove,
		.driver	= {
			.name = "qcom_bwprofiler",
			.of_match_table = bwprofiler_of_match,
		}
};

module_platform_driver(bwprofiler_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. trustzone Communicator");



// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/arm-smccc.h>
#include <linux/cdev.h>
#include <linux/dma-buf.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/version.h>
#include <uapi/misc/scm_user_intf.h>

#if (LINUX_VERSION_CODE != KERNEL_VERSION(5, 14, 0))
#include <qcom_scm.h>
#endif

#define ARG_TO_ARRAY(type, ...) ((type[]) {__VA_ARGS__})

#define DMA_FD_ARGS(...) ARG_TO_ARRAY(__u32, __VA_ARGS__)

#define SVC_CMD_GRP(svc, num_of_cmds, ...)                      \
	{                                                       \
		.svc_id = (__u32)svc,                           \
		.num_cmd = (__u32)num_of_cmds,                  \
		.cmd_ids = ARG_TO_ARRAY(__u32, __VA_ARGS__),        \
	}

#define DT_LABEL_LENGTH      32
#define MAX_VM_SIZE          128
#define SRC_VMS_LIST_INDEX   2
#define DEST_VMS_LIST_INDEX  4
#define DEST_VMS_SIZE_INDEX  5

struct scm_user_res {
	u64 result[MAX_QCOM_SCM_RETS];
};

struct svc_cmd_list {
	__u32 svc_id;
	__u32 num_cmd;
	__u32 *cmd_ids;
};

struct svc_cmd_dma_list {
	__u32 svc_id;
	__u32 cmd_id;
	__u32 num_dma_fds;
	__u32 *dma_fds;
};

struct scm_dev_data {
	struct platform_device *scm_pdev;
	struct miscdevice scm_miscdev;
	struct device *dev;
};

static struct scm_dev_data *__scm_dev;

/*
 * SVC IC and CM ID pairs allowed to be called using SCM from userspace
 */
static const struct svc_cmd_list sip_tbl[] = {
	[QCOM_SCM_SVC_BOOT] = SVC_CMD_GRP(QCOM_SCM_SVC_BOOT,
					  1,
					  QCOM_SCM_BOOT_SET_REMOTE_STATE),

	[QCOM_SCM_SVC_PIL] = SVC_CMD_GRP(QCOM_SCM_SVC_PIL,
					 4,
					 QCOM_SCM_PIL_PAS_INIT_IMAGE,
					 QCOM_SCM_PIL_PAS_MEM_SETUP,
					 QCOM_SCM_PIL_PAS_AUTH_AND_RESET,
					 QCOM_SCM_PIL_PAS_SHUTDOWN),

	[QCOM_SCM_SVC_INFO] = SVC_CMD_GRP(QCOM_SCM_SVC_INFO,
					  4,
					  QCOM_SCM_INFO_IS_CALL_AVAIL,
					  QCOM_SCM_INFO_GET_FEAT_VERSION_CMD,
					  QCOM_SCM_GET_SECURE_STATE,
					  QCOM_SCM_INFO_BW_PROF_ID),

	[QCOM_SCM_SVC_MP] = SVC_CMD_GRP(QCOM_SCM_SVC_MP,
					5,
					QCOM_SCM_MP_VIDEO_VAR,
					QCOM_SCM_MP_ASSIGN,
					QCOM_SCM_MP_SHM_BRIDGE_ENABLE,
					QCOM_SCM_MP_SHM_BRIDGE_DELETE,
					QCOM_SCM_MP_SHM_BRIDGE_CREATE),

	[QCOM_SCM_SVC_SHE] = SVC_CMD_GRP(QCOM_SCM_SVC_SHE,
					 1,
					 QCOM_SCM_SHE_ID),

	[QCOM_SCM_SVC_SAFETY] = SVC_CMD_GRP(QCOM_SCM_SVC_SAFETY,
					    1,
					    QCOM_SCM_SAFETY_ENABLE_FFI_ID),

	[QCOM_SCM_SVC_LMH] = SVC_CMD_GRP(QCOM_SCM_SVC_LMH,
					    1,
					    QCOM_SCM_LMH_LIMIT_PROFILE_CHANGE),

	[QCOM_SCM_SVC_CAMERA] = SVC_CMD_GRP(QCOM_SCM_SVC_CAMERA,
					    1,
					    QCOM_SCM_CAMERA_UPDATE_CAMNOC_QOS),

	[QCOM_SCM_SVC_GPU] = SVC_CMD_GRP(QCOM_SCM_SVC_GPU,
					 2,
					 QCOM_SCM_SVC_GPU0_INIT_REGS,
					 QCOM_SCM_SVC_GPU1_INIT_REGS),
};

/*
 * SVC/ Cmd ID pair that needs DMA buf fd to PA translation
 */
static const struct svc_cmd_dma_list dma_fd_tbl[] = {

	{QCOM_SCM_SVC_SHE, QCOM_SCM_SHE_ID, 1, DMA_FD_ARGS(2)},

};

#if (LINUX_VERSION_CODE == KERNEL_VERSION(5, 14, 0))
int qcom_scm_kgsl_set_smmu_aperture(unsigned int num_context_bank);

int qcom_scm_kgsl_set_smmu_aperture(unsigned int num_context_bank)
{
	struct qcom_scm_desc desc = {
		.svc = QCOM_SCM_SVC_MP,
		.cmd = QCOM_SCM_MP_CP_SMMU_APERTURE_ID,
		.owner = ARM_SMCCC_OWNER_SIP,
		.args[0] = 0xffff0000
                           | ((QCOM_SCM_CP_APERTURE_REG & 0xff) << 8)
                           | (num_context_bank & 0xff),
		.args[1] = 0xffffffff,
		.args[2] = 0xffffffff,
		.args[3] = 0xffffffff,
		.arginfo = QCOM_SCM_ARGS(4),
	};

	return qcom_scm_call(__scm_dev->dev, &desc, NULL);
}
EXPORT_SYMBOL(qcom_scm_kgsl_set_smmu_aperture);

int qcom_scm_ddrbw_profiler(uint64_t in_buf,
    size_t in_buf_size, uint64_t out_buf, size_t out_buf_size)
{
	int ret;
	struct qcom_scm_desc desc = {
		.svc = QCOM_SCM_SVC_INFO,
		.cmd = TZ_SVC_BW_PROF_ID,
		.owner = ARM_SMCCC_OWNER_SIP,
	};

	desc.args[0] = in_buf;
	desc.args[1] = in_buf_size;
	desc.args[2] = out_buf;
	desc.args[3] = out_buf_size;
	desc.arginfo = QCOM_SCM_ARGS(4, QCOM_SCM_RW, QCOM_SCM_VAL, QCOM_SCM_RW,
				 QCOM_SCM_VAL);
	ret = qcom_scm_call(__scm_dev->dev, &desc, NULL);

	return ret;
}
EXPORT_SYMBOL(qcom_scm_ddrbw_profiler);
#endif

static int qcom_scm_intf_assign_mem(struct scm_dev_data *dev_data,
                                 struct scm_hand_shake scm_data)
{
	struct device_node *node;
	struct resource of_res;
	char name[DT_LABEL_LENGTH];
	int ret = 0;

	void __user *destVM_arr = (void __user *)scm_data.args_buffer[DEST_VMS_LIST_INDEX];
	u64 srcVM = BIT(scm_data.args_buffer[SRC_VMS_LIST_INDEX]);
	u64 destVM_cnt = scm_data.args_buffer[DEST_VMS_SIZE_INDEX];

	if (!destVM_cnt || destVM_cnt > MAX_VM_SIZE)
		return -EFAULT;

	// Parse DT label in arg[0] and read PA from reserved-memory node.
	if (copy_from_user(name, (char *)scm_data.args_buffer[0], sizeof(name)))
		return -EFAULT;

	//Null terminate the name to ensure safety
	name[DT_LABEL_LENGTH - 1] = '\0';

	node = of_find_node_by_name(NULL, name);
	if (!node) {
		dev_err(dev_data->dev, "Failed to find DT node : %s\n", name);
		return -ENODEV;
	}

	ret = of_address_to_resource(node, 0, &of_res);
	if (ret) {
		of_node_put(node);
		dev_err(dev_data->dev, "Failed to parse memory region\n");
		return ret;
	}
	of_node_put(node);

	struct qcom_scm_vmperm *destVM __free(kfree) = kzalloc(destVM_cnt *
					   sizeof(struct qcom_scm_vmperm), GFP_KERNEL);
	if(!destVM)
		return -ENOMEM;

	if (copy_from_user(destVM, destVM_arr,
			   destVM_cnt * sizeof(struct qcom_scm_vmperm)))
		return -EFAULT;

	ret = qcom_scm_assign_mem(of_res.start, resource_size(&of_res),
					   &srcVM, destVM, destVM_cnt);

	return ret;
}

static int get_pa_from_dmabuf_fd(struct dma_buf* dma_buf, u64 *p_addr)
{
	struct dma_buf_attachment *buf_attach = NULL;
	struct sg_table *sgt = NULL;
	u64 dmabuf_p_addr;
	int ret = 0;

	buf_attach = dma_buf_attach(dma_buf, __scm_dev->dev);
	if (IS_ERR(buf_attach)) {
		ret = -ENOMEM;
		dev_err(__scm_dev->dev, "dma buf attach failed, ret: %d\n", ret);
		goto out;
	}

	sgt = dma_buf_map_attachment(buf_attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		dev_err(__scm_dev->dev, "mapping dma buffers failed, ret: %ld\n", PTR_ERR(sgt));
		ret = -ENOMEM;
		goto out;
	}

	/* contiguous only => nents=1 */
	if (sgt->nents != 1) {
		ret = -EINVAL;
		dev_err(__scm_dev->dev, "sg entries are not contigous, ret: %d\n", ret);
		goto out;
	}

	dmabuf_p_addr = sg_dma_address(sgt->sgl);
	if (!dmabuf_p_addr) {
		ret = -EINVAL;
		dev_err(__scm_dev->dev, "invalid physical address, ret: %d\n", ret);
		goto out;
	}
	*p_addr = dmabuf_p_addr;

out:
	if (sgt) {
		dma_buf_unmap_attachment(buf_attach, sgt, DMA_BIDIRECTIONAL);
	}
	if (buf_attach) {
		dma_buf_detach(dma_buf, buf_attach);
	}
	return ret;
}

static int get_dmafd_args(unsigned int svc_id, unsigned int cmd_id, unsigned int *dma_fds)
{
	u32 len = sizeof(dma_fd_tbl) / sizeof(dma_fd_tbl[0]);
	int i;

	struct svc_cmd_dma_list dma_args;
	for(i = 0; i < len; i++) {
		dma_args = dma_fd_tbl[i];
		if (dma_args.svc_id == svc_id && dma_args.cmd_id == cmd_id) {
			memcpy(dma_fds, dma_args.dma_fds, sizeof(u32) * dma_args.num_dma_fds);

			return dma_args.num_dma_fds;
		}
	}

	return -EINVAL;
}

static int translate_fd_to_pa(unsigned  int *dmabuf_fd,
			      unsigned int no_dma_fds,
			      long long unsigned int *scm_args)
{
	struct dma_buf *dma_buf = NULL;
	u64 pa = 0;
	int i;

        for (i = 0; i < no_dma_fds; i++) {
                dma_buf = dma_buf_get(scm_args[dmabuf_fd[i]]);
                if (IS_ERR_OR_NULL(dma_buf)) {
                        dev_err(__scm_dev->dev, "dma buf get failed\n");
                        return -EFAULT;
                }

		if (get_pa_from_dmabuf_fd(dma_buf, &pa) < 0) {
			dma_buf_put(dma_buf);
			return -EINVAL;
		}

		scm_args[dmabuf_fd[i]] = pa;
		dma_buf_put(dma_buf);
	}

	return 0;
}

static bool validate_svc_cmd(unsigned int svc_id, unsigned int cmd_id)
{
	struct svc_cmd_list svc_cmd_tmp;
	int i;

	switch (svc_id) {
	case QCOM_SCM_SVC_BOOT:
	case QCOM_SCM_SVC_PIL:
	case QCOM_SCM_SVC_INFO:
	case QCOM_SCM_SVC_MP:
	case QCOM_SCM_SVC_SHE:
	case QCOM_SCM_SVC_SAFETY:
	case QCOM_SCM_SVC_LMH:
	case QCOM_SCM_SVC_CAMERA:
	case QCOM_SCM_SVC_GPU:

		svc_cmd_tmp = sip_tbl[svc_id];
		break;

	default:
		return false;
	}

	for (i = 0; i < svc_cmd_tmp.num_cmd; i++) {
		if (cmd_id == svc_cmd_tmp.cmd_ids[i])
			return true;
	}

	return false;
}

static int qcom_scm_open(struct inode *inode, struct file *filp)
{
	struct scm_dev_data *dev_data = container_of(filp->private_data,
						     struct scm_dev_data,
						     scm_miscdev);

	filp->private_data = dev_data;
	return 0;
}

static long scm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct scm_dev_data *dev_data = (struct scm_dev_data *)file->private_data;
	void __user *ip = (void __user *)arg;
	struct scm_hand_shake scm_data;
#if (LINUX_VERSION_CODE == KERNEL_VERSION(5, 14, 0))
	struct platform_device *scm_pdev = dev_data->scm_pdev;
	struct qcom_scm_desc desc;
	struct qcom_scm_res res;
#else
	struct scm_user_res res;
	u64 res1 = 0;
#endif

	int i, no_of_args, no_dma_fds, ret = 0;
	u32 dma_fd_args[MAX_QCOM_SCM_ARGS];

	if (cmd != SCM_HAND_SHAKE_IOCTL)
		return -EFAULT;

	if (copy_from_user(&scm_data, ip, sizeof(struct scm_hand_shake)))
		return -EFAULT;

	// Validate allowed SVC ID and CMD pairs
	if (!validate_svc_cmd(scm_data.svc, scm_data.cmd)) {
		dev_err(dev_data->dev, "Unsupported svc: %d and command:%d\n",
			scm_data.svc, scm_data.cmd);
		return -EINVAL;
	}

	// Validate number of args
	no_of_args = scm_data.arginfo & 0x0F;
	if (no_of_args > MAX_QCOM_SCM_ARGS)
		return -EINVAL;

        no_dma_fds = get_dmafd_args(scm_data.svc, scm_data.cmd, dma_fd_args);
        if (no_dma_fds > 0) {
                //translate fd to pa
                ret = translate_fd_to_pa(dma_fd_args, no_dma_fds, scm_data.args_buffer);
                if (ret)
                        return ret;
        }

#if (LINUX_VERSION_CODE != KERNEL_VERSION(5, 14, 0))
	switch(scm_data.svc) {
	case QCOM_SCM_SVC_BOOT:
		switch(scm_data.cmd) {
		case QCOM_SCM_BOOT_SET_REMOTE_STATE:
			scm_data.ret = qcom_scm_set_remote_state(scm_data.args_buffer[0],
							         scm_data.args_buffer[1]);
			break;
		}
		break;

	case QCOM_SCM_SVC_PIL:
		switch(scm_data.cmd) {
		case QCOM_SCM_PIL_PAS_INIT_IMAGE:
		{
			void *data;
			__u32 meta_len = scm_data.args_buffer[2];

			data = memremap(scm_data.args_buffer[1], meta_len, MEMREMAP_WB);
			if(!data)
				return -ENOMEM;

			scm_data.ret = qcom_scm_pas_init_image(scm_data.args_buffer[0],
							       data, meta_len, NULL);
			if(data)
				memunmap(data);
			break;
		}

		case QCOM_SCM_PIL_PAS_MEM_SETUP:
			scm_data.ret = qcom_scm_pas_mem_setup(scm_data.args_buffer[0],
							      scm_data.args_buffer[1],
							      scm_data.args_buffer[2]);
			break;
		case QCOM_SCM_PIL_PAS_AUTH_AND_RESET:
			scm_data.ret = qcom_scm_pas_auth_and_reset(scm_data.args_buffer[0]);
			break;
		case QCOM_SCM_PIL_PAS_SHUTDOWN:
			scm_data.ret = qcom_scm_pas_shutdown(scm_data.args_buffer[0]);
			break;
		}
		break;

	case QCOM_SCM_SVC_INFO:
		switch(scm_data.cmd) {
		case QCOM_SCM_INFO_GET_FEAT_VERSION_CMD:
			scm_data.ret = qcom_scm_get_tz_feat_id_version(scm_data.args_buffer[0], NULL);
			break;
		case  QCOM_SCM_INFO_BW_PROF_ID:
			scm_data.ret = qcom_scm_ddrbw_profiler(scm_data.args_buffer[0],
							       scm_data.args_buffer[1],
							       scm_data.args_buffer[2],
							       scm_data.args_buffer[3]);
			break;

		case QCOM_SCM_GET_SECURE_STATE:
				scm_data.ret = qcom_scm_get_secure_state(&res1);
				res.result[0] = res1;

			break;

		}
		break;

	case QCOM_SCM_SVC_MP:
		switch(scm_data.cmd) {
		case QCOM_SCM_MP_VIDEO_VAR:
			scm_data.ret = qcom_scm_mem_protect_video_var(scm_data.args_buffer[0],
								      scm_data.args_buffer[1],
								      scm_data.args_buffer[2],
								      scm_data.args_buffer[3]);
			break;
		case QCOM_SCM_MP_ASSIGN:
			scm_data.ret = qcom_scm_intf_assign_mem(dev_data, scm_data);
			break;
		}
		break;

	case QCOM_SCM_SVC_SHE:
		switch(scm_data.cmd) {
		case QCOM_SCM_SHE_ID:
			scm_data.ret = qcom_scm_she_op(scm_data.args_buffer[0],
						       scm_data.args_buffer[1],
						       scm_data.args_buffer[2],
						       scm_data.args_buffer[3],
						       &res1);
			res.result[0] = res1;
			break;
		}
		break;

	case QCOM_SCM_SVC_LMH:
		switch(scm_data.cmd) {
		case QCOM_SCM_LMH_LIMIT_PROFILE_CHANGE:
			scm_data.ret = qcom_scm_lmh_profile_change(scm_data.args_buffer[0]);
			break;
		}
		break;

	case QCOM_SCM_SVC_CAMERA:
		switch(scm_data.cmd) {
		case QCOM_SCM_CAMERA_UPDATE_CAMNOC_QOS:
		{
			__u32 size;
			struct qcom_scm_camera_qos scm_buf[QCOM_SCM_CAMERA_MAX_QOS_CNT] = {0};
			size = scm_data.args_buffer[2] * sizeof(struct qcom_scm_camera_qos);

			if (copy_from_user(scm_buf,
					(struct qcom_scm_camera_qos *)scm_data.args_buffer[1],
					min(size, sizeof(scm_buf)))) {
				dev_err(dev_data->dev, "copy_from_user failed\n");
			}
			scm_data.ret = qcom_scm_camera_update_camnoc_qos(
								scm_data.args_buffer[0],
								scm_data.args_buffer[2],
								scm_buf);
			break;
		}
		}
		break;

	case QCOM_SCM_SVC_GPU:
		scm_data.ret = qcom_scm_multi_kgsl_init_regs(scm_data.args_buffer[0], scm_data.cmd);
		break;
	}

#else
	desc.svc = scm_data.svc;
	desc.cmd = scm_data.cmd;
	desc.owner = ARM_SMCCC_OWNER_SIP;
	desc.arginfo = scm_data.arginfo;

	for (id = 0; id < no_of_args; id++)
		desc.args[id] = scm_data.args_buffer[id];

	// Special case for PIL_INIT_IMAGE, to remove metadata length
	if (desc.cmd == QCOM_SCM_PIL_PAS_INIT_IMAGE) {
		desc.arginfo = 0x82;
		desc.args[2] = 0;
	}

	scm_data.ret = qcom_scm_call(&scm_pdev->dev, &desc, &res);
#endif

	for (i = 0; i < MAX_QCOM_SCM_RETS; i++)
		scm_data.qcom_scm_res[i] = res.result[i];

	if (copy_to_user(ip, &scm_data, sizeof(struct scm_hand_shake)))
		ret = -EFAULT;

	if (scm_data.ret)
		dev_err(dev_data->dev, "scm ioctl failed for svc_id : %u and cmd_id : %u with ret : %d\n",
			scm_data.svc, scm_data.cmd, scm_data.ret);

        return ret;
}

static const struct file_operations qcom_scm_fops = {
	.open = qcom_scm_open,
	.unlocked_ioctl = scm_ioctl,
};

static int qcom_scm_intf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct scm_dev_data *dev_data;
	int ret = 0;

	if (!qcom_scm_is_available())
		dev_err_probe(dev_data->dev, -EPROBE_DEFER, "qcom_scm is not up!\n");

	dev_data = devm_kzalloc(dev, sizeof(struct scm_dev_data), GFP_KERNEL);
	if (!dev_data)
		return -ENOMEM;

	dev_data->scm_pdev = pdev;
	dev_data->dev = &pdev->dev;

	dev_data->scm_miscdev.minor = MISC_DYNAMIC_MINOR;
	dev_data->scm_miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
						    "scmnode");
	dev_data->scm_miscdev.fops = &qcom_scm_fops;

	ret = misc_register(&dev_data->scm_miscdev);
	if (ret) {
		dev_err(dev, "failed to register misc device. err %d\n", ret);
		return ret;
	}

	__scm_dev = dev_data;
	__scm_dev->dev = dev_data->dev;

	platform_set_drvdata(pdev, dev_data);
	return 0;
}

static int qcom_scm_intf_remove(struct platform_device *pdev)
{
	struct scm_dev_data *dev_data = platform_get_drvdata(pdev);

	misc_deregister(&dev_data->scm_miscdev);
	return 0;
}

static const struct of_device_id qcom_scm_intf_dt_match[] = {
	{.compatible = "qcom,scm-user-intf",
	 },
	{}
};

MODULE_DEVICE_TABLE(of, qcom_scm_intf_dt_match);

static struct platform_driver qcom_scm_intf_driver = {
	.driver = {
		   .name = "qcom_scm_intf",
		   .of_match_table = qcom_scm_intf_dt_match,
		   .suppress_bind_attrs = true,
		   },
	.probe = qcom_scm_intf_probe,
	.remove = qcom_scm_intf_remove,
};

module_platform_driver(qcom_scm_intf_driver);
MODULE_IMPORT_NS(DMA_BUF);
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. user interface SCM driver");
MODULE_LICENSE("GPL v2");

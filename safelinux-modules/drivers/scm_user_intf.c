// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/arm-smccc.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <uapi/misc/scm_user_intf.h>
#include <linux/dma-buf.h>

#define ARG_TO_ARRAY(type, ...) ((type[]) {__VA_ARGS__})

#define DMA_FD_ARGS(...) ARG_TO_ARRAY(__u32, __VA_ARGS__)

#define SVC_CMD_GRP(svc, num_of_cmds, ...)                      \
	{                                                       \
		.svc_id = (__u32)svc,                           \
		.num_cmd = (__u32)num_of_cmds,                  \
		.cmd_ids = ARG_TO_ARRAY(__u32, __VA_ARGS__),        \
	}

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
					  3,
					  QCOM_SCM_INFO_IS_CALL_AVAIL,
					  QCOM_SCM_INFO_GET_FEAT_VERSION_CMD,
					  QCOM_SCM_INFO_BW_PROF_ID),

	[QCOM_SCM_SVC_MP] = SVC_CMD_GRP(QCOM_SCM_SVC_MP,
					4,
					QCOM_SCM_MP_VIDEO_VAR,
					QCOM_SCM_MP_SHM_BRIDGE_ENABLE,
					QCOM_SCM_MP_SHM_BRIDGE_DELETE,
					QCOM_SCM_MP_SHM_BRIDGE_CREATE),

	[QCOM_SCM_SVC_SHE] = SVC_CMD_GRP(QCOM_SCM_SVC_SHE,
					 1,
					 QCOM_SCM_SHE_ID),

	[QCOM_SCM_SVC_SAFETY] = SVC_CMD_GRP(QCOM_SCM_SVC_SAFETY,
					    1,
					    QCOM_SCM_SAFETY_ENABLE_FFI_ID),
};

/*
 * SVC/ Cmd ID pair that needs DMA buf fd to PA translation
 */
static const struct svc_cmd_dma_list dma_fd_tbl[] = {

	{QCOM_SCM_SVC_SHE, QCOM_SCM_SHE_ID, 1, DMA_FD_ARGS(2)},

};

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

                if (get_pa_from_dmabuf_fd(dma_buf, &pa) < 0)
                        return -EINVAL;

                scm_args[dmabuf_fd[i]] = pa;
        }

	if (!IS_ERR_OR_NULL(dma_buf))
		dma_buf_put(dma_buf);

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
	struct platform_device *scm_pdev = dev_data->scm_pdev;
	void __user *ip = (void __user *)arg;
	struct scm_hand_shake scm_data;
	struct qcom_scm_desc desc;
	struct qcom_scm_res res;
	int id, i, no_of_args, no_dma_fds, ret = 0;
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

	desc.svc = scm_data.svc;
	desc.cmd = scm_data.cmd;
	desc.owner = ARM_SMCCC_OWNER_SIP;

	// Validate number of args
	no_of_args = scm_data.arginfo & 0x0F;
	if (no_of_args > MAX_QCOM_SCM_ARGS)
		ret = -EINVAL;

	desc.arginfo = scm_data.arginfo;

	for (id = 0; id < no_of_args; id++)
		desc.args[id] = scm_data.args_buffer[id];

        no_dma_fds = get_dmafd_args(desc.svc, desc.cmd, dma_fd_args);
        if (no_dma_fds > 0) {
                //translate fd to pa
                ret = translate_fd_to_pa(dma_fd_args, no_dma_fds, desc.args);
                if (ret)
                        return ret;
        }

	scm_data.ret = qcom_scm_call(&scm_pdev->dev, &desc, &res);
	if (scm_data.ret)
		dev_err(dev_data->dev, "scm ioctl failed - ret : %d\n", scm_data.ret);

	for (i = 0; i < MAX_QCOM_SCM_RETS; i++)
		scm_data.qcom_scm_res[i] = res.result[i];

	if (copy_to_user(ip, &scm_data, sizeof(struct scm_hand_shake)))
		ret = -EFAULT;


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

	put_device(dev_data->dev);
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

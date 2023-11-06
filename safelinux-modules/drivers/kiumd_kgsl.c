/* SPDX-License-Identifier: GPL-2.0-only
 ** Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 **/
#include <linux/adreno-smmu-priv.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>


int kiumd_kgsl_probe(struct platform_device *pdev)
{
	struct adreno_smmu_priv *adrenodevice;
	struct device *dev = &pdev->dev;
	struct device_node *node = pdev->dev.of_node;
	int ret;

	adrenodevice = devm_kzalloc(dev, sizeof(*adrenodevice), GFP_KERNEL);
	if (!adrenodevice)
		return -ENOMEM;

	dev_set_drvdata(dev, adrenodevice);
	dma_set_coherent_mask(dev, DMA_BIT_MASK(64));
	ret = of_dma_configure(dev, node, true);
	if (ret) {
		dev_err(dev, "%s:of_dma_configure failed, with error: %d\n",__func__, ret);
		return ret;
	}

	return 0;
}

static int kiumd_kgsl_remove(struct platform_device *pdev)
{
	dev_set_drvdata(&pdev->dev, NULL);
	return 0;
}

static const struct of_device_id  kiumd_kgsl_id[] = {
	{.compatible = "qcom,kiumd-platform",},
	{},
};

static struct platform_driver kiumd_kgsl = {
	.probe = kiumd_kgsl_probe,
	.remove = kiumd_kgsl_remove,
	.driver = {
		.name = "kiumd_kgsl",
		.of_match_table =  kiumd_kgsl_id,
		.owner = THIS_MODULE,
	}
};

module_platform_driver(kiumd_kgsl);

MODULE_DESCRIPTION("kiumd_kgsl");
MODULE_LICENSE("GPL v2");


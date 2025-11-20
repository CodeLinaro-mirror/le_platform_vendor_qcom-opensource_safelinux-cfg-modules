// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

struct apps_pinctrl_data {
	struct pinctrl *pinctrl;
	struct pinctrl_state *default_state;
	struct pinctrl_state *sleep_state;
};

static int apps_pinctrl_probe(struct platform_device *pdev)
{
	struct apps_pinctrl_data *data;
	int ret = 0;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR_OR_NULL(data->pinctrl)) {
		ret = PTR_ERR(data->pinctrl);
		dev_err(&pdev->dev, "Failed to get pinctrl, err = %d\n", ret);
		return ret;
	}

	data->default_state = pinctrl_lookup_state(data->pinctrl, "default");
	if (IS_ERR(data->default_state)) {
		ret = PTR_ERR(data->default_state);
		dev_err(&pdev->dev, "Failed to get default state, err = %d\n", ret);
		return ret;
	}

	data->sleep_state = pinctrl_lookup_state(data->pinctrl, "sleep");
	if (IS_ERR(data->sleep_state))
		dev_info(&pdev->dev, "No sleep state defined\n");

	platform_set_drvdata(pdev, data);

	return ret;
}

static int apps_pinctrl_remove(struct platform_device *pdev)
{
	return 0;
}

static int apps_pinctrl_suspend(struct device *dev)
{
	struct apps_pinctrl_data *data = dev_get_drvdata(dev);
	int ret = 0;

	if (!data || !data->pinctrl || IS_ERR(data->sleep_state))
		return ret;

	ret = pinctrl_select_state(data->pinctrl, data->sleep_state);
	if (ret)
		dev_err(dev, "Suspend: failed to set sleep state, err = %d\n", ret);

	return ret;
}

static int apps_pinctrl_resume(struct device *dev)
{
	struct apps_pinctrl_data *data = dev_get_drvdata(dev);
	int ret = 0;

	if (!data || !data->pinctrl || IS_ERR(data->default_state))
		return ret;

	ret = pinctrl_select_state(data->pinctrl, data->default_state);
	if (ret)
		dev_err(dev, "Resume: failed to restore default state\n");

	return ret;
}

static const struct dev_pm_ops apps_pinctrl_pm_ops = {
	.suspend = apps_pinctrl_suspend,
	.resume  = apps_pinctrl_resume,
};

static const struct of_device_id apps_pinctl_id[] = {
	{.compatible = "qcom,apps-pinctlr",},
	{},
};

static struct platform_driver apps_pinctrl = {
	.probe = apps_pinctrl_probe,
	.remove = apps_pinctrl_remove,
	.driver = {
		.name = "apps_pinctrl",
		.of_match_table = apps_pinctl_id,
		.pm    = &apps_pinctrl_pm_ops,   /* Attach PM ops */
	}
};

static int apps_pinctrl_init(void)
{
    return platform_driver_register(&apps_pinctrl);
}
module_init(apps_pinctrl_init);
MODULE_DESCRIPTION("apps_pinctrl");
MODULE_LICENSE("GPL v2");

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

static int apps_pinctrl_probe(struct platform_device *pdev)
{
        struct pinctrl *pinctrl;
        struct pinctrl_state *appsdefault;
        int ret = 0;

        pinctrl = devm_pinctrl_get(&pdev->dev);

        if (IS_ERR_OR_NULL(pinctrl)) {
                ret = PTR_ERR(pinctrl);
                dev_err(&pdev->dev, "Failed to get pinctrl, err = %d", ret);
                return ret;
        }

        dev_info(&pdev->dev, "get pinctrl succeed\n");

        appsdefault = pinctrl_lookup_state(pinctrl, "default");
        if (IS_ERR_OR_NULL(appsdefault)) {
                ret = PTR_ERR(appsdefault);
                dev_err(&pdev->dev, "Failed to get pinctrl state, err = %d", ret);
                return ret;
        }

        ret = pinctrl_select_state(pinctrl, appsdefault);

        if(ret)
                dev_err(&pdev->dev, "Failed to get pinctrl state, err = %d", ret);
        else
                dev_dbg(&pdev->dev, "Set pinctrl state succeeded");

       return (ret);
}

static int apps_pinctrl_remove(struct platform_device *pdev)
{
    return 0;
}

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
            .owner = THIS_MODULE,
        }
};
static int apps_pinctrl_init(void)
{
    return platform_driver_register(&apps_pinctrl);
}
module_init(apps_pinctrl_init);
MODULE_DESCRIPTION("apps_pinctrl");
MODULE_LICENSE("GPL v2");

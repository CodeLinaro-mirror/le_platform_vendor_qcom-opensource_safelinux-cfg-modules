/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>


struct remote_irq_priv {
        struct device_node *tz_node;
};

static irqreturn_t virq_thread_fn(int irq, void *dev_id)
{
        struct remote_irq_priv *priv = dev_id;
        const char *tz_name;
        struct thermal_zone_device *tz;

        if (!priv || !priv->tz_node)
                return IRQ_HANDLED;

        tz_name = priv->tz_node->name;
        if (!tz_name)
                return IRQ_HANDLED;

        tz = thermal_zone_get_zone_by_name(tz_name);
        if (IS_ERR(tz)) {
                pr_err("%s: cannot find thermal zone\n", tz_name);
                return IRQ_HANDLED;
        }

        thermal_zone_device_update(tz, THERMAL_TRIP_VIOLATED);
        return IRQ_HANDLED;
}

static int virq_probe(struct platform_device *pdev)
{
        int ret, irq;
        struct remote_irq_priv *priv;
        struct device *dev = &pdev->dev;
        struct device_node *tz_dn;

        priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
        if (!priv)
                return -ENOMEM;

        tz_dn = of_parse_phandle(dev->of_node, "thermal-zone", 0);
        if (tz_dn)
                priv->tz_node = tz_dn;

        platform_set_drvdata(pdev, priv);

        irq = platform_get_irq(pdev, 0);
        if (irq < 0) {
                dev_err(dev, "failed to get IRQ: %d\n", irq);
                if (tz_dn)
                        of_node_put(tz_dn);
                return irq;
        }

        ret = devm_request_threaded_irq(dev, irq, NULL, virq_thread_fn,
                        IRQF_TRIGGER_RISING | IRQF_ONESHOT,
                        dev->of_node->name, priv);

        if (ret) {
                dev_err(dev, "request irq failed: %d\n", ret);
                if (tz_dn)
                        of_node_put(tz_dn);
                return ret;
        }

        dev_info(dev, "virq bound to thermal zone \"%s\"\n",
                        tz_dn ? tz_dn->name : "<none>");
        return 0;
}

static int virq_remove(struct platform_device *pdev)
{
        struct remote_irq_priv *priv = dev_get_drvdata(&pdev->dev);

        if (priv && priv->tz_node)
                of_node_put(priv->tz_node);
        dev_info(&pdev->dev, "Virtual IRQ driver removed\n");
        return 0;
}

static const struct of_device_id virq_of_match[] = {
        { .compatible = "qcom,remote-virq", },
        { }
};
MODULE_DEVICE_TABLE(of, virq_of_match);

static struct platform_driver virq_driver = {
        .probe  = virq_probe,
        .remove = virq_remove,
        .driver = {
                .name           = "qcom-virq",
                .of_match_table = virq_of_match,
        },
};

module_platform_driver(virq_driver);

MODULE_DESCRIPTION("Qualcomm remote virtual IRQ driver");
MODULE_LICENSE("GPL");

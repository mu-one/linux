// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 The Asahi Linux Contributors
 *
 * PA Semi PWRficient SMBus host driver for Apple SoCs
 */

#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#include "i2c-pasemi-core.h"

struct pasemi_i2c_platform_data {
	struct pasemi_smbus smbus;

	struct clk *clk_ref;
	struct clk *clk_gate;

	struct pinctrl *pctrl;
};

static int
pasemi_i2c_platform_calc_clk_div(struct pasemi_i2c_platform_data *data,
				 u32 frequency)
{
	unsigned long clk_rate = clk_get_rate(data->clk_ref);

	if (!clk_rate)
		return -EINVAL;

	data->smbus.clk_div = DIV_ROUND_UP(clk_rate, 16 * frequency);
	if (data->smbus.clk_div < 4)
		return -EINVAL;
	if (data->smbus.clk_div > 0xff)
		return -EINVAL;

	return 0;
}

static int pasemi_i2c_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pasemi_i2c_platform_data *data;
	struct pasemi_smbus *smbus;
	struct resource *res;
	u32 frequency;
	int error;

	data = devm_kzalloc(dev, sizeof(struct pasemi_i2c_platform_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	smbus = &data->smbus;
	smbus->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	if (of_property_read_u32(dev->of_node, "clock-frequency", &frequency))
		frequency = I2C_MAX_STANDARD_MODE_FREQ;

	smbus->ioaddr = devm_ioremap_resource(smbus->dev, res);
	if (IS_ERR(smbus->ioaddr))
		return PTR_ERR(smbus->ioaddr);

	data->clk_ref = devm_clk_get(dev, "ref");
	if (IS_ERR(data->clk_ref))
		return PTR_ERR(data->clk_ref);
	data->clk_gate = devm_clk_get(dev, "gate");
	if (IS_ERR(data->clk_gate))
		return PTR_ERR(data->clk_gate);

	error = clk_prepare_enable(data->clk_ref);
	if (error)
		return error;
	error = clk_prepare_enable(data->clk_gate);
	if (error)
		goto out_clk_disable_ref;

	error = pasemi_i2c_platform_calc_clk_div(data, frequency);
	if (error) {
		dev_err(dev, "cannot set bus frequency to %dHz\n", frequency);
		goto out_clk_disable_gate;
	}

	data->pctrl = devm_pinctrl_get_select_default(dev);
	if (IS_ERR(data->pctrl)) {
		dev_err(dev, "failed to configure pins.\n");
		goto out_clk_disable_gate;
	}

	smbus->adapter.dev.of_node = pdev->dev.of_node;
	error = pasemi_i2c_common_probe(smbus);
	if (error)
		goto out_clk_disable_gate;

	platform_set_drvdata(pdev, data);

	return 0;

out_clk_disable_gate:
	clk_disable_unprepare(data->clk_gate);
out_clk_disable_ref:
	clk_disable_unprepare(data->clk_ref);

	return error;
}

static int pasemi_i2c_platform_remove(struct platform_device *pdev)
{
	struct pasemi_i2c_platform_data *data = platform_get_drvdata(pdev);

	clk_disable_unprepare(data->clk_gate);
	clk_disable_unprepare(data->clk_ref);

	return 0;
}

static const struct of_device_id pasemi_i2c_of_match[] = {
	{ .compatible = "apple,t8103-i2c" },
	{ .compatible = "apple,i2c" },
	{},
};
MODULE_DEVICE_TABLE(of, pasemi_i2c_of_match);

static struct platform_driver pasemi_i2c_platform_driver = {
	.driver	= {
		.name			= "i2c-pasemi",
		.of_match_table		= pasemi_i2c_of_match,
	},
	.probe	= pasemi_i2c_platform_probe,
	.remove	= pasemi_i2c_platform_remove,
};
module_platform_driver(pasemi_i2c_platform_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("Apple SMBus platform driver");

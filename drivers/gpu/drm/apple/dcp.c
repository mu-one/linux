// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>

struct apple_dcp {
	void __iomem		*regs;
};

static int dcp_platform_probe(struct platform_device *pdev)
{
	struct apple_dcp *dcp;
	int ret;

	dcp = devm_kzalloc(&pdev->dev, sizeof(struct apple_dcp), GFP_KERNEL);
	if (IS_ERR(dcp))
		return PTR_ERR(dcp);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

	dev_info(&pdev->dev, "Probing DCP");

	if (ret)
		return ret;

	return ret;
}

static int dcp_platform_remove(struct platform_device *pdev)
{
	printk("removing dcp\n");
	return 0;
}

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,dcp" },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
	.probe		= dcp_platform_probe,
	.remove		= dcp_platform_remove,
	.driver	= {
		.name = "apple-dcp",
		.of_match_table	= of_match,
	},
};

module_platform_driver(apple_platform_driver);

MODULE_AUTHOR("Alyssa Rosenzweig <alyssa@rosenzweig.io>");
MODULE_DESCRIPTION("Apple Display Controller DRM driver");
MODULE_LICENSE("GPL_v2");

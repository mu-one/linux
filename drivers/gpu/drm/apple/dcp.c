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
#include <linux/mailbox_client.h>

struct apple_dcp {
	struct mbox_client mbox;
	struct mbox_chan *chan;
};

static void dcp_mbox_msg(struct mbox_client *cl, void *msg)
{
//	struct apple_dcp *dcp = container_of(cl, struct apple_dcp, mbox);
	u64 rxmsg = (u64) (uintptr_t) msg;
	printk("DCP sent %llx\n", rxmsg);
}


static int dcp_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_dcp *dcp;
	int ret;

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	dev_info(&pdev->dev, "Probing DCP");

	dcp->mbox.dev = dev;
	dcp->mbox.rx_callback = dcp_mbox_msg;
	dcp->mbox.tx_block = true;
	dcp->mbox.tx_tout = 1000;
	printf("Requesting a channel\n");
	dcp->chan = mbox_request_channel(&dcp->mbox, 0);
	if(IS_ERR(dcp->chan)) {
		dev_err(&pdev->dev, "failed to attach to mailbox\n");
		return PTR_ERR(dcp->chan);
	}
	printk("Attached to mailbox");


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

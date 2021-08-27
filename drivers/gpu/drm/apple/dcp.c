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
#include <linux/apple-mailbox.h>
#include <linux/apple-rtkit.h>

#include "dcpep.h"

static void dcpep_send(struct apple_dcp *dcp, uint64_t msg)
{
	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT, msg);
}

static void dcpep_got_msg(struct apple_dcp *dcp, u64 message)
{
	bool ack;
	enum dcp_context_id ctx_id;
	u16 offset;
	u32 length;

	ack = message & BIT(DCPEP_ACK_SHIFT);
	ctx_id = (message & DCPEP_CONTEXT_MASK) >> DCPEP_CONTEXT_SHIFT;
	offset = (message & DCPEP_OFFSET_MASK) >> DCPEP_OFFSET_SHIFT;
	length = (message >> DCPEP_LENGTH_SHIFT);

	dev_info(dcp->dev, "got %s to context %u offset %u length %u\n",
		 ack ? "ack" : "message", ctx_id, offset, length);
}

static void dcp_got_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;
	enum dcpep_type type;

	WARN_ON(endpoint != DCP_ENDPOINT);

	type = (message >> DCPEP_TYPE_SHIFT) & DCPEP_TYPE_MASK;

	switch (type) {
	case DCPEP_TYPE_INITIALIZED:
		printk("DCP initialized! %llx\n", message);
		break;

	case DCPEP_TYPE_MESSAGE:
		printk("DCP message %llx\n", message);
		dcpep_got_msg(dcp, message);
		break;

	default:
		dev_warn(dcp->dev, "Ignoring unknown type %u in message %llx\n",
			 type, message);
		return;
	}
}

static int dummy_shmem_verify(void *cookie, dma_addr_t addr, size_t len)
{
        return 0;
}

static struct apple_rtkit_ops rtkit_ops =
{
        .shmem_owner = APPLE_RTKIT_SHMEM_OWNER_LINUX,
        .shmem_verify = dummy_shmem_verify,
        .recv_message = dcp_got_msg,
};

static int dcp_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct apple_dcp *dcp;
	int ret;

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;

	platform_set_drvdata(pdev, dcp);

	dcp->dev = dev;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

        res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc");
        if (!res)
                return -EINVAL;

        dcp->rtk = apple_rtkit_init(dev, dcp, res, "mbox", &rtkit_ops);
	apple_rtkit_boot_wait(dcp->rtk);
	apple_rtkit_start_ep(dcp->rtk, DCP_ENDPOINT);

	dcp->shmem = dma_alloc_coherent(dev, DCP_SHMEM_SIZE, &dcp->shmem_iova,
					GFP_KERNEL);
	dev_info(dev, "shmem allocated at dva %x\n", (u32) dcp->shmem_iova);

	dcpep_send(dcp, dcpep_set_shmem(dcp->shmem_iova));

	if (ret)
		return ret;

	return ret;
}

static int dcp_platform_remove(struct platform_device *pdev)
{
#if 0
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	apple_rtkit_free(dcp->rtk);
#endif
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

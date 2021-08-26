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

#define DCP_ENDPOINT 0x37
#define DCP_SHMEM_SIZE 0x100000

struct apple_dcp {
	struct apple_rtkit *rtk;

	/* DCP shared memory */
	void *shmem;
	dma_addr_t shmem_iova;
};

static void
dcpep_send(struct apple_dcp *dcp, uint64_t msg)
{
	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT, msg);
}

static void rtk_got_msg(void *cookie, u8 endpoint, u64 message)
{
	printk("DCP: ep %u got %llx\n", endpoint, message);
}

static int dummy_shmem_verify(void *cookie, dma_addr_t addr, size_t len)
{
        return 0;
}

static struct apple_rtkit_ops rtkit_ops =
{
        .shmem_owner = APPLE_RTKIT_SHMEM_OWNER_LINUX,
        .shmem_verify = dummy_shmem_verify,
        .recv_message = rtk_got_msg,
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

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	dev_info(dev, "Probing DCP");

        res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc");
        if (!res)
                return -EINVAL;
	dev_info(dev, "Got regs");

        dcp->rtk = apple_rtkit_init(dev, dcp, res, "mbox", &rtkit_ops);
	dev_info(dev, "Initialized\n");
	apple_rtkit_boot_wait(dcp->rtk);
	dev_info(dev, "Booted\n");
	apple_rtkit_start_ep(dcp->rtk, DCP_ENDPOINT);
	dev_info(dev, "Started\n");

	dcp->shmem = dma_alloc_coherent(dev, DCP_SHMEM_SIZE, &dcp->shmem_iova,
					GFP_KERNEL);
	dev_info(dev, "shmem allocated at dva %x\n", (u32) dcp->shmem_iova);

	/* DCP SetShmem */
	dcpep_send(dcp, (((u64) dcp->shmem_iova) << 16) | 0x40);

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

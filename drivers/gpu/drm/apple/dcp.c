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

struct apple_dcp {
	struct device *dev;
	struct apple_rtkit *rtk;

	/* DCP shared memory */
	void *shmem;
	dma_addr_t shmem_iova;

	/* Active contexts indexed by ID */
	struct dcp_context contexts[DCP_NUM_CONTEXTS];
};

static enum dcp_context_id dcp_context_get_id(struct dcp_context *ctx)
{
	struct apple_dcp *dcp = ctx->dcp;
	struct dcp_context *base = &dcp->contexts[0];

	return ctx - base;
}

static void dcpep_send(struct apple_dcp *dcp, uint64_t msg)
{
	dev_info(dcp->dev, "-> %llx\n", msg);
	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT, msg);
}

void * dcp_push_packet(struct dcp_context *ctx, char tag[4], u32 in_len,
		       u32 out_len, void *data, bool ack)
{
	struct dcp_packet_header header = {
		.in_len = in_len,
		.out_len = out_len,

		/* Tag is reversed due to endianness of the fourcc */
		.tag[0] = tag[3],
		.tag[1] = tag[2],
		.tag[2] = tag[1],
		.tag[3] = tag[0],
	};

	enum dcp_context_id id = dcp_context_get_id(ctx);
	u16 offset = ctx->offset;
	void *out = ctx->buf + offset;

	void *out_data = out + sizeof(header);
	size_t data_len = sizeof(header) + in_len + out_len;
	size_t copy_len = ack ? out_len : in_len;

	if (ack)
		out_data += in_len;

	memcpy(out, &header, sizeof(header));

	WARN_ON((data == NULL) != (copy_len == 0));

	if (data)
		memcpy(out_data, data, copy_len);

	dcpep_send(ctx->dcp, dcpep_msg(id, data_len, offset, ack));

	ctx->offset += ALIGN(data_len, DCP_PACKET_ALIGNMENT);
	return out + sizeof(header) + in_len;
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

static void dcp_start_signal(struct apple_dcp *dcp)
{
	struct dcp_context *ctx = &dcp->contexts[DCP_CONTEXT_CMD];
	uint32_t *resp;

	resp = dcp_push_packet(ctx, "A401", 0, sizeof(resp), NULL, false);
	printk("Started, resp %u\n", *resp);
}

static void dcp_got_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;
	enum dcpep_type type;

	WARN_ON(endpoint != DCP_ENDPOINT);

	dev_info(dcp->dev, "<- %llx\n", message);

	type = (message >> DCPEP_TYPE_SHIFT) & DCPEP_TYPE_MASK;

	switch (type) {
	case DCPEP_TYPE_INITIALIZED:
		dev_info(dcp->dev, "initialized!\n");
		dcp_start_signal(dcp);
		break;

	case DCPEP_TYPE_MESSAGE:
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
	int ret, i;

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

	for (i = 0; i < ARRAY_SIZE(dcp->contexts); ++i) {
		dcp->contexts[i] = (struct dcp_context) {
			.dcp = dcp,
			.buf = dcp->shmem,
			.offset = 0
		};
	}

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

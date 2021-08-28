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

struct apple_dcp;

/* Limit on call stack depth (arbitrary). Some nesting is required */
#define DCP_MAX_CALL_DEPTH 8

struct dcp_call_channel {
	void (*callbacks[DCP_MAX_CALL_DEPTH])(struct apple_dcp *, void *);
	void *output[DCP_MAX_CALL_DEPTH];
	u16 end[DCP_MAX_CALL_DEPTH];

	/* Current depth of the call stack. Less than DCP_MAX_CALL_DEPTH */
	u8 depth;
};

struct dcp_callback_channel {
	u8 depth;
	void *output[DCP_MAX_CALL_DEPTH];
};

struct apple_dcp {
	struct device *dev;
	struct apple_rtkit *rtk;

	/* DCP shared memory */
	void *shmem;

	struct dcp_call_channel ch_cmd;
	struct dcp_callback_channel ch_cb;
};

static void dcpep_send(struct apple_dcp *dcp, uint64_t msg)
{
	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT, msg);
}

void dcp_push(struct apple_dcp *dcp, enum dcp_context_id context,
	      char tag[4], u32 in_len, u32 out_len, void *data,
	      void (*cb)(struct apple_dcp *, void *))
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

	struct dcp_call_channel *ch = &dcp->ch_cmd;
	u8 depth = ch->depth++;
	u16 offset = (depth > 0) ? ch->end[depth - 1] : 0;
	void *out = dcp->shmem + offset; // TODO: OOB CMD

	void *out_data = out + sizeof(header);
	size_t data_len = sizeof(header) + in_len + out_len;

	memcpy(out, &header, sizeof(header));

	WARN_ON((data == NULL) != (in_len == 0));

	if (data)
		memcpy(out_data, data, in_len);

	WARN_ON(depth >= DCP_MAX_CALL_DEPTH);
	ch->callbacks[depth] = cb;
	ch->output[depth] = out + sizeof(header) + in_len;

	dcpep_send(dcp, dcpep_msg(context, data_len, offset, false));

	ch->end[depth] = offset + ALIGN(data_len, DCP_PACKET_ALIGNMENT);
}

/* Parse a fourcc callback tag "D123" into its number 123. Returns a negative
 * value on parsing failure. */

static int dcp_parse_tag(char tag[4])
{
	u32 d[3];
	int i;

	if (tag[3] != 'D')
		return -EINVAL;

	for (i = 0; i < 3; ++i) {
		d[i] = (u32) (tag[i] - '0');

		if (d[i] > 9)
			return -EINVAL;
	}

	return d[0] + (d[1] * 10) + (d[2] * 100);
}

void dcp_ack(struct apple_dcp *dcp, enum dcp_context_id context)
{
	struct dcp_callback_channel *ch;

	WARN_ON(context != DCP_CONTEXT_CB); // TODO
	ch = &dcp->ch_cb;

	WARN_ON(ch->depth == 0);
	ch->depth--;

	dcpep_send(dcp, dcpep_msg(context, 0, 0, true));
}

static bool HACK_should_hexdump(int tag)
{
	switch (tag) {
	case 121:
	case 122:
	case 123:
		/* setDCPAVProp* is noisy */
		return false;
	default:
		return false;
//		return true;
	}
}

/* A number of callbacks of the form `bool cb()` can be tied to a constant. */

static bool dcpep_cb_true(struct apple_dcp *dcp, void *out, void *in)
{
	u8 *resp = out;

	*resp = 1;
	return true;
}

static bool dcpep_cb_false(struct apple_dcp *dcp, void *out, void *in)
{
	u8 *resp = out;

	*resp = 0;
	return true;
}

#define LATE_INIT_SIGNAL "A000"
#define SETUP_VIDEO_LIMITS "A029"
#define SET_CREATE_DFB "A357"
#define START_SIGNAL "A401"
#define CREATE_DEFAULT_FB "A442"
#define SET_DISPLAY_REFRESH_PROPERTIES "A459"
#define FLUSH_SUPPORTS_POWER "A462"

/* Returns success */

static void boot_done(struct apple_dcp *dcp, void *out)
{
	struct dcp_callback_channel *ch = &dcp->ch_cb;
	u8 *succ = ch->output[ch->depth - 1];

	*succ = 1;
	dcp_ack(dcp, DCP_CONTEXT_CB);
}

static void boot_5(struct apple_dcp *dcp, void *out)
{
	dcp_push(dcp, DCP_CONTEXT_CB,
		 SET_DISPLAY_REFRESH_PROPERTIES, 0, sizeof(u8), NULL,
		 boot_done);
}

static void boot_4(struct apple_dcp *dcp, void *out)
{
	dcp_push(dcp, DCP_CONTEXT_CB,
		 LATE_INIT_SIGNAL, 0, sizeof(u8), NULL, boot_5);
}

static void boot_3(struct apple_dcp *dcp, void *out)
{
	u8 v_true = 1;

	dcp_push(dcp, DCP_CONTEXT_CB, FLUSH_SUPPORTS_POWER, sizeof(v_true), 0, &v_true, boot_4);
}

static void boot_2(struct apple_dcp *dcp, void *out)
{
	dcp_push(dcp, DCP_CONTEXT_CB, SETUP_VIDEO_LIMITS, 0, 0, NULL, boot_3);
}

static bool dcpep_cb_boot_1(struct apple_dcp *dcp, void *out, void *in)
{
	dcp_push(dcp, DCP_CONTEXT_CB, SET_CREATE_DFB, 0, 0, NULL, boot_2);
	return false;
}

#define DCPEP_MAX_CB (1000)

/* Represents a single callback. Name is for debug only. */

struct dcpep_cb {
	const char *name;
	bool (*cb)(struct apple_dcp *dcp, void *out, void *in);
};

struct dcpep_cb dcpep_cb_handlers[DCPEP_MAX_CB] = {
	[107] = {"create_provider_service", dcpep_cb_true },
	[108] = {"create_product_service", dcpep_cb_true },
	[109] = {"create_pmu_service", dcpep_cb_true },
	[110] = {"create_iomfb_service", dcpep_cb_true },
	[111] = {"create_backlight_service", dcpep_cb_false },
	[116] = {"start_hardware_boot", dcpep_cb_boot_1 },
	[206] = {"match_pmu_service_2", dcpep_cb_true },
	[207] = {"match_backlight_service", dcpep_cb_true },
};

static void dcpep_handle_cb(struct apple_dcp *dcp, enum dcp_context_id context, void *data, u32 length)
{
	struct device *dev = dcp->dev;
	struct dcpep_cb *cb;
	struct dcp_packet_header *hdr = data;
	void *in, *out;
	int tag = dcp_parse_tag(hdr->tag);
	bool ack = true;
	struct dcp_callback_channel *ch;
	u8 depth;

	WARN_ON(context != DCP_CONTEXT_CB); // TODO: unexpected
	ch = &dcp->ch_cb;

	if (tag < 0 || tag >= DCPEP_MAX_CB) {
		dev_warn(dev, "received invalid tag %c%c%c%c\n",
			 hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0]);
		goto ack;
	}

	cb = &dcpep_cb_handlers[tag];
	depth = ch->depth++;
	WARN_ON(ch->depth >= DCP_MAX_CALL_DEPTH);

	if (!cb->cb) {
		dev_warn(dev, "received unknown callback %c%c%c%c\n",
			 hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0]);
		goto ack;
	}

	in = data + sizeof(*hdr);
	out = in + hdr->in_len;

	dev_info(dev, "received callback %s\n", cb->name);

	ch->output[depth] = out;
	ack = cb->cb(dcp, out, in);

ack:
	if (ack)
		dcp_ack(dcp, DCP_CONTEXT_CB);

	if (HACK_should_hexdump(tag)) {
		print_hex_dump(KERN_INFO, "apple-dcp: ", DUMP_PREFIX_OFFSET,
				16, 1, data, length, true);
	}
}

static void dcpep_handle_ack(struct apple_dcp *dcp, enum dcp_context_id context,
			     void *data, u32 length)
{
	struct dcp_packet_header *header = data;
	struct dcp_call_channel *ch;

	switch (context) {
	case DCP_CONTEXT_CMD:
	case DCP_CONTEXT_CB:
		ch = &dcp->ch_cmd;
		break;
	default:
		dev_warn(dcp->dev, "ignoring ack on unknown context %X\n",
			 context);
		return;
	}

	WARN_ON(ch->depth == 0);
	ch->depth--;

	ch->callbacks[ch->depth](dcp, data + sizeof(*header) + header->in_len);
}

static void dcpep_got_msg(struct apple_dcp *dcp, u64 message)
{
	void *data;
	int channel_offset;

	bool ack;
	enum dcp_context_id ctx_id;
	u16 offset;
	u32 length;

	ack = message & BIT(DCPEP_ACK_SHIFT);
	ctx_id = (message & DCPEP_CONTEXT_MASK) >> DCPEP_CONTEXT_SHIFT;
	offset = (message & DCPEP_OFFSET_MASK) >> DCPEP_OFFSET_SHIFT;
	length = (message >> DCPEP_LENGTH_SHIFT);

	channel_offset = dcp_channel_offset(ctx_id);

	if (channel_offset < 0) {
		dev_warn(dcp->dev, "invalid context received %u", ctx_id);
		return;
	}

	data = dcp->shmem + channel_offset + offset;

	dev_info(dcp->dev, "got %s to context %u offset %u length %u\n",
		 ack ? "ack" : "message", ctx_id, offset, length);

	if (ack)
		dcpep_handle_ack(dcp, ctx_id, data, length);
	else
		dcpep_handle_cb(dcp, ctx_id, data, length);
}

static void dcp_started(struct apple_dcp *dcp, void *data)
{
	u32 *resp = data;

	dev_info(dcp->dev, "DCP started, status %u\n", *resp);
}

static void dcp_start_signal(struct apple_dcp *dcp)
{
	dcp_push(dcp, DCP_CONTEXT_CMD, START_SIGNAL, 0, sizeof(u32), NULL,
		 dcp_started);
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

static struct apple_rtkit_ops rtkit_ops = {
	.shmem_owner = APPLE_RTKIT_SHMEM_OWNER_LINUX,
	.shmem_verify = dummy_shmem_verify,
	.recv_message = dcp_got_msg,
};

static int dcp_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct apple_dcp *dcp;
	dma_addr_t shmem_iova;
	int ret;

	BUILD_BUG_ON(sizeof(struct dcp_rect) != 0x10);
	BUILD_BUG_ON(sizeof(struct dcp_iouserclient) != 0x10);
	BUILD_BUG_ON(sizeof(struct dcp_iomfbswaprec) != 0x274);
	BUILD_BUG_ON(sizeof(struct dcp_plane_info) != 0x50);
	BUILD_BUG_ON(sizeof(struct dcp_component_types) != 0x8);
	BUILD_BUG_ON(sizeof(struct dcp_iosurface) != 0x204);

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

	dcp->shmem = dma_alloc_coherent(dev, DCP_SHMEM_SIZE, &shmem_iova,
					GFP_KERNEL);
	dev_info(dev, "shmem allocated at dva %x\n", (u32) shmem_iova);

	dcpep_send(dcp, dcpep_set_shmem(shmem_iova));

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
MODULE_LICENSE("GPL v2");

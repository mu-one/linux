// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/align.h>
#include <linux/apple-mailbox.h>
#include <linux/apple-rtkit.h>

#include "dcpep.h"
#include "dcp.h"

#define DISP0_FRAMEBUFFER_0 0x54
#define DART_PAGE_SIZE (16384)

struct apple_dcp;

/* Limit on call stack depth (arbitrary). Some nesting is required */
#define DCP_MAX_CALL_DEPTH 8

typedef void (*dcp_callback_t)(struct apple_dcp *, void *, void *);

struct dcp_call_channel {
	dcp_callback_t callbacks[DCP_MAX_CALL_DEPTH];
	void *cookies[DCP_MAX_CALL_DEPTH];
	void *output[DCP_MAX_CALL_DEPTH];
	u16 end[DCP_MAX_CALL_DEPTH];

	/* Current depth of the call stack. Less than DCP_MAX_CALL_DEPTH */
	u8 depth;
};

struct dcp_cb_channel {
	u8 depth;
	void *output[DCP_MAX_CALL_DEPTH];
};

struct apple_dcp {
	struct device *dev;
	struct apple_rtkit *rtk;

	/* DCP shared memory */
	void *shmem;

	/* Number of memory mappings made by the DCP, used as an ID */
	u32 nr_mappings;

	struct dcp_call_channel ch_cmd, ch_oobcmd;
	struct dcp_cb_channel ch_cb, ch_oobcb, ch_async;

	bool active;
};

/* Get a call channel for a context */
struct dcp_call_channel *dcp_get_call_channel(struct apple_dcp *dcp,
					      enum dcp_context_id context)
{
	switch (context) {
	case DCP_CONTEXT_CMD:
	case DCP_CONTEXT_CB:
		return &dcp->ch_cmd;
	case DCP_CONTEXT_OOBCMD:
	case DCP_CONTEXT_OOBCB:
		return &dcp->ch_oobcmd;
	default:
		return NULL;
	}
}

/* Get a callback channel for a context */
struct dcp_cb_channel *dcp_get_cb_channel(struct apple_dcp *dcp,
					  enum dcp_context_id context)
{
	switch (context) {
	case DCP_CONTEXT_CB:
		return &dcp->ch_cb;
	case DCP_CONTEXT_OOBCB:
		return &dcp->ch_oobcb;
	case DCP_CONTEXT_ASYNC:
		return &dcp->ch_async;
	default:
		return NULL;
	}
}

/* Send a message to the DCP endpoint */
static void dcpep_send(struct apple_dcp *dcp, uint64_t msg)
{
	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT, msg);
}

/* Get the start of a packet: after the end of the previous packet */
static u16 dcp_packet_start(struct dcp_call_channel *ch, u8 depth)
{
	if (depth > 0)
		return ch->end[depth - 1];
	else
		return 0;
}

/* Pushes and pops the depth of the call stack with safety checks */
static u8 dcp_push_depth(u8 *depth)
{
	u8 ret = (*depth)++;

	WARN_ON(ret >= DCP_MAX_CALL_DEPTH);
	return ret;
}

static u8 dcp_pop_depth(u8 *depth)
{
	WARN_ON((*depth) == 0);

	return --(*depth);
}

/* Call a DCP function given by a tag */
void dcp_push(struct apple_dcp *dcp, enum dcp_context_id context,
	      char tag[4], u32 in_len, u32 out_len, void *data,
	      dcp_callback_t cb, void *cookie)
{
	struct dcp_call_channel *ch = dcp_get_call_channel(dcp, context);

	struct dcp_packet_header header = {
		.in_len = in_len,
		.out_len = out_len,

		/* Tag is reversed due to endianness of the fourcc */
		.tag[0] = tag[3],
		.tag[1] = tag[2],
		.tag[2] = tag[1],
		.tag[3] = tag[0],
	};

	u8 depth = dcp_push_depth(&ch->depth);
	u16 offset = dcp_packet_start(ch, depth);

	void *out = dcp->shmem + dcp_tx_offset(context) + offset;
	void *out_data = out + sizeof(header);
	size_t data_len = sizeof(header) + in_len + out_len;

	memcpy(out, &header, sizeof(header));

	if (in_len > 0)
		memcpy(out_data, data, in_len);

	ch->callbacks[depth] = cb;
	ch->cookies[depth] = cookie;
	ch->output[depth] = out + sizeof(header) + in_len;
	ch->end[depth] = offset + ALIGN(data_len, DCP_PACKET_ALIGNMENT);

	dcpep_send(dcp, dcpep_msg(context, data_len, offset, false));
}

/* Parse a callback tag "D123" into the ID 123. Returns -EINVAL on failure. */
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

/* Ack a callback from the DCP */
void dcp_ack(struct apple_dcp *dcp, enum dcp_context_id context)
{
	struct dcp_cb_channel *ch = dcp_get_cb_channel(dcp, context);

	dcp_pop_depth(&ch->depth);
	dcpep_send(dcp, dcpep_msg(context, 0, 0, true));
}

/* DCP callback handlers */
static bool dcpep_cb_nop(struct apple_dcp *dcp, void *out, void *in)
{
	return true;
}

static bool dcpep_cb_zero(struct apple_dcp *dcp, void *out, void *in)
{
	u32 *resp = out;

	*resp = 0;
	return true;
}

static bool dcpep_cb_get_uint_prop(struct apple_dcp *dcp, void *out, void *in)
{
	struct dcp_get_uint_prop_resp *resp = out;
	struct dcp_get_uint_prop_req *req = in;

	char obj[4 + 1] = { 0 };
	char key[0x40 + 1] = { 0 };

	memcpy(obj, req->obj, sizeof(req->obj));
	memcpy(key, req->key, sizeof(req->key));

	dev_info(dcp->dev, "ignoring property request %s:%s\n", obj, key);

	resp->value = 0;
	return true;
}

static bool dcpep_cb_map_physical(struct apple_dcp *dcp, void *out, void *in)
{
	struct dcp_map_physical_resp *resp = out;
	struct dcp_map_physical_req *req = in;

	resp->dva_size = ALIGN(req->size, DART_PAGE_SIZE);
	resp->dva = dma_map_resource(dcp->dev, req->paddr, resp->dva_size,
				     DMA_BIDIRECTIONAL, 0);
	resp->mem_desc_id = ++dcp->nr_mappings;

	WARN_ON(resp->mem_desc_id == 0);

	/* XXX: need to validate the DCP is allowed to access */
	dev_warn(dcp->dev,
		 "dangerously mapping phys addr %llx size %llx flags %x to dva %X\n",
		 req->paddr, req->size, req->flags, (u32) resp->dva);

	return true;
}

/* Pixel clock frequency in Hz. This is 533.333328 Mhz, factored as 33.333333
 * MHz * 16. Slightly greater than the 4K@60 VGA pixel clock 533.250 MHz. */
#define DCP_PIXEL_CLOCK (533333328)

static bool dcpep_cb_get_frequency(struct apple_dcp *dcp, void *out, void *in)
{
	u64 *frequency = out;

	*frequency = DCP_PIXEL_CLOCK;
	return true;
}

static bool dcpep_cb_map_reg(struct apple_dcp *dcp, void *out, void *in)
{
	struct dcp_map_reg_resp *resp = out;
	struct dcp_map_reg_req *req = in;

	/*
	 * XXX: values extracted from the Apple device tree
	 * TODO: don't hardcode, get this from Linux device tree
	 */
	struct dcp_map_reg_resp registers[] = {
		{ 0x230000000, 0x3e8000 },
		{ 0x231320000, 0x4000 },
		{ 0x231344000, 0x4000 },
		{ 0x231800000, 0x800000 },
		{ 0x23b3d0000, 0x4000 },
		{ 0x23b738000, 0x1000 },
		{ 0x23bc3c000, 0x1000 },
	};

	struct dcp_map_reg_resp error = {
		.ret = 1
	};

	if (req->index >= ARRAY_SIZE(registers)) {
		dev_warn(dcp->dev, "attempted to read invalid reg index %u",
			 req->index);

		*resp = error;
	} else {
		*resp = registers[req->index];
	}

	return true;
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

/* Returns success */

static void boot_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_cb_channel *ch = &dcp->ch_cb;
	u8 *succ = ch->output[ch->depth - 1];

	*succ = 1;
	dcp_ack(dcp, DCP_CONTEXT_CB);
}

static void boot_5(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, DCP_CONTEXT_CB, SET_DISPLAY_REFRESH_PROPERTIES, 0,
		 sizeof(u8), NULL, boot_done, NULL);
}

static void boot_4(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, DCP_CONTEXT_CB, LATE_INIT_SIGNAL, 0, sizeof(u8), NULL,
		 boot_5, NULL);
}

static void boot_3(struct apple_dcp *dcp, void *out, void *cookie)
{
	u8 v_true = 1;

	dcp_push(dcp, DCP_CONTEXT_CB, FLUSH_SUPPORTS_POWER, sizeof(v_true), 0,
		 &v_true, boot_4, NULL);
}

static void boot_2(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, DCP_CONTEXT_CB, SETUP_VIDEO_LIMITS, 0, 0, NULL, boot_3, NULL);
}

static bool dcpep_cb_boot_1(struct apple_dcp *dcp, void *out, void *in)
{
	dcp_push(dcp, DCP_CONTEXT_CB, SET_CREATE_DFB, 0, 0, NULL, boot_2, NULL);
	return false;
}

static bool dcpep_cb_rt_bandwidth_setup(struct apple_dcp *dcp, void *out, void *in)
{
	uint8_t blob[] = {
		0x6C, 0x43, 0x6C, 0x6F, 0x63, 0x6B, 0x00, 0x44, 0x14, 0x80,
		0x73, 0x3B, 0x02, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xC3, 0x3B,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x26, 0xFB, 0x43,
		0xFF, 0xFF, 0xFF, 0xFF, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x65, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	BUILD_BUG_ON(sizeof(blob) != 0x3C);

	memcpy(out, blob, sizeof(blob));
	return true;
}

#define DCPEP_MAX_CB (1000)

/* Represents a single callback. Name is for debug only. */

struct dcpep_cb {
	const char *name;
	bool (*cb)(struct apple_dcp *dcp, void *out, void *in);
};

struct dcpep_cb dcpep_cb_handlers[DCPEP_MAX_CB] = {
	[0] = {"did_boot_signal", dcpep_cb_true },
	[1] = {"did_power_on_signal", dcpep_cb_true },
	[2] = {"will_power_off_signal", dcpep_cb_true },
	[3] = {"rt_bandwidth_setup_ap", dcpep_cb_rt_bandwidth_setup },

	[100] = {"match_pmu_service", dcpep_cb_nop },
	[101] = {"get_display_default_stride", dcpep_cb_zero },
	[103] = {"set_boolean_property", dcpep_cb_nop },
	[106] = {"remove_property", dcpep_cb_nop },
	[107] = {"create_provider_service", dcpep_cb_true },
	[108] = {"create_product_service", dcpep_cb_true },
	[109] = {"create_pmu_service", dcpep_cb_true },
	[110] = {"create_iomfb_service", dcpep_cb_true },
	[111] = {"create_backlight_service", dcpep_cb_false },
	[121] = {"set_dcpav_prop_start", dcpep_cb_true },
	[122] = {"set_dcpav_prop_chunk", dcpep_cb_true },
	[123] = {"set_dcpav_prop_end", dcpep_cb_true },
	[116] = {"start_hardware_boot", dcpep_cb_boot_1 },

	[206] = {"match_pmu_service_2", dcpep_cb_true },
	[207] = {"match_backlight_service", dcpep_cb_true },

	[300] = {"pr_publish", dcpep_cb_nop },

	[401] = {"sr_get_uint_prop", dcpep_cb_get_uint_prop },
	[408] = {"sr_get_clock_frequency", dcpep_cb_get_frequency },
	[411] = {"sr_map_device_memory_with_index", dcpep_cb_map_reg },
	[413] = {"sr_set_property_dict", dcpep_cb_true },
	[414] = {"sr_set_property_int", dcpep_cb_true },
	[415] = {"sr_set_property_bool", dcpep_cb_true },

	[452] = {"map_physical", dcpep_cb_map_physical },

	[552] = {"set_property_dict_0", dcpep_cb_true },
	[561] = {"set_property_dict", dcpep_cb_true },
	[563] = {"set_property_int", dcpep_cb_true },
	[565] = {"set_property_bool", dcpep_cb_true },
	[567] = {"set_property_str", dcpep_cb_true },
	[574] = {"power_up_dart", dcpep_cb_zero },
	[576] = {"hotplug_notify_gated", dcpep_cb_nop },
	[577] = {"powerstate_notify", dcpep_cb_nop },
	[589] = {"swap_complete_ap_gated", dcpep_cb_nop },
	[591] = {"swap_complete_intent_gated", dcpep_cb_nop },
	[598] = {"find_swap_function_gated", dcpep_cb_nop },
};

static void dcpep_handle_cb(struct apple_dcp *dcp, enum dcp_context_id context,
			    void *data, u32 length)
{
	struct device *dev = dcp->dev;
	struct dcpep_cb *cb;
	struct dcp_packet_header *hdr = data;
	void *in, *out;
	int tag = dcp_parse_tag(hdr->tag);
	bool ack = true;
	struct dcp_cb_channel *ch = dcp_get_cb_channel(dcp, context);
	u8 depth;

	if (tag < 0 || tag >= DCPEP_MAX_CB) {
		dev_warn(dev, "received invalid tag %c%c%c%c\n",
			 hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0]);
		goto ack;
	}

	cb = &dcpep_cb_handlers[tag];
	depth = dcp_push_depth(&ch->depth);

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
		dcp_ack(dcp, context);
}

static void dcpep_handle_ack(struct apple_dcp *dcp, enum dcp_context_id context,
			     void *data, u32 length)
{
	struct dcp_packet_header *header = data;
	struct dcp_call_channel *ch = dcp_get_call_channel(dcp, context);
	void *cookie;
	dcp_callback_t cb;

	if (!ch) {
		dev_warn(dcp->dev, "ignoring ack on unknown context %X\n",
			 context);
		return;
	}

	dcp_pop_depth(&ch->depth);

	cb = ch->callbacks[ch->depth];
	cookie = ch->cookies[ch->depth];

	if (cb)
		cb(dcp, data + sizeof(*header) + header->in_len, cookie);
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

	if (ack)
		dcpep_handle_ack(dcp, ctx_id, data, length);
	else
		dcpep_handle_cb(dcp, ctx_id, data, length);
}

static void dcp_swap_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct dcp_swap_start_resp *resp = data;
	dma_addr_t surf_iova = (uintptr_t) cookie;
	u32 surf_id = 3; // XXX

	struct dcp_iomfbswaprec swap_rec = {
		.swap_id = resp->swap_id,
		.surf_ids[0] = surf_id,
		.src_rect[0] = {
			0, 0, 1920, 1080
		},
		.surf_flags[0] = 1,
		.dst_rect[0] = {
			0, 0, 1920, 1080
		},
		.swap_enabled = 1,
		.swap_completed = 1,
	};

	struct dcp_iosurface surf = {
		.format[0] = 'A',
		.format[1] = 'B',
		.format[2] = 'G',
		.format[3] = 'R',
		.unk_13 = 13,
		.unk_14 = 1,
		.stride = 1920 * 4,
		.pix_size = 1,
		.pel_w = 1,
		.pel_h = 1,
		.width = 1920,
		.height = 1080,
		.buf_size = 1920 * 1080 * 4,
		.surface_id = surf_id,
		.has_comp = 1,
		.has_planes = 1,
	};

	struct dcp_swap_submit_req *req = kmalloc(sizeof(*req), GFP_KERNEL);

	*req = (struct dcp_swap_submit_req) {
		.swap_rec = swap_rec,
		.surf[0] = surf,
		.surf_iova[0] = surf_iova,
	};

	dcp_push(dcp, DCP_CONTEXT_CMD, SWAP_SUBMIT,
		 sizeof(struct dcp_swap_submit_req),
		 sizeof(struct dcp_swap_submit_resp),
		 req, NULL, NULL);

	kfree(req);
}

void dcp_swap(struct platform_device *pdev, dma_addr_t dva)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct dcp_swap_start_req req = { 0 };

	WARN_ON(!dcp->active);

	printk("Swapping now! Well, not actually. DVA %X\n", (u32) dva);

	dcp_push(dcp, DCP_CONTEXT_CMD, SWAP_START,
		 sizeof(struct dcp_swap_start_req),
		 sizeof(struct dcp_swap_start_resp),
		 &req, dcp_swap_started, (void *) (uintptr_t) dva);
}
EXPORT_SYMBOL_GPL(dcp_swap);

bool dcp_is_initialized(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->active;
}
EXPORT_SYMBOL_GPL(dcp_is_initialized);

static void dcp_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	u32 *resp = data;

	dev_info(dcp->dev, "DCP started, status %u\n", *resp);
	dcp->active = true;
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
		dcp_push(dcp, DCP_CONTEXT_CMD, START_SIGNAL, 0, sizeof(u32),
			 NULL, dcp_started, NULL);
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
	BUILD_BUG_ON(sizeof(struct dcp_swap_start_req) != 0x18);
	BUILD_BUG_ON(sizeof(struct dcp_swap_start_resp) != 0x18);
	BUILD_BUG_ON(sizeof(struct dcp_swap_submit_req) != 0x8a0);
	BUILD_BUG_ON(sizeof(struct dcp_swap_submit_resp) != 0x8);
	BUILD_BUG_ON(sizeof(struct dcp_map_reg_req) != 0x10);
	BUILD_BUG_ON(sizeof(struct dcp_map_reg_resp) != 0x14);
	BUILD_BUG_ON(sizeof(struct dcp_get_uint_prop_req) != 0x50);
	BUILD_BUG_ON(sizeof(struct dcp_get_uint_prop_resp) != 0xc);
	BUILD_BUG_ON(sizeof(struct dcp_map_physical_req) != 0x18);
	BUILD_BUG_ON(sizeof(struct dcp_map_physical_resp) != 0x14);

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

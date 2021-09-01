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

#include <drm/drm_fb_cma_helper.h>

#include "dcpep.h"
#include "dcp.h"

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

#define DCP_MAX_MAPPINGS (128) /* should be enough */

struct dcp_mapping {
	struct sg_table sg;
	dma_addr_t iova;
	size_t size;
};

struct apple_dcp {
	struct device *dev;
	struct device *piodma;
	struct apple_rtkit *rtk;
	struct apple_drm_private *apple;

	/* DCP shared memory */
	void *shmem;

	/* Number of memory mappings made by the DCP, used as an ID */
	u32 nr_mappings;

	/* Indexed table of mappings */
	struct dcp_mapping mappings[DCP_MAX_MAPPINGS];

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

#define DCP_METHOD(name, tag) \
	[dcp_ ## name ] = { #name, "A" # tag }

struct dcp_method_entry dcp_methods[dcp_num_methods] = {
	DCP_METHOD(late_init_signal, 000),
	DCP_METHOD(setup_video_limits, 029),
	DCP_METHOD(set_create_dfb, 357),
	DCP_METHOD(start_signal, 401),
	DCP_METHOD(swap_start, 407),
	DCP_METHOD(swap_submit, 408),
	DCP_METHOD(set_display_device, 410),
	DCP_METHOD(set_digital_out_mode, 412),
	DCP_METHOD(create_default_fb, 442),
	DCP_METHOD(set_display_refresh_properties, 459),
	DCP_METHOD(flush_supports_power, 462),
};

/* Call a DCP function given by a tag */
void dcp_push(struct apple_dcp *dcp, enum dcp_context_id context,
	      enum dcp_method method, u32 in_len, u32 out_len, void *data,
	      dcp_callback_t cb, void *cookie)
{
	struct dcp_call_channel *ch = dcp_get_call_channel(dcp, context);

	struct dcp_packet_header header = {
		.in_len = in_len,
		.out_len = out_len,

		/* Tag is reversed due to endianness of the fourcc */
		.tag[0] = dcp_methods[method].tag[3],
		.tag[1] = dcp_methods[method].tag[2],
		.tag[2] = dcp_methods[method].tag[1],
		.tag[3] = dcp_methods[method].tag[0],
	};

	u8 depth = dcp_push_depth(&ch->depth);
	u16 offset = dcp_packet_start(ch, depth);

	void *out = dcp->shmem + dcp_tx_offset(context) + offset;
	void *out_data = out + sizeof(header);
	size_t data_len = sizeof(header) + in_len + out_len;

	memcpy(out, &header, sizeof(header));

	if (in_len > 0)
		memcpy(out_data, data, in_len);

	dev_info(dcp->dev, "---> %s: context %u, offset %u, depth %u\n",
		 dcp_methods[method].name, context, offset, depth);
#if 0
       print_hex_dump(KERN_INFO, "dcp: ",
                       DUMP_PREFIX_OFFSET, 16, 1,
                       out, data_len, true);
#endif

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

static bool dcpep_cb_swap_complete(struct apple_dcp *dcp, void *out, void *in)
{
	apple_crtc_vblank(dcp->apple);
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

/*
 * Callback to map a buffer allocated with allocate_buf for PIODMA usage.
 * PIODMA is separate from the main DCP and uses own IOVA space on a dedicated
 * stream of the display DART, rather than the expected DCP DART.
 *
 * XXX: This relies on dma_get_sgtable in concert with dma_map_sgtable, which
 * is a "fundamentally unsafe" operation according to the docs. And yet
 * everyone does it...
 */
static bool dcpep_cb_map_piodma(struct apple_dcp *dcp, void *out, void *in)
{
	struct dcp_map_buf_resp *resp = out;
	struct dcp_map_buf_req *req = in;
	struct dcp_mapping *map;

	if (req->buffer >= ARRAY_SIZE(dcp->mappings))
		goto reject;

	map = &dcp->mappings[req->buffer];

	if (!map->sg.sgl)
		goto reject;

	/*
	 * XNU leaks a kernel VA here. Since it's ignored by the DCP and breaks
	 * kASLR, zero the field. The existence of this field may be an XNU bug.
	 */
	resp->vaddr = 0;

	/* Use PIODMA device instead of DCP to map against the right IOMMU. */
	resp->ret = dma_map_sgtable(dcp->piodma, &map->sg, DMA_BIDIRECTIONAL, 0);

	if (resp->ret)
		dev_warn(dcp->dev, "failed to map for piodma %d\n", resp->ret);
	else
		resp->dva = sg_dma_address(map->sg.sgl);

	resp->ret = 0;
	return true;

reject:
	dev_warn(dcp->dev, "denying map of invalid buffer %llx for pidoma\n",
		 req->buffer);
	resp->ret = EINVAL;
	return true;
}

/*
 * Allocate an IOVA contiguous buffer mapped to the DCP. The buffer need not be
 * physically contigiuous, however we should save the sgtable in case the
 * buffer needs to be later mapped for PIODMA.
 */
static bool dcpep_cb_allocate_buffer(struct apple_dcp *dcp, void *out, void *in)
{
	struct dcp_allocate_buffer_resp *resp = out;
	struct dcp_allocate_buffer_req *req = in;
	void *buf;

	resp->dva_size = ALIGN(req->size, 4096);
	resp->mem_desc_id = ++dcp->nr_mappings;

	if (resp->mem_desc_id >= ARRAY_SIZE(dcp->mappings)) {
		dev_warn(dcp->dev, "DCP overflowed mapping table, ignoring");
		return true;
	}

	buf = dma_alloc_coherent(dcp->dev, resp->dva_size, &resp->dva,
				 GFP_KERNEL);

	dcp->mappings[resp->mem_desc_id] = (struct dcp_mapping) {
		.iova = resp->dva,
		.size = resp->dva_size
	};

	dma_get_sgtable(dcp->dev, &dcp->mappings[resp->mem_desc_id].sg, buf,
			resp->dva, resp->dva_size);

	dev_info(dcp->dev, "allocated %llx bytes to (%u, %llx)\n",
		 resp->dva_size, resp->mem_desc_id, resp->dva);

	WARN_ON(resp->mem_desc_id == 0);
	return true;
}

/*
 * Map an arbitrary chunk of physical memory into the DCP's address space. As
 * stated that's a massive security hole. In practice, benevolent DCP firmware
 * only uses this to map the display registers we advertise in
 * sr_map_device_memory_with_index. As long as we bounds check against this
 * register memory, the routine is safe against malicious coprocessors.
 *
 * XXX: actually bounds check!
 */
static bool dcpep_cb_map_physical(struct apple_dcp *dcp, void *out, void *in)
{
	struct dcp_map_physical_resp *resp = out;
	struct dcp_map_physical_req *req = in;

	printk("map_physical(%llx, %llx, %X, %u, %u)\n",
	       req->paddr, req->size, req->flags, req->dva_null, req->dva_size_null);

	resp->dva_size = ALIGN(req->size, 4096);
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

/*
 * Pixel clock frequency in Hz. This is 533.333328 Mhz, factored as 33.333333
 * MHz * 16. Slightly greater than the 4K@60 VGA pixel clock 533.250 MHz.
 */
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

	char obj[5] = { 0 };
	memcpy(&obj, req->obj, 4);

	printk("map_reg(%s, %u, %X, %u, %u)\n",
	       obj, req->index, req->flags, req->addr_null, req->length_null);

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

	printk("= (%llx, %llx, %x)\n",
		resp->addr, resp->length, resp->ret);

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
	dcp_push(dcp, DCP_CONTEXT_CB, dcp_set_display_refresh_properties, 0,
		 4, NULL, boot_done, NULL);
}

static void boot_4(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, DCP_CONTEXT_CB, dcp_late_init_signal, 0, 4, NULL,
		 boot_5, NULL);
}

static void boot_3(struct apple_dcp *dcp, void *out, void *cookie)
{
	u8 v_true = 1;

	dcp_push(dcp, DCP_CONTEXT_CB, dcp_flush_supports_power, sizeof(v_true), 0,
		 &v_true, boot_4, NULL);
}

static void boot_2(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, DCP_CONTEXT_CB, dcp_setup_video_limits, 0, 0, NULL, boot_3, NULL);
}

static void boot_1_5(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, DCP_CONTEXT_CB, dcp_create_default_fb, 0, sizeof(u32), NULL, boot_2, NULL);
}

static bool dcpep_cb_boot_1(struct apple_dcp *dcp, void *out, void *in)
{
	dcp_push(dcp, DCP_CONTEXT_CB, dcp_set_create_dfb, 0, 0, NULL, boot_1_5, NULL);
	return false;
}

static bool dcpep_cb_rt_bandwidth_setup(struct apple_dcp *dcp, void *out, void *in)
{
#if 0
	struct dcp_rt_bandwidth *data = out;

	*data = (struct dcp_rt_bandwidth) {
            .reg1 = 0x23b738014, // reg[5] in disp0/dispext0, plus 0x14 - part of pmgr
            .reg2 = 0x23bc3c000, // reg[6] in disp0/dispext0 - part of pmp/pmgr
            .bit = 2,
	};
#endif

	/* XXX */
uint8_t data[] = {
        0x6C, 0x43, 0x6C, 0x6F, 0x63, 0x6B, 0x00, 0x44, 0x14, 0x80,
        0x73, 0x3B, 0x02, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xC3, 0x3B,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x26, 0xFB, 0x43,
        0xFF, 0xFF, 0xFF, 0xFF, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x65, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

memcpy(out, data, sizeof(data));


	BUILD_BUG_ON(sizeof(data) != 0x3C);
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
	[2] = {"will_power_off_signal", dcpep_cb_nop },
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
	[119] = {"read_edt_data", dcpep_cb_false },

	[201] = {"map_piodma", dcpep_cb_map_piodma },
	[206] = {"match_pmu_service_2", dcpep_cb_true },
	[207] = {"match_backlight_service", dcpep_cb_true },

	[300] = {"pr_publish", dcpep_cb_nop },

	[401] = {"sr_get_uint_prop", dcpep_cb_get_uint_prop },
	[408] = {"sr_get_clock_frequency", dcpep_cb_get_frequency },
	[411] = {"sr_map_device_memory_with_index", dcpep_cb_map_reg },
	[413] = {"sr_set_property_dict", dcpep_cb_true },
	[414] = {"sr_set_property_int", dcpep_cb_true },
	[415] = {"sr_set_property_bool", dcpep_cb_true },

	[451] = {"allocate_buffer", dcpep_cb_allocate_buffer },
	[452] = {"map_physical", dcpep_cb_map_physical },

	[552] = {"set_property_dict_0", dcpep_cb_true },
	[561] = {"set_property_dict", dcpep_cb_true },
	[563] = {"set_property_int", dcpep_cb_true },
	[565] = {"set_property_bool", dcpep_cb_true },
	[567] = {"set_property_str", dcpep_cb_true },
	[574] = {"power_up_dart", dcpep_cb_zero },
	[576] = {"hotplug_notify_gated", dcpep_cb_nop },
	[577] = {"powerstate_notify", dcpep_cb_nop },
	[589] = {"swap_complete_ap_gated", dcpep_cb_swap_complete },
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

	dev_info(dev, "channel %u: received callback %s\n", context, cb->name);

#if 0
       print_hex_dump(KERN_INFO, "dcp: ",
                       DUMP_PREFIX_OFFSET, 16, 1,
                       data, length, true);
#endif

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

/*
 * Callback for swap requests. If a swap failed, we'll never get a swap
 * complete event so we need to fake a vblank event early to avoid a hang.
 */

static void dcp_swapped(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct dcp_swap_submit_resp *resp = data;

	if (resp->ret) {
		dev_err(dcp->dev, "swap failed! status %u\n", resp->ret);
		apple_crtc_vblank(dcp->apple);
	}

}


static void dump_rect(struct dcp_rect r)
{
	printk("\t\t\t(%u, %u) -> (%u, %u)\n", r.x, r.y, r.x + r.w, r.y + r.h);
}

static void dump_swap_rec(struct dcp_iomfbswaprec *r)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(r->unk0); ++i)
		WARN_ON(r->unk0[i] != 0);

	for (i = 0; i < ARRAY_SIZE(r->unk1); ++i)
		WARN_ON(r->unk1[i] != 0);

	WARN_ON(r->flags1);
	WARN_ON(r->flags2);

	printk("\tSwap ID: %u\n", r->swap_id);
	printk("\tSwap enabled: %u\n", r->swap_enabled);
	printk("\tSwap completed: %u\n", r->swap_completed);

	for (i = 0; i < SWAP_SURFACES; ++i) {
		printk("\tSurface %u:\n", r->surf_ids[i]);
		printk("\t\tFlags %u:\n", r->surf_flags[i]);
		printk("\t\tSource rect:\n");
		dump_rect(r->src_rect[i]);
		printk("\t\tDestination rect:\n");
		dump_rect(r->dst_rect[i]);

		WARN_ON(r->surf_unk[i]);
	}
}

static void dump_iosurface(struct dcp_iosurface *r)
{
	printk("\tTiled: %u\n", r->is_tiled);
	printk("\tPlane count 1: %u\n", r->plane_cnt);
	printk("\tPlane count 2: %u\n", r->plane_cnt2);
	printk("\tFormat: %c%c%c%c\n",
	       r->format[3], r->format[2], r->format[1], r->format[0]);

	printk("\tStride: %u\n", r->stride);
	printk("\tPixel size: %u\n", r->pix_size);
	printk("\tPixel element width: %u\n", r->pel_w);
	printk("\tPixel element height: %u\n", r->pel_h);
	printk("\tOffset: %u\n", r->offset);
	printk("\tWidth: %u\n", r->width);
	printk("\tHeight: %u\n", r->height);
	printk("\tBuffer size: %u\n", r->buf_size);
	printk("\tUnk 1: %u\n", r->unk_1);
	printk("\tUnk 2: %u\n", r->unk_2);
	printk("\tUnk F: %u\n", r->unk_f);
	printk("\tUnk 13: %u\n", r->unk_13);
	printk("\tUnk 14: %u\n", r->unk_14);
	printk("\tUnk 14: %u\n", r->unk_14);

	/* TODO: finish */
}

static void dump_swap_submit_req(struct dcp_swap_submit_req *r)
{
	int i;

	dump_swap_rec(&r->swap_rec);

	for (i = 0; i < SWAP_SURFACES; ++i) {
		printk("Surface %d:\n", i);
		dump_iosurface(&r->surf[i]);
		printk("\tIOVA: 0x%X\n", r->surf_iova[i]);
		printk("\tNull:%u\n", r->surf_null[i]);
	}

	printk("Unboolk %u\n", r->unkbool);
	printk("Unkdouble %llu\n", r->unkdouble);
	printk("Unkint %u\n", r->unkint);
	printk("Swap rec null %u\n", r->swap_rec_null);
	printk("Unkout_bool null %u\n", r->unkoutbool_null);
}

static void dcp_swap_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct dcp_swap_start_resp *resp = data;
	struct dcp_swap_submit_req *req = cookie;

	req->swap_rec.swap_id = resp->swap_id;
	dump_swap_submit_req(req);
	dcp_push(dcp, DCP_CONTEXT_CMD, dcp_swap_submit,
		 sizeof(struct dcp_swap_submit_req),
		 sizeof(struct dcp_swap_submit_resp),
		 req, dcp_swapped, NULL);

	kfree(req);
}

/*
 * DRM specifies rectangles as a product of semi-open intervals [x1, x2) x [y1,
 * y2). DCP specifies rectangles as a start coordinate and a width/height
 * <x1, y1> + <w, h>. Convert between these forms.
 */
struct dcp_rect drm_to_dcp_rect(struct drm_rect *rect)
{
	return (struct dcp_rect) {
		.x = rect->x1,
		.y = rect->y1,
		.w = drm_rect_width(rect),
		.h = drm_rect_height(rect)
	};
}

void dcp_swap(struct platform_device *pdev, struct drm_atomic_state *state)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	struct dcp_swap_submit_req *req = kzalloc(sizeof(*req), GFP_KERNEL);

	int i;
	int nr_layers = 0;

	for_each_new_plane_in_state(state, plane, plane_state, i) {
		struct drm_framebuffer *fb = plane_state->fb;
		struct drm_rect src_rect;
		int l = nr_layers;

		if (!fb)
			continue;

		req->surf_iova[l] = drm_fb_cma_get_gem_addr(fb, plane_state, 0);

		nr_layers++;

		drm_rect_fp_to_int(&src_rect, &plane_state->src);

		req->swap_rec.src_rect[l] = drm_to_dcp_rect(&src_rect);
		req->swap_rec.dst_rect[l] = drm_to_dcp_rect(&plane_state->dst);

		req->swap_rec.surf_flags[l] = 1;
		req->swap_rec.surf_ids[l] = 3 + i; // XXX

		req->swap_rec.swap_enabled |= BIT(l);
		req->swap_rec.swap_completed |= BIT(l);

		req->surf[l] = (struct dcp_iosurface) {
			.format[0] = 'A',
			.format[1] = 'R',
			.format[2] = 'G',
			.format[3] = 'B',
			.unk_13 = 13,
			.unk_14 = 1,
			.stride = fb->pitches[0],
			.pix_size = 1,
			.pel_w = 1,
			.pel_h = 1,
			.width = fb->width,
			.height = fb->height,
			.buf_size = fb->height * fb->pitches[0],
			.surface_id = req->swap_rec.surf_ids[l],
			.has_comp = 1,
			.has_planes = 1,
		};
	}

	for (; nr_layers < SWAP_SURFACES; ++nr_layers)
		req->surf_null[nr_layers] = true;

	/*
 	 * Bitmap of layers to update. Bit 31 indicates that a layer (may) be
 	 * deleted, which is required to unmap without faults.
 	 *
 	 * TODO: dirty track all the things! macOS only sets the bits
 	 * corresponding to the layers that actually changed. This might be
 	 * more efficient.
 	 */
	req->swap_rec.swap_enabled = BIT(31) | BIT(0) | BIT(1);
	req->swap_rec.swap_completed |= BIT(31) | BIT(0) | BIT(1);

	WARN_ON(!dcp->active);

	dcp_push(dcp, DCP_CONTEXT_CMD, dcp_swap_start,
		 sizeof(struct dcp_swap_start_req),
		 sizeof(struct dcp_swap_start_resp),
		 &req, dcp_swap_started, req);
}
EXPORT_SYMBOL_GPL(dcp_swap);

bool dcp_is_initialized(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->active;
}
EXPORT_SYMBOL_GPL(dcp_is_initialized);

static void modeset_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	u32 *ret = out;

	dev_info(dcp->dev, "mode set returned %u\n", *ret);
	dcp->active = true;
}

static void dcp_set_4k(struct apple_dcp *dcp, void *out, void *cookie)
{
	u32 *resp = out;

	struct dcp_set_digital_out_mode_req req = {
		.mode0 = 0x69,
		.mode1 = 0x45
	};

	printk("Setting 4k (status %u)\n", *resp);

	dcp_push(dcp, DCP_CONTEXT_CMD, dcp_set_digital_out_mode, sizeof(req),
		 sizeof(u32), &req, modeset_done, NULL);
}

static void dcp_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	u32 *resp = data;
	u32 handle = 2;

	dev_info(dcp->dev, "DCP started, status %u\n", *resp);
#if 0
	dcp->active = true;
#else
	dcp_push(dcp, DCP_CONTEXT_CMD, dcp_set_display_device, sizeof(handle),
		 sizeof(u32), &handle, dcp_set_4k, NULL);
#endif
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
		dcp_push(dcp, DCP_CONTEXT_CMD, dcp_start_signal, 0, sizeof(u32),
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

void dcp_link(struct platform_device *pdev, struct apple_drm_private *apple)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	dcp->apple = apple;
}

struct device *dcp_get_piodma(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *node;

	of_platform_default_populate(dev->of_node, NULL, dev);

	node = of_get_child_by_name(dev->of_node, "piodma");

	if (!node)
		return NULL;

	pdev = of_find_device_by_node(node);

	if (!pdev)
		return NULL;

	return &pdev->dev;
}

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
	BUILD_BUG_ON(sizeof(struct dcp_map_buf_req) != 0xc);
	BUILD_BUG_ON(sizeof(struct dcp_map_buf_resp) != 0x14);
	BUILD_BUG_ON(sizeof(struct dcp_allocate_buffer_req) != 0x14);
	BUILD_BUG_ON(sizeof(struct dcp_allocate_buffer_resp) != 0x1c);
	BUILD_BUG_ON(sizeof(struct dcp_set_digital_out_mode_req) != 0x8);

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;

	platform_set_drvdata(pdev, dcp);

	dcp->dev = dev;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc");
	if (!res)
		return -EINVAL;

	dcp->piodma = dcp_get_piodma(dev);
	if (!dcp->piodma) {
		dev_err(dev, "failed to find piodma\n");
		return -ENODEV;
	}

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

// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/dma-mapping.h>
#include <linux/align.h>
#include <linux/apple-mailbox.h>
#include <linux/apple-rtkit.h>

#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_probe_helper.h>

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

struct apple_dcp {
	struct device *dev;
	struct device *piodma;
	struct apple_rtkit *rtk;
	struct apple_crtc *crtc;
	struct apple_connector *connector;

	/* DCP shared memory */
	void *shmem;

	/* Number of memory mappings made by the DCP, used as an ID */
	u32 nr_mappings;

	/* Indexed table of mappings */
	struct sg_table mappings[DCP_MAX_MAPPINGS];

	struct dcp_call_channel ch_cmd, ch_oobcmd;
	struct dcp_cb_channel ch_cb, ch_oobcb, ch_async;

	bool active;
};

/*
 * A channel is busy if we have sent a message that has yet to be
 * acked. The driver must not sent a message to a busy channel.
 */
static bool dcp_channel_busy(struct dcp_call_channel *ch)
{
	return (ch->depth != 0);
}

/*
 * XXX: values extracted from the Apple device tree
 * TODO: don't hardcode, get this from Linux device tree
 */
struct dcp_map_reg_resp disp0_registers[] = {
	{ 0x230000000, 0x3e8000 },
	{ 0x231320000, 0x4000 },
	{ 0x231344000, 0x4000 },
	{ 0x231800000, 0x800000 },
	{ 0x23b3d0000, 0x4000 },
	{ 0x23b738000, 0x1000 },
	{ 0x23bc3c000, 0x1000 },
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

/*
 * Get the context ID passed to the DCP for a command we push. The rule is
 * simple: callback contexts are used when replying to the DCP, command
 * contexts are used otherwise. That corresponds to a non/zero call stack
 * depth. This rule frees the caller from tracking the call context manually.
 */
static enum dcp_context_id dcp_call_context(struct apple_dcp *dcp, bool oob)
{
	u8 depth = oob ? dcp->ch_oobcmd.depth : dcp->ch_cmd.depth;

	if (depth)
		return oob ? DCP_CONTEXT_OOBCB : DCP_CONTEXT_CB;
	else
		return oob ? DCP_CONTEXT_OOBCMD : DCP_CONTEXT_CMD;
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
	DCP_METHOD(set_power_state, 467),
};

/* Call a DCP function given by a tag */
void dcp_push(struct apple_dcp *dcp, bool oob, enum dcp_method method,
	      u32 in_len, u32 out_len, void *data, dcp_callback_t cb,
	      void *cookie)
{
	struct dcp_call_channel *ch = oob ? &dcp->ch_oobcmd : &dcp->ch_cmd;
	enum dcp_context_id context = dcp_call_context(dcp, oob);

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

	dev_dbg(dcp->dev, "---> %s: context %u, offset %u, depth %u\n",
		 dcp_methods[method].name, context, offset, depth);

	ch->callbacks[depth] = cb;
	ch->cookies[depth] = cookie;
	ch->output[depth] = out + sizeof(header) + in_len;
	ch->end[depth] = offset + ALIGN(data_len, DCP_PACKET_ALIGNMENT);

	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT,
				 dcpep_msg(context, data_len, offset));
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
	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT, dcpep_ack(context));
}

static void dcp_set_4k(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_callback_t cb = cookie;

	struct dcp_set_digital_out_mode_req req = {
		.mode0 = 0x5a,
		.mode1 = 0x48
	};

	dcp_push(dcp, false, dcp_set_digital_out_mode, sizeof(req),
		 sizeof(u32), &req, cb, NULL);
}

static void dcp_modeset(struct apple_dcp *dcp, dcp_callback_t cb)
{
	u32 handle = 2;

	dcp_push(dcp, false, dcp_set_display_device, sizeof(handle),
		 sizeof(u32), &handle, dcp_set_4k, cb);
}

/* DCP callback handlers */
static bool dcpep_cb_nop(struct apple_dcp *dcp, void *out, void *in)
{
	return true;
}

static bool dcpep_cb_swap_complete(struct apple_dcp *dcp, void *out, void *in)
{
	apple_crtc_vblank(dcp->crtc);
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
	struct sg_table *map;

	if (req->buffer >= ARRAY_SIZE(dcp->mappings))
		goto reject;

	map = &dcp->mappings[req->buffer];

	if (!map->sgl)
		goto reject;

	/* XNU leaks a kernel VA here, breaking kASLR. Don't do that. */
	resp->vaddr = 0;

	/* Use PIODMA device instead of DCP to map against the right IOMMU. */
	resp->ret = dma_map_sgtable(dcp->piodma, map, DMA_BIDIRECTIONAL, 0);

	if (resp->ret)
		dev_warn(dcp->dev, "failed to map for piodma %d\n", resp->ret);
	else
		resp->dva = sg_dma_address(map->sgl);

	resp->ret = 0;
	return true;

reject:
	dev_err(dcp->dev, "denying map of invalid buffer %llx for pidoma\n",
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

	dma_get_sgtable(dcp->dev, &dcp->mappings[resp->mem_desc_id], buf,
			resp->dva, resp->dva_size);

	WARN_ON(resp->mem_desc_id == 0);
	return true;
}

/* Validate that the specified region is a display register */
static bool is_disp0_register(u64 start, u64 end)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(disp0_registers); ++i) {
		struct dcp_map_reg_resp reg = disp0_registers[i];

		if ((start >= reg.addr) && (end <= reg.addr + reg.length))
			return true;
	}

	return false;
}

/*
 * Map an arbitrary chunk of physical memory into the DCP's address space. As
 * stated that's a massive security hole. In practice, benevolent DCP firmware
 * only uses this to map the display registers we advertise in
 * sr_map_device_memory_with_index. As long as we bounds check against this
 * register memory, the routine is safe against malicious coprocessors.
 */
static bool dcpep_cb_map_physical(struct apple_dcp *dcp, void *out, void *in)
{
	struct dcp_map_physical_resp *resp = out;
	struct dcp_map_physical_req *req = in;

	/* Padding for alignment could affect bounds checking, so pad first */
	resp->dva_size = ALIGN(req->size, 4096);

	if (!is_disp0_register(req->paddr, req->paddr + resp->dva_size)) {
		dev_err(dcp->dev, "refusing to map phys address %llx size %llx",
			req->paddr, req->size);
		return true;
	}

	resp->dva = dma_map_resource(dcp->dev, req->paddr, resp->dva_size,
				     DMA_BIDIRECTIONAL, 0);
	resp->mem_desc_id = ++dcp->nr_mappings;

	WARN_ON(resp->mem_desc_id == 0);

	return true;
}

/* Pixel clock frequency in Hz, a bit more than 4K@60 VGA clock 533.250 MHz */
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

	struct dcp_map_reg_resp error = {
		.ret = 1
	};

	if (req->index >= ARRAY_SIZE(disp0_registers)) {
		dev_warn(dcp->dev, "attempted to read invalid reg index %u",
			 req->index);

		*resp = error;
	} else {
		*resp = disp0_registers[req->index];
	}

	return true;
}

/* A number of callbacks of the form `bool cb()` can be tied to a constant. */
static bool dcpep_cb_true(struct apple_dcp *dcp, void *out, void *in)
{
	u8 *resp = out;

	*resp = true;
	return true;
}

static bool dcpep_cb_false(struct apple_dcp *dcp, void *out, void *in)
{
	u8 *resp = out;

	*resp = false;
	return true;
}

static void boot_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_cb_channel *ch = &dcp->ch_cb;
	u8 *succ = ch->output[ch->depth - 1];

	*succ = true;
	dcp_ack(dcp, DCP_CONTEXT_CB);
}

static void boot_5(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, false, dcp_set_display_refresh_properties, 0,
		 4, NULL, boot_done, NULL);
}

static void boot_4(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, false, dcp_late_init_signal, 0, 4, NULL, boot_5, NULL);
}

static void boot_3(struct apple_dcp *dcp, void *out, void *cookie)
{
	u8 v_true = true;

	dcp_push(dcp, false, dcp_flush_supports_power, sizeof(v_true), 0,
		 &v_true, boot_4, NULL);
}

static void boot_2(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, false, dcp_setup_video_limits, 0, 0, NULL, boot_3, NULL);
}

static void boot_1_5(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_push(dcp, false, dcp_create_default_fb, 0, sizeof(u32), NULL, boot_2, NULL);
}

static bool dcpep_cb_boot_1(struct apple_dcp *dcp, void *out, void *in)
{
	dcp_push(dcp, false, dcp_set_create_dfb, 0, 0, NULL, boot_1_5, NULL);
	return false;
}

static bool dcpep_cb_rt_bandwidth_setup(struct apple_dcp *dcp, void *out, void *in)
{
	struct dcp_rt_bandwidth *data = out;

	*data = (struct dcp_rt_bandwidth) {
		.unk1 = 0x44006B636F6C436CULL,
		.reg_scratch = 0x23B738014, // reg[5] in disp0/dispext0, plus 0x14 - part of pmgr
		.reg_doorbell = 0x23BC3C000, // reg[6] in disp0/dispext0 - part of pmp/pmgr
		.doorbell_bit = 2,
		.padding[1] = 0x43FB2690,
		.padding[2] = 0xFFFFFFFF,
		.padding[3] = 0x4,
		.padding[4] = 0x0,
		.padding[5] = 0x465,
	};

	BUILD_BUG_ON(sizeof(*data) != 0x3C);
	return true;
}

/* Callback to get the current time as milliseconds since the UNIX epoch */
static bool dcpep_cb_get_time(struct apple_dcp *dcp, void *out, void *in)
{
	u64 *ms = out;
	ktime_t time = ktime_get_real();

	*ms = ktime_to_ms(time);
	return true;
}

static void got_hotplug(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct apple_connector *connector = dcp->connector;
	struct drm_device *dev = connector->base.dev;

	connector->connected = !!(data);

	if (dev && dev->registered)
		drm_kms_helper_hotplug_event(dev);
}

static bool dcpep_cb_hotplug(struct apple_dcp *dcp, void *out, void *in)
{
	u64 *connected = in;

	/* Mode sets are required to reenable the connector */
	if (*connected)
		dcp_modeset(dcp, got_hotplug);
	else
		got_hotplug(dcp, NULL, NULL);

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
	[208] = {"get_calendar_time_ms", dcpep_cb_get_time },

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
	[576] = {"hotplug_notify_gated", dcpep_cb_hotplug },
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

	dev_dbg(dev, "channel %u: received callback %s\n", context, cb->name);

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
		dev_warn(dcp->dev, "ignoring ack on context %X\n", context);
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
	enum dcp_context_id ctx_id;
	u16 offset;
	u32 length;
	int channel_offset;
	void *data;

	ctx_id = (message & DCPEP_CONTEXT_MASK) >> DCPEP_CONTEXT_SHIFT;
	offset = (message & DCPEP_OFFSET_MASK) >> DCPEP_OFFSET_SHIFT;
	length = (message >> DCPEP_LENGTH_SHIFT);

	channel_offset = dcp_channel_offset(ctx_id);

	if (channel_offset < 0) {
		dev_warn(dcp->dev, "invalid context received %u", ctx_id);
		return;
	}

	data = dcp->shmem + channel_offset + offset;

	if (message & DCPEP_ACK)
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
		apple_crtc_vblank(dcp->crtc);
	}
}

static void dcp_swap_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct dcp_swap_start_resp *resp = data;
	struct dcp_swap_submit_req *req = cookie;

	req->swap.swap_id = resp->swap_id;

	dcp_push(dcp, false, dcp_swap_submit,
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
	struct drm_plane_state *new_state, *old_state;
	struct dcp_swap_submit_req *req;

	int l;

	if (WARN(dcp_channel_busy(&dcp->ch_cmd), "unexpected busy channel")) {
		apple_crtc_vblank(dcp->crtc);
		return;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);

	for_each_oldnew_plane_in_state(state, plane, old_state, new_state, l) {
		struct drm_framebuffer *fb = new_state->fb;
		struct drm_rect src_rect;

		WARN_ON(l >= SWAP_SURFACES);

		req->swap.swap_enabled |= BIT(l);

		if (!new_state->fb) {
			if (old_state->fb)
				req->swap.swap_enabled |= DCP_REMOVE_LAYERS;

			req->surf_null[l] = true;
			continue;
		}

		req->surf_iova[l] = drm_fb_cma_get_gem_addr(fb, new_state, 0);

		drm_rect_fp_to_int(&src_rect, &new_state->src);

		req->swap.src_rect[l] = drm_to_dcp_rect(&src_rect);
		req->swap.dst_rect[l] = drm_to_dcp_rect(&new_state->dst);

		req->swap.surf_flags[l] = 1;
		req->swap.surf_ids[l] = 3 + l; // XXX

		req->surf[l] = (struct dcp_surface) {
			//.format = dcp_formats[fb->format->format].dcp,
			.format = dcp_formats[0].dcp,
			.stride = fb->pitches[0],
			.width = fb->width,
			.height = fb->height,
			.buf_size = fb->height * fb->pitches[0],
			.surface_id = req->swap.surf_ids[l],

			/* Only used for compressed or multiplanar surfaces */
			.pix_size = 1,
			.pel_w = 1,
			.pel_h = 1,
			.has_comp = 1,
			.has_planes = 1,

			.unk_13 = 13,
			.unk_14 = 1,
		};
	}

	/* These fields should be set together */
	req->swap.swap_completed = req->swap.swap_enabled;

	WARN_ON(!dcp->active);

	dcp_push(dcp, false, dcp_swap_start,
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

static void dcp_active(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp->active = true;
}

static void dcp_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	u32 *resp = data;

	dev_info(dcp->dev, "DCP started, status %u\n", *resp);
	dcp_modeset(dcp, dcp_active);
}

static void dcp_got_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;
	enum dcpep_type type;

	WARN_ON(endpoint != DCP_ENDPOINT);

	type = (message >> DCPEP_TYPE_SHIFT) & DCPEP_TYPE_MASK;

	switch (type) {
	case DCPEP_TYPE_INITIALIZED:
		dcp_push(dcp, false, dcp_start_signal, 0, sizeof(u32), NULL,
			 dcp_started, NULL);
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

void dcp_link(struct platform_device *pdev, struct apple_crtc *crtc, struct apple_connector *connector)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	dcp->crtc = crtc;
	dcp->connector = connector;
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
	BUILD_BUG_ON(sizeof(struct dcp_swap) != 0x274);
	BUILD_BUG_ON(sizeof(struct dcp_plane_info) != 0x50);
	BUILD_BUG_ON(sizeof(struct dcp_component_types) != 0x8);
	BUILD_BUG_ON(sizeof(struct dcp_surface) != 0x204);
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

	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT,
				 dcpep_set_shmem(shmem_iova));

	if (ret)
		return ret;

	return ret;
}

/*
 * We need to shutdown DCP before tearing down the display subsystem. As Linux
 * shutdown clobbers video memory, failing to do so crashes the DCP, flashing
 * an annoying green screen of death.
 */
static void dcp_platform_shutdown(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	struct dcp_set_power_state_req req = {
		/* defaults are ok */
	};

	dcp_push(dcp, false, dcp_set_power_state,
		 sizeof(struct dcp_set_power_state_req),
		 sizeof(struct dcp_set_power_state_resp),
		 &req, NULL, NULL);
}

static int dcp_platform_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,t8103-dcp" },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
	.probe		= dcp_platform_probe,
	.remove		= dcp_platform_remove,
	.shutdown	= dcp_platform_shutdown,
	.driver	= {
		.name = "apple-dcp",
		.of_match_table	= of_match,
	},
};

module_platform_driver(apple_platform_driver);

MODULE_AUTHOR("Alyssa Rosenzweig <alyssa@rosenzweig.io>");
MODULE_DESCRIPTION("Apple Display Controller DRM driver");
MODULE_LICENSE("GPL v2");

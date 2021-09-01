/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */

#ifndef __APPLE_DCPEP_H__
#define __APPLE_DCPEP_H__

/* Endpoint for general DCP traffic (dcpep in macOS) */
#define DCP_ENDPOINT 0x37

/* Fixed size of shared memory between DCP and AP */
#define DCP_SHMEM_SIZE 0x100000

/* DCP message contexts */
enum dcp_context_id {
	/* Callback */
	DCP_CONTEXT_CB = 0,

	/* Command */
	DCP_CONTEXT_CMD = 2,

	/* Asynchronous */
	DCP_CONTEXT_ASYNC = 3,

	/* Out-of-band callback */
	DCP_CONTEXT_OOBCB = 4,

	/* Out-of-band command */
	DCP_CONTEXT_OOBCMD = 5,

	DCP_NUM_CONTEXTS
};

static int dcp_tx_offset(enum dcp_context_id id)
{
	switch (id) {
	case DCP_CONTEXT_CB:
	case DCP_CONTEXT_CMD:    return 0x00000;
	case DCP_CONTEXT_OOBCB:
	case DCP_CONTEXT_OOBCMD: return 0x08000;
	default:		 return -EINVAL;
	}
}

static int dcp_channel_offset(enum dcp_context_id id)
{
	switch (id) {
	case DCP_CONTEXT_ASYNC:  return 0x40000;
	case DCP_CONTEXT_CB:     return 0x60000;
	case DCP_CONTEXT_OOBCB:  return 0x68000;
	default:		 return dcp_tx_offset(id);
	}
}

/* RTKit endpoint message types */
enum dcpep_type {
	/* Set shared memory */
	DCPEP_TYPE_SET_SHMEM = 0,

	/* DCP is initialized */
	DCPEP_TYPE_INITIALIZED = 1,

	/* Remote procedure call */
	DCPEP_TYPE_MESSAGE = 2,
};

/* Message */
#define DCPEP_TYPE_SHIFT (0)
#define DCPEP_TYPE_MASK GENMASK(1, 0)
#define DCPEP_ACK_SHIFT (6)
#define DCPEP_CONTEXT_SHIFT (8)
#define DCPEP_CONTEXT_MASK GENMASK(11, 8)
#define DCPEP_OFFSET_SHIFT (16)
#define DCPEP_OFFSET_MASK GENMASK(31, 16)
#define DCPEP_LENGTH_SHIFT (32)

/* Set shmem */
#define DCPEP_DVA_SHIFT (16)
#define DCPEP_FLAG_SHIFT (4)
#define DCPEP_FLAG_VALUE (4)

struct dcp_packet_header {
	char tag[4];
	u32 in_len;
	u32 out_len;
} __packed;

#define DCP_IS_NULL(ptr) ((ptr) ? 1 : 0)
#define DCP_PACKET_ALIGNMENT (0x40)

static inline u64
dcpep_set_shmem(u64 dart_va)
{
	return (DCPEP_TYPE_SET_SHMEM << DCPEP_TYPE_SHIFT) |
		(DCPEP_FLAG_VALUE << DCPEP_FLAG_SHIFT) |
		(dart_va << DCPEP_DVA_SHIFT);
}

static inline u64
dcpep_msg(enum dcp_context_id id, u32 length, u16 offset, bool ack)
{
	return (DCPEP_TYPE_MESSAGE << DCPEP_TYPE_SHIFT) |
		(ack ? BIT_ULL(DCPEP_ACK_SHIFT) : 0) |
		((u64) id << DCPEP_CONTEXT_SHIFT) |
		((u64) offset << DCPEP_OFFSET_SHIFT) |
		((u64) length << DCPEP_LENGTH_SHIFT);
}

/* Structures used in v11.4 firmware (TODO: versioning as these change) */

#define SWAP_SURFACES 3
#define MAX_PLANES 3

struct dcp_iouserclient {
	/* Handle for the IOUserClient. macOS sets this to a kernel VA. */
	u64 handle;
	u32 unk;
	u32 flags;
} __packed;

struct dcp_rect {
	u32 x;
	u32 y;
	u32 w;
	u32 h;
} __packed;

/*
 * Set in the swap_{enabled,completed} field to remove missing
 * layers. Without this flag, the DCP will assume missing layers have
 * not changed since the previous frame and will preserve their
 * content.
  */
#define DCP_REMOVE_LAYERS BIT(31)

struct dcp_iomfbswaprec {
	u64 unk0[8];
	u64 flags1;
	u64 flags2;

	u32 swap_id;

	u32 surf_ids[SWAP_SURFACES];
	struct dcp_rect src_rect[SWAP_SURFACES];
	u32 surf_flags[SWAP_SURFACES];
	u32 surf_unk[SWAP_SURFACES];
	struct dcp_rect dst_rect[SWAP_SURFACES];
	u32 swap_enabled;
	u32 swap_completed;

	u32 unk1[101];
} __packed;

/* Information describing a plane of a planar compressed surface */
struct dcp_plane_info {
	u32 width;
	u32 height;
	u32 base;
	u32 offset;
	u32 stride;
	u32 size;
	u16 tile_size;
	u8 tile_w;
	u8 tile_h;
	u32 unk[13];
} __packed;

struct dcp_component_types {
	u8 count;
	u8 types[7];
} __packed;

/* Information describing a surface */
struct dcp_iosurface {
	u8 is_tiled;
	u8 unk_1;
	u8 unk_2;
	u32 plane_cnt;
	u32 plane_cnt2;
	u32 format; /* DCP fourcc */
	u32 unk_f;
	u8 unk_13;
	u8 unk_14;
	u32 stride;
	u16 pix_size;
	u8 pel_w;
	u8 pel_h;
	u32 offset;
	u32 width;
	u32 height;
	u32 buf_size;
	u32 unk_2d;
	u32 unk_31;
	u32 surface_id;
	struct dcp_component_types comp_types[MAX_PLANES];
	u64 has_comp;
	struct dcp_plane_info planes[MAX_PLANES];
	u64 has_planes;
	u32 compression_info[MAX_PLANES][13];
	u64 has_compr_info;
	u64 unk_1f5;
	u8 padding[7];
} __packed;

struct dcp_rt_bandwidth {
	u64 unk1;
	u64 reg_scratch;
	u64 reg_doorbell;
	u32 unk2;
	u32 doorbell_bit;
	u32 padding[7];
} __packed;

/* Method calls */

enum dcp_method {
	dcp_late_init_signal,
	dcp_setup_video_limits,
	dcp_set_create_dfb,
	dcp_start_signal,
	dcp_swap_start,
	dcp_swap_submit,
	dcp_set_display_device,
	dcp_set_digital_out_mode,
	dcp_create_default_fb,
	dcp_set_display_refresh_properties,
	dcp_flush_supports_power,
	dcp_num_methods
};

struct dcp_method_entry {
	const char *name;
	char tag[4];
};

/* Prototypes */

struct dcp_set_digital_out_mode_req {
	u32 mode0;
	u32 mode1;
} __packed;

struct dcp_map_buf_req {
	u64 buffer;
	u8 unk;
	u8 buf_null;
	u8 vaddr_null;
	u8 dva_null;
} __packed;

struct dcp_map_buf_resp {
	u64 vaddr;
	u64 dva;
	u32 ret;
} __packed;

struct dcp_allocate_buffer_req {
	u32 unk0;
	u64 size;
	u32 unk2;
	u8 paddr_null;
	u8 dva_null;
	u8 dva_size_null;
	u8 padding;
} __packed;

struct dcp_allocate_buffer_resp {
	u64 paddr;
	u64 dva;
	u64 dva_size;
	u32 mem_desc_id;
} __packed;

struct dcp_map_physical_req {
	u64 paddr;
	u64 size;
	u32 flags;
	u8 dva_null;
	u8 dva_size_null;
	u8 padding[2];
} __packed;

struct dcp_map_physical_resp {
	u64 dva;
	u64 dva_size;
	u32 mem_desc_id;
} __packed;

struct dcp_map_reg_req {
	char obj[4];
	u32 index;
	u32 flags;
	u8 addr_null;
	u8 length_null;
	u8 padding[2];
} __packed;

struct dcp_map_reg_resp {
	u64 addr;
	u64 length;
	u32 ret;
} __packed;

struct dcp_swap_start_req {
	u32 swap_id;
	struct dcp_iouserclient client;
	u8 swap_id_null;
	u8 client_null;
	u8 padding[2];
} __packed;

struct dcp_swap_start_resp {
	u32 swap_id;
	struct dcp_iouserclient client;
	u32 ret;
} __packed;

struct dcp_swap_submit_req {
	struct dcp_iomfbswaprec swap_rec;
	struct dcp_iosurface surf[SWAP_SURFACES];
	u32 surf_iova[SWAP_SURFACES];
	u8 unkbool;
	u64 unkdouble;
	u32 unkint;
	u8 swap_rec_null;
	u8 surf_null[SWAP_SURFACES];
	u8 unkoutbool_null;
	u8 padding[2];
} __packed;

struct dcp_swap_submit_resp {
	u8 unkoutbool;
	u32 ret;
	u8 padding[3];
} __packed;

struct dcp_get_uint_prop_req {
	char obj[4];
	char key[0x40];
	u64 value;
	u8 value_null;
	u8 padding[3];
} __packed;

struct dcp_get_uint_prop_resp {
	u64 value;
	u8 ret;
	u8 padding[3];
} __packed;

#endif

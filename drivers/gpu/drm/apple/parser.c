// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/math.h>
#include <linux/string.h>
#include <linux/slab.h>
#include "parser.h"

#define DCP_PARSE_HEADER 0xd3

enum dcp_parse_type {
	DCP_TYPE_DICTIONARY = 1,
	DCP_TYPE_ARRAY = 2,
	DCP_TYPE_INT64 = 4,
	DCP_TYPE_STRING = 9,
	DCP_TYPE_BLOB = 10,
	DCP_TYPE_BOOL = 11
};

struct dcp_parse_tag {
	unsigned int size : 24;
	enum dcp_parse_type type : 5;
	unsigned int padding : 2;
	bool last : 1;
} __packed;

static void *parse_bytes(struct dcp_parse_ctx *ctx, size_t count)
{
	void *ptr = ctx->blob + ctx->pos;

	if (ctx->pos + count > ctx->len)
		return ERR_PTR(-EINVAL);

	ctx->pos += count;
	return ptr;
}

static u32 *parse_u32(struct dcp_parse_ctx *ctx)
{
	return parse_bytes(ctx, sizeof(u32));
}

static struct dcp_parse_tag *parse_tag(struct dcp_parse_ctx *ctx)
{
	struct dcp_parse_tag *tag;

	/* Align to 32-bits */
	ctx->pos = round_up(ctx->pos, 4);

	tag = parse_bytes(ctx, sizeof(struct dcp_parse_tag));

	if (IS_ERR(tag))
		return tag;

	if (tag->padding)
		return ERR_PTR(-EINVAL);

	return tag;
}

static struct dcp_parse_tag *parse_tag_of_type(struct dcp_parse_ctx *ctx,
					       enum dcp_parse_type type)
{
	struct dcp_parse_tag *tag = parse_tag(ctx);

	if (IS_ERR(tag))
		return tag;

	if (tag->type != type)
		return ERR_PTR(-EINVAL);

	return tag;
}

static int skip(struct dcp_parse_ctx *handle)
{
	struct dcp_parse_tag *tag = parse_tag(handle);
	int ret = 0;
	int i;

	if (IS_ERR(tag))
		return PTR_ERR(tag);

	switch (tag->type) {
	case DCP_TYPE_DICTIONARY:
		for (i = 0; i < tag->size; ++i) {
			ret |= skip(handle); /* key */
			ret |= skip(handle); /* value */
		}

		return ret;

	case DCP_TYPE_ARRAY:
		for (i = 0; i < tag->size; ++i)
			ret |= skip(handle);

		return ret;

	case DCP_TYPE_INT64:
		handle->pos += sizeof(s64);
		return 0;

	case DCP_TYPE_STRING:
	case DCP_TYPE_BLOB:
		handle->pos += tag->size;
		return 0;

	case DCP_TYPE_BOOL:
		return 0;

	default:
		return -EINVAL;
	}
}

/* Caller must free the result */
static char *parse_string(struct dcp_parse_ctx *handle)
{
	struct dcp_parse_tag *tag = parse_tag_of_type(handle, DCP_TYPE_STRING);
	const char *in;
	char *out;

	if (IS_ERR(tag))
		return (void *) tag;

	in = parse_bytes(handle, tag->size);
	if (IS_ERR(in))
		return (void *) in;

	out = kmalloc(tag->size + 1, GFP_KERNEL);

	memcpy(out, in, tag->size);
	out[tag->size] = '\0';
	return out;
}

static int parse_int(struct dcp_parse_ctx *handle, s64 *value)
{
	void *tag = parse_tag_of_type(handle, DCP_TYPE_INT64);
	s64 *in;

	if (IS_ERR(tag))
		return PTR_ERR(tag);

	in = parse_bytes(handle, sizeof(s64));

	if (IS_ERR(in))
		return PTR_ERR(in);

	memcpy(value, in, sizeof(*value));
	return 0;
}

static int parse_bool(struct dcp_parse_ctx *handle, bool *b)
{
	struct dcp_parse_tag *tag = parse_tag_of_type(handle, DCP_TYPE_BOOL);
	if (IS_ERR(tag))
		return PTR_ERR(tag);

	*b = !!tag->size;
	return 0;
}

struct iterator {
	struct dcp_parse_ctx *handle;
	u32 idx;
	u32 len;
};

int iterator_begin(struct dcp_parse_ctx *handle, struct iterator *it, bool dict)
{
	struct dcp_parse_tag *tag;
	enum dcp_parse_type type = dict ? DCP_TYPE_DICTIONARY : DCP_TYPE_ARRAY;

	*it = (struct iterator) {
		.handle = handle,
		.idx = 0
	};

	tag = parse_tag_of_type(it->handle, type);
	if (IS_ERR(tag))
		return PTR_ERR(tag);

	it->len = tag->size;
	return 0;
}

#define dcp_parse_foreach_in_array(handle, it) \
	for (iterator_begin(handle, &it, false); it.idx < it.len; ++it.idx)
#define dcp_parse_foreach_in_dict(handle, it) \
	for (iterator_begin(handle, &it, true); it.idx < it.len; ++it.idx)

int parse(void *blob, size_t size, struct dcp_parse_ctx *ctx)
{
	u32 *header;

	*ctx = (struct dcp_parse_ctx) {
		.blob = blob,
		.len = size,
		.pos = 0,
	};

	header = parse_u32(ctx);
	if (IS_ERR(header))
		return PTR_ERR(header);

	if (*header != DCP_PARSE_HEADER)
		return -EINVAL;

	return 0;
}

struct dimension {
	s64 total, front_porch, sync_width, back_porch, active;
	s64 sync_rate, precise_sync_rate;
};

static int parse_dimension(struct dcp_parse_ctx *handle, struct dimension *dim)
{
	struct iterator it;
	int ret = 0;

	dcp_parse_foreach_in_dict(handle, it) {
		char *key = parse_string(it.handle);

		if (IS_ERR(key))
			return PTR_ERR(handle);

		if (!strcmp(key, "Active"))
			ret = parse_int(it.handle, &dim->active);
		else if (!strcmp(key, "Total"))
			ret = parse_int(it.handle, &dim->total);
		else if (!strcmp(key, "FrontPorch"))
			ret = parse_int(it.handle, &dim->front_porch);
		else if (!strcmp(key, "BackPorch"))
			ret = parse_int(it.handle, &dim->back_porch);
		else if (!strcmp(key, "SyncWidth"))
			ret = parse_int(it.handle, &dim->sync_width);
		else if (!strcmp(key, "SyncRate"))
			ret = parse_int(it.handle, &dim->sync_rate);
		else if (!strcmp(key, "PreciseSyncRate"))
			ret = parse_int(it.handle, &dim->precise_sync_rate);
		else
			skip(it.handle);

		if (ret)
			return ret;
	}

	return 0;
}

static int parse_color_modes(struct dcp_parse_ctx *handle, s64 *best_id)
{
	struct iterator outer_it;
	int ret = 0;
	s64 best_score = -1;

	*best_id = -1;

	dcp_parse_foreach_in_array(handle, outer_it) {
		struct iterator it;
		s64 score = -1, id = -1;

		dcp_parse_foreach_in_dict(handle, it) {
			char *key = parse_string(it.handle);

			if (IS_ERR(key))
				return PTR_ERR(key);

			if (!strcmp(key, "Score"))
				ret = parse_int(it.handle, &score);
			else if (!strcmp(key, "ID"))
				ret = parse_int(it.handle, &id);
			else
				skip(it.handle);

			if (ret)
				return ret;
		}

		/* Skip partial entries */
		if (score < 0 || id < 0)
			continue;

		if (score > best_score) {
			best_score = score;
			*best_id = id;
		}
	}

	return 0;
}

static int parse_mode(struct dcp_parse_ctx *handle, struct dcp_display_mode *out)
{
	int ret = 0;
	struct iterator it;
	struct dimension horiz, vert;
	s64 id = -1;
	s64 best_color_mode = -1;

	struct drm_display_mode *mode = &out->mode;

	dcp_parse_foreach_in_dict(handle, it) {
		char *key = parse_string(it.handle);

		if (IS_ERR(key))
			return PTR_ERR(key);

		if (!strcmp(key, "HorizontalAttributes"))
			ret = parse_dimension(it.handle, &horiz);
		else if (!strcmp(key, "VerticalAttributes"))
			ret = parse_dimension(it.handle, &vert);
		else if (!strcmp(key, "ColorModes"))
			ret = parse_color_modes(it.handle, &best_color_mode);
		else if (!strcmp(key, "ID"))
			ret = parse_int(it.handle, &id);
		else
			skip(it.handle);

		if (ret)
			return ret;
	}

	*mode = (struct drm_display_mode) {
		DRM_SIMPLE_MODE(horiz.active, vert.active, 508, 286)
	};

	mode->clock = (s32) (vert.sync_rate >> 16) * mode->htotal * mode->vtotal;
	drm_mode_set_name(mode);

	out->timing_mode_id = id;
	out->color_mode_id = best_color_mode;

#if 0
	printk("mode: %lld:%lld: %lldx%lld@%d\n",
		id, best_color_mode, horiz.active, vert.active,
	        (s32) (vert.sync_rate >> 16));
#endif
	return 0;
}

struct dcp_display_mode *enumerate_modes(struct dcp_parse_ctx *handle,
					 unsigned int *count)
{
	struct iterator it;
	int ret;
	struct dcp_display_mode *modes = NULL;

	dcp_parse_foreach_in_array(handle, it) {
		printk("%u modes\n", it.len);
		if (!modes)
			modes = kmalloc_array(it.len, sizeof(*modes), GFP_KERNEL);
		if (!modes)
			return ERR_PTR(-ENOMEM);

		ret = parse_mode(it.handle, &modes[it.idx]);

		if (ret)
			return ERR_PTR(ret);
	}

	*count = it.len;
	return modes;
}

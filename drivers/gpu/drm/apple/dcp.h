/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */

#ifndef __APPLE_DCP_H__
#define __APPLE_DCP_H__

#include <drm/drm_atomic.h>
#include "parser.h"

struct apple_crtc {
	struct drm_crtc base;
	struct drm_pending_vblank_event *event;
	bool vsync_disabled;

	/* Reference to the DCP device owning this CRTC */
	struct platform_device *dcp;
};

#define to_apple_crtc(x) container_of(x, struct apple_crtc, base)

void dcp_hotplug(struct work_struct *work);

struct apple_connector {
	struct drm_connector base;
	bool connected;

	struct platform_device *dcp;

	/* Workqueue for sending hotplug events to the associated device */
	struct work_struct hotplug_wq;
};

#define to_apple_connector(x) container_of(x, struct apple_connector, base)

/*
 * Table of supported formats, mapping from DRM fourccs to DCP fourccs.
 *
 * For future work, DCP supports more formats not listed, including YUV
 * formats, an extra RGBA format, and a biplanar RGB10_A8 format (fourcc b3a8)
 * used for HDR.
 *
 * Note: we don't have non-alpha formats but userspace breaks without XRGB. It
 * doesn't matter for the primary plane, but cursors/overlays must not
 * advertise formats without alpha.
 */
static const u32 dcp_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
};

static inline u32 drm_format_to_dcp(u32 drm)
{
	switch (drm) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return fourcc_code('A', 'R', 'G', 'B');

	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return fourcc_code('A', 'B', 'G', 'R');
	}

	pr_warn("DRM format %X not supported in DCP\n", drm);
	return 0;
}

void dcp_link(struct platform_device *pdev, struct apple_crtc *apple,
	      struct apple_connector *connector);
void dcp_flush(struct drm_crtc *crtc, struct drm_atomic_state *state);
bool dcp_is_initialized(struct platform_device *pdev);
void apple_crtc_vblank(struct apple_crtc *apple);
int dcp_get_modes(struct drm_connector *connector);
int dcp_mode_valid(struct drm_connector *connector,
		   struct drm_display_mode *mode);

#endif

// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */
/* Based on meson driver which is
 * Copyright (C) 2016 BayLibre, SAS
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 * Copyright (C) 2015 Amlogic, Inc. All rights reserved.
 * Copyright (C) 2014 Endless Mobile
 */

#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>

#include <drm/drm_aperture.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_fixed.h>

#include "dcp.h"

#define DRIVER_NAME     "apple"
#define DRIVER_DESC     "Apple display controller DRM driver"
#define DRIVER_DATE     "20210901"
#define DRIVER_MAJOR    1
#define DRIVER_MINOR    0

/* TODO: Workaround src rect limitations */
#define TODO_WITH_CURSOR 1

struct apple_crtc {
	struct drm_crtc base;
	struct drm_pending_vblank_event *event;
	bool vsync_disabled;
};

#define to_apple_crtc(x) container_of(x, struct apple_crtc, base)

struct apple_drm_private {
	struct drm_device drm;
	struct platform_device *dcp;
	struct apple_crtc *crtc;
};

#define to_apple_drm_private(x) \
	container_of(x, struct apple_drm_private, drm)

DEFINE_DRM_GEM_CMA_FOPS(apple_fops);

static const struct drm_driver apple_drm_driver = {
	DRM_GEM_CMA_DRIVER_OPS,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.date			= DRIVER_DATE,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops			= &apple_fops,
};

static int apple_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state;
	struct drm_crtc_state *crtc_state;

	new_plane_state	= drm_atomic_get_new_plane_state(state, plane);

	if (!new_plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_crtc_state(state, new_plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	/*
	 * DCP limits downscaling to 2x and upscaling to 4x. Attempting to
	 * scale outside these bounds results in an error reported on the DCP
	 * syslog and a lost swap.
	 *
	 * This function also takes care of clipping the src/dest rectangles,
	 * which is required for correct operation. Partially off-screen
	 * surfaces may appear corrupted.
	 *
	 * There is no distinction between plane types in the hardware, so we
	 * set can_position. If the primary plane does not fill the screen, the
	 * hardware will fill in zeroes (black).
	 */
	return drm_atomic_helper_check_plane_state(new_plane_state,
						   crtc_state,
						   drm_fixed_16_16(1, 4),
						   drm_fixed_16_16(2, 1),
						   true, true);
}

static void apple_plane_atomic_disable(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	/* TODO */
}

static void apple_plane_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
}

static const struct drm_plane_helper_funcs apple_plane_helper_funcs = {
	.atomic_check	= apple_plane_atomic_check,
	.atomic_disable	= apple_plane_atomic_disable,
	.atomic_update	= apple_plane_atomic_update,
};

static const struct drm_plane_funcs apple_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

/* Mapping of DRM formats to DCP formats specified as a fourcc */
u64 apple_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

struct drm_plane *apple_plane_init(struct drm_device *dev, enum drm_plane_type type)
{
	int ret;
	struct drm_plane *plane;
	u32 plane_formats[ARRAY_SIZE(dcp_formats)];
	int i;

	for (i = 0; i < ARRAY_SIZE(dcp_formats); ++i)
		plane_formats[i] = dcp_formats[i].drm;

	plane = devm_kzalloc(dev->dev, sizeof(*plane), GFP_KERNEL);

	ret = drm_universal_plane_init(dev, plane, 0x1, &apple_plane_funcs,
				       plane_formats,
				       ARRAY_SIZE(dcp_formats),
				       apple_format_modifiers,
				       type, NULL);

	drm_plane_helper_add(plane, &apple_plane_helper_funcs);

	if (ret)
		return ERR_PTR(ret);

	return plane;
}

static int apple_enable_vblank(struct drm_crtc *crtc)
{
	struct apple_crtc *apple_crtc = to_apple_crtc(crtc);
	apple_crtc->vsync_disabled = false;
	return 0;
}

static void apple_disable_vblank(struct drm_crtc *crtc)
{
	struct apple_crtc *apple_crtc = to_apple_crtc(crtc);
	apple_crtc->vsync_disabled = true;
}

static int apple_connector_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;

	/* STUB */

	struct drm_display_mode dummy = {
		DRM_SIMPLE_MODE(1920*2, 1080*2, 508, 286),
	};

	dummy.clock = 60 * dummy.hdisplay * dummy.vdisplay;
	drm_mode_set_name(&dummy);

	mode = drm_mode_duplicate(dev, &dummy);
	if (!mode) {
		DRM_ERROR("Failed to create a new display mode\n");
		return 0;
	}

	drm_mode_probed_add(connector, mode);
	return 1;
}

static int apple_connector_mode_valid(struct drm_connector *connector,
				      struct drm_display_mode *mode)
{
	/* STUB */
	return MODE_OK;
}

static void apple_crtc_atomic_enable(struct drm_crtc *crtc,
				     struct drm_atomic_state *state)
{
	drm_crtc_vblank_on(crtc);
}

static void apple_crtc_atomic_disable(struct drm_crtc *crtc,
				      struct drm_atomic_state *state)
{
	drm_crtc_vblank_off(crtc);

	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static void apple_crtc_atomic_begin(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	struct apple_crtc *apple_crtc = to_apple_crtc(crtc);
	unsigned long flags;

	if (crtc->state->event) {
		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		apple_crtc->event = crtc->state->event;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		crtc->state->event = NULL;
	}
}

void apple_crtc_vblank(struct apple_drm_private *apple)
{
	unsigned long flags;

	if (apple->crtc->vsync_disabled)
		return;

	drm_crtc_handle_vblank(&apple->crtc->base);

	spin_lock_irqsave(&apple->drm.event_lock, flags);
	if (apple->crtc->event) {
		drm_crtc_send_vblank_event(&apple->crtc->base, apple->crtc->event);
		drm_crtc_vblank_put(&apple->crtc->base);
		apple->crtc->event = NULL;
	}
	spin_unlock_irqrestore(&apple->drm.event_lock, flags);
}

static void apple_crtc_atomic_flush(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	struct apple_drm_private *apple = to_apple_drm_private(crtc->dev);
	dcp_swap(apple->dcp, state);
}

static const struct drm_crtc_funcs apple_crtc_funcs = {
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.destroy		= drm_crtc_cleanup,
	.page_flip		= drm_atomic_helper_page_flip,
	.reset			= drm_atomic_helper_crtc_reset,
	.set_config             = drm_atomic_helper_set_config,
	.enable_vblank		= apple_enable_vblank,
	.disable_vblank		= apple_disable_vblank,
};

static const struct drm_encoder_funcs apple_encoder_funcs = {
	.destroy		= drm_encoder_cleanup,
};

static const struct drm_mode_config_funcs apple_mode_config_funcs = {
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
	.fb_create		= drm_gem_fb_create,
};

static const struct drm_mode_config_helper_funcs apple_mode_config_helpers = {
	.atomic_commit_tail	= drm_atomic_helper_commit_tail_rpm,
};

static const struct drm_connector_funcs apple_connector_funcs = {
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs apple_connector_helper_funcs = {
	.get_modes		= apple_connector_get_modes,
	.mode_valid		= apple_connector_mode_valid,
};

static const struct drm_crtc_helper_funcs apple_crtc_helper_funcs = {
	.atomic_begin		= apple_crtc_atomic_begin,
	.atomic_flush		= apple_crtc_atomic_flush,
	.atomic_enable		= apple_crtc_atomic_enable,
	.atomic_disable		= apple_crtc_atomic_disable,
};

static int apple_platform_probe(struct platform_device *pdev)
{
	struct apple_drm_private *apple;
	struct platform_device *dcp;
	struct device_node *dcp_node;
	struct drm_plane *plane = NULL, *cursor = NULL;
	struct apple_crtc *crtc;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	int ret;

	dcp_node = of_parse_phandle(pdev->dev.of_node, "coprocessor", 0);

	if (!dcp_node)
		return -ENODEV;

	dcp = of_find_device_by_node(dcp_node);

	if (!dcp)
		return -ENODEV;

	/* DCP needs to be initialized before KMS can come online */
	if (!platform_get_drvdata(dcp))
		return -EPROBE_DEFER;

	if (!dcp_is_initialized(dcp))
		return -EPROBE_DEFER;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	apple = devm_drm_dev_alloc(&pdev->dev, &apple_drm_driver,
				   struct apple_drm_private, drm);
	if (IS_ERR(apple))
		return PTR_ERR(apple);

	apple->dcp = dcp;
	dcp_link(dcp, apple);

	ret = drm_vblank_init(&apple->drm, 1);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(&apple->drm);
	if (ret)
		goto err_unload;

	/* DCP clamps surfaces below this size */
	apple->drm.mode_config.min_width = 32;
	apple->drm.mode_config.min_height = 32;

	/* Unknown maximum, use a safe value */
	apple->drm.mode_config.max_width = 3840;
	apple->drm.mode_config.max_height = 2160;
	apple->drm.mode_config.cursor_width = 64;
	apple->drm.mode_config.cursor_height = 64;

	apple->drm.mode_config.funcs = &apple_mode_config_funcs;
	apple->drm.mode_config.helper_private = &apple_mode_config_helpers;

	plane = apple_plane_init(&apple->drm, DRM_PLANE_TYPE_PRIMARY);

	if (IS_ERR(plane)) {
		ret = PTR_ERR(plane);
		goto err_unload;
	}

	if (TODO_WITH_CURSOR) {
		cursor = apple_plane_init(&apple->drm, DRM_PLANE_TYPE_CURSOR);

		if (IS_ERR(cursor)) {
			ret = PTR_ERR(cursor);
			goto err_unload;
		}
	}

	crtc = devm_kzalloc(&pdev->dev, sizeof(*crtc), GFP_KERNEL);
	ret = drm_crtc_init_with_planes(&apple->drm, &crtc->base, plane, cursor,
					&apple_crtc_funcs, NULL);
	if (ret)
		goto err_unload;

	drm_crtc_helper_add(&crtc->base, &apple_crtc_helper_funcs);
	apple->crtc = crtc;

	encoder = devm_kzalloc(&pdev->dev, sizeof(*encoder), GFP_KERNEL);
	encoder->possible_crtcs = drm_crtc_mask(&crtc->base);
	ret = drm_encoder_init(&apple->drm, encoder, &apple_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS /* XXX */, "apple_hdmi");
	if (ret)
		goto err_unload;

	connector = devm_kzalloc(&pdev->dev, sizeof(*connector), GFP_KERNEL);
	drm_connector_helper_add(connector, &apple_connector_helper_funcs);

	ret = drm_connector_init(&apple->drm, connector, &apple_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		goto err_unload;

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		goto err_unload;

	drm_mode_config_reset(&apple->drm); // TODO: needed?

	/* Remove early framebuffers (simplefb) */
	ret = drm_aperture_remove_framebuffers(false, &apple_drm_driver);
	if (ret)
		return ret;

	ret = drm_dev_register(&apple->drm, 0);
	if (ret)
		goto err_unload;

	drm_fbdev_generic_setup(&apple->drm, 32);

	return 0;

err_unload:
	drm_dev_put(&apple->drm);
	return ret;
}

static int apple_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	drm_dev_unregister(drm);

	return 0;
}

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,t8103-dcp" },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
	.driver	= {
		.name = "apple",
		.of_match_table	= of_match,
	},
	.probe		= apple_platform_probe,
	.remove		= apple_platform_remove,
};

module_platform_driver(apple_platform_driver);

MODULE_AUTHOR("Alyssa Rosenzweig <alyssa@rosenzweig.io>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL v2");

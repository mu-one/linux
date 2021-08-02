// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */
/* Based on meson driver which is
 * Copyright (C) 2016 BayLibre, SAS
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 * Copyright (C) 2015 Amlogic, Inc. All rights reserved.
 * Copyright (C) 2014 Endless Mobile
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>

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

#define DISP0_FORMAT 0x30
#define    DISP0_FORMAT_BGRA 0x5000
#define DISP0_FRAMEBUFFER_0 0x54
#define DISP0_FRAMEBUFFER_1 0x58
#define DISP0_FRAMEBUFFER_2 0x5c

struct apple_drm_private {
	struct drm_device	drm;
	void __iomem		*regs;
};

#define to_apple_drm_private(x) \
	container_of(x, struct apple_drm_private, drm)

DEFINE_DRM_GEM_CMA_FOPS(apple_fops);

static const struct drm_driver apple_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = "apple",
	.desc = "Apple Display Controller DRM driver",
	.date = "20210801",
	.major = 1,
	.minor = 0,
	.patchlevel = 0,
	.fops = &apple_fops,
	DRM_GEM_CMA_DRIVER_OPS,
};

static int apple_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	/* TODO */
	return 0;

}

static void apple_plane_atomic_disable(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	/* TODO */
}

static void apple_plane_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct apple_drm_private *apple = to_apple_drm_private(plane->dev);
	struct drm_plane_state *plane_state;
	struct drm_framebuffer *fb;
	dma_addr_t dva;

	plane_state = drm_atomic_get_new_plane_state(state, plane);
	fb = plane_state->fb;
	dva = drm_fb_cma_get_gem_addr(fb, plane_state, 0);

	writel(dva, apple->regs + DISP0_FRAMEBUFFER_0);
	writel(DISP0_FORMAT_BGRA, apple->regs + DISP0_FORMAT);
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

uint32_t apple_plane_formats[] = {
	/* TODO: More formats */
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

uint64_t apple_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

struct drm_plane *apple_plane_init(struct drm_device *dev)
{
	int ret;
	struct drm_plane *plane;

	plane = devm_kzalloc(dev->dev, sizeof(*plane), GFP_KERNEL);

	ret = drm_universal_plane_init(dev, plane, 0x1, &apple_plane_funcs,
				       apple_plane_formats,
				       ARRAY_SIZE(apple_plane_formats),
				       apple_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);

	drm_plane_helper_add(plane, &apple_plane_helper_funcs);

	if (ret)
		return ERR_PTR(ret);

	return plane;
}

static const struct drm_crtc_funcs apple_crtc_funcs = {
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.destroy		= drm_crtc_cleanup,
	.page_flip		= drm_atomic_helper_page_flip,
	.reset			= drm_atomic_helper_crtc_reset,
	.set_config             = drm_atomic_helper_set_config,
};

static void apple_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
}

static const struct drm_encoder_funcs apple_encoder_funcs = {
	.destroy        = apple_encoder_destroy,
};

static const struct drm_mode_config_funcs apple_mode_config_funcs = {
	.atomic_check        = drm_atomic_helper_check,
	.atomic_commit       = drm_atomic_helper_commit,
	.fb_create           = drm_gem_fb_create,
};

static const struct drm_mode_config_helper_funcs apple_mode_config_helpers = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static void apple_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
}

static enum drm_connector_status
apple_connector_detect(struct drm_connector *connector, bool force)
{
	/* TODO: stub */
	return connector_status_connected;
}

static const struct drm_connector_funcs apple_connector_funcs = {
	.detect			= apple_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= apple_connector_destroy,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static int apple_connector_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;

	/* STUB */

	struct drm_display_mode dummy = {
		DRM_SIMPLE_MODE(1920, 1080, 508, 286),
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

static const
struct drm_connector_helper_funcs apple_connector_helper_funcs = {
	.get_modes	= apple_connector_get_modes,
	.mode_valid	= apple_connector_mode_valid,
};

static void apple_crtc_atomic_enable(struct drm_crtc *crtc,
				     struct drm_atomic_state *state)
{
	/* TODO */
}

static void apple_crtc_atomic_disable(struct drm_crtc *crtc,
				      struct drm_atomic_state *state)
{
	/* TODO */
}

static void apple_crtc_atomic_begin(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	/* TODO */
}

static void apple_crtc_atomic_flush(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	/* TODO */
}

static const struct drm_crtc_helper_funcs apple_crtc_helper_funcs = {
	.atomic_begin	= apple_crtc_atomic_begin,
	.atomic_flush	= apple_crtc_atomic_flush,
	.atomic_enable	= apple_crtc_atomic_enable,
	.atomic_disable	= apple_crtc_atomic_disable,
};

static int apple_platform_probe(struct platform_device *pdev)
{
	struct apple_drm_private *apple;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	int ret;

	apple = devm_drm_dev_alloc(&pdev->dev, &apple_drm_driver,
				   struct apple_drm_private, drm);
	if (IS_ERR(apple))
		return PTR_ERR(apple);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

	if (ret)
		return ret;

        apple->regs = devm_platform_ioremap_resource(pdev, 0);

	if (!apple->regs)
		return -ENODEV;

	/*
	 * Remove early framebuffers (ie. simplefb). The framebuffer can be
	 * located anywhere in RAM
	 */
	ret = drm_aperture_remove_framebuffers(false, &apple_drm_driver);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(&apple->drm);
	if (ret)
		goto err_unload;

	apple->drm.mode_config.max_width = 3840;
	apple->drm.mode_config.max_height = 2160;
	apple->drm.mode_config.funcs = &apple_mode_config_funcs;
	apple->drm.mode_config.helper_private	= &apple_mode_config_helpers;

	plane = apple_plane_init(&apple->drm);

	if (IS_ERR(plane)) {
		ret = PTR_ERR(plane);
		goto err_unload;
	}

	crtc = devm_kzalloc(&pdev->dev, sizeof(*crtc), GFP_KERNEL);
	ret = drm_crtc_init_with_planes(&apple->drm, crtc, plane, NULL, &apple_crtc_funcs, NULL);
	if (ret)
		goto err_unload;


	drm_crtc_helper_add(crtc, &apple_crtc_helper_funcs);

	encoder = devm_kzalloc(&pdev->dev, sizeof(*encoder), GFP_KERNEL);
	encoder->possible_crtcs = drm_crtc_mask(crtc);
	ret = drm_encoder_init(&apple->drm, encoder, &apple_encoder_funcs, DRM_MODE_ENCODER_TMDS /* XXX */, "apple_hdmi");
	if (ret)
		goto err_unload;

	connector = devm_kzalloc(&pdev->dev, sizeof(*connector), GFP_KERNEL);

	drm_connector_helper_add(connector,
				 &apple_connector_helper_funcs);

	ret = drm_connector_init(&apple->drm, connector, &apple_connector_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		goto err_unload;

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		goto err_unload;

	drm_mode_config_reset(&apple->drm); // TODO: needed?

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
	.probe		= apple_platform_probe,
	.remove		= apple_platform_remove,
	.driver	= {
		.name = "apple",
		.of_match_table	= of_match,
	},
};

module_platform_driver(apple_platform_driver);

MODULE_AUTHOR("Alyssa Rosenzweig <alyssa@rosenzweig.io>");
MODULE_DESCRIPTION("Apple Display Controller DRM driver");
MODULE_LICENSE("GPL_v2");

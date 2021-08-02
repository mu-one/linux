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

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

struct apple_drm_private {
	struct drm_device	drm;
	void __iomem		*regs;
};

DEFINE_DRM_GEM_CMA_FOPS(apple_fops);

static const struct drm_driver apple_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = "apple",
	.desc = "Apple Display Controller",
	.date = "20210801",
	.major = 1,
	.minor = 0,
	.patchlevel = 0,
	.fops = &apple_fops,
	DRM_GEM_CMA_DRIVER_OPS,
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
	DRM_FORMAT_ARGB8888,
};

uint64_t apple_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,

	/* Keep last */
	DRM_FORMAT_MOD_INVALID
};

struct drm_plane *apple_plane_init(struct drm_device *dev)
{
	int ret;
	struct drm_plane *plane;

	plane = devm_kzalloc(dev->dev, sizeof(*plane), GFP_KERNEL);

	ret = drm_universal_plane_init(dev, plane, 0, &apple_plane_funcs,
				       apple_plane_formats,
				       ARRAY_SIZE(apple_plane_formats),
				       apple_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);

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

static int apple_platform_probe(struct platform_device *pdev)
{
	struct apple_drm_private *apple;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	int ret;

	apple = devm_drm_dev_alloc(&pdev->dev, &apple_drm_driver,
				   struct apple_drm_private, drm);
	if (IS_ERR(apple))
		return PTR_ERR(apple);

	ret = drmm_mode_config_init(&apple->drm);
	if (ret)
		goto err_unload;

	apple->drm.mode_config.max_width = 3840;
	apple->drm.mode_config.max_height = 2160;
	apple->drm.mode_config.funcs = &apple_mode_config_funcs;
	apple->drm.mode_config.helper_private	= &apple_mode_config_helpers;

	printk("hello from apple drm\n");
	plane = apple_plane_init(&apple->drm);

	if (IS_ERR(plane)) {
		ret = PTR_ERR(plane);
		goto err_unload;
	}

	printk("got plane %p\n", plane);

	crtc = devm_kzalloc(&pdev->dev, sizeof(*crtc), GFP_KERNEL);
	ret = drm_crtc_init_with_planes(&apple->drm, crtc, plane, NULL, &apple_crtc_funcs, NULL);
	if (ret)
		goto err_unload;

	printk("got crtc %p\n", crtc);

	encoder = devm_kzalloc(&pdev->dev, sizeof(*encoder), GFP_KERNEL);
	ret = drm_encoder_init(&apple->drm, encoder, &apple_encoder_funcs, DRM_MODE_ENCODER_TMDS /* XXX */, "apple_hdmi");
	if (ret)
		goto err_unload;

	drm_mode_config_reset(&apple->drm); // TODO: needed?

	printk("reset\n");

	ret = drm_dev_register(&apple->drm, 0);
	if (ret)
		goto err_unload;
	printk("registered\n");

	drm_fbdev_generic_setup(&apple->drm, 32);

	printk("setup\n");
	return 0;

err_unload:
	drm_dev_put(&apple->drm);
	printk("probing failed with error %d\n", ret);
	return ret;
}

static int apple_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	drm_dev_unregister(drm);

	return 0;
}

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,m1-dcp" },
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
MODULE_DESCRIPTION("Apple Silicon DRM driver");
MODULE_LICENSE("GPL_v2");

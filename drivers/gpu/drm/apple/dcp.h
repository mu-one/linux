#include <drm/drm_atomic.h>

struct apple_drm_private;

/*
 * Table of supported formats, mapping from DRM fourccs to DCP fourccs.
 *
 * TODO: Rather than RGB10_A2, macOS uses a biplanar RGB10_A8 format,
 * corresponding to DCP format "b3a8". Should a DRM format be created for this?
 *
 * TODO: DCP supports a large number of YUV formats. Support these.
 */
static const struct dcp_format {
	u32 drm;
	u32 dcp;
} dcp_formats[] = {
	{ DRM_FORMAT_XRGB8888, fourcc_code('A', 'R', 'G', 'B') },
	{ DRM_FORMAT_ARGB8888, fourcc_code('A', 'R', 'G', 'B') },
	{ DRM_FORMAT_XBGR8888, fourcc_code('A', 'B', 'G', 'R') },
	{ DRM_FORMAT_ABGR8888, fourcc_code('A', 'B', 'G', 'R') },
	{ DRM_FORMAT_BGRA8888, fourcc_code('B', 'G', 'R', 'A') },
	{ DRM_FORMAT_BGRX8888, fourcc_code('B', 'G', 'R', 'A') },
};

void dcp_link(struct platform_device *pdev, struct apple_drm_private *apple);
void dcp_swap(struct platform_device *pdev, struct drm_atomic_state *state);
bool dcp_is_initialized(struct platform_device *pdev);
void apple_crtc_vblank(struct apple_drm_private *apple);

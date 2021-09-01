#include <drm/drm_atomic.h>

struct apple_drm_private;

static const struct dcp_format {
	u32 drm;
	char dcp[4];
} dcp_formats[] = {
	{ DRM_FORMAT_XRGB8888, "BGRA" },
	{ DRM_FORMAT_ARGB8888, "BGRA" },
};

void dcp_link(struct platform_device *pdev, struct apple_drm_private *apple);
void dcp_swap(struct platform_device *pdev, struct drm_atomic_state *state);
bool dcp_is_initialized(struct platform_device *pdev);
void apple_crtc_vblank(struct apple_drm_private *apple);

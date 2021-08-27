#include <drm/drm_atomic.h>

struct apple_drm_private;

void dcp_link(struct platform_device *pdev, struct apple_drm_private *apple);
void dcp_swap(struct platform_device *pdev, struct drm_atomic_state *state);
bool dcp_is_initialized(struct platform_device *pdev);
void apple_crtc_vblank(struct apple_drm_private *apple);

#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <stdbool.h>

typedef struct {
    // DRM device handle (for GBM device creation only)
    int drm_fd;
    
    // GBM resources
    struct gbm_device *gbm_device;
    struct gbm_surface *gbm_surface;
    
    // Buffer management
    struct gbm_bo *current_bo;
    struct gbm_bo *next_bo;
    
    // Surface properties
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;
} display_ctx_t;

// Function declarations - GBM buffer management without DRM master
int drm_init(display_ctx_t *drm);
int drm_swap_buffers(display_ctx_t *drm);
void drm_cleanup(display_ctx_t *drm);

#endif // DRM_DISPLAY_H
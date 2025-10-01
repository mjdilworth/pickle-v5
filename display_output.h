/*
 * Display Output Module - DRM/KMS Direct Display Output
 * 
 * This module handles:
 * - DRM/KMS initialization and mode setting
 * - EGL surface creation for direct display output
 * - Display configuration (resolution, refresh rate)
 * - Frame presentation and vsync
 */

#ifndef DISPLAY_OUTPUT_H
#define DISPLAY_OUTPUT_H

#include <stdint.h>
#include <EGL/egl.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* Return codes */
#define DISPLAY_OUTPUT_OK          0
#define DISPLAY_OUTPUT_ERROR      -1
#define DISPLAY_OUTPUT_EAGAIN     -2

/* Forward declarations */
typedef struct display_output_ctx display_output_ctx_t;

/* Display mode information */
typedef struct {
    int width;
    int height;
    int refresh_rate;
    int interlaced;
    char name[32];
} display_mode_t;

/* Display configuration */
typedef struct {
    int preferred_width;      /* Preferred width (0 = auto) */
    int preferred_height;     /* Preferred height (0 = auto) */
    int preferred_refresh;    /* Preferred refresh rate (0 = auto) */
    int force_mode;          /* Force specific mode even if not preferred */
    int connector_id;        /* Specific connector ID (0 = auto) */
    int crtc_id;            /* Specific CRTC ID (0 = auto) */
    const char *device_path; /* DRM device path (NULL = auto) */
} display_config_t;

/* Display information */
typedef struct {
    int width;
    int height;
    int refresh_rate;
    int physical_width_mm;
    int physical_height_mm;
    char connector_name[32];
    char monitor_name[64];
} display_info_t;

/* API Functions */

/**
 * Create display output context
 * @return New context or NULL on error
 */
display_output_ctx_t *display_output_create(void);

/**
 * Configure display output
 * @param ctx Display context
 * @param width Display width (0 = auto)
 * @param height Display height (0 = auto)  
 * @param refresh_rate Refresh rate (0 = auto)
 * @return 0 on success, negative on error
 */
int display_output_configure(display_output_ctx_t *ctx, int width, int height, int refresh_rate);

/**
 * Set advanced display configuration
 * @param ctx Display context
 * @param config Advanced configuration options
 * @return 0 on success, negative on error
 */
int display_output_set_config(display_output_ctx_t *ctx, const display_config_t *config);

/**
 * Get current display information
 * @param ctx Display context
 * @param info Output display information
 * @return 0 on success, negative on error
 */
int display_output_get_info(display_output_ctx_t *ctx, display_info_t *info);

/**
 * Get list of available display modes
 * @param ctx Display context
 * @param modes Output array of modes
 * @param max_modes Maximum number of modes to return
 * @return Number of modes returned, negative on error
 */
int display_output_get_modes(display_output_ctx_t *ctx, display_mode_t *modes, int max_modes);

/**
 * Present frame to display (swap buffers)
 * @param ctx Display context
 * @return 0 on success, negative on error
 */
int display_output_present_frame(display_output_ctx_t *ctx);

/**
 * Wait for vertical blank (vsync)
 * @param ctx Display context
 * @return 0 on success, negative on error
 */
int display_output_wait_vblank(display_output_ctx_t *ctx);

/**
 * Get EGL display handle (for renderer integration)
 * @param ctx Display context
 * @return EGL display handle or EGL_NO_DISPLAY on error
 */
EGLDisplay display_output_get_egl_display(display_output_ctx_t *ctx);

/**
 * Get EGL surface handle (for renderer integration)
 * @param ctx Display context
 * @return EGL surface handle or EGL_NO_SURFACE on error
 */
EGLSurface display_output_get_egl_surface(display_output_ctx_t *ctx);

/**
 * Get GBM device handle
 * @param ctx Display context
 * @return GBM device handle or NULL on error
 */
struct gbm_device *display_output_get_gbm_device(display_output_ctx_t *ctx);

/**
 * Set display brightness (if supported)
 * @param ctx Display context
 * @param brightness Brightness level (0.0-1.0)
 * @return 0 on success, negative on error
 */
int display_output_set_brightness(display_output_ctx_t *ctx, float brightness);

/**
 * Enable/disable display power management
 * @param ctx Display context
 * @param enable 1 to enable DPMS, 0 to disable
 * @return 0 on success, negative on error
 */
int display_output_set_dpms(display_output_ctx_t *ctx, int enable);

/**
 * Get display statistics
 * @param ctx Display context
 * @param frames_presented Number of frames presented
 * @param vblank_count Vertical blank count
 * @param avg_present_time_us Average present time in microseconds
 */
void display_output_get_stats(display_output_ctx_t *ctx,
                             uint64_t *frames_presented,
                             uint64_t *vblank_count,
                             uint64_t *avg_present_time_us);

/**
 * Destroy display output and free resources
 * @param ctx Display context
 */
void display_output_destroy(display_output_ctx_t *ctx);

/* Utility functions */

/**
 * Check if DRM/KMS is available on this system
 * @return 1 if available, 0 if not
 */
int display_output_is_available(void);

/**
 * Get list of available DRM devices
 * @param devices Output array of device paths
 * @param max_devices Maximum number of devices to return
 * @return Number of devices found
 */
int display_output_enumerate_devices(char devices[][64], int max_devices);

/**
 * Get connector type name
 * @param connector_type DRM connector type
 * @return Human readable connector name
 */
const char *display_output_connector_type_name(uint32_t connector_type);

#endif /* DISPLAY_OUTPUT_H */
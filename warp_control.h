/*
 * Warp Control Module - Runtime Keystone Correction
 * 
 * This module handles:
 * - Interactive keystone/perspective correction
 * - Transformation matrix generation and updates
 * - Real-time parameter adjustment via keyboard input
 * - Corner-based and matrix-based warp controls
 */

#ifndef WARP_CONTROL_H
#define WARP_CONTROL_H

#include <stdint.h>
#include "gpu_renderer.h"

/* Return codes */
#define WARP_CONTROL_OK          0
#define WARP_CONTROL_ERROR      -1
#define WARP_CONTROL_UPDATED     1

/* Forward declarations */
typedef struct warp_control_ctx warp_control_ctx_t;

/* Control modes */
typedef enum {
    WARP_MODE_CORNERS,        /* Adjust individual corners */
    WARP_MODE_MATRIX,         /* Direct matrix manipulation */
    WARP_MODE_PERSPECTIVE,    /* Perspective correction */
    WARP_MODE_KEYSTONE        /* Keystone correction */
} warp_mode_t;

/* Corner points for perspective correction */
typedef struct {
    float top_left[2];        /* X, Y coordinates (-1.0 to 1.0) */
    float top_right[2];
    float bottom_left[2];
    float bottom_right[2];
} corner_points_t;

/* Warp parameters */
typedef struct {
    warp_mode_t mode;
    corner_points_t corners;
    float keystone_h;         /* Horizontal keystone (-1.0 to 1.0) */
    float keystone_v;         /* Vertical keystone (-1.0 to 1.0) */
    float rotation;           /* Rotation in degrees */
    float scale_x, scale_y;   /* Scale factors */
    float offset_x, offset_y; /* Position offset */
} warp_params_t;

/* Input configuration */
typedef struct {
    float step_size;          /* Adjustment step size */
    int enable_fine_control;  /* Enable fine adjustment mode */
    int enable_auto_save;     /* Auto-save parameters */
    const char *config_file;  /* Configuration file path */
} warp_input_config_t;

/* API Functions */

/**
 * Create warp control context
 * @return New context or NULL on error
 */
warp_control_ctx_t *warp_control_create(void);

/**
 * Configure warp control with renderer
 * @param ctx Warp control context
 * @param renderer_ctx GPU renderer context
 * @return 0 on success, negative on error
 */
int warp_control_configure(warp_control_ctx_t *ctx, gpu_renderer_ctx_t *renderer_ctx);

/**
 * Set input configuration
 * @param ctx Warp control context
 * @param config Input configuration
 * @return 0 on success, negative on error
 */
int warp_control_set_input_config(warp_control_ctx_t *ctx, const warp_input_config_t *config);

/**
 * Process input events (non-blocking)
 * @param ctx Warp control context
 * @return WARP_CONTROL_UPDATED if parameters changed, 0 if no change, negative on error
 */
int warp_control_process_input(warp_control_ctx_t *ctx);

/**
 * Set warp mode
 * @param ctx Warp control context
 * @param mode Warp mode
 * @return 0 on success, negative on error
 */
int warp_control_set_mode(warp_control_ctx_t *ctx, warp_mode_t mode);

/**
 * Get current warp parameters
 * @param ctx Warp control context
 * @param params Output warp parameters
 * @return 0 on success, negative on error
 */
int warp_control_get_params(warp_control_ctx_t *ctx, warp_params_t *params);

/**
 * Set warp parameters
 * @param ctx Warp control context
 * @param params Warp parameters
 * @return 0 on success, negative on error
 */
int warp_control_set_params(warp_control_ctx_t *ctx, const warp_params_t *params);

/**
 * Reset warp to identity (no transformation)
 * @param ctx Warp control context
 * @return 0 on success, negative on error
 */
int warp_control_reset(warp_control_ctx_t *ctx);

/**
 * Load warp parameters from file
 * @param ctx Warp control context
 * @param filename Configuration file path
 * @return 0 on success, negative on error
 */
int warp_control_load_config(warp_control_ctx_t *ctx, const char *filename);

/**
 * Save warp parameters to file
 * @param ctx Warp control context
 * @param filename Configuration file path
 * @return 0 on success, negative on error
 */
int warp_control_save_config(warp_control_ctx_t *ctx, const char *filename);

/**
 * Generate transformation matrix from current parameters
 * @param ctx Warp control context
 * @param matrix Output 4x4 matrix (column-major)
 * @return 0 on success, negative on error
 */
int warp_control_generate_matrix(warp_control_ctx_t *ctx, warp_matrix_t *matrix);

/**
 * Apply corner-based perspective correction
 * @param ctx Warp control context
 * @param corners Corner points
 * @return 0 on success, negative on error
 */
int warp_control_set_corners(warp_control_ctx_t *ctx, const corner_points_t *corners);

/**
 * Apply keystone correction
 * @param ctx Warp control context
 * @param horizontal Horizontal keystone (-1.0 to 1.0)
 * @param vertical Vertical keystone (-1.0 to 1.0)
 * @return 0 on success, negative on error
 */
int warp_control_set_keystone(warp_control_ctx_t *ctx, float horizontal, float vertical);

/**
 * Get control help text
 * @return Help text string
 */
const char *warp_control_get_help(void);

/**
 * Destroy warp control and free resources
 * @param ctx Warp control context
 */
void warp_control_destroy(warp_control_ctx_t *ctx);

/* Utility functions */

/**
 * Convert corner points to transformation matrix
 * @param corners Corner points
 * @param matrix Output matrix
 * @return 0 on success, negative on error
 */
int warp_control_corners_to_matrix(const corner_points_t *corners, warp_matrix_t *matrix);

/**
 * Convert keystone parameters to transformation matrix
 * @param h_keystone Horizontal keystone
 * @param v_keystone Vertical keystone
 * @param matrix Output matrix
 * @return 0 on success, negative on error
 */
int warp_control_keystone_to_matrix(float h_keystone, float v_keystone, warp_matrix_t *matrix);

#endif /* WARP_CONTROL_H */
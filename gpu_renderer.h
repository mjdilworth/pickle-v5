/*
 * GPU Renderer Module - OpenGL ES 3.2 with DMABUF Import
 * 
 * This module handles:
 * - OpenGL ES 3.2 context creation and management
 * - DMABUF import as OpenGL textures (zero-copy from decoder)
 * - YUV→RGB conversion using shaders
 * - Keystone correction/warping with transformation matrices
 * - Frame rendering and presentation
 */

#ifndef GPU_RENDERER_H
#define GPU_RENDERER_H

#include <stdint.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "hw_decoder.h"
#include "display_output.h"

/* OpenGL extension function pointer types */
#ifndef GL_OES_EGL_image
typedef void* GLeglImageOES;
typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, GLeglImageOES image);
#endif

/* Return codes */
#define GPU_RENDERER_OK          0
#define GPU_RENDERER_ERROR      -1
#define GPU_RENDERER_EAGAIN     -2

/* Forward declarations */
typedef struct gpu_renderer_ctx gpu_renderer_ctx_t;

/* Warp transformation matrix (4x4) */
typedef struct {
    float matrix[16];  /* Column-major order for OpenGL */
    int dirty;         /* 1 if matrix needs to be updated */
} warp_matrix_t;

/* Renderer configuration */
typedef struct {
    int enable_vsync;          /* Enable vertical sync */
    int msaa_samples;          /* MSAA samples (0 = disabled) */
    int debug_output;          /* Enable debug output */
    float brightness;          /* Brightness adjustment (0.0-2.0, 1.0 = normal) */
    float contrast;            /* Contrast adjustment (0.0-2.0, 1.0 = normal) */
    float saturation;          /* Saturation adjustment (0.0-2.0, 1.0 = normal) */
} renderer_config_t;

/* Shader types */
typedef enum {
    SHADER_VERTEX,
    SHADER_FRAGMENT
} shader_type_t;

/* Texture format info */
typedef struct {
    GLenum internal_format;
    GLenum format;
    GLenum type;
    int width;
    int height;
} texture_info_t;

/* API Functions */

/**
 * Create GPU renderer context
 * @return New context or NULL on error
 */
gpu_renderer_ctx_t *gpu_renderer_create(void);

/**
 * Configure renderer with display and video parameters
 * @param ctx Renderer context
 * @param display_ctx Display context for EGL surface creation
 * @param video_width Width of input video
 * @param video_height Height of input video
 * @return 0 on success, negative on error
 */
int gpu_renderer_configure(gpu_renderer_ctx_t *ctx, 
                          display_output_ctx_t *display_ctx,
                          int video_width, int video_height);

/**
 * Set renderer configuration (optional)
 * @param ctx Renderer context
 * @param config Configuration options
 * @return 0 on success, negative on error
 */
int gpu_renderer_set_config(gpu_renderer_ctx_t *ctx, const renderer_config_t *config);

/**
 * Import DMABUF as OpenGL texture (zero-copy)
 * @param ctx Renderer context
 * @param dmabuf_fd DMABUF file descriptor
 * @param width Texture width
 * @param height Texture height
 * @param format V4L2 pixel format
 * @param texture_out Output texture ID
 * @return 0 on success, negative on error
 */
int gpu_renderer_import_dmabuf(gpu_renderer_ctx_t *ctx,
                              int dmabuf_fd, int width, int height,
                              uint32_t format, GLuint *texture_out);

/**
 * Render frame with current warp matrix
 * @param ctx Renderer context
 * @param frame Decoded frame with DMABUF handles
 * @return 0 on success, negative on error
 */
int gpu_renderer_render_frame(gpu_renderer_ctx_t *ctx, const decoded_frame_t *frame);

/**
 * Set warp transformation matrix
 * @param ctx Renderer context
 * @param matrix 4x4 transformation matrix (column-major)
 * @return 0 on success, negative on error
 */
int gpu_renderer_set_warp_matrix(gpu_renderer_ctx_t *ctx, const warp_matrix_t *matrix);

/**
 * Get current warp transformation matrix
 * @param ctx Renderer context
 * @param matrix Output matrix
 * @return 0 on success, negative on error
 */
int gpu_renderer_get_warp_matrix(gpu_renderer_ctx_t *ctx, warp_matrix_t *matrix);

/**
 * Load and compile shader from source
 * @param ctx Renderer context
 * @param type Shader type (vertex/fragment)
 * @param source Shader source code
 * @param shader_out Output shader ID
 * @return 0 on success, negative on error
 */
int gpu_renderer_compile_shader(gpu_renderer_ctx_t *ctx,
                               shader_type_t type, const char *source,
                               GLuint *shader_out);

/**
 * Create shader program from vertex and fragment shaders
 * @param ctx Renderer context
 * @param vertex_shader Vertex shader ID
 * @param fragment_shader Fragment shader ID
 * @param program_out Output program ID
 * @return 0 on success, negative on error
 */
int gpu_renderer_create_program(gpu_renderer_ctx_t *ctx,
                               GLuint vertex_shader, GLuint fragment_shader,
                               GLuint *program_out);

/**
 * Set color adjustment parameters
 * @param ctx Renderer context
 * @param brightness Brightness (0.0-2.0, 1.0 = normal)
 * @param contrast Contrast (0.0-2.0, 1.0 = normal)
 * @param saturation Saturation (0.0-2.0, 1.0 = normal)
 * @return 0 on success, negative on error
 */
int gpu_renderer_set_color_adjustments(gpu_renderer_ctx_t *ctx,
                                      float brightness, float contrast, float saturation);

/**
 * Get renderer statistics
 * @param ctx Renderer context
 * @param frames_rendered Number of frames rendered
 * @param avg_render_time_us Average render time in microseconds
 * @param gpu_memory_used GPU memory usage in bytes
 */
void gpu_renderer_get_stats(gpu_renderer_ctx_t *ctx,
                           uint64_t *frames_rendered,
                           uint64_t *avg_render_time_us,
                           uint64_t *gpu_memory_used);

/**
 * Resize renderer viewport
 * @param ctx Renderer context
 * @param width New width
 * @param height New height
 * @return 0 on success, negative on error
 */
int gpu_renderer_resize(gpu_renderer_ctx_t *ctx, int width, int height);

/**
 * Destroy renderer and free resources
 * @param ctx Renderer context
 */
void gpu_renderer_destroy(gpu_renderer_ctx_t *ctx);

/* Utility functions */

/**
 * Check if required OpenGL extensions are available
 * @return 1 if supported, 0 if not
 */
int gpu_renderer_check_extensions(void);

/**
 * Get OpenGL renderer information
 * @param vendor Output vendor string
 * @param renderer Output renderer string  
 * @param version Output version string
 */
void gpu_renderer_get_info(const char **vendor, const char **renderer, const char **version);

/* Built-in shader sources */
extern const char *gpu_renderer_vertex_shader_yuv420;
extern const char *gpu_renderer_fragment_shader_yuv420;
extern const char *gpu_renderer_vertex_shader_nv12;
extern const char *gpu_renderer_fragment_shader_nv12;

#endif /* GPU_RENDERER_H */
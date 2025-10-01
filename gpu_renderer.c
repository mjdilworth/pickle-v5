/*
 * GPU Renderer Implementation - OpenGL ES 3.2 with DMABUF Import
 * 
 * Implements zero-copy video rendering using DMABUF import to OpenGL textures.
 * Supports YUV→RGB conversion and real-time keystone correction.
 */

#include "gpu_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <libavutil/pixfmt.h>

/* Ensure we have all necessary EGL extension definitions */
#ifndef EGL_EXT_image_dma_buf_import
#define EGL_EXT_image_dma_buf_import 1
#endif

/* EGL extensions for DMABUF import */
#ifndef EGL_EXT_image_dma_buf_import
#define EGL_LINUX_DMA_BUF_EXT                0x3270
#define EGL_LINUX_DRM_FOURCC_EXT             0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT            0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT        0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT         0x3274
#endif

/* OpenGL ES extensions */
#ifndef GL_OES_EGL_image_external
#define GL_TEXTURE_EXTERNAL_OES              0x8D65
#endif

/* Default configuration */
#define DEFAULT_BRIGHTNESS  1.0f
#define DEFAULT_CONTRAST    1.0f
#define DEFAULT_SATURATION  1.0f

/* Internal renderer context */
struct gpu_renderer_ctx {
    /* EGL context */
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    
    /* OpenGL resources */
    GLuint shader_program_yuv420;
    GLuint shader_program_nv12;
    GLuint current_program;
    GLuint vertex_buffer;
    GLuint vertex_array;
    
    /* Uniform locations */
    GLint u_matrix;
    GLint u_tex_y, u_tex_u, u_tex_v;  /* YUV420 textures */
    GLint u_tex_nv12_y, u_tex_nv12_uv; /* NV12 textures */
    GLint u_brightness, u_contrast, u_saturation;
    
    /* Current state */
    warp_matrix_t warp_matrix;
    renderer_config_t config;
    int video_width, video_height;
    int display_width, display_height;
    
    /* Statistics */
    uint64_t frames_rendered;
    uint64_t total_render_time_us;
    struct timeval last_frame_time;
    
    /* Extension function pointers */
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
};

/* Vertex data for fullscreen quad with texture coordinates */
static const GLfloat quad_vertices[] = {
    /* Position    Texture coords */
    -1.0f, -1.0f,  0.0f, 1.0f,  /* Bottom left */
     1.0f, -1.0f,  1.0f, 1.0f,  /* Bottom right */
     1.0f,  1.0f,  1.0f, 0.0f,  /* Top right */
    -1.0f,  1.0f,  0.0f, 0.0f   /* Top left */
};

static const GLushort quad_indices[] = {
    0, 1, 2,  /* First triangle */
    2, 3, 0   /* Second triangle */
};

/* YUV420 Vertex Shader */
const char *gpu_renderer_vertex_shader_yuv420 = 
    "#version 310 es\n"
    "precision highp float;\n"
    "\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "\n"
    "uniform mat4 u_matrix;\n"
    "\n"
    "out vec2 v_texcoord;\n"
    "\n"
    "void main() {\n"
    "    gl_Position = u_matrix * vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

/* YUV420 Fragment Shader with color correction */
const char *gpu_renderer_fragment_shader_yuv420 = 
    "#version 310 es\n"
    "precision highp float;\n"
    "\n"
    "in vec2 v_texcoord;\n"
    "out vec4 fragColor;\n"
    "\n"
    "uniform sampler2D u_tex_y;\n"
    "uniform sampler2D u_tex_u;\n"
    "uniform sampler2D u_tex_v;\n"
    "uniform float u_brightness;\n"
    "uniform float u_contrast;\n"
    "uniform float u_saturation;\n"
    "\n"
    "void main() {\n"
    "    float y = texture(u_tex_y, v_texcoord).r;\n"
    "    float u = texture(u_tex_u, v_texcoord).r - 0.5;\n"
    "    float v = texture(u_tex_v, v_texcoord).r - 0.5;\n"
    "    \n"
    "    /* YUV to RGB conversion (BT.709) */\n"
    "    vec3 rgb;\n"
    "    rgb.r = y + 1.5748 * v;\n"
    "    rgb.g = y - 0.1873 * u - 0.4681 * v;\n"
    "    rgb.b = y + 1.8556 * u;\n"
    "    \n"
    "    /* Color adjustments */\n"
    "    rgb = (rgb - 0.5) * u_contrast + 0.5; /* Contrast */\n"
    "    rgb += u_brightness - 1.0;            /* Brightness */\n"
    "    \n"
    "    /* Saturation */\n"
    "    float gray = dot(rgb, vec3(0.299, 0.587, 0.114));\n"
    "    rgb = mix(vec3(gray), rgb, u_saturation);\n"
    "    \n"
    "    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

/* NV12 shaders (similar structure, different texture sampling) */
const char *gpu_renderer_vertex_shader_nv12 = 
    "#version 310 es\n"
    "precision highp float;\n"
    "\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "\n"
    "uniform mat4 u_matrix;\n"
    "\n"
    "out vec2 v_texcoord;\n"
    "\n"
    "void main() {\n"
    "    gl_Position = u_matrix * vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

const char *gpu_renderer_fragment_shader_nv12 = 
    "#version 310 es\n"
    "precision highp float;\n"
    "\n"
    "in vec2 v_texcoord;\n"
    "out vec4 fragColor;\n"
    "\n"
    "uniform sampler2D u_tex_nv12_y;\n"
    "uniform sampler2D u_tex_nv12_uv;\n"
    "uniform float u_brightness;\n"
    "uniform float u_contrast;\n"
    "uniform float u_saturation;\n"
    "\n"
    "void main() {\n"
    "    float y = texture(u_tex_nv12_y, v_texcoord).r;\n"
    "    vec2 uv = texture(u_tex_nv12_uv, v_texcoord).rg - 0.5;\n"
    "    \n"
    "    /* YUV to RGB conversion (BT.709) */\n"
    "    vec3 rgb;\n"
    "    rgb.r = y + 1.5748 * uv.y;\n"
    "    rgb.g = y - 0.1873 * uv.x - 0.4681 * uv.y;\n"
    "    rgb.b = y + 1.8556 * uv.x;\n"
    "    \n"
    "    /* Color adjustments */\n"
    "    rgb = (rgb - 0.5) * u_contrast + 0.5;\n"
    "    rgb += u_brightness - 1.0;\n"
    "    \n"
    "    float gray = dot(rgb, vec3(0.299, 0.587, 0.114));\n"
    "    rgb = mix(vec3(gray), rgb, u_saturation);\n"
    "    \n"
    "    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

/**
 * Check for OpenGL errors and print debug info
 */
static void check_gl_error(const char *operation) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error in %s: 0x%x\n", operation, error);
    }
}

/**
 * Create identity matrix
 */
static void matrix_identity(float *matrix) {
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

/**
 * Load EGL extension function pointers
 */
static int load_egl_extensions(gpu_renderer_ctx_t *ctx) {
    ctx->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    ctx->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    ctx->glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
        
    if (!ctx->eglCreateImageKHR || !ctx->eglDestroyImageKHR || 
        !ctx->glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "Required EGL extensions not available\n");
        return GPU_RENDERER_ERROR;
    }
    
    return GPU_RENDERER_OK;
}

/**
 * Create GPU renderer context
 */
gpu_renderer_ctx_t *gpu_renderer_create(void) {
    gpu_renderer_ctx_t *ctx = calloc(1, sizeof(gpu_renderer_ctx_t));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate GPU renderer context\n");
        return NULL;
    }
    
    /* Set default configuration */
    ctx->config.brightness = DEFAULT_BRIGHTNESS;
    ctx->config.contrast = DEFAULT_CONTRAST;
    ctx->config.saturation = DEFAULT_SATURATION;
    ctx->config.enable_vsync = 1;
    
    /* Initialize warp matrix to identity */
    matrix_identity(ctx->warp_matrix.matrix);
    
    return ctx;
}

/**
 * Compile shader from source
 */
int gpu_renderer_compile_shader(gpu_renderer_ctx_t *ctx __attribute__((unused)),
                               shader_type_t type, const char *source,
                               GLuint *shader_out) {
    GLenum shader_type_gl = (type == SHADER_VERTEX) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    GLuint shader = glCreateShader(shader_type_gl);
    GLint compiled;
    
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info_log = malloc(info_len);
            glGetShaderInfoLog(shader, info_len, NULL, info_log);
            fprintf(stderr, "Shader compile error:\n%s\n", info_log);
            free(info_log);
        }
        glDeleteShader(shader);
        return GPU_RENDERER_ERROR;
    }
    
    *shader_out = shader;
    return GPU_RENDERER_OK;
}

/**
 * Create shader program
 */
int gpu_renderer_create_program(gpu_renderer_ctx_t *ctx __attribute__((unused)),
                               GLuint vertex_shader, GLuint fragment_shader,
                               GLuint *program_out) {
    GLuint program = glCreateProgram();
    GLint linked;
    
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    
    /* Bind attribute locations */
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    
    glLinkProgram(program);
    
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info_log = malloc(info_len);
            glGetProgramInfoLog(program, info_len, NULL, info_log);
            fprintf(stderr, "Program link error:\n%s\n", info_log);
            free(info_log);
        }
        glDeleteProgram(program);
        return GPU_RENDERER_ERROR;
    }
    
    *program_out = program;
    return GPU_RENDERER_OK;
}

/**
 * Setup shaders and uniforms
 */
static int setup_shaders(gpu_renderer_ctx_t *ctx) {
    GLuint vs_yuv420, fs_yuv420, vs_nv12, fs_nv12;
    int ret;
    
    /* Compile YUV420 shaders */
    ret = gpu_renderer_compile_shader(ctx, SHADER_VERTEX, 
                                     gpu_renderer_vertex_shader_yuv420, &vs_yuv420);
    if (ret < 0) return ret;
    
    ret = gpu_renderer_compile_shader(ctx, SHADER_FRAGMENT, 
                                     gpu_renderer_fragment_shader_yuv420, &fs_yuv420);
    if (ret < 0) return ret;
    
    ret = gpu_renderer_create_program(ctx, vs_yuv420, fs_yuv420, &ctx->shader_program_yuv420);
    if (ret < 0) return ret;
    
    /* Compile NV12 shaders */
    ret = gpu_renderer_compile_shader(ctx, SHADER_VERTEX,
                                     gpu_renderer_vertex_shader_nv12, &vs_nv12);
    if (ret < 0) return ret;
    
    ret = gpu_renderer_compile_shader(ctx, SHADER_FRAGMENT,
                                     gpu_renderer_fragment_shader_nv12, &fs_nv12);
    if (ret < 0) return ret;
    
    ret = gpu_renderer_create_program(ctx, vs_nv12, fs_nv12, &ctx->shader_program_nv12);
    if (ret < 0) return ret;
    
    /* Clean up individual shaders */
    glDeleteShader(vs_yuv420);
    glDeleteShader(fs_yuv420);
    glDeleteShader(vs_nv12);
    glDeleteShader(fs_nv12);
    
    printf("GPU shaders compiled successfully\n");
    return GPU_RENDERER_OK;
}

/**
 * Setup vertex buffers and geometry
 */
static int setup_geometry(gpu_renderer_ctx_t *ctx) {
    GLuint index_buffer;
    
    /* Generate vertex array object */
    glGenVertexArrays(1, &ctx->vertex_array);
    glBindVertexArray(ctx->vertex_array);
    
    /* Generate and setup vertex buffer */
    glGenBuffers(1, &ctx->vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
    
    /* Generate and setup index buffer */
    glGenBuffers(1, &index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STATIC_DRAW);
    
    /* Setup vertex attributes */
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(0);
    
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 
                         (void*)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
    check_gl_error("setup_geometry");
    
    return GPU_RENDERER_OK;
}

/**
 * Configure renderer with display and video parameters
 */
int gpu_renderer_configure(gpu_renderer_ctx_t *ctx, 
                          display_output_ctx_t *display_ctx,
                          int video_width, int video_height) {
    int ret;
    
    if (!ctx || !display_ctx) {
        return GPU_RENDERER_ERROR;
    }
    
    ctx->video_width = video_width;
    ctx->video_height = video_height;
    
    /* Get EGL display from display output module */
    ctx->egl_display = display_output_get_egl_display(display_ctx);
    if (ctx->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return GPU_RENDERER_ERROR;
    }
    
    /* Get EGL surface */
    ctx->egl_surface = display_output_get_egl_surface(display_ctx);
    if (ctx->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to get EGL surface\n");
        return GPU_RENDERER_ERROR;
    }
    
    /* Load EGL extensions */
    ret = load_egl_extensions(ctx);
    if (ret < 0) {
        return ret;
    }
    
    /* Setup OpenGL resources */
    ret = setup_shaders(ctx);
    if (ret < 0) {
        return ret;
    }
    
    ret = setup_geometry(ctx);
    if (ret < 0) {
        return ret;
    }
    
    /* Get uniform locations for YUV420 shader */
    glUseProgram(ctx->shader_program_yuv420);
    ctx->u_matrix = glGetUniformLocation(ctx->shader_program_yuv420, "u_matrix");
    ctx->u_tex_y = glGetUniformLocation(ctx->shader_program_yuv420, "u_tex_y");
    ctx->u_tex_u = glGetUniformLocation(ctx->shader_program_yuv420, "u_tex_u");
    ctx->u_tex_v = glGetUniformLocation(ctx->shader_program_yuv420, "u_tex_v");
    ctx->u_brightness = glGetUniformLocation(ctx->shader_program_yuv420, "u_brightness");
    ctx->u_contrast = glGetUniformLocation(ctx->shader_program_yuv420, "u_contrast");
    ctx->u_saturation = glGetUniformLocation(ctx->shader_program_yuv420, "u_saturation");
    
    /* Setup OpenGL state */
    glViewport(0, 0, 1920, 1080);  /* Will be updated by resize */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    
    if (ctx->config.enable_vsync) {
        eglSwapInterval(ctx->egl_display, 1);
    }
    
    check_gl_error("gpu_renderer_configure");
    printf("GPU renderer configured: %dx%d video, OpenGL ES ready\n", 
           video_width, video_height);
    
    return GPU_RENDERER_OK;
}

/**
 * Import DMABUF as OpenGL texture
 */
int gpu_renderer_import_dmabuf(gpu_renderer_ctx_t *ctx,
                              int dmabuf_fd, int width, int height,
                              uint32_t format, GLuint *texture_out) {
    EGLImageKHR egl_image;
    GLuint texture;
    
    if (!ctx || dmabuf_fd < 0 || !texture_out) {
        return GPU_RENDERER_ERROR;
    }
    
    /* EGL attributes for DMABUF import */
    EGLint attribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, format,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, width,  /* Simplified - should be actual stride */
        EGL_NONE
    };
    
    /* Create EGL image from DMABUF */
    egl_image = ctx->eglCreateImageKHR(ctx->egl_display, EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (egl_image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "Failed to create EGL image from DMABUF\n");
        return GPU_RENDERER_ERROR;
    }
    
    /* Create OpenGL texture from EGL image */
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_image);
    
    /* Set texture parameters */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    /* Clean up EGL image */
    ctx->eglDestroyImageKHR(ctx->egl_display, egl_image);
    
    *texture_out = texture;
    check_gl_error("import_dmabuf");
    
    return GPU_RENDERER_OK;
}

/**
 * Render frame with current warp matrix
 */
int gpu_renderer_render_frame(gpu_renderer_ctx_t *ctx, const decoded_frame_t *frame) {
    struct timeval start_time, end_time;
    GLuint textures[3] = {0};
    int ret;
    
    if (!ctx || !frame) {
        return GPU_RENDERER_ERROR;
    }
    
    gettimeofday(&start_time, NULL);
    
    /* Clear framebuffer */
    glClear(GL_COLOR_BUFFER_BIT);
    
    /* Check for test pattern mode (no valid DMABUF) */
    if (frame->dmabuf_fd[0] < 0) {
        /* Render a simple test pattern */
        glUseProgram(0); // Use fixed pipeline for simple pattern
        
        /* Set viewport */
        glViewport(0, 0, ctx->video_width, ctx->video_height);
        
        /* Create a simple animated color pattern */
        static float color_cycle = 0.0f;
        color_cycle += 0.02f;
        if (color_cycle > 1.0f) color_cycle = 0.0f;
        
        float red = 0.5f + 0.5f * sin(color_cycle * 6.28f);
        float green = 0.5f + 0.5f * sin(color_cycle * 6.28f + 2.09f);
        float blue = 0.5f + 0.5f * sin(color_cycle * 6.28f + 4.18f);
        
        glClearColor(red, green, blue, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        /* Swap buffers */
        eglSwapBuffers(ctx->egl_display, ctx->egl_surface);
        
        return GPU_RENDERER_OK;
    }
    
    /* Import frame textures from DMABUF */
    if (frame->format == AV_PIX_FMT_NV12) {
        /* NV12: Y plane + interleaved UV plane */
        ret = gpu_renderer_import_dmabuf(ctx, frame->dmabuf_fd[0], 
                                        frame->width, frame->height,
                                        frame->format, &textures[0]);
        if (ret < 0) return ret;
        
        /* Use NV12 shader program */
        glUseProgram(ctx->shader_program_nv12);
        ctx->current_program = ctx->shader_program_nv12;
        
        /* Bind Y texture */
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        glUniform1i(glGetUniformLocation(ctx->current_program, "u_tex_nv12_y"), 0);
        
        /* TODO: Bind UV texture when multi-plane DMABUF is properly supported */
        
    } else {
        /* YUV420: separate Y, U, V planes */
        ret = gpu_renderer_import_dmabuf(ctx, frame->dmabuf_fd[0],
                                        frame->width, frame->height,
                                        frame->format, &textures[0]);
        if (ret < 0) return ret;
        
        /* Use YUV420 shader program */
        glUseProgram(ctx->shader_program_yuv420);
        ctx->current_program = ctx->shader_program_yuv420;
        
        /* Bind Y texture */
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        glUniform1i(ctx->u_tex_y, 0);
        
        /* TODO: Import and bind U, V planes when multi-plane DMABUF is supported */
    }
    
    /* Update uniforms */
    glUniformMatrix4fv(ctx->u_matrix, 1, GL_FALSE, ctx->warp_matrix.matrix);
    glUniform1f(ctx->u_brightness, ctx->config.brightness);
    glUniform1f(ctx->u_contrast, ctx->config.contrast);
    glUniform1f(ctx->u_saturation, ctx->config.saturation);
    
    /* Render quad */
    glBindVertexArray(ctx->vertex_array);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
    
    /* Clean up textures */
    for (int i = 0; i < 3; i++) {
        if (textures[i]) {
            glDeleteTextures(1, &textures[i]);
        }
    }
    
    /* Present frame */
    eglSwapBuffers(ctx->egl_display, ctx->egl_surface);
    
    /* Update statistics */
    gettimeofday(&end_time, NULL);
    uint64_t render_time = (end_time.tv_sec - start_time.tv_sec) * 1000000LL +
                          (end_time.tv_usec - start_time.tv_usec);
    
    ctx->frames_rendered++;
    ctx->total_render_time_us += render_time;
    ctx->last_frame_time = end_time;
    
    check_gl_error("render_frame");
    return GPU_RENDERER_OK;
}

/**
 * Set warp transformation matrix
 */
int gpu_renderer_set_warp_matrix(gpu_renderer_ctx_t *ctx, const warp_matrix_t *matrix) {
    if (!ctx || !matrix) {
        return GPU_RENDERER_ERROR;
    }
    
    memcpy(&ctx->warp_matrix, matrix, sizeof(warp_matrix_t));
    ctx->warp_matrix.dirty = 0;  /* Mark as clean */
    
    return GPU_RENDERER_OK;
}

/**
 * Get current warp transformation matrix
 */
int gpu_renderer_get_warp_matrix(gpu_renderer_ctx_t *ctx, warp_matrix_t *matrix) {
    if (!ctx || !matrix) {
        return GPU_RENDERER_ERROR;
    }
    
    memcpy(matrix, &ctx->warp_matrix, sizeof(warp_matrix_t));
    return GPU_RENDERER_OK;
}

/**
 * Check if required extensions are available
 */
int gpu_renderer_check_extensions(void) {
    const char *egl_extensions = eglQueryString(eglGetDisplay(EGL_DEFAULT_DISPLAY), EGL_EXTENSIONS);
    const char *gl_extensions = (const char*)glGetString(GL_EXTENSIONS);
    
    if (!strstr(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr, "EGL_EXT_image_dma_buf_import not supported\n");
        return 0;
    }
    
    if (!strstr(gl_extensions, "GL_OES_EGL_image_external")) {
        fprintf(stderr, "GL_OES_EGL_image_external not supported\n");
        return 0;
    }
    
    return 1;
}

/**
 * Destroy renderer and free resources
 */
void gpu_renderer_destroy(gpu_renderer_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    
    /* Clean up OpenGL resources */
    if (ctx->shader_program_yuv420) {
        glDeleteProgram(ctx->shader_program_yuv420);
    }
    if (ctx->shader_program_nv12) {
        glDeleteProgram(ctx->shader_program_nv12);
    }
    if (ctx->vertex_buffer) {
        glDeleteBuffers(1, &ctx->vertex_buffer);
    }
    if (ctx->vertex_array) {
        glDeleteVertexArrays(1, &ctx->vertex_array);
    }
    
    free(ctx);
}
/*
 * Display Output Implementation - DRM/KMS Direct Display
 * 
 * Implements direct display output using DRM/KMS for zero-copy presentation.
 * Integrates with EGL for GPU rendering pipeline.
 * Uses robust drm_display module for improved DRM/GBM handling.
 */

#define _GNU_SOURCE
#include "display_output.h"
#include "drm_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <EGL/eglext.h>

/* Fallback for O_CLOEXEC if not defined */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* Default DRM device path */
#define DEFAULT_DRM_DEVICE "/dev/dri/card1"

/* EGL error reporting helper */
static const char* egl_error_string(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "Unknown EGL error";
    }
}

/* Internal display context */
struct display_output_ctx {
    /* DRM/GBM context - using robust drm_display module */
    display_ctx_t drm_ctx;
    
    /* Configuration */
    display_config_t config;
    display_info_t info;
    
    /* EGL resources */
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    
    /* State */
    int configured;
    
    /* Statistics */
    uint64_t frames_presented;
    uint64_t vblank_count;
    uint64_t total_present_time_us;
    struct timeval last_present_time;
};



/**
 * Initialize EGL with DRM surface from user-provided working implementation
 */
static int init_egl_with_drm_surface(display_output_ctx_t *ctx) {
    EGLint major, minor;
    EGLint config_count;
    
    /* Get EGL display from DRM device using user's working implementation */
    ctx->egl_display = eglGetDisplay((EGLNativeDisplayType)ctx->drm_ctx.gbm_device);
    if (ctx->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Initialize EGL */
    if (!eglInitialize(ctx->egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    printf("EGL %d.%d initialized with DRM surface\n", major, minor);
    
    /* Bind OpenGL ES API */
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "Failed to bind OpenGL ES API\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Multiple EGL config approaches for maximum compatibility */
    
    /* Approach 1: Exact format match */
    EGLint exact_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NATIVE_VISUAL_ID, GBM_FORMAT_XRGB8888,
        EGL_NONE
    };
    
    printf("Trying EGL config approach 1: Exact XRGB8888 format match...\n");
    if (eglChooseConfig(ctx->egl_display, exact_attribs, &ctx->egl_config, 1, &config_count) &&
        config_count > 0) {
        printf("✓ Using exact XRGB8888 format match\n");
    } else {
        printf("✗ Exact match failed, trying approach 2: ARGB8888...\n");
        
        /* Approach 2: Try ARGB8888 */
        EGLint argb_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NATIVE_VISUAL_ID, GBM_FORMAT_ARGB8888,
            EGL_NONE
        };
        
        if (eglChooseConfig(ctx->egl_display, argb_attribs, &ctx->egl_config, 1, &config_count) &&
            config_count > 0) {
            printf("✓ Using ARGB8888 format match\n");
        } else {
            printf("✗ ARGB8888 failed, trying approach 3: Generic config...\n");
            
            /* Approach 3: Generic config without format constraint */
            EGLint generic_attribs[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_NONE
            };
            
            if (eglChooseConfig(ctx->egl_display, generic_attribs, &ctx->egl_config, 1, &config_count) &&
                config_count > 0) {
                printf("✓ Using generic EGL config\n");
            } else {
                fprintf(stderr, "✗ All EGL config approaches failed\n");
                return DISPLAY_OUTPUT_ERROR;
            }
        }
    }
    
    /* Debug: Print chosen config details */
    EGLint red, green, blue, alpha, visual_id;
    eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_RED_SIZE, &red);
    eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_BLUE_SIZE, &blue);
    eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_ALPHA_SIZE, &alpha);
    eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_NATIVE_VISUAL_ID, &visual_id);
    
    printf("Chosen EGL config: R%dG%dB%dA%d, Visual ID: 0x%x (GBM format: 0x%x)\n",
           red, green, blue, alpha, visual_id, GBM_FORMAT_XRGB8888);
    
    /* Create EGL context */
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    
    ctx->egl_context = eglCreateContext(ctx->egl_display, ctx->egl_config,
                                        EGL_NO_CONTEXT, context_attribs);
    if (ctx->egl_context == EGL_NO_CONTEXT) {
        EGLint egl_error = eglGetError();
        fprintf(stderr, "Failed to create EGL context: %s (0x%04x)\n",
                egl_error_string(egl_error), egl_error);
        return DISPLAY_OUTPUT_ERROR;
    }
    
    printf("✓ EGL context created successfully\n");
    
    /* Create EGL window surface using DRM GBM surface */
    printf("Creating EGL surface with GBM surface %p...\n", ctx->drm_ctx.gbm_surface);
    ctx->egl_surface = eglCreateWindowSurface(ctx->egl_display, ctx->egl_config,
                                              (EGLNativeWindowType)ctx->drm_ctx.gbm_surface, NULL);
    if (ctx->egl_surface == EGL_NO_SURFACE) {
        EGLint egl_error = eglGetError();
        fprintf(stderr, "Failed to create EGL window surface: %s (0x%04x)\n",
                egl_error_string(egl_error), egl_error);
        
        /* Additional debugging */
        printf("Debug info:\n");
        printf("  GBM surface: %p\n", ctx->drm_ctx.gbm_surface);
        printf("  EGL display: %p\n", ctx->egl_display);
        printf("  EGL config: %p\n", ctx->egl_config);
        
        if (egl_error == EGL_BAD_MATCH) {
            printf("EGL_BAD_MATCH indicates format mismatch between EGL config and GBM surface\n");
            
            /* Try recreating GBM surface with format that matches EGL config */
            EGLint visual_id;
            if (eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_NATIVE_VISUAL_ID, &visual_id)) {
                printf("Trying to recreate GBM surface with EGL visual format 0x%x\n", visual_id);
                
                /* Backup current GBM surface and try with EGL format */
                struct gbm_surface *old_surface = ctx->drm_ctx.gbm_surface;
                ctx->drm_ctx.gbm_surface = gbm_surface_create(ctx->drm_ctx.gbm_device,
                                                              ctx->drm_ctx.width, ctx->drm_ctx.height,
                                                              (uint32_t)visual_id,
                                                              GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
                
                if (ctx->drm_ctx.gbm_surface) {
                    printf("✓ Recreated GBM surface with EGL format\n");
                    gbm_surface_destroy(old_surface);
                    
                    /* Try EGL surface creation again */
                    ctx->egl_surface = eglCreateWindowSurface(ctx->egl_display, ctx->egl_config,
                                                              (EGLNativeWindowType)ctx->drm_ctx.gbm_surface, NULL);
                    if (ctx->egl_surface != EGL_NO_SURFACE) {
                        printf("✓ EGL surface created successfully with format conversion\n");
                        goto surface_success;
                    }
                } else {
                    /* Restore old surface */
                    ctx->drm_ctx.gbm_surface = old_surface;
                }
            }
        }
        
        return DISPLAY_OUTPUT_ERROR;
    }
    
surface_success:
    
    printf("✓ EGL window surface created successfully with DRM integration\n");
    
    /* Make context current */
    if (!eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    return DISPLAY_OUTPUT_OK;
}

/**
 * Create display output context
 */
display_output_ctx_t *display_output_create(void) {
    display_output_ctx_t *ctx = calloc(1, sizeof(display_output_ctx_t));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate display output context\n");
        return NULL;
    }
    
    return ctx;
}

/**
 * Configure display output
 */
int display_output_configure(display_output_ctx_t *ctx, int width, int height, int refresh_rate) {
    int ret;
    
    if (!ctx) {
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Set configuration */
    ctx->config.preferred_width = width;
    ctx->config.preferred_height = height;
    ctx->config.preferred_refresh = refresh_rate;
    
    /* Initialize DRM/GBM using robust drm_display module */
    printf("Initializing DRM/KMS display...\n");
    ret = drm_init(&ctx->drm_ctx);
    if (ret) {
        fprintf(stderr, "Failed to initialize DRM display\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    printf("GBM surface initialized: %dx%d@%dHz\n", 
           ctx->drm_ctx.width, 
           ctx->drm_ctx.height, 
           ctx->drm_ctx.refresh_rate);
    
    /* Initialize EGL with GBM surface from DRM context */
    ret = init_egl_with_drm_surface(ctx);
    if (ret < 0) {
        drm_cleanup(&ctx->drm_ctx);
        return ret;
    }
    
    /* Fill display info with GBM surface data */
    ctx->info.width = ctx->drm_ctx.width;
    ctx->info.height = ctx->drm_ctx.height;
    ctx->info.refresh_rate = ctx->drm_ctx.refresh_rate;
    ctx->info.physical_width_mm = 0;  /* Unknown in GBM-only mode */
    ctx->info.physical_height_mm = 0; /* Unknown in GBM-only mode */
    
    /* Set generic connector name for GBM-only mode */
    snprintf(ctx->info.connector_name, sizeof(ctx->info.connector_name), 
             "GBM-Surface");
    
    ctx->configured = 1;
    printf("Display output configured: %dx%d@%dHz on %s\n",
           ctx->info.width, ctx->info.height, ctx->info.refresh_rate,
           ctx->info.connector_name);
    
    return DISPLAY_OUTPUT_OK;
}

/**
 * Present frame to display using DRM module
 */
int display_output_present_frame(display_output_ctx_t *ctx) {
    struct timeval start_time, end_time;
    int ret;
    
    if (!ctx || !ctx->configured) {
        return DISPLAY_OUTPUT_ERROR;
    }
    
    gettimeofday(&start_time, NULL);
    
    /* Swap EGL buffers */
    if (!eglSwapBuffers(ctx->egl_display, ctx->egl_surface)) {
        fprintf(stderr, "Failed to swap EGL buffers\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Use DRM module's robust buffer swapping */
    ret = drm_swap_buffers(&ctx->drm_ctx);
    if (ret) {
        fprintf(stderr, "Failed to swap DRM buffers\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Update statistics */
    gettimeofday(&end_time, NULL);
    uint64_t present_time = (end_time.tv_sec - start_time.tv_sec) * 1000000LL +
                           (end_time.tv_usec - start_time.tv_usec);
    
    ctx->frames_presented++;
    ctx->total_present_time_us += present_time;
    ctx->last_present_time = end_time;
    
    return DISPLAY_OUTPUT_OK;
}

/**
 * Get EGL display handle
 */
EGLDisplay display_output_get_egl_display(display_output_ctx_t *ctx) {
    return ctx ? ctx->egl_display : EGL_NO_DISPLAY;
}

/**
 * Get EGL surface handle
 */
EGLSurface display_output_get_egl_surface(display_output_ctx_t *ctx) {
    return ctx ? ctx->egl_surface : EGL_NO_SURFACE;
}

/**
 * Get display information
 */
int display_output_get_info(display_output_ctx_t *ctx, display_info_t *info) {
    if (!ctx || !info || !ctx->configured) {
        return DISPLAY_OUTPUT_ERROR;
    }
    
    memcpy(info, &ctx->info, sizeof(display_info_t));
    return DISPLAY_OUTPUT_OK;
}

/**
 * Check if DRM/KMS is available
 */
int display_output_is_available(void) {
    int fd = open(DEFAULT_DRM_DEVICE, O_RDWR);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return 0;
}

/**
 * Get connector type name
 */
const char *display_output_connector_type_name(uint32_t connector_type) {
    switch (connector_type) {
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
        case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
        case DRM_MODE_CONNECTOR_Composite: return "Composite";
        case DRM_MODE_CONNECTOR_SVIDEO: return "S-Video";
        case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
        case DRM_MODE_CONNECTOR_Component: return "Component";
        case DRM_MODE_CONNECTOR_9PinDIN: return "9-pin DIN";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
        case DRM_MODE_CONNECTOR_TV: return "TV";
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
        case DRM_MODE_CONNECTOR_DSI: return "DSI";
        default: return "Unknown";
    }
}

/**
 * Destroy display output
 */
void display_output_destroy(display_output_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    
    /* Clean up EGL */
    if (ctx->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(ctx->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx->egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(ctx->egl_display, ctx->egl_context);
        }
        if (ctx->egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(ctx->egl_display, ctx->egl_surface);
        }
        eglTerminate(ctx->egl_display);
    }
    
    /* Clean up DRM using robust drm_display module */
    drm_cleanup(&ctx->drm_ctx);
    
    free(ctx);
}
/*
 * Display Output Implementation - DRM/KMS Direct Display
 * 
 * Implements direct display output using DRM/KMS for zero-copy presentation.
 * Integrates with EGL for GPU rendering pipeline.
 */

#define _GNU_SOURCE
#include "display_output.h"
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
    /* DRM resources */
    int drm_fd;
    drmModeRes *drm_resources;
    drmModeConnector *connector;
    drmModeEncoder *encoder;
    drmModeCrtc *crtc;
    drmModeModeInfo mode;
    
    /* Configuration */
    display_config_t config;
    display_info_t info;
    
    /* GBM resources */
    struct gbm_device *gbm_device;
    struct gbm_surface *gbm_surface;
    
    /* EGL resources */
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    
    /* State */
    int configured;
    int mode_set;
    
    /* Statistics */
    uint64_t frames_presented;
    uint64_t vblank_count;
    uint64_t total_present_time_us;
    struct timeval last_present_time;
};

/**
 * Find suitable connector and mode
 */
static int find_display_configuration(display_output_ctx_t *ctx) {
    drmModeConnector *connector = NULL;
    drmModeModeInfo *mode = NULL;
    int found_connector = 0;
    
    /* Find connected connector */
    for (int i = 0; i < ctx->drm_resources->count_connectors; i++) {
        connector = drmModeGetConnector(ctx->drm_fd, ctx->drm_resources->connectors[i]);
        if (!connector) continue;
        
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            /* Check if this is the requested connector */
            if (ctx->config.connector_id > 0 && 
                connector->connector_id != (uint32_t)ctx->config.connector_id) {
                drmModeFreeConnector(connector);
                continue;
            }
            
            found_connector = 1;
            break;
        }
        
        drmModeFreeConnector(connector);
    }
    
    if (!found_connector || !connector) {
        fprintf(stderr, "No connected display found\n");
        fprintf(stderr, "Checked %d connectors\n", ctx->drm_resources->count_connectors);
        return DISPLAY_OUTPUT_ERROR;
    }
    
    printf("Found connected display: connector %d, %d modes available\n", 
           connector->connector_id, connector->count_modes);
    
    ctx->connector = connector;
    
    /* Find suitable mode */
    mode = &connector->modes[0];  /* Default to first mode */
    
    for (int i = 0; i < connector->count_modes; i++) {
        drmModeModeInfo *candidate = &connector->modes[i];
        
        /* Check for preferred mode first */
        if (candidate->type & DRM_MODE_TYPE_PREFERRED) {
            mode = candidate;
            if (ctx->config.preferred_width == 0 || ctx->config.preferred_height == 0) {
                break;  /* Use preferred mode if no specific size requested */
            }
        }
        
        /* Check for exact match with requested resolution */
        if (ctx->config.preferred_width > 0 && ctx->config.preferred_height > 0) {
            if (candidate->hdisplay == ctx->config.preferred_width &&
                candidate->vdisplay == ctx->config.preferred_height) {
                
                /* Check refresh rate if specified */
                if (ctx->config.preferred_refresh > 0) {
                    int refresh = candidate->vrefresh;
                    if (refresh == ctx->config.preferred_refresh) {
                        mode = candidate;
                        break;
                    }
                } else {
                    mode = candidate;
                    break;
                }
            }
        }
    }
    
    if (!mode) {
        fprintf(stderr, "No suitable display mode found\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    memcpy(&ctx->mode, mode, sizeof(drmModeModeInfo));
    
    printf("Selected display mode: %dx%d@%dHz (%s)\n",
           mode->hdisplay, mode->vdisplay, mode->vrefresh, mode->name);
    
    return DISPLAY_OUTPUT_OK;
}

/**
 * Setup CRTC and encoder
 */
static int setup_crtc(display_output_ctx_t *ctx) {
    /* Find encoder */
    if (ctx->connector->encoder_id) {
        ctx->encoder = drmModeGetEncoder(ctx->drm_fd, ctx->connector->encoder_id);
    }
    
    if (!ctx->encoder) {
        /* Find any compatible encoder */
        for (int i = 0; i < ctx->drm_resources->count_encoders; i++) {
            drmModeEncoder *enc = drmModeGetEncoder(ctx->drm_fd, 
                                                   ctx->drm_resources->encoders[i]);
            if (!enc) continue;
            
            if (enc->encoder_id == ctx->connector->encoder_id) {
                ctx->encoder = enc;
                break;
            }
            
            drmModeFreeEncoder(enc);
        }
    }
    
    if (!ctx->encoder) {
        fprintf(stderr, "No suitable encoder found\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Find CRTC */
    if (ctx->config.crtc_id > 0) {
        ctx->crtc = drmModeGetCrtc(ctx->drm_fd, ctx->config.crtc_id);
    } else if (ctx->encoder->crtc_id) {
        ctx->crtc = drmModeGetCrtc(ctx->drm_fd, ctx->encoder->crtc_id);
    }
    
    if (!ctx->crtc) {
        /* Find available CRTC */
        for (int i = 0; i < ctx->drm_resources->count_crtcs; i++) {
            if (ctx->encoder->possible_crtcs & (1 << i)) {
                ctx->crtc = drmModeGetCrtc(ctx->drm_fd, ctx->drm_resources->crtcs[i]);
                if (ctx->crtc) break;
            }
        }
    }
    
    if (!ctx->crtc) {
        fprintf(stderr, "No suitable CRTC found\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    printf("Using CRTC %d with encoder %d\n", ctx->crtc->crtc_id, ctx->encoder->encoder_id);
    return DISPLAY_OUTPUT_OK;
}

/**
 * Initialize GBM
 */
static int init_gbm(display_output_ctx_t *ctx) {
    ctx->gbm_device = gbm_create_device(ctx->drm_fd);
    if (!ctx->gbm_device) {
        fprintf(stderr, "Failed to create GBM device\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Try different GBM formats for compatibility */
    uint32_t gbm_formats[] = {
        GBM_FORMAT_XRGB8888,  /* 24-bit RGB */
        GBM_FORMAT_ARGB8888,  /* 32-bit RGBA */
        GBM_FORMAT_RGB565,    /* 16-bit RGB */
        0
    };
    
    const char* format_names[] = {
        "XRGB8888", 
        "ARGB8888",
        "RGB565"
    };
    
    for (int i = 0; gbm_formats[i] != 0; i++) {
        printf("Trying GBM surface: %dx%d, format=%s, flags=0x%x\n",
               ctx->mode.hdisplay, ctx->mode.vdisplay, 
               format_names[i], GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        
        ctx->gbm_surface = gbm_surface_create(ctx->gbm_device,
                                             ctx->mode.hdisplay,
                                             ctx->mode.vdisplay,
                                             gbm_formats[i],
                                             GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        
        if (ctx->gbm_surface) {
            printf("✓ GBM surface created successfully with %s format\n", format_names[i]);
            break;
        } else {
            printf("✗ Failed to create GBM surface with %s: %s\n", 
                   format_names[i], strerror(errno));
        }
    }
    
    if (!ctx->gbm_surface) {
        fprintf(stderr, "Failed to create GBM surface with any supported format\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    printf("GBM initialized: %dx%d surface\n", 
           ctx->mode.hdisplay, ctx->mode.vdisplay);
    return DISPLAY_OUTPUT_OK;
}

/**
 * Initialize EGL with comprehensive diagnostics
 */
static int init_egl(display_output_ctx_t *ctx) {
    EGLint major, minor;
    EGLint config_count;
    uint32_t gbm_format = GBM_FORMAT_XRGB8888;  /* Default format */
    
    /* Get EGL display */
    ctx->egl_display = eglGetDisplay((EGLNativeDisplayType)ctx->gbm_device);
    if (ctx->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Initialize EGL */
    if (!eglInitialize(ctx->egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    printf("EGL %d.%d initialized\n", major, minor);
    
    /* Bind OpenGL ES API */
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "Failed to bind OpenGL ES API\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* List all available EGL configs for debugging */
    EGLConfig all_configs[256];
    EGLint total_configs;
    if (eglGetConfigs(ctx->egl_display, all_configs, 256, &total_configs)) {
        printf("Total EGL configs available: %d\n", total_configs);
        
        /* Print details of first few configs */
        for (int i = 0; i < (total_configs < 5 ? total_configs : 5); i++) {
            EGLint red, green, blue, alpha, depth, stencil, samples;
            EGLint surface_type, renderable_type, native_visual;
            
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_RED_SIZE, &red);
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_GREEN_SIZE, &green);
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_BLUE_SIZE, &blue);
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_ALPHA_SIZE, &alpha);
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_DEPTH_SIZE, &depth);
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_STENCIL_SIZE, &stencil);
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_SAMPLES, &samples);
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_SURFACE_TYPE, &surface_type);
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_RENDERABLE_TYPE, &renderable_type);
            eglGetConfigAttrib(ctx->egl_display, all_configs[i], EGL_NATIVE_VISUAL_ID, &native_visual);
            
            printf("Config %d: RGBA=%d,%d,%d,%d Depth=%d Stencil=%d Samples=%d\n",
                   i, red, green, blue, alpha, depth, stencil, samples);
            printf("  Surface type: 0x%x, Renderable: 0x%x, Native visual: 0x%x\n",
                   surface_type, renderable_type, native_visual);
        }
    }
    
    /* Try multiple EGL config approaches */
    
    /* Approach 1: Match GBM format exactly with native visual ID */
    EGLint config_attribs1[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,  /* No alpha for XRGB */
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NATIVE_VISUAL_ID, GBM_FORMAT_XRGB8888,  /* Match GBM format */
        EGL_NONE
    };
    
    if (eglChooseConfig(ctx->egl_display, config_attribs1, &ctx->egl_config, 1, &config_count) &&
        config_count > 0) {
        printf("✓ Using approach 1: XRGB8888 with native visual ID match\n");
        gbm_format = GBM_FORMAT_XRGB8888;
    } else {
        printf("✗ Approach 1 failed: No matching config for XRGB8888\n");
        
        /* Approach 2: Try with ARGB format */
        EGLint config_attribs2[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,  /* Alpha for ARGB */
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NATIVE_VISUAL_ID, GBM_FORMAT_ARGB8888,
            EGL_NONE
        };
        
        if (eglChooseConfig(ctx->egl_display, config_attribs2, &ctx->egl_config, 1, &config_count) &&
            config_count > 0) {
            printf("✓ Using approach 2: ARGB8888 format\n");
            gbm_format = GBM_FORMAT_ARGB8888;
        } else {
            printf("✗ Approach 2 failed: No matching config for ARGB8888\n");
            
            /* Approach 3: Simple config without native visual ID constraint */
            EGLint config_attribs3[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_NONE
            };
            
            if (eglChooseConfig(ctx->egl_display, config_attribs3, &ctx->egl_config, 1, &config_count) &&
                config_count > 0) {
                printf("✓ Using approach 3: Simple config without native visual constraint\n");
                gbm_format = GBM_FORMAT_ARGB8888;  /* Default to ARGB for flexibility */
            } else {
                fprintf(stderr, "✗ All approaches failed: Cannot find suitable EGL config\n");
                return DISPLAY_OUTPUT_ERROR;
            }
        }
    }
    
    printf("Found %d matching EGL configs\n", config_count);
    
    /* Print chosen config details and use EXACT format from EGL config */
    EGLint visual_id;
    if (eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_NATIVE_VISUAL_ID, &visual_id)) {
        printf("Chosen config native visual ID: 0x%x\n", visual_id);
        
        /* CRITICAL: Use the exact visual ID as our GBM format */
        gbm_format = (uint32_t)visual_id;
        printf("Setting GBM format to match EGL config: 0x%x\n", gbm_format);
    }
    
    /* Recreate GBM surface with the EXACT format from EGL config */
    if (ctx->gbm_surface) {
        gbm_surface_destroy(ctx->gbm_surface);
    }
    
    printf("Recreating GBM surface with EGL-matched format 0x%x...\n", gbm_format);
    ctx->gbm_surface = gbm_surface_create(ctx->gbm_device,
                                          ctx->mode.hdisplay,
                                          ctx->mode.vdisplay,
                                          gbm_format,
                                          GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!ctx->gbm_surface) {
        fprintf(stderr, "Failed to recreate GBM surface with format 0x%x: %s\n", 
                gbm_format, strerror(errno));
        
        /* Try a few common fallback formats if exact match fails */
        uint32_t fallback_formats[] = {
            GBM_FORMAT_XRGB8888,
            GBM_FORMAT_ARGB8888,
            GBM_FORMAT_RGB565,
            0
        };
        
        for (int i = 0; fallback_formats[i] != 0; i++) {
            printf("Trying fallback GBM format: 0x%x\n", fallback_formats[i]);
            ctx->gbm_surface = gbm_surface_create(ctx->gbm_device,
                                                  ctx->mode.hdisplay,
                                                  ctx->mode.vdisplay,
                                                  fallback_formats[i],
                                                  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
            if (ctx->gbm_surface) {
                gbm_format = fallback_formats[i];
                printf("✓ Fallback GBM surface created with format 0x%x\n", gbm_format);
                break;
            }
        }
        
        if (!ctx->gbm_surface) {
            fprintf(stderr, "All GBM surface creation attempts failed\n");
            return DISPLAY_OUTPUT_ERROR;
        }
    } else {
        printf("✓ GBM surface recreated successfully with EGL-matched format\n");
    }
    
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
    

    
    /* Create EGL window surface with detailed diagnostics */
    printf("Attempting EGL surface creation...\n");
    printf("  GBM surface pointer: %p\n", ctx->gbm_surface);
    printf("  GBM surface format: 0x%x\n", gbm_format);
    printf("  EGL config: %p\n", ctx->egl_config);
    
    ctx->egl_surface = eglCreateWindowSurface(ctx->egl_display, ctx->egl_config,
                                              (EGLNativeWindowType)ctx->gbm_surface, NULL);
    if (ctx->egl_surface == EGL_NO_SURFACE) {
        EGLint egl_error = eglGetError();
        fprintf(stderr, "Failed to create EGL window surface: %s (0x%04x)\n",
                egl_error_string(egl_error), egl_error);
        
        /* Detailed error analysis */
        if (egl_error == EGL_BAD_MATCH) {
            printf("EGL_BAD_MATCH analysis:\n");
            printf("  - This usually indicates format incompatibility\n");
            printf("  - GBM surface format may not match EGL config\n");
            printf("  - Checking if EGL config supports window surfaces...\n");
            
            EGLint surface_type;
            if (eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_SURFACE_TYPE, &surface_type)) {
                printf("  - EGL config surface type: 0x%x (window bit: %s)\n",
                       surface_type, (surface_type & EGL_WINDOW_BIT) ? "YES" : "NO");
            }
            
            EGLint visual_id;
            if (eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_NATIVE_VISUAL_ID, &visual_id)) {
                printf("  - EGL config visual ID: 0x%x\n", visual_id);
                printf("  - Current GBM format: 0x%x\n", gbm_format);
                printf("  - Match: %s\n", (visual_id == gbm_format) ? "YES" : "NO");
            }
        }
        
        return DISPLAY_OUTPUT_ERROR;
    }
    
    printf("✓ EGL window surface created successfully\n");
    
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
    
    ctx->drm_fd = -1;
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
    ctx->config.device_path = DEFAULT_DRM_DEVICE;
    
    /* Open DRM device */
    ctx->drm_fd = open(ctx->config.device_path, O_RDWR | O_CLOEXEC);
    if (ctx->drm_fd < 0) {
        fprintf(stderr, "Failed to open DRM device %s: %s\n", 
                ctx->config.device_path, strerror(errno));
        if (errno == EACCES) {
            fprintf(stderr, "Permission denied. Try running as root or add user to 'video' group:\n");
            fprintf(stderr, "  sudo usermod -a -G video $USER\n");
            fprintf(stderr, "  sudo chmod 666 /dev/dri/card*\n");
        }
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Get DRM resources */
    ctx->drm_resources = drmModeGetResources(ctx->drm_fd);
    if (!ctx->drm_resources) {
        fprintf(stderr, "Failed to get DRM resources\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* Find display configuration */
    ret = find_display_configuration(ctx);
    if (ret < 0) {
        return ret;
    }
    
    /* Setup CRTC */
    ret = setup_crtc(ctx);
    if (ret < 0) {
        return ret;
    }
    
    /* Initialize GBM */
    ret = init_gbm(ctx);
    if (ret < 0) {
        return ret;
    }
    
    /* Initialize EGL */
    ret = init_egl(ctx);
    if (ret < 0) {
        return ret;
    }
    
    /* Fill display info */
    ctx->info.width = ctx->mode.hdisplay;
    ctx->info.height = ctx->mode.vdisplay;
    ctx->info.refresh_rate = ctx->mode.vrefresh;
    ctx->info.physical_width_mm = ctx->connector->mmWidth;
    ctx->info.physical_height_mm = ctx->connector->mmHeight;
    
    /* Get connector name */
    const char *conn_name = display_output_connector_type_name(ctx->connector->connector_type);
    snprintf(ctx->info.connector_name, sizeof(ctx->info.connector_name), 
             "%s-%d", conn_name, ctx->connector->connector_type_id);
    
    ctx->configured = 1;
    printf("Display output configured: %dx%d@%dHz on %s\n",
           ctx->info.width, ctx->info.height, ctx->info.refresh_rate,
           ctx->info.connector_name);
    
    return DISPLAY_OUTPUT_OK;
}

/**
 * Present frame to display
 */
int display_output_present_frame(display_output_ctx_t *ctx) {
    struct timeval start_time, end_time;
    
    if (!ctx || !ctx->configured) {
        return DISPLAY_OUTPUT_ERROR;
    }
    
    gettimeofday(&start_time, NULL);
    
    /* Swap EGL buffers */
    if (!eglSwapBuffers(ctx->egl_display, ctx->egl_surface)) {
        fprintf(stderr, "Failed to swap EGL buffers\n");
        return DISPLAY_OUTPUT_ERROR;
    }
    
    /* TODO: Set mode on first frame if not already done */
    if (!ctx->mode_set) {
        struct gbm_bo *bo = gbm_surface_lock_front_buffer(ctx->gbm_surface);
        if (!bo) {
            fprintf(stderr, "Failed to lock front buffer\n");
            return DISPLAY_OUTPUT_ERROR;
        }
        
        uint32_t fb_id;
        int ret = drmModeAddFB(ctx->drm_fd,
                              ctx->mode.hdisplay, ctx->mode.vdisplay,
                              24, 32, gbm_bo_get_stride(bo),
                              gbm_bo_get_handle(bo).u32, &fb_id);
        
        if (ret) {
            fprintf(stderr, "Failed to add framebuffer\n");
            gbm_surface_release_buffer(ctx->gbm_surface, bo);
            return DISPLAY_OUTPUT_ERROR;
        }
        
        ret = drmModeSetCrtc(ctx->drm_fd, ctx->crtc->crtc_id, fb_id, 0, 0,
                            &ctx->connector->connector_id, 1, &ctx->mode);
        
        if (ret) {
            fprintf(stderr, "Failed to set CRTC mode\n");
            drmModeRmFB(ctx->drm_fd, fb_id);
            gbm_surface_release_buffer(ctx->gbm_surface, bo);
            return DISPLAY_OUTPUT_ERROR;
        }
        
        gbm_surface_release_buffer(ctx->gbm_surface, bo);
        ctx->mode_set = 1;
        printf("Display mode set successfully\n");
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
    
    /* Clean up GBM */
    if (ctx->gbm_surface) {
        gbm_surface_destroy(ctx->gbm_surface);
    }
    if (ctx->gbm_device) {
        gbm_device_destroy(ctx->gbm_device);
    }
    
    /* Clean up DRM */
    if (ctx->crtc) {
        drmModeFreeCrtc(ctx->crtc);
    }
    if (ctx->encoder) {
        drmModeFreeEncoder(ctx->encoder);
    }
    if (ctx->connector) {
        drmModeFreeConnector(ctx->connector);
    }
    if (ctx->drm_resources) {
        drmModeFreeResources(ctx->drm_resources);
    }
    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
    }
    
    free(ctx);
}
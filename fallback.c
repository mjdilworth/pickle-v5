/*
 * Fallback Implementation - libmpv Software Playback
 * 
 * Provides software video playback using libmpv when hardware acceleration fails.
 * Simplified implementation for basic playback functionality.
 */

#define _GNU_SOURCE  /* For strdup() */

#include "fallback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Conditional libmpv includes - may not be available on all systems */
#ifdef HAVE_LIBMPV
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#endif

/* Internal fallback context */
struct fallback_ctx {
    #ifdef HAVE_LIBMPV
    mpv_handle *mpv;
    mpv_render_context *mpv_gl;
    #endif
    
    fallback_config_t config;
    int initialized;
    int playing;
    char *current_file;
};

/**
 * Create fallback context
 */
fallback_ctx_t *fallback_create(void) {
    fallback_ctx_t *ctx = calloc(1, sizeof(fallback_ctx_t));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate fallback context\n");
        return NULL;
    }
    
    /* Set default configuration */
    ctx->config.enable_hardware_decode = 1;
    ctx->config.enable_audio = 1;
    ctx->config.vo_driver = "gpu";  /* Default to GPU output */
    
    return ctx;
}

/**
 * Initialize libmpv (if available)
 */
static int init_mpv(fallback_ctx_t *ctx) {
    #ifdef HAVE_LIBMPV
    ctx->mpv = mpv_create();
    if (!ctx->mpv) {
        fprintf(stderr, "Failed to create mpv context\n");
        return FALLBACK_ERROR;
    }
    
    /* Set basic options */
    mpv_set_option_string(ctx->mpv, "vo", ctx->config.vo_driver ? ctx->config.vo_driver : "gpu");
    
    if (ctx->config.enable_hardware_decode && ctx->config.hwdec) {
        mpv_set_option_string(ctx->mpv, "hwdec", ctx->config.hwdec);
    }
    
    if (!ctx->config.enable_audio) {
        mpv_set_option_string(ctx->mpv, "audio", "no");
    }
    
    if (ctx->config.loop_file) {
        mpv_set_option_string(ctx->mpv, "loop-file", "yes");
    }
    
    /* Initialize mpv */
    if (mpv_initialize(ctx->mpv) < 0) {
        fprintf(stderr, "Failed to initialize mpv\n");
        mpv_destroy(ctx->mpv);
        ctx->mpv = NULL;
        return FALLBACK_ERROR;
    }
    
    ctx->initialized = 1;
    printf("libmpv initialized successfully\n");
    return FALLBACK_OK;
    
    #else
    fprintf(stderr, "libmpv not available - fallback disabled\n");
    return FALLBACK_ERROR;
    #endif
}

/**
 * Set fallback configuration
 */
int fallback_set_config(fallback_ctx_t *ctx, const fallback_config_t *config) {
    if (!ctx || !config) {
        return FALLBACK_ERROR;
    }
    
    if (ctx->initialized) {
        fprintf(stderr, "Cannot change config after initialization\n");
        return FALLBACK_ERROR;
    }
    
    memcpy(&ctx->config, config, sizeof(fallback_config_t));
    return FALLBACK_OK;
}

/**
 * Play video file using libmpv
 */
int fallback_play_file(fallback_ctx_t *ctx, const char *filename) {
    if (!ctx || !filename) {
        return FALLBACK_ERROR;
    }
    
    printf("Starting fallback playback with libmpv: %s\n", filename);
    
    #ifdef HAVE_LIBMPV
    /* Initialize mpv if not already done */
    if (!ctx->initialized) {
        int ret = init_mpv(ctx);
        if (ret < 0) {
            return ret;
        }
    }
    
    /* Load file */
    const char *cmd[] = {"loadfile", filename, NULL};
    if (mpv_command(ctx->mpv, cmd) < 0) {
        fprintf(stderr, "Failed to load file in mpv\n");
        return FALLBACK_ERROR;
    }
    
    /* Store current file */
    free(ctx->current_file);
    ctx->current_file = strdup(filename);
    ctx->playing = 1;
    
    /* Simple event loop for playback */
    printf("Playing video with libmpv. Press Ctrl+C to stop.\n");
    
    while (ctx->playing) {
        mpv_event *event = mpv_wait_event(ctx->mpv, 0.1);
        
        switch (event->event_id) {
            case MPV_EVENT_SHUTDOWN:
            case MPV_EVENT_END_FILE:
                printf("Playback finished\n");
                ctx->playing = 0;
                break;
                
            case MPV_EVENT_FILE_LOADED:
                printf("File loaded successfully\n");
                break;
                
            case MPV_EVENT_PLAYBACK_RESTART:
                printf("Playback started\n");
                break;
                
            case MPV_EVENT_LOG_MESSAGE: {
                mpv_event_log_message *msg = event->data;
                if (msg->log_level <= MPV_LOG_LEVEL_ERROR) {
                    printf("mpv: [%s] %s", msg->prefix, msg->text);
                }
                break;
            }
            
            default:
                break;
        }
        
        /* Check for external stop signal */
        if (access("/tmp/pickle_stop", F_OK) == 0) {
            unlink("/tmp/pickle_stop");
            printf("Stop signal received\n");
            break;
        }
        
        /* Brief yield to allow signal handling */
        usleep(1000);
    }
    
    printf("Stopping mpv playback...\n");
    const char *stop_cmd[] = {"stop", NULL};
    mpv_command(ctx->mpv, stop_cmd);
    
    return FALLBACK_OK;
    
    #else
    /* Simple fallback without libmpv - just notify user */
    fprintf(stderr, "Fallback playback requested for: %s\n", filename);
    fprintf(stderr, "libmpv not available. Please install libmpv-dev and recompile.\n");
    fprintf(stderr, "Alternative: Use system video player:\n");
    fprintf(stderr, "  vlc '%s'\n", filename);
    fprintf(stderr, "  mpv '%s'\n", filename);
    fprintf(stderr, "  ffplay '%s'\n", filename);
    
    /* Try to launch external player as last resort */
    char command[1024];
    snprintf(command, sizeof(command), "which mpv >/dev/null 2>&1 && mpv --fs '%s' &", filename);
    
    int ret = system(command);
    if (ret == 0) {
        printf("Launched external mpv player\n");
        sleep(2);  /* Give it time to start */
        return FALLBACK_OK;
    }
    
    snprintf(command, sizeof(command), "which vlc >/dev/null 2>&1 && vlc --intf dummy --fullscreen '%s' &", filename);
    ret = system(command);
    if (ret == 0) {
        printf("Launched external VLC player\n");
        sleep(2);
        return FALLBACK_OK;
    }
    
    return FALLBACK_ERROR;
    #endif
}

/**
 * Stop playback
 */
int fallback_stop(fallback_ctx_t *ctx) {
    if (!ctx) {
        return FALLBACK_ERROR;
    }
    
    #ifdef HAVE_LIBMPV
    if (ctx->mpv && ctx->playing) {
        const char *cmd[] = {"stop", NULL};
        mpv_command(ctx->mpv, cmd);
        ctx->playing = 0;
    }
    #endif
    
    ctx->playing = 0;
    return FALLBACK_OK;
}

/**
 * Check if libmpv is available
 */
int fallback_is_available(void) {
    #ifdef HAVE_LIBMPV
    return 1;
    #else
    /* Check for external players */
    if (system("which mpv >/dev/null 2>&1") == 0) return 1;
    if (system("which vlc >/dev/null 2>&1") == 0) return 1;
    return 0;
    #endif
}

/**
 * Get libmpv version
 */
const char *fallback_get_version(void) {
    #ifdef HAVE_LIBMPV
    static char version_str[32];
    snprintf(version_str, sizeof(version_str), "libmpv %lu", mpv_client_api_version());
    return version_str;
    #else
    return "External player fallback";
    #endif
}

/**
 * Get playback position
 */
double fallback_get_position(fallback_ctx_t *ctx) {
    if (!ctx) {
        return -1.0;
    }
    
    #ifdef HAVE_LIBMPV
    if (ctx->mpv) {
        double pos = 0;
        if (mpv_get_property(ctx->mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0) {
            return pos;
        }
    }
    #endif
    
    return -1.0;
}

/**
 * Get video duration
 */
double fallback_get_duration(fallback_ctx_t *ctx) {
    if (!ctx) {
        return -1.0;
    }
    
    #ifdef HAVE_LIBMPV
    if (ctx->mpv) {
        double duration = 0;
        if (mpv_get_property(ctx->mpv, "duration", MPV_FORMAT_DOUBLE, &duration) >= 0) {
            return duration;
        }
    }
    #endif
    
    return -1.0;
}

/**
 * Check if playing
 */
int fallback_is_playing(fallback_ctx_t *ctx) {
    return ctx ? ctx->playing : 0;
}

/**
 * Destroy fallback context
 */
void fallback_destroy(fallback_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    
    #ifdef HAVE_LIBMPV
    if (ctx->mpv_gl) {
        mpv_render_context_free(ctx->mpv_gl);
    }
    
    if (ctx->mpv) {
        mpv_destroy(ctx->mpv);
    }
    #endif
    
    free(ctx->current_file);
    free(ctx);
}
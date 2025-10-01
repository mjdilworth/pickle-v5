/*
 * Fallback Module - libmpv Software Playback
 * 
 * This module provides:
 * - Software video playback using libmpv
 * - Fallback when hardware acceleration fails
 * - Basic playback controls and error handling
 */

#ifndef FALLBACK_H
#define FALLBACK_H

#include <stdint.h>

/* Return codes */
#define FALLBACK_OK          0
#define FALLBACK_ERROR      -1
#define FALLBACK_EOF        -2

/* Forward declarations */
typedef struct fallback_ctx fallback_ctx_t;

/* Playback configuration */
typedef struct {
    int enable_hardware_decode;  /* Try hardware decode in mpv */
    int enable_audio;           /* Enable audio playback */
    const char *vo_driver;      /* Video output driver (NULL = auto) */
    const char *hwdec;          /* Hardware decode method (NULL = auto) */
    int loop_file;              /* Loop playback */
} fallback_config_t;

/* API Functions */

/**
 * Create fallback context
 * @return New context or NULL on error
 */
fallback_ctx_t *fallback_create(void);

/**
 * Set fallback configuration
 * @param ctx Fallback context
 * @param config Configuration options
 * @return 0 on success, negative on error
 */
int fallback_set_config(fallback_ctx_t *ctx, const fallback_config_t *config);

/**
 * Play video file using libmpv
 * @param ctx Fallback context
 * @param filename Path to video file
 * @return 0 on success, negative on error
 */
int fallback_play_file(fallback_ctx_t *ctx, const char *filename);

/**
 * Stop playback
 * @param ctx Fallback context
 * @return 0 on success, negative on error
 */
int fallback_stop(fallback_ctx_t *ctx);

/**
 * Pause/resume playback
 * @param ctx Fallback context
 * @param pause 1 to pause, 0 to resume
 * @return 0 on success, negative on error
 */
int fallback_pause(fallback_ctx_t *ctx, int pause);

/**
 * Seek to position
 * @param ctx Fallback context
 * @param position_seconds Position in seconds
 * @return 0 on success, negative on error
 */
int fallback_seek(fallback_ctx_t *ctx, double position_seconds);

/**
 * Get playback position
 * @param ctx Fallback context
 * @return Current position in seconds, negative on error
 */
double fallback_get_position(fallback_ctx_t *ctx);

/**
 * Get video duration
 * @param ctx Fallback context
 * @return Duration in seconds, negative on error
 */
double fallback_get_duration(fallback_ctx_t *ctx);

/**
 * Check if playback is active
 * @param ctx Fallback context
 * @return 1 if playing, 0 if stopped/paused, negative on error
 */
int fallback_is_playing(fallback_ctx_t *ctx);

/**
 * Destroy fallback context and free resources
 * @param ctx Fallback context
 */
void fallback_destroy(fallback_ctx_t *ctx);

/* Utility functions */

/**
 * Check if libmpv is available
 * @return 1 if available, 0 if not
 */
int fallback_is_available(void);

/**
 * Get libmpv version string
 * @return Version string or NULL if not available
 */
const char *fallback_get_version(void);

#endif /* FALLBACK_H */
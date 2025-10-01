/*
 * Hardware Decoder Module - FFmpeg h264_v4l2m2m Hardware Decode
 * 
 * This module handles:
 * - FFmpeg h264_v4l2m2m codec for RPi4 hardware decoder
 * - AVFrame management for zero-copy operation
 * - DMABUF export for GPU texture import
 * - YUV420p/NV12 output format handling
 */

#ifndef HW_DECODER_H
#define HW_DECODER_H

#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include "video_input.h"

/* Return codes */
#define HW_DECODER_OK          0
#define HW_DECODER_ERROR      -1
#define HW_DECODER_EAGAIN     -2
#define HW_DECODER_EOF        -3

/* Forward declarations */
typedef struct hw_decoder_ctx hw_decoder_ctx_t;

/* Decoded frame structure with DMABUF handles */
typedef struct {
    /* Frame properties */
    int width;
    int height;
    int format;               /* AV_PIX_FMT_* */
    int64_t timestamp_us;
    
    /* DMABUF file descriptors for zero-copy GPU import */
    int dmabuf_fd[3];         /* Y, U, V planes (or Y, UV for NV12) */
    int num_planes;
    
    /* Buffer offsets and strides */
    uint32_t offsets[3];
    uint32_t pitches[3];
    uint32_t sizes[3];
    
    /* FFmpeg AVFrame reference for release */
    AVFrame *av_frame;
    
    /* Private data for cleanup */
    void *private_data;
} decoded_frame_t;

/* Buffer configuration */
typedef struct {
    int num_input_buffers;    /* Number of input buffers to allocate */
    int num_output_buffers;   /* Number of output buffers to allocate */
    int input_buffer_size;    /* Size of each input buffer */
} decoder_buffer_config_t;

/* API Functions */

/**
 * Create hardware decoder context
 * @return New context or NULL on error
 */
hw_decoder_ctx_t *hw_decoder_create(void);

/**
 * Configure decoder with stream parameters
 * @param ctx Decoder context
 * @param stream_info Video stream information from input module
 * @return 0 on success, negative on error
 */
int hw_decoder_configure(hw_decoder_ctx_t *ctx, const video_stream_info_t *stream_info);

/**
 * Set buffer configuration (optional, uses defaults if not called)
 * @param ctx Decoder context
 * @param config Buffer configuration
 * @return 0 on success, negative on error
 */
int hw_decoder_set_buffer_config(hw_decoder_ctx_t *ctx, const decoder_buffer_config_t *config);

/**
 * Submit compressed packet for decoding
 * @param ctx Decoder context
 * @param packet Input packet from video_input module
 * @return 0 on success, negative on error
 */
int hw_decoder_submit_packet(hw_decoder_ctx_t *ctx, const frame_packet_t *packet);

/**
 * Get decoded frame (non-blocking)
 * @param ctx Decoder context  
 * @param frame Output frame structure with DMABUF handles
 * @return 0 on success, HW_DECODER_EAGAIN if no frame ready, negative on error
 */
int hw_decoder_get_frame(hw_decoder_ctx_t *ctx, decoded_frame_t *frame);

/**
 * Release frame buffer back to decoder
 * @param ctx Decoder context
 * @param frame Frame to release
 */
void hw_decoder_release_frame(hw_decoder_ctx_t *ctx, decoded_frame_t *frame);

/**
 * Flush decoder buffers
 * @param ctx Decoder context
 * @return 0 on success, negative on error
 */
int hw_decoder_flush(hw_decoder_ctx_t *ctx);

/**
 * Get decoder statistics
 * @param ctx Decoder context
 * @param frames_decoded Number of frames decoded
 * @param frames_dropped Number of frames dropped
 * @param avg_decode_time_us Average decode time in microseconds
 */
void hw_decoder_get_stats(hw_decoder_ctx_t *ctx, 
                         uint64_t *frames_decoded,
                         uint64_t *frames_dropped, 
                         uint64_t *avg_decode_time_us);

/**
 * Destroy decoder and free resources
 * @param ctx Decoder context
 */
void hw_decoder_destroy(hw_decoder_ctx_t *ctx);

/* Utility functions */

/**
 * Check if hardware decoder is available on this system
 * @return 1 if available, 0 if not
 */
int hw_decoder_is_available(void);

/**
 * Get list of supported input formats
 * @param formats Output array of V4L2_PIX_FMT_* values
 * @param max_formats Maximum number of formats to return
 * @return Number of formats returned
 */
int hw_decoder_get_supported_formats(uint32_t *formats, int max_formats);

#endif /* HW_DECODER_H */
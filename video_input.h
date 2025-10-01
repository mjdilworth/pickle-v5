/*
 * Video Input Module - libavformat MP4 demuxing and H.264 packet extraction
 * 
 * This module handles:
 * - MP4 container demuxing
 * - H.264 stream parsing
 * - Packet extraction for hardware decoder
 * - Stream metadata and timing information
 */

#ifndef VIDEO_INPUT_H
#define VIDEO_INPUT_H

#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

/* Return codes */
#define VIDEO_INPUT_OK          0
#define VIDEO_INPUT_ERROR      -1
#define VIDEO_INPUT_EOF        -2
#define VIDEO_INPUT_EAGAIN     -3

/* Forward declarations */
typedef struct video_input_ctx video_input_ctx_t;

/* Video stream information */
typedef struct {
    int width;
    int height;
    int fps_num;
    int fps_den;
    int profile;
    int level;
    uint8_t *extradata;
    int extradata_size;
    int64_t duration_us;  /* Duration in microseconds */
} video_stream_info_t;

/* Frame packet structure for zero-copy operation */
typedef struct {
    uint8_t *data;
    int size;
    int64_t pts;          /* Presentation timestamp */
    int64_t dts;          /* Decode timestamp */
    int keyframe;         /* 1 if keyframe, 0 otherwise */
    void *private_data;   /* Internal AVPacket reference */
} frame_packet_t;

/* API Functions */

/**
 * Create video input context
 * @return New context or NULL on error
 */
video_input_ctx_t *video_input_create(void);

/**
 * Open video file and initialize demuxer
 * @param ctx Video input context
 * @param filename Path to video file
 * @return 0 on success, negative on error
 */
int video_input_open(video_input_ctx_t *ctx, const char *filename);

/**
 * Get video stream information
 * @param ctx Video input context
 * @param info Output stream information
 * @return 0 on success, negative on error
 */
int video_input_get_stream_info(video_input_ctx_t *ctx, video_stream_info_t *info);

/**
 * Read next video packet (zero-copy)
 * @param ctx Video input context
 * @param packet Output packet structure
 * @return 0 on success, VIDEO_INPUT_EOF on end, negative on error
 */
int video_input_read_packet(video_input_ctx_t *ctx, frame_packet_t *packet);

/**
 * Free packet resources
 * @param packet Packet to free
 */
void video_input_free_packet(frame_packet_t *packet);

/**
 * Seek to specific timestamp
 * @param ctx Video input context
 * @param timestamp_us Timestamp in microseconds
 * @return 0 on success, negative on error
 */
int video_input_seek(video_input_ctx_t *ctx, int64_t timestamp_us);

/**
 * Get current playback position
 * @param ctx Video input context
 * @return Current position in microseconds, negative on error
 */
int64_t video_input_get_position(video_input_ctx_t *ctx);

/**
 * Close video input and free resources
 * @param ctx Video input context
 */
void video_input_destroy(video_input_ctx_t *ctx);

#endif /* VIDEO_INPUT_H */
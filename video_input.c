/*
 * Video Input Module Implementation
 * 
 * Implements MP4 demuxing using libavformat with zero-copy packet extraction.
 * Optimized for H.264 streams on Raspberry Pi 4.
 */

#include "video_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FFmpeg compatibility */
#ifndef AV_TIME_BASE_Q
#define AV_TIME_BASE_Q (AVRational){1, AV_TIME_BASE}
#endif

/* Internal context structure */
struct video_input_ctx {
    AVFormatContext *format_ctx;
    AVCodecParameters *codec_params;
    int video_stream_index;
    int64_t start_time;
    int initialized;
};

/**
 * Create video input context
 */
video_input_ctx_t *video_input_create(void) {
    video_input_ctx_t *ctx = calloc(1, sizeof(video_input_ctx_t));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate video input context\n");
        return NULL;
    }
    
    ctx->video_stream_index = -1;
    return ctx;
}

/**
 * Open video file and initialize demuxer
 */
int video_input_open(video_input_ctx_t *ctx, const char *filename) {
    int ret = 0;
    
    if (!ctx || !filename) {
        fprintf(stderr, "Invalid parameters to video_input_open\n");
        return VIDEO_INPUT_ERROR;
    }
    
    /* Initialize libavformat if not already done */
    if (!ctx->initialized) {
        /* Note: av_register_all() is deprecated in newer FFmpeg versions */
        ctx->initialized = 1;
    }
    
    /* Open input file */
    ret = avformat_open_input(&ctx->format_ctx, filename, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to open input file '%s': %s\n", 
                filename, av_err2str(ret));
        return VIDEO_INPUT_ERROR;
    }
    
    /* Retrieve stream information */
    ret = avformat_find_stream_info(ctx->format_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to find stream info: %s\n", av_err2str(ret));
        return VIDEO_INPUT_ERROR;
    }
    
    /* Find H.264 video stream */
    ctx->video_stream_index = av_find_best_stream(
        ctx->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    
    if (ctx->video_stream_index < 0) {
        fprintf(stderr, "No video stream found in file\n");
        return VIDEO_INPUT_ERROR;
    }
    
    /* Get codec parameters */
    ctx->codec_params = ctx->format_ctx->streams[ctx->video_stream_index]->codecpar;
    
    /* Verify it's H.264 */
    if (ctx->codec_params->codec_id != AV_CODEC_ID_H264) {
        fprintf(stderr, "Video stream is not H.264 (codec_id: %d)\n", 
                ctx->codec_params->codec_id);
        return VIDEO_INPUT_ERROR;
    }
    
    /* Store start time for timestamp calculations */
    AVStream *video_stream = ctx->format_ctx->streams[ctx->video_stream_index];
    ctx->start_time = video_stream->start_time != AV_NOPTS_VALUE ? 
                      video_stream->start_time : 0;
    
    printf("Video input opened successfully:\n");
    printf("  Codec: H.264\n");
    printf("  Resolution: %dx%d\n", ctx->codec_params->width, ctx->codec_params->height);
    printf("  Profile: %d, Level: %d\n", ctx->codec_params->profile, ctx->codec_params->level);
    
    return VIDEO_INPUT_OK;
}

/**
 * Get video stream information
 */
int video_input_get_stream_info(video_input_ctx_t *ctx, video_stream_info_t *info) {
    if (!ctx || !info || ctx->video_stream_index < 0) {
        return VIDEO_INPUT_ERROR;
    }
    
    AVStream *stream = ctx->format_ctx->streams[ctx->video_stream_index];
    
    /* Basic parameters */
    info->width = ctx->codec_params->width;
    info->height = ctx->codec_params->height;
    info->profile = ctx->codec_params->profile;
    info->level = ctx->codec_params->level;
    
    /* Frame rate */
    AVRational fps = stream->avg_frame_rate;
    if (fps.num == 0) {
        fps = stream->r_frame_rate;  /* Fallback to container frame rate */
    }
    info->fps_num = fps.num;
    info->fps_den = fps.den;
    
    /* Extradata (SPS/PPS for H.264) */
    if (ctx->codec_params->extradata_size > 0) {
        info->extradata_size = ctx->codec_params->extradata_size;
        info->extradata = malloc(info->extradata_size);
        if (info->extradata) {
            memcpy(info->extradata, ctx->codec_params->extradata, info->extradata_size);
        }
    } else {
        info->extradata = NULL;
        info->extradata_size = 0;
    }
    
    /* Duration */
    if (stream->duration != AV_NOPTS_VALUE) {
        info->duration_us = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
    } else if (ctx->format_ctx->duration != AV_NOPTS_VALUE) {
        info->duration_us = ctx->format_ctx->duration;
    } else {
        info->duration_us = -1;  /* Unknown duration */
    }
    
    return VIDEO_INPUT_OK;
}

/**
 * Read next video packet (zero-copy)
 */
int video_input_read_packet(video_input_ctx_t *ctx, frame_packet_t *packet) {
    if (!ctx || !packet) {
        return VIDEO_INPUT_ERROR;
    }
    
    /* Allocate AVPacket (will be freed in video_input_free_packet) */
    AVPacket *av_packet = av_packet_alloc();
    if (!av_packet) {
        fprintf(stderr, "Failed to allocate AVPacket\n");
        return VIDEO_INPUT_ERROR;
    }
    
    int ret;
    while (1) {
        ret = av_read_frame(ctx->format_ctx, av_packet);
        if (ret < 0) {
            av_packet_free(&av_packet);
            if (ret == AVERROR_EOF) {
                return VIDEO_INPUT_EOF;
            }
            fprintf(stderr, "Error reading frame: %s\n", av_err2str(ret));
            return VIDEO_INPUT_ERROR;
        }
        
        /* Check if this is our video stream */
        if (av_packet->stream_index == ctx->video_stream_index) {
            break;
        }
        
        /* Skip non-video packets */
        av_packet_unref(av_packet);
    }
    
    /* Fill output packet structure (zero-copy) */
    packet->data = av_packet->data;
    packet->size = av_packet->size;
    packet->keyframe = (av_packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
    packet->private_data = av_packet;  /* Store for later cleanup */
    
    /* Convert timestamps to microseconds */
    AVStream *stream = ctx->format_ctx->streams[ctx->video_stream_index];
    if (av_packet->pts != AV_NOPTS_VALUE) {
        packet->pts = av_rescale_q(av_packet->pts, stream->time_base, AV_TIME_BASE_Q);
    } else {
        packet->pts = AV_NOPTS_VALUE;
    }
    
    if (av_packet->dts != AV_NOPTS_VALUE) {
        packet->dts = av_rescale_q(av_packet->dts, stream->time_base, AV_TIME_BASE_Q);
    } else {
        packet->dts = AV_NOPTS_VALUE;
    }
    
    return VIDEO_INPUT_OK;
}

/**
 * Free packet resources
 */
void video_input_free_packet(frame_packet_t *packet) {
    if (!packet || !packet->private_data) {
        return;
    }
    
    AVPacket *av_packet = (AVPacket *)packet->private_data;
    av_packet_free(&av_packet);
    
    /* Clear packet structure */
    memset(packet, 0, sizeof(frame_packet_t));
}

/**
 * Seek to specific timestamp
 */
int video_input_seek(video_input_ctx_t *ctx, int64_t timestamp_us) {
    if (!ctx || ctx->video_stream_index < 0) {
        return VIDEO_INPUT_ERROR;
    }
    
    AVStream *stream = ctx->format_ctx->streams[ctx->video_stream_index];
    int64_t seek_target = av_rescale_q(timestamp_us, AV_TIME_BASE_Q, stream->time_base);
    
    int ret = av_seek_frame(ctx->format_ctx, ctx->video_stream_index, 
                           seek_target, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        fprintf(stderr, "Seek failed: %s\n", av_err2str(ret));
        return VIDEO_INPUT_ERROR;
    }
    
    return VIDEO_INPUT_OK;
}

/**
 * Get current playback position
 */
int64_t video_input_get_position(video_input_ctx_t *ctx) {
    if (!ctx || ctx->video_stream_index < 0) {
        return -1;
    }
    
    /* This is approximate - would need packet-level tracking for accuracy */
    AVStream *stream = ctx->format_ctx->streams[ctx->video_stream_index];
    int64_t pos = avio_tell(ctx->format_ctx->pb);
    
    /* Convert to timestamp (rough estimate) */
    if (stream->duration > 0) {
        int64_t file_size = avio_size(ctx->format_ctx->pb);
        if (file_size > 0) {
            double progress = (double)pos / file_size;
            return av_rescale_q((int64_t)(progress * stream->duration), 
                              stream->time_base, AV_TIME_BASE_Q);
        }
    }
    
    return 0;  /* Fallback */
}

/**
 * Close video input and free resources
 */
void video_input_destroy(video_input_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    
    if (ctx->format_ctx) {
        avformat_close_input(&ctx->format_ctx);
    }
    
    free(ctx);
}
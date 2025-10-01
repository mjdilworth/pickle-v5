/*
 * Hardware Decoder Implementation - FFmpeg h264_v4l2m2m for Raspberry Pi 4
 * 
 * Uses FFmpeg's h264_v4l2m2m decoder which wraps the RPi4's hardware video decode unit.
 * Implements zero-copy operation with DMABUF export for GPU texture import.
 */

#include "hw_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>

/* Forward declarations */
static int setup_drm_prime_context(hw_decoder_ctx_t *ctx);
static int extract_dmabuf_from_frame(AVFrame *frame, decoded_frame_t *decoded_frame);

/* Internal decoder context */
struct hw_decoder_ctx {
    /* FFmpeg decoder components */
    const AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVPacket *packet;
    AVFrame *frame;
    
    /* DRM Prime hardware context */
    AVBufferRef *hw_device_ctx;
    
    /* Stream configuration */
    int width, height;
    int configured;
    
    /* Statistics */
    uint64_t frames_decoded;
    uint64_t frames_dropped;
    uint64_t total_decode_time_us;
    struct timeval last_frame_time;
};

/**
 * Create hardware decoder context
 */
hw_decoder_ctx_t *hw_decoder_create(void) {
    hw_decoder_ctx_t *ctx = calloc(1, sizeof(hw_decoder_ctx_t));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate decoder context\n");
        return NULL;
    }
    
    /* Find h264_v4l2m2m decoder */
    ctx->codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    if (!ctx->codec) {
        fprintf(stderr, "h264_v4l2m2m decoder not found\n");
        free(ctx);
        return NULL;
    }
    
    /* Allocate codec context */
    ctx->codec_ctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        free(ctx);
        return NULL;
    }
    
    /* Allocate packet and frame */
    ctx->packet = av_packet_alloc();
    ctx->frame = av_frame_alloc();
    if (!ctx->packet || !ctx->frame) {
        fprintf(stderr, "Failed to allocate AVPacket/AVFrame\n");
        avcodec_free_context(&ctx->codec_ctx);
        av_packet_free(&ctx->packet);
        av_frame_free(&ctx->frame);
        free(ctx);
        return NULL;
    }
    
    printf("Created h264_v4l2m2m decoder context\n");
    return ctx;
}

/**
 * Setup DRM Prime hardware context for zero-copy operation
 */
static int setup_drm_prime_context(hw_decoder_ctx_t *ctx) {
    int ret;
    
    /* Create DRM device context */
    ret = av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_DRM, 
                                "/dev/dri/card1", NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to create DRM device context: %s\n", av_err2str(ret));
        return HW_DECODER_ERROR;
    }
    
    /* Set hardware device context */
    ctx->codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
    printf("DRM Prime hardware context configured\n");
    return HW_DECODER_OK;
}

/**
 * Allocate and map buffers
 */
static int allocate_buffers(hw_decoder_ctx_t *ctx) {
    struct v4l2_requestbuffers reqbufs;
    int ret, i;
    
    /* Allocate input buffers */
    memset(&reqbufs, 0, sizeof(reqbufs));
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;
    reqbufs.count = ctx->num_input_buffers;
    
    ret = ioctl(ctx->fd, VIDIOC_REQBUFS, &reqbufs);
    if (ret < 0) {
        fprintf(stderr, "Failed to request input buffers: %s\n", strerror(errno));
        return HW_DECODER_ERROR;
    }
    
    ctx->input_buffers = calloc(reqbufs.count, sizeof(buffer_info_t));
    if (!ctx->input_buffers) {
        return HW_DECODER_ERROR;
    }
    
    /* Map input buffers */
    for (i = 0; i < (int)reqbufs.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane plane;
        
        memset(&buf, 0, sizeof(buf));
        memset(&plane, 0, sizeof(plane));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = &plane;
        buf.length = 1;
        
        ret = ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf);
        if (ret < 0) {
            fprintf(stderr, "Failed to query input buffer %d: %s\n", i, strerror(errno));
            return HW_DECODER_ERROR;
        }
        
        ctx->input_buffers[i].mmap_length = plane.length;
        ctx->input_buffers[i].mmap_addr = mmap(NULL, plane.length,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, ctx->fd, plane.m.mem_offset);
        
        if (ctx->input_buffers[i].mmap_addr == MAP_FAILED) {
            fprintf(stderr, "Failed to mmap input buffer %d: %s\n", i, strerror(errno));
            return HW_DECODER_ERROR;
        }
    }
    
    /* Allocate output buffers */
    memset(&reqbufs, 0, sizeof(reqbufs));
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;
    reqbufs.count = ctx->num_output_buffers;
    
    ret = ioctl(ctx->fd, VIDIOC_REQBUFS, &reqbufs);
    if (ret < 0) {
        fprintf(stderr, "Failed to request output buffers: %s\n", strerror(errno));
        return HW_DECODER_ERROR;
    }
    
    ctx->output_buffers = calloc(reqbufs.count, sizeof(buffer_info_t));
    if (!ctx->output_buffers) {
        return HW_DECODER_ERROR;
    }
    
    /* Map output buffers and export DMABUFs */
    for (i = 0; i < (int)reqbufs.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[3];
        struct v4l2_exportbuffer expbuf;
        
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = (ctx->output_format == V4L2_PIX_FMT_NV12) ? 2 : 3;
        
        ret = ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf);
        if (ret < 0) {
            fprintf(stderr, "Failed to query output buffer %d: %s\n", i, strerror(errno));
            return HW_DECODER_ERROR;
        }
        
        /* Try to export DMABUF, but continue without it if it fails (permission issue) */
        memset(&expbuf, 0, sizeof(expbuf));
        expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        expbuf.index = i;
        expbuf.plane = 0;
        
        ret = ioctl(ctx->fd, VIDIOC_EXPBUF, &expbuf);
        if (ret < 0) {
            printf("Failed to export DMABUF for buffer %d: %s (errno=%d)\n", 
                   i, strerror(errno), errno);
            return HW_DECODER_ERROR;
        } else {
            printf("✓ DMABUF exported successfully for buffer %d: fd=%d\n", i, expbuf.fd);
            ctx->output_buffers[i].dmabuf_fd = expbuf.fd;
        }
    }
    
    /* Queue all output buffers initially so decoder can fill them */
    for (i = 0; i < (int)reqbufs.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[3];
        
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = (ctx->output_format == V4L2_PIX_FMT_NV12) ? 2 : 3;
        
        ret = ioctl(ctx->fd, VIDIOC_QBUF, &buf);
        if (ret < 0) {
            fprintf(stderr, "Failed to queue initial output buffer %d: %s\n", i, strerror(errno));
            return HW_DECODER_ERROR;
        }
        
        /* Debug: Verify the buffer was actually queued */
        struct v4l2_buffer verify_buf;
        struct v4l2_plane verify_planes[3];
        memset(&verify_buf, 0, sizeof(verify_buf));
        memset(verify_planes, 0, sizeof(verify_planes));
        verify_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        verify_buf.memory = V4L2_MEMORY_MMAP;
        verify_buf.index = i;
        verify_buf.m.planes = verify_planes;
        verify_buf.length = (ctx->output_format == V4L2_PIX_FMT_NV12) ? 2 : 3;
        ret = ioctl(ctx->fd, VIDIOC_QUERYBUF, &verify_buf);
        if (ret == 0) {
            printf("Buffer %d queued: flags=0x%x (QUEUED=%s, DONE=%s, ERROR=%s)\n", i, verify_buf.flags,
                   (verify_buf.flags & V4L2_BUF_FLAG_QUEUED) ? "YES" : "NO",
                   (verify_buf.flags & V4L2_BUF_FLAG_DONE) ? "YES" : "NO", 
                   (verify_buf.flags & V4L2_BUF_FLAG_ERROR) ? "YES" : "NO");
        } else {
            printf("Failed to verify buffer %d: %s\n", i, strerror(errno));
        }
    }
    
    printf("Allocated %d input and %d output buffers\n", 
           ctx->num_input_buffers, ctx->num_output_buffers);
    printf("Queued %d output buffers for capture\n", (int)reqbufs.count);
    
    return HW_DECODER_OK;
}

/**
 * Configure decoder with stream parameters
 */
int hw_decoder_configure(hw_decoder_ctx_t *ctx, const video_stream_info_t *stream_info) {
    int ret;
    
    if (!ctx || !stream_info) {
        return HW_DECODER_ERROR;
    }
    
    /* Set codec parameters */
    ctx->codec_ctx->width = stream_info->width;
    ctx->codec_ctx->height = stream_info->height;
    ctx->codec_ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;  /* Request DRM Prime format for zero-copy */
    
    /* Set H.264 extradata if available */
    if (stream_info->extradata && stream_info->extradata_size > 0) {
        printf("H.264 extradata found: %d bytes (SPS/PPS parameters)\n", stream_info->extradata_size);
        
        ctx->codec_ctx->extradata = av_mallocz(stream_info->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->codec_ctx->extradata) {
            fprintf(stderr, "Failed to allocate extradata buffer\n");
            return HW_DECODER_ERROR;
        }
        
        memcpy(ctx->codec_ctx->extradata, stream_info->extradata, stream_info->extradata_size);
        ctx->codec_ctx->extradata_size = stream_info->extradata_size;
    }
    
    /* Setup hardware context */
    ret = setup_drm_prime_context(ctx);
    if (ret < 0) {
        return ret;
    }
    
    /* Open codec */
    ret = avcodec_open2(ctx->codec_ctx, ctx->codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to open h264_v4l2m2m codec: %s\n", av_err2str(ret));
        return HW_DECODER_ERROR;
    }
        
        /* TEMPORARY: Skip AVC conversion for debugging */
        printf("WARNING: Skipping AVC-to-Annex-B conversion for debugging\n");
        uint8_t *annexb_data = malloc(stream_info->extradata_size);
        memcpy(annexb_data, stream_info->extradata, stream_info->extradata_size);
        int annexb_size = stream_info->extradata_size;
        
        /* Find an available input buffer for extradata */
        int buf_index = -1;
        for (int i = 0; i < ctx->num_input_buffers; i++) {
            if (!ctx->input_buffers[i].queued) {
                buf_index = i;
                break;
            }
        }
        
        if (buf_index >= 0) {
            struct v4l2_buffer buf;
            struct v4l2_plane plane;
            
            /* Copy converted extradata to input buffer */
            if (annexb_size <= ctx->input_buffer_size) {
                memcpy(ctx->input_buffers[buf_index].mmap_addr, annexb_data, annexb_size);
                
                /* Queue extradata buffer */
                memset(&buf, 0, sizeof(buf));
                memset(&plane, 0, sizeof(plane));
                buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = buf_index;
                buf.m.planes = &plane;
                buf.length = 1;
                plane.bytesused = annexb_size;
                plane.length = ctx->input_buffer_size;
                
                ret = ioctl(ctx->fd, VIDIOC_QBUF, &buf);
                if (ret < 0) {
                    fprintf(stderr, "Failed to queue extradata buffer: %s\n", strerror(errno));
                    free(annexb_data);
                    return HW_DECODER_ERROR;
                } else {
                    ctx->input_buffers[buf_index].queued = 1;
                    ctx->input_queue_count++;
                    printf("✓ H.264 extradata converted and sent to decoder successfully\n");
                }
            } else {
                fprintf(stderr, "Warning: Converted extradata too large (%d > %d)\n", 
                        annexb_size, ctx->input_buffer_size);
                free(annexb_data);
                return HW_DECODER_ERROR;
            }
        } else {
            fprintf(stderr, "No available input buffer for extradata\n");
            free(annexb_data);
            return HW_DECODER_ERROR;
        }
        
        /* Free the temporary converted data */
        free(annexb_data);
    }

    ctx->configured = 1;
    return HW_DECODER_OK;
}

/**
 * Set buffer configuration
 */
int hw_decoder_set_buffer_config(hw_decoder_ctx_t *ctx, const decoder_buffer_config_t *config) {
    if (!ctx || !config || ctx->configured) {
        return HW_DECODER_ERROR;
    }
    
    ctx->num_input_buffers = config->num_input_buffers;
    ctx->num_output_buffers = config->num_output_buffers;
    ctx->input_buffer_size = config->input_buffer_size;
    
    return HW_DECODER_OK;
}

/**
 * Submit compressed packet for decoding
 */
int hw_decoder_submit_packet(hw_decoder_ctx_t *ctx, const frame_packet_t *packet) {
    struct v4l2_buffer buf;
    struct v4l2_plane plane;
    int ret, buf_index = -1;
    
    if (!ctx || !packet || !ctx->configured) {
        return HW_DECODER_ERROR;
    }
    
    /* First, try to dequeue consumed input buffers */
    dequeue_input_buffers(ctx);
    
    /* Find available input buffer */
    for (int i = 0; i < ctx->num_input_buffers; i++) {
        if (!ctx->input_buffers[i].queued) {
            buf_index = i;
            break;
        }
    }
    
    if (buf_index < 0) {
        return HW_DECODER_EAGAIN;  /* No buffers available */
    }
    
    /* TEMPORARY: Skip packet conversion for debugging */
    if (packet->size > ctx->input_buffer_size) {
        fprintf(stderr, "Packet too large for buffer: %d > %d\n", 
                packet->size, ctx->input_buffer_size);
        return HW_DECODER_ERROR;
    }
    
    memcpy(ctx->input_buffers[buf_index].mmap_addr, packet->data, packet->size);
    int converted_size = packet->size;
    
    /* Queue buffer */
    memset(&buf, 0, sizeof(buf));
    memset(&plane, 0, sizeof(plane));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buf_index;
    buf.m.planes = &plane;
    buf.length = 1;
    
    plane.bytesused = converted_size;
    plane.length = ctx->input_buffer_size;
    
    /* Set timestamp if available */
    if (packet->pts != AV_NOPTS_VALUE) {
        buf.timestamp.tv_sec = packet->pts / 1000000;
        buf.timestamp.tv_usec = packet->pts % 1000000;
    }
    
    ret = ioctl(ctx->fd, VIDIOC_QBUF, &buf);
    if (ret < 0) {
        fprintf(stderr, "Failed to queue input buffer: %s\n", strerror(errno));
        return HW_DECODER_ERROR;
    }

    ctx->input_buffers[buf_index].queued = 1;
    ctx->input_queue_count++;
    
    /* Debug input queue status */
    static int submit_count = 0;
    submit_count++;
    if (submit_count <= 10) {
        printf("✓ Submitted packet %d (AVC: %d bytes → Annex-B: %d bytes) to input buffer %d, queue_count=%d\n", 
               submit_count, packet->size, converted_size, buf_index, ctx->input_queue_count);
    }    /* Start streaming if not already started */
    if (!ctx->streaming) {
        enum v4l2_buf_type type;
        
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ret = ioctl(ctx->fd, VIDIOC_STREAMON, &type);
        if (ret < 0) {
            fprintf(stderr, "Failed to start input stream: %s\n", strerror(errno));
            return HW_DECODER_ERROR;
        }
        
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ret = ioctl(ctx->fd, VIDIOC_STREAMON, &type);
        if (ret < 0) {
            fprintf(stderr, "Failed to start output stream: %s\n", strerror(errno));
            return HW_DECODER_ERROR;
        }
        
        ctx->streaming = 1;
        printf("Decoder streaming started (input and output streams active)\n");
        
        /* Debug: Check output buffer status immediately after streaming starts */
        printf("Checking output buffer status after STREAMON:\n");
        for (int i = 0; i < ctx->num_output_buffers; i++) {
            struct v4l2_buffer check_buf;
            struct v4l2_plane check_planes[3];
            memset(&check_buf, 0, sizeof(check_buf));
            memset(check_planes, 0, sizeof(check_planes));
            check_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            check_buf.memory = V4L2_MEMORY_MMAP;
            check_buf.index = i;
            check_buf.m.planes = check_planes;
            check_buf.length = (ctx->output_format == V4L2_PIX_FMT_NV12) ? 2 : 3;
            ret = ioctl(ctx->fd, VIDIOC_QUERYBUF, &check_buf);
            if (ret == 0) {
                printf("  Buffer %d: flags=0x%x (QUEUED=%s, DONE=%s)\n", i, check_buf.flags,
                       (check_buf.flags & V4L2_BUF_FLAG_QUEUED) ? "YES" : "NO",
                       (check_buf.flags & V4L2_BUF_FLAG_DONE) ? "YES" : "NO");
            } else {
                printf("  Buffer %d: Failed to query: %s\n", i, strerror(errno));
            }
        }
    }
    
    return HW_DECODER_OK;
}

/**
 * Dequeue consumed input buffers to free up space
 */
static void dequeue_input_buffers(hw_decoder_ctx_t *ctx) {
    struct v4l2_buffer buf;
    struct v4l2_plane plane;
    int ret;
    
    if (!ctx || !ctx->streaming) {
        return;
    }
    
    /* Try to dequeue consumed input buffers */
    while (ctx->input_queue_count > 0) {
        memset(&buf, 0, sizeof(buf));
        memset(&plane, 0, sizeof(plane));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = &plane;
        buf.length = 1;
        
        ret = ioctl(ctx->fd, VIDIOC_DQBUF, &buf);
        if (ret < 0) {
            if (errno == EAGAIN) {
                break;  /* No more consumed buffers */
            }
            fprintf(stderr, "Failed to dequeue input buffer: %s\n", strerror(errno));
            break;
        }
        
        /* Mark buffer as available */
        ctx->input_buffers[buf.index].queued = 0;
        ctx->input_queue_count--;
        
        static int dequeue_count = 0;
        dequeue_count++;
        if (dequeue_count <= 10) {
            printf("✓ Dequeued consumed input buffer %d, queue_count=%d\n", 
                   buf.index, ctx->input_queue_count);
        }
    }
}

/**
 * Get decoded frame (non-blocking)
 */
int hw_decoder_get_frame(hw_decoder_ctx_t *ctx, decoded_frame_t *frame) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[3];
    int ret;
    
    if (!ctx || !frame || !ctx->configured) {
        return HW_DECODER_ERROR;
    }
    
    /* Dequeue consumed input buffers first */
    dequeue_input_buffers(ctx);
    
    /* Try to dequeue output buffer */
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = (ctx->output_format == V4L2_PIX_FMT_NV12) ? 2 : 3;
    
    ret = ioctl(ctx->fd, VIDIOC_DQBUF, &buf);
    if (ret < 0) {
        if (errno == EAGAIN) {
            /* Debug: Check decoder state more thoroughly */
            static int debug_count = 0;
            debug_count++;
            if (debug_count <= 5 || debug_count % 50 == 0) {
                /* Check if there are queued output buffers waiting */
                struct v4l2_buffer query_buf;
                struct v4l2_plane query_planes[3];
                int queued_outputs = 0;
                for (int i = 0; i < ctx->num_output_buffers; i++) {
                    memset(&query_buf, 0, sizeof(query_buf));
                    memset(query_planes, 0, sizeof(query_planes));
                    query_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                    query_buf.memory = V4L2_MEMORY_MMAP;
                    query_buf.index = i;
                    query_buf.m.planes = query_planes;
                    query_buf.length = (ctx->output_format == V4L2_PIX_FMT_NV12) ? 2 : 3;
                    if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &query_buf) == 0) {
                        if (query_buf.flags & V4L2_BUF_FLAG_QUEUED) {
                            queued_outputs++;
                        }
                    }
                }
                printf("Debug [%d]: Decoder EAGAIN - input_queue=%d, output_queued=%d, streaming=%d\n", 
                       debug_count, ctx->input_queue_count, queued_outputs, ctx->streaming);
            }
            return HW_DECODER_EAGAIN;
        }
        fprintf(stderr, "Failed to dequeue output buffer: %s (errno=%d)\n", strerror(errno), errno);
        return HW_DECODER_ERROR;
    }
    
    /* DEBUG: If we get here, we successfully dequeued an output buffer! */
    printf("🎉 SUCCESS: Dequeued output buffer %d with %d bytes!\n", buf.index, buf.m.planes[0].bytesused);
    
    /* Fill frame structure */
    frame->width = ctx->width;
    frame->height = ctx->height;
    frame->format = ctx->output_format;
    frame->buffer_index = buf.index;
    frame->timestamp_us = buf.timestamp.tv_sec * 1000000LL + buf.timestamp.tv_usec;
    
    /* Set DMABUF handle */
    frame->dmabuf_fd[0] = ctx->output_buffers[buf.index].dmabuf_fd;
    frame->num_planes = buf.length;
    
    /* Set plane information */
    for (int i = 0; i < (int)buf.length; i++) {
        frame->offsets[i] = planes[i].data_offset;
        frame->pitches[i] = planes[i].length;  /* This should be stride, but V4L2 doesn't provide it directly */
        frame->sizes[i] = planes[i].bytesused;
    }
    
    frame->private_data = ctx;  /* For release function */
    
    /* Update statistics */
    ctx->frames_decoded++;
    gettimeofday(&ctx->last_frame_time, NULL);
    
    return HW_DECODER_OK;
}

/**
 * Release frame buffer back to decoder
 */
void hw_decoder_release_frame(hw_decoder_ctx_t *ctx, decoded_frame_t *frame) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[3];
    
    if (!ctx || !frame) {
        return;
    }
    
    /* Re-queue the buffer */
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = frame->buffer_index;
    buf.m.planes = planes;
    buf.length = frame->num_planes;
    
    if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "Failed to re-queue output buffer %d: %s\n", 
                frame->buffer_index, strerror(errno));
    }
    
    /* Clear frame structure */
    memset(frame, 0, sizeof(decoded_frame_t));
}

/**
 * Convert AVC configuration record to Annex-B format SPS/PPS
 */
static int convert_avc_extradata_to_annexb(const uint8_t *avc_extradata, int avc_size, uint8_t **annexb_data, int *annexb_size) {
    if (!avc_extradata || avc_size < 8 || !annexb_data || !annexb_size) {
        return -1;
    }
    
    const uint8_t *data = avc_extradata;
    int pos = 0;
    
    /* Check AVC configuration record magic */
    if (data[0] != 0x01) {
        fprintf(stderr, "Invalid AVC configuration record\n");
        return -1;
    }
    
    /* Skip version, profile, level, reserved, NAL length size */
    pos = 5;
    
    /* Get number of SPS */
    int sps_count = data[pos] & 0x1f;
    pos++;
    
    /* Calculate total size needed */
    int total_size = 0;
    int temp_pos = pos;
    
    /* Calculate SPS size */
    for (int i = 0; i < sps_count; i++) {
        if (temp_pos + 2 > avc_size) return -1;
        int sps_length = (data[temp_pos] << 8) | data[temp_pos + 1];
        total_size += 4 + sps_length;  /* start code + SPS */
        temp_pos += 2 + sps_length;
    }
    
    /* Get number of PPS */
    if (temp_pos >= avc_size) return -1;
    int pps_count = data[temp_pos];
    temp_pos++;
    
    /* Calculate PPS size */
    for (int i = 0; i < pps_count; i++) {
        if (temp_pos + 2 > avc_size) return -1;
        int pps_length = (data[temp_pos] << 8) | data[temp_pos + 1];
        total_size += 4 + pps_length;  /* start code + PPS */
        temp_pos += 2 + pps_length;
    }
    
    /* Allocate output buffer */
    uint8_t *output = malloc(total_size);
    if (!output) return -1;
    
    int out_pos = 0;
    
    /* Convert SPS */
    for (int i = 0; i < sps_count; i++) {
        int sps_length = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        
        /* Add start code */
        output[out_pos++] = 0x00;
        output[out_pos++] = 0x00;
        output[out_pos++] = 0x00;
        output[out_pos++] = 0x01;
        
        /* Copy SPS data */
        memcpy(&output[out_pos], &data[pos], sps_length);
        out_pos += sps_length;
        pos += sps_length;
    }
    
    /* Get number of PPS */
    pps_count = data[pos];
    pos++;
    
    /* Convert PPS */
    for (int i = 0; i < pps_count; i++) {
        int pps_length = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        
        /* Add start code */
        output[out_pos++] = 0x00;
        output[out_pos++] = 0x00;
        output[out_pos++] = 0x00;
        output[out_pos++] = 0x01;
        
        /* Copy PPS data */
        memcpy(&output[out_pos], &data[pos], pps_length);
        out_pos += pps_length;
        pos += pps_length;
    }
    
    *annexb_data = output;
    *annexb_size = out_pos;
    
    printf("Converted AVC extradata (%d bytes) to Annex-B format (%d bytes)\n", avc_size, out_pos);
    return 0;
}

/**
 * Convert AVC packet (length-prefixed NALUs) to Annex-B format (start code prefixed)
 */
static int convert_avc_packet_to_annexb(const uint8_t *avc_data, int avc_size, uint8_t *annexb_buffer, int buffer_size) {
    if (!avc_data || !annexb_buffer || avc_size <= 0) {
        return -1;
    }
    
    int pos = 0;
    int out_pos = 0;
    
    while (pos < avc_size) {
        /* Check if we have enough data for length field */
        if (pos + 4 > avc_size) {
            fprintf(stderr, "Invalid AVC packet: incomplete length field at pos %d\n", pos);
            return -1;
        }
        
        /* Read NALU length (4 bytes, big-endian) */
        int nalu_length = (avc_data[pos] << 24) | (avc_data[pos + 1] << 16) | 
                          (avc_data[pos + 2] << 8) | avc_data[pos + 3];
        pos += 4;
        
        /* Validate NALU length */
        if (nalu_length <= 0 || pos + nalu_length > avc_size) {
            fprintf(stderr, "Invalid NALU length %d at pos %d (remaining: %d)\n", 
                    nalu_length, pos, avc_size - pos);
            return -1;
        }
        
        /* Check output buffer space */
        if (out_pos + 4 + nalu_length > buffer_size) {
            fprintf(stderr, "Output buffer too small\n");
            return -1;
        }
        
        /* Write start code */
        annexb_buffer[out_pos++] = 0x00;
        annexb_buffer[out_pos++] = 0x00;
        annexb_buffer[out_pos++] = 0x00;
        annexb_buffer[out_pos++] = 0x01;
        
        /* Copy NALU data */
        memcpy(&annexb_buffer[out_pos], &avc_data[pos], nalu_length);
        out_pos += nalu_length;
        pos += nalu_length;
    }
    
    return out_pos;
}

/**
 * Check if hardware decoder is available
 */
int hw_decoder_is_available(void) {
    int fd = open(V4L2_DECODER_DEVICE, O_RDWR);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return 0;
}

/**
 * Destroy decoder and free resources
 */
void hw_decoder_destroy(hw_decoder_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    
    /* Stop streaming */
    if (ctx->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    }
    
    /* Free input buffers */
    if (ctx->input_buffers) {
        for (int i = 0; i < ctx->num_input_buffers; i++) {
            if (ctx->input_buffers[i].mmap_addr != MAP_FAILED) {
                munmap(ctx->input_buffers[i].mmap_addr, ctx->input_buffers[i].mmap_length);
            }
        }
        free(ctx->input_buffers);
    }
    
    /* Free output buffers */
    if (ctx->output_buffers) {
        for (int i = 0; i < ctx->num_output_buffers; i++) {
            if (ctx->output_buffers[i].dmabuf_fd >= 0) {
                close(ctx->output_buffers[i].dmabuf_fd);
            }
        }
        free(ctx->output_buffers);
    }
    
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    
    free(ctx);
}
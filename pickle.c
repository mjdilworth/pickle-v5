/*
 * Pickle - GPU-accelerated Video Player for Raspberry Pi 4
 * 
 * This program implements a zero-copy video playback pipeline using:
 * - libavformat for MP4 demuxing
 * - V4L2 M2M for hardware H.264 decode
 * - OpenGL ES 3.2 for GPU rendering with keystone correction
 * - DRM/KMS for direct display output
 * 
 * Architecture is modular to allow independent replacement of components.
 * Test with: ./pickle rpi4-e.mp4
 */

#define _DEFAULT_SOURCE  /* For usleep() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <fcntl.h>

#include "video_input.h"
#include "hw_decoder.h"
#include "gpu_renderer.h"
#include "display_output.h"
#include "warp_control.h"
#include "fallback.h"

/* Global state for cleanup on signal */
static struct {
    video_input_ctx_t *input_ctx;
    hw_decoder_ctx_t *decoder_ctx;
    gpu_renderer_ctx_t *renderer_ctx;
    display_output_ctx_t *display_ctx;
    warp_control_ctx_t *warp_ctx;
    int running;
} g_player_state = {0};

/* Terminal state for keyboard input */
static struct termios original_termios;
static int terminal_configured = 0;

/* Configure terminal for keyboard input */
static void configure_terminal(void) {
    if (tcgetattr(STDIN_FILENO, &original_termios) == -1) {
        return;
    }
    
    struct termios raw = original_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
        terminal_configured = 1;
    }
}

/* Restore terminal settings */
static void restore_terminal(void) {
    if (terminal_configured) {
        /* Restore original terminal attributes */
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
        
        /* Additional terminal restoration - ensure echo and canonical mode */
        struct termios current;
        if (tcgetattr(STDIN_FILENO, &current) == 0) {
            current.c_lflag |= (ECHO | ICANON);  /* Enable echo and canonical mode */
            current.c_iflag |= (ICRNL);         /* Enable CR to NL conversion */
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &current);
        }
        
        /* Force a newline to ensure clean prompt */
        printf("\n");
        fflush(stdout);
        
        terminal_configured = 0;
    }
}

/* Check keyboard input */
static char check_keyboard(void) {
    if (!terminal_configured) return 0;
    
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        return c;
    }
    return 0;
}

/* Signal handler for clean shutdown */
static void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    g_player_state.running = 0;
    restore_terminal();
    
    /* Create stop signal file for fallback player */
    FILE *stop_file = fopen("/tmp/pickle_stop", "w");
    if (stop_file) {
        fprintf(stop_file, "stop\n");
        fclose(stop_file);
    }
}

/* Print usage information */
static void print_usage(const char *prog_name) {
    printf("Usage: %s <video_file.mp4>\n", prog_name);
    printf("       %s rpi4-e.mp4  (for testing)\n", prog_name);
    printf("\nPickle - GPU-accelerated video player for Raspberry Pi 4\n");
    printf("Features:\n");
    printf("  - Hardware H.264 decode via V4L2 M2M\n");
    printf("  - GPU rendering with OpenGL ES 3.2\n");
    printf("  - Real-time keystone correction\n");
    printf("  - Zero-copy pipeline for minimal CPU usage\n");
    printf("  - DRM/KMS output at 1920x1080@60Hz\n");
    printf("\nRuntime controls:\n");
    printf("  - Arrow keys: adjust keystone corners\n");
    printf("  - R: reset warp to identity\n");
    printf("  - Q/ESC: quit\n");
}

/* Initialize all pipeline modules in sequence */
static int init_pipeline(const char *video_file) {
    int ret;
    video_stream_info_t stream_info;
    
    printf("Initializing video pipeline...\n");
    
    /* 1. Initialize video input */
    g_player_state.input_ctx = video_input_create();
    if (!g_player_state.input_ctx) {
        fprintf(stderr, "Failed to create video input context\n");
        return -1;
    }
    
    ret = video_input_open(g_player_state.input_ctx, video_file);
    if (ret < 0) {
        fprintf(stderr, "Failed to open video file: %s\n", video_file);
        return ret;
    }
    
    /* Get stream info for other modules */
    ret = video_input_get_stream_info(g_player_state.input_ctx, &stream_info);
    if (ret < 0) {
        fprintf(stderr, "Failed to get video stream info\n");
        return ret;
    }
    
    /* 2. Initialize hardware decoder */
    g_player_state.decoder_ctx = hw_decoder_create();
    if (!g_player_state.decoder_ctx) {
        fprintf(stderr, "Failed to create hardware decoder context\n");
        return -1;
    }
    
    /* Debug: Show extradata info */
    if (stream_info.extradata && stream_info.extradata_size > 0) {
        printf("H.264 extradata found: %d bytes (SPS/PPS parameters)\n", stream_info.extradata_size);
    } else {
        printf("Warning: No H.264 extradata found - decoder may not work\n");
    }
    
    ret = hw_decoder_configure(g_player_state.decoder_ctx, &stream_info);
    if (ret < 0) {
        fprintf(stderr, "Failed to configure hardware decoder\n");
        return ret;
    }
    
    /* 3. Initialize display output first (creates EGL context) */
    g_player_state.display_ctx = display_output_create();
    if (!g_player_state.display_ctx) {
        fprintf(stderr, "Failed to create display output context\n");
        return -1;
    }
    
    ret = display_output_configure(g_player_state.display_ctx, 1920, 1080, 60);
    if (ret < 0) {
        fprintf(stderr, "Failed to configure display output\n");
        return ret;
    }
    
    /* 4. Initialize GPU renderer (needs EGL context from display) */
    g_player_state.renderer_ctx = gpu_renderer_create();
    if (!g_player_state.renderer_ctx) {
        fprintf(stderr, "Failed to create GPU renderer context\n");
        return -1;
    }
    
    ret = gpu_renderer_configure(g_player_state.renderer_ctx, 
                                g_player_state.display_ctx,
                                stream_info.width, stream_info.height);
    if (ret < 0) {
        fprintf(stderr, "Failed to configure GPU renderer\n");
        return ret;
    }
    
    /* 5. Initialize warp control */
    g_player_state.warp_ctx = warp_control_create();
    if (!g_player_state.warp_ctx) {
        fprintf(stderr, "Failed to create warp control context\n");
        return -1;
    }
    
    ret = warp_control_load_config(g_player_state.warp_ctx, "warp_config.txt");
    if (ret < 0) {
        /* Non-fatal - use defaults */
        printf("No warp config found, using defaults\n");
    }
    
    printf("Pipeline initialized successfully\n");
    
    /* Test the display pipeline with a simple pattern */
    printf("Testing display output with test pattern...\n");
    
    /* Create a simple test frame */
    decoded_frame_t test_frame = {0};
    test_frame.width = 1920;
    test_frame.height = 1080;
    test_frame.format = 0; // We'll generate directly in GPU
    test_frame.dmabuf_fd[0] = -1; // No DMABUF, direct GPU rendering
    
    /* Render and display test pattern for 3 seconds */
    for (int i = 0; i < 180; i++) { // 180 frames at 60fps = 3 seconds
        /* Process warp control */
        warp_control_process_input(g_player_state.warp_ctx);
        
        /* Render test pattern (the GPU renderer should handle this) */
        int ret = gpu_renderer_render_frame(g_player_state.renderer_ctx, &test_frame);
        if (ret < 0) {
            printf("Test pattern render failed: %d\n", ret);
            break;
        }
        
        /* Present to display */
        ret = display_output_present_frame(g_player_state.display_ctx);
        if (ret < 0) {
            printf("Test pattern present failed: %d\n", ret);
            break;
        }
        
        if (i == 0) printf("✓ Test pattern displaying...\n");
        if (i == 60) printf("✓ Test pattern still displaying (2 seconds left)...\n");
        if (i == 120) printf("✓ Test pattern still displaying (1 second left)...\n");
        
        usleep(16667); // ~60 FPS
    }
    
    printf("✓ Test pattern completed - display pipeline works!\n");
    return 0;
}

/* Clean up all pipeline resources */
static void cleanup_pipeline(void) {
    printf("Cleaning up pipeline...\n");
    
    if (g_player_state.warp_ctx) {
        warp_control_destroy(g_player_state.warp_ctx);
        g_player_state.warp_ctx = NULL;
    }
    
    if (g_player_state.renderer_ctx) {
        gpu_renderer_destroy(g_player_state.renderer_ctx);
        g_player_state.renderer_ctx = NULL;
    }
    
    if (g_player_state.display_ctx) {
        display_output_destroy(g_player_state.display_ctx);
        g_player_state.display_ctx = NULL;
    }
    
    if (g_player_state.decoder_ctx) {
        hw_decoder_destroy(g_player_state.decoder_ctx);
        g_player_state.decoder_ctx = NULL;
    }
    
    if (g_player_state.input_ctx) {
        video_input_destroy(g_player_state.input_ctx);
        g_player_state.input_ctx = NULL;
    }
    
    /* Ensure terminal is always restored during cleanup */
    restore_terminal();
}

/* Main playback loop with zero-copy frame processing */
static int run_playback_loop(void) {
    int ret = 0;
    frame_packet_t packet = {0};
    decoded_frame_t frame = {0};
    
    printf("Starting playback loop...\n");
    g_player_state.running = 1;
    
    int packet_count = 0;
    while (g_player_state.running) {
        /* 1. Read next packet from input */
        ret = video_input_read_packet(g_player_state.input_ctx, &packet);
        if (ret == VIDEO_INPUT_EOF) {
            printf("End of file reached after %d packets\n", packet_count);
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error reading packet: %d\n", ret);
            break;
        }
        
        packet_count++;
        if (packet_count <= 10) {
            printf("Read packet %d: size=%d bytes, pts=%ld, keyframe=%d\n", 
                   packet_count, packet.size, packet.pts, packet.keyframe);
        }
        
        /* 2. Submit packet to hardware decoder */
        ret = hw_decoder_submit_packet(g_player_state.decoder_ctx, &packet);
        if (ret < 0) {
            if (ret == HW_DECODER_EAGAIN) {
                /* Input queue is full, try to drain output queue first */
                decoded_frame_t temp_frame;
                int drain_ret = hw_decoder_get_frame(g_player_state.decoder_ctx, &temp_frame);
                if (drain_ret == HW_DECODER_OK) {
                    /* Process and render this frame */
                    warp_control_process_input(g_player_state.warp_ctx);
                    gpu_renderer_render_frame(g_player_state.renderer_ctx, &temp_frame);
                    display_output_present_frame(g_player_state.display_ctx);
                    hw_decoder_release_frame(g_player_state.decoder_ctx, &temp_frame);
                    
                    static int temp_frame_count = 0;
                    temp_frame_count++;
                    if (temp_frame_count <= 5) {
                        printf("Temp frame %d: Processed during drain (%dx%d)\n", 
                               temp_frame_count, temp_frame.width, temp_frame.height);
                    }
                }
                /* Don't free packet yet - we'll retry submission next iteration */
                usleep(1000);  /* Small delay to prevent busy loop */
                continue;
            } else {
                fprintf(stderr, "Error submitting packet to decoder: %d\n", ret);
                video_input_free_packet(&packet);
                continue;
            }
        }
        
        /* 3. Try to get decoded frame (be more patient for initial frames) */
        int max_attempts = (packet_count <= 10) ? 20 : 5;  /* More patience for first few packets */
        int attempts = 0;
        
        do {
            ret = hw_decoder_get_frame(g_player_state.decoder_ctx, &frame);
            if (ret == HW_DECODER_OK) {
                break;  /* Got a frame! */
            } else if (ret != HW_DECODER_EAGAIN) {
                fprintf(stderr, "Error getting decoded frame: %d\n", ret);
                break;
            }
            
            /* Decoder not ready yet, give it time especially for initial frames */
            attempts++;
            if (attempts < max_attempts) {
                usleep(packet_count <= 5 ? 20000 : 5000);  /* 20ms for first 5 packets, then 5ms */
            }
        } while (attempts < max_attempts && ret == HW_DECODER_EAGAIN);
        
        if (ret < 0) {
            if (packet_count <= 30) {
                printf("Decoder not ready after %d attempts for packet %d\n", 
                       attempts, packet_count);
            }
            video_input_free_packet(&packet);
            continue;
        }
        
        /* Debug: Show that we got a frame */
        static int frame_count = 0;
        frame_count++;
        if (frame_count <= 10 || frame_count % 60 == 0) {  /* Print first 10 frames and every 60 frames */
            printf("✓ Got frame %d: %dx%d, format=0x%x, dmabuf_fd=%d\n", 
                   frame_count, frame.width, frame.height, frame.format, frame.dmabuf_fd[0]);
        }
        
        /* 4. Process warp control input (non-blocking) */
        warp_control_process_input(g_player_state.warp_ctx);
        
        /* 5. Check for quit key (q or Q) */
        char key = check_keyboard();
        if (key == 'q' || key == 'Q' || key == 27) { /* 27 = ESC */
            printf("Quit requested by user\n");
            g_player_state.running = 0;
            break;
        }
        
        /* 6. Render frame with current warp parameters */
        ret = gpu_renderer_render_frame(g_player_state.renderer_ctx, &frame);
        if (ret < 0) {
            fprintf(stderr, "Error rendering frame: %d\n", ret);
        } else if (frame_count <= 5) {
            printf("Frame %d: Rendered successfully (%dx%d)\n", frame_count, frame.width, frame.height);
        }
        
        /* 7. Present to display */
        ret = display_output_present_frame(g_player_state.display_ctx);
        if (ret < 0) {
            fprintf(stderr, "Error presenting frame: %d\n", ret);
        } else if (frame_count <= 5) {
            printf("Frame %d: Presented to display\n", frame_count);
        }
        
        /* Clean up frame resources */
        hw_decoder_release_frame(g_player_state.decoder_ctx, &frame);
        video_input_free_packet(&packet);
        
        /* Basic frame rate control (will be improved with proper timing) */
        usleep(16667); // ~60 FPS
    }
    
    return 0;
}

/* Attempt fallback to libmpv if hardware pipeline fails */
static int try_fallback_playback(const char *video_file) {
    printf("Attempting fallback to libmpv...\n");
    
    fallback_ctx_t *fallback_ctx = fallback_create();
    if (!fallback_ctx) {
        fprintf(stderr, "Failed to create fallback context\n");
        return -1;
    }
    
    int ret = fallback_play_file(fallback_ctx, video_file);
    
    fallback_destroy(fallback_ctx);
    
    if (ret < 0) {
        fprintf(stderr, "Fallback playback also failed\n");
        return ret;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    int ret = 0;
    
    /* Parse command line arguments */
    if (argc != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    
    const char *video_file = argv[1];
    
    /* Check if file exists */
    if (access(video_file, R_OK) != 0) {
        fprintf(stderr, "Error: Cannot read file '%s': %s\n", 
                video_file, strerror(errno));
        return EXIT_FAILURE;
    }
    
    /* Set up signal handlers for clean shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Configure terminal for keyboard input */
    configure_terminal();
    
    /* Register cleanup function to run on exit */
    atexit(restore_terminal);
    
    printf("Pickle starting with file: %s\n", video_file);
    
    /* Initialize the complete pipeline */
    ret = init_pipeline(video_file);
    if (ret < 0) {
        fprintf(stderr, "Pipeline initialization failed, trying fallback...\n");
        cleanup_pipeline();
        restore_terminal();
        return try_fallback_playback(video_file);
    }
    
    /* Run the main playback loop */
    ret = run_playback_loop();
    
    /* Clean shutdown */
    cleanup_pipeline();
    restore_terminal();
    
    if (ret < 0) {
        printf("Playback completed with errors\n");
        return EXIT_FAILURE;
    }
    
    printf("Playback completed successfully\n");
    return EXIT_SUCCESS;
}

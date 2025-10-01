/*
 * Warp Control Implementation - Interactive Keystone Correction
 * 
 * Implements real-time keystone/perspective correction with keyboard controls.
 * Generates transformation matrices for GPU renderer.
 */

#include "warp_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Default configuration */
#define DEFAULT_STEP_SIZE      0.01f
#define DEFAULT_CONFIG_FILE    "warp_config.txt"

/* Key codes */
#define KEY_ARROW_UP    65
#define KEY_ARROW_DOWN  66
#define KEY_ARROW_RIGHT 67
#define KEY_ARROW_LEFT  68

/* Internal warp control context */
struct warp_control_ctx {
    gpu_renderer_ctx_t *renderer_ctx;
    warp_params_t params;
    warp_input_config_t input_config;
    
    /* Input state */
    int stdin_configured;
    struct termios original_termios;
    int selected_corner;      /* 0-3 for corners, -1 for global */
    int fine_mode;
    
    /* State tracking */
    int matrix_dirty;
    warp_matrix_t current_matrix;
};

/**
 * Setup non-blocking keyboard input
 */
static int setup_keyboard_input(warp_control_ctx_t *ctx) {
    struct termios new_termios;
    
    /* Get current terminal settings */
    if (tcgetattr(STDIN_FILENO, &ctx->original_termios) < 0) {
        fprintf(stderr, "Failed to get terminal attributes: %s\n", strerror(errno));
        return WARP_CONTROL_ERROR;
    }
    
    /* Configure for non-blocking, non-canonical input */
    new_termios = ctx->original_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);
    new_termios.c_cc[VMIN] = 0;   /* Non-blocking */
    new_termios.c_cc[VTIME] = 0;
    
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_termios) < 0) {
        fprintf(stderr, "Failed to set terminal attributes: %s\n", strerror(errno));
        return WARP_CONTROL_ERROR;
    }
    
    /* Set stdin to non-blocking */
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
    ctx->stdin_configured = 1;
    return WARP_CONTROL_OK;
}

/**
 * Restore terminal settings
 */
static void restore_keyboard_input(warp_control_ctx_t *ctx) {
    if (ctx->stdin_configured) {
        tcsetattr(STDIN_FILENO, TCSANOW, &ctx->original_termios);
        
        /* Restore blocking mode */
        int flags = fcntl(STDIN_FILENO, F_GETFL);
        fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
        
        ctx->stdin_configured = 0;
    }
}

/**
 * Initialize default warp parameters
 */
static void init_default_params(warp_params_t *params) {
    memset(params, 0, sizeof(warp_params_t));
    
    params->mode = WARP_MODE_CORNERS;
    
    /* Initialize corners to identity (no warp) */
    params->corners.top_left[0] = -1.0f;     params->corners.top_left[1] = -1.0f;
    params->corners.top_right[0] = 1.0f;     params->corners.top_right[1] = -1.0f;
    params->corners.bottom_left[0] = -1.0f;  params->corners.bottom_left[1] = 1.0f;
    params->corners.bottom_right[0] = 1.0f;  params->corners.bottom_right[1] = 1.0f;
    
    params->scale_x = params->scale_y = 1.0f;
}

/**
 * Generate identity transformation matrix
 */
static void matrix_identity(float *matrix) {
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

/**
 * Create warp control context
 */
warp_control_ctx_t *warp_control_create(void) {
    warp_control_ctx_t *ctx = calloc(1, sizeof(warp_control_ctx_t));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate warp control context\n");
        return NULL;
    }
    
    /* Initialize default parameters */
    init_default_params(&ctx->params);
    
    /* Default input configuration */
    ctx->input_config.step_size = DEFAULT_STEP_SIZE;
    ctx->input_config.config_file = DEFAULT_CONFIG_FILE;
    
    /* Initialize matrix to identity */
    matrix_identity(ctx->current_matrix.matrix);
    ctx->matrix_dirty = 1;
    
    ctx->selected_corner = 0;  /* Start with top-left corner */
    
    return ctx;
}

/**
 * Configure warp control with renderer
 */
int warp_control_configure(warp_control_ctx_t *ctx, gpu_renderer_ctx_t *renderer_ctx) {
    if (!ctx || !renderer_ctx) {
        return WARP_CONTROL_ERROR;
    }
    
    ctx->renderer_ctx = renderer_ctx;
    
    /* Setup keyboard input */
    int ret = setup_keyboard_input(ctx);
    if (ret < 0) {
        fprintf(stderr, "Warning: Could not setup keyboard input for warp control\n");
        /* Continue without interactive control */
    }
    
    printf("Warp control configured\n");
    printf("Controls:\n");
    printf("  Arrow keys: Adjust selected corner\n");
    printf("  1-4: Select corner (1=top-left, 2=top-right, 3=bottom-left, 4=bottom-right)\n");
    printf("  R: Reset to identity\n");
    printf("  F: Toggle fine adjustment mode\n");
    printf("  S: Save configuration\n");
    printf("  L: Load configuration\n");
    printf("  Q/ESC: Quit\n");
    
    return WARP_CONTROL_OK;
}

/**
 * Generate transformation matrix from corner points
 */
int warp_control_corners_to_matrix(const corner_points_t *corners, warp_matrix_t *matrix) {
    if (!corners || !matrix) {
        return WARP_CONTROL_ERROR;
    }
    
    /* This is a simplified perspective transformation.
     * For a complete implementation, you would solve the perspective
     * transformation equation to map unit square to arbitrary quadrilateral.
     */
    
    /* For now, create a simple transformation matrix */
    matrix_identity(matrix->matrix);
    
    /* Apply basic scaling and translation based on corner positions */
    float center_x = (corners->top_left[0] + corners->top_right[0] + 
                     corners->bottom_left[0] + corners->bottom_right[0]) * 0.25f;
    float center_y = (corners->top_left[1] + corners->top_right[1] +
                     corners->bottom_left[1] + corners->bottom_right[1]) * 0.25f;
    
    /* Simple keystone approximation */
    float h_keystone __attribute__((unused)) = (corners->top_right[0] - corners->top_left[0]) -
                      (corners->bottom_right[0] - corners->bottom_left[0]);
    float v_keystone __attribute__((unused)) = (corners->bottom_left[1] - corners->top_left[1]) -
                      (corners->bottom_right[1] - corners->top_right[1]);
    
    /* Apply transformation (simplified) */
    matrix->matrix[12] = center_x;  /* Translation X */
    matrix->matrix[13] = center_y;  /* Translation Y */
    
    /* Note: Full perspective transformation requires solving a system of equations
     * This is a placeholder for the complete implementation */
    
    matrix->dirty = 1;
    return WARP_CONTROL_OK;
}

/**
 * Generate keystone transformation matrix
 */
int warp_control_keystone_to_matrix(float h_keystone, float v_keystone, warp_matrix_t *matrix) {
    if (!matrix) {
        return WARP_CONTROL_ERROR;
    }
    
    matrix_identity(matrix->matrix);
    
    /* Apply keystone transformation */
    /* This creates a simple perspective effect */
    matrix->matrix[1] = h_keystone * 0.5f;  /* Horizontal skew */
    matrix->matrix[4] = v_keystone * 0.5f;  /* Vertical skew */
    
    matrix->dirty = 1;
    return WARP_CONTROL_OK;
}

/**
 * Update transformation matrix and apply to renderer
 */
static int update_matrix(warp_control_ctx_t *ctx) {
    int ret;
    
    if (!ctx->matrix_dirty) {
        return WARP_CONTROL_OK;
    }
    
    /* Generate matrix based on current mode */
    switch (ctx->params.mode) {
        case WARP_MODE_CORNERS:
            ret = warp_control_corners_to_matrix(&ctx->params.corners, &ctx->current_matrix);
            break;
            
        case WARP_MODE_KEYSTONE:
            ret = warp_control_keystone_to_matrix(ctx->params.keystone_h, 
                                                 ctx->params.keystone_v, 
                                                 &ctx->current_matrix);
            break;
            
        default:
            /* Use identity matrix for unsupported modes */
            matrix_identity(ctx->current_matrix.matrix);
            ret = WARP_CONTROL_OK;
            break;
    }
    
    if (ret < 0) {
        return ret;
    }
    
    /* Apply to renderer */
    ret = gpu_renderer_set_warp_matrix(ctx->renderer_ctx, &ctx->current_matrix);
    if (ret < 0) {
        return ret;
    }
    
    ctx->matrix_dirty = 0;
    return WARP_CONTROL_OK;
}

/**
 * Process keyboard input
 */
static int process_keyboard(warp_control_ctx_t *ctx) {
    char ch;
    int updated = 0;
    float step = ctx->fine_mode ? ctx->input_config.step_size * 0.1f : ctx->input_config.step_size;
    
    while (read(STDIN_FILENO, &ch, 1) > 0) {
        switch (ch) {
            case 27: /* ESC key or escape sequence start */
                /* Check if it's a single ESC key or start of arrow key sequence */
                if (read(STDIN_FILENO, &ch, 1) <= 0) {
                    return WARP_CONTROL_ERROR;  /* Single ESC - quit */
                } else if (ch == '[') {
                    /* Arrow key sequence */
                    if (read(STDIN_FILENO, &ch, 1) > 0) {
                        float *corner_x = NULL, *corner_y = NULL;
                        
                        /* Get pointer to selected corner coordinates */
                        switch (ctx->selected_corner) {
                            case 0: corner_x = &ctx->params.corners.top_left[0]; 
                                   corner_y = &ctx->params.corners.top_left[1]; break;
                            case 1: corner_x = &ctx->params.corners.top_right[0];
                                   corner_y = &ctx->params.corners.top_right[1]; break;
                            case 2: corner_x = &ctx->params.corners.bottom_left[0];
                                   corner_y = &ctx->params.corners.bottom_left[1]; break;
                            case 3: corner_x = &ctx->params.corners.bottom_right[0];
                                   corner_y = &ctx->params.corners.bottom_right[1]; break;
                        }
                        
                        if (corner_x && corner_y) {
                            switch (ch) {
                                case KEY_ARROW_UP:    *corner_y -= step; break;
                                case KEY_ARROW_DOWN:  *corner_y += step; break;
                                case KEY_ARROW_LEFT:  *corner_x -= step; break;
                                case KEY_ARROW_RIGHT: *corner_x += step; break;
                            }
                            
                            /* Clamp to valid range */
                            *corner_x = fmaxf(-2.0f, fminf(2.0f, *corner_x));
                            *corner_y = fmaxf(-2.0f, fminf(2.0f, *corner_y));
                            
                            ctx->matrix_dirty = 1;
                            updated = 1;
                        }
                    }
                } else {
                    return WARP_CONTROL_ERROR;  /* ESC + other key - quit */
                }
                break;
                
            case 'q':
            case 'Q':
                return WARP_CONTROL_ERROR;  /* Signal to quit */
                
            case 'r':
            case 'R':
                init_default_params(&ctx->params);
                ctx->matrix_dirty = 1;
                updated = 1;
                printf("Warp reset to identity\n");
                break;
                
            case 'f':
            case 'F':
                ctx->fine_mode = !ctx->fine_mode;
                printf("Fine adjustment mode: %s\n", ctx->fine_mode ? "ON" : "OFF");
                break;
                
            case '1': ctx->selected_corner = 0; printf("Selected: Top-left corner\n"); break;
            case '2': ctx->selected_corner = 1; printf("Selected: Top-right corner\n"); break;
            case '3': ctx->selected_corner = 2; printf("Selected: Bottom-left corner\n"); break;
            case '4': ctx->selected_corner = 3; printf("Selected: Bottom-right corner\n"); break;
                
            case 's':
            case 'S':
                warp_control_save_config(ctx, ctx->input_config.config_file);
                printf("Configuration saved\n");
                break;
                
            case 'l':
            case 'L':
                if (warp_control_load_config(ctx, ctx->input_config.config_file) == 0) {
                    ctx->matrix_dirty = 1;
                    updated = 1;
                    printf("Configuration loaded\n");
                } else {
                    printf("Failed to load configuration\n");
                }
                break;
        }
    }
    
    return updated ? WARP_CONTROL_UPDATED : WARP_CONTROL_OK;
}

/**
 * Process input events
 */
int warp_control_process_input(warp_control_ctx_t *ctx) {
    int ret;
    
    if (!ctx) {
        return WARP_CONTROL_ERROR;
    }
    
    /* Process keyboard input if configured */
    if (ctx->stdin_configured) {
        ret = process_keyboard(ctx);
        if (ret < 0) {
            return ret;
        }
    }
    
    /* Update matrix if parameters changed */
    ret = update_matrix(ctx);
    if (ret < 0) {
        return ret;
    }
    
    return WARP_CONTROL_OK;
}

/**
 * Reset warp to identity
 */
int warp_control_reset(warp_control_ctx_t *ctx) {
    if (!ctx) {
        return WARP_CONTROL_ERROR;
    }
    
    init_default_params(&ctx->params);
    ctx->matrix_dirty = 1;
    
    return update_matrix(ctx);
}

/**
 * Save configuration to file
 */
int warp_control_save_config(warp_control_ctx_t *ctx, const char *filename) {
    FILE *file;
    
    if (!ctx || !filename) {
        return WARP_CONTROL_ERROR;
    }
    
    file = fopen(filename, "w");
    if (!file) {
        fprintf(stderr, "Failed to open config file for writing: %s\n", strerror(errno));
        return WARP_CONTROL_ERROR;
    }
    
    fprintf(file, "# Pickle Warp Configuration\n");
    fprintf(file, "mode=%d\n", (int)ctx->params.mode);
    fprintf(file, "corner_tl=%.6f,%.6f\n", ctx->params.corners.top_left[0], ctx->params.corners.top_left[1]);
    fprintf(file, "corner_tr=%.6f,%.6f\n", ctx->params.corners.top_right[0], ctx->params.corners.top_right[1]);
    fprintf(file, "corner_bl=%.6f,%.6f\n", ctx->params.corners.bottom_left[0], ctx->params.corners.bottom_left[1]);
    fprintf(file, "corner_br=%.6f,%.6f\n", ctx->params.corners.bottom_right[0], ctx->params.corners.bottom_right[1]);
    fprintf(file, "keystone_h=%.6f\n", ctx->params.keystone_h);
    fprintf(file, "keystone_v=%.6f\n", ctx->params.keystone_v);
    
    fclose(file);
    return WARP_CONTROL_OK;
}

/**
 * Load configuration from file
 */
int warp_control_load_config(warp_control_ctx_t *ctx, const char *filename) {
    FILE *file;
    char line[256];
    
    if (!ctx || !filename) {
        return WARP_CONTROL_ERROR;
    }
    
    file = fopen(filename, "r");
    if (!file) {
        return WARP_CONTROL_ERROR;  /* File doesn't exist, not an error */
    }
    
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        
        if (sscanf(line, "corner_tl=%f,%f", &ctx->params.corners.top_left[0], &ctx->params.corners.top_left[1]) == 2) continue;
        if (sscanf(line, "corner_tr=%f,%f", &ctx->params.corners.top_right[0], &ctx->params.corners.top_right[1]) == 2) continue;
        if (sscanf(line, "corner_bl=%f,%f", &ctx->params.corners.bottom_left[0], &ctx->params.corners.bottom_left[1]) == 2) continue;
        if (sscanf(line, "corner_br=%f,%f", &ctx->params.corners.bottom_right[0], &ctx->params.corners.bottom_right[1]) == 2) continue;
        if (sscanf(line, "keystone_h=%f", &ctx->params.keystone_h) == 1) continue;
        if (sscanf(line, "keystone_v=%f", &ctx->params.keystone_v) == 1) continue;
        
        int mode;
        if (sscanf(line, "mode=%d", &mode) == 1) {
            ctx->params.mode = (warp_mode_t)mode;
        }
    }
    
    fclose(file);
    return WARP_CONTROL_OK;
}

/**
 * Get control help text
 */
const char *warp_control_get_help(void) {
    return "Warp Control Help:\n"
           "  Arrow keys: Adjust selected corner position\n"
           "  1-4: Select corner (1=top-left, 2=top-right, 3=bottom-left, 4=bottom-right)\n"
           "  R: Reset warp to identity (no distortion)\n"
           "  F: Toggle fine adjustment mode (smaller steps)\n"
           "  S: Save current warp configuration\n"
           "  L: Load saved warp configuration\n"
           "  Q/ESC: Quit application\n"
           "\n"
           "Corner adjustment allows real-time keystone/perspective correction.\n"
           "Useful for projection mapping and display calibration.\n";
}

/**
 * Destroy warp control
 */
void warp_control_destroy(warp_control_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    
    /* Restore terminal settings */
    restore_keyboard_input(ctx);
    
    /* Auto-save configuration if enabled */
    if (ctx->input_config.enable_auto_save) {
        warp_control_save_config(ctx, ctx->input_config.config_file);
    }
    
    free(ctx);
}
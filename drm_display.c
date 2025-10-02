#define _GNU_SOURCE
#include "drm_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <stdint.h>

// Forward declarations - none needed for GBM-only implementation

static const char *drm_device_paths[] = {
    "/dev/dri/card1",       // Working display card (detected by diagnostics)
    "/dev/dri/card0",       // Fallback display card
    "/dev/dri/renderD128",  // Render node (for compute only)
    "/dev/dri/renderD129",  // Additional render nodes
    NULL
};

static int find_drm_device(void) {
    for (int i = 0; drm_device_paths[i]; i++) {
        int fd = open(drm_device_paths[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            return fd;
        }
    }
    
    printf("\nTroubleshooting:\n");
    printf("1. Make sure you're in the 'render' group: groups | grep render\n");
    printf("2. If not, run: sudo usermod -a -G render $USER && logout\n");
    printf("3. Or try with sudo: sudo ./pickle <video>\n");
    return -1;
}



int drm_init(display_ctx_t *drm) {
    memset(drm, 0, sizeof(*drm));

    printf("Initializing GBM (Generic Buffer Manager) for hardware-accelerated rendering...\n");

    // Open DRM device for GBM (no master required)
    drm->drm_fd = find_drm_device();
    if (drm->drm_fd < 0) {
        fprintf(stderr, "Failed to open DRM device\n");
        fprintf(stderr, "Hint: Make sure you're in the 'video' group: sudo usermod -a -G video $USER\n");
        return -1;
    }

    printf("Opened DRM device for GBM buffer management\n");
    
    // Note: We intentionally do NOT become DRM master
    // This allows the system's display manager to handle mode setting
    // while we use GBM for efficient buffer management

    // For GBM-only mode, use standard display resolution without querying DRM
    // This avoids need for DRM master permissions while still providing efficient buffers
    drm->width = 1920;   // Standard HD resolution
    drm->height = 1080;
    drm->refresh_rate = 60;
    
    printf("Using GBM surface: %dx%d@%dHz (standard resolution)\n", 
           drm->width, drm->height, drm->refresh_rate);
    
    // Initialize GBM buffer state
    drm->current_bo = NULL;
    drm->next_bo = NULL;

    // Initialize GBM device
    drm->gbm_device = gbm_create_device(drm->drm_fd);
    if (!drm->gbm_device) {
        fprintf(stderr, "Failed to create GBM device\n");
        close(drm->drm_fd);
        return -1;
    }

    // Create GBM surface for efficient buffer management
    drm->gbm_surface = gbm_surface_create(drm->gbm_device,
                                          drm->width, drm->height,
                                          GBM_FORMAT_XRGB8888,
                                          GBM_BO_USE_RENDERING);  // Only rendering, no scanout needed
    if (!drm->gbm_surface) {
        fprintf(stderr, "Failed to create GBM surface\n");
        gbm_device_destroy(drm->gbm_device);
        close(drm->drm_fd);
        return -1;
    }

    printf("✓ GBM initialized successfully - hardware-accelerated buffer management ready\n");
    return 0;
}







int drm_swap_buffers(display_ctx_t *drm) {
    // GBM-only buffer management without DRM master requirements
    
    // Get the front buffer from GBM surface
    drm->next_bo = gbm_surface_lock_front_buffer(drm->gbm_surface);
    if (!drm->next_bo) {
        fprintf(stderr, "Failed to lock front buffer\n");
        return -1;
    }
    
    // Release previous buffer if we have one
    if (drm->current_bo) {
        gbm_surface_release_buffer(drm->gbm_surface, drm->current_bo);
    }
    
    // Update current buffer
    drm->current_bo = drm->next_bo;
    
    // GBM buffer management successful - no DRM master operations needed
    // The EGL context and OpenGL rendering handles the actual display output
    // through the existing display manager or compositor
    
    return 0;
}

void drm_cleanup(display_ctx_t *drm) {
    // GBM buffer cleanup
    if (drm->current_bo) {
        gbm_surface_release_buffer(drm->gbm_surface, drm->current_bo);
    }
    if (drm->next_bo && drm->next_bo != drm->current_bo) {
        gbm_surface_release_buffer(drm->gbm_surface, drm->next_bo);
    }
    
    // GBM resources cleanup
    if (drm->gbm_surface) {
        gbm_surface_destroy(drm->gbm_surface);
    }
    if (drm->gbm_device) {
        gbm_device_destroy(drm->gbm_device);
    }
    
    // Close DRM device (no master to drop since we never acquired it)
    if (drm->drm_fd >= 0) {
        close(drm->drm_fd);
    }
    
    printf("GBM cleanup completed\n");
}
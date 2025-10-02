#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

int main() {
    const char* device_path = "/dev/dri/card1";
    int drm_fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "Failed to open DRM device: %s\n", strerror(errno));
        return 1;
    }

    /* Set DRM master */
    if (drmSetMaster(drm_fd)) {
        fprintf(stderr, "Failed to become DRM master: %s\n", strerror(errno));
        close(drm_fd);
        return 1;
    }

    /* Get DRM resources */
    drmModeRes *resources = drmModeGetResources(drm_fd);
    if (!resources) {
        fprintf(stderr, "Failed to get DRM resources\n");
        close(drm_fd);
        return 1;
    }

    /* Find HDMI-A-1 connector */
    drmModeConnector *connector = NULL;
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(drm_fd, resources->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED &&
            conn->connector_type == DRM_MODE_CONNECTOR_HDMIA) {
            connector = conn;
            break;
        }
        if (conn) drmModeFreeConnector(conn);
    }

    if (!connector) {
        fprintf(stderr, "No connected HDMI connector found\n");
        drmModeFreeResources(resources);
        close(drm_fd);
        return 1;
    }

    printf("Found connector %d with %d modes\n", connector->connector_id, connector->count_modes);

    /* Find 1920x1080@60Hz mode */
    drmModeModeInfo mode;
    int found_mode = 0;
    for (int i = 0; i < connector->count_modes; i++) {
        if (connector->modes[i].hdisplay == 1920 && 
            connector->modes[i].vdisplay == 1080 &&
            connector->modes[i].vrefresh == 60) {
            mode = connector->modes[i];
            found_mode = 1;
            printf("Found mode: %dx%d@%dHz\n", mode.hdisplay, mode.vdisplay, mode.vrefresh);
            break;
        }
    }

    if (!found_mode) {
        fprintf(stderr, "1920x1080@60Hz mode not found\n");
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(drm_fd);
        return 1;
    }

    /* Find encoder */
    drmModeEncoder *encoder = NULL;
    if (connector->encoder_id) {
        encoder = drmModeGetEncoder(drm_fd, connector->encoder_id);
    }
    if (!encoder) {
        for (int i = 0; i < connector->count_encoders; i++) {
            encoder = drmModeGetEncoder(drm_fd, connector->encoders[i]);
            if (encoder) break;
        }
    }

    if (!encoder) {
        fprintf(stderr, "No encoder found\n");
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(drm_fd);
        return 1;
    }

    printf("Found encoder %d\n", encoder->encoder_id);

    /* Find CRTC */
    drmModeCrtc *crtc = NULL;
    if (encoder->crtc_id) {
        crtc = drmModeGetCrtc(drm_fd, encoder->crtc_id);
    }
    if (!crtc) {
        for (int i = 0; i < resources->count_crtcs; i++) {
            if (encoder->possible_crtcs & (1 << i)) {
                crtc = drmModeGetCrtc(drm_fd, resources->crtcs[i]);
                if (crtc) break;
            }
        }
    }

    if (!crtc) {
        fprintf(stderr, "No CRTC found\n");
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(drm_fd);
        return 1;
    }

    printf("Found CRTC %d\n", crtc->crtc_id);

    /* Create GBM device and buffer for a framebuffer */
    printf("Creating GBM device...\n");
    struct gbm_device *gbm_device = gbm_create_device(drm_fd);
    if (!gbm_device) {
        fprintf(stderr, "Failed to create GBM device\n");
        drmModeFreeCrtc(crtc);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(drm_fd);
        return 1;
    }

    /* Create a buffer object directly */
    printf("Creating GBM buffer object %dx%d...\n", mode.hdisplay, mode.vdisplay);
    struct gbm_bo *bo = gbm_bo_create(gbm_device, mode.hdisplay, mode.vdisplay,
                                      GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT);
    if (!bo) {
        fprintf(stderr, "Failed to create GBM buffer object\n");
        gbm_device_destroy(gbm_device);
        drmModeFreeCrtc(crtc);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(drm_fd);
        return 1;
    }
    printf("Got buffer object\n");

    /* Add framebuffer */
    uint32_t fb_id;
    int ret = drmModeAddFB(drm_fd,
                          mode.hdisplay, mode.vdisplay,
                          24, 32, gbm_bo_get_stride(bo),
                          gbm_bo_get_handle(bo).u32, &fb_id);

    if (ret) {
        fprintf(stderr, "Failed to add framebuffer: %s\n", strerror(errno));
        gbm_bo_destroy(bo);
        gbm_device_destroy(gbm_device);
        drmModeFreeCrtc(crtc);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(drm_fd);
        return 1;
    }

    printf("Created framebuffer %d\n", fb_id);

    /* Set CRTC mode - this is where it's failing */
    printf("Attempting to set CRTC mode...\n");
    ret = drmModeSetCrtc(drm_fd, crtc->crtc_id, fb_id, 0, 0,
                        &connector->connector_id, 1, &mode);

    if (ret) {
        fprintf(stderr, "Failed to set CRTC mode: %s (errno %d)\n", strerror(errno), errno);
        if (errno == EACCES) {
            fprintf(stderr, "Permission denied - this suggests a DRM master issue\n");
        } else if (errno == EINVAL) {
            fprintf(stderr, "Invalid argument - mode or configuration issue\n");
        } else if (errno == EBUSY) {
            fprintf(stderr, "Device busy - another process may be using DRM\n");
        }
    } else {
        printf("✓ Successfully set CRTC mode!\n");
    }

    /* Cleanup */
    drmModeRmFB(drm_fd, fb_id);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm_device);
    drmModeFreeCrtc(crtc);
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    close(drm_fd);

    return ret ? 1 : 0;
}
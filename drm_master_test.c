#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <drm/drm.h>

int main() {
    const char* drm_device = "/dev/dri/card1";
    int drm_fd;
    
    printf("Testing DRM master access on %s\n", drm_device);
    
    // Open DRM device
    drm_fd = open(drm_device, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        printf("Failed to open %s: %s\n", drm_device, strerror(errno));
        return 1;
    }
    
    printf("Successfully opened DRM device\n");
    
    // Try to become DRM master
    if (ioctl(drm_fd, DRM_IOCTL_SET_MASTER, 0) < 0) {
        printf("Failed to become DRM master: %s\n", strerror(errno));
        printf("This is expected if another process has master\n");
    } else {
        printf("Successfully became DRM master!\n");
        
        // Drop master
        if (ioctl(drm_fd, DRM_IOCTL_DROP_MASTER, 0) < 0) {
            printf("Failed to drop DRM master: %s\n", strerror(errno));
        } else {
            printf("Successfully dropped DRM master\n");
        }
    }
    
    close(drm_fd);
    return 0;
}
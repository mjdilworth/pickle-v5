#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main() {
    const char *device = "/dev/dri/card1";
    int fd;
    drmModeRes *resources;
    
    printf("Testing DRM access to %s\n", device);
    
    // Open device
    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("ERROR: Cannot open %s: %s\n", device, strerror(errno));
        return 1;
    }
    printf("✓ Device opened successfully (fd=%d)\n", fd);
    
    // Try to get resources
    printf("Calling drmModeGetResources()...\n");
    resources = drmModeGetResources(fd);
    if (!resources) {
        printf("ERROR: drmModeGetResources() returned NULL\n");
        printf("errno: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    
    printf("✓ Got DRM resources successfully!\n");
    printf("  CRTCs: %d\n", resources->count_crtcs);
    printf("  Connectors: %d\n", resources->count_connectors);
    printf("  Encoders: %d\n", resources->count_encoders);
    
    drmModeFreeResources(resources);
    close(fd);
    return 0;
}

# Pickle - GPU-accelerated Video Player for Raspberry Pi 4
# Makefile with complete dependency management

# Compiler and base flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g
TARGET = pickle

# Source files
SOURCES = pickle.c video_input.c hw_decoder.c gpu_renderer.c display_output.c warp_control.c fallback.c
HEADERS = video_input.h hw_decoder.h gpu_renderer.h display_output.h warp_control.h fallback.h

# Object files
OBJECTS = $(SOURCES:.c=.o)

# Package config for dependency detection
PKG_CONFIG = pkg-config

# Required libraries with fallback detection
LIBS = 
CFLAGS_EXTRA = 

# FFmpeg libraries (required)
FFMPEG_LIBS = libavformat libavcodec libavutil
FFMPEG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(FFMPEG_LIBS) 2>/dev/null)
FFMPEG_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(FFMPEG_LIBS) 2>/dev/null)

# OpenGL ES and EGL (required)
GLES_LIBS = glesv2 egl
GLES_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(GLES_LIBS) 2>/dev/null)
GLES_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GLES_LIBS) 2>/dev/null)

# GBM and DRM (required for direct display)
DRM_LIBS = gbm libdrm
DRM_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(DRM_LIBS) 2>/dev/null)
DRM_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(DRM_LIBS) 2>/dev/null)

# libmpv (optional - for fallback)
MPV_CFLAGS = $(shell $(PKG_CONFIG) --cflags mpv 2>/dev/null)
MPV_LDFLAGS = $(shell $(PKG_CONFIG) --libs mpv 2>/dev/null)

# Math library
MATH_LDFLAGS = -lm

# Combine all flags
ALL_CFLAGS = $(CFLAGS) $(FFMPEG_CFLAGS) $(GLES_CFLAGS) $(DRM_CFLAGS)
ALL_LDFLAGS = $(FFMPEG_LDFLAGS) $(GLES_LDFLAGS) $(DRM_LDFLAGS) $(MATH_LDFLAGS)

# Add libmpv if available
ifneq ($(MPV_LDFLAGS),)
    ALL_CFLAGS += $(MPV_CFLAGS) -DHAVE_LIBMPV
    ALL_LDFLAGS += $(MPV_LDFLAGS)
    $(info [INFO] libmpv found - fallback support enabled)
else
    $(info [WARNING] libmpv not found - fallback support disabled)
endif

# Default target
all: check-deps $(TARGET)

# Check for required dependencies
check-deps:
	@echo "Checking dependencies..."
	@$(PKG_CONFIG) --exists $(FFMPEG_LIBS) || (echo "ERROR: FFmpeg development libraries not found. Install: libavformat-dev libavcodec-dev libavutil-dev" && exit 1)
	@$(PKG_CONFIG) --exists $(GLES_LIBS) || (echo "ERROR: OpenGL ES development libraries not found. Install: libgles2-mesa-dev libegl1-mesa-dev" && exit 1)
	@$(PKG_CONFIG) --exists $(DRM_LIBS) || (echo "ERROR: DRM/GBM development libraries not found. Install: libgbm-dev libdrm-dev" && exit 1)
	@echo "All required dependencies found!"

# Build the executable
$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CC) $(OBJECTS) $(ALL_LDFLAGS) -o $(TARGET)
	@echo "Build complete: $(TARGET)"

# Compile source files
%.o: %.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(ALL_CFLAGS) -c $< -o $@

# Run with test video
run: $(TARGET)
	@if [ -f "rpi4-e.mp4" ]; then \
		echo "Running with rpi4-e.mp4..."; \
		./$(TARGET) rpi4-e.mp4; \
	else \
		echo "Test file rpi4-e.mp4 not found."; \
		echo "Usage: ./$(TARGET) <video_file.mp4>"; \
		./$(TARGET); \
	fi

# Run with custom video file
run-file: $(TARGET)
	@read -p "Enter video file path: " file; \
	if [ -f "$$file" ]; then \
		./$(TARGET) "$$file"; \
	else \
		echo "File not found: $$file"; \
	fi

# Debug build with additional flags
debug: CFLAGS += -DDEBUG -g3 -O0 -fsanitize=address
debug: ALL_LDFLAGS += -fsanitize=address
debug: clean $(TARGET)

# Release build with optimizations
release: CFLAGS += -DNDEBUG -O3 -flto
release: ALL_LDFLAGS += -flto
release: clean $(TARGET)

# Install system dependencies (Ubuntu/Debian)
install-deps-ubuntu:
	@echo "Installing dependencies for Ubuntu/Debian..."
	sudo apt-get update
	sudo apt-get install -y \
		build-essential \
		pkg-config \
		libavformat-dev \
		libavcodec-dev \
		libavutil-dev \
		libgles2-mesa-dev \
		libegl1-mesa-dev \
		libgbm-dev \
		libdrm-dev \
		libmpv-dev \
		git

# Install system dependencies (Raspberry Pi OS)
install-deps-rpi:
	@echo "Installing dependencies for Raspberry Pi OS..."
	sudo apt-get update
	sudo apt-get install -y \
		build-essential \
		pkg-config \
		libavformat-dev \
		libavcodec-dev \
		libavutil-dev \
		libgles2-mesa-dev \
		libegl1-mesa-dev \
		libgbm-dev \
		libdrm-dev \
		libmpv-dev

# Show library versions
show-versions:
	@echo "=== Dependency Versions ==="
	@$(PKG_CONFIG) --modversion $(FFMPEG_LIBS) 2>/dev/null | sed 's/^/FFmpeg: /' || echo "FFmpeg: Not found"
	@$(PKG_CONFIG) --modversion $(GLES_LIBS) 2>/dev/null | sed 's/^/OpenGL ES: /' || echo "OpenGL ES: Not found"
	@$(PKG_CONFIG) --modversion $(DRM_LIBS) 2>/dev/null | sed 's/^/DRM\/GBM: /' || echo "DRM/GBM: Not found"
	@$(PKG_CONFIG) --modversion mpv 2>/dev/null | sed 's/^/libmpv: /' || echo "libmpv: Not found"
	@echo "GCC: $$(gcc --version | head -n1)"

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(TARGET) $(OBJECTS)
	rm -f warp_config.txt
	rm -f *.log

# Deep clean (including backup files)
distclean: clean
	rm -f *~ *.bak
	rm -f core.*
	rm -f .*.swp

# Rebuild (clean then build)
rebuild: clean all

# Create a simple test video (requires ffmpeg)
create-test-video:
	@if command -v ffmpeg >/dev/null 2>&1; then \
		echo "Creating test video: rpi4-e.mp4..."; \
		ffmpeg -f lavfi -i testsrc2=duration=10:size=1920x1080:rate=30 \
		       -f lavfi -i sine=frequency=440:duration=10 \
		       -c:v libx264 -preset fast -crf 23 \
		       -c:a aac -b:a 128k \
		       -y rpi4-e.mp4; \
		echo "Test video created: rpi4-e.mp4"; \
	else \
		echo "ffmpeg not found. Please install ffmpeg to create test video."; \
	fi

# Show compilation database for IDE support
compile-commands:
	@echo "Generating compile_commands.json..."
	@echo '[' > compile_commands.json
	@for src in $(SOURCES); do \
		echo "  {" >> compile_commands.json; \
		echo "    \"directory\": \"$$(pwd)\"," >> compile_commands.json; \
		echo "    \"command\": \"$(CC) $(ALL_CFLAGS) -c $$src\"," >> compile_commands.json; \
		echo "    \"file\": \"$$src\"" >> compile_commands.json; \
		echo "  }," >> compile_commands.json; \
	done
	@sed -i '$$s/,$//' compile_commands.json
	@echo ']' >> compile_commands.json
	@echo "Created compile_commands.json for IDE support"

# Help target
help:
	@echo "Pickle Video Player Build System"
	@echo "================================"
	@echo ""
	@echo "Main targets:"
	@echo "  all              - Build the application (default)"
	@echo "  clean            - Remove build artifacts"
	@echo "  rebuild          - Clean and build"
	@echo "  run              - Build and run with rpi4-e.mp4"
	@echo "  run-file         - Build and run with custom file"
	@echo ""
	@echo "Development targets:"
	@echo "  debug            - Build with debug symbols and AddressSanitizer"
	@echo "  release          - Build optimized release version"
	@echo "  check-deps       - Check for required dependencies"
	@echo "  show-versions    - Show library versions"
	@echo ""
	@echo "Setup targets:"
	@echo "  install-deps-ubuntu - Install dependencies on Ubuntu/Debian"
	@echo "  install-deps-rpi    - Install dependencies on Raspberry Pi OS"
	@echo "  create-test-video   - Create test video file"
	@echo ""
	@echo "Utility targets:"
	@echo "  compile-commands - Generate compile_commands.json for IDEs"
	@echo "  distclean        - Deep clean including backup files"
	@echo "  help             - Show this help"

# Mark targets that don't create files
.PHONY: all clean rebuild run run-file debug release check-deps install-deps-ubuntu install-deps-rpi show-versions create-test-video compile-commands help distclean
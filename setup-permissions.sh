#!/bin/bash

# Pickle Video Player - Permission Setup Script
# This script helps set up the necessary permissions for DRM/KMS access

echo "Setting up permissions for Pickle video player..."

# Check if running as root
if [[ $EUID -eq 0 ]]; then
    echo "Running as root - setting up system permissions..."
    
    # Make DRM devices accessible
    chmod 666 /dev/dri/card*
    chmod 666 /dev/video*
    
    echo "Permissions set for current session."
    echo "To make permanent, add user to video group:"
    echo "  usermod -a -G video [username]"
    
else
    echo "Not running as root. Checking current user permissions..."
    
    # Check if user is in video group
    if groups $USER | grep -q '\bvideo\b'; then
        echo "✓ User $USER is in video group"
    else
        echo "✗ User $USER is NOT in video group"
        echo "To fix: sudo usermod -a -G video $USER"
        echo "Then log out and back in"
    fi
    
    # Check DRM device permissions
    for device in /dev/dri/card*; do
        if [ -r "$device" ] && [ -w "$device" ]; then
            echo "✓ Can access $device"
        else
            echo "✗ Cannot access $device"
            echo "To fix: sudo chmod 666 $device"
        fi
    done
    
    # Check V4L2 device permissions  
    for device in /dev/video*; do
        if [ -r "$device" ] && [ -w "$device" ]; then
            echo "✓ Can access $device"
        else
            echo "✗ Cannot access $device"
            echo "To fix: sudo chmod 666 $device"
        fi
    done
fi

echo ""
echo "If you're still having issues, try:"
echo "1. Run with sudo: sudo ./pickle video.mp4"
echo "2. Use fallback mode (automatically triggered on DRM failure)"
echo "3. Check if you're running in a desktop environment vs console"
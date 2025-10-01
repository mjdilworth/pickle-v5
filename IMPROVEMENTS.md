# Pickle v5 - Recent Improvements

## Keyboard Support Added! ⌨️ (Latest Update)

**NEW FEATURE**: Press 'q' to quit the video player instantly!

### What's New:
- **'q' key quits instantly**: No need to use Ctrl+C anymore
- **ESC key also works**: Alternative quit method  
- **Non-blocking input**: Keyboard checking doesn't interfere with video playback
- **Proper terminal handling**: Automatically restores terminal settings on exit

## Signal Handling Fixes (Previous Update)

The signal handling has been significantly improved to address the issue where you had to use `pkill` to terminate the application:

### Changes Made:

1. **Enhanced Main Signal Handler** (`pickle.c`):
   - Creates stop files that the fallback system monitors
   - Better cleanup of all subsystems
   - More responsive to Ctrl+C

2. **Improved Fallback Event Loop** (`fallback.c`):
   - Checks for stop signals every 100ms during playback
   - Properly sends stop command to mpv
   - Graceful shutdown with status reporting

3. **Better DRM Error Reporting** (`display_output.c`):
   - Clearer error messages when DRM access fails
   - Hints about permission requirements
   - Automatic fallback trigger

## Testing the Improvements

### Basic Signal Handling Test:
```bash
# Test with any video file
./pickle your_video.mp4

# Press Ctrl+C - should terminate cleanly within 1-2 seconds
# No more need to use pkill!
```

### Permission Status:
Your system is properly configured:
- ✅ User 'dilly' is in video group
- ✅ All DRM devices (/dev/dri/card*) accessible  
- ✅ All V4L2 devices (/dev/video*) accessible

### Expected Behavior:

1. **Hardware Mode**: Direct GPU rendering at 1920x1080@60Hz
   - Should work if running from console (not desktop)
   - Falls back gracefully if DRM busy/unavailable
   - **Press 'q' to quit instantly**

2. **Fallback Mode**: libmpv software playback
   - Automatically triggered when hardware fails
   - **Press 'q' to quit instantly** 
   - Also responds to Ctrl+C
   - Clean shutdown without hanging

## Usage Examples:

```bash
# Normal playbook (tries hardware first)
./pickle video.mp4
# Press 'q' to quit anytime!

# During playbook:
# - Press 'q' or 'Q' to quit immediately
# - Press ESC to quit  
# - Press Ctrl+C for signal-based quit
# - Arrow keys: adjust keystone corners (hardware mode)
# - 'r': reset warp to identity

# Hardware-accelerated with keystone correction
./pickle video.mp4
# All keyboard controls work seamlessly
```

## Troubleshooting:

If you still encounter issues:

1. **DRM still fails**: Try from a console terminal (Ctrl+Alt+F1)
2. **Video codec issues**: Ensure file is H.264 MP4
3. **Performance issues**: Check `dmesg` for hardware decoder messages
4. **Dependencies missing**: Run `make deps-check`

The system should now be much more responsive and user-friendly!
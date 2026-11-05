A Codec2 (C2) Hardware Video Encoder HAL for Redroid (Android 12, 13, and 14+). It replaces the older OpenMAX (OMX) subsystem with a VA-API hardware pipeline for better performance.

## Current Status & Known Issues

> [!WARNING]
> This project is in active development.

- **H.264 (`c2.redroid.avc.encoder`)**: **Working with artifacts.** Encoding works, but output shows visual corruption (vertical green banding/stride padding artifacts along the right frame edge).
- **H.265 / HEVC (`c2.redroid.hevc.encoder`)**: **Not working.** Fails to initialize during client capture/encoding setup:
  ```text
  [server] ERROR: Capture/encoding error: java.lang.IllegalStateException: null
  [server] INFO: Applying video encoder constraints
  [server] ERROR: Capture/encoding error: java.lang.IllegalStateException: null
  ```

### Development Roadmap
1. Fix H.264 stride/pitch alignment and edge artifacts **(Current priority)**
2. Diagnose and fix H.265 / HEVC encoder initialization failure

---

## How it works

- **Multilib support**: Builds both 32-bit and 64-bit libraries. On 64-bit systems, the HAL daemon runs natively as 64-bit, avoiding 32-to-64-bit translation overhead for apps like scrcpy, games, or WebRTC.
- **Zero-copy pipeline**: Instead of copying pixel data to the CPU, it extracts the DRM PRIME file descriptor from the gralloc buffer and passes it directly to VA-API. This keeps frames in GPU VRAM and keeps encoding latency very low.
- **Low-latency tuning**: Uses a pure IPPP stream structure (no B-frames) and CBR rate control to prevent bitrate spikes. Great for game streaming or screen mirroring.
- **Supported codecs**: H.264 (AVC) for maximum compatibility, and H.265 (HEVC) for better bandwidth efficiency.

## Integration

To add this to your Redroid AOSP build, include it in your device config (for example, `device/redroid/redroid.mk`):

```makefile
$(call inherit-product, hardware/redroid/c2-redroid12/c2.mk)
```

Then compile it:

```bash
m android.hardware.media.c2@1.2-service.redroid libcodec2_redroid_vaapi
```

## Verification

Once booted, check if the service is running:

```bash
ps -ef | grep media.c2
```

You can also check if scrcpy sees the hardware encoders:

```bash
scrcpy --list-encoders
```

It should show up like this:

```text
--video-codec=h264 --video-encoder=c2.redroid.avc.encoder   (hw)
--video-codec=hevc --video-encoder=c2.redroid.hevc.encoder  (hw)
```

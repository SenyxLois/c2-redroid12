## c2-redroid12

A Codec2 (C2) Hardware Video Encoder HAL for Redroid (Android 12, 13, and 14+). It replaces the older OpenMAX (OMX) subsystem with a VA-API hardware pipeline for better performance.

## For Intel GPU user :-
If your server has intel GPU (Using iHD Driver), please check out (https://github.com/SenyxLois/c2-redroid12/tree/c2-redroid12-intel)[c2-redroid12-intel]

## Current Status & Known Issues

> [!WARNING]
> This project is in active development.

- **H.264 (`c2.redroid.avc.encoder`)**: **Work** Works now without any artifacts
- **H.265 / HEVC (`c2.redroid.hevc.encoder`)**: **Work** Works without any artifacts.

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

    /*
 * Copyright (C) 2018 The Android Open Source Project
 * Copyright (C) 2026 Xynes / Redroid C2 Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "vaapi_defs.h"
#include <functional>
#include <memory>
#include <mutex>
#include <sys/types.h>
#include <vector>

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

class VaapiWrapper {
public:
    using OutputBufferAllocator = std::function<void *(size_t exact_size)>;

    static std::shared_ptr<VaapiWrapper> Create(const VaapiEncoderConfig &config);
    ~VaapiWrapper();

    bool Initialize();
    void Deinitialize();

    VASurfaceID ImportDrmPrimeSurface(const DrmPrimeBuffer &buf);
    void DestroySurface(VASurfaceID surface_id);

    bool EncodeFrame(VASurfaceID input_surface,
                     bool force_keyframe,
                     void *out_linear_buf,
                     size_t out_max_size,
                     size_t *out_actual_size,
                     bool *out_is_keyframe);

    bool EncodeFrame(VASurfaceID input_surface,
                     bool force_keyframe,
                     const OutputBufferAllocator &allocator,
                     size_t *out_actual_size,
                     bool *out_is_keyframe);

    bool UpdateBitrate(uint32_t new_bitrate);
    bool RequestKeyFrame();

    const VaapiEncoderConfig &GetConfig() const { return mConfig; }
    int GetDrmFd() const { return mDrmFd; }

private:
    explicit VaapiWrapper(const VaapiEncoderConfig &config);

    bool InitDrmDevice();
    bool CreateEncoderContext();
    bool SetupRateControl();
    bool InitVpp();
    VASurfaceID ConvertRgbToNv12(VASurfaceID rgb_surface);

    bool RenderSequenceParameters();
    bool RenderPictureParameters(VASurfaceID surface, bool is_keyframe);
    bool RenderPackedHeaders(bool is_keyframe);
    bool RenderSliceParameters(VASurfaceID surface, bool is_keyframe);
    bool IsSurfaceRgb(VASurfaceID surface_id) const;

    struct SurfaceCacheEntry {
        dev_t dev = 0;
        ino_t ino = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        uint32_t fourcc = 0;
        VASurfaceID surface = VA_INVALID_SURFACE;
        bool is_rgb = true;
        uint64_t last_used = 0;
    };
    static constexpr size_t kMaxCachedSurfaces = 16;

    VaapiEncoderConfig mConfig;
    int mDrmFd = -1;
    VADisplay mVaDisplay = nullptr;
    VAConfigID mVaConfigId = VA_INVALID_ID;
    VAContextID mVaContextId = VA_INVALID_ID;
    VABufferID mCodedBuf = VA_INVALID_ID;

    // VPP hardware color conversion pipeline (RGBA -> NV12)
    VAConfigID mVppConfigId = VA_INVALID_ID;
    VAContextID mVppContextId = VA_INVALID_ID;
    VASurfaceID mNv12Surface = VA_INVALID_SURFACE;
    VASurfaceID mReconSurfaces[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    int mCurrentRecon = 0;
    int mPrevRecon = 1;
    bool mHasReference = false;

    SurfaceCacheEntry mSurfaceCache[kMaxCachedSurfaces]{};
    uint64_t mCacheTick = 0;
    VASurfaceID mLastUncachedSurface = VA_INVALID_SURFACE;
    bool mLastUncachedIsRgb = true;

    std::vector<uint8_t> mCachedSeqHeaders;

    uint32_t mFrameIndex = 0;
    bool mForceKeyFrame = false;
    bool mBitrateNeedsUpdate = false;
    std::mutex mLock;
};

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

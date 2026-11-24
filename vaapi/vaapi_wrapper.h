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

struct VaapiFrameStats {
    uint64_t vpp_us = 0;
    uint64_t enc_submit_us = 0;
    uint64_t enc_sync_us = 0;
    uint64_t map_copy_us = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t last_ino = 0;
    uint64_t last_modifier = 0;
    uint32_t last_fourcc = 0;
    uint32_t entrypoint = 0;
    bool using_cqp = false;
    uint32_t active_qp = 26;
    bool last_cache_hit = false;
};

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
    VaapiFrameStats GetFrameStats() const;

private:
    explicit VaapiWrapper(const VaapiEncoderConfig &config);

    bool InitDrmDevice();
    bool CreateEncoderContext();
    bool InitVpp();
    VASurfaceID ConvertRgbToNv12(VASurfaceID rgb_surface);

    bool AppendRateControlBuffers(bool reset_brc, VABufferID *bufs, int *num_bufs);
    bool AppendSequenceBuffer(VABufferID *bufs, int *num_bufs);
    bool AppendPictureBuffer(VASurfaceID surface, bool is_keyframe, uint32_t poc, VABufferID *bufs, int *num_bufs);
    bool AppendPackedHeaderBuffers(bool is_keyframe, uint32_t poc, VABufferID *bufs, int *num_bufs);
    bool AppendSliceBuffer(VASurfaceID surface, bool is_keyframe, uint32_t poc, VABufferID *bufs, int *num_bufs);
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
    bool mRateControlInitialized = false;
    bool mBitrateNeedsUpdate = false;
    bool mUsingCqp = false;
    uint32_t mActiveQp = 26;
    uint32_t mBaseQp = 26;
    uint64_t mRcWindowBytes = 0;
    uint32_t mRcWindowFrames = 0;
    uint32_t mEntrypoint = 0;
    uint64_t mCacheHits = 0;
    uint64_t mCacheMisses = 0;
    uint64_t mLastIno = 0;
    uint64_t mLastModifier = 0;
    uint32_t mLastFourcc = 0;
    bool mLastCacheHit = false;
    uint64_t mLastVppUs = 0;
    uint64_t mLastEncSubmitUs = 0;
    uint64_t mLastEncSyncUs = 0;
    uint64_t mLastMapCopyUs = 0;
    mutable std::mutex mLock;
};

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

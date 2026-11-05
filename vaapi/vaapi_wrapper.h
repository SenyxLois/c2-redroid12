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
#include <memory>
#include <mutex>
#include <vector>

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

class VaapiWrapper {
public:
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

    bool UpdateBitrate(uint32_t new_bitrate);
    bool RequestKeyFrame();

    const VaapiEncoderConfig &GetConfig() const { return mConfig; }

private:
    explicit VaapiWrapper(const VaapiEncoderConfig &config);

    bool InitDrmDevice();
    bool CreateEncoderContext();
    bool SetupRateControl();
    bool InitVpp();
    VASurfaceID ConvertRgbToNv12(VASurfaceID rgb_surface);

    bool RenderSequenceParameters();
    bool RenderPictureParameters(VASurfaceID surface, bool is_keyframe);
    bool RenderSliceParameters(VASurfaceID surface, bool is_keyframe);

    VaapiEncoderConfig mConfig;
    int mDrmFd = -1;
    VADisplay mVaDisplay = nullptr;
    VAConfigID mVaConfigId = VA_INVALID_ID;
    VAContextID mVaContextId = VA_INVALID_ID;
    VABufferID mCodedBuf = VA_INVALID_ID;

    // hardware color conversion (rgba -> nv12)
    VAConfigID mVppConfigId = VA_INVALID_ID;
    VAContextID mVppContextId = VA_INVALID_ID;
    VASurfaceID mNv12Surface = VA_INVALID_SURFACE;
    VASurfaceID mReconSurfaces[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    int mCurrentRecon = 0;
    int mPrevRecon = 1;
    bool mHasReference = false;

    uint32_t mFrameIndex = 0;
    bool mForceKeyFrame = false;
    std::mutex mLock;
};

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

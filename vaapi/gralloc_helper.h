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
#include <cutils/native_handle.h>
#include <log/log.h>
#include <drm_fourcc.h>
#include <drm/drm_fourcc.h>

#define LOG_TAG "C2RedroidGralloc"

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

class GrallocHelper {
public:
    // minigbm / cros_gralloc packs the prime dma-buf fd into data[0]
    static bool ExtractDrmBuffer(const native_handle_t *handle,
                                uint32_t width,
                                uint32_t height,
                                uint32_t format,
                                uint32_t stride,
                                DrmPrimeBuffer *out_buf) {
        if (!handle || handle->numFds < 1) {
            ALOGE("Invalid native handle or numFds == 0");
            return false;
        }

        out_buf->prime_fd = handle->data[0];
        out_buf->width = width;
        out_buf->height = height;
        out_buf->stride = stride > 0 ? stride : width;

        switch (format) {
            case 0x11:  // HAL_PIXEL_FORMAT_YCrCb_420_SP
                out_buf->fourcc = DRM_FORMAT_NV21;
                out_buf->total_size = out_buf->stride * height * 3 / 2;
                break;
            case 0x32315659:  // HAL_PIXEL_FORMAT_YV12
                out_buf->fourcc = DRM_FORMAT_YVU420;
                out_buf->total_size = out_buf->stride * height * 3 / 2;
                break;
            case 0x1:
                out_buf->fourcc = DRM_FORMAT_ABGR8888;
                out_buf->total_size = out_buf->stride * height * 4;
                break;
            case 0x2:
                out_buf->fourcc = DRM_FORMAT_XBGR8888;
                out_buf->total_size = out_buf->stride * height * 4;
                break;
            case 0x3:
                out_buf->fourcc = DRM_FORMAT_BGR888;
                out_buf->total_size = out_buf->stride * height * 3;
                break;
            case 0x4:
                out_buf->fourcc = DRM_FORMAT_RGB565;
                out_buf->total_size = out_buf->stride * height * 2;
                break;
            case 0x5:
                out_buf->fourcc = DRM_FORMAT_ARGB8888;
                out_buf->total_size = out_buf->stride * height * 4;
                break;
            case 0x35:  // HAL_PIXEL_FORMAT_YCBCR_420_888, maps to nv12 in cros_gralloc
            default:
                out_buf->fourcc = DRM_FORMAT_NV12;
                out_buf->total_size = out_buf->stride * height * 3 / 2;
                break;
        }

        out_buf->modifier = DRM_FORMAT_MOD_LINEAR;  // virtualized gbm surfaces are linear
        return true;
    }
};

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

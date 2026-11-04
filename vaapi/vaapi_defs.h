/*
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

#include <cstdint>
#include <cstddef>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

enum class CodecType {
    H264,
    HEVC
};

enum class RateControlMode {
    CBR,    // Constant Bitrate (ideal for low-latency streaming)
    VBR,    // Variable Bitrate
    CQP     // Constant Quantization Parameter
};

struct VaapiEncoderConfig {
    CodecType codec = CodecType::H264;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t frame_rate = 60;
    uint32_t bit_rate = 10000000;       // 10 Mbps default
    uint32_t max_bit_rate = 15000000;   // 15 Mbps peak
    uint32_t intra_period = 60;         // IDR frame every 1s at 60fps (or periodic)
    uint32_t ip_period = 1;             // 1 means NO B-frames (IPPP stream for 0 latency)
    RateControlMode rc_mode = RateControlMode::CBR;
    uint32_t initial_qp = 26;
    uint32_t min_qp = 10;
    uint32_t max_qp = 51;
    bool enable_intra_refresh = true;   // Low latency dirty-slice refresh
    const char *drm_node = "/dev/dri/renderD128";
};

struct DrmPrimeBuffer {
    int prime_fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    size_t total_size = 0;
};

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

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

#include "C2RedroidEnc.h"
#include <log/log.h>
#include <media/stagefright/foundation/ADebug.h>
#include <cutils/properties.h>

#define LOG_TAG "C2RedroidEnc"

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

C2RedroidEnc::C2RedroidEnc(const std::shared_ptr<IntfImpl> &intfImpl)
    : SimpleC2Component(intfImpl),
      mIntf(intfImpl) {
}

C2RedroidEnc::~C2RedroidEnc() {
    onRelease();
}

c2_status_t C2RedroidEnc::onInit() {
    return C2_OK;
}

c2_status_t C2RedroidEnc::onStop() {
    releaseEncoder();
    mStarted = false;
    return C2_OK;
}

void C2RedroidEnc::onReset() {
    releaseEncoder();
    mStarted = false;
}

void C2RedroidEnc::onRelease() {
    releaseEncoder();
    mStarted = false;
}

c2_status_t C2RedroidEnc::onFlush_sm() {
    return C2_OK;
}

bool C2RedroidEnc::initEncoder() {
    if (mVaapi) {
        return true;
    }

    VaapiEncoderConfig config{};
    config.codec = getCodecType();
    config.width = mWidth;
    config.height = mHeight;
    config.frame_rate = mFrameRate;
    config.bit_rate = mBitrate;
    config.max_bit_rate = mBitrate * 3 / 2;
    config.intra_period = mIntraPeriod;
    config.ip_period = 1; // Strict low-latency (0 B-frames)
    config.rc_mode = RateControlMode::CBR;
    config.drm_node = "/dev/dri/renderD128";

    // Read optional overrides from system properties
    char prop_val[PROPERTY_VALUE_MAX];
    if (property_get("ro.boot.redroid_gpu_node", prop_val, nullptr) > 0) {
        config.drm_node = prop_val;
    }

    mVaapi = VaapiWrapper::Create(config);
    if (!mVaapi) {
        ALOGE("[%s] Failed to create VaapiWrapper context", getComponentName());
        return false;
    }

    mStarted = true;
    ALOGI("[%s] Initialized VA-API encoder context: %ux%u @ %u fps, %u bps",
          getComponentName(), mWidth, mHeight, mFrameRate, mBitrate);
    return true;
}

void C2RedroidEnc::releaseEncoder() {
    if (mVaapi) {
        mVaapi->Deinitialize();
        mVaapi.reset();
    }
}

void C2RedroidEnc::process(const std::unique_ptr<C2Work> &work,
                           const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 1u;
    work->worklets.front()->output.flags = work->input.flags;

    if (mSawInputEOS || work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        mSawInputEOS = true;
        work->worklets.front()->output.flags = (C2FrameData::flags_t)(
            work->worklets.front()->output.flags | C2FrameData::FLAG_END_OF_STREAM);
        if (work->input.buffers.empty()) {
            return;
        }
    }

    if (work->input.buffers.empty()) {
        return;
    }

    std::shared_ptr<C2Buffer> inBuffer = work->input.buffers[0];
    if (!inBuffer) {
        work->result = C2_CORRUPTED;
        return;
    }

    const C2ConstGraphicBlock &graphicBlock = inBuffer->data().graphicBlocks().front();
    uint32_t width = graphicBlock.width();
    uint32_t height = graphicBlock.height();

    // Check dynamic resolution change
    if (mWidth != width || mHeight != height || !mVaapi) {
        mWidth = width;
        mHeight = height;
        releaseEncoder();
        if (!initEncoder()) {
            work->result = C2_CORRUPTED;
            return;
        }
    }

    // Zero-Copy buffer import from native handle
    const native_handle_t *handle = graphicBlock.handle();
    if (!handle) {
        ALOGE("Failed to obtain native_handle_t from C2GraphicBlock");
        work->result = C2_CORRUPTED;
        return;
    }

    DrmPrimeBuffer drm_buf{};
    if (!GrallocHelper::ExtractDrmBuffer(handle, width, height, 0x35 /* NV12 */, width, &drm_buf)) {
        ALOGE("Failed to extract DRM PRIME parameters from handle");
        work->result = C2_CORRUPTED;
        return;
    }

    VASurfaceID input_surface = mVaapi->ImportDrmPrimeSurface(drm_buf);
    if (input_surface == VA_INVALID_SURFACE) {
        ALOGE("Failed to zero-copy import DRM PRIME surface to VA-API");
        work->result = C2_CORRUPTED;
        return;
    }

    // Allocate output linear block from block pool
    size_t max_out_size = width * height * 3 / 2;
    std::shared_ptr<C2LinearBlock> outBlock;
    c2_status_t status = pool->fetchLinearBlock(
        max_out_size,
        { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE },
        &outBlock);

    if (status != C2_OK || !outBlock) {
        ALOGE("Failed to fetch output C2LinearBlock from pool: %d", status);
        mVaapi->DestroySurface(input_surface);
        work->result = C2_NO_MEMORY;
        return;
    }

    C2WriteView writeView = outBlock->map().get();
    if (writeView.error() != C2_OK) {
        ALOGE("Failed to map output C2LinearBlock: %d", writeView.error());
        mVaapi->DestroySurface(input_surface);
        work->result = C2_CORRUPTED;
        return;
    }

    // Direct hardware encode from imported surface straight into output linear block
    size_t actual_out_size = 0;
    bool is_keyframe = false;
    bool force_keyframe = (work->input.flags & C2FrameData::FLAG_SYNC_FRAME);

    bool ok = mVaapi->EncodeFrame(input_surface, force_keyframe,
                                  writeView.data(), max_out_size,
                                  &actual_out_size, &is_keyframe);

    // Free transient imported surface handle (underlying buffer kept by graphicBlock)
    mVaapi->DestroySurface(input_surface);

    if (!ok || actual_out_size == 0) {
        ALOGE("Hardware frame encoding failed in VA-API");
        work->result = C2_CORRUPTED;
        return;
    }

    // Populate output worklet
    std::shared_ptr<C2Buffer> outBuffer = C2Buffer::CreateLinearBuffer(
        outBlock->share(0, actual_out_size, C2Fence()));

    if (is_keyframe) {
        work->worklets.front()->output.flags = (C2FrameData::flags_t)(
            work->worklets.front()->output.flags | C2FrameData::FLAG_SYNC_FRAME);
    }

    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->worklets.front()->output.buffers.push_back(outBuffer);
}

c2_status_t C2RedroidEnc::drain(uint32_t drainMode,
                                const std::shared_ptr<C2BlockPool> &pool) {
    (void)drainMode;
    (void)pool;
    return C2_OK;
}

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

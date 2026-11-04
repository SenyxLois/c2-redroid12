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

#include <C2Component.h>
#include <C2Buffer.h>
#include <C2Work.h>
#include <SimpleC2Component.h>
#include <C2Config.h>

#include "vaapi/vaapi_wrapper.h"
#include "vaapi/gralloc_helper.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

class C2RedroidEnc : public SimpleC2Component {
public:
    class IntfImpl;

    C2RedroidEnc(const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2RedroidEnc();

    // From SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(const std::unique_ptr<C2Work> &work,
                 const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(uint32_t drainMode,
                      const std::shared_ptr<C2BlockPool> &pool) override;

protected:
    virtual CodecType getCodecType() const = 0;
    virtual const char *getComponentName() const = 0;

private:
    std::shared_ptr<IntfImpl> mIntf;
    std::shared_ptr<VaapiWrapper> mVaapi;
    bool mStarted = false;
    bool mSawInputEOS = false;
    bool mSawOutputEOS = false;

    uint32_t mWidth = 1280;
    uint32_t mHeight = 720;
    uint32_t mBitrate = 10000000;
    uint32_t mFrameRate = 60;
    uint32_t mIntraPeriod = 60;

    bool initEncoder();
    void releaseEncoder();
};

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

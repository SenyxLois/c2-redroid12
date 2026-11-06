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

#include "C2RedroidHevcEnc.h"
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

class C2RedroidHevcEnc::IntfImpl : public SimpleInterface<void>::BaseParams {
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : SimpleInterface<void>::BaseParams(
            helper,
            "c2.redroid.hevc.encoder",
            C2Component::KIND_ENCODER,
            C2Component::DOMAIN_VIDEO,
            "video/hevc") {
        noInputLatency();
        noOutputLatency();
        noPipelineLatency();
        noPrivateBuffers();
        noInputReferences();
        noOutputReferences();
        noTimeStretch();
        setDerivedInstance(this);

        addParameter(
            DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::input(0u, 1280, 720))
            .withFields({
                C2F(mSize, width).inRange(64, 4096, 16),
                C2F(mSize, height).inRange(64, 4096, 16),
            })
            .withSetter(SizeSetter)
            .build());

        addParameter(
            DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
            .withDefault(new C2StreamBitrateInfo::output(0u, 20000000))
            .withFields({C2F(mBitrate, value).inRange(64000, 200000000)})
            .withSetter(BitrateSetter)
            .build());

        addParameter(
            DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
            .withConstValue(new C2StreamUsageTuning::input(
                0u, (uint64_t)(0x10000 | 0x100 | 0x200) /* GRALLOC_USAGE_HW_VIDEO_ENCODER | HW_TEXTURE | HW_RENDER */))
            .build());

        addParameter(
            DefineParam(mRequestSync, C2_PARAMKEY_REQUEST_SYNC_FRAME)
            .withDefault(new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
            .withFields({C2F(mRequestSync, value).any()})
            .withSetter(RequestSyncSetter)
            .build());

        addParameter(
            DefineParam(mSyncFramePeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
            .withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
            .withFields({C2F(mSyncFramePeriod, value).any()})
            .withSetter(SyncFramePeriodSetter)
            .build());
    }

    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                          C2P<C2StreamPictureSizeInfo::input> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (me.v.width < 64 || me.v.width > 4096) {
            me.set().width = oldMe.v.width;
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
        }
        if (me.v.height < 64 || me.v.height > 4096) {
            me.set().height = oldMe.v.height;
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
        }
        return res;
    }

    static C2R BitrateSetter(bool mayBlock, C2P<C2StreamBitrateInfo::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R RequestSyncSetter(bool mayBlock, C2P<C2StreamRequestSyncFrameTuning::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R SyncFramePeriodSetter(bool mayBlock, C2P<C2StreamSyncFrameIntervalTuning::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

private:
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamUsageTuning::input> mUsage;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;
};

C2RedroidHevcEnc::C2RedroidHevcEnc(const std::shared_ptr<C2ComponentInterface> &intf)
    : C2RedroidEnc(intf) {
}

std::shared_ptr<C2Component> C2RedroidHevcEnc::Create(const std::shared_ptr<C2ReflectorHelper> &reflector) {
    auto intfImpl = std::make_shared<IntfImpl>(reflector);
    auto intf = std::make_shared<SimpleC2Interface<IntfImpl>>("c2.redroid.hevc.encoder", 0, intfImpl);
    return std::make_shared<C2RedroidHevcEnc>(intf);
}

std::shared_ptr<C2ComponentInterface> C2RedroidHevcEnc::CreateInterface(const std::shared_ptr<C2ReflectorHelper> &reflector) {
    auto intfImpl = std::make_shared<IntfImpl>(reflector);
    return std::make_shared<SimpleC2Interface<IntfImpl>>("c2.redroid.hevc.encoder", 0, intfImpl);
}

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

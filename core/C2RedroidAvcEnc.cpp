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

#include "C2RedroidAvcEnc.h"
#include <C2InterfaceHelper.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

class C2RedroidAvcEnc::IntfImpl : public C2InterfaceHelper {
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : C2InterfaceHelper(helper) {
        setHelper(helper);
        setDerivedInstance(this);

        addParameter(
            DefineParam(mInputFormat, C2_PARAMKEY_INPUT_STREAM_BUFFER_TYPE)
            .withConstValue(new C2StreamBufferTypeSetting::input(0u, C2BufferData::GRAPHIC))
            .build());

        addParameter(
            DefineParam(mOutputFormat, C2_PARAMKEY_OUTPUT_STREAM_BUFFER_TYPE)
            .withConstValue(new C2StreamBufferTypeSetting::output(0u, C2BufferData::LINEAR))
            .build());

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
            .withDefault(new C2StreamBitrateInfo::output(0u, 10000000))
            .withFields({C2F(mBitrate, value).inRange(64000, 100000000)})
            .withSetter(BitrateSetter)
            .build());
    }

    static C2R SizeSetter(bool mayBlock, C2P<C2StreamPictureSizeInfo::input> &me) {
    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                          C2P<C2StreamPictureSizeInfo::input> &me) {
        (void)mayBlock;
        return me.F(me.v.width).validate() ? C2R::Ok() : C2R::BadValue();
        (void)oldMe;
        (void)me;
        return C2R::Ok();
    }

    static C2R BitrateSetter(bool mayBlock, C2P<C2StreamBitrateInfo::output> &me) {
        (void)mayBlock;
        return me.F(me.v.value).validate() ? C2R::Ok() : C2R::BadValue();
        (void)me;
        return C2R::Ok();
    }

private:
    std::shared_ptr<C2StreamBufferTypeSetting::input> mInputFormat;
    std::shared_ptr<C2StreamBufferTypeSetting::output> mOutputFormat;
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
};

C2RedroidAvcEnc::C2RedroidAvcEnc(const std::shared_ptr<IntfImpl> &intfImpl)
    : C2RedroidEnc(intfImpl) {
C2RedroidAvcEnc::C2RedroidAvcEnc(const std::shared_ptr<C2ComponentInterface> &intf)
    : C2RedroidEnc(intf) {
}

std::shared_ptr<C2Component> C2RedroidAvcEnc::Create(const std::shared_ptr<C2ReflectorHelper> &reflector) {
    auto intfImpl = std::make_shared<IntfImpl>(reflector);
    auto intf = std::make_shared<SimpleC2Interface<IntfImpl>>("c2.redroid.avc.encoder", 0, intfImpl);
    return std::make_shared<C2RedroidAvcEnc>(intf);
}

std::shared_ptr<C2ComponentInterface> C2RedroidAvcEnc::CreateInterface(const std::shared_ptr<C2ReflectorHelper> &reflector) {
    auto intfImpl = std::make_shared<IntfImpl>(reflector);
    return std::make_shared<SimpleC2Interface<IntfImpl>>("c2.redroid.avc.encoder", 0, intfImpl);
}

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

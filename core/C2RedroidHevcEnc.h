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

#include "C2RedroidEnc.h"

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

class C2RedroidHevcEnc : public C2RedroidEnc {
public:
    class IntfImpl;

    C2RedroidHevcEnc(const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2RedroidHevcEnc() = default;

protected:
    CodecType getCodecType() const override { return CodecType::HEVC; }
    const char *getComponentName() const override { return "c2.redroid.hevc.encoder"; }
};

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

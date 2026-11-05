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

#include <C2Component.h>
#include <C2ComponentFactory.h>
#include <util/C2InterfaceHelper.h>
#include <vector>
#include <memory>
#include <mutex>

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

class C2RedroidComponentStore : public C2ComponentStore {
public:
    C2RedroidComponentStore();
    virtual ~C2RedroidComponentStore() = default;

    c2_status_t createComponent(
            C2String name,
            std::shared_ptr<C2Component> *const component) override;

    c2_status_t createInterface(
            C2String name,
            std::shared_ptr<C2ComponentInterface> *const interface) override;

    std::vector<std::shared_ptr<const C2Component::Traits>> listComponents() override;

    C2String getName() const override { return "c2.redroid.store"; }

    c2_status_t copyBuffer(
            std::shared_ptr<C2GraphicBuffer> src,
            std::shared_ptr<C2GraphicBuffer> dst) override;

    c2_status_t query_sm(
            const std::vector<C2Param *> &stackParams,
            const std::vector<C2Param::Index> &heapParamIndices,
            std::vector<std::unique_ptr<C2Param>> *const heapParams) const override;

    c2_status_t config_sm(
            const std::vector<C2Param *> &params,
            std::vector<std::unique_ptr<C2SettingResult>> *const failures) override;

    std::shared_ptr<C2ParamReflector> getParamReflector() const override;

    c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const override;

    c2_status_t querySupportedValues_sm(
            std::vector<C2FieldSupportedValuesQuery> &fields) const override;

private:
    std::shared_ptr<C2ReflectorHelper> mReflector;
    std::vector<std::shared_ptr<const C2Component::Traits>> mTraitsList;
    void initTraits();
};

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

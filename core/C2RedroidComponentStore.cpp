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

#include "C2RedroidComponentStore.h"
#include "C2RedroidAvcEnc.h"
#include "C2RedroidHevcEnc.h"
#include <C2Config.h>
#include <log/log.h>
#include <string.h>

#define LOG_TAG "C2RedroidStore"

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

C2RedroidComponentStore::C2RedroidComponentStore()
    : mReflector(std::make_shared<C2ReflectorHelper>()) {
    initTraits();
}

void C2RedroidComponentStore::initTraits() {
    {
        auto traits = std::make_shared<C2Component::Traits>();
        traits->name = "c2.redroid.avc.encoder";
        traits->domain = C2Component::DOMAIN_VIDEO;
        traits->kind = C2Component::KIND_ENCODER;
        traits->rank = 256;  // beat c2.android.sw (512) for codec matching
        traits->mediaType = "video/avc";
        mTraitsList.push_back(traits);
    }

    {
        auto traits = std::make_shared<C2Component::Traits>();
        traits->name = "c2.redroid.hevc.encoder";
        traits->domain = C2Component::DOMAIN_VIDEO;
        traits->kind = C2Component::KIND_ENCODER;
        traits->rank = 256;
        traits->mediaType = "video/hevc";
        mTraitsList.push_back(traits);
    }
}

c2_status_t C2RedroidComponentStore::createComponent(
        C2String name,
        std::shared_ptr<C2Component> *const component) {
    component->reset();

    if (name == "c2.redroid.avc.encoder") {
        *component = C2RedroidAvcEnc::Create(mReflector);
        return C2_OK;
    } else if (name == "c2.redroid.hevc.encoder") {
        *component = C2RedroidHevcEnc::Create(mReflector);
        return C2_OK;
    }

    return C2_NOT_FOUND;
}

c2_status_t C2RedroidComponentStore::createInterface(
        C2String name,
        std::shared_ptr<C2ComponentInterface> *const interface) {
    interface->reset();

    if (name == "c2.redroid.avc.encoder") {
        *interface = C2RedroidAvcEnc::CreateInterface(mReflector);
        return C2_OK;
    } else if (name == "c2.redroid.hevc.encoder") {
        *interface = C2RedroidHevcEnc::CreateInterface(mReflector);
        return C2_OK;
    }

    return C2_NOT_FOUND;
}

std::vector<std::shared_ptr<const C2Component::Traits>> C2RedroidComponentStore::listComponents() {
    return mTraitsList;
}

c2_status_t C2RedroidComponentStore::copyBuffer(
        std::shared_ptr<C2GraphicBuffer> src,
        std::shared_ptr<C2GraphicBuffer> dst) {
    (void)src;
    (void)dst;
    return C2_OMITTED;
}

c2_status_t C2RedroidComponentStore::query_sm(
        const std::vector<C2Param *> &stackParams,
        const std::vector<C2Param::Index> &heapParamIndices,
        std::vector<std::unique_ptr<C2Param>> *const heapParams) const {
    (void)stackParams;
    (void)heapParamIndices;
    (void)heapParams;
    return C2_OK;
}

c2_status_t C2RedroidComponentStore::config_sm(
        const std::vector<C2Param *> &params,
        std::vector<std::unique_ptr<C2SettingResult>> *const failures) {
    (void)failures;
    for (C2Param *param : params) {
        if (!param) continue;
        if (param->type() == C2StoreDmaBufUsageInfo::PARAM_TYPE) {
            C2StoreDmaBufUsageInfo *dmaUsage = (C2StoreDmaBufUsageInfo *)param;
            long long usage = (long long)dmaUsage->m.usage;
            size_t maxLen = dmaUsage->flexCount();
            if (maxLen > 0) {
                const char *heap = "system";
                if (!(usage & (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE))) {
                    heap = "system-uncached";
                }
                strncpy(dmaUsage->m.heapName, heap, maxLen - 1);
                dmaUsage->m.heapName[maxLen - 1] = '\0';
                dmaUsage->m.allocFlags = 0;
                ALOGI("config_sm: mapped dmabuf usage 0x%llx to heap '%s'", (unsigned long long)usage, dmaUsage->m.heapName);
            }
        } else if (param->type() == C2StoreIonUsageInfo::PARAM_TYPE) {
            C2StoreIonUsageInfo *ionUsage = (C2StoreIonUsageInfo *)param;
            ionUsage->heapMask = ~0;
            ionUsage->allocFlags = 0;
            ionUsage->minAlignment = 0;
            ALOGI("config_sm: configured ion usage");
        }
    }
    return C2_OK;
}

std::shared_ptr<C2ParamReflector> C2RedroidComponentStore::getParamReflector() const {
    return mReflector;
}

c2_status_t C2RedroidComponentStore::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const {
    (void)params;
    return C2_OK;
}

c2_status_t C2RedroidComponentStore::querySupportedValues_sm(
        std::vector<C2FieldSupportedValuesQuery> &fields) const {
    for (auto &fsv : fields) {
        fsv.status = C2_BAD_INDEX;
    }
    return C2_OK;
}

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

extern "C" ::C2ComponentStore* CreateCodec2Store() {
    return new ::android::hardware::media::c2::redroid::C2RedroidComponentStore();
}

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

#define LOG_TAG "android.hardware.media.c2@1.2-service.redroid"

#include <android/hardware/media/c2/1.2/IComponentStore.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlSupport.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>
#include <binder/ProcessState.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "../core/C2RedroidComponentStore.h"

using namespace android;
using namespace android::hardware;
using namespace android::hardware::media::c2::V1_2;

int main(int /* argc */, char** /* argv */) {
    ALOGI("Starting Redroid Codec2 (c2-redroid12) Hardware HAL Service...");

    // Elevate priority for low-latency streaming
    setpriority(PRIO_PROCESS, 0, -10);

    // Ensure 64-bit / 32-bit threadpool supports concurrent streams
    configureRpcThreadpool(16, true /* callerWillJoin */);

    // Create Redroid VA-API Hardware Component Store
    std::shared_ptr<C2ComponentStore> store =
        std::make_shared<media::c2::redroid::C2RedroidComponentStore>();

    if (!store) {
        ALOGE("Failed to create C2RedroidComponentStore");
        return 1;
    }

    // Wrap in AOSP HIDL 1.2 interface
    sp<IComponentStore> hidlStore = new utils::ComponentStore(store);

    if (hidlStore == nullptr) {
        ALOGE("Cannot create HIDL ComponentStore wrapper");
        return 1;
    }

    const std::string serviceName = "default";
    status_t status = hidlStore->registerAsService(serviceName);
    if (status != OK) {
        ALOGE("Cannot register Codec2 Redroid service '%s' (status: %d)",
              serviceName.c_str(), status);
        return 1;
    }

    ALOGI("Redroid Codec2 HAL service '%s' registered successfully. Entering joinRpcThreadpool.",
          serviceName.c_str());

    joinRpcThreadpool();
    return 0;
}

#
# Copyright (C) 2018 The Android Open Source Project
# Copyright (C) 2026 Xynes / Redroid C2 Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libcodec2_redroid_vaapi
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := both

LOCAL_SRC_FILES := \
    vaapi/vaapi_wrapper.cpp \
    core/C2RedroidEnc.cpp \
    core/C2RedroidAvcEnc.cpp \
    core/C2RedroidHevcEnc.cpp \
    core/C2RedroidComponentStore.cpp \

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/vaapi \
    $(LOCAL_PATH)/core \

LOCAL_SHARED_LIBRARIES := \
    libva \
    libva-drm \
    libdrm \
    libcodec2 \
    libcodec2_vndk \
    libcodec2_simple_component \
    libstagefright_foundation \
    libbase \
    libutils \
    libcutils \
    liblog \
    libui \

LOCAL_CFLAGS := -Wall -Werror -Wno-unused-parameter -O3
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := android.hardware.media.c2@1.2-service.redroid
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MULTILIB := first

LOCAL_SRC_FILES := service/service.cpp
LOCAL_SHARED_LIBRARIES := \
    libcodec2_redroid_vaapi \
    android.hardware.media.c2@1.0 \
    android.hardware.media.c2@1.1 \
    android.hardware.media.c2@1.2 \
    libcodec2_hidl@1.2 \
    libbinder \
    libhidlbase \
    libutils \
    libcutils \
    liblog \

LOCAL_INIT_RC := android.hardware.media.c2@1.2-service.redroid.rc
LOCAL_VINTF_FRAGMENTS := manifest_media_c2.xml
LOCAL_CFLAGS := -Wall -Werror -O3
include $(BUILD_EXECUTABLE)

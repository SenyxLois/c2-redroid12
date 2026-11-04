#
# Copyright (C) 2026 Xynes / Redroid C2 Project
# Legacy GNU Make build file for AOSP compatibility
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

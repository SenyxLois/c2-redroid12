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

DEVICE_MANIFEST_FILE += hardware/redroid/c2-redroid12/manifest_media_c2.xml

PRODUCT_PACKAGES += \
    android.hardware.media.c2@1.2-service.redroid \
    libcodec2_redroid_vaapi \

PRODUCT_COPY_FILES += \
    hardware/redroid/c2-redroid12/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml \
    hardware/redroid/c2-redroid12/android.hardware.media.c2@1.2-service.redroid.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/android.hardware.media.c2@1.2-service.redroid.rc \

# force framework to pass native graphic buffers instead of byte buffers
PRODUCT_PROPERTY_OVERRIDES += \
    ro.media.c2.redroid=1 \
    debug.stagefright.c2inputsurface=-1 \

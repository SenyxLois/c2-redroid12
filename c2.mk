#
# Copyright (C) 2026 Xynes / Redroid C2 Project
# Makefile snippet for Redroid AOSP device configuration
#

# Add VINTF Device Manifest for Codec2 1.2
DEVICE_MANIFEST_FILE += hardware/redroid/c2-redroid12/manifest_media_c2.xml

# Codec2 Hardware HAL Service and Core VA-API Libraries
PRODUCT_PACKAGES += \
    android.hardware.media.c2@1.2-service.redroid \
    libcodec2_redroid_vaapi \

# Product Copy Files
PRODUCT_COPY_FILES += \
    hardware/redroid/c2-redroid12/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml \
    hardware/redroid/c2-redroid12/android.hardware.media.c2@1.2-service.redroid.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/android.hardware.media.c2@1.2-service.redroid.rc \

# Property flags
PRODUCT_PROPERTY_OVERRIDES += \
    ro.media.c2.redroid=1 \
    debug.stagefright.c2inputsurface=-1 \

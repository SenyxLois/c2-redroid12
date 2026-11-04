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

#include "vaapi_wrapper.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <log/log.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

#define LOG_TAG "C2RedroidVaapi"

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

std::shared_ptr<VaapiWrapper> VaapiWrapper::Create(const VaapiEncoderConfig &config) {
    auto wrapper = std::shared_ptr<VaapiWrapper>(new VaapiWrapper(config));
    if (!wrapper->Initialize()) {
        ALOGE("Failed to initialize VA-API encoder context");
        return nullptr;
    }
    return wrapper;
}

VaapiWrapper::VaapiWrapper(const VaapiEncoderConfig &config)
    : mConfig(config) {}

VaapiWrapper::~VaapiWrapper() {
    Deinitialize();
}

bool VaapiWrapper::InitDrmDevice() {
    const char *node = mConfig.drm_node ? mConfig.drm_node : "/dev/dri/renderD128";
    mDrmFd = open(node, O_RDWR);
    if (mDrmFd < 0) {
        ALOGE("Failed to open DRM render node %s: %s", node, strerror(errno));
        mDrmFd = open("/dev/dri/card0", O_RDWR);  // fallback if render node wasn't passed into the container
        if (mDrmFd < 0) {
            ALOGE("Failed fallback to /dev/dri/card0: %s", strerror(errno));
            return false;
        }
    }

    mVaDisplay = vaGetDisplayDRM(mDrmFd);
    if (!mVaDisplay) {
        ALOGE("vaGetDisplayDRM failed");
        return false;
    }

    int major = 0, minor = 0;
    VAStatus status = vaInitialize(mVaDisplay, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaInitialize failed: %s", vaErrorStr(status));
        return false;
    }

    ALOGI("VA-API initialized successfully: version %d.%d on DRM fd %d", major, minor, mDrmFd);
    return true;
}

bool VaapiWrapper::Initialize() {
    std::lock_guard<std::mutex> lock(mLock);
    if (!InitDrmDevice()) {
        return false;
    }
    return CreateEncoderContext();
}

void VaapiWrapper::Deinitialize() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mVaDisplay) {
        if (mCodedBuf != VA_INVALID_ID) {
            vaDestroyBuffer(mVaDisplay, mCodedBuf);
            mCodedBuf = VA_INVALID_ID;
        }
        if (mVaContextId != VA_INVALID_ID) {
            vaDestroyContext(mVaDisplay, mVaContextId);
            mVaContextId = VA_INVALID_ID;
        }
        if (mVaConfigId != VA_INVALID_ID) {
            vaDestroyConfig(mVaDisplay, mVaConfigId);
            mVaConfigId = VA_INVALID_ID;
        }
        vaTerminate(mVaDisplay);
        mVaDisplay = nullptr;
    }
    if (mDrmFd >= 0) {
        close(mDrmFd);
        mDrmFd = -1;
    }
}

bool VaapiWrapper::CreateEncoderContext() {
    VAProfile profile = (mConfig.codec == CodecType::HEVC) ? VAProfileHEVCMain : VAProfileH264Main;
    VAEntrypoint entrypoint = VAEntrypointEncSlice;

    VAConfigAttrib attribs[2];
    attribs[0].type = VAConfigAttribRTFormat;
    attribs[0].value = VA_RT_FORMAT_YUV420;

    attribs[1].type = VAConfigAttribRateControl;
    attribs[1].value = (mConfig.rc_mode == RateControlMode::CBR) ? VA_RC_CBR : VA_RC_VBR;

    VAStatus status = vaCreateConfig(mVaDisplay, profile, entrypoint, attribs, 2, &mVaConfigId);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateConfig failed for profile %d: %s", profile, vaErrorStr(status));
        return false;
    }

    uint32_t coded_buf_size = mConfig.width * mConfig.height * 3 / 2;
    status = vaCreateBuffer(mVaDisplay, mVaConfigId, VAEncCodedBufferType,
                            coded_buf_size, 1, nullptr, &mCodedBuf);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateBuffer VAEncCodedBufferType failed: %s", vaErrorStr(status));
        return false;
    }

    status = vaCreateContext(mVaDisplay, mVaConfigId, mConfig.width, mConfig.height,
                             VA_PROGRESSIVE, nullptr, 0, &mVaContextId);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateContext failed: %s", vaErrorStr(status));
        return false;
    }

    SetupRateControl();
    ALOGI("VA-API encoder context ready: %ux%u @ %ufps, target bitrate: %u bps",
          mConfig.width, mConfig.height, mConfig.frame_rate, mConfig.bit_rate);
    return true;
}

bool VaapiWrapper::SetupRateControl() {
    VAEncMiscParameterBuffer *misc_param = nullptr;
    VAEncMiscParameterRateControl *rc_param = nullptr;
    VABufferID rc_buf_id = VA_INVALID_ID;

    unsigned int total_size = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl);
    VAStatus status = vaCreateBuffer(mVaDisplay, mVaContextId, VAEncMiscParameterBufferType,
                                     total_size, 1, nullptr, &rc_buf_id);
    if (status != VA_STATUS_SUCCESS) {
        return false;
    }

    vaMapBuffer(mVaDisplay, rc_buf_id, (void **)&misc_param);
    if (!misc_param) {
        vaDestroyBuffer(mVaDisplay, rc_buf_id);
        return false;
    }

    misc_param->type = VAEncMiscParameterTypeRateControl;
    rc_param = (VAEncMiscParameterRateControl *)misc_param->data;
    memset(rc_param, 0, sizeof(*rc_param));

    rc_param->bits_per_second = mConfig.bit_rate;
    rc_param->target_percentage = 95;
    rc_param->window_size = 500;  // 500ms sliding window keeps bit spikes in check without lag
    rc_param->initial_qp = mConfig.initial_qp;
    rc_param->min_qp = mConfig.min_qp;
    rc_param->basic_unit_size = 0;
    rc_param->rc_flags.bits.reset = 1;

    vaUnmapBuffer(mVaDisplay, rc_buf_id);
    vaRenderPicture(mVaDisplay, mVaContextId, &rc_buf_id, 1);
    vaDestroyBuffer(mVaDisplay, rc_buf_id);
    return true;
}

VASurfaceID VaapiWrapper::ImportDrmPrimeSurface(const DrmPrimeBuffer &buf) {
    if (!mVaDisplay || buf.prime_fd < 0) {
        ALOGE("Invalid VA display or prime_fd");
        return VA_INVALID_SURFACE;
    }

    VADRMPRIMESurfaceDescriptor prime_desc{};
    prime_desc.fourcc = buf.fourcc ? buf.fourcc : VA_FOURCC_NV12;
    prime_desc.width = buf.width;
    prime_desc.height = buf.height;
    prime_desc.num_layers = 1;

    prime_desc.layers[0].drm_format = buf.fourcc;
    prime_desc.layers[0].num_planes = 2;
    prime_desc.layers[0].pitches[0] = buf.stride;
    prime_desc.layers[0].offset[0] = 0;
    prime_desc.layers[0].pitches[1] = buf.stride;
    prime_desc.layers[0].offset[1] = buf.stride * buf.height;
    prime_desc.layers[0].object_index[0] = 0;
    prime_desc.layers[0].object_index[1] = 0;

    prime_desc.num_objects = 1;
    prime_desc.objects[0].fd = buf.prime_fd;
    prime_desc.objects[0].size = buf.total_size > 0 ? buf.total_size : (buf.stride * buf.height * 3 / 2);
    prime_desc.objects[0].drm_format_modifier = buf.modifier;

    VASurfaceAttrib attribs[2];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &prime_desc;

    VASurfaceID surface_id = VA_INVALID_SURFACE;
    VAStatus status = vaCreateSurfaces(mVaDisplay, VA_RT_FORMAT_YUV420,
                                       buf.width, buf.height,
                                       &surface_id, 1, attribs, 2);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateSurfaces DRM PRIME 2 import failed: %s (fd=%d, %ux%u)",
              vaErrorStr(status), buf.prime_fd, buf.width, buf.height);
        return VA_INVALID_SURFACE;
    }

    return surface_id;
}

void VaapiWrapper::DestroySurface(VASurfaceID surface_id) {
    if (mVaDisplay && surface_id != VA_INVALID_SURFACE) {
        vaDestroySurfaces(mVaDisplay, &surface_id, 1);
    }
}

bool VaapiWrapper::RenderSequenceParameters() {
    if (mConfig.codec == CodecType::H264) {
        VAEncSequenceParameterBufferH264 seq_param{};
        seq_param.seq_parameter_set_id = 0;
        seq_param.level_idc = 41;  // level 4.1 needed for 1080p60
        seq_param.intra_period = mConfig.intra_period;
        seq_param.intra_idr_period = mConfig.intra_period;
        seq_param.ip_period = mConfig.ip_period;
        seq_param.bits_per_second = mConfig.bit_rate;
        seq_param.max_num_ref_frames = 1;
        seq_param.picture_width_in_mbs = (mConfig.width + 15) / 16;
        seq_param.picture_height_in_mbs = (mConfig.height + 15) / 16;
        seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
        seq_param.seq_fields.bits.direct_8x8_inference_flag = 1;

        VABufferID seq_buf_id = VA_INVALID_ID;
        vaCreateBuffer(mVaDisplay, mVaContextId, VAEncSequenceParameterBufferType,
                       sizeof(seq_param), 1, &seq_param, &seq_buf_id);
        vaRenderPicture(mVaDisplay, mVaContextId, &seq_buf_id, 1);
        vaDestroyBuffer(mVaDisplay, seq_buf_id);
    }
    return true;
}

bool VaapiWrapper::RenderPictureParameters(VASurfaceID surface, bool is_keyframe) {
    if (mConfig.codec == CodecType::H264) {
        VAEncPictureParameterBufferH264 pic_param{};
        pic_param.reference_picture = surface;
        pic_param.reconstructed_picture = surface;
        pic_param.coded_buf = mCodedBuf;
        pic_param.picture_width = mConfig.width;
        pic_param.picture_height = mConfig.height;
        pic_param.pic_fields.bits.idr_pic_flag = is_keyframe ? 1 : 0;
        pic_param.pic_fields.bits.reference_pic_flag = 1;

        VABufferID pic_buf_id = VA_INVALID_ID;
        vaCreateBuffer(mVaDisplay, mVaContextId, VAEncPictureParameterBufferType,
                       sizeof(pic_param), 1, &pic_param, &pic_buf_id);
        vaRenderPicture(mVaDisplay, mVaContextId, &pic_buf_id, 1);
        vaDestroyBuffer(mVaDisplay, pic_buf_id);
    }
    return true;
}

bool VaapiWrapper::RenderSliceParameters(VASurfaceID surface, bool is_keyframe) {
    if (mConfig.codec == CodecType::H264) {
        VAEncSliceParameterBufferH264 slice_param{};
        slice_param.start_row_number = 0;
        slice_param.slice_height = (mConfig.height + 15) / 16;
        slice_param.slice_type = is_keyframe ? 2 : 0;  // 2 = I-slice, 0 = P-slice in vaapi

        VABufferID slice_buf_id = VA_INVALID_ID;
        vaCreateBuffer(mVaDisplay, mVaContextId, VAEncSliceParameterBufferType,
                       sizeof(slice_param), 1, &slice_param, &slice_buf_id);
        vaRenderPicture(mVaDisplay, mVaContextId, &slice_buf_id, 1);
        vaDestroyBuffer(mVaDisplay, slice_buf_id);
    }
    return true;
}

bool VaapiWrapper::EncodeFrame(VASurfaceID input_surface,
                               bool force_keyframe,
                               void *out_linear_buf,
                               size_t out_max_size,
                               size_t *out_actual_size,
                               bool *out_is_keyframe) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mVaDisplay || input_surface == VA_INVALID_SURFACE || !out_linear_buf) {
        return false;
    }

    bool is_keyframe = (mFrameIndex % mConfig.intra_period == 0) || force_keyframe || mForceKeyFrame;
    mForceKeyFrame = false;

    VAStatus status = vaBeginPicture(mVaDisplay, mVaContextId, input_surface);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaBeginPicture failed: %s", vaErrorStr(status));
        return false;
    }

    if (is_keyframe) {
        RenderSequenceParameters();
    }
    RenderPictureParameters(input_surface, is_keyframe);
    RenderSliceParameters(input_surface, is_keyframe);

    status = vaEndPicture(mVaDisplay, mVaContextId);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaEndPicture failed: %s", vaErrorStr(status));
        return false;
    }

    status = vaSyncSurface(mVaDisplay, input_surface);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaSyncSurface failed: %s", vaErrorStr(status));
        return false;
    }

    VACodedBufferSegment *buf_segment = nullptr;
    status = vaMapBuffer(mVaDisplay, mCodedBuf, (void **)&buf_segment);
    if (status != VA_STATUS_SUCCESS || !buf_segment) {
        ALOGE("vaMapBuffer failed: %s", vaErrorStr(status));
        return false;
    }

    size_t copied = 0;
    VACodedBufferSegment *curr = buf_segment;
    while (curr) {
        if (copied + curr->size <= out_max_size) {
            memcpy(reinterpret_cast<uint8_t *>(out_linear_buf) + copied, curr->buf, curr->size);
            copied += curr->size;
        } else {
            ALOGW("Output linear buffer overflow (copied %zu, chunk %u, max %zu)",
                  copied, curr->size, out_max_size);
            break;
        }
        curr = (VACodedBufferSegment *)curr->next;
    }

    vaUnmapBuffer(mVaDisplay, mCodedBuf);

    *out_actual_size = copied;
    if (out_is_keyframe) {
        *out_is_keyframe = is_keyframe;
    }

    mFrameIndex++;
    return true;
}

bool VaapiWrapper::RequestKeyFrame() {
    mForceKeyFrame = true;
    return true;
}

bool VaapiWrapper::UpdateBitrate(uint32_t new_bitrate) {
    std::lock_guard<std::mutex> lock(mLock);
    mConfig.bit_rate = new_bitrate;
    return SetupRateControl();
}

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

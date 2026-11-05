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

#define LOG_TAG "C2RedroidVaapi"

#include "vaapi_wrapper.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <log/log.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <drm/drm_fourcc.h>

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

namespace {

class BitWriter {
public:
    void writeBits(uint32_t val, int numBits) {
        for (int i = numBits - 1; i >= 0; --i) writeBit((val >> i) & 1);
    }
    void writeBit(uint32_t bit) {
        mCurByte = (mCurByte << 1) | (bit & 1);
        mBitIndex++;
        if (mBitIndex == 8) {
            mData.push_back(mCurByte);
            mCurByte = 0;
            mBitIndex = 0;
        }
    }
    void writeUe(uint32_t val) {
        if (val == 0) { writeBit(1); return; }
        uint32_t temp = val + 1;
        int len = 0;
        while (temp > 0) { temp >>= 1; len++; }
        for (int i = 0; i < len - 1; ++i) writeBit(0);
        writeBits(val + 1, len);
    }
    void writeSe(int32_t val) {
        if (val <= 0) writeUe((uint32_t)(-val * 2));
        else writeUe((uint32_t)(val * 2 - 1));
    }
    std::vector<uint8_t> flushRbsp() {
        if (mBitIndex > 0) {
            mCurByte <<= (8 - mBitIndex);
            mCurByte |= (1 << (7 - mBitIndex));
            mData.push_back(mCurByte);
            mCurByte = 0;
            mBitIndex = 0;
        }
        std::vector<uint8_t> rbsp;
        int zeroCount = 0;
        for (uint8_t b : mData) {
            if (zeroCount == 2 && b <= 3) {
                rbsp.push_back(3);  // emulation prevention byte (0x000003)
                zeroCount = 0;
            }
            rbsp.push_back(b);
            if (b == 0) {
                zeroCount++;
            } else {
                zeroCount = 0;
            }
        }
        return rbsp;
    }
private:
    std::vector<uint8_t> mData;
    uint8_t mCurByte = 0;
    int mBitIndex = 0;
};

void GenerateH264Headers(uint32_t width, uint32_t height, std::vector<uint8_t> &out) {
    BitWriter spsBw;
    spsBw.writeBits(77, 8);
    spsBw.writeBits(0x40, 8);
    spsBw.writeBits(41, 8);  // level 4.1 for 1080p60
    spsBw.writeUe(0);
    spsBw.writeUe(0);
    spsBw.writeUe(0);
    spsBw.writeUe(0);
    spsBw.writeUe(1);
    spsBw.writeBit(0);

    uint32_t pic_width_in_mbs_minus1 = (width + 15) / 16 - 1;
    uint32_t pic_height_in_map_units_minus1 = (height + 15) / 16 - 1;
    spsBw.writeUe(pic_width_in_mbs_minus1);
    spsBw.writeUe(pic_height_in_map_units_minus1);
    spsBw.writeBit(1);
    spsBw.writeBit(1);

    uint32_t crop_right = ((pic_width_in_mbs_minus1 + 1) * 16 - width) / 2;
    uint32_t crop_bottom = ((pic_height_in_map_units_minus1 + 1) * 16 - height) / 2;
    if (crop_right > 0 || crop_bottom > 0) {
        spsBw.writeBit(1);
        spsBw.writeUe(0);
        spsBw.writeUe(crop_right);
        spsBw.writeUe(0);
        spsBw.writeUe(crop_bottom);
    } else {
        spsBw.writeBit(0);
    }

    // VUI parameters for BT.709 color space
    spsBw.writeBit(1);
    spsBw.writeBit(0);
    spsBw.writeBit(0);
    spsBw.writeBit(1);
    spsBw.writeBits(5, 3);
    spsBw.writeBit(0);
    spsBw.writeBit(1);
    spsBw.writeBits(1, 8);
    spsBw.writeBits(1, 8);
    spsBw.writeBits(1, 8);
    spsBw.writeBit(0);
    spsBw.writeBit(0);
    spsBw.writeBit(0);
    spsBw.writeBit(0);
    spsBw.writeBit(0);
    spsBw.writeBit(0);

    std::vector<uint8_t> spsRbsp = spsBw.flushRbsp();

    BitWriter ppsBw;
    ppsBw.writeUe(0);
    ppsBw.writeUe(0);
    ppsBw.writeBit(1);
    ppsBw.writeBit(0);
    ppsBw.writeUe(0);
    ppsBw.writeUe(0);
    ppsBw.writeUe(0);
    ppsBw.writeBit(0);
    ppsBw.writeBits(0, 2);
    ppsBw.writeSe(0);
    ppsBw.writeSe(0);
    ppsBw.writeSe(0);
    ppsBw.writeBit(0);  // matches intel vaapi driver deblocking settings
    ppsBw.writeBit(0);
    ppsBw.writeBit(0);
    std::vector<uint8_t> ppsRbsp = ppsBw.flushRbsp();

    const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};
    out.clear();
    out.insert(out.end(), startCode, startCode + 4);
    out.push_back(0x67);  // sps
    out.insert(out.end(), spsRbsp.begin(), spsRbsp.end());

    out.insert(out.end(), startCode, startCode + 4);
    out.push_back(0x68);  // pps
    out.insert(out.end(), ppsRbsp.begin(), ppsRbsp.end());
}

} // anonymous namespace

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
    if (!CreateEncoderContext()) {
        return false;
    }
    return InitVpp();
}

void VaapiWrapper::Deinitialize() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mVaDisplay) {
        if (mNv12Surface != VA_INVALID_SURFACE) {
            vaDestroySurfaces(mVaDisplay, &mNv12Surface, 1);
            mNv12Surface = VA_INVALID_SURFACE;
        }
        if (mReconSurfaces[0] != VA_INVALID_SURFACE) {
            vaDestroySurfaces(mVaDisplay, &mReconSurfaces[0], 1);
            mReconSurfaces[0] = VA_INVALID_SURFACE;
        }
        if (mReconSurfaces[1] != VA_INVALID_SURFACE) {
            vaDestroySurfaces(mVaDisplay, &mReconSurfaces[1], 1);
            mReconSurfaces[1] = VA_INVALID_SURFACE;
        }
        if (mVppContextId != VA_INVALID_ID) {
            vaDestroyContext(mVaDisplay, mVppContextId);
            mVppContextId = VA_INVALID_ID;
        }
        if (mVppConfigId != VA_INVALID_ID) {
            vaDestroyConfig(mVaDisplay, mVppConfigId);
            mVppConfigId = VA_INVALID_ID;
        }
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

bool VaapiWrapper::InitVpp() {
    if (!mVaDisplay) return false;

    VAStatus status = vaCreateConfig(mVaDisplay, VAProfileNone, VAEntrypointVideoProc,
                                     nullptr, 0, &mVppConfigId);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateConfig for VPP failed: %s", vaErrorStr(status));
        return false;
    }

    status = vaCreateContext(mVaDisplay, mVppConfigId, mConfig.width, mConfig.height,
                             0, nullptr, 0, &mVppContextId);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateContext for VPP failed: %s", vaErrorStr(status));
        return false;
    }

    VASurfaceAttrib attr{};
    attr.type = VASurfaceAttribPixelFormat;
    attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attr.value.type = VAGenericValueTypeInteger;
    attr.value.value.i = VA_FOURCC_NV12;

    status = vaCreateSurfaces(mVaDisplay, VA_RT_FORMAT_YUV420,
                              mConfig.width, mConfig.height,
                              &mNv12Surface, 1, &attr, 1);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateSurfaces for NV12 VPP surface failed: %s", vaErrorStr(status));
        return false;
    }

    status = vaCreateSurfaces(mVaDisplay, VA_RT_FORMAT_YUV420,
                              mConfig.width, mConfig.height,
                              mReconSurfaces, 2, &attr, 1);
    if (status != VA_STATUS_SUCCESS) {
        ALOGW("vaCreateSurfaces for recon DPB surfaces failed: %s", vaErrorStr(status));
    }

    ALOGI("VA-API VPP & DPB initialized: NV12 %d, Recon [%d, %d]",
          mNv12Surface, mReconSurfaces[0], mReconSurfaces[1]);
    return true;
}

VASurfaceID VaapiWrapper::ConvertRgbToNv12(VASurfaceID rgb_surface) {
    if (mVppContextId == VA_INVALID_ID || mNv12Surface == VA_INVALID_SURFACE) {
        ALOGE("VPP not initialized for color conversion");
        return VA_INVALID_SURFACE;
    }

    VARectangle src_rect{0, 0, (uint16_t)mConfig.width, (uint16_t)mConfig.height};
    VARectangle dst_rect{0, 0, (uint16_t)mConfig.width, (uint16_t)mConfig.height};

    VAProcPipelineParameterBuffer pipeline_param{};
    memset(&pipeline_param, 0, sizeof(pipeline_param));
    pipeline_param.surface = rgb_surface;
    pipeline_param.surface_region = &src_rect;
    pipeline_param.output_region = &dst_rect;
    pipeline_param.surface_color_standard = VAProcColorStandardBT709;
    pipeline_param.output_color_standard = VAProcColorStandardBT709;

    VABufferID pipeline_buf = VA_INVALID_ID;
    VAStatus status = vaCreateBuffer(mVaDisplay, mVppContextId,
                                     VAProcPipelineParameterBufferType,
                                     sizeof(pipeline_param), 1,
                                     &pipeline_param, &pipeline_buf);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateBuffer for VPP pipeline failed: %s", vaErrorStr(status));
        return VA_INVALID_SURFACE;
    }

    status = vaBeginPicture(mVaDisplay, mVppContextId, mNv12Surface);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaBeginPicture for VPP failed: %s", vaErrorStr(status));
        vaDestroyBuffer(mVaDisplay, pipeline_buf);
        return VA_INVALID_SURFACE;
    }

    status = vaRenderPicture(mVaDisplay, mVppContextId, &pipeline_buf, 1);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaRenderPicture for VPP failed: %s", vaErrorStr(status));
        vaDestroyBuffer(mVaDisplay, pipeline_buf);
        return VA_INVALID_SURFACE;
    }

    status = vaEndPicture(mVaDisplay, mVppContextId);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaEndPicture for VPP failed: %s", vaErrorStr(status));
        vaDestroyBuffer(mVaDisplay, pipeline_buf);
        return VA_INVALID_SURFACE;
    }

    vaDestroyBuffer(mVaDisplay, pipeline_buf);
    vaSyncSurface(mVaDisplay, mNv12Surface);
    return mNv12Surface;
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

    status = vaCreateContext(mVaDisplay, mVaConfigId, mConfig.width, mConfig.height,
                             VA_PROGRESSIVE, nullptr, 0, &mVaContextId);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateContext failed: %s", vaErrorStr(status));
        return false;
    }

    uint32_t coded_buf_size = mConfig.width * mConfig.height * 3 / 2;
    status = vaCreateBuffer(mVaDisplay, mVaContextId, VAEncCodedBufferType,
                            coded_buf_size, 1, nullptr, &mCodedBuf);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateBuffer VAEncCodedBufferType failed: %s", vaErrorStr(status));
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
    rc_param->rc_flags.bits.disable_frame_skip = 1;

    vaUnmapBuffer(mVaDisplay, rc_buf_id);
    vaRenderPicture(mVaDisplay, mVaContextId, &rc_buf_id, 1);
    vaDestroyBuffer(mVaDisplay, rc_buf_id);

    // hrd params keep qp from fluctuating wildly between intra and inter frames
    VAEncMiscParameterBuffer *misc_hrd = nullptr;
    VAEncMiscParameterHRD *hrd_param = nullptr;
    VABufferID hrd_buf_id = VA_INVALID_ID;

    unsigned int hrd_size = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterHRD);
    status = vaCreateBuffer(mVaDisplay, mVaContextId, VAEncMiscParameterBufferType,
                            hrd_size, 1, nullptr, &hrd_buf_id);
    if (status == VA_STATUS_SUCCESS) {
        vaMapBuffer(mVaDisplay, hrd_buf_id, (void **)&misc_hrd);
        if (misc_hrd) {
            misc_hrd->type = VAEncMiscParameterTypeHRD;
            hrd_param = (VAEncMiscParameterHRD *)misc_hrd->data;
            memset(hrd_param, 0, sizeof(*hrd_param));
            hrd_param->buffer_size = mConfig.bit_rate * 2;
            hrd_param->initial_buffer_fullness = mConfig.bit_rate;
            vaUnmapBuffer(mVaDisplay, hrd_buf_id);
            vaRenderPicture(mVaDisplay, mVaContextId, &hrd_buf_id, 1);
        }
        vaDestroyBuffer(mVaDisplay, hrd_buf_id);
    }

    // explicitly pass framerate to vaapi rate controller
    VAEncMiscParameterBuffer *misc_fps = nullptr;
    VAEncMiscParameterFrameRate *fps_param = nullptr;
    VABufferID fps_buf_id = VA_INVALID_ID;

    unsigned int fps_size = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterFrameRate);
    status = vaCreateBuffer(mVaDisplay, mVaContextId, VAEncMiscParameterBufferType,
                            fps_size, 1, nullptr, &fps_buf_id);
    if (status == VA_STATUS_SUCCESS) {
        vaMapBuffer(mVaDisplay, fps_buf_id, (void **)&misc_fps);
        if (misc_fps) {
            misc_fps->type = VAEncMiscParameterTypeFrameRate;
            fps_param = (VAEncMiscParameterFrameRate *)misc_fps->data;
            memset(fps_param, 0, sizeof(*fps_param));
            fps_param->framerate = mConfig.frame_rate > 0 ? mConfig.frame_rate : 60;
            vaUnmapBuffer(mVaDisplay, fps_buf_id);
            vaRenderPicture(mVaDisplay, mVaContextId, &fps_buf_id, 1);
        }
        vaDestroyBuffer(mVaDisplay, fps_buf_id);
    }

    return true;
}

VASurfaceID VaapiWrapper::ImportDrmPrimeSurface(const DrmPrimeBuffer &buf) {
    if (!mVaDisplay || buf.prime_fd < 0) {
        ALOGE("Invalid VA display or prime_fd");
        return VA_INVALID_SURFACE;
    }

    bool is_rgb = (buf.fourcc == DRM_FORMAT_ABGR8888 ||
                   buf.fourcc == DRM_FORMAT_XBGR8888 ||
                   buf.fourcc == DRM_FORMAT_ARGB8888 ||
                   buf.fourcc == DRM_FORMAT_BGR888 ||
                   buf.fourcc == DRM_FORMAT_RGB565);

    VADRMPRIMESurfaceDescriptor prime_desc{};
    prime_desc.width = buf.width;
    prime_desc.height = buf.height;
    prime_desc.num_layers = 1;
    prime_desc.layers[0].drm_format = buf.fourcc;
    prime_desc.num_objects = 1;
    prime_desc.objects[0].fd = buf.prime_fd;
    prime_desc.objects[0].drm_format_modifier = buf.modifier;

    unsigned int rt_format = VA_RT_FORMAT_YUV420;

    if (is_rgb) {
        if (buf.fourcc == DRM_FORMAT_ARGB8888) {
            prime_desc.fourcc = VA_FOURCC_ARGB;
        } else if (buf.fourcc == DRM_FORMAT_XBGR8888) {
            prime_desc.fourcc = VA_FOURCC_XBGR;
        } else {
            prime_desc.fourcc = VA_FOURCC_ABGR;
        }
        prime_desc.layers[0].num_planes = 1;
        uint32_t byte_pitch = buf.stride;
        if (byte_pitch < buf.width * 4) {
            byte_pitch = buf.width * 4;
        }
        prime_desc.layers[0].pitch[0] = byte_pitch;
        prime_desc.layers[0].offset[0] = 0;
        prime_desc.layers[0].object_index[0] = 0;
        prime_desc.objects[0].size = buf.total_size > 0 ? buf.total_size : (byte_pitch * buf.height);
        rt_format = VA_RT_FORMAT_RGB32;
    } else {
        prime_desc.fourcc = VA_FOURCC_NV12;
        prime_desc.layers[0].num_planes = 2;
        prime_desc.layers[0].pitch[0] = buf.stride;
        prime_desc.layers[0].offset[0] = 0;
        prime_desc.layers[0].pitch[1] = buf.stride;
        prime_desc.layers[0].offset[1] = buf.stride * buf.height;
        prime_desc.layers[0].object_index[0] = 0;
        prime_desc.layers[0].object_index[1] = 0;
        prime_desc.objects[0].size = buf.total_size > 0 ? buf.total_size : (buf.stride * buf.height * 3 / 2);
        rt_format = VA_RT_FORMAT_YUV420;
    }

    VASurfaceAttrib attribs[2];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &prime_desc;

    VASurfaceID imported_surface = VA_INVALID_SURFACE;
    VAStatus status = vaCreateSurfaces(mVaDisplay, rt_format,
                                       buf.width, buf.height,
                                       &imported_surface, 1, attribs, 2);
    if (status != VA_STATUS_SUCCESS || imported_surface == VA_INVALID_SURFACE) {
        ALOGE("vaCreateSurfaces DRM PRIME 2 import failed: %s (fd=%d, fourcc=0x%x, rt_format=0x%x, %ux%u)",
              vaErrorStr(status), buf.prime_fd, buf.fourcc, rt_format, buf.width, buf.height);
        return VA_INVALID_SURFACE;
    }

    return imported_surface;
}

void VaapiWrapper::DestroySurface(VASurfaceID surface_id) {
    if (!mVaDisplay || surface_id == VA_INVALID_SURFACE) {
        return;
    }
    // intermediate vpp nv12 and recon dpb surfaces persist across frames
    if (surface_id == mNv12Surface ||
        surface_id == mReconSurfaces[0] ||
        surface_id == mReconSurfaces[1]) {
        return;
    }
    vaDestroySurfaces(mVaDisplay, &surface_id, 1);
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
        seq_param.seq_fields.bits.chroma_format_idc = 1;  // 4:2:0 YUV
        seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
        seq_param.seq_fields.bits.direct_8x8_inference_flag = 1;
        seq_param.seq_fields.bits.log2_max_frame_num_minus4 = 0;
        seq_param.seq_fields.bits.pic_order_cnt_type = 0;
        seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;

        VABufferID seq_buf_id = VA_INVALID_ID;
        vaCreateBuffer(mVaDisplay, mVaContextId, VAEncSequenceParameterBufferType,
                       sizeof(seq_param), 1, &seq_param, &seq_buf_id);
        vaRenderPicture(mVaDisplay, mVaContextId, &seq_buf_id, 1);
        vaDestroyBuffer(mVaDisplay, seq_buf_id);
    } else if (mConfig.codec == CodecType::HEVC) {
        VAEncSequenceParameterBufferHEVC seq_param{};
        seq_param.general_profile_idc = 1;  // main profile
        seq_param.general_level_idc = 120;  // level 4.0
        seq_param.general_tier_flag = 0;
        seq_param.intra_period = mConfig.intra_period;
        seq_param.intra_idr_period = mConfig.intra_period;
        seq_param.ip_period = mConfig.ip_period;
        seq_param.bits_per_second = mConfig.bit_rate;
        seq_param.pic_width_in_luma_samples = mConfig.width;
        seq_param.pic_height_in_luma_samples = mConfig.height;
        seq_param.seq_fields.bits.chroma_format_idc = 1;  // 4:2:0
        seq_param.seq_fields.bits.separate_colour_plane_flag = 0;
        seq_param.seq_fields.bits.bit_depth_luma_minus8 = 0;
        seq_param.seq_fields.bits.bit_depth_chroma_minus8 = 0;
        seq_param.seq_fields.bits.strong_intra_smoothing_enabled_flag = 1;
        seq_param.seq_fields.bits.amp_enabled_flag = 1;
        seq_param.seq_fields.bits.sample_adaptive_offset_enabled_flag = 1;
        seq_param.seq_fields.bits.sps_temporal_mvp_enabled_flag = 1;
        seq_param.seq_fields.bits.low_delay_seq = 1;
        seq_param.log2_min_luma_coding_block_size_minus3 = 0;  // min cu 8x8
        seq_param.log2_diff_max_min_luma_coding_block_size = 2;  // max cu 32x32 (ctu size 32)
        seq_param.log2_min_transform_block_size_minus2 = 0;  // min tu 4x4
        seq_param.log2_diff_max_min_transform_block_size = 3;  // max tu 32x32
        seq_param.max_transform_hierarchy_depth_inter = 2;
        seq_param.max_transform_hierarchy_depth_intra = 2;

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
        pic_param.CurrPic.picture_id = (mReconSurfaces[mCurrentRecon] != VA_INVALID_SURFACE)
                                        ? mReconSurfaces[mCurrentRecon] : surface;
        pic_param.CurrPic.frame_idx = is_keyframe ? 0 : (mFrameIndex % 16);
        pic_param.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        pic_param.CurrPic.TopFieldOrderCnt = is_keyframe ? 0 : (mFrameIndex % 16) * 2;
        pic_param.CurrPic.BottomFieldOrderCnt = is_keyframe ? 0 : (mFrameIndex % 16) * 2;
        pic_param.coded_buf = mCodedBuf;
        pic_param.pic_fields.bits.idr_pic_flag = is_keyframe ? 1 : 0;
        pic_param.pic_fields.bits.reference_pic_flag = 1;
        pic_param.pic_fields.bits.entropy_coding_mode_flag = 1;  // cabac
        pic_param.pic_fields.bits.deblocking_filter_control_present_flag = 0;
        pic_param.pic_init_qp = 26;
        pic_param.num_ref_idx_l0_active_minus1 = 0;
        pic_param.num_ref_idx_l1_active_minus1 = 0;
        for (int i = 0; i < 16; i++) {
            pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
            pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
        }

        if (!is_keyframe && mHasReference && mReconSurfaces[mPrevRecon] != VA_INVALID_SURFACE) {
            pic_param.ReferenceFrames[0].picture_id = mReconSurfaces[mPrevRecon];
            pic_param.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            pic_param.ReferenceFrames[0].frame_idx = (mFrameIndex - 1) % 16;
            pic_param.ReferenceFrames[0].TopFieldOrderCnt = ((mFrameIndex - 1) % 16) * 2;
            pic_param.ReferenceFrames[0].BottomFieldOrderCnt = ((mFrameIndex - 1) % 16) * 2;
        }

        VABufferID pic_buf_id = VA_INVALID_ID;
        vaCreateBuffer(mVaDisplay, mVaContextId, VAEncPictureParameterBufferType,
                       sizeof(pic_param), 1, &pic_param, &pic_buf_id);
        vaRenderPicture(mVaDisplay, mVaContextId, &pic_buf_id, 1);
        vaDestroyBuffer(mVaDisplay, pic_buf_id);
    } else if (mConfig.codec == CodecType::HEVC) {
        VAEncPictureParameterBufferHEVC pic_param{};
        pic_param.decoded_curr_pic.picture_id = (mReconSurfaces[mCurrentRecon] != VA_INVALID_SURFACE)
                                                ? mReconSurfaces[mCurrentRecon] : surface;
        pic_param.decoded_curr_pic.pic_order_cnt = mFrameIndex;
        pic_param.decoded_curr_pic.flags = 0;
        pic_param.coded_buf = mCodedBuf;
        pic_param.collocated_ref_pic_index = 0xFF;
        pic_param.nal_unit_type = is_keyframe ? 19 : 1;  // 19 = idr, 1 = non-idr
        pic_param.pic_init_qp = mConfig.initial_qp;
        pic_param.pic_fields.bits.idr_pic_flag = is_keyframe ? 1 : 0;
        pic_param.pic_fields.bits.coding_type = is_keyframe ? 1 : 2;  // 1 = I frame, 2 = P frame

        for (int i = 0; i < 15; i++) {
            pic_param.reference_frames[i].picture_id = VA_INVALID_SURFACE;
            pic_param.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
        }

        if (!is_keyframe && mHasReference && mReconSurfaces[mPrevRecon] != VA_INVALID_SURFACE) {
            pic_param.reference_frames[0].picture_id = mReconSurfaces[mPrevRecon];
            pic_param.reference_frames[0].flags = 0;
            pic_param.reference_frames[0].pic_order_cnt = mFrameIndex - 1;
        }

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
        slice_param.macroblock_address = 0;
        slice_param.num_macroblocks = ((mConfig.width + 15) / 16) * ((mConfig.height + 15) / 16);
        slice_param.macroblock_info = VA_INVALID_ID;
        slice_param.slice_type = is_keyframe ? 2 : 0;  // 2 = I-slice, 0 = P-slice in vaapi
        slice_param.pic_order_cnt_lsb = is_keyframe ? 0 : (mFrameIndex % 16) * 2;
        slice_param.idr_pic_id = 0;

        for (int i = 0; i < 32; i++) {
            slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
            slice_param.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
            slice_param.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
        }

        if (!is_keyframe && mHasReference && mReconSurfaces[mPrevRecon] != VA_INVALID_SURFACE) {
            slice_param.num_ref_idx_active_override_flag = 1;
            slice_param.num_ref_idx_l0_active_minus1 = 0;
            slice_param.RefPicList0[0].picture_id = mReconSurfaces[mPrevRecon];
            slice_param.RefPicList0[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            slice_param.RefPicList0[0].frame_idx = (mFrameIndex - 1) % 16;
            slice_param.RefPicList0[0].TopFieldOrderCnt = ((mFrameIndex - 1) % 16) * 2;
            slice_param.RefPicList0[0].BottomFieldOrderCnt = ((mFrameIndex - 1) % 16) * 2;
        }

        VABufferID slice_buf_id = VA_INVALID_ID;
        vaCreateBuffer(mVaDisplay, mVaContextId, VAEncSliceParameterBufferType,
                       sizeof(slice_param), 1, &slice_param, &slice_buf_id);
        vaRenderPicture(mVaDisplay, mVaContextId, &slice_buf_id, 1);
        vaDestroyBuffer(mVaDisplay, slice_buf_id);
    } else if (mConfig.codec == CodecType::HEVC) {
        VAEncSliceParameterBufferHEVC slice_param{};
        slice_param.slice_segment_address = 0;
        uint32_t ctu_size = 32;
        uint32_t width_in_ctu = (mConfig.width + ctu_size - 1) / ctu_size;
        uint32_t height_in_ctu = (mConfig.height + ctu_size - 1) / ctu_size;
        slice_param.num_ctu_in_slice = width_in_ctu * height_in_ctu;
        slice_param.slice_type = is_keyframe ? 2 : 1;  // 2 = I slice, 1 = P slice in hevc
        slice_param.max_num_merge_cand = 5;
        slice_param.slice_fields.bits.last_slice_of_pic_flag = 1;
        slice_param.slice_fields.bits.slice_sao_luma_flag = 1;
        slice_param.slice_fields.bits.slice_sao_chroma_flag = 1;

        for (int i = 0; i < 15; i++) {
            slice_param.ref_pic_list0[i].picture_id = VA_INVALID_SURFACE;
            slice_param.ref_pic_list0[i].flags = VA_PICTURE_HEVC_INVALID;
            slice_param.ref_pic_list1[i].picture_id = VA_INVALID_SURFACE;
            slice_param.ref_pic_list1[i].flags = VA_PICTURE_HEVC_INVALID;
        }

        if (!is_keyframe && mHasReference && mReconSurfaces[mPrevRecon] != VA_INVALID_SURFACE) {
            slice_param.num_ref_idx_l0_active_minus1 = 0;
            slice_param.ref_pic_list0[0].picture_id = mReconSurfaces[mPrevRecon];
            slice_param.ref_pic_list0[0].flags = 0;
            slice_param.ref_pic_list0[0].pic_order_cnt = mFrameIndex - 1;
        }

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

    VASurfaceID enc_surface = input_surface;
    if (mVppContextId != VA_INVALID_ID && mNv12Surface != VA_INVALID_SURFACE) {
        enc_surface = ConvertRgbToNv12(input_surface);
        if (enc_surface == VA_INVALID_SURFACE) {
            ALOGE("Failed to convert RGB surface to NV12 for encoding");
            return false;
        }
    }

    bool is_keyframe = (mFrameIndex % mConfig.intra_period == 0) || force_keyframe || mForceKeyFrame;
    mForceKeyFrame = false;
    if (is_keyframe) {
        mFrameIndex = 0;
    }

    if (is_keyframe) {
        RenderSequenceParameters();
    }

    VAStatus status = vaBeginPicture(mVaDisplay, mVaContextId, enc_surface);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaBeginPicture failed: %s", vaErrorStr(status));
        return false;
    }

    RenderPictureParameters(enc_surface, is_keyframe);
    RenderSliceParameters(enc_surface, is_keyframe);

    status = vaEndPicture(mVaDisplay, mVaContextId);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaEndPicture failed: %s", vaErrorStr(status));
        return false;
    }

    VASurfaceID sync_target = (mReconSurfaces[mCurrentRecon] != VA_INVALID_SURFACE)
                              ? mReconSurfaces[mCurrentRecon] : enc_surface;
    status = vaSyncSurface(mVaDisplay, sync_target);
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
    if (is_keyframe && mConfig.codec == CodecType::H264) {
        std::vector<uint8_t> headers;
        GenerateH264Headers(mConfig.width, mConfig.height, headers);
        if (copied + headers.size() <= out_max_size) {
            memcpy(reinterpret_cast<uint8_t *>(out_linear_buf) + copied, headers.data(), headers.size());
            copied += headers.size();
        }
    }

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

    mHasReference = true;
    std::swap(mCurrentRecon, mPrevRecon);
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
    mConfig.max_bit_rate = new_bitrate * 3 / 2;
    return SetupRateControl();
}

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

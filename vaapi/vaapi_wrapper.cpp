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
#include <sys/stat.h>
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
        writeBit(1); // rbsp_stop_one_bit
        while (mBitIndex != 0) {
            writeBit(0); // rbsp_alignment_zero_bit
        }
        std::vector<uint8_t> rbsp;
        int zeroCount = 0;
        for (uint8_t b : mData) {
            if (zeroCount == 2 && b <= 3) {
                rbsp.push_back(3); // emulation_prevention_three_byte
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
    // 1. Generate SPS
    BitWriter spsBw;
    spsBw.writeBits(77, 8);   // profile_idc = 77 (Main profile)
    spsBw.writeBits(0x40, 8); // constraint_set1 = 1
    spsBw.writeBits(41, 8);   // level_idc = 41 (Level 4.1)
    spsBw.writeUe(0);         // seq_parameter_set_id = 0
    spsBw.writeUe(0);         // log2_max_frame_num_minus4 = 0 (max frame_num 16)
    spsBw.writeUe(0);         // pic_order_cnt_type = 0
    spsBw.writeUe(0);         // log2_max_pic_order_cnt_lsb_minus4 = 0
    spsBw.writeUe(1);         // max_num_ref_frames = 1
    spsBw.writeBit(0);        // gaps_in_frame_num_value_allowed_flag = 0

    uint32_t pic_width_in_mbs_minus1 = (width + 15) / 16 - 1;
    uint32_t pic_height_in_map_units_minus1 = (height + 15) / 16 - 1;
    spsBw.writeUe(pic_width_in_mbs_minus1);
    spsBw.writeUe(pic_height_in_map_units_minus1);
    spsBw.writeBit(1);        // frame_mbs_only_flag = 1
    spsBw.writeBit(1);        // direct_8x8_inference_flag = 1

    uint32_t crop_right = ((pic_width_in_mbs_minus1 + 1) * 16 - width) / 2;
    uint32_t crop_bottom = ((pic_height_in_map_units_minus1 + 1) * 16 - height) / 2;
    if (crop_right > 0 || crop_bottom > 0) {
        spsBw.writeBit(1);    // frame_cropping_flag
        spsBw.writeUe(0);     // left
        spsBw.writeUe(crop_right);
        spsBw.writeUe(0);     // top
        spsBw.writeUe(crop_bottom);
    } else {
        spsBw.writeBit(0);
    }

    // VUI parameters for Rec.709 color space and proper display
    spsBw.writeBit(1);        // vui_parameters_present_flag = 1
    spsBw.writeBit(0);        // aspect_ratio_info_present_flag = 0
    spsBw.writeBit(0);        // overscan_info_present_flag = 0
    spsBw.writeBit(1);        // video_signal_type_present_flag = 1
    spsBw.writeBits(5, 3);    // video_format = 5 (Unspecified)
    spsBw.writeBit(0);        // video_full_range_flag = 0 (Limited range)
    spsBw.writeBit(1);        // colour_description_present_flag = 1
    spsBw.writeBits(1, 8);    // colour_primaries = 1 (BT.709)
    spsBw.writeBits(1, 8);    // transfer_characteristics = 1 (BT.709)
    spsBw.writeBits(1, 8);    // matrix_coefficients = 1 (BT.709)
    spsBw.writeBit(0);        // chroma_loc_info_present_flag = 0
    spsBw.writeBit(0);        // timing_info_present_flag = 0
    spsBw.writeBit(0);        // nal_hrd_parameters_present_flag = 0
    spsBw.writeBit(0);        // vcl_hrd_parameters_present_flag = 0
    spsBw.writeBit(0);        // pic_struct_present_flag = 0
    spsBw.writeBit(0);        // bitstream_restriction_flag = 0

    std::vector<uint8_t> spsRbsp = spsBw.flushRbsp();

    // 2. Generate PPS
    BitWriter ppsBw;
    ppsBw.writeUe(0);         // pic_parameter_set_id = 0
    ppsBw.writeUe(0);         // seq_parameter_set_id = 0
    ppsBw.writeBit(1);        // entropy_coding_mode_flag = 1 (CABAC)
    ppsBw.writeBit(0);        // bottom_field_pic_order_in_frame_present_flag = 0
    ppsBw.writeUe(0);         // num_slice_groups_minus1 = 0
    ppsBw.writeUe(0);         // num_ref_idx_l0_default_active_minus1 = 0
    ppsBw.writeUe(0);         // num_ref_idx_l1_default_active_minus1 = 0
    ppsBw.writeBit(0);        // weighted_pred_flag = 0
    ppsBw.writeBits(0, 2);    // weighted_bipred_idc = 0
    ppsBw.writeSe(0);         // pic_init_qp_minus26 = 0
    ppsBw.writeSe(0);         // pic_init_qs_minus26 = 0
    ppsBw.writeSe(0);         // chroma_qp_index_offset = 0
    ppsBw.writeBit(0);        // deblocking_filter_control_present_flag = 0 (Matches Intel hardware driver!)
    ppsBw.writeBit(0);        // constrained_intra_pred_flag = 0
    ppsBw.writeBit(0);        // redundant_pic_cnt_present_flag = 0
    std::vector<uint8_t> ppsRbsp = ppsBw.flushRbsp();

    // Assemble Annex-B NAL units with 4-byte start codes
    const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};
    out.clear();
    out.insert(out.end(), startCode, startCode + 4);
    out.push_back(0x67); // NAL_SPS
    out.insert(out.end(), spsRbsp.begin(), spsRbsp.end());

    out.insert(out.end(), startCode, startCode + 4);
    out.push_back(0x68); // NAL_PPS
    out.insert(out.end(), ppsRbsp.begin(), ppsRbsp.end());
}

void WriteHevcProfileTierLevel(BitWriter &bw) {
    bw.writeBits(0, 2);            // general_profile_space = 0
    bw.writeBit(0);                // general_tier_flag = 0
    bw.writeBits(1, 5);            // general_profile_idc = 1 (Main)
    bw.writeBits(0x60000000, 32);  // general_profile_compatibility_flag (Main + Main10)
    bw.writeBit(1);                // general_progressive_source_flag = 1
    bw.writeBit(0);                // general_interlaced_source_flag = 0
    bw.writeBit(1);                // general_non_packed_constraint_flag = 1
    bw.writeBit(1);                // general_frame_only_constraint_flag = 1
    bw.writeBits(0, 32);           // general_reserved_zero_44bits (top 32)
    bw.writeBits(0, 12);           // general_reserved_zero_44bits (bottom 12)
    bw.writeBits(120, 8);          // general_level_idc = 120 (Level 4.0)
}

void AppendHevcNal(std::vector<uint8_t> &out, uint8_t nal_unit_type, const std::vector<uint8_t> &rbsp) {
    const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};
    out.insert(out.end(), startCode, startCode + 4);
    out.push_back((nal_unit_type << 1) & 0x7E); // forbidden_zero_bit(0), nal_unit_type(6), nuh_layer_id_hi(0)
    out.push_back(0x01);                        // nuh_layer_id_lo(0), nuh_temporal_id_plus1(1)
    out.insert(out.end(), rbsp.begin(), rbsp.end());
}

void GenerateHevcSequenceHeaders(uint32_t width, uint32_t height, uint32_t fps, std::vector<uint8_t> &out) {
    out.clear();
    uint32_t coded_width = (width + 15) & ~15u;
    uint32_t coded_height = (height + 15) & ~15u;
    if (fps == 0) fps = 60;

    // 1. VPS (NAL 32)
    BitWriter vps;
    vps.writeBits(0, 4);      // vps_video_parameter_set_id = 0
    vps.writeBits(3, 2);      // vps_base_layer_internal_flag=1, vps_base_layer_available_flag=1
    vps.writeBits(0, 6);      // vps_max_layers_minus1 = 0
    vps.writeBits(0, 3);      // vps_max_sub_layers_minus1 = 0
    vps.writeBit(1);          // vps_temporal_id_nesting_flag = 1
    vps.writeBits(0xFFFF, 16);// vps_reserved_0xffff_16bits
    WriteHevcProfileTierLevel(vps);
    vps.writeBit(0);          // vps_sub_layer_ordering_info_present_flag = 0
    vps.writeUe(1);           // vps_max_dec_pic_buffering_minus1[0] = 1
    vps.writeUe(0);           // vps_max_num_reorder_pics[0] = 0
    vps.writeUe(0);           // vps_max_latency_increase_plus1[0] = 0
    vps.writeBits(0, 6);      // vps_max_layer_id = 0
    vps.writeUe(0);           // vps_num_layer_sets_minus1 = 0
    vps.writeBit(1);          // vps_timing_info_present_flag = 1
    vps.writeBits(1, 32);     // vps_num_units_in_tick = 1
    vps.writeBits(fps, 32);   // vps_time_scale = fps
    vps.writeBit(1);          // vps_poc_proportional_to_timing_flag = 1
    vps.writeUe(0);           // vps_num_ticks_poc_diff_one_minus1 = 0
    vps.writeUe(0);           // vps_num_hrd_parameters = 0
    vps.writeBit(0);          // vps_extension_flag = 0
    AppendHevcNal(out, 32, vps.flushRbsp());

    // 2. SPS (NAL 33)
    BitWriter sps;
    sps.writeBits(0, 4);      // sps_video_parameter_set_id = 0
    sps.writeBits(0, 3);      // sps_max_sub_layers_minus1 = 0
    sps.writeBit(1);          // sps_temporal_id_nesting_flag = 1
    WriteHevcProfileTierLevel(sps);
    sps.writeUe(0);           // sps_seq_parameter_set_id = 0
    sps.writeUe(1);           // chroma_format_idc = 1 (4:2:0)
    sps.writeUe(coded_width); // pic_width_in_luma_samples
    sps.writeUe(coded_height);// pic_height_in_luma_samples
    uint32_t crop_right = (coded_width - width) / 2;
    uint32_t crop_bottom = (coded_height - height) / 2;
    if (crop_right > 0 || crop_bottom > 0) {
        sps.writeBit(1);      // conformance_window_flag = 1
        sps.writeUe(0);
        sps.writeUe(crop_right);
        sps.writeUe(0);
        sps.writeUe(crop_bottom);
    } else {
        sps.writeBit(0);      // conformance_window_flag = 0
    }
    sps.writeUe(0);           // bit_depth_luma_minus8 = 0
    sps.writeUe(0);           // bit_depth_chroma_minus8 = 0
    sps.writeUe(8);           // log2_max_pic_order_cnt_lsb_minus4 = 8 (12-bit POC LSB)
    sps.writeBit(0);          // sps_sub_layer_ordering_info_present_flag = 0
    sps.writeUe(1);           // sps_max_dec_pic_buffering_minus1[0] = 1
    sps.writeUe(0);           // sps_max_num_reorder_pics[0] = 0
    sps.writeUe(0);           // sps_max_latency_increase_plus1[0] = 0
    sps.writeUe(0);           // log2_min_luma_coding_block_size_minus3 = 0 (8x8)
    sps.writeUe(3);           // log2_diff_max_min_luma_coding_block_size = 3 (64x64 CTU)
    sps.writeUe(0);           // log2_min_luma_transform_block_size_minus2 = 0 (4x4)
    sps.writeUe(3);           // log2_diff_max_min_luma_transform_block_size = 3 (32x32)
    sps.writeUe(2);           // max_transform_hierarchy_depth_inter = 2
    sps.writeUe(2);           // max_transform_hierarchy_depth_intra = 2
    sps.writeBit(0);          // scaling_list_enabled_flag = 0
    sps.writeBit(1);          // amp_enabled_flag = 1
    sps.writeBit(1);          // sample_adaptive_offset_enabled_flag = 1
    sps.writeBit(0);          // pcm_enabled_flag = 0
    sps.writeUe(0);           // num_short_term_ref_pic_sets = 0
    sps.writeBit(0);          // long_term_ref_pics_present_flag = 0
    sps.writeBit(1);          // sps_temporal_mvp_enabled_flag = 1
    sps.writeBit(0);          // strong_intra_smoothing_enabled_flag = 0
    sps.writeBit(1);          // vui_parameters_present_flag = 1
    sps.writeBit(0);          // aspect_ratio_info_present_flag = 0
    sps.writeBit(0);          // overscan_info_present_flag = 0
    sps.writeBit(1);          // video_signal_type_present_flag = 1
    sps.writeBits(5, 3);      // video_format = 5
    sps.writeBit(0);          // video_full_range_flag = 0
    sps.writeBit(1);          // colour_description_present_flag = 1
    sps.writeBits(1, 8);      // colour_primaries = 1 (BT.709)
    sps.writeBits(1, 8);      // transfer_characteristics = 1 (BT.709)
    sps.writeBits(1, 8);      // matrix_coefficients = 1 (BT.709)
    sps.writeBit(0);          // chroma_loc_info_present_flag = 0
    sps.writeBit(0);          // neutral_chroma_indication_flag = 0
    sps.writeBit(0);          // field_seq_flag = 0
    sps.writeBit(0);          // frame_field_info_present_flag = 0
    sps.writeBit(0);          // default_display_window_flag = 0
    sps.writeBit(1);          // vui_timing_info_present_flag = 1
    sps.writeBits(1, 32);     // vui_num_units_in_tick = 1
    sps.writeBits(fps, 32);   // vui_time_scale = fps
    sps.writeBit(1);          // vui_poc_proportional_to_timing_flag = 1
    sps.writeUe(0);           // vui_num_ticks_poc_diff_one_minus1 = 0
    sps.writeBit(0);          // vui_hrd_parameters_present_flag = 0
    sps.writeBit(0);          // bitstream_restriction_flag = 0
    sps.writeBit(0);          // sps_extension_present_flag = 0
    AppendHevcNal(out, 33, sps.flushRbsp());

    // 3. PPS (NAL 34)
    BitWriter pps;
    pps.writeUe(0);           // pps_pic_parameter_set_id = 0
    pps.writeUe(0);           // pps_seq_parameter_set_id = 0
    pps.writeBit(0);          // dependent_slice_segments_enabled_flag = 0
    pps.writeBit(0);          // output_flag_present_flag = 0
    pps.writeBits(0, 3);      // num_extra_slice_header_bits = 0
    pps.writeBit(0);          // sign_data_hiding_enabled_flag = 0
    pps.writeBit(0);          // cabac_init_present_flag = 0
    pps.writeUe(0);           // num_ref_idx_l0_default_active_minus1 = 0
    pps.writeUe(0);           // num_ref_idx_l1_default_active_minus1 = 0
    pps.writeSe(4);           // init_qp_minus26 = 4 (pic_init_qp = 30)
    pps.writeBit(0);          // constrained_intra_pred_flag = 0
    pps.writeBit(1);          // transform_skip_enabled_flag = 1
    pps.writeBit(1);          // cu_qp_delta_enabled_flag = 1
    pps.writeUe(3);           // diff_cu_qp_delta_depth = 3
    pps.writeSe(0);           // pps_cb_qp_offset = 0
    pps.writeSe(0);           // pps_cr_qp_offset = 0
    pps.writeBit(0);          // pps_slice_chroma_qp_offsets_present_flag = 0
    pps.writeBit(0);          // weighted_pred_flag = 0
    pps.writeBit(0);          // weighted_bipred_flag = 0
    pps.writeBit(0);          // transquant_bypass_enabled_flag = 0
    pps.writeBit(0);          // tiles_enabled_flag = 0
    pps.writeBit(0);          // entropy_coding_sync_enabled_flag = 0
    pps.writeBit(1);          // pps_loop_filter_across_slices_enabled_flag = 1
    pps.writeBit(0);          // deblocking_filter_control_present_flag = 0
    pps.writeBit(0);          // pps_scaling_list_data_present_flag = 0
    pps.writeBit(0);          // lists_modification_present_flag = 0
    pps.writeUe(0);           // log2_parallel_merge_level_minus2 = 0
    pps.writeBit(0);          // slice_segment_header_extension_present_flag = 0
    pps.writeBit(0);          // pps_extension_present_flag = 0
    AppendHevcNal(out, 34, pps.flushRbsp());
}

uint32_t ComputeTargetQp(uint32_t bit_rate, uint32_t width, uint32_t height, uint32_t fps) {
    double pixels_per_sec = static_cast<double>(width) * static_cast<double>(height) *
                            static_cast<double>(fps > 0 ? fps : 60);
    if (pixels_per_sec <= 0.0) return 26;
    double bpp = static_cast<double>(bit_rate) / pixels_per_sec;
    if (bpp >= 0.25) return 22;
    if (bpp >= 0.14) return 25;
    if (bpp >= 0.07) return 28;
    if (bpp >= 0.035) return 31;
    return 34;
}

void GenerateHevcSliceHeader(bool is_keyframe, uint32_t poc, int32_t slice_qp_delta, std::vector<uint8_t> &out) {
    out.clear();
    BitWriter sw;
    sw.writeBit(1);           // first_slice_segment_in_pic_flag = 1
    if (is_keyframe) {
        sw.writeBit(0);       // no_output_of_prior_pics_flag = 0
    }
    sw.writeUe(0);            // slice_pic_parameter_set_id = 0
    if (is_keyframe) {
        sw.writeUe(2);        // slice_type = 2 (I)
        sw.writeBit(1);       // slice_sao_luma_flag = 1
        sw.writeBit(1);       // slice_sao_chroma_flag = 1
        sw.writeSe(slice_qp_delta); // slice_qp_delta
        sw.writeBit(0);       // slice_loop_filter_across_slices_enabled_flag = 0
        AppendHevcNal(out, 19, sw.flushRbsp()); // IDR_W_RADL = 19
    } else {
        sw.writeUe(0);        // slice_type = 0 (GPB B-slice)
        sw.writeBits(poc & 0xFFF, 12); // slice_pic_order_cnt_lsb (12 bits)
        sw.writeBit(0);       // short_term_ref_pic_set_sps_flag = 0
        // st_ref_pic_set(0): 1 negative pic with delta_poc = -1
        sw.writeUe(1);        // num_negative_pics = 1
        sw.writeUe(0);        // num_positive_pics = 0
        sw.writeUe(0);        // delta_poc_s0_minus1[0] = 0
        sw.writeBit(1);       // used_by_curr_pic_s0_flag[0] = 1
        sw.writeBit(1);       // slice_temporal_mvp_enabled_flag = 1
        sw.writeBit(1);       // slice_sao_luma_flag = 1
        sw.writeBit(1);       // slice_sao_chroma_flag = 1
        sw.writeBit(0);       // num_ref_idx_active_override_flag = 0
        sw.writeBit(0);       // mvd_l1_zero_flag = 0
        sw.writeBit(1);       // collocated_from_l0_flag = 1
        sw.writeUe(0);        // five_minus_max_num_merge_cand = 0
        sw.writeSe(slice_qp_delta); // slice_qp_delta
        sw.writeBit(0);       // slice_loop_filter_across_slices_enabled_flag = 0
        AppendHevcNal(out, 1, sw.flushRbsp());  // TRAIL_R = 1
    }
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
    if (!InitVpp()) {
        return false;
    }
    if (mConfig.codec == CodecType::HEVC) {
        GenerateHevcSequenceHeaders(mConfig.width, mConfig.height, mConfig.frame_rate, mCachedSeqHeaders);
    } else {
        GenerateH264Headers(mConfig.width, mConfig.height, mCachedSeqHeaders);
    }
    return true;
}

void VaapiWrapper::Deinitialize() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mVaDisplay) {
        for (size_t i = 0; i < kMaxCachedSurfaces; ++i) {
            if (mSurfaceCache[i].surface != VA_INVALID_SURFACE) {
                vaDestroySurfaces(mVaDisplay, &mSurfaceCache[i].surface, 1);
                mSurfaceCache[i] = SurfaceCacheEntry{};
            }
        }
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

    VASurfaceAttrib attr{};
    attr.type = VASurfaceAttribPixelFormat;
    attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attr.value.type = VAGenericValueTypeInteger;
    attr.value.value.i = VA_FOURCC_NV12;

    VAStatus status = vaCreateSurfaces(mVaDisplay, VA_RT_FORMAT_YUV420,
                                       mConfig.width, mConfig.height,
                                       mReconSurfaces, 2, &attr, 1);
    if (status != VA_STATUS_SUCCESS) {
        ALOGW("vaCreateSurfaces for recon DPB surfaces failed: %s", vaErrorStr(status));
    }

    if (mDirectRgbInput) {
        ALOGI("Intel VDEnc direct RGB32 SFC enabled: bypassing VPP pass (Recon [%d, %d])",
              mReconSurfaces[0], mReconSurfaces[1]);
        return true;
    }

    status = vaCreateConfig(mVaDisplay, VAProfileNone, VAEntrypointVideoProc,
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

    status = vaCreateSurfaces(mVaDisplay, VA_RT_FORMAT_YUV420,
                              mConfig.width, mConfig.height,
                              &mNv12Surface, 1, &attr, 1);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateSurfaces for NV12 VPP surface failed: %s", vaErrorStr(status));
        return false;
    }

    ALOGI("VA-API VEBOX VPP & DPB initialized: NV12 %d, Recon [%d, %d]",
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
    // Do not call vaSyncSurface here: let the GPU DRM scheduler pipeline VPP -> Encode directly
    return mNv12Surface;
}

bool VaapiWrapper::CreateEncoderContext() {
    VAProfile profile = (mConfig.codec == CodecType::HEVC) ? VAProfileHEVCMain : VAProfileH264Main;
    VAEntrypoint entrypoint = VAEntrypointEncSlice;

    mDirectRgbInput = false;
    mUsingCqp = false;
    mActiveQp = ComputeTargetQp(mConfig.bit_rate, mConfig.width, mConfig.height, mConfig.frame_rate);

    uint32_t rt_format = VA_RT_FORMAT_YUV420;
    uint32_t rc_mode = (mConfig.rc_mode == RateControlMode::CBR) ? VA_RC_CBR : VA_RC_VBR;

    // Probe Intel Low-Power VDEnc (VAEntrypointEncSliceLP) to keep 3D Render engine at 0%
    VAConfigAttrib lp_attribs[2]{};
    lp_attribs[0].type = VAConfigAttribRTFormat;
    lp_attribs[1].type = VAConfigAttribRateControl;
    if (vaGetConfigAttributes(mVaDisplay, profile, VAEntrypointEncSliceLP, lp_attribs, 2) == VA_STATUS_SUCCESS &&
        lp_attribs[0].value != VA_ATTRIB_NOT_SUPPORTED &&
        lp_attribs[1].value != VA_ATTRIB_NOT_SUPPORTED) {
        entrypoint = VAEntrypointEncSliceLP;
        if (lp_attribs[0].value & VA_RT_FORMAT_RGB32) {
            rt_format = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
            mDirectRgbInput = true;
        }
        if (lp_attribs[1].value & rc_mode) {
            // HuC firmware loaded: CBR/VBR supported on VDEnc
        } else if (lp_attribs[1].value & VA_RC_CBR) {
            rc_mode = VA_RC_CBR;
        } else if (lp_attribs[1].value & VA_RC_VBR) {
            rc_mode = VA_RC_VBR;
        } else if (lp_attribs[1].value & VA_RC_CQP) {
            rc_mode = VA_RC_CQP;
            mUsingCqp = true;
        }
    }

    VAConfigAttrib attribs[3]{};
    int num_attribs = 2;
    attribs[0].type = VAConfigAttribRTFormat;
    attribs[0].value = rt_format;

    attribs[1].type = VAConfigAttribRateControl;
    attribs[1].value = rc_mode;

    if (mConfig.codec == CodecType::HEVC) {
        attribs[2].type = VAConfigAttribEncPackedHeaders;
        attribs[2].value = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_SLICE;
        num_attribs = 3;
    }

    VAStatus status = vaCreateConfig(mVaDisplay, profile, entrypoint, attribs, num_attribs, &mVaConfigId);
    if (status != VA_STATUS_SUCCESS && entrypoint == VAEntrypointEncSliceLP) {
        ALOGW("VAEntrypointEncSliceLP config failed (%s), falling back to VAEntrypointEncSlice", vaErrorStr(status));
        entrypoint = VAEntrypointEncSlice;
        mDirectRgbInput = false;
        mUsingCqp = false;
        attribs[0].value = VA_RT_FORMAT_YUV420;
        attribs[1].value = (mConfig.rc_mode == RateControlMode::CBR) ? VA_RC_CBR : VA_RC_VBR;
        status = vaCreateConfig(mVaDisplay, profile, entrypoint, attribs, num_attribs, &mVaConfigId);
    }
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

    mBitrateNeedsUpdate = true;
    ALOGI("Intel VA-API encoder context ready (ep=%d, direct_rgb=%d, cqp=%d, qp=%u): %ux%u @ %ufps, %u bps",
          entrypoint, mDirectRgbInput, mUsingCqp, mActiveQp,
          mConfig.width, mConfig.height, mConfig.frame_rate, mConfig.bit_rate);
    return true;
}

bool VaapiWrapper::SetupRateControl() {
    // 0. Target Usage / Quality Level = 7 (Fastest / Low-Latency VDEnc streaming preset)
    uint8_t qbuf[sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterBufferQualityLevel)]{};
    auto *misc_q = reinterpret_cast<VAEncMiscParameterBuffer *>(qbuf);
    misc_q->type = VAEncMiscParameterTypeQualityLevel;
    auto *q_param = reinterpret_cast<VAEncMiscParameterBufferQualityLevel *>(misc_q->data);
    q_param->quality_level = 7;
    VABufferID q_buf_id = VA_INVALID_ID;
    if (vaCreateBuffer(mVaDisplay, mVaContextId, VAEncMiscParameterBufferType,
                       sizeof(qbuf), 1, qbuf, &q_buf_id) == VA_STATUS_SUCCESS) {
        vaRenderPicture(mVaDisplay, mVaContextId, &q_buf_id, 1);
        vaDestroyBuffer(mVaDisplay, q_buf_id);
    }

    if (mUsingCqp) {
        return true;
    }

    // 1. Rate control parameter
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

    // 2. HRD (Hypothetical Reference Decoder) parameter - prevents buffer overflow/underrun QP spikes
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

    // 3. Frame rate parameter
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
            uint32_t fps = mConfig.frame_rate > 0 ? mConfig.frame_rate : 60;
            fps_param->framerate = (1u << 16) | (fps & 0xFFFF);
            vaUnmapBuffer(mVaDisplay, fps_buf_id);
            vaRenderPicture(mVaDisplay, mVaContextId, &fps_buf_id, 1);
        }
        vaDestroyBuffer(mVaDisplay, fps_buf_id);
    }

    return true;
}

bool VaapiWrapper::IsSurfaceRgb(VASurfaceID surface_id) const {
    for (size_t i = 0; i < kMaxCachedSurfaces; ++i) {
        if (mSurfaceCache[i].surface == surface_id) {
            return mSurfaceCache[i].is_rgb;
        }
    }
    if (surface_id == mLastUncachedSurface) {
        return mLastUncachedIsRgb;
    }
    return true;
}

VASurfaceID VaapiWrapper::ImportDrmPrimeSurface(const DrmPrimeBuffer &buf) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mVaDisplay || buf.prime_fd < 0) {
        ALOGE("Invalid VA display or prime_fd");
        return VA_INVALID_SURFACE;
    }

    bool is_rgb = (buf.fourcc == DRM_FORMAT_ABGR8888 ||
                   buf.fourcc == DRM_FORMAT_XBGR8888 ||
                   buf.fourcc == DRM_FORMAT_ARGB8888 ||
                   buf.fourcc == DRM_FORMAT_BGR888 ||
                   buf.fourcc == DRM_FORMAT_RGB565);

    struct stat st{};
    bool has_stat = (fstat(buf.prime_fd, &st) == 0 && st.st_ino != 0);
    if (has_stat) {
        for (size_t i = 0; i < kMaxCachedSurfaces; ++i) {
            auto &entry = mSurfaceCache[i];
            if (entry.surface != VA_INVALID_SURFACE &&
                entry.ino == st.st_ino &&
                entry.dev == st.st_dev &&
                entry.width == buf.width &&
                entry.height == buf.height &&
                entry.stride == buf.stride &&
                entry.fourcc == buf.fourcc) {
                entry.last_used = ++mCacheTick;
                return entry.surface;
            }
        }
    }

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

    if (has_stat) {
        size_t slot = 0;
        uint64_t oldest_tick = UINT64_MAX;
        for (size_t i = 0; i < kMaxCachedSurfaces; ++i) {
            if (mSurfaceCache[i].surface == VA_INVALID_SURFACE) {
                slot = i;
                oldest_tick = 0;
                break;
            }
            if (mSurfaceCache[i].last_used < oldest_tick) {
                oldest_tick = mSurfaceCache[i].last_used;
                slot = i;
            }
        }
        if (mSurfaceCache[slot].surface != VA_INVALID_SURFACE) {
            vaDestroySurfaces(mVaDisplay, &mSurfaceCache[slot].surface, 1);
        }
        mSurfaceCache[slot].dev = st.st_dev;
        mSurfaceCache[slot].ino = st.st_ino;
        mSurfaceCache[slot].width = buf.width;
        mSurfaceCache[slot].height = buf.height;
        mSurfaceCache[slot].stride = buf.stride;
        mSurfaceCache[slot].fourcc = buf.fourcc;
        mSurfaceCache[slot].surface = imported_surface;
        mSurfaceCache[slot].is_rgb = is_rgb;
        mSurfaceCache[slot].last_used = ++mCacheTick;
    } else {
        mLastUncachedSurface = imported_surface;
        mLastUncachedIsRgb = is_rgb;
    }

    return imported_surface;
}

void VaapiWrapper::DestroySurface(VASurfaceID surface_id) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mVaDisplay || surface_id == VA_INVALID_SURFACE) {
        return;
    }
    if (surface_id == mNv12Surface ||
        surface_id == mReconSurfaces[0] ||
        surface_id == mReconSurfaces[1]) {
        return;
    }
    for (size_t i = 0; i < kMaxCachedSurfaces; ++i) {
        if (mSurfaceCache[i].surface == surface_id) {
            return; // Keep cached BufferQueue surface alive across frames
        }
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
        seq_param.seq_fields.bits.chroma_format_idc = 1; // 4:2:0 YUV
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
        seq_param.general_profile_idc = 1; // Profile Main
        seq_param.general_level_idc = 120; // Level 4.0
        seq_param.general_tier_flag = 0;
        seq_param.intra_period = mConfig.intra_period;
        seq_param.intra_idr_period = mConfig.intra_period;
        seq_param.ip_period = mConfig.ip_period;
        seq_param.bits_per_second = mConfig.bit_rate;
        seq_param.pic_width_in_luma_samples = (mConfig.width + 15) & ~15u;
        seq_param.pic_height_in_luma_samples = (mConfig.height + 15) & ~15u;
        seq_param.seq_fields.bits.chroma_format_idc = 1; // 4:2:0
        seq_param.seq_fields.bits.separate_colour_plane_flag = 0;
        seq_param.seq_fields.bits.bit_depth_luma_minus8 = 0;
        seq_param.seq_fields.bits.bit_depth_chroma_minus8 = 0;
        seq_param.seq_fields.bits.scaling_list_enabled_flag = 0;
        seq_param.seq_fields.bits.strong_intra_smoothing_enabled_flag = 0;
        seq_param.seq_fields.bits.amp_enabled_flag = 1;
        seq_param.seq_fields.bits.sample_adaptive_offset_enabled_flag = 1;
        seq_param.seq_fields.bits.pcm_enabled_flag = 0;
        seq_param.seq_fields.bits.pcm_loop_filter_disabled_flag = 0;
        seq_param.seq_fields.bits.sps_temporal_mvp_enabled_flag = 1;
        seq_param.seq_fields.bits.low_delay_seq = 0;
        seq_param.seq_fields.bits.hierachical_flag = 0;
        seq_param.log2_min_luma_coding_block_size_minus3 = 0; // min CU 8x8
        seq_param.log2_diff_max_min_luma_coding_block_size = 3; // max CU 64x64 (CTU size 64)
        seq_param.log2_min_transform_block_size_minus2 = 0; // min TU 4x4
        seq_param.log2_diff_max_min_transform_block_size = 3; // max TU 32x32
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
        pic_param.pic_fields.bits.entropy_coding_mode_flag = 1; // CABAC
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

        for (int i = 0; i < 15; i++) {
            pic_param.reference_frames[i].picture_id = VA_INVALID_SURFACE;
            pic_param.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
        }

        if (!is_keyframe && mHasReference && mReconSurfaces[mPrevRecon] != VA_INVALID_SURFACE) {
            pic_param.reference_frames[0].picture_id = mReconSurfaces[mPrevRecon];
            pic_param.reference_frames[0].pic_order_cnt = mFrameIndex - 1;
            pic_param.reference_frames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
        }

        pic_param.coded_buf = mCodedBuf;
        pic_param.collocated_ref_pic_index = 0;
        pic_param.last_picture = 0;
        pic_param.pic_init_qp = 30;
        pic_param.diff_cu_qp_delta_depth = 3;
        pic_param.nal_unit_type = is_keyframe ? 19 : 1;
        pic_param.pic_fields.bits.idr_pic_flag = is_keyframe ? 1 : 0;
        pic_param.pic_fields.bits.coding_type = is_keyframe ? 1 : 2;
        pic_param.pic_fields.bits.reference_pic_flag = 1;
        pic_param.pic_fields.bits.transform_skip_enabled_flag = 1;
        pic_param.pic_fields.bits.cu_qp_delta_enabled_flag = 1;
        pic_param.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;

        VABufferID pic_buf_id = VA_INVALID_ID;
        vaCreateBuffer(mVaDisplay, mVaContextId, VAEncPictureParameterBufferType,
                       sizeof(pic_param), 1, &pic_param, &pic_buf_id);
        vaRenderPicture(mVaDisplay, mVaContextId, &pic_buf_id, 1);
        vaDestroyBuffer(mVaDisplay, pic_buf_id);
    }
    return true;
}

bool VaapiWrapper::RenderPackedHeaders(bool is_keyframe) {
    if (mConfig.codec != CodecType::HEVC) {
        return true;
    }

    if (is_keyframe && !mCachedSeqHeaders.empty()) {
        VAEncPackedHeaderParameterBuffer ph{};
        ph.type = VAEncPackedHeaderSequence;
        ph.bit_length = mCachedSeqHeaders.size() * 8;
        ph.has_emulation_bytes = 1;

        VABufferID ph_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
        vaCreateBuffer(mVaDisplay, mVaContextId, VAEncPackedHeaderParameterBufferType,
                       sizeof(ph), 1, &ph, &ph_bufs[0]);
        vaCreateBuffer(mVaDisplay, mVaContextId, VAEncPackedHeaderDataBufferType,
                       mCachedSeqHeaders.size(), 1, mCachedSeqHeaders.data(), &ph_bufs[1]);
        vaRenderPicture(mVaDisplay, mVaContextId, ph_bufs, 2);
        vaDestroyBuffer(mVaDisplay, ph_bufs[0]);
        vaDestroyBuffer(mVaDisplay, ph_bufs[1]);
    }

    std::vector<uint8_t> slice_hdr;
    int32_t qp_delta = mUsingCqp ? (static_cast<int32_t>(mActiveQp) - 30) : 0;
    GenerateHevcSliceHeader(is_keyframe, mFrameIndex, qp_delta, slice_hdr);

    VAEncPackedHeaderParameterBuffer sh{};
    sh.type = VAEncPackedHeaderSlice;
    sh.bit_length = slice_hdr.size() * 8;
    sh.has_emulation_bytes = 1;

    VABufferID sh_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
    vaCreateBuffer(mVaDisplay, mVaContextId, VAEncPackedHeaderParameterBufferType,
                   sizeof(sh), 1, &sh, &sh_bufs[0]);
    vaCreateBuffer(mVaDisplay, mVaContextId, VAEncPackedHeaderDataBufferType,
                   slice_hdr.size(), 1, slice_hdr.data(), &sh_bufs[1]);
    vaRenderPicture(mVaDisplay, mVaContextId, sh_bufs, 2);
    vaDestroyBuffer(mVaDisplay, sh_bufs[0]);
    vaDestroyBuffer(mVaDisplay, sh_bufs[1]);

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
        if (mUsingCqp) {
            slice_param.slice_qp_delta = static_cast<int8_t>(static_cast<int32_t>(mActiveQp) - 26);
        }

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
        uint32_t ctu_size = 64;
        uint32_t coded_width = (mConfig.width + 15) & ~15u;
        uint32_t coded_height = (mConfig.height + 15) & ~15u;
        uint32_t width_in_ctu = (coded_width + ctu_size - 1) / ctu_size;
        uint32_t height_in_ctu = (coded_height + ctu_size - 1) / ctu_size;
        slice_param.num_ctu_in_slice = width_in_ctu * height_in_ctu;
        slice_param.slice_type = is_keyframe ? 2 : 0; // 2: I-slice, 0: GPB B-slice (required by Intel iHD low-delay HEVC)
        slice_param.slice_pic_parameter_set_id = 0;
        slice_param.max_num_merge_cand = 5;
        if (mUsingCqp) {
            slice_param.slice_qp_delta = static_cast<int8_t>(static_cast<int32_t>(mActiveQp) - 30);
        }
        slice_param.slice_fields.bits.last_slice_of_pic_flag = 1;
        slice_param.slice_fields.bits.slice_temporal_mvp_enabled_flag = is_keyframe ? 0 : 1;
        slice_param.slice_fields.bits.slice_sao_luma_flag = 1;
        slice_param.slice_fields.bits.slice_sao_chroma_flag = 1;
        slice_param.slice_fields.bits.collocated_from_l0_flag = is_keyframe ? 0 : 1;

        for (int i = 0; i < 15; i++) {
            slice_param.ref_pic_list0[i].picture_id = VA_INVALID_SURFACE;
            slice_param.ref_pic_list0[i].flags = VA_PICTURE_HEVC_INVALID;
            slice_param.ref_pic_list1[i].picture_id = VA_INVALID_SURFACE;
            slice_param.ref_pic_list1[i].flags = VA_PICTURE_HEVC_INVALID;
        }

        if (!is_keyframe && mHasReference && mReconSurfaces[mPrevRecon] != VA_INVALID_SURFACE) {
            slice_param.num_ref_idx_l0_active_minus1 = 0;
            slice_param.num_ref_idx_l1_active_minus1 = 0;
            slice_param.ref_pic_list0[0].picture_id = mReconSurfaces[mPrevRecon];
            slice_param.ref_pic_list0[0].pic_order_cnt = mFrameIndex - 1;
            slice_param.ref_pic_list0[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
            slice_param.ref_pic_list1[0].picture_id = mReconSurfaces[mPrevRecon];
            slice_param.ref_pic_list1[0].pic_order_cnt = mFrameIndex - 1;
            slice_param.ref_pic_list1[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
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
    if (!out_linear_buf) {
        return false;
    }
    return EncodeFrame(
        input_surface, force_keyframe,
        [out_linear_buf, out_max_size](size_t exact_size) -> void * {
            return (exact_size <= out_max_size) ? out_linear_buf : nullptr;
        },
        out_actual_size, out_is_keyframe);
}

bool VaapiWrapper::EncodeFrame(VASurfaceID input_surface,
                               bool force_keyframe,
                               const OutputBufferAllocator &allocator,
                               size_t *out_actual_size,
                               bool *out_is_keyframe) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mVaDisplay || input_surface == VA_INVALID_SURFACE || !allocator) {
        return false;
    }

    VASurfaceID enc_surface = input_surface;
    if (!mDirectRgbInput && IsSurfaceRgb(input_surface) && mVppContextId != VA_INVALID_ID && mNv12Surface != VA_INVALID_SURFACE) {
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

    VAStatus status = vaBeginPicture(mVaDisplay, mVaContextId, enc_surface);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaBeginPicture failed: %s", vaErrorStr(status));
        return false;
    }

    if (is_keyframe) {
        RenderSequenceParameters();
    }
    if (is_keyframe || mBitrateNeedsUpdate) {
        SetupRateControl();
        mBitrateNeedsUpdate = false;
    }

    RenderPictureParameters(enc_surface, is_keyframe);
    RenderPackedHeaders(is_keyframe);
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

    size_t total_size = 0;
    if (is_keyframe && mConfig.codec == CodecType::H264) {
        total_size += mCachedSeqHeaders.size();
    }
    for (VACodedBufferSegment *curr = buf_segment; curr != nullptr;
         curr = reinterpret_cast<VACodedBufferSegment *>(curr->next)) {
        total_size += curr->size;
    }

    if (total_size == 0) {
        vaUnmapBuffer(mVaDisplay, mCodedBuf);
        *out_actual_size = 0;
        return false;
    }

    uint8_t *dst = reinterpret_cast<uint8_t *>(allocator(total_size));
    if (!dst) {
        ALOGE("Output buffer allocator failed for %zu bytes", total_size);
        vaUnmapBuffer(mVaDisplay, mCodedBuf);
        *out_actual_size = 0;
        return false;
    }

    size_t copied = 0;
    if (is_keyframe && mConfig.codec == CodecType::H264 && !mCachedSeqHeaders.empty()) {
        memcpy(dst + copied, mCachedSeqHeaders.data(), mCachedSeqHeaders.size());
        copied += mCachedSeqHeaders.size();
    }

    for (VACodedBufferSegment *curr = buf_segment; curr != nullptr;
         curr = reinterpret_cast<VACodedBufferSegment *>(curr->next)) {
        memcpy(dst + copied, curr->buf, curr->size);
        copied += curr->size;
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
    mActiveQp = ComputeTargetQp(new_bitrate, mConfig.width, mConfig.height, mConfig.frame_rate);
    mBitrateNeedsUpdate = true;
    return true;
}

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

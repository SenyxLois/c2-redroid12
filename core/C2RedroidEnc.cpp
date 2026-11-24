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

#define LOG_TAG "C2RedroidEnc"

#include "C2RedroidEnc.h"
#include <C2PlatformSupport.h>
#include <C2BufferPriv.h>
#include <log/log.h>
#include <media/stagefright/foundation/ADebug.h>
#include <cutils/properties.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace android {
namespace hardware {
namespace media {
namespace c2 {
namespace redroid {

std::atomic<uint64_t> gRedroidEncConfigSeq{1};

namespace {

inline uint64_t NowMonoUs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

inline uint64_t NowWallMs() {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

void CountSelfFdsFast(uint32_t *io_total_fds, uint32_t *io_dmabuf_fds, bool force_rescan) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        return;
    }
    int dfd = dirfd(dir);
    uint32_t total = 0;
    struct dirent *ent = nullptr;
    if (!force_rescan && *io_total_fds > 0) {
        // Fast pass: count directory entries without ~100 readlinkat syscalls if unchanged
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] != '.') {
                total++;
            }
        }
        if (total == *io_total_fds) {
            closedir(dir);
            return;
        }
        rewinddir(dir);
        total = 0;
    }

    uint32_t dmabuf = 0;
    char target[64];
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        total++;
        ssize_t len = readlinkat(dfd, ent->d_name, target, sizeof(target) - 1);
        if (len >= 6) {
            target[len] = '\0';
            if (strncmp(target, "/dmabuf", 7) == 0 || strncmp(target, "dmabuf", 6) == 0) {
                dmabuf++;
            }
        }
    }
    closedir(dir);
    *io_total_fds = total;
    *io_dmabuf_fds = dmabuf;
}

} // anonymous namespace

C2RedroidEnc::C2RedroidEnc(const std::shared_ptr<C2ComponentInterface> &intf)
    : SimpleC2Component(intf),
      mIntf(intf) {
}

C2RedroidEnc::~C2RedroidEnc() {
    onRelease();
}

c2_status_t C2RedroidEnc::onInit() {
    return C2_OK;
}

c2_status_t C2RedroidEnc::onStop() {
    releaseEncoder();
    mStarted = false;
    return C2_OK;
}

void C2RedroidEnc::onReset() {
    releaseEncoder();
    mStarted = false;
}

void C2RedroidEnc::onRelease() {
    releaseEncoder();
    mStarted = false;
}

c2_status_t C2RedroidEnc::onFlush_sm() {
    return C2_OK;
}

bool C2RedroidEnc::initEncoder() {
    if (mVaapi) {
        return true;
    }

    // Query configured bitrate and sync interval once on init
    C2StreamBitrateInfo::output bitrate(0u);
    C2StreamSyncFrameIntervalTuning::output syncInterval(0u);
    if (mIntf && mIntf->query_vb({ &bitrate, &syncInterval }, {}, C2_MAY_BLOCK, nullptr) == C2_OK) {
        if (bitrate.value > 0) {
            mBitrate = bitrate.value;
        }
        if (syncInterval.value > 0) {
            uint32_t period = (uint32_t)((syncInterval.value * mFrameRate) / 1000000LL);
            if (period > 0) {
                mIntraPeriod = period;
            }
        }
    }
    mLastConfigSeq = gRedroidEncConfigSeq.load(std::memory_order_acquire);
    mLastExtIdrSeq = property_get_int32("sys.xynes.c2.force_idr", 0);

    VaapiEncoderConfig config{};
    config.codec = getCodecType();
    config.width = mWidth;
    config.height = mHeight;
    config.frame_rate = mFrameRate;
    config.bit_rate = mBitrate;
    config.max_bit_rate = mBitrate * 3 / 2;
    config.intra_period = mIntraPeriod;
    config.ip_period = 1;  // no b-frames, keeps latency flat
    config.rc_mode = RateControlMode::CBR;
    config.drm_node = "/dev/dri/renderD128";

    // allow overriding the render node per container/board
    char prop_val[PROPERTY_VALUE_MAX];
    if (property_get("ro.boot.redroid_gpu_node", prop_val, nullptr) > 0) {
        config.drm_node = prop_val;
    }

    mVaapi = VaapiWrapper::Create(config);
    if (!mVaapi) {
        ALOGE("[%s] Failed to create VaapiWrapper context", getComponentName());
        return false;
    }

    mStarted = true;
    mComponentName = getComponentName();
    mLastFrameStartUs = 0;
    resetPerfWindow(NowMonoUs());
    mPrevCacheHits = 0;
    mPrevCacheMisses = 0;
    mVaapi->RequestKeyFrame();
    ALOGI("[%s] Initialized VA-API encoder context: %ux%u @ %u fps, %u bps",
          mComponentName.c_str(), mWidth, mHeight, mFrameRate, mBitrate);
    return true;
}

void C2RedroidEnc::releaseEncoder() {
    if (mVaapi) {
        if (mWinFrames > 0) {
            flushPerfWindow(NowMonoUs(), mVaapi->GetFrameStats());
        }
        mVaapi->Deinitialize();
        mVaapi.reset();
    }
}

void C2RedroidEnc::resetPerfWindow(uint64_t now_us) {
    mLastFlushUs = now_us;
    mWinFrames = 0;
    mWinKeyframes = 0;
    mWinBytesSum = 0;
    mWinBytesMax = 0;
    mWinTotalSumUs = 0;
    mWinTotalMaxUs = 0;
    mWinFenceSumUs = 0;
    mWinFenceMaxUs = 0;
    mWinImportSumUs = 0;
    mWinImportMaxUs = 0;
    mWinVppSumUs = 0;
    mWinVppMaxUs = 0;
    mWinEncSubmitSumUs = 0;
    mWinEncSubmitMaxUs = 0;
    mWinEncSyncSumUs = 0;
    mWinEncSyncMaxUs = 0;
    mWinAllocCopySumUs = 0;
    mWinAllocCopyMaxUs = 0;
    mWinInterFrameSumUs = 0;
    mWinInterFrameMaxUs = 0;
    mWinInterFrameCount = 0;
}

void C2RedroidEnc::flushPerfWindow(uint64_t now_us, const VaapiFrameStats &vstats) {
    if (mWinFrames == 0) {
        resetPerfWindow(now_us);
        return;
    }

    uint64_t elapsed_us = (now_us > mLastFlushUs) ? (now_us - mLastFlushUs) : 1ULL;
    double elapsed_s = static_cast<double>(elapsed_us) / 1000000.0;
    double fps = static_cast<double>(mWinFrames) / elapsed_s;
    double avg_bitrate_kbps = (static_cast<double>(mWinBytesSum) * 8.0) / elapsed_s / 1000.0;
    uint64_t avg_frame_bytes = mWinBytesSum / mWinFrames;

    uint64_t avg_total = mWinTotalSumUs / mWinFrames;
    uint64_t avg_fence = mWinFenceSumUs / mWinFrames;
    uint64_t avg_import = mWinImportSumUs / mWinFrames;
    uint64_t avg_vpp = mWinVppSumUs / mWinFrames;
    uint64_t avg_submit = mWinEncSubmitSumUs / mWinFrames;
    uint64_t avg_sync = mWinEncSyncSumUs / mWinFrames;
    uint64_t avg_copy = mWinAllocCopySumUs / mWinFrames;
    uint64_t avg_inter = (mWinInterFrameCount > 0) ? (mWinInterFrameSumUs / mWinInterFrameCount) : 0ULL;

    uint64_t win_hits = (vstats.cache_hits >= mPrevCacheHits) ? (vstats.cache_hits - mPrevCacheHits) : vstats.cache_hits;
    uint64_t win_misses = (vstats.cache_misses >= mPrevCacheMisses) ? (vstats.cache_misses - mPrevCacheMisses) : vstats.cache_misses;
    mPrevCacheHits = vstats.cache_hits;
    mPrevCacheMisses = vstats.cache_misses;
    uint64_t win_total_imports = win_hits + win_misses;
    double cache_hit_pct = (win_total_imports > 0)
        ? (100.0 * static_cast<double>(win_hits) / static_cast<double>(win_total_imports))
        : 100.0;

    const char *ep_str = (vstats.entrypoint == static_cast<uint32_t>(VAEntrypointEncSliceLP)) ? "LP" : "Slice";
    const char *rc_str = vstats.using_cqp ? "CQP" : "CBR";

    bool force_fd_rescan = ((++mStatsFlushCounter % 5u) == 1u);
    CountSelfFdsFast(&mCachedOpenFds, &mCachedDmabufFds, force_fd_rescan);
    uint32_t open_fds = mCachedOpenFds;
    uint32_t dmabuf_fds = mCachedDmabufFds;

    const char *comp_name = mComponentName.empty() ? "c2.redroid.enc" : mComponentName.c_str();
    __android_log_print(
        ANDROID_LOG_INFO, "C2RedroidPerf",
        "[%s] %ux%u fps=%.1f kbps=%.0f bytes=%llu/%llu kf=%u "
        "tot=%llu/%llu fence=%llu/%llu imp=%llu/%llu vpp=%llu/%llu "
        "sub=%llu/%llu sync=%llu/%llu copy=%llu/%llu inter=%llu/%llu "
        "cache=%.1f%%(%llu/%llu) ep=%s rc=%s qp=%u fds=%u dmabuf=%u",
        comp_name, mWidth, mHeight, fps, avg_bitrate_kbps,
        (unsigned long long)avg_frame_bytes, (unsigned long long)mWinBytesMax, mWinKeyframes,
        (unsigned long long)avg_total, (unsigned long long)mWinTotalMaxUs,
        (unsigned long long)avg_fence, (unsigned long long)mWinFenceMaxUs,
        (unsigned long long)avg_import, (unsigned long long)mWinImportMaxUs,
        (unsigned long long)avg_vpp, (unsigned long long)mWinVppMaxUs,
        (unsigned long long)avg_submit, (unsigned long long)mWinEncSubmitMaxUs,
        (unsigned long long)avg_sync, (unsigned long long)mWinEncSyncMaxUs,
        (unsigned long long)avg_copy, (unsigned long long)mWinAllocCopyMaxUs,
        (unsigned long long)avg_inter, (unsigned long long)mWinInterFrameMaxUs,
        cache_hit_pct, (unsigned long long)win_hits, (unsigned long long)win_misses,
        ep_str, rc_str, vstats.active_qp, open_fds, dmabuf_fds);

    char json_buf[2048];
    int n = snprintf(
        json_buf, sizeof(json_buf),
        "{\n"
        "  \"timestamp_ms\": %llu,\n"
        "  \"component\": \"%s\",\n"
        "  \"width\": %u,\n"
        "  \"height\": %u,\n"
        "  \"window_frames\": %u,\n"
        "  \"window_sec\": %.3f,\n"
        "  \"fps\": %.2f,\n"
        "  \"target_bitrate_bps\": %u,\n"
        "  \"avg_bitrate_kbps\": %.1f,\n"
        "  \"avg_frame_bytes\": %llu,\n"
        "  \"max_frame_bytes\": %llu,\n"
        "  \"keyframes\": %u,\n"
        "  \"entrypoint\": \"%s\",\n"
        "  \"rc_mode\": \"%s\",\n"
        "  \"active_qp\": %u,\n"
        "  \"cache_hits\": %llu,\n"
        "  \"cache_misses\": %llu,\n"
        "  \"total_cache_hits\": %llu,\n"
        "  \"total_cache_misses\": %llu,\n"
        "  \"cache_hit_pct\": %.1f,\n"
        "  \"last_ino\": %llu,\n"
        "  \"last_modifier\": \"0x%llx\",\n"
        "  \"open_fds\": %u,\n"
        "  \"dmabuf_fds\": %u,\n"
        "  \"stages_us\": {\n"
        "    \"total\": {\"avg\": %llu, \"max\": %llu},\n"
        "    \"fence_wait\": {\"avg\": %llu, \"max\": %llu},\n"
        "    \"import\": {\"avg\": %llu, \"max\": %llu},\n"
        "    \"vpp\": {\"avg\": %llu, \"max\": %llu},\n"
        "    \"enc_submit\": {\"avg\": %llu, \"max\": %llu},\n"
        "    \"enc_sync\": {\"avg\": %llu, \"max\": %llu},\n"
        "    \"alloc_copy\": {\"avg\": %llu, \"max\": %llu},\n"
        "    \"inter_frame\": {\"avg\": %llu, \"max\": %llu}\n"
        "  }\n"
        "}\n",
        (unsigned long long)NowWallMs(),
        comp_name, mWidth, mHeight, mWinFrames, elapsed_s, fps,
        mBitrate, avg_bitrate_kbps,
        (unsigned long long)avg_frame_bytes, (unsigned long long)mWinBytesMax, mWinKeyframes,
        ep_str, rc_str, vstats.active_qp,
        (unsigned long long)win_hits, (unsigned long long)win_misses,
        (unsigned long long)vstats.cache_hits, (unsigned long long)vstats.cache_misses,
        cache_hit_pct,
        (unsigned long long)vstats.last_ino, (unsigned long long)vstats.last_modifier,
        open_fds, dmabuf_fds,
        (unsigned long long)avg_total, (unsigned long long)mWinTotalMaxUs,
        (unsigned long long)avg_fence, (unsigned long long)mWinFenceMaxUs,
        (unsigned long long)avg_import, (unsigned long long)mWinImportMaxUs,
        (unsigned long long)avg_vpp, (unsigned long long)mWinVppMaxUs,
        (unsigned long long)avg_submit, (unsigned long long)mWinEncSubmitMaxUs,
        (unsigned long long)avg_sync, (unsigned long long)mWinEncSyncMaxUs,
        (unsigned long long)avg_copy, (unsigned long long)mWinAllocCopyMaxUs,
        (unsigned long long)avg_inter, (unsigned long long)mWinInterFrameMaxUs);

    if (n > 0) {
        size_t len = static_cast<size_t>(std::min<int>(n, static_cast<int>(sizeof(json_buf) - 1)));
        char tmp_path[96];
        snprintf(tmp_path, sizeof(tmp_path), "/data/local/tmp/.c2_stats_%d_%d.tmp", (int)getpid(), (int)gettid());
        bool saved_atomic = false;
        int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            ssize_t written = write(fd, json_buf, len);
            fchmod(fd, 0666);
            close(fd);
            if (written == static_cast<ssize_t>(len) &&
                rename(tmp_path, "/data/local/tmp/c2_encoder_stats.json") == 0) {
                saved_atomic = true;
            } else {
                unlink(tmp_path);
            }
        }
        if (!saved_atomic) {
            int dfd = open("/data/local/tmp/c2_encoder_stats.json", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (dfd >= 0) {
                ssize_t ignored = write(dfd, json_buf, len);
                (void)ignored;
                close(dfd);
            }
        }
    }

    resetPerfWindow(now_us);
}

void C2RedroidEnc::process(const std::unique_ptr<C2Work> &work,
                           const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 1u;
    work->worklets.front()->output.flags = work->input.flags;

    if (mSawInputEOS || work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        mSawInputEOS = true;
        work->worklets.front()->output.flags = (C2FrameData::flags_t)(
            work->worklets.front()->output.flags | C2FrameData::FLAG_END_OF_STREAM);
        if (work->input.buffers.empty()) {
            return;
        }
    }

    if (work->input.buffers.empty()) {
        return;
    }

    uint64_t t_frame_start = NowMonoUs();
    if (mLastFlushUs == 0) {
        mLastFlushUs = t_frame_start;
    }
    uint64_t inter_frame_us = 0;
    bool has_inter_frame = false;
    if (mLastFrameStartUs > 0 && t_frame_start >= mLastFrameStartUs) {
        inter_frame_us = t_frame_start - mLastFrameStartUs;
        has_inter_frame = true;
    }
    mLastFrameStartUs = t_frame_start;

    // Lock-free sequence check: only query C2InterfaceHelper when parameters actually changed
    bool force_keyframe = false;
    int32_t ext_idr_seq = property_get_int32("sys.xynes.c2.force_idr", 0);
    if (ext_idr_seq != mLastExtIdrSeq) {
        mLastExtIdrSeq = ext_idr_seq;
        force_keyframe = true;
    }
    uint64_t cur_seq = gRedroidEncConfigSeq.load(std::memory_order_acquire);
    if (mIntf && (cur_seq != mLastConfigSeq || !work->input.configUpdate.empty())) {
        mLastConfigSeq = cur_seq;
        C2StreamBitrateInfo::output bitrate(0u);
        C2StreamSyncFrameIntervalTuning::output syncInterval(0u);
        C2StreamRequestSyncFrameTuning::output requestSync(0u);
        if (mIntf->query_vb({ &bitrate, &syncInterval, &requestSync }, {}, C2_DONT_BLOCK, nullptr) == C2_OK) {
            if (bitrate.value > 0 && mBitrate != bitrate.value) {
                ALOGI("[%s] Bitrate dynamically updated: %u -> %u bps", getComponentName(), mBitrate, bitrate.value);
                mBitrate = bitrate.value;
                if (mVaapi) {
                    mVaapi->UpdateBitrate(mBitrate);
                }
            }
            if (syncInterval.value > 0) {
                uint32_t period = (uint32_t)((syncInterval.value * mFrameRate) / 1000000LL);
                if (period > 0 && period != mIntraPeriod) {
                    mIntraPeriod = period;
                }
            }
            if (requestSync.value) {
                force_keyframe = true;
                C2StreamRequestSyncFrameTuning::output clearSync(0u, C2_FALSE);
                std::vector<std::unique_ptr<C2SettingResult>> failures;
                mIntf->config_vb({ &clearSync }, C2_MAY_BLOCK, &failures);
                mLastConfigSeq = gRedroidEncConfigSeq.load(std::memory_order_acquire);
    mLastExtIdrSeq = property_get_int32("sys.xynes.c2.force_idr", 0);
            }
        }
    }

    std::shared_ptr<C2Buffer> inBuffer = work->input.buffers[0];
    if (!inBuffer) {
        work->result = C2_CORRUPTED;
        return;
    }

    C2ConstGraphicBlock graphicBlock = inBuffer->data().graphicBlocks().front();
    uint32_t width = graphicBlock.width();
    uint32_t height = graphicBlock.height();

    // Ensure the GPU has finished rendering to the graphic buffer before encoding
    uint64_t t_fence = NowMonoUs();
    C2Fence fence = graphicBlock.fence();
    if (fence.valid() && !fence.ready()) {
        c2_status_t wait_res = fence.wait(100000000LL); // 100ms timeout
        if (wait_res != C2_OK) {
            ALOGW("graphicBlock acquire fence wait returned %d", wait_res);
        }
    }
    uint64_t fence_wait_us = NowMonoUs() - t_fence;

    if (mWidth != width || mHeight != height || !mVaapi) {
        mWidth = width;
        mHeight = height;
        releaseEncoder();
        if (!initEncoder()) {
            work->result = C2_CORRUPTED;
            return;
        }
    }

    const native_handle_t *handle = graphicBlock.handle();
    if (!handle) {
        ALOGE("Failed to obtain native_handle_t from C2GraphicBlock");
        work->result = C2_CORRUPTED;
        return;
    }

    uint64_t t_import = NowMonoUs();
    uint32_t buf_stride = (handle->numInts >= 8 && handle->data[7] > 0) ? (uint32_t)handle->data[7] : width;

    DrmPrimeBuffer drm_buf{};
    uint32_t buf_format = (handle->numInts >= 6 && handle->data[5] != 0) ? (uint32_t)handle->data[5] : 0x35;
    if (!GrallocHelper::ExtractDrmBuffer(handle, width, height, buf_format, buf_stride, &drm_buf)) {
        ALOGE("Failed to extract DRM PRIME parameters from handle");
        work->result = C2_CORRUPTED;
        return;
    }

    VASurfaceID input_surface = mVaapi->ImportDrmPrimeSurface(drm_buf);
    if (input_surface == VA_INVALID_SURFACE) {
        ALOGE("Failed to zero-copy import DRM PRIME surface to VA-API");
        work->result = C2_CORRUPTED;
        return;
    }
    uint64_t import_us = NowMonoUs() - t_import;

    std::shared_ptr<C2LinearBlock> outBlock;
    std::unique_ptr<C2WriteView> writeView;
    c2_status_t alloc_status = C2_OK;

    size_t actual_out_size = 0;
    bool is_keyframe = false;

    bool ok = mVaapi->EncodeFrame(
        input_surface, force_keyframe,
        [&](size_t exact_size) -> void * {
            // Bucket output allocations (>=256KB, 64KB aligned above) so C2BlockPool recycles a few dmabufs instead of leaking/allocating ~55/s
            const size_t min_bucket = 256 * 1024;
            size_t alloc_size = exact_size <= min_bucket
                ? min_bucket
                : ((exact_size + 65535) & ~size_t(65535));
            alloc_status = pool->fetchLinearBlock(
                alloc_size,
                { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE },
                &outBlock);
            if ((alloc_status != C2_OK || !outBlock) && alloc_status != C2_TIMED_OUT) {
                if (!mFallbackPool) {
                    auto store = GetCodec2PlatformAllocatorStore();
                    std::shared_ptr<C2Allocator> alloc;
                    if (store && store->fetchAllocator(C2PlatformAllocatorStore::DMABUFHEAP, &alloc) == C2_OK && alloc) {
                        mFallbackPool = std::make_shared<C2BasicLinearBlockPool>(alloc);
                    }
                }
                if (mFallbackPool) {
                    alloc_status = mFallbackPool->fetchLinearBlock(
                        alloc_size,
                        { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE },
                        &outBlock);
                }
            }
            if (alloc_status != C2_OK || !outBlock) {
                if (alloc_status == C2_TIMED_OUT) {
                    ALOGW("[%s] Output block pool saturated (downstream stalled); dropping frame cleanly", getComponentName());
                } else {
                    ALOGE("Failed to fetch output C2LinearBlock (%zu bytes): %d", alloc_size, alloc_status);
                }
                return nullptr;
            }
            writeView = std::make_unique<C2WriteView>(outBlock->map().get());
            if (writeView->error() != C2_OK) {
                ALOGE("Failed to map output C2LinearBlock: %d", writeView->error());
                return nullptr;
            }
            return writeView->data();
        },
        &actual_out_size, &is_keyframe);

    // Release WriteView mapping before sharing the output linear block
    writeView.reset();

    // Release transient surface if uncached; cached BufferQueue surfaces stay alive in mVaapi
    mVaapi->DestroySurface(input_surface);

    if (alloc_status == C2_TIMED_OUT) {
        // Downstream consumer stalled: drop this frame cleanly without raising MediaCodec error
        mVaapi->RequestKeyFrame();
        work->worklets.front()->output.flags = C2FrameData::FLAG_DROP_FRAME;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->result = C2_OK;
        return;
    }

    if (!ok || actual_out_size == 0 || !outBlock) {
        ALOGE("Hardware frame encoding failed in VA-API");
        work->result = (alloc_status != C2_OK) ? C2_NO_MEMORY : C2_CORRUPTED;
        return;
    }

    std::shared_ptr<C2Buffer> outBuffer = C2Buffer::CreateLinearBuffer(
        outBlock->share(0, actual_out_size, C2Fence()));

    C2FrameData::flags_t out_flags = (C2FrameData::flags_t)0;
    if (is_keyframe) {
        outBuffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(0u, C2Config::SYNC_FRAME));
    }
    if (mSawInputEOS) {
        out_flags = (C2FrameData::flags_t)(out_flags | C2FrameData::FLAG_END_OF_STREAM);
    }

    work->worklets.front()->output.flags = out_flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->worklets.front()->output.buffers.push_back(outBuffer);
    work->result = C2_OK;

    uint64_t t_frame_end = NowMonoUs();
    uint64_t total_process_us = t_frame_end - t_frame_start;
    VaapiFrameStats vstats = mVaapi->GetFrameStats();

    mWinFrames++;
    if (is_keyframe) {
        mWinKeyframes++;
    }
    mWinBytesSum += actual_out_size;
    mWinBytesMax = std::max(mWinBytesMax, static_cast<uint64_t>(actual_out_size));
    mWinTotalSumUs += total_process_us;
    mWinTotalMaxUs = std::max(mWinTotalMaxUs, total_process_us);
    mWinFenceSumUs += fence_wait_us;
    mWinFenceMaxUs = std::max(mWinFenceMaxUs, fence_wait_us);
    mWinImportSumUs += import_us;
    mWinImportMaxUs = std::max(mWinImportMaxUs, import_us);
    mWinVppSumUs += vstats.vpp_us;
    mWinVppMaxUs = std::max(mWinVppMaxUs, vstats.vpp_us);
    mWinEncSubmitSumUs += vstats.enc_submit_us;
    mWinEncSubmitMaxUs = std::max(mWinEncSubmitMaxUs, vstats.enc_submit_us);
    mWinEncSyncSumUs += vstats.enc_sync_us;
    mWinEncSyncMaxUs = std::max(mWinEncSyncMaxUs, vstats.enc_sync_us);
    mWinAllocCopySumUs += vstats.map_copy_us;
    mWinAllocCopyMaxUs = std::max(mWinAllocCopyMaxUs, vstats.map_copy_us);
    if (has_inter_frame) {
        mWinInterFrameSumUs += inter_frame_us;
        mWinInterFrameMaxUs = std::max(mWinInterFrameMaxUs, inter_frame_us);
        mWinInterFrameCount++;
    }

    if (mWinFrames >= 120 || (mWinFrames > 0 && (t_frame_end - mLastFlushUs) >= 2000000ULL)) {
        flushPerfWindow(t_frame_end, vstats);
    }
}

c2_status_t C2RedroidEnc::drain(uint32_t drainMode,
                                const std::shared_ptr<C2BlockPool> &pool) {
    (void)drainMode;
    (void)pool;
    return C2_OK;
}

} // namespace redroid
} // namespace c2
} // namespace media
} // namespace hardware
} // namespace android

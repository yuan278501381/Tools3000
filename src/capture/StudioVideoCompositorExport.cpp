// ─────────────────────────────────────────────────────────────────────────────
// StudioVideoCompositorExport.cpp — 演播室 FFmpeg 视频/GIF/WebP 导出、音频波形/预览帧抽取与高分帧读取器子模块
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioVideoCompositorInternal.h"
#include "capture/ScreenRecorder.h"
#include "capture/StudioBackdropCatalog.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <webp/encode.h>
#include <webp/mux.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace tools3000::capture {

using Microsoft::WRL::ComPtr;
using namespace compositor_detail;

namespace {

void addCodecIfPresent(std::vector<const AVCodec*>& list, const char* name) {
    const AVCodec* c = avcodec_find_encoder_by_name(name);
    if (!c) return;
    for (const auto* existing : list) {
        if (existing == c || std::strcmp(existing->name, c->name) == 0) return;
    }
    list.push_back(c);
}

AVPixelFormat selectStudioEncoderPixFmt(const AVCodec* codec) {
    const bool isMf = codec && std::strstr(codec->name, "_mf") != nullptr;
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) < 0 ||
        !configs || count <= 0) {
        return isMf ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    }
    const auto* fmts = static_cast<const AVPixelFormat*>(configs);
    const std::array<AVPixelFormat, 3> pref = isMf
        ? std::array<AVPixelFormat, 3>{AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P}
        : std::array<AVPixelFormat, 3>{AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_YUVJ420P};
    for (auto target : pref) {
        for (int i = 0; i < count; ++i) {
            if (fmts[i] == target) return target;
        }
    }
    return fmts[0];
}


bool streamComposedFramesFromSource(
    const std::string& rawInputPathUtf8,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    cv::Size outSize,
    int outFps,
    const std::function<bool(const cv::Mat& composedBgr, int64_t frameIdx, double totalEstFrames)>& onFrame,
    const std::function<void(AVFormatContext* inFmt, int inAudioIdx, AVPacket* audioPkt)>& onAudioPacket,
    const std::atomic<bool>* cancelFlag) {
    AVFormatContext* inFmt = nullptr;
    if (avformat_open_input(&inFmt, rawInputPathUtf8.c_str(), nullptr, nullptr) < 0 || !inFmt) {
        return false;
    }
    if (avformat_find_stream_info(inFmt, nullptr) < 0) {
        avformat_close_input(&inFmt);
        return false;
    }

    int inVideoIdx = -1;
    int inAudioIdx = -1;
    for (unsigned i = 0; i < inFmt->nb_streams; ++i) {
        const auto type = inFmt->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && inVideoIdx < 0) inVideoIdx = static_cast<int>(i);
        else if (type == AVMEDIA_TYPE_AUDIO && inAudioIdx < 0) inAudioIdx = static_cast<int>(i);
    }
    if (inVideoIdx < 0) {
        avformat_close_input(&inFmt);
        return false;
    }

    AVStream* inVStream = inFmt->streams[inVideoIdx];
    const AVCodec* vDecCodec = avcodec_find_decoder(inVStream->codecpar->codec_id);
    if (!vDecCodec) {
        avformat_close_input(&inFmt);
        return false;
    }

    AVCodecContext* decCtx = avcodec_alloc_context3(vDecCodec);
    if (decCtx) {
        decCtx->thread_count = 0;
    }
    if (!decCtx ||
        avcodec_parameters_to_context(decCtx, inVStream->codecpar) < 0 ||
        avcodec_open2(decCtx, vDecCodec, nullptr) < 0) {
        if (decCtx) avcodec_free_context(&decCtx);
        avformat_close_input(&inFmt);
        return false;
    }

    const int safeFps = std::clamp(outFps, 4, 120);
    const double inTimeBaseSec = (inVStream->time_base.den > 0)
        ? av_q2d(inVStream->time_base)
        : (1.0 / static_cast<double>(safeFps));

    double estDurSec = timeline.durationMs > 0 ? (static_cast<double>(timeline.durationMs) / 1000.0) : 0.0;
    if (inFmt->duration > 0) {
        estDurSec = (std::max)(estDurSec, static_cast<double>(inFmt->duration) / static_cast<double>(AV_TIME_BASE));
    }
    if (inVStream->duration > 0 && inTimeBaseSec > 0.0) {
        estDurSec = (std::max)(estDurSec, static_cast<double>(inVStream->duration) * inTimeBaseSec);
    }
    const int estTotalFrames = (std::max)(
        30, static_cast<int>(std::ceil(estDurSec * safeFps)) + safeFps * 4);

    StudioTimelineData bakedTimeline = timeline;
    if (bakedTimeline.sourceWidth <= 0) bakedTimeline.sourceWidth = decCtx->width;
    if (bakedTimeline.sourceHeight <= 0) bakedTimeline.sourceHeight = decCtx->height;
    const auto evaluatedFrames = StudioTimelineAnalyzer::bakeEvaluatedFrames(
        bakedTimeline, config, safeFps, estTotalFrames);

    const int64_t srcTotalMs = (std::max<int64_t>)(
        100, static_cast<int64_t>(std::llround(estDurSec * 1000.0)));
    const auto normCuts = StudioTimelineAnalyzer::normalizeCutSegments(
        bakedTimeline.cutSegments, srcTotalMs);
    const int64_t keptDurMs = StudioTimelineAnalyzer::computeKeptDurationMs(normCuts, srcTotalMs);

    StudioVideoCompositor compositor;
    SwsContext* toBgrSws = nullptr;
    AVPacket* inPkt = av_packet_alloc();
    AVFrame* decFrame = av_frame_alloc();

    int64_t rawStepIdx = 0;
    int64_t keptEmittedCount = 0;
    double speedStepAccum = 0.0;
    bool ok = true;
    const double progressDenomFrames = (std::max)(
        1.0, (static_cast<double>(keptDurMs) / 1000.0) * static_cast<double>(safeFps));

    auto resolveSpeedAtMs = [&](int64_t ms) -> float {
        for (const auto& c : normCuts) {
            if (ms >= c.startMs && ms < c.endMs) {
                return c.speedFactor;
            }
        }
        return 1.0f;
    };

    auto emitOne = [&](const cv::Mat& rawBgr) {
        if (!ok || rawBgr.empty()) return;
        const int64_t curSrcMs = static_cast<int64_t>(std::llround(
            (static_cast<double>(rawStepIdx) * 1000.0) / static_cast<double>(safeFps)));
        const float spd = resolveSpeedAtMs(curSrcMs);
        if (spd <= 0.05f) {
            ++rawStepIdx;
            return;
        }
        if (spd > 1.01f) {
            speedStepAccum += 1.0 / static_cast<double>(spd);
            if (speedStepAccum + 1e-6 < 1.0) {
                ++rawStepIdx;
                return;
            }
            speedStepAccum -= 1.0;
        } else {
            speedStepAccum = 0.0;
        }

        const size_t evalIdx = (std::min)(
            static_cast<size_t>(rawStepIdx),
            evaluatedFrames.empty() ? size_t(0) : (evaluatedFrames.size() - 1));
        const StudioEvaluatedFrame fallbackEval{};
        const StudioEvaluatedFrame& eval = evaluatedFrames.empty() ? fallbackEval : evaluatedFrames[evalIdx];
        const auto* annPtr = bakedTimeline.annotations.empty() ? nullptr : &bakedTimeline.annotations;
        cv::Mat composed = compositor.composeFrame(rawBgr, eval, config, outSize, annPtr);
        if (!onFrame(composed, keptEmittedCount, progressDenomFrames)) {
            ok = false;
            return;
        }
        ++keptEmittedCount;
        ++rawStepIdx;
    };

    cv::Mat heldRawBgr;
    bool hasHeldRaw = false;
    int64_t firstRawPts = AV_NOPTS_VALUE;
    int64_t firstAudioPts = AV_NOPTS_VALUE;
    double heldRawSec = 0.0;
    const int maxGapFillFrames = safeFps * 30;

    auto processDecoded = [&]() {
        while (ok && avcodec_receive_frame(decCtx, decFrame) >= 0) {
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                ok = false;
                break;
            }
            if (!toBgrSws) {
                toBgrSws = sws_getContext(
                    decFrame->width, decFrame->height, static_cast<AVPixelFormat>(decFrame->format),
                    decFrame->width, decFrame->height, AV_PIX_FMT_BGR24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
            }

            int64_t rawPts = (decFrame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? decFrame->best_effort_timestamp
                : decFrame->pts;
            double curSec = 0.0;
            if (rawPts != AV_NOPTS_VALUE && inTimeBaseSec > 0.0) {
                if (firstRawPts == AV_NOPTS_VALUE) firstRawPts = rawPts;
                curSec = (std::max)(0.0, static_cast<double>(rawPts - firstRawPts) * inTimeBaseSec);
                if (hasHeldRaw) curSec = (std::max)(curSec, heldRawSec);
            } else {
                curSec = hasHeldRaw ? (heldRawSec + 1.0 / static_cast<double>(safeFps)) : 0.0;
            }

            if (hasHeldRaw) {
                int filled = 0;
                while (ok && filled < maxGapFillFrames &&
                       (static_cast<double>(rawStepIdx) + 0.5) / static_cast<double>(safeFps) < curSec) {
                    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                        ok = false;
                        break;
                    }
                    emitOne(heldRawBgr);
                    ++filled;
                }
            }
            if (!ok) break;

            if (heldRawBgr.rows != decFrame->height || heldRawBgr.cols != decFrame->width || heldRawBgr.type() != CV_8UC3) {
                heldRawBgr.create(decFrame->height, decFrame->width, CV_8UC3);
            }
            uint8_t* dstBgr[4] = {heldRawBgr.data, nullptr, nullptr, nullptr};
            int dstBgrStride[4] = {static_cast<int>(heldRawBgr.step[0]), 0, 0, 0};
            sws_scale(toBgrSws, decFrame->data, decFrame->linesize, 0, decFrame->height, dstBgr, dstBgrStride);
            heldRawSec = curSec;
            hasHeldRaw = true;
        }
    };

    while (ok && av_read_frame(inFmt, inPkt) >= 0) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            av_packet_unref(inPkt);
            ok = false;
            break;
        }
        if (inPkt->stream_index == inVideoIdx) {
            if (avcodec_send_packet(decCtx, inPkt) >= 0) {
                processDecoded();
            }
        } else if (onAudioPacket && inAudioIdx >= 0 && inPkt->stream_index == inAudioIdx) {
            if (normCuts.empty()) {
                onAudioPacket(inFmt, inAudioIdx, inPkt);
            } else {
                AVStream* aSt = inFmt->streams[inAudioIdx];
                const double aTbSec = (aSt->time_base.den > 0) ? av_q2d(aSt->time_base) : (1.0 / 48000.0);
                const int64_t aPts = (inPkt->pts != AV_NOPTS_VALUE) ? inPkt->pts : inPkt->dts;
                if (aPts != AV_NOPTS_VALUE) {
                    if (firstAudioPts == AV_NOPTS_VALUE) firstAudioPts = aPts;
                    const int64_t aSrcMs = static_cast<int64_t>(std::llround(
                        (std::max)(0.0, static_cast<double>(aPts - firstAudioPts) * aTbSec * 1000.0)));
                    const float aSpd = resolveSpeedAtMs(aSrcMs);
                    // 仅保留正常 1.0x 语速区间的原声音频包，并将 PTS/DTS 重映射至剪辑后时间轴
                    if (aSpd > 0.05f && aSpd <= 1.05f) {
                        const int64_t keptMs = StudioTimelineAnalyzer::mapSourceToKeptMs(normCuts, aSrcMs);
                        AVPacket* adjPkt = av_packet_clone(inPkt);
                        if (adjPkt) {
                            const int64_t newPts = static_cast<int64_t>(std::llround(
                                (static_cast<double>(keptMs) / 1000.0) / (std::max)(1e-9, aTbSec)));
                            adjPkt->pts = newPts;
                            adjPkt->dts = newPts;
                            onAudioPacket(inFmt, inAudioIdx, adjPkt);
                            av_packet_free(&adjPkt);
                        }
                    }
                }
            }
        }
        av_packet_unref(inPkt);
    }

    if (ok) {
        avcodec_send_packet(decCtx, nullptr);
        processDecoded();
        if (ok && hasHeldRaw) {
            double finalEndSec = heldRawSec + (1.0 / static_cast<double>(safeFps));
            if (timeline.durationMs > 0) {
                const double tlSec = static_cast<double>(timeline.durationMs) / 1000.0;
                if (tlSec > finalEndSec && tlSec <= heldRawSec + 5.0) {
                    finalEndSec = tlSec;
                }
            }
            int tailFilled = 0;
            while (ok && tailFilled < maxGapFillFrames &&
                   (rawStepIdx == 0 ||
                    (static_cast<double>(rawStepIdx) / static_cast<double>(safeFps)) <
                        finalEndSec - (0.25 / static_cast<double>(safeFps)))) {
                if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                    ok = false;
                    break;
                }
                emitOne(heldRawBgr);
                ++tailFilled;
            }
            // 若全片均被裁剪导致 0 帧输出，兜底保留最后一帧防止空容器报错
            if (ok && keptEmittedCount == 0 && !heldRawBgr.empty()) {
                const StudioEvaluatedFrame fallbackEval{};
                const auto* annPtr = bakedTimeline.annotations.empty() ? nullptr : &bakedTimeline.annotations;
                cv::Mat composed = compositor.composeFrame(
                    heldRawBgr,
                    evaluatedFrames.empty() ? fallbackEval : evaluatedFrames.back(),
                    config, outSize, annPtr);
                if (onFrame(composed, 0, 1.0)) {
                    ++keptEmittedCount;
                } else {
                    ok = false;
                }
            }
        }
    }

    if (toBgrSws) sws_freeContext(toBgrSws);
    av_frame_free(&decFrame);
    av_packet_free(&inPkt);
    avcodec_free_context(&decCtx);
    avformat_close_input(&inFmt);
    return ok && keptEmittedCount > 0;
}

bool probeSourceVideoInfo(const std::string& rawInputPathUtf8, int& outW, int& outH, double& outDurSec, int& outAudioIdx) {
    outW = 1920;
    outH = 1080;
    outDurSec = 0.0;
    outAudioIdx = -1;

    AVFormatContext* inFmt = nullptr;
    if (avformat_open_input(&inFmt, rawInputPathUtf8.c_str(), nullptr, nullptr) < 0 || !inFmt) {
        return false;
    }
    if (avformat_find_stream_info(inFmt, nullptr) < 0) {
        avformat_close_input(&inFmt);
        return false;
    }
    int vIdx = -1;
    for (unsigned i = 0; i < inFmt->nb_streams; ++i) {
        const auto type = inFmt->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && vIdx < 0) {
            vIdx = static_cast<int>(i);
            outW = inFmt->streams[i]->codecpar->width;
            outH = inFmt->streams[i]->codecpar->height;
            if (inFmt->streams[i]->duration > 0 && inFmt->streams[i]->time_base.den > 0) {
                outDurSec = static_cast<double>(inFmt->streams[i]->duration) * av_q2d(inFmt->streams[i]->time_base);
            }
        } else if (type == AVMEDIA_TYPE_AUDIO && outAudioIdx < 0) {
            outAudioIdx = static_cast<int>(i);
        }
    }
    if (inFmt->duration > 0) {
        outDurSec = (std::max)(outDurSec, static_cast<double>(inFmt->duration) / static_cast<double>(AV_TIME_BASE));
    }
    avformat_close_input(&inFmt);
    return vIdx >= 0 && outW > 0 && outH > 0;
}

bool exportContainerVideoOnMta(
    const std::string& rawInputPathUtf8,
    const std::wstring& finalWide,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    const StudioExportOptions& opts,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag) {
    int srcW = timeline.sourceWidth;
    int srcH = timeline.sourceHeight;
    double probedDur = 0.0;
    int probedAudioIdx = -1;
    if (!probeSourceVideoInfo(rawInputPathUtf8, srcW, srcH, probedDur, probedAudioIdx)) {
        return false;
    }

    const cv::Size outSize = StudioVideoCompositor::resolveExportDimensions(srcW, srcH, opts, config.aspectRatio);
    const int outFps = std::clamp(opts.fps > 0 ? opts.fps : 30, 10, 120);
    const bool isWebm = (opts.format == StudioExportFormat::WebM_VP9);
    const bool isHevc = (opts.format == StudioExportFormat::MP4_HEVC);
    const char* containerFmt = isWebm ? "webm" : "mp4";

    const std::wstring tempWide = finalWide + (isWebm ? L".studio_tmp.webm" : L".studio_tmp.mp4");
    const std::string tempUtf8 = tools3000::core::WinUtils::wstringToUtf8(tempWide);

    AVFormatContext* outFmt = nullptr;
    if (avformat_alloc_output_context2(&outFmt, nullptr, containerFmt, tempUtf8.c_str()) < 0 || !outFmt) {
        return false;
    }

    std::vector<const AVCodec*> encCandidates;
    wchar_t testModeEnv[8]{};
    const bool inUnitTestMode = GetEnvironmentVariableW(L"TOOLS3000_TEST_MODE", testModeEnv, 8) > 0 &&
                                testModeEnv[0] == L'1';
    if (isWebm) {
        addCodecIfPresent(encCandidates, "libvpx-vp9");
        addCodecIfPresent(encCandidates, "vp9");
    } else if (inUnitTestMode) {
        for (const char* name : {"libx264", "libopenh264", "mpeg4", "h264_mf"}) {
            addCodecIfPresent(encCandidates, name);
        }
    } else if (isHevc) {
        for (const char* name : {"hevc_nvenc", "hevc_qsv", "hevc_amf", "hevc_mf", "libx265", "h264_mf", "libopenh264"}) {
            addCodecIfPresent(encCandidates, name);
        }
    } else {
        const bool highRes = (static_cast<int64_t>(outSize.width) * outSize.height) > (1920LL * 1200LL);
        for (const char* name : {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"}) {
            addCodecIfPresent(encCandidates, name);
        }
        if (highRes) {
            addCodecIfPresent(encCandidates, "hevc_mf");
        }
        for (const char* name : {"libx264", "libopenh264", "mpeg4"}) {
            addCodecIfPresent(encCandidates, name);
        }
    }

    const int64_t areaPx = static_cast<int64_t>(outSize.width) * static_cast<int64_t>(outSize.height);
    const double areaRatio = std::clamp(static_cast<double>(areaPx) / (1920.0 * 1080.0), 0.30, 6.0);
    const double fpsRatio = std::clamp(static_cast<double>(outFps) / 30.0, 0.55, 2.2);
    const double qScale = qualityScaleFactor(opts.quality);
    // 桌面录屏含高频锐利代码边缘与全屏自动运镜缩放（全画面同时位移无平移运动矢量复用），
    // 必须为硬件/软件编码器提供充裕动态码率天花板（1080p 推荐档 14 Mbps），彻底根除缩放期间 QP=51 宏块马赛克崩坏
    const int64_t targetBitrate = std::clamp(
        static_cast<int64_t>(std::llround(areaRatio * fpsRatio * qScale * (isHevc ? 10'000'000.0 : 14'000'000.0))),
        2'500'000LL, 85'000'000LL);

    AVCodecContext* encCtx = nullptr;
    const AVCodec* chosenEnc = nullptr;
    for (const auto* cand : encCandidates) {
        const bool isMf = std::strstr(cand->name, "_mf") != nullptr;
        // Media Foundation 编码器：第 0 轮优先尝试硬件 Quality 恒定画质模式，第 1 轮硬件高码率 VBR/CBR，第 2 轮软件 MFT
        const int mfPasses = (isMf && !inUnitTestMode) ? 3 : 1;

        for (int mfPass = 0; mfPass < mfPasses; ++mfPass) {
            AVCodecContext* attempt = avcodec_alloc_context3(cand);
            if (!attempt) continue;
            attempt->width = outSize.width;
            attempt->height = outSize.height;
            attempt->time_base = {1, outFps};
            attempt->framerate = {outFps, 1};
            attempt->gop_size = outFps * 2;
            attempt->max_b_frames = 0;
            attempt->bit_rate = targetBitrate;
            attempt->rc_max_rate = targetBitrate * 3 / 2;
            attempt->rc_buffer_size = static_cast<int>((std::min<int64_t>)(targetBitrate * 2, 160'000'000LL));
            attempt->pix_fmt = selectStudioEncoderPixFmt(cand);
            if (outFmt->oformat->flags & AVFMT_GLOBALHEADER) {
                attempt->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            if (isMf) {
                if (!inUnitTestMode && mfPass <= 1 && attempt->priv_data) {
                    av_opt_set_int(attempt->priv_data, "hw_encoding", 1, 0);
                }
                if (attempt->priv_data) {
                    const int mfQual = (opts.quality == StudioExportQuality::High)
                        ? 92 : ((opts.quality == StudioExportQuality::Compact) ? 74 : 86);
                    if (mfPass == 0) {
                        av_opt_set(attempt->priv_data, "rate_control", "quality", 0);
                        av_opt_set_int(attempt->priv_data, "quality", mfQual, 0);
                    } else {
                        av_opt_set(attempt->priv_data, "rate_control", "u_vbr", 0);
                        av_opt_set_int(attempt->priv_data, "quality", mfQual, 0);
                    }
                }
            } else {
                attempt->thread_count = 0;
            }

            if (std::strstr(cand->name, "_nvenc") != nullptr && attempt->priv_data) {
                const int cqVal = (opts.quality == StudioExportQuality::High)
                    ? 18 : ((opts.quality == StudioExportQuality::Compact) ? 26 : 21);
                av_opt_set(attempt->priv_data, "rc", "vbr", 0);
                av_opt_set_int(attempt->priv_data, "cq", cqVal, 0);
            } else if (std::strstr(cand->name, "_qsv") != nullptr && attempt->priv_data) {
                const int qsvQual = (opts.quality == StudioExportQuality::High)
                    ? 18 : ((opts.quality == StudioExportQuality::Compact) ? 26 : 21);
                av_opt_set_int(attempt->priv_data, "global_quality", qsvQual, 0);
            } else if (std::strcmp(cand->name, "libx264") == 0 && attempt->priv_data) {
                av_opt_set(attempt->priv_data, "preset", areaPx >= (2560LL * 1440LL) ? "ultrafast" : "veryfast", 0);
                const int crfVal = (opts.quality == StudioExportQuality::High)
                    ? 19 : ((opts.quality == StudioExportQuality::Compact) ? 27 : 23);
                av_opt_set_int(attempt->priv_data, "crf", crfVal, 0);
                attempt->rc_max_rate = targetBitrate * 3 / 2;
                attempt->rc_buffer_size = static_cast<int>((std::min<int64_t>)(targetBitrate * 2, 120'000'000LL));
            } else if (std::strcmp(cand->name, "libx265") == 0 && attempt->priv_data) {
                av_opt_set(attempt->priv_data, "preset", "ultrafast", 0);
            } else if (std::strcmp(cand->name, "libopenh264") == 0 && attempt->priv_data) {
                av_opt_set_int(attempt->priv_data, "allow_skip_frames", 0, 0);
                av_opt_set(attempt->priv_data, "slice_mode", "auto", 0);
                attempt->rc_max_rate = targetBitrate * 3 / 2;
                attempt->rc_buffer_size = static_cast<int>((std::min<int64_t>)(targetBitrate * 2, 120'000'000LL));
            } else if (std::strcmp(cand->name, "libvpx-vp9") == 0 && attempt->priv_data) {
                av_opt_set(attempt->priv_data, "deadline", "realtime", 0);
                av_opt_set_int(attempt->priv_data, "cpu-used", 6, 0);
                av_opt_set_int(attempt->priv_data, "row-mt", 1, 0);
            }

            int openRet = avcodec_open2(attempt, cand, nullptr);
            if (openRet >= 0 && isMf &&
                (attempt->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
                attempt->extradata_size <= 0) {
                // 异步硬件 MFT 首帧前无 SPS/PPS extradata，预馈单帧提取序列头写入 MP4 avcC/hvcC
                if (AVCodecContext* probeCtx = avcodec_alloc_context3(cand)) {
                    probeCtx->width = attempt->width;
                    probeCtx->height = attempt->height;
                    probeCtx->time_base = attempt->time_base;
                    probeCtx->framerate = attempt->framerate;
                    probeCtx->gop_size = attempt->gop_size;
                    probeCtx->max_b_frames = attempt->max_b_frames;
                    probeCtx->bit_rate = attempt->bit_rate;
                    probeCtx->pix_fmt = attempt->pix_fmt;
                    probeCtx->flags = attempt->flags;
                    if (!inUnitTestMode && mfPass <= 1 && probeCtx->priv_data) {
                        av_opt_set_int(probeCtx->priv_data, "hw_encoding", 1, 0);
                    }
                    if (probeCtx->priv_data) {
                        const int mfQual = (opts.quality == StudioExportQuality::High)
                            ? 92 : ((opts.quality == StudioExportQuality::Compact) ? 74 : 86);
                        av_opt_set(probeCtx->priv_data, "rate_control", mfPass == 0 ? "quality" : "u_vbr", 0);
                        av_opt_set_int(probeCtx->priv_data, "quality", mfQual, 0);
                    }
                    if (avcodec_open2(probeCtx, cand, nullptr) >= 0) {
                        AVFrame* pf = av_frame_alloc();
                        AVPacket* pp = av_packet_alloc();
                        if (pf && pp) {
                            pf->format = probeCtx->pix_fmt;
                            pf->width = probeCtx->width;
                            pf->height = probeCtx->height;
                            if (av_frame_get_buffer(pf, 0) >= 0) {
                                if (pf->data[0]) std::memset(pf->data[0], 16, static_cast<size_t>(pf->linesize[0]) * pf->height);
                                if (pf->data[1]) std::memset(pf->data[1], 128, static_cast<size_t>(pf->linesize[1]) * (pf->height / 2));
                                if (pf->data[2]) std::memset(pf->data[2], 128, static_cast<size_t>(pf->linesize[2]) * (pf->height / 2));
                                for (int k = 0; k < 6 && attempt->extradata_size <= 0; ++k) {
                                    pf->pts = k;
                                    pf->pict_type = (k == 0) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
                                    avcodec_send_frame(probeCtx, pf);
                                    while (avcodec_receive_packet(probeCtx, pp) >= 0) {
                                        size_t sdBytes = 0;
                                        const uint8_t* sd = av_packet_get_side_data(pp, AV_PKT_DATA_NEW_EXTRADATA, &sdBytes);
                                        if (probeCtx->extradata && probeCtx->extradata_size > 0) {
                                            const int sz = probeCtx->extradata_size;
                                            attempt->extradata = static_cast<uint8_t*>(av_mallocz(static_cast<size_t>(sz) + AV_INPUT_BUFFER_PADDING_SIZE));
                                            if (attempt->extradata) {
                                                std::memcpy(attempt->extradata, probeCtx->extradata, static_cast<size_t>(sz));
                                                attempt->extradata_size = sz;
                                            }
                                        } else if (sd && sdBytes > 0) {
                                            attempt->extradata = static_cast<uint8_t*>(av_mallocz(sdBytes + AV_INPUT_BUFFER_PADDING_SIZE));
                                            if (attempt->extradata) {
                                                std::memcpy(attempt->extradata, sd, sdBytes);
                                                attempt->extradata_size = static_cast<int>(sdBytes);
                                            }
                                        }
                                        av_packet_unref(pp);
                                        if (attempt->extradata_size > 0) break;
                                    }
                                }
                            }
                        }
                        if (pp) av_packet_free(&pp);
                        if (pf) av_frame_free(&pf);
                    }
                    avcodec_free_context(&probeCtx);
                }
                if (attempt->extradata_size <= 0) {
                    openRet = AVERROR_EXTERNAL;
                }
            }

            if (openRet >= 0) {
                encCtx = attempt;
                chosenEnc = cand;
                break;
            }
            avcodec_free_context(&attempt);
        }
        if (encCtx && chosenEnc) break;
    }

    if (!encCtx || !chosenEnc) {
        avformat_free_context(outFmt);
        return false;
    }

    AVStream* outVStream = avformat_new_stream(outFmt, chosenEnc);
    if (!outVStream || avcodec_parameters_from_context(outVStream->codecpar, encCtx) < 0) {
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        return false;
    }
    outVStream->time_base = encCtx->time_base;

    // MP4 封装下无损复制 AAC/MP3 音轨
    AVStream* outAStream = nullptr;
    if (!isWebm && probedAudioIdx >= 0) {
        AVFormatContext* probeIn = nullptr;
        if (avformat_open_input(&probeIn, rawInputPathUtf8.c_str(), nullptr, nullptr) >= 0 && probeIn) {
            if (avformat_find_stream_info(probeIn, nullptr) >= 0 &&
                static_cast<unsigned>(probedAudioIdx) < probeIn->nb_streams) {
                AVStream* inAS = probeIn->streams[probedAudioIdx];
                if (inAS->codecpar->codec_id == AV_CODEC_ID_AAC || inAS->codecpar->codec_id == AV_CODEC_ID_MP3) {
                    outAStream = avformat_new_stream(outFmt, nullptr);
                    if (outAStream && avcodec_parameters_copy(outAStream->codecpar, inAS->codecpar) >= 0) {
                        outAStream->codecpar->codec_tag = 0;
                        outAStream->time_base = inAS->time_base;
                    } else {
                        outAStream = nullptr;
                    }
                }
            }
            avformat_close_input(&probeIn);
        }
    }

    if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outFmt->pb, tempUtf8.c_str(), AVIO_FLAG_WRITE) < 0) {
            avcodec_free_context(&encCtx);
            avformat_free_context(outFmt);
            return false;
        }
    }

    if (avformat_write_header(outFmt, nullptr) < 0) {
        if (outFmt->pb) avio_closep(&outFmt->pb);
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        std::error_code ec;
        std::filesystem::remove(tempWide, ec);
        return false;
    }

    SwsContext* fromBgrSws = sws_getContext(
        outSize.width, outSize.height, AV_PIX_FMT_BGR24,
        outSize.width, outSize.height, encCtx->pix_fmt,
        SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    AVPacket* outPkt = av_packet_alloc();
    AVFrame* encFrame = av_frame_alloc();
    encFrame->format = encCtx->pix_fmt;
    encFrame->width = outSize.width;
    encFrame->height = outSize.height;
    av_frame_get_buffer(encFrame, 0);

    bool writeOk = true;
    auto drainEncoder = [&]() {
        while (avcodec_receive_packet(encCtx, outPkt) >= 0) {
            av_packet_rescale_ts(outPkt, encCtx->time_base, outVStream->time_base);
            outPkt->stream_index = outVStream->index;
            if (av_interleaved_write_frame(outFmt, outPkt) < 0) {
                writeOk = false;
            }
            av_packet_unref(outPkt);
        }
    };

    cv::Mat lastEncodedBgr;
    cv::Mat deferredTailBgr;
    int64_t lastEncodedPts = -1;
    int64_t lastObservedPts = -1;
    // 硬件 MFT 编码器的内部码率与量化器步进依赖连续均匀帧率：禁止对 _mf 进行长间隔 VFR 跳帧，防止静止转运镜瞬间触发 QP=51 块噪
    const bool chosenIsMf = chosenEnc && (std::strstr(chosenEnc->name, "_mf") != nullptr);
    const int maxStaticSkipFrames = chosenIsMf ? 1 : (std::max)(2, outFps / 2);

    auto isFrameNearlyIdentical = [](const cv::Mat& a, const cv::Mat& b) noexcept -> bool {
        if (a.empty() || b.empty() || a.rows != b.rows || a.cols != b.cols || a.type() != b.type()) {
            return false;
        }
        const int rowStep = (a.rows > 240) ? 2 : 1;
        const int bytesPerRow = a.cols * 3;
        uint64_t diffSum = 0;
        uint64_t sampleCount = 0;
        for (int y = 0; y < a.rows; y += rowStep) {
            const uint8_t* ra = a.ptr<uint8_t>(y);
            const uint8_t* rb = b.ptr<uint8_t>(y);
            for (int i = 0; i < bytesPerRow; i += 3) {
                const int d0 = std::abs(static_cast<int>(ra[i]) - static_cast<int>(rb[i]));
                const int d1 = std::abs(static_cast<int>(ra[i + 1]) - static_cast<int>(rb[i + 1]));
                const int d2 = std::abs(static_cast<int>(ra[i + 2]) - static_cast<int>(rb[i + 2]));
                if (d0 > 2 || d1 > 2 || d2 > 2) return false;
                diffSum += static_cast<uint64_t>(d0 + d1 + d2);
                sampleCount += 3;
            }
        }
        return sampleCount > 0 && (static_cast<double>(diffSum) / static_cast<double>(sampleCount)) < 0.14;
    };

    auto encodeComposedFrameAtPts = [&](const cv::Mat& bgr, int64_t pts) -> bool {
        if (!writeOk || av_frame_make_writable(encFrame) < 0) return false;
        const uint8_t* srcSlice[4] = {bgr.data, nullptr, nullptr, nullptr};
        int srcStride[4] = {static_cast<int>(bgr.step[0]), 0, 0, 0};
        sws_scale(fromBgrSws, srcSlice, srcStride, 0, outSize.height, encFrame->data, encFrame->linesize);
        encFrame->pts = pts;
        encFrame->pict_type = (pts == 0) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (avcodec_send_frame(encCtx, encFrame) < 0) return false;
        drainEncoder();
        lastEncodedBgr = bgr.clone();
        lastEncodedPts = pts;
        return writeOk;
    };

    const bool streamOk = streamComposedFramesFromSource(
        rawInputPathUtf8,
        timeline,
        config,
        outSize,
        outFps,
        [&](const cv::Mat& composedBgr, int64_t frameIdx, double totalEst) -> bool {
            lastObservedPts = frameIdx;
            if (lastEncodedPts >= 0 &&
                (frameIdx - lastEncodedPts) < maxStaticSkipFrames &&
                isFrameNearlyIdentical(composedBgr, lastEncodedBgr)) {
                deferredTailBgr = composedBgr;
            } else {
                deferredTailBgr.release();
                if (!encodeComposedFrameAtPts(composedBgr, frameIdx)) {
                    return false;
                }
            }
            if (onProgress) {
                onProgress(std::clamp(static_cast<double>(frameIdx + 1) / totalEst, 0.0, 0.99));
            }
            return writeOk;
        },
        [&](AVFormatContext* inFmt, int inAudioIdx, AVPacket* audioPkt) {
            if (!outAStream) return;
            AVPacket* cloned = av_packet_clone(audioPkt);
            if (!cloned) return;
            av_packet_rescale_ts(cloned, inFmt->streams[inAudioIdx]->time_base, outAStream->time_base);
            cloned->stream_index = outAStream->index;
            cloned->pos = -1;
            av_interleaved_write_frame(outFmt, cloned);
            av_packet_free(&cloned);
        },
        cancelFlag);

    if (streamOk && writeOk) {
        if (!deferredTailBgr.empty() && lastObservedPts > lastEncodedPts) {
            encodeComposedFrameAtPts(deferredTailBgr, lastObservedPts);
        }
        avcodec_send_frame(encCtx, nullptr);
        drainEncoder();
        if (av_write_trailer(outFmt) < 0) {
            writeOk = false;
        }
    }

    if (fromBgrSws) sws_freeContext(fromBgrSws);
    av_frame_free(&encFrame);
    av_packet_free(&outPkt);
    if (outFmt->pb) avio_closep(&outFmt->pb);
    avcodec_free_context(&encCtx);
    avformat_free_context(outFmt);

    std::error_code ec;
    if (!streamOk || !writeOk) {
        std::filesystem::remove(tempWide, ec);
        return false;
    }
    if (!MoveFileExW(tempWide.c_str(), finalWide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tempWide, ec);
        return false;
    }
    return true;
}

bool runSingleGifEncodePass(
    const std::string& rawInputPathUtf8,
    const std::wstring& tempWide,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    cv::Size outSize,
    int effectiveFps,
    bool enableDithering,
    double progressBase,
    double progressSpan,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag) {
    const std::string tempUtf8 = tools3000::core::WinUtils::wstringToUtf8(tempWide);
    const AVCodec* gifCodec = avcodec_find_encoder(AV_CODEC_ID_GIF);
    if (!gifCodec) return false;

    AVFormatContext* outFmt = nullptr;
    if (avformat_alloc_output_context2(&outFmt, nullptr, "gif", tempUtf8.c_str()) < 0 || !outFmt) {
        return false;
    }

    AVCodecContext* encCtx = avcodec_alloc_context3(gifCodec);
    if (!encCtx) {
        avformat_free_context(outFmt);
        return false;
    }
    encCtx->width = outSize.width;
    encCtx->height = outSize.height;
    encCtx->time_base = {1, 100}; // GIF89a 原生 10ms (centisecond) 时间基准
    encCtx->framerate = {effectiveFps, 1};
    encCtx->pix_fmt = AV_PIX_FMT_PAL8;
    if (encCtx->priv_data) {
        av_opt_set(encCtx->priv_data, "gifflags", "+transdiff", 0);
    }

    if (avcodec_open2(encCtx, gifCodec, nullptr) < 0) {
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        return false;
    }

    AVStream* outVStream = avformat_new_stream(outFmt, gifCodec);
    if (!outVStream || avcodec_parameters_from_context(outVStream->codecpar, encCtx) < 0) {
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        return false;
    }
    outVStream->time_base = {1, 100};

    if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outFmt->pb, tempUtf8.c_str(), AVIO_FLAG_WRITE) < 0) {
            avcodec_free_context(&encCtx);
            avformat_free_context(outFmt);
            return false;
        }
    }

    AVDictionary* muxOpts = nullptr;
    av_dict_set(&muxOpts, "loop", "0", 0);
    av_dict_set(&muxOpts, "final_delay", "8", 0);
    const int hdrRet = avformat_write_header(outFmt, &muxOpts);
    av_dict_free(&muxOpts);
    if (hdrRet < 0) {
        if (outFmt->pb) avio_closep(&outFmt->pb);
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        return false;
    }

    AVPacket* outPkt = av_packet_alloc();
    AVFrame* encFrame = av_frame_alloc();
    encFrame->format = AV_PIX_FMT_PAL8;
    encFrame->width = outSize.width;
    encFrame->height = outSize.height;
    av_frame_get_buffer(encFrame, 0);

    // 演播室画布自带完整矩形背景，严禁在无 GCE 透明索引元数据下手工把静止像素挖空为 palette[255] = 0x00000000，
    // 统一交由 FFmpeg gif 编码器内置 gifflags=+transdiff 合规执行帧间透明裁剪，彻底根治微信表情渲染白底反色花屏
    GifPaletteOptimizer palOpt{};
    palOpt.reserveTransparent = false;
    palOpt.enableFrameDiff = false;
    palOpt.enableDithering = enableDithering;

    // 跨全片均匀采样预构建全局 256 色调色板并全程锁定：
    // 1) 避免片头第 0 帧尚未出现中途浅色新窗口导致前 30 帧亮底严重偏黄/偏青、播放几秒后才跳变恢复；
    // 2) 全片 0 个 Local Color Table，保证 FFmpeg gifflags=+transdiff 帧间差分压缩全程生效，杜绝 100MB+ 体积失控膨胀。
    {
        std::vector<cv::Mat> sampleDeck;
        int rawW = 0, rawH = 0;
        double rawDurSec = 0.0;
        if (StudioVideoCompositor::extractPreviewDeck(rawInputPathUtf8, 720, 12, sampleDeck, rawW, rawH, rawDurSec) &&
            !sampleDeck.empty()) {
            StudioTimelineData sampleTl = timeline;
            const float scaleSampleX = (rawW > 0) ? (static_cast<float>(sampleDeck[0].cols) / static_cast<float>(rawW)) : 1.0f;
            const float scaleSampleY = (rawH > 0) ? (static_cast<float>(sampleDeck[0].rows) / static_cast<float>(rawH)) : 1.0f;
            sampleTl.sourceWidth = sampleDeck[0].cols;
            sampleTl.sourceHeight = sampleDeck[0].rows;
            for (auto& cs : sampleTl.cursorSamples) { cs.x *= scaleSampleX; cs.y *= scaleSampleY; }
            for (auto& ck : sampleTl.clickEvents)   { ck.x *= scaleSampleX; ck.y *= scaleSampleY; }
            for (auto& fc : sampleTl.focusEvents)   { fc.x *= scaleSampleX; fc.y *= scaleSampleY; }
            for (auto& zs : sampleTl.zoomSegments)  { zs.focusX *= scaleSampleX; zs.focusY *= scaleSampleY; }

            const int sampleEvalCount = (std::max)(2, static_cast<int>(sampleDeck.size()));
            const double safeDur = (rawDurSec > 0.05) ? rawDurSec : 1.0;
            const int sampleEvalFps = (std::max)(1, static_cast<int>(std::ceil(sampleEvalCount / safeDur)));
            const auto sampleEvals = StudioTimelineAnalyzer::bakeEvaluatedFrames(
                sampleTl, config, sampleEvalFps, (std::max)(sampleEvalCount + 2, static_cast<int>(std::ceil(safeDur * sampleEvalFps)) + 2));

            const cv::Size palCanvasSz(
                (std::min)(outSize.width, 640),
                (std::max)(64, static_cast<int>(std::round(static_cast<double>(outSize.height) *
                    (std::min)(1.0, 640.0 / static_cast<double>((std::max)(1, outSize.width)))))));
            StudioVideoCompositor palComp;
            const auto* annPtr = sampleTl.annotations.empty() ? nullptr : &sampleTl.annotations;

            palOpt.resetHistogram();
            for (size_t si = 0; si < sampleDeck.size(); ++si) {
                const double ratio = (sampleDeck.size() > 1)
                    ? (static_cast<double>(si) / static_cast<double>(sampleDeck.size() - 1))
                    : 0.0;
                const size_t eIdx = sampleEvals.empty()
                    ? 0
                    : (std::min)(sampleEvals.size() - 1,
                                 static_cast<size_t>(std::llround(ratio * static_cast<double>(sampleEvals.size() - 1))));
                const StudioEvaluatedFrame fbEval{};
                const cv::Mat sampleComp = palComp.composeFrame(
                    sampleDeck[si], sampleEvals.empty() ? fbEval : sampleEvals[eIdx], config, palCanvasSz, annPtr, false);
                if (!sampleComp.empty()) {
                    palOpt.accumulateHistogram(
                        sampleComp.data, sampleComp.cols, sampleComp.rows, static_cast<int>(sampleComp.step[0]), 3);
                }
            }
            palOpt.finalizePaletteFromHistogram();
            palOpt.lockPalette = true;
        }
    }

    bool writeOk = true;
    int64_t lastCentisecPts = -1;

    auto drainGif = [&]() {
        while (avcodec_receive_packet(encCtx, outPkt) >= 0) {
            av_packet_rescale_ts(outPkt, encCtx->time_base, outVStream->time_base);
            outPkt->stream_index = outVStream->index;
            if (av_interleaved_write_frame(outFmt, outPkt) < 0) {
                writeOk = false;
            }
            av_packet_unref(outPkt);
        }
    };

    const bool streamOk = streamComposedFramesFromSource(
        rawInputPathUtf8,
        timeline,
        config,
        outSize,
        effectiveFps,
        [&](const cv::Mat& composedBgr, int64_t frameIdx, double totalEst) -> bool {
            if (!writeOk || av_frame_make_writable(encFrame) < 0) return false;
            palOpt.quantizeFrame(
                composedBgr.data,
                outSize.width,
                outSize.height,
                static_cast<int>(composedBgr.step[0]),
                3,
                encFrame->data[0],
                encFrame->linesize[0],
                reinterpret_cast<uint32_t*>(encFrame->data[1]));

            // 严格保障每帧间隔 >= 2 centiseconds (20ms)，彻底规避微信与 Chromium 对 <20ms GIF 强制降速慢放惩罚
            int64_t csPts = static_cast<int64_t>(std::llround(
                (static_cast<double>(frameIdx) * 100.0) / static_cast<double>(effectiveFps)));
            if (lastCentisecPts >= 0 && csPts < lastCentisecPts + 2) {
                csPts = lastCentisecPts + 2;
            }
            lastCentisecPts = csPts;
            encFrame->pts = csPts;

            if (avcodec_send_frame(encCtx, encFrame) < 0) return false;
            drainGif();
            if (onProgress) {
                const double sub = std::clamp(static_cast<double>(frameIdx + 1) / totalEst, 0.0, 0.99);
                onProgress(std::clamp(progressBase + sub * progressSpan, 0.0, 0.99));
            }
            return writeOk;
        },
        nullptr,
        cancelFlag);

    if (streamOk && writeOk) {
        avcodec_send_frame(encCtx, nullptr);
        drainGif();
        if (av_write_trailer(outFmt) < 0) {
            writeOk = false;
        }
    }

    av_frame_free(&encFrame);
    av_packet_free(&outPkt);
    if (outFmt->pb) avio_closep(&outFmt->pb);
    avcodec_free_context(&encCtx);
    avformat_free_context(outFmt);
    return streamOk && writeOk;
}

bool exportAnimatedGifOnMta(
    const std::string& rawInputPathUtf8,
    const std::wstring& finalWide,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    const StudioExportOptions& opts,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag) {
    int srcW = timeline.sourceWidth;
    int srcH = timeline.sourceHeight;
    double probedDur = 0.0;
    int dummyAudio = -1;
    if (!probeSourceVideoInfo(rawInputPathUtf8, srcW, srcH, probedDur, dummyAudio)) {
        return false;
    }

    const double durSec = (std::max)(
        probedDur,
        timeline.durationMs > 0 ? static_cast<double>(timeline.durationMs) / 1000.0 : 1.0);
    const bool wechatAutoPlay = (opts.format == StudioExportFormat::GIF_WeChat);

    cv::Size outSize = StudioVideoCompositor::resolveExportDimensions(srcW, srcH, opts, config.aspectRatio);
    int targetFps = std::clamp(opts.fps > 0 ? opts.fps : 12, 6, wechatAutoPlay ? 15 : 20);
    if (wechatAutoPlay) {
        // 微信聊天自动播放总帧数安全上限守护 (<= 260 帧)，超长片段自动平滑降帧率保持 1.0x 原速
        const int maxSafeFps = static_cast<int>(std::floor(static_cast<double>(kWeChatGifMaxFrames) / (std::max)(1.0, durSec)));
        targetFps = std::clamp((std::min)(targetFps, maxSafeFps), 5, 15);
    }

    const bool useDitherPass1 = (opts.quality != StudioExportQuality::Compact);
    const std::wstring tempWide = finalWide + L".studio_tmp.gif";
    std::error_code ec;
    std::filesystem::remove(tempWide, ec);

    if (!runSingleGifEncodePass(
            rawInputPathUtf8, tempWide, timeline, config, outSize, targetFps, useDitherPass1,
            0.0, wechatAutoPlay ? 0.85 : 1.0, onProgress, cancelFlag)) {
        std::filesystem::remove(tempWide, ec);
        return false;
    }

    // 微信聊天框自动播放 < 4.85MB 闭环硬预算护栏：若首轮因全屏滚动超出安全线，自动收敛尺寸与无抖动游程重压
    if (wechatAutoPlay) {
        for (int retry = 0; retry < 2; ++retry) {
            const uint64_t actualBytes = std::filesystem::file_size(tempWide, ec);
            if (ec || actualBytes <= kWeChatGifSafeMaxBytes) {
                break;
            }
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                std::filesystem::remove(tempWide, ec);
                return false;
            }
            const double scaleDown = std::clamp(
                std::sqrt(static_cast<double>(kWeChatGifSafeMaxBytes) * 0.90 / static_cast<double>(actualBytes)),
                0.62, 0.86);
            outSize = StudioVideoCompositor::resolveOutputCanvasSize(
                (std::max)(520, static_cast<int>(std::round(outSize.width * scaleDown))),
                (std::max)(320, static_cast<int>(std::round(outSize.height * scaleDown))));
            targetFps = (std::max)(6, targetFps - 2);
            std::filesystem::remove(tempWide, ec);
            if (!runSingleGifEncodePass(
                    rawInputPathUtf8, tempWide, timeline, config, outSize, targetFps,
                    /*enableDithering=*/false, 0.85 + retry * 0.07, 0.07, onProgress, cancelFlag)) {
                std::filesystem::remove(tempWide, ec);
                return false;
            }
        }
    }

    if (!MoveFileExW(tempWide.c_str(), finalWide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tempWide, ec);
        return false;
    }
    return true;
}

bool exportAnimatedWebPOnMta(
    const std::string& rawInputPathUtf8,
    const std::wstring& finalWide,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    const StudioExportOptions& opts,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag) {
    int srcW = timeline.sourceWidth;
    int srcH = timeline.sourceHeight;
    double probedDur = 0.0;
    int dummyAudio = -1;
    if (!probeSourceVideoInfo(rawInputPathUtf8, srcW, srcH, probedDur, dummyAudio)) {
        return false;
    }

    const cv::Size outSize = StudioVideoCompositor::resolveExportDimensions(srcW, srcH, opts, config.aspectRatio);
    const int outFps = std::clamp(opts.fps > 0 ? opts.fps : 24, 6, 60);

    WebPAnimEncoderOptions animOpts;
    if (!WebPAnimEncoderOptionsInit(&animOpts)) {
        return false;
    }
    animOpts.anim_params.loop_count = 0; // 网页无限循环自动播放
    animOpts.allow_mixed = 1;
    animOpts.minimize_size = 0;

    WebPAnimEncoder* animEnc = WebPAnimEncoderNew(outSize.width, outSize.height, &animOpts);
    if (!animEnc) {
        return false;
    }

    WebPConfig webpCfg;
    if (!WebPConfigInit(&webpCfg)) {
        WebPAnimEncoderDelete(animEnc);
        return false;
    }
    webpCfg.quality = (opts.quality == StudioExportQuality::High)
        ? 88.0f
        : ((opts.quality == StudioExportQuality::Compact) ? 62.0f : 76.0f);
    webpCfg.method = 2; // 高速帧间差分与体积甜点平衡
    webpCfg.thread_level = 1;

    int lastTsMs = 0;
    int frameStepMs = (std::max)(16, static_cast<int>(std::llround(1000.0 / static_cast<double>(outFps))));
    bool encOk = true;

    const bool streamOk = streamComposedFramesFromSource(
        rawInputPathUtf8,
        timeline,
        config,
        outSize,
        outFps,
        [&](const cv::Mat& composedBgr, int64_t frameIdx, double totalEst) -> bool {
            WebPPicture pic;
            if (!WebPPictureInit(&pic)) return false;
            pic.use_argb = 1;
            pic.width = outSize.width;
            pic.height = outSize.height;
            if (!WebPPictureImportBGR(&pic, composedBgr.data, static_cast<int>(composedBgr.step[0]))) {
                WebPPictureFree(&pic);
                return false;
            }
            const int tsMs = static_cast<int>(std::llround(
                (static_cast<double>(frameIdx) * 1000.0) / static_cast<double>(outFps)));
            lastTsMs = tsMs;
            const int addRet = WebPAnimEncoderAdd(animEnc, &pic, tsMs, &webpCfg);
            WebPPictureFree(&pic);
            if (!addRet) {
                encOk = false;
                return false;
            }
            if (onProgress) {
                onProgress(std::clamp(static_cast<double>(frameIdx + 1) / totalEst, 0.0, 0.96));
            }
            return true;
        },
        nullptr,
        cancelFlag);

    if (!streamOk || !encOk) {
        WebPAnimEncoderDelete(animEnc);
        return false;
    }

    WebPAnimEncoderAdd(animEnc, nullptr, lastTsMs + frameStepMs, nullptr);
    WebPData webpData;
    WebPDataInit(&webpData);
    const int asmOk = WebPAnimEncoderAssemble(animEnc, &webpData);
    WebPAnimEncoderDelete(animEnc);
    if (!asmOk || !webpData.bytes || webpData.size == 0) {
        WebPDataClear(&webpData);
        return false;
    }

    const std::wstring tempWide = finalWide + L".studio_tmp.webp";
    bool wroteFile = false;
    {
        std::ofstream ofs(tempWide, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(reinterpret_cast<const char*>(webpData.bytes), static_cast<std::streamsize>(webpData.size));
            ofs.flush();
            wroteFile = static_cast<bool>(ofs);
        }
    }
    WebPDataClear(&webpData);

    std::error_code ec;
    if (!wroteFile) {
        std::filesystem::remove(tempWide, ec);
        return false;
    }
    if (!MoveFileExW(tempWide.c_str(), finalWide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tempWide, ec);
        return false;
    }
    return true;
}

bool dispatchStudioExportOnCleanMta(
    const std::string& rawInputPathUtf8,
    const std::string& finalOutputPathUtf8,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    const StudioExportOptions& exportOpts,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag) {
    struct ScopedComMta {
        HRESULT hr = E_FAIL;
        ScopedComMta() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
        ~ScopedComMta() {
            if (SUCCEEDED(hr)) {
                CoUninitialize();
            }
        }
    } comMtaGuard;

    const std::wstring finalWide = tools3000::core::WinUtils::utf8ToWstring(finalOutputPathUtf8);
    if (finalWide.empty()) return false;

    std::error_code dirEc;
    const auto parentDir = std::filesystem::path(finalWide).parent_path();
    if (!parentDir.empty()) {
        std::filesystem::create_directories(parentDir, dirEc);
    }

    bool ok = false;
    switch (exportOpts.format) {
        case StudioExportFormat::GIF_WeChat:
        case StudioExportFormat::GIF_HD:
            ok = exportAnimatedGifOnMta(rawInputPathUtf8, finalWide, timeline, config, exportOpts, onProgress, cancelFlag);
            break;
        case StudioExportFormat::WebP_Animated:
            ok = exportAnimatedWebPOnMta(rawInputPathUtf8, finalWide, timeline, config, exportOpts, onProgress, cancelFlag);
            break;
        case StudioExportFormat::MP4_H264:
        case StudioExportFormat::MP4_HEVC:
        case StudioExportFormat::WebM_VP9:
        default:
            ok = exportContainerVideoOnMta(rawInputPathUtf8, finalWide, timeline, config, exportOpts, onProgress, cancelFlag);
            break;
    }

    if (ok) {
        if (config.exportSrtSidecar && config.autoCaptionsEnabled && !timeline.captions.empty() &&
            (exportOpts.format == StudioExportFormat::MP4_H264 ||
             exportOpts.format == StudioExportFormat::MP4_HEVC ||
             exportOpts.format == StudioExportFormat::WebM_VP9)) {
            std::filesystem::path srtPath(finalWide);
            srtPath.replace_extension(L".srt");
            StudioTimelineAnalyzer::exportSrtFile(srtPath.wstring(), timeline.captions, timeline.cutSegments);
        }
        if (onProgress) onProgress(1.0);
    }
    return ok;
}


} // namespace

bool StudioVideoCompositor::extractPreviewDeck(
    const std::string& rawInputPathUtf8,
    int maxPreviewWidth,
    int maxDeckFrames,
    std::vector<cv::Mat>& outFramesBgr,
    int& outOrigWidth,
    int& outOrigHeight,
    double& outDurationSec) {
    outFramesBgr.clear();
    outOrigWidth = 0;
    outOrigHeight = 0;
    outDurationSec = 0.0;

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, rawInputPathUtf8.c_str(), nullptr, nullptr) < 0 || !fmtCtx) {
        return false;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    int videoIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVStream* vStream = fmtCtx->streams[videoIdx];
    const AVCodec* decCodec = avcodec_find_decoder(vStream->codecpar->codec_id);
    if (!decCodec) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVCodecContext* decCtx = avcodec_alloc_context3(decCodec);
    if (decCtx) {
        decCtx->thread_count = 0;
    }
    if (!decCtx ||
        avcodec_parameters_to_context(decCtx, vStream->codecpar) < 0 ||
        avcodec_open2(decCtx, decCodec, nullptr) < 0) {
        if (decCtx) avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    outOrigWidth = decCtx->width;
    outOrigHeight = decCtx->height;
    if (outOrigWidth <= 0 || outOrigHeight <= 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    if (fmtCtx->duration > 0) {
        outDurationSec = static_cast<double>(fmtCtx->duration) / static_cast<double>(AV_TIME_BASE);
    } else if (vStream->duration > 0 && vStream->time_base.den > 0) {
        outDurationSec = static_cast<double>(vStream->duration) * av_q2d(vStream->time_base);
    }

    const int safeMaxW = std::clamp(maxPreviewWidth, 160, 2560);
    const double scale = (outOrigWidth > safeMaxW)
        ? (static_cast<double>(safeMaxW) / static_cast<double>(outOrigWidth))
        : 1.0;
    const int dstW = (std::max)(32, static_cast<int>(std::round(outOrigWidth * scale)) & ~1);
    const int dstH = (std::max)(32, static_cast<int>(std::round(outOrigHeight * scale)) & ~1);

    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    const int targetDeck = std::clamp(maxDeckFrames, 8, 360);
    const double streamTbSec = (vStream->time_base.den > 0) ? av_q2d(vStream->time_base) : (1.0 / 30.0);
    const double targetIntervalSec = (outDurationSec > 0.2)
        ? (outDurationSec / static_cast<double>(targetDeck))
        : (1.0 / 15.0);

    std::vector<std::pair<double, cv::Mat>> timedDeck;
    timedDeck.reserve(static_cast<size_t>(targetDeck) + 16);
    int64_t firstPts = AV_NOPTS_VALUE;
    double maxObservedSec = 0.0;
    double nextCaptureSec = 0.0;

    auto harvestDecoded = [&]() {
        while (avcodec_receive_frame(decCtx, frame) >= 0) {
            int64_t rawPts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? frame->best_effort_timestamp
                : frame->pts;
            double frameSec = maxObservedSec;
            if (rawPts != AV_NOPTS_VALUE) {
                if (firstPts == AV_NOPTS_VALUE) firstPts = rawPts;
                frameSec = (std::max)(0.0, static_cast<double>(rawPts - firstPts) * streamTbSec);
                maxObservedSec = (std::max)(maxObservedSec, frameSec);
            } else {
                maxObservedSec += (1.0 / 30.0);
                frameSec = maxObservedSec;
            }

            if (!sws) {
                const int swsFlags = (dstW < frame->width || dstH < frame->height) ? SWS_AREA : SWS_BILINEAR;
                sws = sws_getContext(
                    frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                    dstW, dstH, AV_PIX_FMT_BGR24,
                    swsFlags, nullptr, nullptr, nullptr);
            }
            if (!sws) continue;

            if (timedDeck.empty() || frameSec >= nextCaptureSec) {
                cv::Mat bgr(dstH, dstW, CV_8UC3);
                uint8_t* dstData[4] = {bgr.data, nullptr, nullptr, nullptr};
                int dstLinesize[4] = {static_cast<int>(bgr.step[0]), 0, 0, 0};
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
                timedDeck.emplace_back(frameSec, std::move(bgr));
                nextCaptureSec = frameSec + targetIntervalSec * 0.92;
            }
        }
    };

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == videoIdx) {
            if (avcodec_send_packet(decCtx, pkt) >= 0) {
                harvestDecoded();
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(decCtx, nullptr);
    harvestDecoded();

    if (outDurationSec <= 0.05) {
        outDurationSec = (std::max)(0.1, maxObservedSec + (1.0 / 30.0));
    }

    if (!timedDeck.empty()) {
        const size_t resampleCount = (std::min)(
            static_cast<size_t>(targetDeck),
            (std::max)(timedDeck.size(), static_cast<size_t>(std::ceil(outDurationSec * 30.0))));
        outFramesBgr.reserve(resampleCount);
        size_t cursor = 0;
        for (size_t i = 0; i < resampleCount; ++i) {
            const double t = (resampleCount > 1)
                ? (outDurationSec * static_cast<double>(i) / static_cast<double>(resampleCount - 1))
                : 0.0;
            while (cursor + 1 < timedDeck.size() && timedDeck[cursor + 1].first <= t + 1e-4) {
                ++cursor;
            }
            outFramesBgr.push_back(timedDeck[cursor].second);
        }
    }

    if (sws) sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);
    return !outFramesBgr.empty();
}

size_t StudioVideoCompositor::compressPreviewDeck(
    const std::vector<cv::Mat>& rawDeckBgr,
    std::vector<std::vector<uint8_t>>& outJpegDeck,
    std::vector<cv::Mat>& outVisualDiffGrayDeck,
    int& outProxyWidth,
    int& outProxyHeight,
    int maxLongSide,
    int maxFrames,
    int jpegQuality) {
    outJpegDeck.clear();
    outVisualDiffGrayDeck.clear();
    outProxyWidth = 0;
    outProxyHeight = 0;
    if (rawDeckBgr.empty()) {
        return 0;
    }

    const int safeLongSide = std::clamp(maxLongSide, 160, 1280);
    const size_t safeMaxFrames = static_cast<size_t>(std::clamp(maxFrames, 8, 224));
    const int safeQuality = std::clamp(jpegQuality, 45, 95);
    const std::vector<int> encodeParams = {cv::IMWRITE_JPEG_QUALITY, safeQuality};

    const size_t srcCount = rawDeckBgr.size();
    const size_t targetCount = (std::min)(srcCount, safeMaxFrames);
    outJpegDeck.reserve(targetCount);
    outVisualDiffGrayDeck.reserve(targetCount);

    size_t totalJpegBytes = 0;
    cv::Mat bgrConverted;
    cv::Mat scaledBgr;
    cv::Mat probeBgr;

    for (size_t i = 0; i < targetCount; ++i) {
        const size_t srcIdx = (targetCount <= 1 || srcCount <= safeMaxFrames)
            ? i
            : ((i * (srcCount - 1) + (targetCount - 1) / 2) / (targetCount - 1));
        const cv::Mat& rawFrame = rawDeckBgr[srcIdx];
        if (rawFrame.empty()) {
            continue;
        }

        const cv::Mat* bgrPtr = &rawFrame;
        if (rawFrame.channels() == 4) {
            cv::cvtColor(rawFrame, bgrConverted, cv::COLOR_BGRA2BGR);
            bgrPtr = &bgrConverted;
        } else if (rawFrame.channels() == 1) {
            cv::cvtColor(rawFrame, bgrConverted, cv::COLOR_GRAY2BGR);
            bgrPtr = &bgrConverted;
        } else if (rawFrame.channels() != 3) {
            continue;
        }

        const int origW = bgrPtr->cols;
        const int origH = bgrPtr->rows;
        const int longSide = (std::max)(origW, origH);
        const cv::Mat* proxyPtr = bgrPtr;
        if (longSide > safeLongSide) {
            const double scale = static_cast<double>(safeLongSide) / static_cast<double>(longSide);
            const int dstW = (std::max)(2, static_cast<int>(std::round(origW * scale)) & ~1);
            const int dstH = (std::max)(2, static_cast<int>(std::round(origH * scale)) & ~1);
            cv::resize(*bgrPtr, scaledBgr, cv::Size(dstW, dstH), 0.0, 0.0, cv::INTER_AREA);
            proxyPtr = &scaledBgr;
        }

        std::vector<uint8_t> jpegBuf;
        if (!cv::imencode(".jpg", *proxyPtr, jpegBuf, encodeParams) || jpegBuf.empty()) {
            continue;
        }
        if (outProxyWidth <= 0) {
            outProxyWidth = proxyPtr->cols;
            outProxyHeight = proxyPtr->rows;
        }

        cv::resize(
            *proxyPtr,
            probeBgr,
            cv::Size(kStudioVisualDiffProbeWidth, kStudioVisualDiffProbeHeight),
            0.0, 0.0, cv::INTER_AREA);
        cv::Mat probeGray;
        cv::cvtColor(probeBgr, probeGray, cv::COLOR_BGR2GRAY);

        totalJpegBytes += jpegBuf.size();
        outJpegDeck.push_back(std::move(jpegBuf));
        outVisualDiffGrayDeck.push_back(std::move(probeGray));
    }
    return totalJpegBytes;
}

bool StudioVideoCompositor::extractAudioWaveformPeaks(
    const std::string& rawInputPathUtf8,
    size_t bucketCount,
    std::vector<float>& outPeaks) {
    outPeaks.clear();
    if (rawInputPathUtf8.empty() || bucketCount == 0) return false;

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, rawInputPathUtf8.c_str(), nullptr, nullptr) < 0 || !fmtCtx) {
        return false;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    const AVCodec* aCodec = nullptr;
    const int audioIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &aCodec, 0);
    if (audioIdx < 0 || !aCodec) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVStream* aStream = fmtCtx->streams[audioIdx];
    AVCodecContext* aDecCtx = avcodec_alloc_context3(aCodec);
    if (!aDecCtx ||
        avcodec_parameters_to_context(aDecCtx, aStream->codecpar) < 0 ||
        avcodec_open2(aDecCtx, aCodec, nullptr) < 0) {
        if (aDecCtx) avcodec_free_context(&aDecCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    double totalDurSec = 0.0;
    if (fmtCtx->duration > 0) {
        totalDurSec = static_cast<double>(fmtCtx->duration) / static_cast<double>(AV_TIME_BASE);
    } else if (aStream->duration > 0 && aStream->time_base.den > 0) {
        totalDurSec = static_cast<double>(aStream->duration) * av_q2d(aStream->time_base);
    }

    const size_t safeBuckets = std::clamp<size_t>(bucketCount, 32, 4096);
    std::vector<std::pair<double, float>> framePeaks;
    framePeaks.reserve(2048);

    const double tbSec = (aStream->time_base.den > 0)
        ? av_q2d(aStream->time_base)
        : (1.0 / static_cast<double>((std::max)(8000, aDecCtx->sample_rate)));
    const int sampleRate = (std::max)(8000, aDecCtx->sample_rate);
    int64_t firstPts = AV_NOPTS_VALUE;
    double runningSec = 0.0;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto appendSubWindowEnvelopes = [&](const AVFrame* f, double frameStartSec) {
        if (!f || f->nb_samples <= 0) return;
        const int chs = (std::max)(1, f->ch_layout.nb_channels);
        const int n = f->nb_samples;
        const auto fmt = static_cast<AVSampleFormat>(f->format);
        const bool planar = av_sample_fmt_is_planar(fmt) != 0;
        constexpr int kSubWin = 128;

        auto readAbsSample = [&](int sampleIdx, int ch) -> float {
            if (fmt == AV_SAMPLE_FMT_FLT || fmt == AV_SAMPLE_FMT_FLTP) {
                const auto* p = reinterpret_cast<const float*>(planar ? (f->extended_data ? f->extended_data[ch] : f->data[ch]) : f->data[0]);
                return p ? std::abs(p[planar ? sampleIdx : (sampleIdx * chs + ch)]) : 0.0f;
            }
            if (fmt == AV_SAMPLE_FMT_S16 || fmt == AV_SAMPLE_FMT_S16P) {
                constexpr float kInv = 1.0f / 32768.0f;
                const auto* p = reinterpret_cast<const int16_t*>(planar ? (f->extended_data ? f->extended_data[ch] : f->data[ch]) : f->data[0]);
                return p ? std::abs(static_cast<float>(p[planar ? sampleIdx : (sampleIdx * chs + ch)])) * kInv : 0.0f;
            }
            if (fmt == AV_SAMPLE_FMT_S32 || fmt == AV_SAMPLE_FMT_S32P) {
                constexpr float kInv = 1.0f / 2147483648.0f;
                const auto* p = reinterpret_cast<const int32_t*>(planar ? (f->extended_data ? f->extended_data[ch] : f->data[ch]) : f->data[0]);
                return p ? std::abs(static_cast<float>(p[planar ? sampleIdx : (sampleIdx * chs + ch)])) * kInv : 0.0f;
            }
            if (fmt == AV_SAMPLE_FMT_DBL || fmt == AV_SAMPLE_FMT_DBLP) {
                const auto* p = reinterpret_cast<const double*>(planar ? (f->extended_data ? f->extended_data[ch] : f->data[ch]) : f->data[0]);
                return p ? static_cast<float>(std::abs(p[planar ? sampleIdx : (sampleIdx * chs + ch)])) : 0.0f;
            }
            return 0.0f;
        };

        for (int s0 = 0; s0 < n; s0 += kSubWin) {
            const int sEnd = (std::min)(n, s0 + kSubWin);
            float subPeak = 0.0f;
            double sumSq = 0.0;
            int cnt = 0;
            for (int i = s0; i < sEnd; ++i) {
                for (int c = 0; c < chs; ++c) {
                    const float v = readAbsSample(i, c);
                    subPeak = (std::max)(subPeak, v);
                    sumSq += static_cast<double>(v) * static_cast<double>(v);
                    ++cnt;
                }
            }
            const float rms = (cnt > 0) ? static_cast<float>(std::sqrt(sumSq / static_cast<double>(cnt))) : 0.0f;
            const float env = std::clamp(0.82f * rms + 0.18f * (std::min)(1.0f, subPeak), 0.0f, 1.0f);
            const double subMidSec = frameStartSec + (static_cast<double>(s0 + sEnd) * 0.5) / static_cast<double>(sampleRate);
            framePeaks.emplace_back(subMidSec, env);
        }
    };

    auto drainAudio = [&]() {
        while (avcodec_receive_frame(aDecCtx, frame) >= 0) {
            const int64_t rawPts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? frame->best_effort_timestamp
                : frame->pts;
            double tSec = runningSec;
            if (rawPts != AV_NOPTS_VALUE) {
                if (firstPts == AV_NOPTS_VALUE) firstPts = rawPts;
                tSec = (std::max)(0.0, static_cast<double>(rawPts - firstPts) * tbSec);
            }
            const double frameSpanSec = static_cast<double>((std::max)(1, frame->nb_samples)) / static_cast<double>(sampleRate);
            runningSec = (std::max)(runningSec, tSec + frameSpanSec);
            appendSubWindowEnvelopes(frame, tSec);
        }
    };

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == audioIdx) {
            if (avcodec_send_packet(aDecCtx, pkt) >= 0) {
                drainAudio();
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(aDecCtx, nullptr);
    drainAudio();

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&aDecCtx);
    avformat_close_input(&fmtCtx);

    if (framePeaks.empty()) return false;
    const double finalDur = (std::max)(totalDurSec, runningSec);
    if (finalDur <= 1e-4) return false;

    outPeaks.assign(safeBuckets, 0.0f);
    std::vector<uint32_t> bucketCounts(safeBuckets, 0u);
    for (const auto& [tCenterSec, env] : framePeaks) {
        const double u = std::clamp(tCenterSec / finalDur, 0.0, 0.999999);
        const size_t b = std::min<size_t>(safeBuckets - 1, static_cast<size_t>(u * static_cast<double>(safeBuckets)));
        outPeaks[b] += env;
        bucketCounts[b] += 1u;
    }
    for (size_t b = 0; b < safeBuckets; ++b) {
        if (bucketCounts[b] > 0u) {
            outPeaks[b] /= static_cast<float>(bucketCounts[b]);
        } else if (b > 0) {
            outPeaks[b] = outPeaks[b - 1] * 0.85f;
        }
    }
    return true;
}

bool StudioVideoCompositor::exportStudioMedia(
    const std::string& rawInputPathUtf8,
    const std::string& finalOutputPathUtf8,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    const StudioExportOptions& exportOpts,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag) {
    if (rawInputPathUtf8.empty() || finalOutputPathUtf8.empty()) return false;

    APTTYPE aptType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER aptQual = APTTYPEQUALIFIER_NONE;
    const bool onSta = SUCCEEDED(CoGetApartmentType(&aptType, &aptQual)) &&
                       (aptType == APTTYPE_STA || aptType == APTTYPE_MAINSTA);
    if (onSta) {
        auto ok = std::make_shared<bool>(false);
        std::thread mtaWorker([&, ok]() {
            *ok = dispatchStudioExportOnCleanMta(
                rawInputPathUtf8, finalOutputPathUtf8, timeline, config, exportOpts, onProgress, cancelFlag);
        });
        tools3000::core::joinWorkerWhilePumpingSentMessages(mtaWorker, 600000);
        return *ok;
    }

    return dispatchStudioExportOnCleanMta(
        rawInputPathUtf8, finalOutputPathUtf8, timeline, config, exportOpts, onProgress, cancelFlag);
}

bool StudioVideoCompositor::exportRenderedVideo(
    const std::string& rawInputPathUtf8,
    const std::string& finalOutputPathUtf8,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    int bitrateMbps,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag) {
    StudioExportOptions opts{};
    opts.format = StudioExportFormat::MP4_H264;
    opts.resolution = StudioExportResolution::Original;
    opts.fps = timeline.fps > 0 ? timeline.fps : 30;
    opts.quality = StudioExportQuality::Recommended;
    opts.baseBitrateMbps = bitrateMbps;
    return exportStudioMedia(
        rawInputPathUtf8, finalOutputPathUtf8, timeline, config, opts, onProgress, cancelFlag);
}

// ─────────────────────────────────────────────────────────────────────────────
// StudioHiResFrameReader — 演播室驻留式原生全分辨率原片帧解码器 + LRU 缓存
// ─────────────────────────────────────────────────────────────────────────────

struct StudioHiResFrameReader::Impl {
    struct CachedEntry {
        double sec = -1.0;
        uint64_t stamp = 0;
        cv::Mat bgr;
    };

    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext* decCtx = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* bestCandidateFrame = nullptr;

    std::string openedPath;
    int videoIdx = -1;
    int origW = 0;
    int origH = 0;
    int dstW = 0;
    int dstH = 0;
    int swsSrcW = 0;
    int swsSrcH = 0;
    int swsSrcFmt = -1;

    double streamTbSec = 1.0 / 30.0;
    double durationSec = 0.0;
    int64_t firstPts = AV_NOPTS_VALUE;
    double lastDecodedSec = -1e9;
    bool freshAtStreamStart = true;
    cv::Mat lastDecodedBgr;
    double lastDecodedBgrSec = -1e9;

    uint64_t accessCounter = 0;
    size_t cachedBytes = 0;
    static constexpr size_t kMaxCacheBytes = StudioHiResFrameReader::kMaxLruCacheBytes;
    static constexpr size_t kMaxCacheEntries = StudioHiResFrameReader::kMaxLruCacheEntries;
    std::vector<CachedEntry> cache;

    ~Impl() {
        reset();
    }

    void reset() noexcept {
        cache.clear();
        cache.shrink_to_fit();
        cachedBytes = 0;
        lastDecodedBgr.release();
        lastDecodedBgrSec = -1e9;
        lastDecodedSec = -1e9;
        freshAtStreamStart = true;
        firstPts = AV_NOPTS_VALUE;
        videoIdx = -1;
        origW = 0;
        origH = 0;
        dstW = 0;
        dstH = 0;
        swsSrcW = 0;
        swsSrcH = 0;
        swsSrcFmt = -1;
        openedPath.clear();

        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        if (bestCandidateFrame) {
            av_frame_free(&bestCandidateFrame);
        }
        if (frame) {
            av_frame_free(&frame);
        }
        if (pkt) {
            av_packet_free(&pkt);
        }
        if (decCtx) {
            avcodec_free_context(&decCtx);
        }
        if (fmtCtx) {
            avformat_close_input(&fmtCtx);
        }
    }

    bool convertAvFrameToBgr(const AVFrame* srcFrame, cv::Mat& outMat) {
        if (!srcFrame || srcFrame->width <= 0 || srcFrame->height <= 0 || dstW <= 0 || dstH <= 0) {
            return false;
        }
        if (!sws || swsSrcW != srcFrame->width || swsSrcH != srcFrame->height || swsSrcFmt != srcFrame->format) {
            if (sws) {
                sws_freeContext(sws);
                sws = nullptr;
            }
            const int flags = (dstW < srcFrame->width || dstH < srcFrame->height) ? SWS_AREA : SWS_BILINEAR;
            sws = sws_getContext(
                srcFrame->width, srcFrame->height, static_cast<AVPixelFormat>(srcFrame->format),
                dstW, dstH, AV_PIX_FMT_BGR24,
                flags, nullptr, nullptr, nullptr);
            swsSrcW = srcFrame->width;
            swsSrcH = srcFrame->height;
            swsSrcFmt = srcFrame->format;
        }
        if (!sws) return false;

        outMat.create(dstH, dstW, CV_8UC3);
        uint8_t* dstData[4] = {outMat.data, nullptr, nullptr, nullptr};
        int dstLinesize[4] = {static_cast<int>(outMat.step[0]), 0, 0, 0};
        return sws_scale(sws, srcFrame->data, srcFrame->linesize, 0, srcFrame->height, dstData, dstLinesize) > 0;
    }

    void insertCache(double sec, const cv::Mat& bgr) {
        if (bgr.empty()) return;
        const size_t frameBytes = bgr.total() * bgr.elemSize();
        if (frameBytes > kMaxCacheBytes) return;
        for (auto& e : cache) {
            if (std::abs(e.sec - sec) <= 0.012) {
                const size_t oldBytes = e.bgr.total() * e.bgr.elemSize();
                cachedBytes = (cachedBytes >= oldBytes) ? (cachedBytes - oldBytes) : 0;
                e.stamp = ++accessCounter;
                e.bgr = bgr;
                cachedBytes += frameBytes;
                return;
            }
        }
        while (!cache.empty() &&
               (cache.size() >= kMaxCacheEntries || (cachedBytes + frameBytes > kMaxCacheBytes))) {
            auto oldestIt = std::min_element(
                cache.begin(), cache.end(),
                [](const CachedEntry& a, const CachedEntry& b) { return a.stamp < b.stamp; });
            const size_t oldBytes = oldestIt->bgr.total() * oldestIt->bgr.elemSize();
            cachedBytes = (cachedBytes >= oldBytes) ? (cachedBytes - oldBytes) : 0;
            cache.erase(oldestIt);
        }
        cache.push_back(CachedEntry{sec, ++accessCounter, bgr});
        cachedBytes += frameBytes;
    }

    bool findInLruCache(double clampedSec, double toleranceSec, cv::Mat& outBgr) noexcept {
        if (cache.empty()) return false;
        double bestDiff = toleranceSec + 1.0;
        CachedEntry* bestEntry = nullptr;
        for (auto& entry : cache) {
            const double d = std::abs(entry.sec - clampedSec);
            if (d <= toleranceSec && d < bestDiff && !entry.bgr.empty()) {
                bestDiff = d;
                bestEntry = &entry;
            }
        }
        if (!bestEntry) return false;
        bestEntry->stamp = ++accessCounter;
        outBgr = bestEntry->bgr;
        return true;
    }
};

StudioHiResFrameReader::StudioHiResFrameReader()
    : m_impl(std::make_unique<Impl>()) {}

StudioHiResFrameReader::~StudioHiResFrameReader() = default;

void StudioHiResFrameReader::close() noexcept {
    if (m_impl) {
        m_impl->reset();
    }
}

bool StudioHiResFrameReader::isOpen() const noexcept {
    return m_impl && m_impl->fmtCtx != nullptr && m_impl->decCtx != nullptr && m_impl->videoIdx >= 0;
}

int StudioHiResFrameReader::nativeWidth() const noexcept {
    return m_impl ? m_impl->dstW : 0;
}

int StudioHiResFrameReader::nativeHeight() const noexcept {
    return m_impl ? m_impl->dstH : 0;
}

size_t StudioHiResFrameReader::cachedFrameCount() const noexcept {
    return m_impl ? m_impl->cache.size() : 0;
}

size_t StudioHiResFrameReader::cachedMemoryBytes() const noexcept {
    return m_impl ? m_impl->cachedBytes : 0;
}

bool StudioHiResFrameReader::open(const std::string& videoPathUtf8, int maxDecodeLongSide) {
    if (!m_impl) return false;
    if (videoPathUtf8.empty()) {
        m_impl->reset();
        return false;
    }
    if (isOpen() && m_impl->openedPath == videoPathUtf8) {
        return true;
    }

    m_impl->reset();

    if (avformat_open_input(&m_impl->fmtCtx, videoPathUtf8.c_str(), nullptr, nullptr) < 0 || !m_impl->fmtCtx) {
        m_impl->reset();
        return false;
    }
    if (avformat_find_stream_info(m_impl->fmtCtx, nullptr) < 0) {
        m_impl->reset();
        return false;
    }

    for (unsigned i = 0; i < m_impl->fmtCtx->nb_streams; ++i) {
        if (m_impl->fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_impl->videoIdx = static_cast<int>(i);
            break;
        }
    }
    if (m_impl->videoIdx < 0) {
        m_impl->reset();
        return false;
    }

    AVStream* vStream = m_impl->fmtCtx->streams[m_impl->videoIdx];
    const AVCodec* decCodec = avcodec_find_decoder(vStream->codecpar->codec_id);
    if (!decCodec) {
        m_impl->reset();
        return false;
    }

    m_impl->decCtx = avcodec_alloc_context3(decCodec);
    if (!m_impl->decCtx) {
        m_impl->reset();
        return false;
    }
    m_impl->decCtx->thread_count = 4;
    if (avcodec_parameters_to_context(m_impl->decCtx, vStream->codecpar) < 0 ||
        avcodec_open2(m_impl->decCtx, decCodec, nullptr) < 0) {
        m_impl->reset();
        return false;
    }

    m_impl->origW = m_impl->decCtx->width;
    m_impl->origH = m_impl->decCtx->height;
    if (m_impl->origW <= 0 || m_impl->origH <= 0) {
        m_impl->reset();
        return false;
    }

    const int safeCap = std::clamp(maxDecodeLongSide, 640, 3840);
    const int longSide = (std::max)(m_impl->origW, m_impl->origH);
    const double scale = (longSide > safeCap)
        ? (static_cast<double>(safeCap) / static_cast<double>(longSide))
        : 1.0;
    m_impl->dstW = (std::max)(32, static_cast<int>(std::round(m_impl->origW * scale)) & ~1);
    m_impl->dstH = (std::max)(32, static_cast<int>(std::round(m_impl->origH * scale)) & ~1);

    m_impl->streamTbSec = (vStream->time_base.den > 0) ? av_q2d(vStream->time_base) : (1.0 / 30.0);
    if (vStream->start_time != AV_NOPTS_VALUE) {
        m_impl->firstPts = vStream->start_time;
    }
    if (m_impl->fmtCtx->duration > 0) {
        m_impl->durationSec = static_cast<double>(m_impl->fmtCtx->duration) / static_cast<double>(AV_TIME_BASE);
    } else if (vStream->duration > 0 && vStream->time_base.den > 0) {
        m_impl->durationSec = static_cast<double>(vStream->duration) * av_q2d(vStream->time_base);
    }

    m_impl->pkt = av_packet_alloc();
    m_impl->frame = av_frame_alloc();
    m_impl->bestCandidateFrame = av_frame_alloc();
    if (!m_impl->pkt || !m_impl->frame || !m_impl->bestCandidateFrame) {
        m_impl->reset();
        return false;
    }

    m_impl->openedPath = videoPathUtf8;
    return true;
}

bool StudioHiResFrameReader::tryGetCachedFrame(double targetSec, cv::Mat& outBgr, double toleranceSec) noexcept {
    if (!m_impl) return false;
    const double clampedSec = (std::max)(0.0, targetSec);
    if (m_impl->findInLruCache(clampedSec, toleranceSec, outBgr)) {
        return true;
    }
    // 兜底命中顺序播放流式解码产出的最近单帧（保证随时暂停调参/掠览 0ms 零重解码）
    if (!m_impl->lastDecodedBgr.empty() &&
        m_impl->lastDecodedBgrSec >= -1e-4 &&
        std::abs(m_impl->lastDecodedBgrSec - clampedSec) <= toleranceSec) {
        outBgr = m_impl->lastDecodedBgr;
        return true;
    }
    return false;
}

bool StudioHiResFrameReader::decodeFrameAt(double targetSec, cv::Mat& outBgr, bool cacheInLru) {
    if (!isOpen()) return false;

    double clampedSec = (std::max)(0.0, targetSec);
    if (m_impl->durationSec > 0.05) {
        clampedSec = (std::min)(clampedSec, (std::max)(0.0, m_impl->durationSec - 0.01));
    }

    if (m_impl->findInLruCache(clampedSec, 0.018, outBgr)) {
        return true;
    }
    if (!m_impl->lastDecodedBgr.empty() &&
        m_impl->lastDecodedBgrSec >= -1e-4 &&
        std::abs(m_impl->lastDecodedBgrSec - clampedSec) <= 0.018) {
        if (cacheInLru) {
            m_impl->insertCache(m_impl->lastDecodedBgrSec, m_impl->lastDecodedBgr);
        }
        outBgr = m_impl->lastDecodedBgr;
        return true;
    }

    const bool canRollForward =
        (m_impl->freshAtStreamStart && clampedSec <= 0.65) ||
        ((m_impl->lastDecodedSec >= -1e-4) &&
         (clampedSec >= m_impl->lastDecodedSec - 0.018) &&
         (clampedSec <= m_impl->lastDecodedSec + 0.65));

    if (!m_impl->freshAtStreamStart && canRollForward &&
        clampedSec <= m_impl->lastDecodedSec + 0.016 && !m_impl->lastDecodedBgr.empty()) {
        if (cacheInLru && m_impl->lastDecodedBgrSec >= -1e-4) {
            m_impl->insertCache(m_impl->lastDecodedBgrSec, m_impl->lastDecodedBgr);
        }
        outBgr = m_impl->lastDecodedBgr;
        return true;
    }

    if (!canRollForward) {
        const int64_t basePts = (m_impl->firstPts != AV_NOPTS_VALUE) ? m_impl->firstPts : 0;
        const double seekSec = (std::max)(0.0, clampedSec - 0.04);
        const int64_t seekTs = basePts + static_cast<int64_t>(std::llround(seekSec / (std::max)(1e-6, m_impl->streamTbSec)));
        av_seek_frame(m_impl->fmtCtx, m_impl->videoIdx, seekTs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(m_impl->decCtx);
        m_impl->lastDecodedSec = -1e9;
    }
    m_impl->freshAtStreamStart = false;

    bool hasCandidate = false;
    double candidateSec = 0.0;
    int decodedCount = 0;
    constexpr int kMaxDecodeSteps = 210;
    bool reachedTarget = false;

    auto processDecodedFrames = [&]() {
        while (!reachedTarget && avcodec_receive_frame(m_impl->decCtx, m_impl->frame) >= 0) {
            ++decodedCount;
            const int64_t rawPts = (m_impl->frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? m_impl->frame->best_effort_timestamp
                : m_impl->frame->pts;
            double fSec = (m_impl->lastDecodedSec >= 0.0) ? (m_impl->lastDecodedSec + (1.0 / 30.0)) : 0.0;
            if (rawPts != AV_NOPTS_VALUE) {
                if (m_impl->firstPts == AV_NOPTS_VALUE) {
                    m_impl->firstPts = rawPts;
                }
                fSec = (std::max)(0.0, static_cast<double>(rawPts - m_impl->firstPts) * m_impl->streamTbSec);
            }

            av_frame_unref(m_impl->bestCandidateFrame);
            if (av_frame_ref(m_impl->bestCandidateFrame, m_impl->frame) >= 0) {
                hasCandidate = true;
                candidateSec = fSec;
            }
            m_impl->lastDecodedSec = fSec;

            if (fSec >= clampedSec - 0.014 || decodedCount >= kMaxDecodeSteps) {
                reachedTarget = true;
                av_frame_unref(m_impl->frame);
                break;
            }
            av_frame_unref(m_impl->frame);
        }
    };

    // 先消费解码器内部可能残留的已缓冲帧
    processDecodedFrames();

    while (!reachedTarget && decodedCount < kMaxDecodeSteps) {
        const int rd = av_read_frame(m_impl->fmtCtx, m_impl->pkt);
        if (rd < 0) {
            avcodec_send_packet(m_impl->decCtx, nullptr);
            processDecodedFrames();
            break;
        }
        if (m_impl->pkt->stream_index == m_impl->videoIdx) {
            if (avcodec_send_packet(m_impl->decCtx, m_impl->pkt) >= 0) {
                processDecodedFrames();
            }
        }
        av_packet_unref(m_impl->pkt);
    }

    if (!hasCandidate) {
        if (!m_impl->lastDecodedBgr.empty()) {
            outBgr = m_impl->lastDecodedBgr;
            return true;
        }
        return false;
    }

    cv::Mat decodedBgr;
    const bool converted = m_impl->convertAvFrameToBgr(m_impl->bestCandidateFrame, decodedBgr);
    av_frame_unref(m_impl->bestCandidateFrame);
    if (!converted || decodedBgr.empty()) {
        return false;
    }

    m_impl->lastDecodedBgr = decodedBgr;
    m_impl->lastDecodedBgrSec = candidateSec;
    if (cacheInLru) {
        m_impl->insertCache(candidateSec, decodedBgr);
    }
    outBgr = std::move(decodedBgr);
    return true;
}

} // namespace tools3000::capture

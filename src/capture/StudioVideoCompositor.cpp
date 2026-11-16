#include "capture/StudioVideoCompositor.h"
#include "capture/ScreenRecorder.h"
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

namespace tools3000::capture {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t kWeChatGifSafeMaxBytes = 4'850'000ULL; // ~4.62 MiB (< 5 MB 微信内联自动播放红线)
constexpr int kWeChatGifMaxDimension = 800;
constexpr int kWeChatGifMaxFrames = 260;

struct MeshLightOrb {
    float nx;      // 0..1
    float ny;      // 0..1
    float radius;  // 归一化半径
    cv::Vec3f bgr; // BGR 0..255
    float weight;
};

struct MeshBackdropSpec {
    cv::Vec3f baseBgr;
    std::array<MeshLightOrb, 3> orbs;
    float vignetteStrength = 0.08f;
};

MeshBackdropSpec meshSpecForTheme(BeautyBackgroundType theme) noexcept {
    switch (theme) {
        case BeautyBackgroundType::MidnightGraphite: // [沉稳] 深海曜石 (Nordic Obsidian)
            return {
                {32.0f, 22.0f, 15.0f}, // #0F1620
                {{
                    {0.18f, 0.20f, 0.68f, {92.0f, 64.0f, 38.0f}, 0.72f},   // 冷雾钢蓝柔光
                    {0.84f, 0.78f, 0.72f, {68.0f, 42.0f, 48.0f}, 0.60f},   // 暗夜靛紫微光
                    {0.52f, 0.12f, 0.55f, {118.0f, 88.0f, 52.0f}, 0.36f},  // 顶部天光漫射
                }},
                0.14f
            };
        case BeautyBackgroundType::SilkMist: // [沉稳] 松烟墨翠 (Pine & Emerald)
            return {
                {29.0f, 33.0f, 16.0f}, // #10211D
                {{
                    {0.20f, 0.24f, 0.72f, {68.0f, 82.0f, 36.0f}, 0.75f},   // 冷杉深翠
                    {0.82f, 0.76f, 0.68f, {58.0f, 62.0f, 26.0f}, 0.65f},   // 远林墨青
                    {0.72f, 0.18f, 0.52f, {84.0f, 112.0f, 64.0f}, 0.38f},  // 薄雾苔光
                }},
                0.14f
            };
        case BeautyBackgroundType::PureMinimal: // [轻盈] 晨雾云母 (Airy Cloud)
            return {
                {250.0f, 245.0f, 241.0f}, // #F1F5FA
                {{
                    {0.16f, 0.22f, 0.74f, {254.0f, 232.0f, 214.0f}, 0.85f}, // 晴空冰蓝 #D6E8FE
                    {0.84f, 0.24f, 0.68f, {243.0f, 228.0f, 250.0f}, 0.78f}, // 樱花淡雾 #FAE4F3
                    {0.50f, 0.86f, 0.76f, {252.0f, 234.0f, 226.0f}, 0.72f}, // 薰衣草浅云 #E2EAFC
                }},
                0.03f
            };
        case BeautyBackgroundType::PaperChalk: // [轻盈] 暖杏羊绒 (Warm Cashmere)
            return {
                {236.0f, 243.0f, 249.0f}, // #F9F3EC
                {{
                    {0.20f, 0.22f, 0.75f, {212.0f, 232.0f, 253.0f}, 0.82f}, // 暖杏晨光 #FDE8D4
                    {0.82f, 0.74f, 0.72f, {218.0f, 216.0f, 246.0f}, 0.74f}, // 蜜桃柔雾 #F6D8DA
                    {0.58f, 0.18f, 0.58f, {232.0f, 246.0f, 254.0f}, 0.65f}, // 羊绒高光 #FEF6E8
                }},
                0.03f
            };
        case BeautyBackgroundType::AiryGlacier: // [轻盈] 晴空薄荷 (Glacier Mint)
            return {
                {248.0f, 250.0f, 240.0f}, // #F0FAF8
                {{
                    {0.18f, 0.25f, 0.74f, {242.0f, 249.0f, 214.0f}, 0.84f}, // 薄荷清雾 #D6F9F2
                    {0.82f, 0.22f, 0.70f, {254.0f, 236.0f, 212.0f}, 0.80f}, // 冰川天青 #D4ECFE
                    {0.52f, 0.85f, 0.72f, {246.0f, 242.0f, 228.0f}, 0.68f}, // 云白柔焦
                }},
                0.03f
            };
        case BeautyBackgroundType::StudioSlate:
        default: // [沉稳] 暮夜星云 (Velvet Dusk — 低饱和丝绒深靛，沉稳高级不刺眼)
            return {
                {46.0f, 27.0f, 24.0f}, // #181B2E
                {{
                    {0.18f, 0.22f, 0.72f, {126.0f, 68.0f, 58.0f}, 0.78f},  // 皇家深靛蓝 #3A447E
                    {0.84f, 0.76f, 0.70f, {108.0f, 54.0f, 88.0f}, 0.72f},  // 暮色桑椹紫 #58366C
                    {0.56f, 0.16f, 0.54f, {142.0f, 92.0f, 76.0f}, 0.44f},  // 星云柔光
                }},
                0.12f
            };
    }
}

float ridgeHeightCurve(float nx, float seedPhase, float baseY, float amp) noexcept {
    // 多频平滑余弦山脊合成，生成自然柔和的连绵远山轮廓
    const float w1 = std::sin(nx * 4.2f + seedPhase) * 0.52f;
    const float w2 = std::sin(nx * 9.6f - seedPhase * 1.7f) * 0.28f;
    const float w3 = std::cos(nx * 17.4f + seedPhase * 2.3f) * 0.13f;
    const float w4 = std::sin(nx * 31.0f + seedPhase * 0.8f) * 0.05f;
    return baseY - amp * (w1 + w2 + w3 + w4);
}

void renderLandscapeBackdropLowRes(cv::Mat& gridF32, BeautyBackgroundType theme) {
    const int gw = gridF32.cols;
    const int gh = gridF32.rows;
    const bool isDusk = (theme == BeautyBackgroundType::DuskMountains);

    // 天空三段柔和渐变 + 落日/破晓柔焦天光球
    const cv::Vec3f skyTop    = isDusk ? cv::Vec3f(78.0f, 36.0f, 34.0f)    : cv::Vec3f(198.0f, 146.0f, 88.0f);  // 暮紫蓝 / 拂晓青蓝
    const cv::Vec3f skyMid    = isDusk ? cv::Vec3f(116.0f, 68.0f, 118.0f)  : cv::Vec3f(218.0f, 196.0f, 158.0f); // 晚霞紫玫瑰 / 晨雾浅青
    const cv::Vec3f skyHoriz  = isDusk ? cv::Vec3f(118.0f, 156.0f, 238.0f) : cv::Vec3f(186.0f, 218.0f, 246.0f); // 暖金落日 / 暖白朝霞
    const cv::Vec3f sunGlow   = isDusk ? cv::Vec3f(142.0f, 192.0f, 252.0f) : cv::Vec3f(212.0f, 238.0f, 255.0f);
    const float sunX = isDusk ? 0.68f : 0.34f;
    const float sunY = isDusk ? 0.44f : 0.40f;

    struct RidgeLayer {
        float seed;
        float baseY;
        float amp;
        cv::Vec3f ridgeColor;
        cv::Vec3f mistColor;
        float mistFade;
    };

    const std::array<RidgeLayer, 4> layers = isDusk
        ? std::array<RidgeLayer, 4>{{
            {0.6f, 0.50f, 0.085f, {108.0f, 72.0f, 106.0f}, {116.0f, 112.0f, 168.0f}, 0.48f},
            {2.1f, 0.61f, 0.095f, {84.0f,  50.0f, 74.0f},  {96.0f,  76.0f,  118.0f}, 0.38f},
            {3.9f, 0.73f, 0.080f, {58.0f,  34.0f, 46.0f},  {68.0f,  46.0f,  66.0f},  0.26f},
            {5.4f, 0.86f, 0.065f, {36.0f,  21.0f, 24.0f},  {42.0f,  26.0f,  32.0f},  0.14f},
        }}
        : std::array<RidgeLayer, 4>{{
            {1.2f, 0.48f, 0.075f, {182.0f, 162.0f, 134.0f}, {204.0f, 204.0f, 198.0f}, 0.52f},
            {2.8f, 0.58f, 0.085f, {148.0f, 126.0f, 96.0f},  {178.0f, 168.0f, 148.0f}, 0.40f},
            {4.5f, 0.69f, 0.070f, {108.0f, 88.0f,  62.0f},  {136.0f, 120.0f, 96.0f},  0.28f},
            {6.0f, 0.81f, 0.045f, {72.0f,  58.0f,  38.0f},  {92.0f,  78.0f,  56.0f},  0.18f},
        }};

    std::array<std::vector<float>, 4> ridgeY;
    for (size_t li = 0; li < layers.size(); ++li) {
        ridgeY[li].resize(static_cast<size_t>(gw));
        for (int x = 0; x < gw; ++x) {
            const float nx = static_cast<float>(x) / static_cast<float>((std::max)(1, gw - 1));
            ridgeY[li][static_cast<size_t>(x)] = ridgeHeightCurve(nx, layers[li].seed, layers[li].baseY, layers[li].amp);
        }
    }

    for (int y = 0; y < gh; ++y) {
        const float ny = static_cast<float>(y) / static_cast<float>((std::max)(1, gh - 1));
        auto* row = gridF32.ptr<cv::Vec3f>(y);
        for (int x = 0; x < gw; ++x) {
            const float nx = static_cast<float>(x) / static_cast<float>((std::max)(1, gw - 1));

            // 1. 天空与晨昏太阳柔光
            cv::Vec3f col;
            if (ny < 0.36f) {
                const float k = std::clamp(ny / 0.36f, 0.0f, 1.0f);
                col = skyTop * (1.0f - k) + skyMid * k;
            } else {
                const float k = std::clamp((ny - 0.36f) / 0.38f, 0.0f, 1.0f);
                col = skyMid * (1.0f - k) + skyHoriz * k;
            }
            const float dSun2 = (nx - sunX) * (nx - sunX) + (ny - sunY) * (ny - sunY) * 1.6f;
            const float glow = std::exp(-dSun2 / 0.11f) * 0.52f;
            col = col * (1.0f - glow) + sunGlow * glow;

            // 2. 远近四层大气透视山峦与山谷晨雾
            for (size_t li = 0; li < layers.size(); ++li) {
                const float ry = ridgeY[li][static_cast<size_t>(x)];
                const float softEdge = 0.012f;
                if (ny >= ry - softEdge) {
                    const float edgeAlpha = std::clamp((ny - (ry - softEdge)) / (2.0f * softEdge), 0.0f, 1.0f);
                    const float depthBelow = std::clamp((ny - ry) / 0.18f, 0.0f, 1.0f);
                    const float mistMix = depthBelow * layers[li].mistFade;
                    const cv::Vec3f layerCol =
                        layers[li].ridgeColor * (1.0f - mistMix) + layers[li].mistColor * mistMix;
                    col = col * (1.0f - edgeAlpha) + layerCol * edgeAlpha;
                }
            }

            // 3. 拂晓湖山底部静谧湖水倒影波光
            if (!isDusk && ny > 0.80f) {
                const float lakeK = std::clamp((ny - 0.80f) / 0.04f, 0.0f, 1.0f);
                const float shimmer = 0.5f + 0.5f * std::sin(ny * 95.0f + nx * 12.0f);
                const float sunRefl = std::exp(-((nx - sunX) * (nx - sunX)) / 0.05f) * (0.28f + 0.16f * shimmer);
                cv::Vec3f lakeCol = cv::Vec3f(96.0f, 78.0f, 52.0f) * (1.0f - sunRefl) + sunGlow * sunRefl;
                col = col * (1.0f - lakeK * 0.88f) + lakeCol * (lakeK * 0.88f);
            }

            row[x] = col;
        }
    }
}

void fillRoundedRectMask(cv::Mat& maskU8, cv::Rect rect, float radius) {
    if (rect.width <= 0 || rect.height <= 0) return;
    const int r = static_cast<int>(std::round(
        std::clamp(radius, 0.0f, static_cast<float>((std::min)(rect.width, rect.height)) * 0.5f)));
    if (r <= 1) {
        cv::rectangle(maskU8, rect, cv::Scalar(255), cv::FILLED);
        return;
    }
    cv::rectangle(maskU8, cv::Rect(rect.x + r, rect.y, rect.width - 2 * r, rect.height), cv::Scalar(255), cv::FILLED);
    cv::rectangle(maskU8, cv::Rect(rect.x, rect.y + r, rect.width, rect.height - 2 * r), cv::Scalar(255), cv::FILLED);
    cv::circle(maskU8, cv::Point(rect.x + r, rect.y + r), r, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    cv::circle(maskU8, cv::Point(rect.x + rect.width - r - 1, rect.y + r), r, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    cv::circle(maskU8, cv::Point(rect.x + r, rect.y + rect.height - r - 1), r, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    cv::circle(maskU8, cv::Point(rect.x + rect.width - r - 1, rect.y + rect.height - r - 1), r, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
}

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

void alphaBlendPremultipliedBgraOntoBgr(const cv::Mat& srcBgra, cv::Mat& dstBgr, int dstX, int dstY) {
    if (srcBgra.empty() || srcBgra.type() != CV_8UC4 || dstBgr.empty() || dstBgr.type() != CV_8UC3) return;
    const int x0 = (std::max)(0, dstX);
    const int y0 = (std::max)(0, dstY);
    const int x1 = (std::min)(dstBgr.cols, dstX + srcBgra.cols);
    const int y1 = (std::min)(dstBgr.rows, dstY + srcBgra.rows);
    if (x1 <= x0 || y1 <= y0) return;

    for (int y = y0; y < y1; ++y) {
        const uint8_t* sRow = srcBgra.ptr<uint8_t>(y - dstY) + (x0 - dstX) * 4;
        uint8_t* dRow = dstBgr.ptr<uint8_t>(y) + x0 * 3;
        for (int x = x0; x < x1; ++x) {
            const uint32_t a = sRow[3];
            if (a == 255) {
                dRow[0] = sRow[0];
                dRow[1] = sRow[1];
                dRow[2] = sRow[2];
            } else if (a > 0) {
                const uint32_t invA = 255 - a;
                dRow[0] = static_cast<uint8_t>((std::min<uint32_t>)(255, sRow[0] + (dRow[0] * invA + 127) / 255));
                dRow[1] = static_cast<uint8_t>((std::min<uint32_t>)(255, sRow[1] + (dRow[1] * invA + 127) / 255));
                dRow[2] = static_cast<uint8_t>((std::min<uint32_t>)(255, sRow[2] + (dRow[2] * invA + 127) / 255));
            }
            sRow += 4;
            dRow += 3;
        }
    }
}

int maxLongEdgeForResolution(StudioExportResolution res) noexcept {
    switch (res) {
        case StudioExportResolution::Res2160P: return 3840;
        case StudioExportResolution::Res1440P: return 2560;
        case StudioExportResolution::Res1080P: return 1920;
        case StudioExportResolution::Res720P:  return 1280;
        case StudioExportResolution::Res640P:  return 800;
        case StudioExportResolution::Res480P:  return 600;
        case StudioExportResolution::Original:
        default:                               return 0;
    }
}

double qualityScaleFactor(StudioExportQuality q) noexcept {
    switch (q) {
        case StudioExportQuality::High:    return 1.55;
        case StudioExportQuality::Compact: return 0.58;
        case StudioExportQuality::Recommended:
        default:                           return 1.00;
    }
}

// 统一解码源视频并按真实 PTS 时间戳重采样输出每一帧合成后的 BGR 画布
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

    StudioVideoCompositor compositor;
    SwsContext* toBgrSws = nullptr;
    AVPacket* inPkt = av_packet_alloc();
    AVFrame* decFrame = av_frame_alloc();

    int64_t emittedCount = 0;
    bool ok = true;
    const double progressDenomFrames = (std::max)(1.0, estDurSec * static_cast<double>(safeFps));

    auto emitOne = [&](const cv::Mat& rawBgr) {
        if (!ok || rawBgr.empty()) return;
        const size_t evalIdx = (std::min)(
            static_cast<size_t>(emittedCount),
            evaluatedFrames.empty() ? size_t(0) : (evaluatedFrames.size() - 1));
        const StudioEvaluatedFrame fallbackEval{};
        const StudioEvaluatedFrame& eval = evaluatedFrames.empty() ? fallbackEval : evaluatedFrames[evalIdx];
        cv::Mat composed = compositor.composeFrame(rawBgr, eval, config, outSize);
        if (!onFrame(composed, emittedCount, progressDenomFrames)) {
            ok = false;
            return;
        }
        ++emittedCount;
    };

    cv::Mat heldRawBgr;
    bool hasHeldRaw = false;
    int64_t firstRawPts = AV_NOPTS_VALUE;
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
                       (static_cast<double>(emittedCount) + 0.5) / static_cast<double>(safeFps) < curSec) {
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
            onAudioPacket(inFmt, inAudioIdx, inPkt);
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
                   (emittedCount == 0 ||
                    (static_cast<double>(emittedCount) / static_cast<double>(safeFps)) <
                        finalEndSec - (0.25 / static_cast<double>(safeFps)))) {
                if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                    ok = false;
                    break;
                }
                emitOne(heldRawBgr);
                ++tailFilled;
            }
        }
    }

    if (toBgrSws) sws_freeContext(toBgrSws);
    av_frame_free(&decFrame);
    av_packet_free(&inPkt);
    avcodec_free_context(&decCtx);
    avformat_close_input(&inFmt);
    return ok && emittedCount > 0;
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

    const cv::Size outSize = StudioVideoCompositor::resolveExportDimensions(srcW, srcH, opts);
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
    const double areaRatio = std::clamp(static_cast<double>(areaPx) / (1920.0 * 1080.0), 0.25, 6.0);
    const double fpsRatio = std::clamp(static_cast<double>(outFps) / 30.0, 0.5, 2.2);
    const double qScale = qualityScaleFactor(opts.quality);
    const int64_t baseBps = static_cast<int64_t>(std::clamp(opts.baseBitrateMbps, 2, 80)) * 1'000'000LL;
    const int64_t autoBps = static_cast<int64_t>(std::llround(areaRatio * fpsRatio * qScale * (isHevc ? 6'500'000.0 : 9'000'000.0)));
    const int64_t targetBitrate = std::clamp((std::max)(static_cast<int64_t>(baseBps * qScale), autoBps), 1'500'000LL, 120'000'000LL);

    AVCodecContext* encCtx = nullptr;
    const AVCodec* chosenEnc = nullptr;
    for (const auto* cand : encCandidates) {
        const bool isMf = std::strstr(cand->name, "_mf") != nullptr;
        const int mfPasses = (isMf && !inUnitTestMode) ? 2 : 1;

        for (int mfPass = 0; mfPass < mfPasses; ++mfPass) {
            AVCodecContext* attempt = avcodec_alloc_context3(cand);
            if (!attempt) continue;
            attempt->width = outSize.width;
            attempt->height = outSize.height;
            attempt->time_base = {1, outFps};
            attempt->framerate = {outFps, 1};
            attempt->gop_size = isMf ? 0 : outFps * 2;
            attempt->max_b_frames = 0;
            attempt->bit_rate = targetBitrate;
            attempt->pix_fmt = selectStudioEncoderPixFmt(cand);
            if (outFmt->oformat->flags & AVFMT_GLOBALHEADER) {
                attempt->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            if (isMf) {
                if (!inUnitTestMode && mfPass == 0 && attempt->priv_data) {
                    av_opt_set_int(attempt->priv_data, "hw_encoding", 1, 0);
                }
            } else {
                attempt->thread_count = 0;
            }

            if (std::strcmp(cand->name, "libx264") == 0 && attempt->priv_data) {
                av_opt_set(attempt->priv_data, "preset", areaPx >= (2560LL * 1440LL) ? "ultrafast" : "veryfast", 0);
            } else if (std::strcmp(cand->name, "libx265") == 0 && attempt->priv_data) {
                av_opt_set(attempt->priv_data, "preset", "ultrafast", 0);
            } else if (std::strcmp(cand->name, "libopenh264") == 0 && attempt->priv_data) {
                av_opt_set_int(attempt->priv_data, "allow_skip_frames", 0, 0);
                av_opt_set(attempt->priv_data, "slice_mode", "auto", 0);
                attempt->rc_max_rate = targetBitrate * 2;
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
                    if (!inUnitTestMode && mfPass == 0 && probeCtx->priv_data) {
                        av_opt_set_int(probeCtx->priv_data, "hw_encoding", 1, 0);
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
        SWS_BILINEAR, nullptr, nullptr, nullptr);
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

    const bool streamOk = streamComposedFramesFromSource(
        rawInputPathUtf8,
        timeline,
        config,
        outSize,
        outFps,
        [&](const cv::Mat& composedBgr, int64_t frameIdx, double totalEst) -> bool {
            if (!writeOk || av_frame_make_writable(encFrame) < 0) return false;
            const uint8_t* srcSlice[4] = {composedBgr.data, nullptr, nullptr, nullptr};
            int srcStride[4] = {static_cast<int>(composedBgr.step[0]), 0, 0, 0};
            sws_scale(fromBgrSws, srcSlice, srcStride, 0, outSize.height, encFrame->data, encFrame->linesize);
            encFrame->pts = frameIdx;
            encFrame->pict_type = (frameIdx == 0) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            if (avcodec_send_frame(encCtx, encFrame) < 0) return false;
            drainEncoder();
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

    GifPaletteOptimizer palOpt{};
    palOpt.reserveTransparent = true;
    palOpt.enableFrameDiff = true;
    palOpt.enableDithering = enableDithering;

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

    cv::Size outSize = StudioVideoCompositor::resolveExportDimensions(srcW, srcH, opts);
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
                std::sqrt(static_cast<double>(kWeChatGifSafeMaxBytes) * 0.86 / static_cast<double>(actualBytes)),
                0.48, 0.82);
            outSize = StudioVideoCompositor::resolveOutputCanvasSize(
                (std::max)(320, static_cast<int>(std::round(outSize.width * scaleDown))),
                (std::max)(200, static_cast<int>(std::round(outSize.height * scaleDown))));
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

    const cv::Size outSize = StudioVideoCompositor::resolveExportDimensions(srcW, srcH, opts);
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
            StudioTimelineAnalyzer::exportSrtFile(srtPath.wstring(), timeline.captions);
        }
        if (onProgress) onProgress(1.0);
    }
    return ok;
}

} // namespace

cv::Size StudioVideoCompositor::resolveOutputCanvasSize(int srcWidth, int srcHeight) noexcept {
    int w = (std::max)(64, srcWidth);
    int h = (std::max)(64, srcHeight);
    if (w & 1) ++w;
    if (h & 1) ++h;
    return {w, h};
}

cv::Size StudioVideoCompositor::resolveExportDimensions(
    int srcWidth,
    int srcHeight,
    const StudioExportOptions& exportOpts) noexcept {
    const int safeSrcW = (std::max)(64, srcWidth);
    const int safeSrcH = (std::max)(64, srcHeight);
    int maxLongEdge = maxLongEdgeForResolution(exportOpts.resolution);

    if (exportOpts.format == StudioExportFormat::GIF_WeChat) {
        if (maxLongEdge <= 0 || maxLongEdge > kWeChatGifMaxDimension) {
            maxLongEdge = kWeChatGifMaxDimension;
        }
    }

    int outW = safeSrcW;
    int outH = safeSrcH;
    const int curLong = (std::max)(safeSrcW, safeSrcH);
    if (maxLongEdge > 0 && curLong > maxLongEdge) {
        const double ratio = static_cast<double>(maxLongEdge) / static_cast<double>(curLong);
        outW = static_cast<int>(std::round(safeSrcW * ratio));
        outH = static_cast<int>(std::round(safeSrcH * ratio));
    }
    return resolveOutputCanvasSize(outW, outH);
}

const char* StudioVideoCompositor::extensionForExportFormat(StudioExportFormat format) noexcept {
    switch (format) {
        case StudioExportFormat::GIF_WeChat:
        case StudioExportFormat::GIF_HD:        return ".gif";
        case StudioExportFormat::WebP_Animated: return ".webp";
        case StudioExportFormat::WebM_VP9:      return ".webm";
        case StudioExportFormat::MP4_H264:
        case StudioExportFormat::MP4_HEVC:
        default:                                return ".mp4";
    }
}

uint64_t StudioVideoCompositor::estimateExportSizeBytes(
    int srcWidth,
    int srcHeight,
    double durationSec,
    const StudioExportOptions& exportOpts) noexcept {
    const double safeDur = std::clamp(durationSec, 0.5, 36000.0);
    const cv::Size outSz = resolveExportDimensions(srcWidth, srcHeight, exportOpts);
    const double areaRatio = static_cast<double>(outSz.width) * static_cast<double>(outSz.height) / (1920.0 * 1080.0);
    const double qScale = qualityScaleFactor(exportOpts.quality);

    if (exportOpts.format == StudioExportFormat::GIF_WeChat ||
        exportOpts.format == StudioExportFormat::GIF_HD) {
        const int maxCap = (exportOpts.format == StudioExportFormat::GIF_WeChat) ? 15 : 20;
        int effFps = std::clamp(exportOpts.fps > 0 ? exportOpts.fps : 12, 6, maxCap);
        if (exportOpts.format == StudioExportFormat::GIF_WeChat) {
            const int safeFps = static_cast<int>(std::floor(static_cast<double>(kWeChatGifMaxFrames) / (std::max)(1.0, safeDur)));
            effFps = std::clamp((std::min)(effFps, safeFps), 5, 15);
        }
        // 含 +transdiff 帧间透明差分下的实证桌面动图平均每像素压缩率
        const double bytesPerFrame = static_cast<double>(outSz.width) * static_cast<double>(outSz.height) * 0.055 * qScale;
        uint64_t est = static_cast<uint64_t>(std::llround(bytesPerFrame * effFps * safeDur));
        est = (std::max<uint64_t>)(est, 64ULL * 1024ULL);
        if (exportOpts.format == StudioExportFormat::GIF_WeChat && est > kWeChatGifSafeMaxBytes) {
            est = kWeChatGifSafeMaxBytes - 120'000ULL;
        }
        return est;
    }

    if (exportOpts.format == StudioExportFormat::WebP_Animated) {
        const int effFps = std::clamp(exportOpts.fps > 0 ? exportOpts.fps : 24, 6, 60);
        const double bytesPerFrame = static_cast<double>(outSz.width) * static_cast<double>(outSz.height) * 0.022 * qScale;
        return (std::max<uint64_t>)(48ULL * 1024ULL, static_cast<uint64_t>(std::llround(bytesPerFrame * effFps * safeDur)));
    }

    // MP4 / HEVC / WebM (桌面演示具有极高帧间静止率，实际输出码率约为峰值目标码率的 42%~55%)
    const int effFps = std::clamp(exportOpts.fps > 0 ? exportOpts.fps : 30, 10, 120);
    const double fpsRatio = std::clamp(static_cast<double>(effFps) / 30.0, 0.5, 2.2);
    const bool isHevc = (exportOpts.format == StudioExportFormat::MP4_HEVC);
    const double effMbps = std::clamp(areaRatio * fpsRatio * qScale * (isHevc ? 3.2 : 4.6), 0.8, 45.0);
    const uint64_t videoBytes = static_cast<uint64_t>(std::llround((effMbps * 1'000'000.0 / 8.0) * safeDur));
    const uint64_t audioBytes = static_cast<uint64_t>(std::llround(16'000.0 * safeDur));
    return (std::max<uint64_t>)(64ULL * 1024ULL, videoBytes + audioBytes);
}

void StudioVideoCompositor::renderArtisticBackdropBgr(
    cv::Mat& outBgr,
    int width,
    int height,
    BeautyBackgroundType theme) {
    if (width <= 0 || height <= 0) return;

    const int gw = std::clamp(width / 10, 48, 220);
    const int gh = std::clamp(height / 10, 32, 140);
    cv::Mat gridF32(gh, gw, CV_32FC3);

    if (theme == BeautyBackgroundType::DuskMountains ||
        theme == BeautyBackgroundType::MistyLakeDawn) {
        renderLandscapeBackdropLowRes(gridF32, theme);
    } else {
        const MeshBackdropSpec spec = meshSpecForTheme(theme);
        for (int y = 0; y < gh; ++y) {
            const float ny = static_cast<float>(y) / static_cast<float>((std::max)(1, gh - 1));
            auto* row = gridF32.ptr<cv::Vec3f>(y);
            for (int x = 0; x < gw; ++x) {
                const float nx = static_cast<float>(x) / static_cast<float>((std::max)(1, gw - 1));
                cv::Vec3f col = spec.baseBgr;
                for (const auto& orb : spec.orbs) {
                    const float dx = nx - orb.nx;
                    const float dy = ny - orb.ny;
                    const float d2 = dx * dx + dy * dy;
                    const float r2 = (std::max)(0.04f, orb.radius * orb.radius);
                    const float w = std::clamp(std::exp(-d2 / r2) * orb.weight, 0.0f, 1.0f);
                    col = col * (1.0f - w) + orb.bgr * w;
                }
                if (spec.vignetteStrength > 0.001f) {
                    const float vx = nx - 0.5f;
                    const float vy = ny - 0.5f;
                    const float vig = std::clamp(1.0f - (vx * vx + vy * vy) * spec.vignetteStrength * 1.8f, 0.72f, 1.0f);
                    col *= vig;
                }
                row[x] = col;
            }
        }
    }

    cv::Mat resizedF32;
    cv::resize(gridF32, resizedF32, cv::Size(width, height), 0.0, 0.0, cv::INTER_CUBIC);

    // 4x4 有序亚像素微抖动，彻底消除编码后平滑渐变色带 (Color Banding)
    static constexpr float kBayer4x4[4][4] = {
        {-0.46875f,  0.03125f, -0.34375f,  0.15625f},
        { 0.28125f, -0.21875f,  0.40625f, -0.09375f},
        {-0.28125f,  0.21875f, -0.40625f,  0.09375f},
        { 0.46875f, -0.03125f,  0.34375f, -0.15625f},
    };

    outBgr.create(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        const auto* sRow = resizedF32.ptr<cv::Vec3f>(y);
        uint8_t* dRow = outBgr.ptr<uint8_t>(y);
        const float* ditherRow = kBayer4x4[y & 3];
        for (int x = 0; x < width; ++x) {
            const float d = ditherRow[x & 3] * 1.35f;
            dRow[x * 3 + 0] = static_cast<uint8_t>(std::clamp(sRow[x][0] + d, 0.0f, 255.0f));
            dRow[x * 3 + 1] = static_cast<uint8_t>(std::clamp(sRow[x][1] + d, 0.0f, 255.0f));
            dRow[x * 3 + 2] = static_cast<uint8_t>(std::clamp(sRow[x][2] + d, 0.0f, 255.0f));
        }
    }
}

cv::Mat StudioVideoCompositor::renderThemeThumbnailBgra(
    BeautyBackgroundType theme,
    int width,
    int height) {
    cv::Mat bgr;
    renderArtisticBackdropBgr(bgr, (std::max)(16, width), (std::max)(12, height), theme);
    cv::Mat bgra;
    cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
    return bgra;
}

void StudioVideoCompositor::ensureBackdropCache(
    int outW, int outH,
    bool autoFrame,
    BeautyBackgroundType theme,
    int padX, int padY,
    int innerW, int innerH,
    float radius) {
    if (!m_cachedBackdropBgr.empty() &&
        m_cacheW == outW && m_cacheH == outH &&
        m_cacheAutoFrame == autoFrame &&
        m_cacheTheme == theme &&
        m_cachePadX == padX && m_cachePadY == padY &&
        m_cacheInnerW == innerW && m_cacheInnerH == innerH &&
        std::abs(m_cacheRadius - radius) < 0.2f) {
        return;
    }

    m_cacheW = outW;
    m_cacheH = outH;
    m_cacheAutoFrame = autoFrame;
    m_cacheTheme = theme;
    m_cachePadX = padX;
    m_cachePadY = padY;
    m_cacheInnerW = innerW;
    m_cacheInnerH = innerH;
    m_cacheRadius = radius;

    if (!autoFrame) {
        m_cachedBackdropBgr.create(outH, outW, CV_8UC3);
        m_cachedBackdropBgr.setTo(cv::Scalar(16, 18, 24));
        return;
    }

    renderArtisticBackdropBgr(m_cachedBackdropBgr, outW, outH, theme);

    // 生成深景深柔和弥散投影
    cv::Mat shadowMask(outH, outW, CV_8UC1, cv::Scalar(0));
    const int offsetY = (std::max)(5, outH / 68);
    const cv::Rect shadowRect(
        padX,
        (std::min)(outH - innerH, padY + offsetY),
        innerW,
        innerH);
    fillRoundedRectMask(shadowMask, shadowRect, radius);
    int blurKernel = (std::max)(17, (outH / 20) | 1);
    if ((blurKernel & 1) == 0) ++blurKernel;
    cv::GaussianBlur(shadowMask, shadowMask, cv::Size(blurKernel, blurKernel), 0.0);

    const bool lightTheme =
        (theme == BeautyBackgroundType::PureMinimal ||
         theme == BeautyBackgroundType::PaperChalk ||
         theme == BeautyBackgroundType::AiryGlacier);
    const float shadowMaxAlpha = lightTheme ? 0.24f : 0.48f;

    for (int y = 0; y < outH; ++y) {
        uint8_t* bgrRow = m_cachedBackdropBgr.ptr<uint8_t>(y);
        const uint8_t* shRow = shadowMask.ptr<uint8_t>(y);
        for (int x = 0; x < outW; ++x) {
            const float shAlpha = (static_cast<float>(shRow[x]) / 255.0f) * shadowMaxAlpha;
            if (shAlpha > 0.002f) {
                const float keep = 1.0f - shAlpha;
                bgrRow[x * 3 + 0] = static_cast<uint8_t>(bgrRow[x * 3 + 0] * keep);
                bgrRow[x * 3 + 1] = static_cast<uint8_t>(bgrRow[x * 3 + 1] * keep);
                bgrRow[x * 3 + 2] = static_cast<uint8_t>(bgrRow[x * 3 + 2] * keep);
            }
        }
    }

    // 外圈 1px 微晶玻璃高光衬框 (Glass Rim Highlight)
    if (padX >= 2 && padY >= 2) {
        cv::Mat rimLayer = m_cachedBackdropBgr.clone();
        const int rInt = static_cast<int>(std::round(radius)) + 1;
        const cv::Rect rimRc(padX - 1, padY - 1, innerW + 2, innerH + 2);
        const cv::Scalar rimCol(255, 255, 255);
        cv::line(rimLayer, {rimRc.x + rInt, rimRc.y}, {rimRc.x + rimRc.width - rInt, rimRc.y}, rimCol, 1, cv::LINE_AA);
        cv::line(rimLayer, {rimRc.x + rInt, rimRc.y + rimRc.height}, {rimRc.x + rimRc.width - rInt, rimRc.y + rimRc.height}, rimCol, 1, cv::LINE_AA);
        cv::line(rimLayer, {rimRc.x, rimRc.y + rInt}, {rimRc.x, rimRc.y + rimRc.height - rInt}, rimCol, 1, cv::LINE_AA);
        cv::line(rimLayer, {rimRc.x + rimRc.width, rimRc.y + rInt}, {rimRc.x + rimRc.width, rimRc.y + rimRc.height - rInt}, rimCol, 1, cv::LINE_AA);
        cv::ellipse(rimLayer, {rimRc.x + rInt, rimRc.y + rInt}, {rInt, rInt}, 180, 0, 90, rimCol, 1, cv::LINE_AA);
        cv::ellipse(rimLayer, {rimRc.x + rimRc.width - rInt, rimRc.y + rInt}, {rInt, rInt}, 270, 0, 90, rimCol, 1, cv::LINE_AA);
        cv::ellipse(rimLayer, {rimRc.x + rimRc.width - rInt, rimRc.y + rimRc.height - rInt}, {rInt, rInt}, 0, 0, 90, rimCol, 1, cv::LINE_AA);
        cv::ellipse(rimLayer, {rimRc.x + rInt, rimRc.y + rimRc.height - rInt}, {rInt, rInt}, 90, 0, 90, rimCol, 1, cv::LINE_AA);
        const double rimAlpha = lightTheme ? 0.55 : 0.22;
        cv::addWeighted(rimLayer, rimAlpha, m_cachedBackdropBgr, 1.0 - rimAlpha, 0.0, m_cachedBackdropBgr);
    }
}

void StudioVideoCompositor::ensureCaptionOverlayCache(const std::string& captionUtf8, int canvasW, int canvasH) {
    if (captionUtf8.empty()) {
        m_cachedCaptionText.clear();
        m_cachedCaptionBgra.release();
        return;
    }
    if (captionUtf8 == m_cachedCaptionText && canvasW == m_cachedCaptionCanvasW && !m_cachedCaptionBgra.empty()) {
        return;
    }

    m_cachedCaptionText = captionUtf8;
    m_cachedCaptionCanvasW = canvasW;
    m_cachedCaptionBgra.release();

    const std::wstring wide = tools3000::core::WinUtils::utf8ToWstring(captionUtf8);
    if (wide.empty()) return;

    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> dwFactory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf())))) {
        return;
    }

    const float fontSize = std::clamp(static_cast<float>(canvasH) * 0.029f, 14.0f, 34.0f);
    const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(dwFactory.Get());
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwFactory->CreateTextFormat(
            fontFamily, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"zh-CN", fmt.GetAddressOf())) || !fmt) {
        return;
    }
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const float maxTextW = static_cast<float>(canvasW) * 0.78f;
    const float maxTextH = static_cast<float>(canvasH) * 0.22f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwFactory->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), fmt.Get(), maxTextW, maxTextH, layout.GetAddressOf()))) {
        return;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return;

    const float padX = std::clamp(fontSize * 0.95f, 14.0f, 28.0f);
    const float padY = std::clamp(fontSize * 0.45f, 7.0f, 16.0f);
    const int pillW = (std::min)(canvasW - 16, static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace + padX * 2.0f + 8.0f)));
    const int pillH = (std::min)(canvasH / 3, static_cast<int>(std::ceil(metrics.height + padY * 2.0f + 6.0f)));
    if (pillW <= 8 || pillH <= 8) return;

    layout.Reset();
    if (FAILED(dwFactory->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), fmt.Get(),
            static_cast<float>(pillW - 4), static_cast<float>(pillH - 4), layout.GetAddressOf()))) {
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = pillW;
    bmi.bmiHeader.biHeight = -pillH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC memDc = CreateCompatibleDC(nullptr);
    if (!memDc) return;
    HBITMAP hBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp || !bits) {
        if (hBmp) DeleteObject(hBmp);
        DeleteDC(memDc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(memDc, hBmp);
    std::memset(bits, 0, static_cast<size_t>(pillW) * static_cast<size_t>(pillH) * 4);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1DCRenderTarget> rt;
    if (SUCCEEDED(d2dFactory->CreateDCRenderTarget(&props, rt.GetAddressOf())) && rt) {
        RECT bindRc{0, 0, pillW, pillH};
        if (SUCCEEDED(rt->BindDC(memDc, &bindRc))) {
            rt->BeginDraw();
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));

            const float radius = (std::min)(static_cast<float>(pillH) * 0.46f, 16.0f);
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                D2D1::RectF(1.0f, 1.0f, static_cast<float>(pillW) - 1.0f, static_cast<float>(pillH) - 1.0f),
                radius, radius);

            ComPtr<ID2D1SolidColorBrush> bgBrush;
            ComPtr<ID2D1SolidColorBrush> borderBrush;
            ComPtr<ID2D1SolidColorBrush> textBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.08f, 0.12f, 0.84f), bgBrush.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f), borderBrush.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.99f, 1.0f, 1.0f), textBrush.GetAddressOf());

            if (bgBrush) rt->FillRoundedRectangle(rr, bgBrush.Get());
            if (borderBrush) rt->DrawRoundedRectangle(rr, borderBrush.Get(), 1.0f);
            if (textBrush) rt->DrawTextLayout(D2D1::Point2F(2.0f, 2.0f), layout.Get(), textBrush.Get());
            rt->EndDraw();
        }
    }

    m_cachedCaptionBgra = cv::Mat(pillH, pillW, CV_8UC4, bits).clone();
    m_cachedCaptionDrawX = (canvasW - pillW) / 2;
    m_cachedCaptionDrawY = canvasH - pillH - (std::max)(18, static_cast<int>(canvasH * 0.055f));

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
}

void StudioVideoCompositor::drawCrispVectorCursor(
    cv::Mat& canvasBgr,
    float x, float y,
    StudioCursorKind kind,
    float scale,
    float rippleProgress,
    bool rippleRight) {
    if (kind == StudioCursorKind::Hidden || canvasBgr.empty()) return;
    if (x < -32.0f || y < -32.0f || x > canvasBgr.cols + 32.0f || y > canvasBgr.rows + 32.0f) return;

    constexpr int kShift = 4;
    constexpr float kSub = 16.0f;
    const auto toSubPt = [&](float dx, float dy) -> cv::Point {
        return {
            static_cast<int>(std::round((x + dx * scale) * kSub)),
            static_cast<int>(std::round((y + dy * scale) * kSub))
        };
    };

    if (rippleProgress >= 0.0f && rippleProgress <= 1.0f) {
        const float ease = 1.0f - (1.0f - rippleProgress) * (1.0f - rippleProgress);
        const float radius = (10.0f + 34.0f * ease) * std::clamp(scale, 0.85f, 1.8f);
        const float alpha = (1.0f - rippleProgress) * 0.62f;
        const int rInt = static_cast<int>(std::ceil(radius + 4.0f));
        const int rx0 = (std::max)(0, static_cast<int>(x) - rInt);
        const int ry0 = (std::max)(0, static_cast<int>(y) - rInt);
        const int rx1 = (std::min)(canvasBgr.cols, static_cast<int>(x) + rInt);
        const int ry1 = (std::min)(canvasBgr.rows, static_cast<int>(y) + rInt);
        if (rx1 > rx0 && ry1 > ry0 && alpha > 0.02f) {
            cv::Mat roi = canvasBgr(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0));
            cv::Mat overlay = roi.clone();
            const cv::Scalar ringColor = rippleRight
                ? cv::Scalar(11, 158, 245)
                : cv::Scalar(248, 189, 56);
            cv::circle(
                overlay,
                cv::Point(static_cast<int>((x - rx0) * kSub), static_cast<int>((y - ry0) * kSub)),
                static_cast<int>(radius * kSub),
                ringColor,
                (std::max)(2, static_cast<int>(std::round(2.6f * scale))),
                cv::LINE_AA,
                kShift);
            cv::addWeighted(overlay, alpha, roi, 1.0 - alpha, 0.0, roi);
        }
    }

    if (kind == StudioCursorKind::IBeam) {
        const int thickBg = (std::max)(3, static_cast<int>(std::round(3.6f * scale)));
        const int thickFg = (std::max)(1, static_cast<int>(std::round(1.6f * scale)));
        const cv::Point topL = toSubPt(-4.5f, -9.5f);
        const cv::Point topR = toSubPt(4.5f, -9.5f);
        const cv::Point midT = toSubPt(0.0f, -9.5f);
        const cv::Point midB = toSubPt(0.0f, 9.5f);
        const cv::Point botL = toSubPt(-4.5f, 9.5f);
        const cv::Point botR = toSubPt(4.5f, 9.5f);

        cv::line(canvasBgr, topL, topR, cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, midT, midB, cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, botL, botR, cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);

        cv::line(canvasBgr, topL, topR, cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, midT, midB, cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, botL, botR, cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        return;
    }

    if (kind == StudioCursorKind::Cross) {
        const int thickBg = (std::max)(3, static_cast<int>(std::round(3.4f * scale)));
        const int thickFg = (std::max)(1, static_cast<int>(std::round(1.5f * scale)));
        cv::line(canvasBgr, toSubPt(-9.0f, 0.0f), toSubPt(9.0f, 0.0f), cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(0.0f, -9.0f), toSubPt(0.0f, 9.0f), cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(-9.0f, 0.0f), toSubPt(9.0f, 0.0f), cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(0.0f, -9.0f), toSubPt(0.0f, 9.0f), cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        return;
    }

    static constexpr std::array<std::pair<float, float>, 7> kArrowShape = {{
        {0.0f, 0.0f},
        {0.0f, 19.2f},
        {4.8f, 14.8f},
        {8.2f, 22.0f},
        {11.1f, 20.6f},
        {7.6f, 13.4f},
        {13.8f, 13.4f},
    }};

    std::vector<cv::Point> shadowPoly;
    std::vector<cv::Point> mainPoly;
    shadowPoly.reserve(kArrowShape.size());
    mainPoly.reserve(kArrowShape.size());
    for (const auto& [px, py] : kArrowShape) {
        shadowPoly.push_back(toSubPt(px + 1.2f, py + 1.8f));
        mainPoly.push_back(toSubPt(px, py));
    }

    const int pad = static_cast<int>(std::ceil(28.0f * scale));
    const int sx0 = (std::max)(0, static_cast<int>(x) - 6);
    const int sy0 = (std::max)(0, static_cast<int>(y) - 6);
    const int sx1 = (std::min)(canvasBgr.cols, static_cast<int>(x) + pad);
    const int sy1 = (std::min)(canvasBgr.rows, static_cast<int>(y) + pad);
    if (sx1 > sx0 && sy1 > sy0) {
        cv::Mat roi = canvasBgr(cv::Rect(sx0, sy0, sx1 - sx0, sy1 - sy0));
        cv::Mat shLayer = roi.clone();
        std::vector<cv::Point> localShadow = shadowPoly;
        for (auto& pt : localShadow) {
            pt.x -= sx0 * static_cast<int>(kSub);
            pt.y -= sy0 * static_cast<int>(kSub);
        }
        cv::fillPoly(shLayer, std::vector<std::vector<cv::Point>>{localShadow}, cv::Scalar(10, 12, 18), cv::LINE_AA, kShift);
        cv::addWeighted(shLayer, 0.28, roi, 0.72, 0.0, roi);
    }

    const int borderThick = (std::max)(2, static_cast<int>(std::round(2.4f * scale)));
    cv::polylines(canvasBgr, std::vector<std::vector<cv::Point>>{mainPoly}, true,
                  cv::Scalar(255, 255, 255), borderThick, cv::LINE_AA, kShift);
    cv::fillPoly(canvasBgr, std::vector<std::vector<cv::Point>>{mainPoly},
                 kind == StudioCursorKind::Hand ? cv::Scalar(235, 99, 37) : cv::Scalar(18, 22, 32),
                 cv::LINE_AA, kShift);
}

cv::Mat StudioVideoCompositor::composeFrame(
    const cv::Mat& rawBgrOrBgra,
    const StudioEvaluatedFrame& eval,
    const StudioRenderConfig& config,
    cv::Size targetSize) {
    if (rawBgrOrBgra.empty()) return {};

    cv::Mat srcBgr;
    if (rawBgrOrBgra.channels() == 4) {
        cv::cvtColor(rawBgrOrBgra, srcBgr, cv::COLOR_BGRA2BGR);
    } else if (rawBgrOrBgra.channels() == 3) {
        srcBgr = rawBgrOrBgra;
    } else {
        cv::cvtColor(rawBgrOrBgra, srcBgr, cv::COLOR_GRAY2BGR);
    }

    const cv::Size outSize = (targetSize.width > 0 && targetSize.height > 0)
        ? resolveOutputCanvasSize(targetSize.width, targetSize.height)
        : resolveOutputCanvasSize(srcBgr.cols, srcBgr.rows);
    const int outW = outSize.width;
    const int outH = outSize.height;

    int padX = 0;
    int padY = 0;
    int innerW = outW;
    int innerH = outH;
    float cornerR = 0.0f;

    if (config.autoFrameEnabled) {
        const float safeRatio = std::clamp(config.paddingRatio, 0.02f, 0.16f);
        const int basePad = (std::max)(12, static_cast<int>(std::round((std::min)(outW, outH) * safeRatio)));
        const int availW = (std::max)(32, outW - basePad * 2);
        const int availH = (std::max)(32, outH - basePad * 2);
        const float scaleFit = (std::min)(
            static_cast<float>(availW) / static_cast<float>((std::max)(1, srcBgr.cols)),
            static_cast<float>(availH) / static_cast<float>((std::max)(1, srcBgr.rows)));
        innerW = (std::max)(16, static_cast<int>(std::round(srcBgr.cols * scaleFit)));
        innerH = (std::max)(16, static_cast<int>(std::round(srcBgr.rows * scaleFit)));
        padX = (outW - innerW) / 2;
        padY = (outH - innerH) / 2;
        cornerR = std::clamp(config.cornerRadius * (static_cast<float>(outH) / 1080.0f), 4.0f, 36.0f);
    }

    ensureBackdropCache(outW, outH, config.autoFrameEnabled, config.backgroundTheme, padX, padY, innerW, innerH, cornerR);
    cv::Mat canvas = m_cachedBackdropBgr.clone();

    float effCropX = 0.0f;
    float effCropY = 0.0f;
    float effCropW = static_cast<float>(srcBgr.cols);
    float effCropH = static_cast<float>(srcBgr.rows);
    if (eval.camera.cropW > 0.5f && eval.camera.cropH > 0.5f && eval.camera.zoom >= 0.99f) {
        const float scaleRatioX = static_cast<float>(srcBgr.cols) /
            (std::max)(1.0f, eval.camera.cropW * eval.camera.zoom);
        const float scaleRatioY = static_cast<float>(srcBgr.rows) /
            (std::max)(1.0f, eval.camera.cropH * eval.camera.zoom);

        effCropX = std::clamp(eval.camera.cropX * scaleRatioX, 0.0f, static_cast<float>(srcBgr.cols - 1));
        effCropY = std::clamp(eval.camera.cropY * scaleRatioY, 0.0f, static_cast<float>(srcBgr.rows - 1));
        effCropW = std::clamp(eval.camera.cropW * scaleRatioX, 1.0f, static_cast<float>(srcBgr.cols) - effCropX);
        effCropH = std::clamp(eval.camera.cropH * scaleRatioY, 1.0f, static_cast<float>(srcBgr.rows) - effCropY);
    }

    const int ix = static_cast<int>(std::floor(effCropX));
    const int iy = static_cast<int>(std::floor(effCropY));
    const int iw = std::clamp(static_cast<int>(std::round(effCropW)), 1, srcBgr.cols - ix);
    const int ih = std::clamp(static_cast<int>(std::round(effCropH)), 1, srcBgr.rows - iy);

    cv::Mat zoomedInner;
    const int interp = (innerW < iw || innerH < ih) ? cv::INTER_AREA : cv::INTER_LINEAR;
    cv::resize(srcBgr(cv::Rect(ix, iy, iw, ih)), zoomedInner, cv::Size(innerW, innerH), 0.0, 0.0, interp);

    cv::Mat dstInner = canvas(cv::Rect(padX, padY, innerW, innerH));
    if (!config.autoFrameEnabled || cornerR <= 0.5f) {
        zoomedInner.copyTo(dstInner);
    } else {
        zoomedInner.copyTo(dstInner);
        const float r = (std::min)({cornerR, innerW * 0.5f, innerH * 0.5f});
        const int intR = static_cast<int>(std::ceil(r));
        auto blendCorner = [&](int sx, int ex, int sy, int ey, float cx, float cy) {
            for (int y = sy; y < ey; ++y) {
                uint8_t* dstRow = dstInner.ptr<uint8_t>(y);
                const uint8_t* bgRow = m_cachedBackdropBgr.ptr<uint8_t>(padY + y) + padX * 3;
                const uint8_t* fgRow = zoomedInner.ptr<uint8_t>(y);
                const float py = static_cast<float>(y) + 0.5f;
                for (int x = sx; x < ex; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float d = std::hypot(px - cx, py - cy);
                    if (d > r - 0.5f) {
                        const float cov = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                        const float inv = 1.0f - cov;
                        dstRow[x * 3 + 0] = static_cast<uint8_t>(fgRow[x * 3 + 0] * cov + bgRow[x * 3 + 0] * inv);
                        dstRow[x * 3 + 1] = static_cast<uint8_t>(fgRow[x * 3 + 1] * cov + bgRow[x * 3 + 1] * inv);
                        dstRow[x * 3 + 2] = static_cast<uint8_t>(fgRow[x * 3 + 2] * cov + bgRow[x * 3 + 2] * inv);
                    }
                }
            }
        };
        blendCorner(0, intR, 0, intR, r, r);
        blendCorner(innerW - intR, innerW, 0, intR, innerW - r, r);
        blendCorner(0, intR, innerH - intR, innerH, r, innerH - r);
        blendCorner(innerW - intR, innerW, innerH - intR, innerH, innerW - r, innerH - r);
    }

    if (config.smoothCursorEnabled && eval.cursorKind != StudioCursorKind::Hidden) {
        const float normX = (eval.smoothCursorX - eval.camera.cropX) / (std::max)(1.0f, eval.camera.cropW);
        const float normY = (eval.smoothCursorY - eval.camera.cropY) / (std::max)(1.0f, eval.camera.cropH);
        if (normX >= -0.02f && normX <= 1.02f && normY >= -0.02f && normY <= 1.02f) {
            const float curCanvasX = static_cast<float>(padX) + normX * static_cast<float>(innerW);
            const float curCanvasY = static_cast<float>(padY) + normY * static_cast<float>(innerH);
            const float dpiRatio = std::clamp(static_cast<float>(outH) / 900.0f, 0.55f, 2.2f);
            drawCrispVectorCursor(
                canvas,
                curCanvasX,
                curCanvasY,
                eval.cursorKind,
                config.cursorScale * eval.clickScale * dpiRatio,
                eval.rippleProgress,
                eval.rippleIsRight);
        }
    }

    if (config.autoCaptionsEnabled && !eval.activeCaption.empty()) {
        ensureCaptionOverlayCache(eval.activeCaption, outW, outH);
        if (!m_cachedCaptionBgra.empty()) {
            alphaBlendPremultipliedBgraOntoBgr(
                m_cachedCaptionBgra, canvas, m_cachedCaptionDrawX, m_cachedCaptionDrawY);
        }
    }

    return canvas;
}

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

    const int safeMaxW = std::clamp(maxPreviewWidth, 160, 1920);
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
                sws = sws_getContext(
                    frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                    dstW, dstH, AV_PIX_FMT_BGR24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
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
            (std::max)(timedDeck.size(), static_cast<size_t>(std::ceil(outDurationSec * 12.0))));
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

} // namespace tools3000::capture

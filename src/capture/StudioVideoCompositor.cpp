#include "capture/StudioVideoCompositor.h"
#include "capture/ScreenRecorder.h"
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
        case StudioExportQuality::High:    return 1.65;
        case StudioExportQuality::Compact: return 0.56;
        case StudioExportQuality::Recommended:
        default:                           return 1.00;
    }
}

const wchar_t* builtinWallpaperFilename(BeautyBackgroundType theme) noexcept {
    switch (theme) {
        case BeautyBackgroundType::MidnightGraphite: return L"midnight_graphite.jpg";
        case BeautyBackgroundType::SilkMist:         return L"silk_mist.jpg";
        case BeautyBackgroundType::PureMinimal:      return L"pure_minimal.jpg";
        case BeautyBackgroundType::PaperChalk:       return L"paper_chalk.jpg";
        case BeautyBackgroundType::AiryGlacier:      return L"airy_glacier.jpg";
        case BeautyBackgroundType::DuskMountains:    return L"dusk_mountains.jpg";
        case BeautyBackgroundType::MistyLakeDawn:    return L"misty_lake_dawn.jpg";
        case BeautyBackgroundType::StudioSlate:
        default:                                     return L"studio_slate.jpg";
    }
}

std::filesystem::path resolveBuiltinWallpaperPath(BeautyBackgroundType theme) {
    const wchar_t* fname = builtinWallpaperFilename(theme);
    wchar_t exePathBuf[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePathBuf, static_cast<DWORD>(std::size(exePathBuf)));
    std::error_code ec;
    if (len > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePathBuf).parent_path();
        const auto p1 = exeDir / L"resources" / L"wallpapers" / fname;
        if (std::filesystem::exists(p1, ec)) return p1;
        const auto p2 = exeDir.parent_path() / L"resources" / L"wallpapers" / fname;
        if (std::filesystem::exists(p2, ec)) return p2;
        const auto p3 = exeDir.parent_path().parent_path() / L"resources" / L"wallpapers" / fname;
        if (std::filesystem::exists(p3, ec)) return p3;
    }
    const auto cwdPath = std::filesystem::current_path(ec) / L"resources" / L"wallpapers" / fname;
    if (std::filesystem::exists(cwdPath, ec)) return cwdPath;
    return {};
}

cv::Mat decodeWallpaperFileCached(const std::filesystem::path& filePath) {
    if (filePath.empty()) return {};
    static std::mutex s_wallMtx;
    struct CacheSlot {
        std::wstring path;
        cv::Mat bgr;
    };
    static std::array<CacheSlot, 12> s_slots;
    static size_t s_nextSlot = 0;

    const std::wstring key = filePath.wstring();
    {
        std::lock_guard<std::mutex> lk(s_wallMtx);
        for (const auto& sl : s_slots) {
            if (sl.path == key && !sl.bgr.empty()) {
                return sl.bgr;
            }
        }
    }

    std::ifstream ifs(filePath, std::ios::binary | std::ios::ate);
    if (!ifs) return {};
    const auto sz = ifs.tellg();
    if (sz <= 0 || sz > 64 * 1024 * 1024) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), sz)) return {};

    cv::Mat decoded = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (decoded.empty()) return {};

    {
        std::lock_guard<std::mutex> lk(s_wallMtx);
        s_slots[s_nextSlot] = {key, decoded};
        s_nextSlot = (s_nextSlot + 1) % s_slots.size();
    }
    return decoded;
}

void cropResizeCoverBgr(const cv::Mat& srcBgr, cv::Mat& outBgr, int dstW, int dstH) {
    if (srcBgr.empty() || dstW <= 0 || dstH <= 0) return;
    const double srcAspect = static_cast<double>(srcBgr.cols) / static_cast<double>((std::max)(1, srcBgr.rows));
    const double dstAspect = static_cast<double>(dstW) / static_cast<double>((std::max)(1, dstH));

    cv::Rect cropRc(0, 0, srcBgr.cols, srcBgr.rows);
    if (srcAspect > dstAspect + 1e-4) {
        const int cw = std::clamp(static_cast<int>(std::round(srcBgr.rows * dstAspect)), 1, srcBgr.cols);
        cropRc.x = (srcBgr.cols - cw) / 2;
        cropRc.width = cw;
    } else if (dstAspect > srcAspect + 1e-4) {
        const int ch = std::clamp(static_cast<int>(std::round(srcBgr.cols / dstAspect)), 1, srcBgr.rows);
        cropRc.y = (srcBgr.rows - ch) / 2;
        cropRc.height = ch;
    }
    const int interp = (dstW < cropRc.width || dstH < cropRc.height) ? cv::INTER_AREA : cv::INTER_LINEAR;
    cv::resize(srcBgr(cropRc), outBgr, cv::Size(dstW, dstH), 0.0, 0.0, interp);
}

cv::Scalar parseHexBgrScalar(const std::string& hex, const cv::Scalar& fallbackBgr) noexcept {
    if (hex.size() == 7 && hex[0] == '#') {
        auto hv = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        const int r1 = hv(hex[1]), r2 = hv(hex[2]);
        const int g1 = hv(hex[3]), g2 = hv(hex[4]);
        const int b1 = hv(hex[5]), b2 = hv(hex[6]);
        if (r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0) {
            return cv::Scalar((b1 << 4) | b2, (g1 << 4) | g2, (r1 << 4) | r2);
        }
    }
    return fallbackBgr;
}

void drawStyledTextCalloutBgr(
    cv::Mat& innerBgr,
    int x, int y,
    StudioAnnotationKind kind,
    const std::string& textUtf8,
    const cv::Scalar& accentBgr) {
    if (textUtf8.empty() || innerBgr.empty()) return;

    struct CalloutBitmapCacheEntry {
        StudioAnnotationKind kind{};
        std::string text;
        uint32_t bgrPacked = 0;
        int refRows = 0;
        cv::Mat premulBgra;
    };
    thread_local std::vector<CalloutBitmapCacheEntry> s_calloutCache;
    thread_local ComPtr<ID2D1Factory> s_d2dFactory;
    thread_local ComPtr<IDWriteFactory> s_dwFactory;

    const uint32_t packedColor =
        (static_cast<uint32_t>(accentBgr[0]) & 0xFFu) |
        ((static_cast<uint32_t>(accentBgr[1]) & 0xFFu) << 8) |
        ((static_cast<uint32_t>(accentBgr[2]) & 0xFFu) << 16);

    for (const auto& entry : s_calloutCache) {
        if (entry.kind == kind && entry.bgrPacked == packedColor &&
            entry.refRows == innerBgr.rows && entry.text == textUtf8 && !entry.premulBgra.empty()) {
            const int drawX = std::clamp(x - entry.premulBgra.cols / 2, 2, (std::max)(2, innerBgr.cols - entry.premulBgra.cols - 2));
            const int drawY = std::clamp(y - entry.premulBgra.rows / 2, 2, (std::max)(2, innerBgr.rows - entry.premulBgra.rows - 2));
            alphaBlendPremultipliedBgraOntoBgr(entry.premulBgra, innerBgr, drawX, drawY);
            return;
        }
    }

    const std::wstring wide = tools3000::core::WinUtils::utf8ToWstring(textUtf8);
    if (wide.empty()) return;

    if (!s_d2dFactory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, s_d2dFactory.GetAddressOf()))) {
        return;
    }
    if (!s_dwFactory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                   reinterpret_cast<IUnknown**>(s_dwFactory.GetAddressOf())))) {
        return;
    }
    ComPtr<ID2D1Factory> d2dFactory = s_d2dFactory;
    ComPtr<IDWriteFactory> dwFactory = s_dwFactory;

    const float fontSize = std::clamp(static_cast<float>(innerBgr.rows) *
        (kind == StudioAnnotationKind::TextPlain ? 0.032f : 0.026f), 14.0f, 34.0f);
    const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(dwFactory.Get());
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwFactory->CreateTextFormat(
            fontFamily, nullptr, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"zh-CN", fmt.GetAddressOf())) || !fmt) {
        return;
    }
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwFactory->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), fmt.Get(),
            static_cast<float>(innerBgr.cols) * 0.72f, static_cast<float>(innerBgr.rows) * 0.28f, layout.GetAddressOf()))) {
        return;
    }
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return;

    const bool isBadge = (kind == StudioAnnotationKind::StepBadge);
    const bool isBubble = (kind == StudioAnnotationKind::TextBubble);
    const float tailH = isBubble ? std::clamp(fontSize * 0.42f, 7.0f, 13.0f) : 0.0f;

    int boxW = std::clamp(static_cast<int>(std::ceil(m.widthIncludingTrailingWhitespace + 30.0f)), 28, innerBgr.cols - 4);
    int bodyH = std::clamp(static_cast<int>(std::ceil(m.height + 16.0f)), 24, innerBgr.rows / 2);
    if (isBadge) {
        const int d = (std::max)(boxW - 10, bodyH + 4);
        boxW = d;
        bodyH = d;
    }
    const int totalH = (std::min)(innerBgr.rows - 4, bodyH + static_cast<int>(std::ceil(tailH)) + 4);

    layout.Reset();
    if (FAILED(dwFactory->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), fmt.Get(),
            static_cast<float>(boxW - 4), static_cast<float>(bodyH - 4), layout.GetAddressOf()))) {
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = boxW;
    bmi.bmiHeader.biHeight = -totalH;
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
    std::memset(bits, 0, static_cast<size_t>(boxW) * static_cast<size_t>(totalH) * 4);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1DCRenderTarget> rt;
    if (SUCCEEDED(d2dFactory->CreateDCRenderTarget(&props, rt.GetAddressOf())) && rt) {
        RECT bindRc{0, 0, boxW, totalH};
        if (SUCCEEDED(rt->BindDC(memDc, &bindRc))) {
            rt->BeginDraw();
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));

            const float ar = static_cast<float>(accentBgr[2] / 255.0);
            const float ag = static_cast<float>(accentBgr[1] / 255.0);
            const float ab = static_cast<float>(accentBgr[0] / 255.0);
            const D2D1_COLOR_F colAcc = D2D1::ColorF(ar, ag, ab, 0.96f);

            ComPtr<ID2D1SolidColorBrush> bBg, bBorder, bText, bShadow;
            rt->CreateSolidColorBrush(colAcc, bBg.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), bBorder.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), bText.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.06f, 0.10f, 0.86f), bShadow.GetAddressOf());

            const D2D1_RECT_F bodyRc = D2D1::RectF(2.0f, 2.0f, static_cast<float>(boxW) - 2.0f, static_cast<float>(bodyH));
            const float r = isBadge ? (bodyRc.bottom - bodyRc.top) * 0.5f
                                    : (std::min)((bodyRc.bottom - bodyRc.top) * 0.42f, 12.0f);
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(bodyRc, r, r);

            if (kind == StudioAnnotationKind::TextPlain) {
                if (bShadow) {
                    rt->DrawTextLayout(D2D1::Point2F(3.5f, 3.5f), layout.Get(), bShadow.Get());
                }
                if (bBg) {
                    rt->DrawTextLayout(D2D1::Point2F(2.0f, 2.0f), layout.Get(), bBg.Get());
                }
            } else if (kind == StudioAnnotationKind::StepBadge) {
                const float cx = (bodyRc.left + bodyRc.right) * 0.5f;
                const float cy = (bodyRc.top + bodyRc.bottom) * 0.5f;
                const float rad = (std::min)(bodyRc.right - bodyRc.left, bodyRc.bottom - bodyRc.top) * 0.48f;
                if (bBg) rt->FillEllipse(D2D1::Ellipse({cx, cy}, rad, rad), bBg.Get());
                if (bBorder) rt->DrawEllipse(D2D1::Ellipse({cx, cy}, rad, rad), bBorder.Get(), 2.0f);
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            } else if (kind == StudioAnnotationKind::TextBubble) {
                if (bBg) {
                    rt->FillRoundedRectangle(rr, bBg.Get());
                    ComPtr<ID2D1PathGeometry> tailGeo;
                    if (SUCCEEDED(d2dFactory->CreatePathGeometry(tailGeo.GetAddressOf())) && tailGeo) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(tailGeo->Open(sink.GetAddressOf())) && sink) {
                            const float tx = (bodyRc.left + bodyRc.right) * 0.5f;
                            sink->BeginFigure({tx - tailH * 0.65f, bodyRc.bottom - 1.5f}, D2D1_FIGURE_BEGIN_FILLED);
                            sink->AddLine({tx + tailH * 0.65f, bodyRc.bottom - 1.5f});
                            sink->AddLine({tx - tailH * 0.15f, bodyRc.bottom + tailH});
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sink->Close();
                            rt->FillGeometry(tailGeo.Get(), bBg.Get());
                        }
                    }
                }
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            } else if (kind == StudioAnnotationKind::TextDashed) {
                ComPtr<ID2D1SolidColorBrush> glassBg;
                rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.15f, 0.22f, 0.84f), glassBg.GetAddressOf());
                if (glassBg) rt->FillRoundedRectangle(rr, glassBg.Get());
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
                sp.dashStyle = D2D1_DASH_STYLE_DASH;
                sp.dashCap = D2D1_CAP_STYLE_ROUND;
                ComPtr<ID2D1StrokeStyle> dashStyle;
                d2dFactory->CreateStrokeStyle(sp, nullptr, 0, dashStyle.GetAddressOf());
                if (bBg) rt->DrawRoundedRectangle(rr, bBg.Get(), 2.0f, dashStyle.Get());
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            } else if (kind == StudioAnnotationKind::TextNeon) {
                if (bShadow) rt->FillRoundedRectangle(rr, bShadow.Get());
                ComPtr<ID2D1SolidColorBrush> glowB;
                rt->CreateSolidColorBrush(D2D1::ColorF(ar, ag, ab, 0.36f), glowB.GetAddressOf());
                if (glowB) rt->DrawRoundedRectangle(rr, glowB.Get(), 5.0f);
                if (bBg) rt->DrawRoundedRectangle(rr, bBg.Get(), 2.0f);
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            } else {
                // Text (实心圆角胶囊)
                if (bBg) rt->FillRoundedRectangle(rr, bBg.Get());
                if (bBorder) rt->DrawRoundedRectangle(rr, bBorder.Get(), 1.3f);
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            }
            rt->EndDraw();
        }
    }

    cv::Mat pillBgra = cv::Mat(totalH, boxW, CV_8UC4, bits).clone();
    if (s_calloutCache.size() >= 12) {
        s_calloutCache.erase(s_calloutCache.begin());
    }
    s_calloutCache.push_back({kind, textUtf8, packedColor, innerBgr.rows, pillBgra});

    const int drawX = std::clamp(x - boxW / 2, 2, (std::max)(2, innerBgr.cols - boxW - 2));
    const int drawY = std::clamp(y - totalH / 2, 2, (std::max)(2, innerBgr.rows - totalH - 2));
    alphaBlendPremultipliedBgraOntoBgr(pillBgra, innerBgr, drawX, drawY);

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
}

void drawDashedSegmentBgr(cv::Mat& img, cv::Point p0, cv::Point p1, const cv::Scalar& col, int thick) {
    const double totalLen = std::hypot(static_cast<double>(p1.x - p0.x), static_cast<double>(p1.y - p0.y));
    if (totalLen < 2.0) return;
    const double dashLen = (std::max)(8.0, thick * 3.2);
    const double gapLen = (std::max)(6.0, thick * 2.2);
    const double ux = (p1.x - p0.x) / totalLen;
    const double uy = (p1.y - p0.y) / totalLen;
    for (double d = 0.0; d < totalLen; d += (dashLen + gapLen)) {
        const double dEnd = (std::min)(totalLen, d + dashLen);
        cv::Point a(static_cast<int>(std::round(p0.x + ux * d)), static_cast<int>(std::round(p0.y + uy * d)));
        cv::Point b(static_cast<int>(std::round(p0.x + ux * dEnd)), static_cast<int>(std::round(p0.y + uy * dEnd)));
        cv::line(img, a, b, col, thick, cv::LINE_AA);
    }
}

void drawRoundedBoxStrokeBgr(cv::Mat& img, cv::Rect rc, int radius, const cv::Scalar& col, int thick) {
    if (rc.width < 4 || rc.height < 4) return;
    const int r = std::clamp(radius, 0, (std::min)(rc.width, rc.height) / 2);
    if (r <= 1) {
        cv::rectangle(img, rc, col, thick, cv::LINE_AA);
        return;
    }
    cv::line(img, {rc.x + r, rc.y}, {rc.x + rc.width - r, rc.y}, col, thick, cv::LINE_AA);
    cv::line(img, {rc.x + r, rc.y + rc.height}, {rc.x + rc.width - r, rc.y + rc.height}, col, thick, cv::LINE_AA);
    cv::line(img, {rc.x, rc.y + r}, {rc.x, rc.y + rc.height - r}, col, thick, cv::LINE_AA);
    cv::line(img, {rc.x + rc.width, rc.y + r}, {rc.x + rc.width, rc.y + rc.height - r}, col, thick, cv::LINE_AA);
    cv::ellipse(img, {rc.x + r, rc.y + r}, {r, r}, 180, 0, 90, col, thick, cv::LINE_AA);
    cv::ellipse(img, {rc.x + rc.width - r, rc.y + r}, {r, r}, 270, 0, 90, col, thick, cv::LINE_AA);
    cv::ellipse(img, {rc.x + rc.width - r, rc.y + rc.height - r}, {r, r}, 0, 0, 90, col, thick, cv::LINE_AA);
    cv::ellipse(img, {rc.x + r, rc.y + rc.height - r}, {r, r}, 90, 0, 90, col, thick, cv::LINE_AA);
}

void renderTimelineAnnotationsOnInner(
    cv::Mat& innerBgr,
    const cv::Mat& fullSrcBgr,
    const std::vector<StudioAnnotationItem>& annotations,
    int64_t ptsMs,
    float srcCols, float srcRows,
    float effCropX, float effCropY,
    float effCropW, float effCropH) {
    if (innerBgr.empty() || annotations.empty()) return;
    const float safeCropW = (std::max)(1.0f, effCropW);
    const float safeCropH = (std::max)(1.0f, effCropH);
    const float iwF = static_cast<float>(innerBgr.cols);
    const float ihF = static_cast<float>(innerBgr.rows);
    const float strokeScale = std::clamp(ihF / 900.0f, 0.6f, 2.5f);

    for (const auto& ann : annotations) {
        if (!ann.enabled || ptsMs < ann.startMs || ptsMs > ann.endMs) continue;

        const float sx0 = ann.x0 * srcCols;
        const float sy0 = ann.y0 * srcRows;
        const float sx1 = ann.x1 * srcCols;
        const float sy1 = ann.y1 * srcRows;

        const int px0 = static_cast<int>(std::round(((sx0 - effCropX) / safeCropW) * iwF));
        const int py0 = static_cast<int>(std::round(((sy0 - effCropY) / safeCropH) * ihF));
        const int px1 = static_cast<int>(std::round(((sx1 - effCropX) / safeCropW) * iwF));
        const int py1 = static_cast<int>(std::round(((sy1 - effCropY) / safeCropH) * ihF));

        const cv::Scalar colBgr = parseHexBgrScalar(ann.colorHex, cv::Scalar(153, 72, 236));
        const int thick = (std::max)(2, static_cast<int>(std::round(ann.strokeWidth * strokeScale)));
        const double animPhase = static_cast<double>(ptsMs - ann.startMs) * 0.0075;

        // 1. 文本与步骤徽章家族 (6 款)
        if (ann.kind == StudioAnnotationKind::Text ||
            ann.kind == StudioAnnotationKind::TextPlain ||
            ann.kind == StudioAnnotationKind::TextBubble ||
            ann.kind == StudioAnnotationKind::TextDashed ||
            ann.kind == StudioAnnotationKind::TextNeon ||
            ann.kind == StudioAnnotationKind::StepBadge) {
            const int cx = (px0 + px1) / 2;
            const int cy = (py0 + py1) / 2;
            const std::string fallback = (ann.kind == StudioAnnotationKind::StepBadge) ? "1" : "Text";
            drawStyledTextCalloutBgr(innerBgr, cx, cy, ann.kind,
                                     ann.text.empty() ? fallback : ann.text, colBgr);
            continue;
        }

        // 2. 醒目粗体动态弹跳大箭头 (FocuSee 标志性玫粉色粗体指向大箭头)
        if (ann.kind == StudioAnnotationKind::BoldArrow) {
            const double dx = static_cast<double>(px1 - px0);
            const double dy = static_cast<double>(py1 - py0);
            const double len = std::hypot(dx, dy);
            if (len >= 8.0) {
                const double ux = dx / len;
                const double uy = dy / len;
                const double nx = -uy;
                const double ny = ux;
                // 沿指向方向的柔和正弦呼吸微弹跳，极强视觉引导力
                const double bounce = std::sin(animPhase) * 6.0 * strokeScale;
                const double bx0 = px0 + ux * bounce;
                const double by0 = py0 + uy * bounce;
                const double bx1 = px1 + ux * bounce;
                const double by1 = py1 + uy * bounce;

                const double headLen = std::clamp(len * 0.42, 18.0 * strokeScale, 54.0 * strokeScale);
                const double headHalfW = headLen * 0.62;
                const double shaftHalfW = headHalfW * 0.42;
                const double neckX = bx1 - ux * headLen;
                const double neckY = by1 - uy * headLen;

                std::vector<cv::Point> poly = {
                    cv::Point(static_cast<int>(std::round(bx0 + nx * shaftHalfW)), static_cast<int>(std::round(by0 + ny * shaftHalfW))),
                    cv::Point(static_cast<int>(std::round(neckX + nx * shaftHalfW)), static_cast<int>(std::round(neckY + ny * shaftHalfW))),
                    cv::Point(static_cast<int>(std::round(neckX + nx * headHalfW)), static_cast<int>(std::round(neckY + ny * headHalfW))),
                    cv::Point(static_cast<int>(std::round(bx1)), static_cast<int>(std::round(by1))),
                    cv::Point(static_cast<int>(std::round(neckX - nx * headHalfW)), static_cast<int>(std::round(neckY - ny * headHalfW))),
                    cv::Point(static_cast<int>(std::round(neckX - nx * shaftHalfW)), static_cast<int>(std::round(neckY - ny * shaftHalfW))),
                    cv::Point(static_cast<int>(std::round(bx0 - nx * shaftHalfW)), static_cast<int>(std::round(by0 - ny * shaftHalfW)))
                };
                std::vector<cv::Point> shadowPoly = poly;
                for (auto& p : shadowPoly) {
                    p.x += static_cast<int>(std::round(3.0 * strokeScale));
                    p.y += static_cast<int>(std::round(4.0 * strokeScale));
                }
                cv::Mat shOverlay = innerBgr.clone();
                cv::fillPoly(shOverlay, std::vector<std::vector<cv::Point>>{shadowPoly}, cv::Scalar(12, 14, 22), cv::LINE_AA);
                cv::addWeighted(shOverlay, 0.32, innerBgr, 0.68, 0.0, innerBgr);

                cv::fillPoly(innerBgr, std::vector<std::vector<cv::Point>>{poly}, colBgr, cv::LINE_AA);
                cv::polylines(innerBgr, std::vector<std::vector<cv::Point>>{poly}, true,
                              cv::Scalar(255, 255, 255), (std::max)(1, static_cast<int>(std::round(1.6f * strokeScale))), cv::LINE_AA);
            }
            continue;
        }

        // 3. 纤细指示箭头 / 发光直线 / 引导虚线
        if (ann.kind == StudioAnnotationKind::Arrow) {
            const double len = std::hypot(static_cast<double>(px1 - px0), static_cast<double>(py1 - py0));
            if (len >= 4.0) {
                const double tipRatio = std::clamp(16.0 * strokeScale / len, 0.12, 0.42);
                cv::arrowedLine(innerBgr, {px0, py0}, {px1, py1}, cv::Scalar(16, 20, 28), thick + 2, cv::LINE_AA, 0, tipRatio);
                cv::arrowedLine(innerBgr, {px0, py0}, {px1, py1}, colBgr, thick, cv::LINE_AA, 0, tipRatio);
            }
            continue;
        }
        if (ann.kind == StudioAnnotationKind::GlowLine) {
            cv::Mat glow = innerBgr.clone();
            cv::line(glow, {px0, py0}, {px1, py1}, colBgr, thick * 3 + 4, cv::LINE_AA);
            cv::addWeighted(glow, 0.35, innerBgr, 0.65, 0.0, innerBgr);
            cv::line(innerBgr, {px0, py0}, {px1, py1}, colBgr, thick + 1, cv::LINE_AA);
            continue;
        }
        if (ann.kind == StudioAnnotationKind::DashedLine) {
            drawDashedSegmentBgr(innerBgr, {px0, py0}, {px1, py1}, colBgr, thick + 1);
            continue;
        }

        // 4. 点击小手引导图标 (带同心扩散波纹环)
        if (ann.kind == StudioAnnotationKind::HandPointer) {
            const int hx = (px0 + px1) / 2;
            const int hy = (py0 + py1) / 2;
            const float s = std::clamp(strokeScale * 1.15f, 0.7f, 2.6f);
            const int pulseR = static_cast<int>(std::round((14.0 + 7.0 * (0.5 + 0.5 * std::sin(animPhase * 1.2))) * s));
            cv::Mat ringLayer = innerBgr.clone();
            cv::circle(ringLayer, {hx, hy - static_cast<int>(10 * s)}, pulseR, colBgr, (std::max)(2, thick), cv::LINE_AA);
            cv::addWeighted(ringLayer, 0.65, innerBgr, 0.35, 0.0, innerBgr);

            // 圆润白手套食指点击矢量多边形
            std::vector<cv::Point> handPts = {
                {hx - static_cast<int>(3 * s), hy - static_cast<int>(14 * s)},
                {hx + static_cast<int>(3 * s), hy - static_cast<int>(14 * s)},
                {hx + static_cast<int>(3 * s), hy - static_cast<int>(2 * s)},
                {hx + static_cast<int>(13 * s), hy},
                {hx + static_cast<int>(14 * s), hy + static_cast<int>(13 * s)},
                {hx + static_cast<int>(8 * s), hy + static_cast<int>(21 * s)},
                {hx - static_cast<int>(5 * s), hy + static_cast<int>(21 * s)},
                {hx - static_cast<int>(12 * s), hy + static_cast<int>(9 * s)},
                {hx - static_cast<int>(8 * s), hy + static_cast<int>(5 * s)},
                {hx - static_cast<int>(3 * s), hy + static_cast<int>(9 * s)}
            };
            cv::fillPoly(innerBgr, std::vector<std::vector<cv::Point>>{handPts}, cv::Scalar(252, 253, 255), cv::LINE_AA);
            cv::polylines(innerBgr, std::vector<std::vector<cv::Point>>{handPts}, true, cv::Scalar(24, 30, 42),
                          (std::max)(2, static_cast<int>(std::round(2.0f * s))), cv::LINE_AA);
            continue;
        }

        const int rx0 = std::clamp((std::min)(px0, px1), 0, innerBgr.cols);
        const int ry0 = std::clamp((std::min)(py0, py1), 0, innerBgr.rows);
        const int rx1 = std::clamp((std::max)(px0, px1), 0, innerBgr.cols);
        const int ry1 = std::clamp((std::max)(py0, py1), 0, innerBgr.rows);
        const cv::Rect rc(rx0, ry0, rx1 - rx0, ry1 - ry0);

        if (ann.kind == StudioAnnotationKind::BlurMask) {
            if (rc.width >= 4 && rc.height >= 4) {
                cv::Mat roi = innerBgr(rc);
                const int cell = std::clamp(static_cast<int>(std::round(12.0f * strokeScale)), 6, 24);
                const int pixW = (std::max)(2, rc.width / cell);
                const int pixH = (std::max)(2, rc.height / cell);
                cv::Mat tiny, mosaic;
                cv::resize(roi, tiny, cv::Size(pixW, pixH), 0.0, 0.0, cv::INTER_AREA);
                cv::resize(tiny, mosaic, rc.size(), 0.0, 0.0, cv::INTER_NEAREST);
                mosaic.copyTo(roi);
            }
        } else if (ann.kind == StudioAnnotationKind::Spotlight) {
            cv::Mat dimmed;
            innerBgr.convertTo(dimmed, -1, 0.36, 0.0);
            if (rc.width > 4 && rc.height > 4) {
                cv::Mat mask(innerBgr.rows, innerBgr.cols, CV_8UC1, cv::Scalar(0));
                fillRoundedRectMask(mask, rc, 10.0f * strokeScale);
                innerBgr.copyTo(dimmed, mask);
                drawRoundedBoxStrokeBgr(dimmed, rc, static_cast<int>(std::round(10.0f * strokeScale)),
                                        cv::Scalar(255, 255, 255), (std::max)(1, thick - 1));
            }
            dimmed.copyTo(innerBgr);
        } else if (ann.kind == StudioAnnotationKind::Magnifier) {
            // 圆形局部 2.2x 光学放大镜
            const int cx = (px0 + px1) / 2;
            const int cy = (py0 + py1) / 2;
            const int rad = std::clamp((std::max)(rc.width, rc.height) / 2,
                                       static_cast<int>(28 * strokeScale), (std::min)(innerBgr.cols, innerBgr.rows) / 2 - 4);
            const cv::Rect boxRc(
                std::clamp(cx - rad, 0, innerBgr.cols - 1),
                std::clamp(cy - rad, 0, innerBgr.rows - 1),
                std::clamp(cx + rad, 1, innerBgr.cols) - std::clamp(cx - rad, 0, innerBgr.cols - 1),
                std::clamp(cy + rad, 1, innerBgr.rows) - std::clamp(cy - rad, 0, innerBgr.rows - 1));
            if (boxRc.width >= 12 && boxRc.height >= 12 && !fullSrcBgr.empty()) {
                const float centerSrcX = ((ann.x0 + ann.x1) * 0.5f) * srcCols;
                const float centerSrcY = ((ann.y0 + ann.y1) * 0.5f) * srcRows;
                const float zoomMag = 2.2f;
                const float sampleHalfW = (static_cast<float>(rad) / iwF) * safeCropW / zoomMag;
                const float sampleHalfH = (static_cast<float>(rad) / ihF) * safeCropH / zoomMag;
                const int s0x = std::clamp(static_cast<int>(std::round(centerSrcX - sampleHalfW)), 0, fullSrcBgr.cols - 2);
                const int s0y = std::clamp(static_cast<int>(std::round(centerSrcY - sampleHalfH)), 0, fullSrcBgr.rows - 2);
                const int sw = std::clamp(static_cast<int>(std::round(sampleHalfW * 2.0f)), 2, fullSrcBgr.cols - s0x);
                const int sh = std::clamp(static_cast<int>(std::round(sampleHalfH * 2.0f)), 2, fullSrcBgr.rows - s0y);
                cv::Mat magnified;
                cv::resize(fullSrcBgr(cv::Rect(s0x, s0y, sw, sh)), magnified, cv::Size(rad * 2, rad * 2), 0.0, 0.0, cv::INTER_CUBIC);

                cv::Mat circleMask(innerBgr.rows, innerBgr.cols, CV_8UC1, cv::Scalar(0));
                cv::circle(circleMask, {cx, cy}, rad, cv::Scalar(255), cv::FILLED, cv::LINE_AA);

                const int mx0 = (std::max)(0, rad - cx);
                const int my0 = (std::max)(0, rad - cy);
                const cv::Rect magSub(mx0, my0, (std::min)(boxRc.width, magnified.cols - mx0), (std::min)(boxRc.height, magnified.rows - my0));
                if (magSub.width > 0 && magSub.height > 0) {
                    const cv::Rect dstSub(boxRc.x, boxRc.y, magSub.width, magSub.height);
                    magnified(magSub).copyTo(innerBgr(dstSub), circleMask(dstSub));
                }
                cv::circle(innerBgr, {cx, cy}, rad + 1, cv::Scalar(18, 22, 32), thick + 2, cv::LINE_AA);
                cv::circle(innerBgr, {cx, cy}, rad, cv::Scalar(255, 255, 255), (std::max)(2, thick), cv::LINE_AA);
            }
        } else if (ann.kind == StudioAnnotationKind::Rectangle) {
            if (rc.width >= 2 && rc.height >= 2) {
                cv::rectangle(innerBgr, rc, cv::Scalar(16, 20, 28), thick + 2, cv::LINE_AA);
                cv::rectangle(innerBgr, rc, colBgr, thick, cv::LINE_AA);
            }
        } else if (ann.kind == StudioAnnotationKind::RoundedBox) {
            if (rc.width >= 4 && rc.height >= 4) {
                const int rad = std::clamp(static_cast<int>(std::round(14.0f * strokeScale)), 4, (std::min)(rc.width, rc.height) / 3);
                drawRoundedBoxStrokeBgr(innerBgr, rc, rad, cv::Scalar(16, 20, 28), thick + 2);
                drawRoundedBoxStrokeBgr(innerBgr, rc, rad, colBgr, thick);
            }
        } else if (ann.kind == StudioAnnotationKind::CircleRing) {
            if (rc.width >= 4 && rc.height >= 4) {
                const cv::Point center(rc.x + rc.width / 2, rc.y + rc.height / 2);
                const cv::Size axes(rc.width / 2, rc.height / 2);
                cv::ellipse(innerBgr, center, axes, 0.0, 0.0, 360.0, cv::Scalar(16, 20, 28), thick + 2, cv::LINE_AA);
                cv::ellipse(innerBgr, center, axes, 0.0, 0.0, 360.0, colBgr, thick, cv::LINE_AA);
            }
        } else if (ann.kind == StudioAnnotationKind::SketchCircle) {
            // 双圈错位手绘涂鸦强调圈 + 弧尾小箭头
            if (rc.width >= 8 && rc.height >= 8) {
                const cv::Point c1(rc.x + rc.width / 2, rc.y + rc.height / 2);
                const cv::Point c2(c1.x + static_cast<int>(2 * strokeScale), c1.y - static_cast<int>(2 * strokeScale));
                const cv::Size ax1(rc.width / 2, rc.height / 2);
                const cv::Size ax2((std::max)(4, rc.width / 2 - 3), (std::max)(4, rc.height / 2 + 2));
                cv::ellipse(innerBgr, c1, ax1, -8.0, 18.0, 345.0, colBgr, thick + 1, cv::LINE_AA);
                cv::ellipse(innerBgr, c2, ax2, 6.0, 35.0, 385.0, colBgr, (std::max)(2, thick - 1), cv::LINE_AA);
            }
        }
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
    const double areaRatio = std::clamp(static_cast<double>(areaPx) / (1920.0 * 1080.0), 0.25, 6.0);
    const double fpsRatio = std::clamp(static_cast<double>(outFps) / 30.0, 0.5, 2.2);
    const double qScale = qualityScaleFactor(opts.quality);
    // 针对高占比纯色/文字桌面录屏调优目标码率（配合静态帧 VFR 抽帧，1080p 推荐档仅需 ~2.8 Mbps 峰值预算，体积直降 80%+）
    const int64_t targetBitrate = std::clamp(
        static_cast<int64_t>(std::llround(areaRatio * fpsRatio * qScale * (isHevc ? 1'900'000.0 : 2'800'000.0))),
        600'000LL, 60'000'000LL);

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
            attempt->gop_size = isMf ? 0 : outFps * 4;
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

    cv::Mat lastEncodedBgr;
    cv::Mat deferredTailBgr;
    int64_t lastEncodedPts = -1;
    int64_t lastObservedPts = -1;
    const int maxStaticSkipFrames = (std::max)(2, outFps); // 静态画面最多合并 1.0 秒内的重复帧

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

cv::Size StudioVideoCompositor::resolveOutputCanvasSize(
    int srcWidth,
    int srcHeight,
    StudioAspectRatio aspect) noexcept {
    int w = (std::max)(64, srcWidth);
    int h = (std::max)(64, srcHeight);
    if (aspect != StudioAspectRatio::Original) {
        double targetRatio = 16.0 / 9.0;
        switch (aspect) {
            case StudioAspectRatio::Ratio16x9: targetRatio = 16.0 / 9.0; break;
            case StudioAspectRatio::Ratio9x16: targetRatio = 9.0 / 16.0; break;
            case StudioAspectRatio::Ratio4x3:  targetRatio = 4.0 / 3.0;  break;
            case StudioAspectRatio::Ratio1x1:  targetRatio = 1.0;        break;
            default: break;
        }
        const int refLong = (std::max)(w, h);
        if (targetRatio >= 1.0) {
            w = refLong;
            h = (std::max)(64, static_cast<int>(std::round(static_cast<double>(refLong) / targetRatio)));
        } else {
            h = refLong;
            w = (std::max)(64, static_cast<int>(std::round(static_cast<double>(refLong) * targetRatio)));
        }
    }
    if (w & 1) ++w;
    if (h & 1) ++h;
    return {w, h};
}

cv::Size StudioVideoCompositor::resolveExportDimensions(
    int srcWidth,
    int srcHeight,
    const StudioExportOptions& exportOpts,
    StudioAspectRatio aspect) noexcept {
    const cv::Size baseCanvas = resolveOutputCanvasSize(srcWidth, srcHeight, aspect);
    const int safeSrcW = baseCanvas.width;
    const int safeSrcH = baseCanvas.height;
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
    } else if (exportOpts.format == StudioExportFormat::GIF_HD &&
               exportOpts.resolution != StudioExportResolution::Res640P &&
               exportOpts.resolution != StudioExportResolution::Res480P &&
               curLong < 1080) {
        // 微信客户端会将宽高均 <= 1000px 的 GIF 强制归类为“动态表情”从而禁止双击打开大图查看器放大；
        // 高清动图档 (GIF_HD) 自动保底长边 >= 1080px，确保发到微信按“普通图片”展示并 100% 支持双击放大查看细节
        const double upRatio = 1080.0 / static_cast<double>((std::max)(1, curLong));
        outW = static_cast<int>(std::round(safeSrcW * upRatio));
        outH = static_cast<int>(std::round(safeSrcH * upRatio));
    }
    return resolveOutputCanvasSize(outW, outH, StudioAspectRatio::Original);
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

    // MP4 / HEVC / WebM (配合静态帧智能 VFR 抽帧与桌面专属编码配置，实际平均码率极低)
    const int effFps = std::clamp(exportOpts.fps > 0 ? exportOpts.fps : 30, 10, 120);
    const double fpsRatio = std::clamp(static_cast<double>(effFps) / 30.0, 0.5, 2.2);
    const bool isHevc = (exportOpts.format == StudioExportFormat::MP4_HEVC);
    const double effMbps = std::clamp(areaRatio * fpsRatio * qScale * (isHevc ? 0.90 : 1.32), 0.35, 22.0);
    const uint64_t videoBytes = static_cast<uint64_t>(std::llround((effMbps * 1'000'000.0 / 8.0) * safeDur));
    const uint64_t audioBytes = static_cast<uint64_t>(std::llround(16'000.0 * safeDur));
    return (std::max<uint64_t>)(64ULL * 1024ULL, videoBytes + audioBytes);
}

void StudioVideoCompositor::renderArtisticBackdropBgr(
    cv::Mat& outBgr,
    int width,
    int height,
    BeautyBackgroundType theme,
    const std::string& customPathUtf8) {
    if (width <= 0 || height <= 0) return;

    // 1. 优先尝试加载用户选定的自定义壁纸或内置 resources/wallpapers 高清摄影/3D 丝绸壁纸
    if (!customPathUtf8.empty()) {
        const auto wpath = tools3000::core::WinUtils::utf8ToWstring(customPathUtf8);
        if (!wpath.empty()) {
            const cv::Mat customImg = decodeWallpaperFileCached(std::filesystem::path(wpath));
            if (!customImg.empty()) {
                cropResizeCoverBgr(customImg, outBgr, width, height);
                return;
            }
        }
    }
    {
        const auto builtinPath = resolveBuiltinWallpaperPath(theme);
        if (!builtinPath.empty()) {
            const cv::Mat builtinImg = decodeWallpaperFileCached(builtinPath);
            if (!builtinImg.empty()) {
                cropResizeCoverBgr(builtinImg, outBgr, width, height);
                return;
            }
        }
    }

    // 2. 兜底：若脱离资源目录运行单测，回退至程序化抗色带艺术背景

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

cv::Mat StudioVideoCompositor::renderCustomWallpaperThumbnailBgra(
    const std::string& customPathUtf8,
    int width,
    int height) {
    cv::Mat bgr;
    renderArtisticBackdropBgr(bgr, (std::max)(16, width), (std::max)(12, height),
                              BeautyBackgroundType::StudioSlate, customPathUtf8);
    cv::Mat bgra;
    cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
    return bgra;
}

void StudioVideoCompositor::ensureBackdropCache(
    int outW, int outH,
    bool autoFrame,
    BeautyBackgroundType theme,
    const std::string& customBgPath,
    float bgBlur,
    float shadowIntensity,
    int padX, int padY,
    int innerW, int innerH,
    float radius) {
    const float safeBlur = std::clamp(bgBlur, 0.0f, 48.0f);
    const float safeShadow = std::clamp(shadowIntensity, 0.0f, 1.0f);
    if (!m_cachedBackdropBgr.empty() &&
        m_cacheW == outW && m_cacheH == outH &&
        m_cacheAutoFrame == autoFrame &&
        m_cacheTheme == theme &&
        m_cacheCustomBgPath == customBgPath &&
        std::abs(m_cacheBgBlur - safeBlur) < 0.25f &&
        std::abs(m_cacheShadowIntensity - safeShadow) < 0.015f &&
        m_cachePadX == padX && m_cachePadY == padY &&
        m_cacheInnerW == innerW && m_cacheInnerH == innerH &&
        std::abs(m_cacheRadius - radius) < 0.2f) {
        return;
    }

    m_cacheW = outW;
    m_cacheH = outH;
    m_cacheAutoFrame = autoFrame;
    m_cacheTheme = theme;
    m_cacheCustomBgPath = customBgPath;
    m_cacheBgBlur = safeBlur;
    m_cacheShadowIntensity = safeShadow;
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

    renderArtisticBackdropBgr(m_cachedBackdropBgr, outW, outH, theme, customBgPath);

    // 可调节背景景深高斯模糊 (Background Blur: 0 ~ 40px)
    if (safeBlur > 0.4f && !m_cachedBackdropBgr.empty()) {
        int bKernel = static_cast<int>(std::round(safeBlur * (static_cast<float>(outH) / 480.0f))) * 2 + 1;
        bKernel = std::clamp(bKernel, 3, 121);
        if ((bKernel & 1) == 0) ++bKernel;
        cv::GaussianBlur(m_cachedBackdropBgr, m_cachedBackdropBgr, cv::Size(bKernel, bKernel), 0.0);
    }

    const bool lightTheme = customBgPath.empty() &&
        (theme == BeautyBackgroundType::PureMinimal ||
         theme == BeautyBackgroundType::PaperChalk ||
         theme == BeautyBackgroundType::AiryGlacier);

    // 可调节内框立体投影强度 (Inset Shadow: 0% ~ 100%)
    if (safeShadow > 0.01f && (padX > 0 || padY > 0)) {
        cv::Mat shadowMask(outH, outW, CV_8UC1, cv::Scalar(0));
        const int offsetY = (std::max)(3, static_cast<int>(std::round((outH / 64.0f) * (0.45f + 0.85f * safeShadow))));
        const cv::Rect shadowRect(
            padX,
            (std::min)(outH - innerH, padY + offsetY),
            innerW,
            innerH);
        fillRoundedRectMask(shadowMask, shadowRect, radius);
        int blurKernel = (std::max)(9, static_cast<int>(std::round((outH / 22.0f) * (0.4f + 0.9f * safeShadow))) | 1);
        if ((blurKernel & 1) == 0) ++blurKernel;
        cv::GaussianBlur(shadowMask, shadowMask, cv::Size(blurKernel, blurKernel), 0.0);

        const float baseMaxAlpha = lightTheme ? 0.34f : 0.64f;
        const float shadowMaxAlpha = baseMaxAlpha * safeShadow;

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

void StudioVideoCompositor::drawClickRippleEffect(
    cv::Mat& canvasBgr,
    float x, float y,
    float scale,
    float rippleProgress,
    bool rippleRight) {
    if (canvasBgr.empty() || rippleProgress < 0.0f || rippleProgress > 1.0f) return;
    if (x < -64.0f || y < -64.0f || x > canvasBgr.cols + 64.0f || y > canvasBgr.rows + 64.0f) return;

    constexpr int kShift = 4;
    constexpr float kSub = 16.0f;

    auto& cfg = tools3000::core::ConfigManager::instance();
    std::string colHex = rippleRight
        ? cfg.get<std::string>("/spotlight/rightClickColor", "#fb7185")
        : cfg.get<std::string>("/spotlight/leftClickColor", "auto");
    if (colHex == "auto" || colHex.empty()) {
        colHex = cfg.get<std::string>("/general/accentColor", "#38bdf8");
    }
    const std::string rippleStyle = cfg.get<std::string>("/spotlight/clickRippleStyle", "sparkle_burst");
    const cv::Scalar ringColor = parseHexBgrScalar(
        colHex, rippleRight ? cv::Scalar(133, 113, 251) : cv::Scalar(248, 189, 56));

    const float ease = 1.0f - (1.0f - rippleProgress) * (1.0f - rippleProgress);
    const float radius = (9.0f + 32.0f * ease) * std::clamp(scale, 0.85f, 1.8f);
    const float alpha = (1.0f - rippleProgress) * 0.68f;
    const int rInt = static_cast<int>(std::ceil(radius + 10.0f));
    const int rx0 = (std::max)(0, static_cast<int>(x) - rInt);
    const int ry0 = (std::max)(0, static_cast<int>(y) - rInt);
    const int rx1 = (std::min)(canvasBgr.cols, static_cast<int>(x) + rInt);
    const int ry1 = (std::min)(canvasBgr.rows, static_cast<int>(y) + rInt);
    if (rx1 > rx0 && ry1 > ry0 && alpha > 0.02f) {
        cv::Mat roi = canvasBgr(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0));
        cv::Mat overlay = roi.clone();
        const cv::Point localCenter(
            static_cast<int>(std::round((x - rx0) * kSub)),
            static_cast<int>(std::round((y - ry0) * kSub)));
        const int ringThick = (std::max)(2, static_cast<int>(std::round(2.4f * scale)));

        if (rippleStyle == "soft_glow") {
            cv::circle(overlay, localCenter, static_cast<int>(radius * 0.88f * kSub), ringColor, cv::FILLED, cv::LINE_AA, kShift);
        } else {
            cv::circle(overlay, localCenter, static_cast<int>(radius * kSub), ringColor, ringThick, cv::LINE_AA, kShift);
            if (rippleStyle == "target_pulse" || rippleStyle == "sparkle_burst") {
                cv::circle(overlay, localCenter, static_cast<int>(radius * 0.54f * kSub), ringColor,
                           (std::max)(1, ringThick - 1), cv::LINE_AA, kShift);
            }
            if (rippleStyle == "sparkle_burst" || rippleStyle == "supernova") {
                for (int ray = 0; ray < 8; ++ray) {
                    const float ang = static_cast<float>(ray) * (3.14159265f * 0.25f) + ease * 0.35f;
                    const float cs = std::cos(ang);
                    const float sn = std::sin(ang);
                    const float rIn = radius * 0.72f;
                    const float rOut = radius * 1.12f;
                    const cv::Point pA(
                        static_cast<int>(std::round(((x - rx0) + cs * rIn) * kSub)),
                        static_cast<int>(std::round(((y - ry0) + sn * rIn) * kSub)));
                    const cv::Point pB(
                        static_cast<int>(std::round(((x - rx0) + cs * rOut) * kSub)),
                        static_cast<int>(std::round(((y - ry0) + sn * rOut) * kSub)));
                    cv::line(overlay, pA, pB, ringColor, (std::max)(1, ringThick - 1), cv::LINE_AA, kShift);
                }
            }
        }
        cv::addWeighted(overlay, rippleStyle == "soft_glow" ? alpha * 0.52 : alpha,
                        roi, 1.0 - (rippleStyle == "soft_glow" ? alpha * 0.52 : alpha), 0.0, roi);
    }
}

void StudioVideoCompositor::drawCrispVectorCursor(
    cv::Mat& canvasBgr,
    float x, float y,
    StudioCursorKind kind,
    float scale,
    float rippleProgress,
    bool rippleRight,
    bool highlightHalo) {
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

    if (highlightHalo) {
        const float haloR = 24.0f * std::clamp(scale, 0.8f, 1.8f);
        const int hr = static_cast<int>(std::ceil(haloR + 2.0f));
        const int hx0 = (std::max)(0, static_cast<int>(x) - hr);
        const int hy0 = (std::max)(0, static_cast<int>(y) - hr);
        const int hx1 = (std::min)(canvasBgr.cols, static_cast<int>(x) + hr);
        const int hy1 = (std::min)(canvasBgr.rows, static_cast<int>(y) + hr);
        if (hx1 > hx0 && hy1 > hy0) {
            cv::Mat roi = canvasBgr(cv::Rect(hx0, hy0, hx1 - hx0, hy1 - hy0));
            cv::Mat ov = roi.clone();
            cv::circle(
                ov,
                cv::Point(static_cast<int>(std::round((x - hx0) * kSub)), static_cast<int>(std::round((y - hy0) * kSub))),
                static_cast<int>(haloR * kSub),
                cv::Scalar(94, 212, 255),
                cv::FILLED,
                cv::LINE_AA,
                kShift);
            cv::addWeighted(ov, 0.24, roi, 0.76, 0.0, roi);
        }
    }

    if (rippleProgress >= 0.0f && rippleProgress <= 1.0f) {
        drawClickRippleEffect(canvasBgr, x, y, scale, rippleProgress, rippleRight);
    }

    if (kind == StudioCursorKind::IBeam) {
        const int thickBg = (std::max)(3, static_cast<int>(std::round(3.4f * scale)));
        const int thickFg = (std::max)(1, static_cast<int>(std::round(1.5f * scale)));
        const cv::Point topL = toSubPt(-4.2f, -9.2f);
        const cv::Point topR = toSubPt(4.2f, -9.2f);
        const cv::Point midT = toSubPt(0.0f, -9.2f);
        const cv::Point midB = toSubPt(0.0f, 9.2f);
        const cv::Point botL = toSubPt(-4.2f, 9.2f);
        const cv::Point botR = toSubPt(4.2f, 9.2f);

        cv::line(canvasBgr, topL, topR, cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, midT, midB, cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, botL, botR, cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);

        cv::line(canvasBgr, topL, topR, cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, midT, midB, cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, botL, botR, cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        return;
    }

    if (kind == StudioCursorKind::Cross) {
        const int thickBg = (std::max)(3, static_cast<int>(std::round(3.2f * scale)));
        const int thickFg = (std::max)(1, static_cast<int>(std::round(1.4f * scale)));
        cv::line(canvasBgr, toSubPt(-8.8f, 0.0f), toSubPt(8.8f, 0.0f), cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(0.0f, -8.8f), toSubPt(0.0f, 8.8f), cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(-8.8f, 0.0f), toSubPt(8.8f, 0.0f), cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(0.0f, -8.8f), toSubPt(0.0f, 8.8f), cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        return;
    }

    if (kind == StudioCursorKind::Hand) {
        // 真实点击食指小手矢量图标：食指指尖热点严格锚定于 (0.0f, 0.0f)
        static constexpr std::array<std::pair<float, float>, 18> kHandShape = {{
            { 0.0f,  0.0f}, // 食指顶端热点
            { 1.6f,  0.6f},
            { 1.9f,  7.4f},
            { 3.3f,  6.8f}, // 中指峰
            { 4.9f,  7.5f},
            { 6.3f,  7.4f}, // 无名指峰
            { 7.8f,  8.3f},
            { 9.1f,  8.6f}, // 小指峰
            {10.2f,  9.8f},
            {10.0f, 15.2f},
            { 8.2f, 19.4f}, // 手掌右下腕部
            {-1.5f, 19.4f}, // 手掌左下腕部
            {-4.1f, 15.6f},
            {-6.4f, 11.4f}, // 拇指外侧
            {-5.6f,  9.4f}, // 拇指尖
            {-3.6f,  9.8f},
            {-1.8f, 11.4f}, // 虎口
            {-1.7f,  0.6f},
        }};

        std::vector<cv::Point> handShadow;
        std::vector<cv::Point> handPoly;
        handShadow.reserve(kHandShape.size());
        handPoly.reserve(kHandShape.size());
        for (const auto& [px, py] : kHandShape) {
            handShadow.push_back(toSubPt(px + 1.0f, py + 1.5f));
            handPoly.push_back(toSubPt(px, py));
        }

        const int pad = static_cast<int>(std::ceil(26.0f * scale));
        const int sx0 = (std::max)(0, static_cast<int>(x) - pad);
        const int sy0 = (std::max)(0, static_cast<int>(y) - 6);
        const int sx1 = (std::min)(canvasBgr.cols, static_cast<int>(x) + pad);
        const int sy1 = (std::min)(canvasBgr.rows, static_cast<int>(y) + pad);
        if (sx1 > sx0 && sy1 > sy0) {
            cv::Mat roi = canvasBgr(cv::Rect(sx0, sy0, sx1 - sx0, sy1 - sy0));
            cv::Mat shLayer = roi.clone();
            for (auto& pt : handShadow) {
                pt.x -= sx0 * static_cast<int>(kSub);
                pt.y -= sy0 * static_cast<int>(kSub);
            }
            cv::fillPoly(shLayer, std::vector<std::vector<cv::Point>>{handShadow}, cv::Scalar(10, 12, 18), cv::LINE_AA, kShift);
            cv::addWeighted(shLayer, 0.25, roi, 0.75, 0.0, roi);
        }

        const int outlineThick = (std::max)(2, static_cast<int>(std::round(1.9f * scale)));
        cv::polylines(canvasBgr, std::vector<std::vector<cv::Point>>{handPoly}, true,
                      cv::Scalar(18, 22, 32), outlineThick, cv::LINE_AA, kShift);
        cv::fillPoly(canvasBgr, std::vector<std::vector<cv::Point>>{handPoly},
                     cv::Scalar(252, 253, 255), cv::LINE_AA, kShift);
        // 指缝短刻线增强手指立体层次感
        const int seamThick = (std::max)(1, static_cast<int>(std::round(1.1f * scale)));
        cv::line(canvasBgr, toSubPt(1.9f, 7.2f), toSubPt(1.9f, 11.2f), cv::Scalar(36, 42, 56), seamThick, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(5.0f, 7.6f), toSubPt(5.0f, 11.4f), cv::Scalar(36, 42, 56), seamThick, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(7.9f, 8.5f), toSubPt(7.9f, 11.8f), cv::Scalar(36, 42, 56), seamThick, cv::LINE_AA, kShift);
        return;
    }

    // 标准指针箭头：尖端严格锚定在 (0.0f, 0.0f)，消除粗白边向左上圆头外溢造成的尖端视觉右下偏移
    static constexpr std::array<std::pair<float, float>, 7> kArrowOuter = {{
        { 0.0f,  0.0f},
        { 0.0f, 18.8f},
        { 4.6f, 14.5f},
        { 7.9f, 21.4f},
        {10.7f, 20.1f},
        { 7.4f, 13.1f},
        {13.4f, 13.1f},
    }};
    static constexpr std::array<std::pair<float, float>, 7> kArrowInner = {{
        { 0.85f,  1.95f},
        { 0.85f, 16.75f},
        { 4.75f, 13.10f},
        { 8.15f, 20.15f},
        { 9.55f, 19.45f},
        { 6.15f, 12.25f},
        {11.45f, 12.25f},
    }};

    std::vector<cv::Point> shadowPoly;
    std::vector<cv::Point> outerPoly;
    std::vector<cv::Point> innerPoly;
    shadowPoly.reserve(kArrowOuter.size());
    outerPoly.reserve(kArrowOuter.size());
    innerPoly.reserve(kArrowInner.size());
    for (const auto& [px, py] : kArrowOuter) {
        shadowPoly.push_back(toSubPt(px + 1.0f, py + 1.5f));
        outerPoly.push_back(toSubPt(px, py));
    }
    for (const auto& [px, py] : kArrowInner) {
        innerPoly.push_back(toSubPt(px, py));
    }

    const int pad = static_cast<int>(std::ceil(26.0f * scale));
    const int sx0 = (std::max)(0, static_cast<int>(x) - 4);
    const int sy0 = (std::max)(0, static_cast<int>(y) - 4);
    const int sx1 = (std::min)(canvasBgr.cols, static_cast<int>(x) + pad);
    const int sy1 = (std::min)(canvasBgr.rows, static_cast<int>(y) + pad);
    if (sx1 > sx0 && sy1 > sy0) {
        cv::Mat roi = canvasBgr(cv::Rect(sx0, sy0, sx1 - sx0, sy1 - sy0));
        cv::Mat shLayer = roi.clone();
        for (auto& pt : shadowPoly) {
            pt.x -= sx0 * static_cast<int>(kSub);
            pt.y -= sy0 * static_cast<int>(kSub);
        }
        cv::fillPoly(shLayer, std::vector<std::vector<cv::Point>>{shadowPoly}, cv::Scalar(10, 12, 18), cv::LINE_AA, kShift);
        cv::addWeighted(shLayer, 0.26, roi, 0.74, 0.0, roi);
    }

    cv::fillPoly(canvasBgr, std::vector<std::vector<cv::Point>>{outerPoly}, cv::Scalar(255, 255, 255), cv::LINE_AA, kShift);
    cv::polylines(canvasBgr, std::vector<std::vector<cv::Point>>{outerPoly}, true,
                  cv::Scalar(255, 255, 255), 1, cv::LINE_AA, kShift);
    cv::fillPoly(canvasBgr, std::vector<std::vector<cv::Point>>{innerPoly}, cv::Scalar(18, 22, 32), cv::LINE_AA, kShift);
}

cv::Mat StudioVideoCompositor::composeFrame(
    const cv::Mat& rawBgrOrBgra,
    const StudioEvaluatedFrame& eval,
    const StudioRenderConfig& config,
    cv::Size targetSize,
    const std::vector<StudioAnnotationItem>* annotations) {
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
        ? resolveOutputCanvasSize(targetSize.width, targetSize.height, StudioAspectRatio::Original)
        : resolveOutputCanvasSize(srcBgr.cols, srcBgr.rows, config.aspectRatio);
    const int outW = outSize.width;
    const int outH = outSize.height;

    int padX = 0;
    int padY = 0;
    int innerW = outW;
    int innerH = outH;
    float cornerR = 0.0f;

    const bool hasAspectPad = (config.aspectRatio != StudioAspectRatio::Original);
    if (config.autoFrameEnabled || hasAspectPad) {
        const float safeRatio = config.autoFrameEnabled
            ? std::clamp(config.paddingRatio, 0.0f, 0.18f)
            : 0.0f;
        const int basePad = (safeRatio > 0.002f)
            ? (std::max)(8, static_cast<int>(std::round((std::min)(outW, outH) * safeRatio)))
            : 0;
        const int availW = (std::max)(32, outW - basePad * 2);
        const int availH = (std::max)(32, outH - basePad * 2);
        const float scaleFit = (std::min)(
            static_cast<float>(availW) / static_cast<float>((std::max)(1, srcBgr.cols)),
            static_cast<float>(availH) / static_cast<float>((std::max)(1, srcBgr.rows)));
        innerW = (std::max)(16, static_cast<int>(std::round(srcBgr.cols * scaleFit)));
        innerH = (std::max)(16, static_cast<int>(std::round(srcBgr.rows * scaleFit)));
        padX = (outW - innerW) / 2;
        padY = (outH - innerH) / 2;
        cornerR = config.autoFrameEnabled
            ? std::clamp(config.cornerRadius * (static_cast<float>(outH) / 1080.0f), 0.0f, 48.0f)
            : 0.0f;
    }

    const bool has3DTilt =
        (std::abs(eval.camera.tiltYawDeg) > 0.08f || std::abs(eval.camera.tiltPitchDeg) > 0.08f);
    ensureBackdropCache(
        outW, outH,
        config.autoFrameEnabled || hasAspectPad,
        config.backgroundTheme,
        config.customBackgroundPath,
        config.backgroundBlur,
        has3DTilt ? (config.shadowIntensity * 0.45f) : config.shadowIntensity,
        padX, padY, innerW, innerH, cornerR);
    cv::Mat canvas = m_cachedBackdropBgr.clone();

    float effCropX = 0.0f;
    float effCropY = 0.0f;
    float effCropW = static_cast<float>(srcBgr.cols);
    float effCropH = static_cast<float>(srcBgr.rows);
    const float refSrcW = (eval.sourceWidth > 1.0f)
        ? eval.sourceWidth
        : (std::max)(1.0f, eval.camera.cropW * eval.camera.zoom);
    const float refSrcH = (eval.sourceHeight > 1.0f)
        ? eval.sourceHeight
        : (std::max)(1.0f, eval.camera.cropH * eval.camera.zoom);
    if (eval.camera.cropW > 0.5f && eval.camera.cropH > 0.5f && eval.camera.zoom >= 0.99f) {
        const float scaleRatioX = static_cast<float>(srcBgr.cols) / refSrcW;
        const float scaleRatioY = static_cast<float>(srcBgr.rows) / refSrcH;

        effCropX = std::clamp(eval.camera.cropX * scaleRatioX, 0.0f, static_cast<float>(srcBgr.cols) - 1.0f);
        effCropY = std::clamp(eval.camera.cropY * scaleRatioY, 0.0f, static_cast<float>(srcBgr.rows) - 1.0f);
        effCropW = std::clamp(eval.camera.cropW * scaleRatioX, 1.0f, static_cast<float>(srcBgr.cols) - effCropX);
        effCropH = std::clamp(eval.camera.cropH * scaleRatioY, 1.0f, static_cast<float>(srcBgr.rows) - effCropY);
    }

    cv::Mat zoomedInner;
    const bool isFullUnzoomed = (effCropX <= 0.01f && effCropY <= 0.01f &&
                                 std::abs(effCropW - static_cast<float>(srcBgr.cols)) <= 0.05f &&
                                 std::abs(effCropH - static_cast<float>(srcBgr.rows)) <= 0.05f);
    if (isFullUnzoomed) {
        const int interp = (innerW < srcBgr.cols || innerH < srcBgr.rows) ? cv::INTER_AREA : cv::INTER_LINEAR;
        cv::resize(srcBgr, zoomedInner, cv::Size(innerW, innerH), 0.0, 0.0, interp);
    } else {
        // 亚像素半像素中心对齐逆仿射重采样：彻底消除整数 cv::Rect 裁剪导致的 2~6px 鼠标/画面错位与运镜阶梯抖动
        cv::Mat warpSrc = srcBgr;
        double adjCropX = static_cast<double>(effCropX);
        double adjCropY = static_cast<double>(effCropY);
        double adjSx = static_cast<double>(effCropW) / static_cast<double>((std::max)(1, innerW));
        double adjSy = static_cast<double>(effCropH) / static_cast<double>((std::max)(1, innerH));
        if (adjSx > 1.75 && adjSy > 1.75) {
            const double preScale = 1.0 / std::floor((std::min)(adjSx, adjSy));
            if (preScale >= 0.12 && preScale <= 0.75) {
                cv::resize(srcBgr, warpSrc, cv::Size(), preScale, preScale, cv::INTER_AREA);
                const double rsX = static_cast<double>(warpSrc.cols) / static_cast<double>(srcBgr.cols);
                const double rsY = static_cast<double>(warpSrc.rows) / static_cast<double>(srcBgr.rows);
                adjCropX = static_cast<double>(effCropX) * rsX;
                adjCropY = static_cast<double>(effCropY) * rsY;
                adjSx = (static_cast<double>(effCropW) * rsX) / static_cast<double>((std::max)(1, innerW));
                adjSy = (static_cast<double>(effCropH) * rsY) / static_cast<double>((std::max)(1, innerH));
            }
        }
        const cv::Mat invM = (cv::Mat_<double>(2, 3) <<
            adjSx, 0.0, adjCropX + 0.5 * adjSx - 0.5,
            0.0, adjSy, adjCropY + 0.5 * adjSy - 0.5);
        cv::warpAffine(warpSrc, zoomedInner, invM, cv::Size(innerW, innerH),
                       cv::WARP_INVERSE_MAP | cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    }

    // 1. 渲染时间轴视频标注与隐私遮罩/光学放大镜（锚定源坐标，跟随 2D/3D 运镜无缝缩放平移）
    if (annotations && !annotations->empty()) {
        renderTimelineAnnotationsOnInner(
            zoomedInner, srcBgr, *annotations, eval.ptsMs,
            static_cast<float>(srcBgr.cols), static_cast<float>(srcBgr.rows),
            effCropX, effCropY, effCropW, effCropH);
    }

    // 1.5 可选微晶彩色内边框 (Inset Border: 0 ~ 12px)
    if (config.autoFrameEnabled && config.insetBorderWidth > 0.25f) {
        const float borderScale = std::clamp(static_cast<float>(outH) / 1080.0f, 0.55f, 2.2f);
        const int bThick = (std::max)(1, static_cast<int>(std::round(config.insetBorderWidth * borderScale)));
        const cv::Scalar bdCol = parseHexBgrScalar(config.insetBorderColorHex, cv::Scalar(255, 255, 255));
        cv::Mat bdLayer = zoomedInner.clone();
        const int inset = bThick / 2;
        const cv::Rect bdRc(inset, inset, (std::max)(4, innerW - inset * 2 - 1), (std::max)(4, innerH - inset * 2 - 1));
        drawRoundedBoxStrokeBgr(bdLayer, bdRc, static_cast<int>(std::round(cornerR)), bdCol, bThick);
        cv::addWeighted(bdLayer, 0.72, zoomedInner, 0.28, 0.0, zoomedInner);
    }

    // 2. 镜头高速推拉摇移时的电影级运动模糊 (Camera Motion Blur)
    if (config.motionBlurEnabled && eval.cameraVelocityPx > 14.0f) {
        int mbKernel = static_cast<int>(std::round((eval.cameraVelocityPx - 10.0f) * 0.14f)) * 2 + 1;
        mbKernel = std::clamp(mbKernel, 3, 9);
        if ((mbKernel & 1) == 0) ++mbKernel;
        cv::blur(zoomedInner, zoomedInner, cv::Size(mbKernel, mbKernel));
    }

    // 3. 若处于 3D 运镜透视模式，先把光标与点击水波纹画在内框平面内一起执行 3D 透视投影；2D 模式则画在顶层保持边缘零裁剪
    auto renderCursorOnto = [&](cv::Mat& targetSurface, int offsetX, int offsetY) {
        const float safeCropW = (eval.camera.cropW > 0.5f) ? eval.camera.cropW : refSrcW;
        const float safeCropH = (eval.camera.cropH > 0.5f) ? eval.camera.cropH : refSrcH;
        const float dpiRatio = std::clamp(static_cast<float>(outH) / 900.0f, 0.55f, 2.2f);

        // 点击水波纹独立锚定在真实点击源坐标 (rippleSrcX, rippleSrcY)，即使点击后鼠标迅速移开也绝不漂移
        if (eval.rippleProgress >= 0.0f && eval.rippleProgress <= 1.0f) {
            const float rNormX = (eval.rippleSrcX - eval.camera.cropX) / (std::max)(1.0f, safeCropW);
            const float rNormY = (eval.rippleSrcY - eval.camera.cropY) / (std::max)(1.0f, safeCropH);
            if (rNormX >= -0.08f && rNormX <= 1.08f && rNormY >= -0.08f && rNormY <= 1.08f) {
                const float ripX = static_cast<float>(offsetX) + rNormX * static_cast<float>(innerW);
                const float ripY = static_cast<float>(offsetY) + rNormY * static_cast<float>(innerH);
                drawClickRippleEffect(
                    targetSurface,
                    ripX,
                    ripY,
                    config.cursorScale * dpiRatio,
                    eval.rippleProgress,
                    eval.rippleIsRight);
            }
        }

        if (!config.smoothCursorEnabled || eval.cursorKind == StudioCursorKind::Hidden) return;
        const float normX = (eval.smoothCursorX - eval.camera.cropX) / (std::max)(1.0f, safeCropW);
        const float normY = (eval.smoothCursorY - eval.camera.cropY) / (std::max)(1.0f, safeCropH);
        if (normX >= -0.02f && normX <= 1.02f && normY >= -0.02f && normY <= 1.02f) {
            const float curX = static_cast<float>(offsetX) + normX * static_cast<float>(innerW);
            const float curY = static_cast<float>(offsetY) + normY * static_cast<float>(innerH);
            drawCrispVectorCursor(
                targetSurface,
                curX,
                curY,
                eval.cursorKind,
                config.cursorScale * eval.clickScale * dpiRatio,
                -1.0f, // 水波纹已按真实点击源坐标独立绘制
                eval.rippleIsRight,
                config.cursorHighlightHalo);
        }
    };

    if (has3DTilt && (config.autoFrameEnabled || hasAspectPad)) {
        renderCursorOnto(zoomedInner, 0, 0);

        cv::Mat alphaMask(innerH, innerW, CV_8UC1, cv::Scalar(0));
        fillRoundedRectMask(alphaMask, cv::Rect(0, 0, innerW, innerH), cornerR);

        const float yawRad = eval.camera.tiltYawDeg * (3.14159265f / 180.0f);
        const float pitchRad = eval.camera.tiltPitchDeg * (3.14159265f / 180.0f);
        const float cy = std::cos(yawRad), sy = std::sin(yawRad);
        const float cp = std::cos(pitchRad), sp = std::sin(pitchRad);
        const float focal = 1.68f * static_cast<float>((std::max)(outW, outH));
        const float halfW = static_cast<float>(innerW) * 0.5f;
        const float halfH = static_cast<float>(innerH) * 0.5f;
        const float centerX = static_cast<float>(padX) + halfW;
        const float centerY = static_cast<float>(padY) + halfH;

        auto projectCorner = [&](float lx, float ly) -> cv::Point2f {
            // 先绕 X 轴 Pitch，再绕 Y 轴 Yaw
            const float y1 = ly * cp;
            const float z1 = ly * sp;
            const float x2 = lx * cy + z1 * sy;
            const float z2 = -lx * sy + z1 * cy;
            const float persp = focal / (std::max)(focal * 0.35f, focal + z2);
            return {centerX + x2 * persp, centerY + y1 * persp};
        };

        const std::array<cv::Point2f, 4> srcQuad = {{
            {0.0f, 0.0f},
            {static_cast<float>(innerW), 0.0f},
            {static_cast<float>(innerW), static_cast<float>(innerH)},
            {0.0f, static_cast<float>(innerH)}
        }};
        const std::array<cv::Point2f, 4> dstQuad = {{
            projectCorner(-halfW, -halfH),
            projectCorner( halfW, -halfH),
            projectCorner( halfW,  halfH),
            projectCorner(-halfW,  halfH)
        }};

        const cv::Mat H = cv::getPerspectiveTransform(srcQuad.data(), dstQuad.data());
        cv::Mat warpedBgr, warpedMask;
        cv::warpPerspective(zoomedInner, warpedBgr, H, cv::Size(outW, outH), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        cv::warpPerspective(alphaMask, warpedMask, H, cv::Size(outW, outH), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));

        for (int y = 0; y < outH; ++y) {
            uint8_t* dstRow = canvas.ptr<uint8_t>(y);
            const uint8_t* fgRow = warpedBgr.ptr<uint8_t>(y);
            const uint8_t* mRow = warpedMask.ptr<uint8_t>(y);
            for (int x = 0; x < outW; ++x) {
                const uint32_t a = mRow[x];
                if (a == 255) {
                    dstRow[x * 3 + 0] = fgRow[x * 3 + 0];
                    dstRow[x * 3 + 1] = fgRow[x * 3 + 1];
                    dstRow[x * 3 + 2] = fgRow[x * 3 + 2];
                } else if (a > 0) {
                    const uint32_t inv = 255 - a;
                    dstRow[x * 3 + 0] = static_cast<uint8_t>((fgRow[x * 3 + 0] * a + dstRow[x * 3 + 0] * inv + 127) / 255);
                    dstRow[x * 3 + 1] = static_cast<uint8_t>((fgRow[x * 3 + 1] * a + dstRow[x * 3 + 1] * inv + 127) / 255);
                    dstRow[x * 3 + 2] = static_cast<uint8_t>((fgRow[x * 3 + 2] * a + dstRow[x * 3 + 2] * inv + 127) / 255);
                }
            }
        }
    } else {
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
        renderCursorOnto(canvas, padX, padY);
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

    auto computeFramePeak = [](const AVFrame* f) -> float {
        if (!f || f->nb_samples <= 0) return 0.0f;
        const int chs = (std::max)(1, f->ch_layout.nb_channels);
        const int n = f->nb_samples;
        const auto fmt = static_cast<AVSampleFormat>(f->format);
        const bool planar = av_sample_fmt_is_planar(fmt) != 0;
        float pk = 0.0f;

        if (fmt == AV_SAMPLE_FMT_FLT || fmt == AV_SAMPLE_FMT_FLTP) {
            if (planar) {
                for (int c = 0; c < chs; ++c) {
                    const auto* p = reinterpret_cast<const float*>(f->extended_data ? f->extended_data[c] : f->data[c]);
                    if (!p) continue;
                    for (int i = 0; i < n; ++i) pk = (std::max)(pk, std::abs(p[i]));
                }
            } else {
                const auto* p = reinterpret_cast<const float*>(f->data[0]);
                if (p) {
                    const int total = n * chs;
                    for (int i = 0; i < total; ++i) pk = (std::max)(pk, std::abs(p[i]));
                }
            }
        } else if (fmt == AV_SAMPLE_FMT_S16 || fmt == AV_SAMPLE_FMT_S16P) {
            constexpr float kInv = 1.0f / 32768.0f;
            if (planar) {
                for (int c = 0; c < chs; ++c) {
                    const auto* p = reinterpret_cast<const int16_t*>(f->extended_data ? f->extended_data[c] : f->data[c]);
                    if (!p) continue;
                    for (int i = 0; i < n; ++i) pk = (std::max)(pk, std::abs(static_cast<float>(p[i])) * kInv);
                }
            } else {
                const auto* p = reinterpret_cast<const int16_t*>(f->data[0]);
                if (p) {
                    const int total = n * chs;
                    for (int i = 0; i < total; ++i) pk = (std::max)(pk, std::abs(static_cast<float>(p[i])) * kInv);
                }
            }
        } else if (fmt == AV_SAMPLE_FMT_S32 || fmt == AV_SAMPLE_FMT_S32P) {
            constexpr float kInv = 1.0f / 2147483648.0f;
            if (planar) {
                for (int c = 0; c < chs; ++c) {
                    const auto* p = reinterpret_cast<const int32_t*>(f->extended_data ? f->extended_data[c] : f->data[c]);
                    if (!p) continue;
                    for (int i = 0; i < n; ++i) pk = (std::max)(pk, std::abs(static_cast<float>(p[i])) * kInv);
                }
            } else {
                const auto* p = reinterpret_cast<const int32_t*>(f->data[0]);
                if (p) {
                    const int total = n * chs;
                    for (int i = 0; i < total; ++i) pk = (std::max)(pk, std::abs(static_cast<float>(p[i])) * kInv);
                }
            }
        } else if (fmt == AV_SAMPLE_FMT_DBL || fmt == AV_SAMPLE_FMT_DBLP) {
            if (planar) {
                for (int c = 0; c < chs; ++c) {
                    const auto* p = reinterpret_cast<const double*>(f->extended_data ? f->extended_data[c] : f->data[c]);
                    if (!p) continue;
                    for (int i = 0; i < n; ++i) pk = (std::max)(pk, static_cast<float>(std::abs(p[i])));
                }
            } else {
                const auto* p = reinterpret_cast<const double*>(f->data[0]);
                if (p) {
                    const int total = n * chs;
                    for (int i = 0; i < total; ++i) pk = (std::max)(pk, static_cast<float>(std::abs(p[i])));
                }
            }
        }
        return std::clamp(pk, 0.0f, 1.0f);
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
            framePeaks.emplace_back(tSec + frameSpanSec * 0.5, computeFramePeak(frame));
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
    for (const auto& [tCenterSec, pk] : framePeaks) {
        const double halfSpan = (std::max)(0.011, 1024.0 / (2.0 * static_cast<double>(sampleRate)));
        const double t0 = std::clamp((tCenterSec - halfSpan) / finalDur, 0.0, 0.999999);
        const double t1 = std::clamp((tCenterSec + halfSpan) / finalDur, t0, 0.999999);
        const size_t b0 = std::min<size_t>(safeBuckets - 1, static_cast<size_t>(t0 * static_cast<double>(safeBuckets)));
        const size_t b1 = std::min<size_t>(safeBuckets - 1, static_cast<size_t>(t1 * static_cast<double>(safeBuckets)));
        for (size_t b = b0; b <= b1; ++b) {
            outPeaks[b] = (std::max)(outPeaks[b], pk);
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

} // namespace tools3000::capture

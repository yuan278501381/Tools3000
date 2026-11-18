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

void alphaBlendPremultipliedBgraTransformedOntoBgr(
    const cv::Mat& srcBgra,
    cv::Mat& dstBgr,
    int centerX,
    int centerY,
    float scale,
    float opacity) {
    if (srcBgra.empty() || srcBgra.type() != CV_8UC4 || dstBgr.empty() || dstBgr.type() != CV_8UC3) return;
    const float clampedOp = std::clamp(opacity, 0.0f, 1.0f);
    if (clampedOp <= 0.005f) return;

    cv::Mat activeBgra = srcBgra;
    const float clampedScale = std::clamp(scale, 0.25f, 2.2f);
    if (std::abs(clampedScale - 1.0f) > 0.012f) {
        const int sw = (std::max)(4, static_cast<int>(std::round(srcBgra.cols * clampedScale)));
        const int sh = (std::max)(4, static_cast<int>(std::round(srcBgra.rows * clampedScale)));
        cv::resize(srcBgra, activeBgra, cv::Size(sw, sh), 0.0, 0.0,
                   clampedScale < 1.0f ? cv::INTER_AREA : cv::INTER_LINEAR);
    }

    const int dstX = std::clamp(centerX - activeBgra.cols / 2, 2, (std::max)(2, dstBgr.cols - activeBgra.cols - 2));
    const int dstY = std::clamp(centerY - activeBgra.rows / 2, 2, (std::max)(2, dstBgr.rows - activeBgra.rows - 2));

    if (clampedOp >= 0.995f) {
        alphaBlendPremultipliedBgraOntoBgr(activeBgra, dstBgr, dstX, dstY);
        return;
    }

    const int x0 = (std::max)(0, dstX);
    const int y0 = (std::max)(0, dstY);
    const int x1 = (std::min)(dstBgr.cols, dstX + activeBgra.cols);
    const int y1 = (std::min)(dstBgr.rows, dstY + activeBgra.rows);
    if (x1 <= x0 || y1 <= y0) return;

    const uint32_t op256 = static_cast<uint32_t>(std::round(clampedOp * 256.0f));
    for (int y = y0; y < y1; ++y) {
        const uint8_t* sRow = activeBgra.ptr<uint8_t>(y - dstY) + (x0 - dstX) * 4;
        uint8_t* dRow = dstBgr.ptr<uint8_t>(y) + x0 * 3;
        for (int x = x0; x < x1; ++x) {
            const uint32_t effA = (static_cast<uint32_t>(sRow[3]) * op256 + 128u) >> 8;
            if (effA > 0) {
                const uint32_t sb = (static_cast<uint32_t>(sRow[0]) * op256 + 128u) >> 8;
                const uint32_t sg = (static_cast<uint32_t>(sRow[1]) * op256 + 128u) >> 8;
                const uint32_t sr = (static_cast<uint32_t>(sRow[2]) * op256 + 128u) >> 8;
                const uint32_t invA = 255u - (std::min<uint32_t>)(255u, effA);
                dRow[0] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sb + (dRow[0] * invA + 127u) / 255u));
                dRow[1] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sg + (dRow[1] * invA + 127u) / 255u));
                dRow[2] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sr + (dRow[2] * invA + 127u) / 255u));
            }
            sRow += 4;
            dRow += 3;
        }
    }
}

template <typename DrawFn>
void blendRoiOverlayBgr(cv::Mat& baseBgr, const cv::Rect& dirtyBounds, double alpha, DrawFn&& fn) {
    if (baseBgr.empty() || alpha <= 0.01) return;
    const cv::Rect valid = dirtyBounds & cv::Rect(0, 0, baseBgr.cols, baseBgr.rows);
    if (valid.width <= 1 || valid.height <= 1) return;
    const double clampedAlpha = std::clamp(alpha, 0.0, 1.0);
    if (clampedAlpha >= 0.995) {
        cv::Mat directRoi = baseBgr(valid);
        fn(directRoi, -valid.x, -valid.y);
        return;
    }
    cv::Mat baseRoi = baseBgr(valid);
    cv::Mat overlayRoi = baseRoi.clone();
    fn(overlayRoi, -valid.x, -valid.y);
    cv::addWeighted(overlayRoi, clampedAlpha, baseRoi, 1.0 - clampedAlpha, 0.0, baseRoi);
}

// 线程局部可复用 Direct2D 预乘 BGRA 矢量画板：为圆弧、手绘重点圈、圆角框提供原生圆头笔帽与 8x8 硬件级平滑抗锯齿
struct ThreadLocalD2DSurface {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> dcRt;
    ComPtr<ID2D1StrokeStyle> roundStroke;
    HDC memDc = nullptr;
    HBITMAP dibBmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    void* bits = nullptr;
    int capW = 0;
    int capH = 0;

    ~ThreadLocalD2DSurface() {
        dcRt.Reset();
        roundStroke.Reset();
        factory.Reset();
        if (memDc) {
            if (oldBmp) SelectObject(memDc, oldBmp);
            if (dibBmp) DeleteObject(dibBmp);
            DeleteDC(memDc);
        }
    }

    bool ensureCapacity(int w, int h) {
        if (!factory) {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) || !factory) {
                return false;
            }
        }
        if (!roundStroke) {
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND, 4.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
            factory->CreateStrokeStyle(sp, nullptr, 0, roundStroke.GetAddressOf());
        }
        if (!dcRt) {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(factory->CreateDCRenderTarget(&props, dcRt.GetAddressOf())) || !dcRt) {
                return false;
            }
        }
        if (!memDc) {
            memDc = CreateCompatibleDC(nullptr);
            if (!memDc) return false;
        }
        if (!dibBmp || w > capW || h > capH) {
            const int allocW = (std::max)(capW, (w + 63) & ~63);
            const int allocH = (std::max)(capH, (h + 63) & ~63);
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = allocW;
            bmi.bmiHeader.biHeight = -allocH; // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            void* newBits = nullptr;
            HBITMAP newBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &newBits, nullptr, 0);
            if (!newBmp || !newBits) {
                if (newBmp) DeleteObject(newBmp);
                return false;
            }
            HGDIOBJ prev = SelectObject(memDc, newBmp);
            if (!oldBmp) oldBmp = prev;
            if (dibBmp) DeleteObject(dibBmp);
            dibBmp = newBmp;
            bits = newBits;
            capW = allocW;
            capH = allocH;
        }
        return true;
    }
};

template <typename D2DDrawFn>
void renderD2DRoiVectorOntoBgr(cv::Mat& baseBgr, const cv::Rect& dirtyBounds, float masterOpacity, D2DDrawFn&& fn) {
    if (baseBgr.empty() || masterOpacity <= 0.01f) return;
    const cv::Rect valid = dirtyBounds & cv::Rect(0, 0, baseBgr.cols, baseBgr.rows);
    if (valid.width <= 2 || valid.height <= 2) return;

    thread_local ThreadLocalD2DSurface s_surf;
    if (!s_surf.ensureCapacity(valid.width, valid.height)) return;

    RECT bindRc{0, 0, valid.width, valid.height};
    if (FAILED(s_surf.dcRt->BindDC(s_surf.memDc, &bindRc))) return;

    s_surf.dcRt->BeginDraw();
    s_surf.dcRt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    s_surf.dcRt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(s_surf.dcRt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush.GetAddressOf())) && brush) {
        fn(s_surf.factory.Get(), s_surf.dcRt.Get(), brush.Get(), s_surf.roundStroke.Get(),
           -static_cast<float>(valid.x), -static_cast<float>(valid.y));
    }
    if (FAILED(s_surf.dcRt->EndDraw())) {
        s_surf.dcRt.Reset();
        return;
    }

    cv::Mat roiBgra(valid.height, valid.width, CV_8UC4, s_surf.bits, static_cast<size_t>(s_surf.capW) * 4);
    if (masterOpacity >= 0.995f) {
        alphaBlendPremultipliedBgraOntoBgr(roiBgra, baseBgr, valid.x, valid.y);
    } else {
        const uint32_t op256 = static_cast<uint32_t>(std::round(std::clamp(masterOpacity, 0.0f, 1.0f) * 256.0f));
        for (int y = 0; y < valid.height; ++y) {
            const uint8_t* sRow = roiBgra.ptr<uint8_t>(y);
            uint8_t* dRow = baseBgr.ptr<uint8_t>(valid.y + y) + valid.x * 3;
            for (int x = 0; x < valid.width; ++x) {
                const uint32_t effA = (static_cast<uint32_t>(sRow[3]) * op256 + 128u) >> 8;
                if (effA > 0) {
                    const uint32_t sb = (static_cast<uint32_t>(sRow[0]) * op256 + 128u) >> 8;
                    const uint32_t sg = (static_cast<uint32_t>(sRow[1]) * op256 + 128u) >> 8;
                    const uint32_t sr = (static_cast<uint32_t>(sRow[2]) * op256 + 128u) >> 8;
                    const uint32_t invA = 255u - (std::min<uint32_t>)(255u, effA);
                    dRow[0] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sb + (dRow[0] * invA + 127u) / 255u));
                    dRow[1] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sg + (dRow[1] * invA + 127u) / 255u));
                    dRow[2] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sr + (dRow[2] * invA + 127u) / 255u));
                }
                sRow += 4;
                dRow += 3;
            }
        }
    }
}

void drawStyledTextCalloutBgr(
    cv::Mat& innerBgr,
    int x, int y,
    StudioAnnotationKind kind,
    const std::string& textUtf8,
    const cv::Scalar& accentBgr,
    float scale = 1.0f,
    float opacity = 1.0f,
    int* outBoxRadius = nullptr) {
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
            if (outBoxRadius) {
                *outBoxRadius = (std::max)(entry.premulBgra.cols, entry.premulBgra.rows) / 2;
            }
            alphaBlendPremultipliedBgraTransformedOntoBgr(entry.premulBgra, innerBgr, x, y, scale, opacity);
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

    if (outBoxRadius) {
        *outBoxRadius = (std::max)(boxW, totalH) / 2;
    }
    alphaBlendPremultipliedBgraTransformedOntoBgr(pillBgra, innerBgr, x, y, scale, opacity);

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
}

void drawDashedSegmentBgr(
    cv::Mat& img,
    cv::Point p0,
    cv::Point p1,
    const cv::Scalar& col,
    int thick,
    double phaseOffsetPx = 0.0,
    double revealRatio = 1.0) {
    const double totalLen = std::hypot(static_cast<double>(p1.x - p0.x), static_cast<double>(p1.y - p0.y));
    if (totalLen < 2.0) return;
    const double activeLen = totalLen * std::clamp(revealRatio, 0.0, 1.0);
    if (activeLen < 1.5) return;

    const double dashLen = (std::max)(8.0, thick * 3.2);
    const double gapLen = (std::max)(6.0, thick * 2.2);
    const double cycle = dashLen + gapLen;
    const double shift = std::fmod(std::fmod(phaseOffsetPx, cycle) + cycle, cycle);
    const double ux = (p1.x - p0.x) / totalLen;
    const double uy = (p1.y - p0.y) / totalLen;

    for (double d = shift - cycle; d < activeLen; d += cycle) {
        const double seg0 = (std::max)(0.0, d);
        const double seg1 = (std::min)(activeLen, d + dashLen);
        if (seg1 - seg0 < 1.0) continue;
        cv::Point a(static_cast<int>(std::round(p0.x + ux * seg0)), static_cast<int>(std::round(p0.y + uy * seg0)));
        cv::Point b(static_cast<int>(std::round(p0.x + ux * seg1)), static_cast<int>(std::round(p0.y + uy * seg1)));
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
    float effCropW, float effCropH,
    bool animateAnnotations = true,
    int64_t annotationPreviewElapsedMs = -1,
    int previewTargetAnnIndex = -1) {
    if (innerBgr.empty() || annotations.empty()) return;
    const float safeCropW = (std::max)(1.0f, effCropW);
    const float safeCropH = (std::max)(1.0f, effCropH);
    const float iwF = static_cast<float>(innerBgr.cols);
    const float ihF = static_cast<float>(innerBgr.rows);
    const float strokeScale = std::clamp(ihF / 900.0f, 0.6f, 2.5f);

    auto easeOutCubic = [](double t) noexcept -> double {
        t = std::clamp(t, 0.0, 1.0);
        const double inv = 1.0 - t;
        return 1.0 - inv * inv * inv;
    };
    auto springPopScale = [](double t) noexcept -> float {
        t = std::clamp(t, 0.0, 1.0);
        if (t >= 0.999) return 1.0f;
        // 二阶阻尼微超调弹簧: 0.52 -> 1.09 -> 1.00
        return static_cast<float>(1.0 - 0.48 * std::exp(-5.4 * t) * std::cos(5.2 * t));
    };

    for (size_t annIdx = 0; annIdx < annotations.size(); ++annIdx) {
        const auto& ann = annotations[annIdx];
        const bool isLiveDemoTarget = (annotationPreviewElapsedMs >= 0 &&
                                       static_cast<int>(annIdx) == previewTargetAnnIndex);
        if (!ann.enabled) continue;
        if (!isLiveDemoTarget && (ptsMs < ann.startMs || ptsMs > ann.endMs)) continue;

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

        // ── 计算「抓眼入场 + 驻留微律动 + 优雅退场」三段式动力学包络 ──
        const double durMs = static_cast<double>((std::max<int64_t>)(240, ann.endMs - ann.startMs));
        const double localMs = isLiveDemoTarget
            ? static_cast<double>(annotationPreviewElapsedMs)
            : static_cast<double>(ptsMs - ann.startMs);

        const bool useMotion = animateAnnotations || isLiveDemoTarget;
        double enterLinear = 1.0;
        double exitLinear = 1.0;
        if (useMotion) {
            const double inSpanMs = isLiveDemoTarget ? 270.0 : std::clamp(durMs * 0.20, 120.0, 280.0);
            enterLinear = std::clamp(localMs / inSpanMs, 0.0, 1.0);
            if (!isLiveDemoTarget) {
                const double outSpanMs = std::clamp(durMs * 0.15, 90.0, 190.0);
                exitLinear = std::clamp((durMs - localMs) / outSpanMs, 0.0, 1.0);
            }
        }
        const double enterEase = easeOutCubic(enterLinear);
        const double exitEase = easeOutCubic(exitLinear);
        const double masterAlpha = std::clamp(std::min(enterLinear * 1.65, exitEase), 0.0, 1.0);
        const double animSec = useMotion ? (localMs * 0.001) : 0.45;
        const double animPhase = animSec * 7.2;

        // 隐私打码遮罩特例：从首帧至末帧 100% 严密遮挡，绝不做入退场透明或缩放以防敏感信息泄露
        if (ann.kind == StudioAnnotationKind::BlurMask) {
            const int rx0 = std::clamp((std::min)(px0, px1), 0, innerBgr.cols);
            const int ry0 = std::clamp((std::min)(py0, py1), 0, innerBgr.rows);
            const int rx1 = std::clamp((std::max)(px0, px1), 0, innerBgr.cols);
            const int ry1 = std::clamp((std::max)(py0, py1), 0, innerBgr.rows);
            const cv::Rect rc(rx0, ry0, rx1 - rx0, ry1 - ry0);
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
            continue;
        }

        if (masterAlpha <= 0.015) continue;

        // 1. 文本与步骤徽章家族 (6 款：弹簧跃出/微距上浮入场 + 分类别驻留微律动)
        if (ann.kind == StudioAnnotationKind::Text ||
            ann.kind == StudioAnnotationKind::TextPlain ||
            ann.kind == StudioAnnotationKind::TextBubble ||
            ann.kind == StudioAnnotationKind::TextDashed ||
            ann.kind == StudioAnnotationKind::TextNeon ||
            ann.kind == StudioAnnotationKind::StepBadge) {
            const int baseCx = (px0 + px1) / 2;
            const int baseCy = (py0 + py1) / 2;
            const bool isBadge = (ann.kind == StudioAnnotationKind::StepBadge);

            const float popScale = useMotion
                ? (isBadge ? springPopScale(enterLinear)
                           : static_cast<float>(0.88 + 0.12 * enterEase)) *
                  static_cast<float>(0.95 + 0.05 * exitEase)
                : 1.0f;
            int floatOffsetY = useMotion
                ? static_cast<int>(std::round((1.0 - enterEase) * 10.0 * strokeScale))
                : 0;
            if (useMotion && enterLinear >= 0.99) {
                if (ann.kind == StudioAnnotationKind::TextBubble) {
                    // 对话气泡轻柔微悬浮呼吸
                    floatOffsetY += static_cast<int>(std::round(std::sin(animSec * 3.8) * 2.8 * strokeScale));
                }
            }
            const int drawCx = baseCx;
            const int drawCy = baseCy + floatOffsetY;

            // StepBadge 专属：驻留期向外释放柔和同心雷达脉冲环，一眼锁定步骤顺序
            int approxRad = static_cast<int>(std::round(20.0f * strokeScale));
            const std::string fallback = isBadge ? "1" : "Text";
            drawStyledTextCalloutBgr(
                innerBgr, drawCx, drawCy, ann.kind,
                ann.text.empty() ? fallback : ann.text,
                colBgr, popScale, static_cast<float>(masterAlpha), &approxRad);

            if (useMotion && isBadge && enterLinear > 0.65) {
                const double pulseCycle = std::fmod( (std::max)(0.0, animSec - 0.18) * 0.90, 1.0 );
                const int ringR = approxRad + static_cast<int>(std::round(pulseCycle * 15.0 * strokeScale));
                const double ringAlpha = (1.0 - pulseCycle) * (1.0 - pulseCycle) * 0.58 * masterAlpha;
                const int pad = ringR + thick + 6;
                blendRoiOverlayBgr(
                    innerBgr, cv::Rect(drawCx - pad, drawCy - pad, pad * 2, pad * 2), ringAlpha,
                    [&](cv::Mat& sub, int ox, int oy) {
                        cv::circle(sub, {drawCx + ox, drawCy + oy}, ringR, colBgr, (std::max)(2, thick - 1), cv::LINE_AA);
                    });
            } else if (useMotion && ann.kind == StudioAnnotationKind::TextNeon && enterLinear > 0.8) {
                // 霓虹文字框外围柔和呼吸光晕
                const double glowPulse = (0.5 + 0.5 * std::sin(animSec * 5.2)) * 0.28 * masterAlpha;
                const int hw = approxRad + static_cast<int>(6 * strokeScale);
                const int hh = (std::max)(14, approxRad / 2) + static_cast<int>(4 * strokeScale);
                blendRoiOverlayBgr(
                    innerBgr, cv::Rect(drawCx - hw - 10, drawCy - hh - 10, (hw + 10) * 2, (hh + 10) * 2), glowPulse,
                    [&](cv::Mat& sub, int ox, int oy) {
                        drawRoundedBoxStrokeBgr(
                            sub, cv::Rect(drawCx - hw + ox, drawCy - hh + oy, hw * 2, hh * 2),
                            static_cast<int>(10 * strokeScale), colBgr, thick + 3);
                    });
            }
            continue;
        }

        // 2. 醒目粗体动态弹跳大箭头 (弹簧射入 + 指向呼吸弹跳 + 局部 ROI 高速阴影)
        if (ann.kind == StudioAnnotationKind::BoldArrow) {
            const double dx = static_cast<double>(px1 - px0);
            const double dy = static_cast<double>(py1 - py0);
            const double fullLen = std::hypot(dx, dy);
            const double growScale = useMotion ? std::clamp(0.30 + 0.70 * enterEase, 0.30, 1.0) : 1.0;
            const double len = fullLen * growScale;
            if (len >= 8.0) {
                const double ux = dx / fullLen;
                const double uy = dy / fullLen;
                const double nx = -uy;
                const double ny = ux;
                const double bounce = useMotion ? (std::sin(animPhase) * 6.5 * strokeScale * enterEase) : 0.0;
                const double slideIn = useMotion ? (-(1.0 - enterEase) * 18.0 * strokeScale) : 0.0;
                const double bx0 = px0 + ux * (bounce + slideIn);
                const double by0 = py0 + uy * (bounce + slideIn);
                const double bx1 = bx0 + ux * len;
                const double by1 = by0 + uy * len;

                const double headLen = std::clamp(len * 0.42, 16.0 * strokeScale, 54.0 * strokeScale);
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
                cv::Rect bounds = cv::boundingRect(poly);
                const int shDx = static_cast<int>(std::round(3.0 * strokeScale));
                const int shDy = static_cast<int>(std::round(4.0 * strokeScale));
                bounds.x -= 8;
                bounds.y -= 8;
                bounds.width += shDx + 16;
                bounds.height += shDy + 16;

                blendRoiOverlayBgr(innerBgr, bounds, 0.32 * masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                    std::vector<cv::Point> shPoly = poly;
                    for (auto& p : shPoly) {
                        p.x += ox + shDx;
                        p.y += oy + shDy;
                    }
                    cv::fillPoly(sub, std::vector<std::vector<cv::Point>>{shPoly}, cv::Scalar(12, 14, 22), cv::LINE_AA);
                });
                blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                    std::vector<cv::Point> fgPoly = poly;
                    for (auto& p : fgPoly) {
                        p.x += ox;
                        p.y += oy;
                    }
                    cv::fillPoly(sub, std::vector<std::vector<cv::Point>>{fgPoly}, colBgr, cv::LINE_AA);
                    cv::polylines(sub, std::vector<std::vector<cv::Point>>{fgPoly}, true,
                                  cv::Scalar(255, 255, 255), (std::max)(1, static_cast<int>(std::round(1.6f * strokeScale))), cv::LINE_AA);
                });
            }
            continue;
        }

        // 3. 纤细指示箭头 / 荧光划线 / 引导虚线
        if (ann.kind == StudioAnnotationKind::Arrow) {
            const double dx = static_cast<double>(px1 - px0);
            const double dy = static_cast<double>(py1 - py0);
            const double fullLen = std::hypot(dx, dy);
            if (fullLen >= 4.0) {
                const double ux = dx / fullLen;
                const double uy = dy / fullLen;
                // 入场沿轴线快速射出；驻留期做柔和指向轻叩 + 箭尖微脉冲光点
                const double drawFrac = useMotion ? std::clamp(enterEase, 0.12, 1.0) : 1.0;
                const double nudge = (useMotion && enterLinear >= 0.98)
                    ? (std::sin(animSec * 6.4) * 4.2 * strokeScale)
                    : 0.0;
                const cv::Point a0(
                    static_cast<int>(std::round(px0 + ux * nudge)),
                    static_cast<int>(std::round(py0 + uy * nudge)));
                const cv::Point a1(
                    static_cast<int>(std::round(px0 + ux * (fullLen * drawFrac + nudge))),
                    static_cast<int>(std::round(py0 + uy * (fullLen * drawFrac + nudge))));
                const double curLen = std::hypot(static_cast<double>(a1.x - a0.x), static_cast<double>(a1.y - a0.y));
                if (curLen >= 3.0) {
                    const double tipRatio = std::clamp(16.0 * strokeScale / curLen, 0.12, 0.44);
                    const int pad = static_cast<int>(26 * strokeScale) + thick;
                    const cv::Rect bounds(
                        (std::min)(a0.x, a1.x) - pad, (std::min)(a0.y, a1.y) - pad,
                        std::abs(a1.x - a0.x) + pad * 2, std::abs(a1.y - a0.y) + pad * 2);
                    blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                        cv::arrowedLine(sub, {a0.x + ox, a0.y + oy}, {a1.x + ox, a1.y + oy},
                                        cv::Scalar(16, 20, 28), thick + 2, cv::LINE_AA, 0, tipRatio);
                        cv::arrowedLine(sub, {a0.x + ox, a0.y + oy}, {a1.x + ox, a1.y + oy},
                                        colBgr, thick, cv::LINE_AA, 0, tipRatio);
                    });
                    if (useMotion && enterLinear >= 0.95) {
                        const double tipWave = std::fmod((std::max)(0.0, animSec - 0.2) * 1.15, 1.0);
                        const int tipR = static_cast<int>(std::round((4.0 + tipWave * 14.0) * strokeScale));
                        const double tipAlpha = (1.0 - tipWave) * 0.52 * masterAlpha;
                        blendRoiOverlayBgr(
                            innerBgr, cv::Rect(px1 - tipR - 6, py1 - tipR - 6, (tipR + 6) * 2, (tipR + 6) * 2), tipAlpha,
                            [&](cv::Mat& sub, int ox, int oy) {
                                cv::circle(sub, {px1 + ox, py1 + oy}, tipR, colBgr, (std::max)(1, thick - 1), cv::LINE_AA);
                            });
                    }
                }
            }
            continue;
        }

        if (ann.kind == StudioAnnotationKind::GlowLine) {
            // 荧光马克笔：一笔划出 + 底层荧光呼吸 + 流光高光扫掠
            const double drawFrac = useMotion ? enterEase : 1.0;
            const cv::Point g0(px0, py0);
            const cv::Point g1(
                static_cast<int>(std::round(px0 + (px1 - px0) * drawFrac)),
                static_cast<int>(std::round(py0 + (py1 - py0) * drawFrac)));
            const int pad = thick * 4 + 12;
            const cv::Rect bounds(
                (std::min)(g0.x, g1.x) - pad, (std::min)(g0.y, g1.y) - pad,
                std::abs(g1.x - g0.x) + pad * 2, std::abs(g1.y - g0.y) + pad * 2);
            const double glowBreath = useMotion ? (0.30 + 0.14 * (0.5 + 0.5 * std::sin(animSec * 4.8))) : 0.35;
            blendRoiOverlayBgr(innerBgr, bounds, glowBreath * masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                cv::line(sub, {g0.x + ox, g0.y + oy}, {g1.x + ox, g1.y + oy}, colBgr, thick * 3 + 5, cv::LINE_AA);
            });
            blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                cv::line(sub, {g0.x + ox, g0.y + oy}, {g1.x + ox, g1.y + oy}, colBgr, thick + 1, cv::LINE_AA);
            });
            if (useMotion && enterLinear >= 0.98) {
                const double sweep = std::fmod(animSec * 0.85, 1.35);
                if (sweep <= 1.0) {
                    const double s0 = (std::max)(0.0, sweep - 0.16);
                    const double s1 = (std::min)(1.0, sweep + 0.06);
                    const cv::Point sp0(
                        static_cast<int>(std::round(px0 + (px1 - px0) * s0)),
                        static_cast<int>(std::round(py0 + (py1 - py0) * s0)));
                    const cv::Point sp1(
                        static_cast<int>(std::round(px0 + (px1 - px0) * s1)),
                        static_cast<int>(std::round(py0 + (py1 - py0) * s1)));
                    blendRoiOverlayBgr(innerBgr, bounds, 0.72 * masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                        cv::line(sub, {sp0.x + ox, sp0.y + oy}, {sp1.x + ox, sp1.y + oy},
                                 cv::Scalar(255, 255, 255), (std::max)(2, thick), cv::LINE_AA);
                    });
                }
            }
            continue;
        }

        if (ann.kind == StudioAnnotationKind::DashedLine) {
            // 引导虚线：入场延伸铺开 + 驻留期定向行军流 (Marching Flow)
            const double marchPx = useMotion ? (animSec * 46.0 * strokeScale) : 0.0;
            const double reveal = useMotion ? enterEase : 1.0;
            const int pad = thick * 2 + 8;
            const cv::Rect bounds(
                (std::min)(px0, px1) - pad, (std::min)(py0, py1) - pad,
                std::abs(px1 - px0) + pad * 2, std::abs(py1 - py0) + pad * 2);
            blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                drawDashedSegmentBgr(sub, {px0 + ox, py0 + oy}, {px1 + ox, py1 + oy},
                                     colBgr, thick + 1, marchPx, reveal);
            });
            continue;
        }

        // 4. 点击小手引导图标 (真实食指轻触下压节拍 + 双道扩散点击波纹环)
        if (ann.kind == StudioAnnotationKind::HandPointer) {
            const int baseHx = (px0 + px1) / 2;
            const int baseHy = (py0 + py1) / 2;
            const float s = std::clamp(strokeScale * 1.15f, 0.7f, 2.6f) *
                            (useMotion ? springPopScale(enterLinear) : 1.0f);
            const double tapCycle = useMotion ? std::fmod(animSec * 1.15, 1.0) : 0.35;
            const double tapPress = (tapCycle < 0.24)
                ? std::sin((tapCycle / 0.24) * 3.14159265)
                : 0.0;
            const int hx = baseHx;
            const int hy = baseHy + static_cast<int>(std::round(tapPress * 4.5 * s));
            const int tipY = baseHy - static_cast<int>(10 * s);

            const int pad = static_cast<int>(38 * s) + thick;
            const cv::Rect bounds(baseHx - pad, baseHy - pad, pad * 2, pad * 2);

            if (useMotion) {
                for (int wIdx = 0; wIdx < 2; ++wIdx) {
                    const double wp = std::fmod(tapCycle + wIdx * 0.22, 1.0);
                    const int pulseR = static_cast<int>(std::round((8.0 + wp * 20.0) * s));
                    const double rAlpha = (1.0 - wp) * (1.0 - wp) * 0.72 * masterAlpha;
                    blendRoiOverlayBgr(innerBgr, bounds, rAlpha, [&](cv::Mat& sub, int ox, int oy) {
                        cv::circle(sub, {baseHx + ox, tipY + oy}, pulseR, colBgr, (std::max)(2, thick), cv::LINE_AA);
                    });
                }
            } else {
                blendRoiOverlayBgr(innerBgr, bounds, 0.65, [&](cv::Mat& sub, int ox, int oy) {
                    cv::circle(sub, {baseHx + ox, tipY + oy}, static_cast<int>(16 * s), colBgr, (std::max)(2, thick), cv::LINE_AA);
                });
            }

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
            blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                for (auto& pt : handPts) {
                    pt.x += ox;
                    pt.y += oy;
                }
                cv::fillPoly(sub, std::vector<std::vector<cv::Point>>{handPts}, cv::Scalar(252, 253, 255), cv::LINE_AA);
                cv::polylines(sub, std::vector<std::vector<cv::Point>>{handPts}, true, cv::Scalar(24, 30, 42),
                              (std::max)(2, static_cast<int>(std::round(2.0f * s))), cv::LINE_AA);
            });
            continue;
        }

        const int rx0 = std::clamp((std::min)(px0, px1), 0, innerBgr.cols);
        const int ry0 = std::clamp((std::min)(py0, py1), 0, innerBgr.rows);
        const int rx1 = std::clamp((std::max)(px0, px1), 0, innerBgr.cols);
        const int ry1 = std::clamp((std::max)(py0, py1), 0, innerBgr.rows);
        const cv::Rect rc(rx0, ry0, rx1 - rx0, ry1 - ry0);

        if (ann.kind == StudioAnnotationKind::Spotlight) {
            // 剧场级聚光灯：背景暗场平滑渐合与渐退 + 聚光亮窗呼吸白边光晕
            const double dimStrength = masterAlpha * 0.64; // 峰值保留 0.36 亮度
            cv::Mat dimmed;
            innerBgr.convertTo(dimmed, -1, 1.0 - dimStrength, 0.0);
            if (rc.width > 4 && rc.height > 4) {
                cv::Mat mask(innerBgr.rows, innerBgr.cols, CV_8UC1, cv::Scalar(0));
                const int rad = static_cast<int>(std::round(10.0f * strokeScale));
                fillRoundedRectMask(mask, rc, static_cast<float>(rad));
                innerBgr.copyTo(dimmed, mask);

                if (useMotion) {
                    const double haloWave = 0.5 + 0.5 * std::sin(animSec * 4.2);
                    const int expand = static_cast<int>(std::round((2.0 + haloWave * 5.0) * strokeScale));
                    const cv::Rect haloRc(rc.x - expand, rc.y - expand, rc.width + expand * 2, rc.height + expand * 2);
                    blendRoiOverlayBgr(
                        dimmed, cv::Rect(haloRc.x - 8, haloRc.y - 8, haloRc.width + 16, haloRc.height + 16),
                        (0.18 + 0.18 * haloWave) * masterAlpha,
                        [&](cv::Mat& sub, int ox, int oy) {
                            drawRoundedBoxStrokeBgr(
                                sub, cv::Rect(haloRc.x + ox, haloRc.y + oy, haloRc.width, haloRc.height),
                                rad + expand, cv::Scalar(255, 255, 255), thick + 1);
                        });
                }
                drawRoundedBoxStrokeBgr(dimmed, rc, rad, cv::Scalar(255, 255, 255), (std::max)(1, thick - 1));
            }
            dimmed.copyTo(innerBgr);
        } else if (ann.kind == StudioAnnotationKind::Magnifier) {
            // 圆形局部光学放大镜：倍率 1.0x -> 2.2x 丝滑推近聚焦入场 + 镜框呼吸光环
            const int cx = (px0 + px1) / 2;
            const int cy = (py0 + py1) / 2;
            const double radScale = useMotion ? (0.82 + 0.18 * enterEase) : 1.0;
            const int rad = std::clamp(
                static_cast<int>(std::round(((std::max)(rc.width, rc.height) * 0.5) * radScale)),
                static_cast<int>(24 * strokeScale), (std::min)(innerBgr.cols, innerBgr.rows) / 2 - 4);
            const cv::Rect boxRc(
                std::clamp(cx - rad, 0, innerBgr.cols - 1),
                std::clamp(cy - rad, 0, innerBgr.rows - 1),
                std::clamp(cx + rad, 1, innerBgr.cols) - std::clamp(cx - rad, 0, innerBgr.cols - 1),
                std::clamp(cy + rad, 1, innerBgr.rows) - std::clamp(cy - rad, 0, innerBgr.rows - 1));
            if (boxRc.width >= 12 && boxRc.height >= 12 && !fullSrcBgr.empty()) {
                const float centerSrcX = ((ann.x0 + ann.x1) * 0.5f) * srcCols;
                const float centerSrcY = ((ann.y0 + ann.y1) * 0.5f) * srcRows;
                const float zoomMag = useMotion
                    ? static_cast<float>(1.08 + 1.12 * enterEase * exitEase)
                    : 2.2f;
                const float sampleHalfW = (static_cast<float>(rad) / iwF) * safeCropW / zoomMag;
                const float sampleHalfH = (static_cast<float>(rad) / ihF) * safeCropH / zoomMag;
                const int s0x = std::clamp(static_cast<int>(std::round(centerSrcX - sampleHalfW)), 0, fullSrcBgr.cols - 2);
                const int s0y = std::clamp(static_cast<int>(std::round(centerSrcY - sampleHalfH)), 0, fullSrcBgr.rows - 2);
                const int sw = std::clamp(static_cast<int>(std::round(sampleHalfW * 2.0f)), 2, fullSrcBgr.cols - s0x);
                const int sh = std::clamp(static_cast<int>(std::round(sampleHalfH * 2.0f)), 2, fullSrcBgr.rows - s0y);
                cv::Mat magnified;
                cv::resize(fullSrcBgr(cv::Rect(s0x, s0y, sw, sh)), magnified, cv::Size(rad * 2, rad * 2), 0.0, 0.0, cv::INTER_CUBIC);

                const int pad = rad + static_cast<int>(16 * strokeScale) + thick;
                blendRoiOverlayBgr(
                    innerBgr, cv::Rect(cx - pad, cy - pad, pad * 2, pad * 2), masterAlpha,
                    [&](cv::Mat& sub, int ox, int oy) {
                        const int lcx = cx + ox;
                        const int lcy = cy + oy;
                        cv::Mat circleMask(sub.rows, sub.cols, CV_8UC1, cv::Scalar(0));
                        cv::circle(circleMask, {lcx, lcy}, rad, cv::Scalar(255), cv::FILLED, cv::LINE_AA);

                        const int dstX0 = std::clamp(lcx - rad, 0, sub.cols - 1);
                        const int dstY0 = std::clamp(lcy - rad, 0, sub.rows - 1);
                        const int dstX1 = std::clamp(lcx + rad, 1, sub.cols);
                        const int dstY1 = std::clamp(lcy + rad, 1, sub.rows);
                        const int mx0 = dstX0 - (lcx - rad);
                        const int my0 = dstY0 - (lcy - rad);
                        const int mw = (std::min)(dstX1 - dstX0, magnified.cols - mx0);
                        const int mh = (std::min)(dstY1 - dstY0, magnified.rows - my0);
                        if (mw > 0 && mh > 0) {
                            magnified(cv::Rect(mx0, my0, mw, mh)).copyTo(
                                sub(cv::Rect(dstX0, dstY0, mw, mh)),
                                circleMask(cv::Rect(dstX0, dstY0, mw, mh)));
                        }
                        cv::circle(sub, {lcx, lcy}, rad + 1, cv::Scalar(18, 22, 32), thick + 2, cv::LINE_AA);
                        cv::circle(sub, {lcx, lcy}, rad, cv::Scalar(255, 255, 255), (std::max)(2, thick), cv::LINE_AA);
                    });

                if (useMotion && enterLinear >= 0.90) {
                    const double sonar = std::fmod((std::max)(0.0, animSec - 0.22) * 0.80, 1.0);
                    const int sRad = rad + static_cast<int>(std::round(sonar * 12.0 * strokeScale));
                    blendRoiOverlayBgr(
                        innerBgr, cv::Rect(cx - pad, cy - pad, pad * 2, pad * 2),
                        (1.0 - sonar) * 0.42 * masterAlpha,
                        [&](cv::Mat& sub, int ox, int oy) {
                            cv::circle(sub, {cx + ox, cy + oy}, sRad, cv::Scalar(255, 255, 255), (std::max)(1, thick - 1), cv::LINE_AA);
                        });
                }
            }
        } else if (ann.kind == StudioAnnotationKind::Rectangle ||
                   ann.kind == StudioAnnotationKind::RoundedBox) {
            // 直角框 / 圆角框：Direct2D 圆头抗锯齿一笔闭合勾勒 + 柔和声呐外扩光环
            if (rc.width >= 4 && rc.height >= 4) {
                const bool isRounded = (ann.kind == StudioAnnotationKind::RoundedBox);
                const float radF = isRounded
                    ? std::clamp(14.0f * strokeScale, 4.0f, static_cast<float>((std::min)(rc.width, rc.height)) * 0.34f)
                    : 2.0f;
                const float drawProg = static_cast<float>(useMotion ? enterEase : 1.0);
                const float fThick = (std::max)(2.2f, ann.strokeWidth * strokeScale);
                const int pad = static_cast<int>(22 * strokeScale) + thick * 2;
                const cv::Rect bounds(rc.x - pad, rc.y - pad, rc.width + pad * 2, rc.height + pad * 2);
                const float cr = static_cast<float>(colBgr[2] / 255.0);
                const float cg = static_cast<float>(colBgr[1] / 255.0);
                const float cb = static_cast<float>(colBgr[0] / 255.0);

                renderD2DRoiVectorOntoBgr(innerBgr, bounds, static_cast<float>(masterAlpha),
                    [&](ID2D1Factory* fac, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br,
                        ID2D1StrokeStyle* roundSt, float ox, float oy) {
                        const D2D1_RECT_F box = D2D1::RectF(
                            rc.x + ox, rc.y + oy, rc.x + rc.width + ox, rc.y + rc.height + oy);

                        // 驻留期柔和外扩呼吸光晕
                        if (useMotion && enterLinear >= 0.92) {
                            const float sonar = static_cast<float>(std::fmod((std::max)(0.0, animSec - 0.24) * 0.78, 1.0));
                            const float exp = sonar * 11.0f * strokeScale;
                            const D2D1_RECT_F pBox = D2D1::RectF(box.left - exp, box.top - exp, box.right + exp, box.bottom + exp);
                            br->SetColor(D2D1::ColorF(cr, cg, cb, (1.0f - sonar) * (1.0f - sonar) * 0.48f));
                            rt->DrawRoundedRectangle(D2D1::RoundedRect(pBox, radF + exp, radF + exp), br, (std::max)(1.4f, fThick * 0.65f));
                        }

                        if (drawProg >= 0.995f) {
                            // 完整闭合超平滑圆角/微圆角框 + 立体柔和衬底描边
                            br->SetColor(D2D1::ColorF(0.05f, 0.07f, 0.12f, 0.34f));
                            rt->DrawRoundedRectangle(
                                D2D1::RoundedRect(D2D1::RectF(box.left + 1.0f, box.top + 1.8f, box.right + 1.0f, box.bottom + 1.8f), radF, radF),
                                br, fThick + 1.6f);
                            br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, radF, radF), br, fThick);
                        } else if (fac) {
                            // 入场顺时针连续圆头几何路径一笔勾勒
                            constexpr int kSteps = 80;
                            const int activePts = (std::max)(2, static_cast<int>(std::round(kSteps * drawProg)));
                            const float cx = (box.left + box.right) * 0.5f;
                            const float cy = (box.top + box.bottom) * 0.5f;
                            const float hw = (box.right - box.left) * 0.5f;
                            const float hh = (box.bottom - box.top) * 0.5f;
                            const float r = (std::min)({radF, hw * 0.95f, hh * 0.95f});

                            // 构造顺时针超椭圆/圆角矩形连续参数采样点
                            auto sampleBoxPerim = [&](float u) -> D2D1_POINT_2F {
                                const float topW = (std::max)(0.0f, (hw - r) * 2.0f);
                                const float sideH = (std::max)(0.0f, (hh - r) * 2.0f);
                                const float arcL = 1.5707963f * r;
                                const float totalP = (topW + sideH) * 2.0f + arcL * 4.0f;
                                float d = std::clamp(u, 0.0f, 1.0f) * totalP;
                                if (d <= topW) return {box.left + r + d, box.top};
                                d -= topW;
                                if (d <= arcL) {
                                    const float a = -1.5707963f + (d / (std::max)(0.01f, arcL)) * 1.5707963f;
                                    return {box.right - r + std::cos(a) * r, box.top + r + std::sin(a) * r};
                                }
                                d -= arcL;
                                if (d <= sideH) return {box.right, box.top + r + d};
                                d -= sideH;
                                if (d <= arcL) {
                                    const float a = (d / (std::max)(0.01f, arcL)) * 1.5707963f;
                                    return {box.right - r + std::cos(a) * r, box.bottom - r + std::sin(a) * r};
                                }
                                d -= arcL;
                                if (d <= topW) return {box.right - r - d, box.bottom};
                                d -= topW;
                                if (d <= arcL) {
                                    const float a = 1.5707963f + (d / (std::max)(0.01f, arcL)) * 1.5707963f;
                                    return {box.left + r + std::cos(a) * r, box.bottom - r + std::sin(a) * r};
                                }
                                d -= arcL;
                                if (d <= sideH) return {box.left, box.bottom - r - d};
                                d -= sideH;
                                const float a = 3.1415926f + (d / (std::max)(0.01f, arcL)) * 1.5707963f;
                                return {box.left + r + std::cos(a) * r, box.top + r + std::sin(a) * r};
                            };

                            ComPtr<ID2D1PathGeometry> geo;
                            if (SUCCEEDED(fac->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                                ComPtr<ID2D1GeometrySink> sink;
                                if (SUCCEEDED(geo->Open(sink.GetAddressOf())) && sink) {
                                    sink->BeginFigure(sampleBoxPerim(0.0f), D2D1_FIGURE_BEGIN_HOLLOW);
                                    for (int i = 1; i <= activePts; ++i) {
                                        sink->AddLine(sampleBoxPerim(static_cast<float>(i) / static_cast<float>(kSteps)));
                                    }
                                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                                    sink->Close();
                                    br->SetColor(D2D1::ColorF(0.05f, 0.07f, 0.12f, 0.32f));
                                    rt->DrawGeometry(geo.Get(), br, fThick + 1.6f, roundSt);
                                    br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                                    rt->DrawGeometry(geo.Get(), br, fThick, roundSt);
                                }
                            }
                            (void)cx;
                            (void)cy;
                        }
                    });
            }
        } else if (ann.kind == StudioAnnotationKind::CircleRing) {
            // 标准圆环/椭圆聚焦环：Direct2D 超平滑圆头弧线顺时针一笔闭合 + 同心声呐聚焦波
            if (rc.width >= 4 && rc.height >= 4) {
                const float cx = rc.x + rc.width * 0.5f;
                const float cy = rc.y + rc.height * 0.5f;
                const float rx = rc.width * 0.5f;
                const float ry = rc.height * 0.5f;
                const float drawProg = static_cast<float>(useMotion ? enterEase : 1.0);
                const float fThick = (std::max)(2.2f, ann.strokeWidth * strokeScale);
                const int pad = static_cast<int>(22 * strokeScale) + thick * 2;
                const cv::Rect bounds(rc.x - pad, rc.y - pad, rc.width + pad * 2, rc.height + pad * 2);
                const float cr = static_cast<float>(colBgr[2] / 255.0);
                const float cg = static_cast<float>(colBgr[1] / 255.0);
                const float cb = static_cast<float>(colBgr[0] / 255.0);

                renderD2DRoiVectorOntoBgr(innerBgr, bounds, static_cast<float>(masterAlpha),
                    [&](ID2D1Factory* fac, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br,
                        ID2D1StrokeStyle* roundSt, float ox, float oy) {
                        const D2D1_POINT_2F cPt{cx + ox, cy + oy};
                        if (useMotion && enterLinear >= 0.92) {
                            const float sonar = static_cast<float>(std::fmod((std::max)(0.0, animSec - 0.24) * 0.80, 1.0));
                            const float exp = sonar * 11.0f * strokeScale;
                            br->SetColor(D2D1::ColorF(cr, cg, cb, (1.0f - sonar) * (1.0f - sonar) * 0.48f));
                            rt->DrawEllipse(D2D1::Ellipse(cPt, rx + exp, ry + exp), br, (std::max)(1.4f, fThick * 0.65f));
                        }
                        if (drawProg >= 0.995f) {
                            br->SetColor(D2D1::ColorF(0.05f, 0.07f, 0.12f, 0.34f));
                            rt->DrawEllipse(D2D1::Ellipse({cPt.x + 1.0f, cPt.y + 1.8f}, rx, ry), br, fThick + 1.6f);
                            br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                            rt->DrawEllipse(D2D1::Ellipse(cPt, rx, ry), br, fThick);
                        } else if (fac) {
                            constexpr int kSteps = 96;
                            const int activePts = (std::max)(2, static_cast<int>(std::round(kSteps * drawProg)));
                            ComPtr<ID2D1PathGeometry> geo;
                            if (SUCCEEDED(fac->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                                ComPtr<ID2D1GeometrySink> sink;
                                if (SUCCEEDED(geo->Open(sink.GetAddressOf())) && sink) {
                                    auto ptAt = [&](int idx) -> D2D1_POINT_2F {
                                        const float ang = -1.5707963f + (static_cast<float>(idx) / kSteps) * 6.2831853f;
                                        return {cPt.x + std::cos(ang) * rx, cPt.y + std::sin(ang) * ry};
                                    };
                                    sink->BeginFigure(ptAt(0), D2D1_FIGURE_BEGIN_HOLLOW);
                                    for (int i = 1; i <= activePts; ++i) sink->AddLine(ptAt(i));
                                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                                    sink->Close();
                                    br->SetColor(D2D1::ColorF(0.05f, 0.07f, 0.12f, 0.32f));
                                    rt->DrawGeometry(geo.Get(), br, fThick + 1.6f, roundSt);
                                    br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                                    rt->DrawGeometry(geo.Get(), br, fThick, roundSt);
                                }
                            }
                        }
                    });
            }
        } else if (ann.kind == StudioAnnotationKind::SketchCircle) {
            // 手绘重点圈 (Hand-Drawn Marker Loop)：
            // 彻底摒弃旧版双错位 OpenCV ellipse (>360° 截断撕裂破碎)，升级为 Direct2D 原生圆头抗锯齿单笔流畅马克笔圈重点曲线！
            if (rc.width >= 8 && rc.height >= 8) {
                const float cx = rc.x + rc.width * 0.5f;
                const float cy = rc.y + rc.height * 0.5f;
                const float rx = rc.width * 0.5f;
                const float ry = rc.height * 0.5f;
                const float drawProg = static_cast<float>(useMotion ? easeOutCubic(std::clamp(enterLinear / 0.88, 0.0, 1.0)) : 1.0);
                const float fThick = (std::max)(2.8f, ann.strokeWidth * 1.12f * strokeScale);
                const int pad = static_cast<int>(26 * strokeScale) + thick * 3;
                const cv::Rect bounds(rc.x - pad, rc.y - pad, rc.width + pad * 2, rc.height + pad * 2);
                const float cr = static_cast<float>(colBgr[2] / 255.0);
                const float cg = static_cast<float>(colBgr[1] / 255.0);
                const float cb = static_cast<float>(colBgr[0] / 255.0);

                if (drawProg > 0.015f) {
                    renderD2DRoiVectorOntoBgr(innerBgr, bounds, static_cast<float>(masterAlpha),
                        [&](ID2D1Factory* fac, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br,
                            ID2D1StrokeStyle* roundSt, float ox, float oy) {
                            if (!fac) return;
                            const float lcx = cx + ox;
                            const float lcy = cy + oy;
                            // 顺时针一气呵成画 392°（一整圈 + 顶部 32° 自然微距外飘优雅叠尾，零交叉打结、零缺口断裂）
                            constexpr int kTotalSamples = 128;
                            const int endSample = (std::max)(3, static_cast<int>(std::round(kTotalSamples * drawProg)));
                            const float tiltRad = -0.052f; // -3.0° 自然手写微倾角
                            const float cosTilt = std::cos(tiltRad);
                            const float sinTilt = std::sin(tiltRad);

                            auto evalMarkerPt = [&](float u, float dxOff = 0.0f, float dyOff = 0.0f) -> D2D1_POINT_2F {
                                // u in [0, 1] -> theta from -108 deg (-1.885 rad) clockwise by 392 deg (6.842 rad)
                                const float theta = -1.885f + u * 6.842f;
                                // 半径从 0.965 平滑渐扩至 1.055，使首尾交叠 32° 保持柔和平行微距，浑然天成
                                const float spiral = 0.965f + 0.090f * u;
                                const float organic = 1.0f + 0.012f * std::sin(theta * 2.0f + 0.6f);
                                const float ex = std::cos(theta) * rx * spiral * organic;
                                const float ey = std::sin(theta) * ry * spiral * organic;
                                return {
                                    lcx + ex * cosTilt - ey * sinTilt + dxOff,
                                    lcy + ex * sinTilt + ey * cosTilt + dyOff
                                };
                            };

                            auto buildSubPath = [&](int s0, int s1, float dxOff, float dyOff) -> ComPtr<ID2D1PathGeometry> {
                                ComPtr<ID2D1PathGeometry> g;
                                if (s1 <= s0 || FAILED(fac->CreatePathGeometry(g.GetAddressOf())) || !g) return nullptr;
                                ComPtr<ID2D1GeometrySink> sk;
                                if (SUCCEEDED(g->Open(sk.GetAddressOf())) && sk) {
                                    sk->BeginFigure(evalMarkerPt(static_cast<float>(s0) / kTotalSamples, dxOff, dyOff),
                                                    D2D1_FIGURE_BEGIN_HOLLOW);
                                    for (int i = s0 + 1; i <= s1; ++i) {
                                        sk->AddLine(evalMarkerPt(static_cast<float>(i) / kTotalSamples, dxOff, dyOff));
                                    }
                                    sk->EndFigure(D2D1_FIGURE_END_OPEN);
                                    sk->Close();
                                }
                                return g;
                            };

                            const ComPtr<ID2D1PathGeometry> shadowGeo =
                                buildSubPath(0, endSample, 1.4f * strokeScale, 2.2f * strokeScale);
                            const ComPtr<ID2D1PathGeometry> mainGeo =
                                buildSubPath(0, endSample, 0.0f, 0.0f);
                            if (!mainGeo) return;

                            // 1) 柔和立体暗影托底（深浅背景皆清晰醒目）
                            if (shadowGeo) {
                                br->SetColor(D2D1::ColorF(0.04f, 0.06f, 0.11f, 0.22f));
                                rt->DrawGeometry(shadowGeo.Get(), br, fThick + 1.2f, roundSt);
                            }

                            // 2) 外层轻柔马克笔边缘光晕（驻留期极轻柔呼吸）
                            const float glowBreath = useMotion
                                ? static_cast<float>(0.09 + 0.08 * (0.5 + 0.5 * std::sin(animSec * 3.8)))
                                : 0.10f;
                            br->SetColor(D2D1::ColorF(cr, cg, cb, glowBreath));
                            rt->DrawGeometry(mainGeo.Get(), br, fThick * 2.1f, roundSt);

                            // 3) 主体纯正饱满实心圆头马克笔迹（零夹心白线，白底与深底皆纯净醒目）
                            br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                            rt->DrawGeometry(mainGeo.Get(), br, fThick, roundSt);
                        });
                }
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
    bool rippleRight,
    StudioClickEffectStyle style) {
    if (canvasBgr.empty() || style == StudioClickEffectStyle::TactileOnly ||
        rippleProgress < 0.0f || rippleProgress > 1.0f) {
        return;
    }
    if (x < -80.0f || y < -80.0f || x > canvasBgr.cols + 80.0f || y > canvasBgr.rows + 80.0f) {
        return;
    }

    auto& cfg = tools3000::core::ConfigManager::instance();
    std::string colHex = rippleRight
        ? cfg.get<std::string>("/spotlight/rightClickColor", "#fb7185")
        : cfg.get<std::string>("/spotlight/leftClickColor", "auto");
    if (colHex == "auto" || colHex.empty()) {
        colHex = cfg.get<std::string>("/general/accentColor", "#38bdf8");
    }
    const cv::Scalar baseBgr = parseHexBgrScalar(
        colHex, rippleRight ? cv::Scalar(133, 113, 251) : cv::Scalar(248, 189, 56));
    const float accentB = static_cast<float>(baseBgr[0]);
    const float accentG = static_cast<float>(baseBgr[1]);
    const float accentR = static_cast<float>(baseBgr[2]);

    const float s = std::clamp(scale, 0.75f, 2.2f);
    const float p = std::clamp(rippleProgress, 0.0f, 1.0f);

    // 解析式非对称 SDF 单波剖面求解器（外沿凝练收边 + 内侧指数柔光羽化 + 环宽随扩散动态收细，零硬圈锯齿）
    auto evalAsymmetricWave = [&](float dist, float waveP, float rMin, float rSpan,
                                  float& outCrestMix) noexcept -> float {
        if (waveP <= 0.0f || waveP >= 1.0f) return 0.0f;
        const float inv = 1.0f - waveP;
        const float ease4 = 1.0f - inv * inv * inv * inv; // 四次缓出：清脆瞬态破开，尾段优雅消融
        const float R = (rMin + rSpan * ease4) * s;
        const float outerSigma = (std::max)(0.68f, (1.15f * (1.0f - 0.55f * waveP)) * s);
        const float innerDecay = (std::max)(1.20f, (5.40f * (0.52f + 0.48f * ease4) * (1.0f - 0.38f * waveP)) * s);
        const float envAlpha = std::pow(inv, 1.65f) * (std::min)(1.0f, waveP / 0.06f + 0.30f);

        float spatial = 0.0f;
        if (dist >= R) {
            const float od = (dist - R) / outerSigma;
            if (od < 3.6f) {
                spatial = std::exp(-0.5f * od * od);
            }
        } else {
            const float id = (R - dist) / innerDecay;
            if (id < 4.5f) {
                const float centerClear = std::clamp(dist / (std::max)(1.0f, R * 0.42f), 0.0f, 1.0f);
                spatial = std::exp(-id) * (centerClear * (2.0f - centerClear));
            }
        }
        const float crestProximity = std::exp(-std::abs(dist - R) / (std::max)(0.8f, 1.25f * s));
        outCrestMix = (std::max)(outCrestMix, crestProximity * spatial);
        return spatial * envAlpha;
    };

    const float maxBoundPx = (style == StudioClickEffectStyle::DualRipple ? 33.0f : 29.0f) * s + 4.0f;
    const int rInt = static_cast<int>(std::ceil(maxBoundPx));
    const int rx0 = (std::max)(0, static_cast<int>(std::floor(x)) - rInt);
    const int ry0 = (std::max)(0, static_cast<int>(std::floor(y)) - rInt);
    const int rx1 = (std::min)(canvasBgr.cols, static_cast<int>(std::ceil(x)) + rInt);
    const int ry1 = (std::min)(canvasBgr.rows, static_cast<int>(std::ceil(y)) + rInt);
    if (rx1 <= rx0 || ry1 <= ry0) return;

    const float invP = 1.0f - p;
    const float softEase3 = 1.0f - invP * invP * invP;
    const float softRadius = (6.0f + 17.5f * softEase3) * s;
    const float softTimeEnv = std::pow(invP, 1.45f) * (std::min)(1.0f, p / 0.08f + 0.35f);

    const float flashActive = (p < 0.28f) ? ((1.0f - p / 0.28f) * (1.0f - p / 0.28f)) : 0.0f;
    const float flashSigma = (std::max)(1.5f, 4.6f * s);

    for (int py = ry0; py < ry1; ++py) {
        uint8_t* row = canvasBgr.ptr<uint8_t>(py);
        const float dy = (static_cast<float>(py) + 0.5f) - y;
        const float dy2 = dy * dy;
        for (int px = rx0; px < rx1; ++px) {
            const float dx = (static_cast<float>(px) + 0.5f) - x;
            const float d2 = dx * dx + dy2;
            if (d2 > maxBoundPx * maxBoundPx) continue;
            const float dist = std::sqrt(d2);

            float alpha = 0.0f;
            float whiteHighlight = 0.0f;

            if (style == StudioClickEffectStyle::SoftTouch) {
                // Apple HIG 触控柔雾流：无环纯径向柔光斑，温润克制零线条
                if (dist < softRadius) {
                    const float u = dist / softRadius;
                    const float falloff = (1.0f - u * u) * (1.0f - u * u);
                    alpha = falloff * softTimeEnv * 0.42f;
                    whiteHighlight = (1.0f - u) * 0.28f;
                }
            } else if (style == StudioClickEffectStyle::DualRipple) {
                // 75ms 相位差错峰双细涟漪：主波先行破开，次波滞后从中心涌出
                const float p1 = std::clamp(p / 0.78f, 0.0f, 1.0f);
                const float p2 = std::clamp((p - 0.18f) / 0.82f, 0.0f, 1.0f);
                float crest1 = 0.0f, crest2 = 0.0f;
                const float w1 = evalAsymmetricWave(dist, p1, 3.8f, 23.5f, crest1) * 0.48f;
                const float w2 = evalAsymmetricWave(dist, p2, 3.2f, 16.5f, crest2) * 0.36f;
                alpha = std::clamp(w1 + w2 * (1.0f - w1 * 0.5f), 0.0f, 0.62f);
                whiteHighlight = std::clamp((std::max)(crest1, crest2) * 0.40f, 0.0f, 0.45f);
            } else {
                // 默认 CrystalPulse (Screen Studio 招牌微晶单波)：前段触点微闪 + 单道外锐内柔收细微晶光环
                float crest = 0.0f;
                const float ringVal = evalAsymmetricWave(dist, p, 3.8f, 21.5f, crest) * 0.46f;
                float flashVal = 0.0f;
                if (flashActive > 0.001f && dist < flashSigma * 3.0f) {
                    const float fd = dist / flashSigma;
                    flashVal = flashActive * std::exp(-0.5f * fd * fd) * 0.26f;
                }
                alpha = std::clamp(ringVal + flashVal * (1.0f - ringVal), 0.0f, 0.54f);
                whiteHighlight = std::clamp(crest * 0.44f + (flashVal > 0.02f ? 0.24f : 0.0f), 0.0f, 0.50f);
            }

            if (alpha <= 0.004f) continue;

            // 微晶高光折射调色 + 轻度滤色提亮混合，深浅背景皆晶莹通透不发灰
            const float tintB = accentB + (255.0f - accentB) * whiteHighlight;
            const float tintG = accentG + (255.0f - accentG) * whiteHighlight;
            const float tintR = accentR + (255.0f - accentR) * whiteHighlight;

            uint8_t* dst = row + px * 3;
            const float bgB = static_cast<float>(dst[0]);
            const float bgG = static_cast<float>(dst[1]);
            const float bgR = static_cast<float>(dst[2]);

            // 70% 标准 Alpha + 30% Screen 滤色增亮，模拟真实光学折射感
            const float scrB = 255.0f - ((255.0f - bgB) * (255.0f - tintB)) / 255.0f;
            const float scrG = 255.0f - ((255.0f - bgG) * (255.0f - tintG)) / 255.0f;
            const float scrR = 255.0f - ((255.0f - bgR) * (255.0f - tintR)) / 255.0f;
            const float targetB = tintB * 0.70f + scrB * 0.30f;
            const float targetG = tintG * 0.70f + scrG * 0.30f;
            const float targetR = tintR * 0.70f + scrR * 0.30f;

            dst[0] = static_cast<uint8_t>(std::clamp(bgB + (targetB - bgB) * alpha, 0.0f, 255.0f));
            dst[1] = static_cast<uint8_t>(std::clamp(bgG + (targetG - bgG) * alpha, 0.0f, 255.0f));
            dst[2] = static_cast<uint8_t>(std::clamp(bgR + (targetR - bgR) * alpha, 0.0f, 255.0f));
        }
    }
}

void StudioVideoCompositor::drawCrispVectorCursor(
    cv::Mat& canvasBgr,
    float x, float y,
    StudioCursorKind kind,
    float scale,
    float rippleProgress,
    bool rippleRight,
    bool highlightHalo,
    float tiltDeg,
    StudioClickEffectStyle style) {
    if (kind == StudioCursorKind::Hidden || canvasBgr.empty()) return;
    if (x < -32.0f || y < -32.0f || x > canvasBgr.cols + 32.0f || y > canvasBgr.rows + 32.0f) return;

    constexpr int kShift = 4;
    constexpr float kSub = 16.0f;
    const float rad = tiltDeg * (3.14159265f / 180.0f);
    const float cosT = std::cos(rad);
    const float sinT = std::sin(rad);
    const auto toSubPt = [&](float dx, float dy) -> cv::Point {
        const float rx = dx * cosT - dy * sinT;
        const float ry = dx * sinT + dy * cosT;
        return {
            static_cast<int>(std::round((x + rx * scale) * kSub)),
            static_cast<int>(std::round((y + ry * scale) * kSub))
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
        drawClickRippleEffect(canvasBgr, x, y, scale, rippleProgress, rippleRight, style);
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
    const std::vector<StudioAnnotationItem>* annotations,
    bool animateAnnotations,
    int64_t annotationPreviewElapsedMs,
    int previewTargetAnnIndex) {
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
        // 连续无阶跃亚像素对齐 + 全像素 INTER_AREA 面积积分抗混叠重采样：
        // 彻底消除旧版 floor(preScale) 运镜途中清晰度阶跃跳变及 INTER_LINEAR 下采样导致的细小代码文字高频闪烁摩尔纹噪点
        const bool isDownsampling = (effCropW > static_cast<float>(innerW) * 1.04f ||
                                     effCropH > static_cast<float>(innerH) * 1.04f);
        if (isDownsampling) {
            const int rx0 = std::clamp(static_cast<int>(std::floor(effCropX)) - 2, 0, srcBgr.cols - 1);
            const int ry0 = std::clamp(static_cast<int>(std::floor(effCropY)) - 2, 0, srcBgr.rows - 1);
            const int rx1 = std::clamp(static_cast<int>(std::ceil(effCropX + effCropW)) + 2, rx0 + 1, srcBgr.cols);
            const int ry1 = std::clamp(static_cast<int>(std::ceil(effCropY + effCropH)) + 2, ry0 + 1, srcBgr.rows);
            const cv::Mat srcPatch = srcBgr(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0));

            const int alignedW = (std::max)(innerW, static_cast<int>(std::round(effCropW)));
            const int alignedH = (std::max)(innerH, static_cast<int>(std::round(effCropH)));
            const double subX = static_cast<double>(effCropX) - static_cast<double>(rx0);
            const double subY = static_cast<double>(effCropY) - static_cast<double>(ry0);
            const double sx = static_cast<double>(effCropW) / static_cast<double>((std::max)(1, alignedW));
            const double sy = static_cast<double>(effCropH) / static_cast<double>((std::max)(1, alignedH));

            cv::Mat subAligned;
            const cv::Mat invM = (cv::Mat_<double>(2, 3) <<
                sx, 0.0, subX + 0.5 * sx - 0.5,
                0.0, sy, subY + 0.5 * sy - 0.5);
            cv::warpAffine(srcPatch, subAligned, invM, cv::Size(alignedW, alignedH),
                           cv::WARP_INVERSE_MAP | cv::INTER_LINEAR, cv::BORDER_REPLICATE);
            if (alignedW == innerW && alignedH == innerH) {
                zoomedInner = std::move(subAligned);
            } else {
                cv::resize(subAligned, zoomedInner, cv::Size(innerW, innerH), 0.0, 0.0, cv::INTER_AREA);
            }
        } else {
            const double adjSx = static_cast<double>(effCropW) / static_cast<double>((std::max)(1, innerW));
            const double adjSy = static_cast<double>(effCropH) / static_cast<double>((std::max)(1, innerH));
            const cv::Mat invM = (cv::Mat_<double>(2, 3) <<
                adjSx, 0.0, static_cast<double>(effCropX) + 0.5 * adjSx - 0.5,
                0.0, adjSy, static_cast<double>(effCropY) + 0.5 * adjSy - 0.5);
            cv::warpAffine(srcBgr, zoomedInner, invM, cv::Size(innerW, innerH),
                           cv::WARP_INVERSE_MAP | cv::INTER_CUBIC, cv::BORDER_REPLICATE);
        }
    }

    // 1. 渲染时间轴动态重点标注与隐私遮罩/光学放大镜（锚定源坐标，跟随 2D/3D 运镜无缝缩放平移）
    if (annotations && !annotations->empty()) {
        renderTimelineAnnotationsOnInner(
            zoomedInner, srcBgr, *annotations, eval.ptsMs,
            static_cast<float>(srcBgr.cols), static_cast<float>(srcBgr.rows),
            effCropX, effCropY, effCropW, effCropH,
            animateAnnotations, annotationPreviewElapsedMs, previewTargetAnnIndex);
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
                    eval.rippleIsRight,
                    config.clickEffectStyle);
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
                config.cursorHighlightHalo,
                eval.clickTiltDeg,
                config.clickEffectStyle);
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

    if (config.watermark.enabled) {
        WatermarkRenderer::applyInPlace(canvas, config.watermark, nullptr);
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

    uint64_t accessCounter = 0;
    size_t cachedBytes = 0;
    static constexpr size_t kMaxCacheBytes = 128ULL * 1024ULL * 1024ULL; // 严格锁定 <= 128MB 原生帧 LRU 预算
    static constexpr size_t kMaxCacheEntries = 20;
    std::vector<CachedEntry> cache;

    ~Impl() {
        reset();
    }

    void reset() noexcept {
        cache.clear();
        cachedBytes = 0;
        lastDecodedBgr.release();
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
        for (auto& e : cache) {
            if (std::abs(e.sec - sec) <= 0.012) {
                e.stamp = ++accessCounter;
                e.bgr = bgr;
                return;
            }
        }
        const size_t frameBytes = bgr.total() * bgr.elemSize();
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
    if (!m_impl || m_impl->cache.empty()) return false;
    const double clampedSec = (std::max)(0.0, targetSec);
    double bestDiff = toleranceSec + 1.0;
    Impl::CachedEntry* bestEntry = nullptr;
    for (auto& entry : m_impl->cache) {
        const double d = std::abs(entry.sec - clampedSec);
        if (d <= toleranceSec && d < bestDiff && !entry.bgr.empty()) {
            bestDiff = d;
            bestEntry = &entry;
        }
    }
    if (!bestEntry) return false;
    bestEntry->stamp = ++m_impl->accessCounter;
    outBgr = bestEntry->bgr;
    return true;
}

bool StudioHiResFrameReader::decodeFrameAt(double targetSec, cv::Mat& outBgr) {
    if (!isOpen()) return false;

    double clampedSec = (std::max)(0.0, targetSec);
    if (m_impl->durationSec > 0.05) {
        clampedSec = (std::min)(clampedSec, (std::max)(0.0, m_impl->durationSec - 0.01));
    }

    if (tryGetCachedFrame(clampedSec, outBgr, 0.018)) {
        return true;
    }

    const bool canRollForward =
        (m_impl->freshAtStreamStart && clampedSec <= 0.65) ||
        ((m_impl->lastDecodedSec >= -1e-4) &&
         (clampedSec >= m_impl->lastDecodedSec - 0.018) &&
         (clampedSec <= m_impl->lastDecodedSec + 0.65));

    if (!m_impl->freshAtStreamStart && canRollForward &&
        clampedSec <= m_impl->lastDecodedSec + 0.016 && !m_impl->lastDecodedBgr.empty()) {
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
    m_impl->insertCache(candidateSec, decodedBgr);
    outBgr = std::move(decodedBgr);
    return true;
}

} // namespace tools3000::capture

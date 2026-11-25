// ─────────────────────────────────────────────────────────────────────────────
// StudioVideoCompositor.cpp — 演播室视觉舞台合成核心（背景/缩略图/水晶框/字幕/光标与单帧合成）
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

namespace compositor_detail {

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


double qualityScaleFactor(StudioExportQuality q) noexcept {
    switch (q) {
        case StudioExportQuality::High:    return 1.65;
        case StudioExportQuality::Compact: return 0.56;
        case StudioExportQuality::Recommended:
        default:                           return 1.00;
    }
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


} // namespace compositor_detail

namespace {

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


std::filesystem::path resolveBuiltinWallpaperPathByName(const wchar_t* fname) {
    if (!fname || fname[0] == L'\0') return {};
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

struct WallpaperCacheSlot {
    std::wstring path;
    cv::Mat bgr;
};

std::mutex& globalWallpaperCacheMutex() {
    static std::mutex mtx;
    return mtx;
}

std::array<WallpaperCacheSlot, 8>& globalWallpaperCacheSlots() {
    static std::array<WallpaperCacheSlot, 8> slots;
    return slots;
}

size_t& globalWallpaperCacheNextSlot() {
    static size_t nextSlot = 0;
    return nextSlot;
}

std::mutex& globalThumbCacheMutex() {
    static std::mutex mtx;
    return mtx;
}

std::unordered_map<std::string, cv::Mat>& globalThumbCacheMap() {
    static std::unordered_map<std::string, cv::Mat> cache;
    return cache;
}

cv::Mat decodeWallpaperFileCached(const std::filesystem::path& filePath) {
    if (filePath.empty()) return {};
    auto& wallMtx = globalWallpaperCacheMutex();
    auto& slots = globalWallpaperCacheSlots();
    auto& nextSlot = globalWallpaperCacheNextSlot();

    const std::wstring key = filePath.wstring();
    {
        std::lock_guard<std::mutex> lk(wallMtx);
        for (const auto& sl : slots) {
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
        std::lock_guard<std::mutex> lk(wallMtx);
        slots[nextSlot] = {key, decoded};
        nextSlot = (nextSlot + 1) % slots.size();
    }
    return decoded;
}

cv::Mat decodeWallpaperFastThumbBgr(const std::filesystem::path& filePath) {
    if (filePath.empty()) return {};
    std::ifstream ifs(filePath, std::ios::binary | std::ios::ate);
    if (!ifs) return {};
    const auto sz = ifs.tellg();
    if (sz <= 0 || sz > 64 * 1024 * 1024) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), sz)) return {};

    // JPEG 1/8 DCT 快速缩略解码，比全量解码 1080p 快 15 倍且峰值内存几乎为零
    cv::Mat decoded = cv::imdecode(buf, cv::IMREAD_REDUCED_COLOR_8);
    if (decoded.empty()) {
        decoded = cv::imdecode(buf, cv::IMREAD_COLOR);
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
        // 含静态帧时长合并 + +transdiff 帧间透明差分下的实证桌面动图平均每像素压缩率
        const double bytesPerFrame = static_cast<double>(outSz.width) * static_cast<double>(outSz.height) * 0.036 * qScale;
        uint64_t est = static_cast<uint64_t>(std::llround(bytesPerFrame * effFps * safeDur));
        est = (std::max<uint64_t>)(est, 64ULL * 1024ULL);
        if (exportOpts.format == StudioExportFormat::GIF_WeChat && est > kWeChatGifSafeMaxBytes) {
            est = kWeChatGifSafeMaxBytes - 120'000ULL;
        }
        return est;
    }

    if (exportOpts.format == StudioExportFormat::WebP_Animated) {
        const int effFps = std::clamp(exportOpts.fps > 0 ? exportOpts.fps : 24, 6, 60);
        // 静态帧时长合并 + 桌面真彩帧间差分压缩后的实证估算
        const double bytesPerFrame = static_cast<double>(outSz.width) * static_cast<double>(outSz.height) * 0.012 * qScale;
        return (std::max<uint64_t>)(48ULL * 1024ULL, static_cast<uint64_t>(std::llround(bytesPerFrame * effFps * safeDur)));
    }

    // MP4 (x264 CRF) / HEVC / WebM (VP9 CQ) + 静态帧 VFR 抽帧与长 GOP 桌面专属高压缩实证估算
    const int effFps = std::clamp(exportOpts.fps > 0 ? exportOpts.fps : 30, 10, 120);
    const double fpsRatio = std::clamp(static_cast<double>(effFps) / 30.0, 0.5, 2.2);
    const bool isHevc = (exportOpts.format == StudioExportFormat::MP4_HEVC);
    const bool isWebm = (exportOpts.format == StudioExportFormat::WebM_VP9);
    const double baseMbps = isWebm ? 0.85 : (isHevc ? 0.90 : 1.15);
    const double effMbps = std::clamp(areaRatio * fpsRatio * qScale * baseMbps, 0.22, 16.0);
    const uint64_t videoBytes = static_cast<uint64_t>(std::llround((effMbps * 1'000'000.0 / 8.0) * safeDur));
    const uint64_t audioBytes = static_cast<uint64_t>(std::llround((isWebm ? 12'000.0 : 16'000.0) * safeDur));
    return (std::max<uint64_t>)(64ULL * 1024ULL, videoBytes + audioBytes);
}

void StudioVideoCompositor::renderArtisticBackdropBgr(
    cv::Mat& outBgr,
    int width,
    int height,
    BeautyBackgroundType theme,
    const std::string& customPathUtf8,
    const std::string& presetKey) {
    if (width <= 0 || height <= 0) return;

    // 1. 优先尝试加载用户选定的自定义壁纸
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

    // 2. 通过单一事实源目录解析内置高定预设（21 款原创壁纸 + 28 款零体积多光源渐变/纯色）
    const StudioBackdropPreset& preset = StudioBackdropCatalog::resolvePreset(presetKey, theme);
    if (preset.wallpaperFilename && preset.wallpaperFilename[0] != L'\0') {
        const auto builtinPath = resolveBuiltinWallpaperPathByName(preset.wallpaperFilename);
        if (!builtinPath.empty()) {
            const cv::Mat builtinImg = decodeWallpaperFileCached(builtinPath);
            if (!builtinImg.empty()) {
                cropResizeCoverBgr(builtinImg, outBgr, width, height);
                return;
            }
        }
    }

    // 3. 零体积高定二维流体域扭曲丝绸极光着色器 / 远山兜底渲染（任意 4K 零锯齿 + 4x4 Bayer + TPDF 磨砂）
    const bool isFluidSilk = (std::strncmp(preset.key, "mesh_", 5) == 0);
    const int gw = isFluidSilk ? std::clamp(width / 6, 64, 320) : std::clamp(width / 10, 48, 220);
    const int gh = isFluidSilk ? std::clamp(height / 6, 40, 180) : std::clamp(height / 10, 32, 140);
    cv::Mat gridF32(gh, gw, CV_32FC3);

    if (preset.hasLegacyEnum &&
        (preset.legacyTheme == BeautyBackgroundType::DuskMountains ||
         preset.legacyTheme == BeautyBackgroundType::MistyLakeDawn)) {
        renderLandscapeBackdropLowRes(gridF32, preset.legacyTheme);
    } else {
        const StudioMeshBackdropSpec& spec = preset.meshSpec;
        // 用预设键哈希生成独一无二的丝绸流向相位，使 21 款渐变每一款拥有不同折面姿态
        uint32_t keyHash = 2166136261u;
        for (const char* p = preset.key; p && *p; ++p) {
            keyHash = (keyHash ^ static_cast<uint8_t>(*p)) * 16777619u;
        }
        const float phaseA = static_cast<float>(keyHash & 0xFFu) * 0.0245f;
        const float phaseB = static_cast<float>((keyHash >> 8) & 0xFFu) * 0.0245f;
        const float flipDir = ((keyHash & 0x10000u) != 0u) ? -1.0f : 1.0f;

        for (int y = 0; y < gh; ++y) {
            const float ny = static_cast<float>(y) / static_cast<float>((std::max)(1, gh - 1));
            auto* row = gridF32.ptr<cv::Vec3f>(y);
            for (int x = 0; x < gw; ++x) {
                const float nx = static_cast<float>(x) / static_cast<float>((std::max)(1, gw - 1));
                float wx = nx;
                float wy = ny;
                float foldRidge = 0.0f;
                float foldValley = 0.0f;
                if (isFluidSilk) {
                    // 双级迭代流体域扭曲 (2-Octave Domain Warping)：雕琢具有真实丝绸褶皱张力的流光彩带
                    const float qx = nx + 0.22f * std::sin(ny * 3.6f + phaseA) + 0.09f * std::cos((nx + ny * flipDir) * 5.4f + phaseB);
                    const float qy = ny + 0.22f * std::cos(nx * 3.3f + phaseB) + 0.09f * std::sin((nx * flipDir - ny) * 5.1f + phaseA);
                    wx = qx + 0.12f * std::sin(qy * 5.8f - qx * 2.4f + phaseB);
                    wy = qy + 0.12f * std::cos(qx * 5.2f + qy * 2.6f + phaseA);

                    const float wave1 = std::sin(wx * 5.4f - wy * 4.1f * flipDir + phaseA);
                    const float wave2 = std::cos(wx * 3.1f + wy * 6.2f + phaseB);
                    foldRidge = std::pow((std::max)(0.0f, wave1 * 0.68f + wave2 * 0.32f), 3.2f);
                    foldValley = std::pow((std::max)(0.0f, -wave1 * 0.65f - wave2 * 0.35f), 2.2f);
                }
                cv::Vec3f col = spec.baseBgr;
                for (const auto& orb : spec.orbs) {
                    if (orb.weight <= 0.0001f) continue;
                    const float dx = wx - orb.nx;
                    const float dy = wy - orb.ny;
                    const float d2 = dx * dx + dy * dy;
                    const float effRadius = isFluidSilk ? (orb.radius * 0.64f) : orb.radius;
                    const float r2 = (std::max)(0.035f, effRadius * effRadius);
                    const float w = std::clamp(std::exp(-d2 / r2) * orb.weight, 0.0f, 1.0f);
                    col = col * (1.0f - w) + orb.bgr * w;
                }
                if (isFluidSilk) {
                    // 立体丝绸明暗折面着色：波谷深邃沉稳、波峰透出丝缎镜面流光
                    const float valleyMul = 1.0f - foldValley * (preset.isLightTheme ? 0.11f : 0.24f);
                    col *= valleyMul;
                    const float crestLift = foldRidge * (preset.isLightTheme ? 26.0f : 42.0f);
                    col = col + (cv::Vec3f(255.0f, 255.0f, 255.0f) - col) * (crestLift / 255.0f);
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

    // 4x4 有序亚像素微抖动 + 空间锁定 TPDF 有机磨砂微颗粒（零色带 + 苹果官网级细腻磨砂质感）
    static constexpr float kBayer4x4[4][4] = {
        {-0.46875f,  0.03125f, -0.34375f,  0.15625f},
        { 0.28125f, -0.21875f,  0.40625f, -0.09375f},
        {-0.28125f,  0.21875f, -0.40625f,  0.09375f},
        { 0.46875f, -0.03125f,  0.34375f, -0.15625f},
    };
    const float grainAmp = (width >= 200) ? std::clamp(preset.meshSpec.grainStrength, 0.0f, 4.0f) : 0.0f;

    outBgr.create(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        const auto* sRow = resizedF32.ptr<cv::Vec3f>(y);
        uint8_t* dRow = outBgr.ptr<uint8_t>(y);
        const float* ditherRow = kBayer4x4[y & 3];
        const uint32_t hy = static_cast<uint32_t>(y) * 668265263u;
        for (int x = 0; x < width; ++x) {
            float delta = ditherRow[x & 3] * 1.35f;
            if (grainAmp > 0.01f) {
                uint32_t h = (static_cast<uint32_t>(x) * 374761393u) ^ hy;
                h = (h ^ (h >> 13)) * 1274126177u;
                h ^= (h >> 16);
                const float tpdf = (static_cast<float>(h & 0xFFFFu) + static_cast<float>((h >> 16) & 0xFFFFu))
                                   * (1.0f / 65535.0f) - 1.0f;
                delta += tpdf * grainAmp;
            }
            dRow[x * 3 + 0] = static_cast<uint8_t>(std::clamp(sRow[x][0] + delta, 0.0f, 255.0f));
            dRow[x * 3 + 1] = static_cast<uint8_t>(std::clamp(sRow[x][1] + delta, 0.0f, 255.0f));
            dRow[x * 3 + 2] = static_cast<uint8_t>(std::clamp(sRow[x][2] + delta, 0.0f, 255.0f));
        }
    }
}

cv::Mat StudioVideoCompositor::renderThemeThumbnailBgra(
    BeautyBackgroundType theme,
    int width,
    int height) {
    return renderPresetThumbnailBgra(StudioBackdropCatalog::canonicalKeyForLegacyTheme(theme), theme, width, height);
}

cv::Mat StudioVideoCompositor::renderPresetThumbnailBgra(
    const std::string& presetKey,
    BeautyBackgroundType fallbackTheme,
    int width,
    int height) {
    const int safeW = (std::max)(16, width);
    const int safeH = (std::max)(12, height);
    const StudioBackdropPreset& preset = StudioBackdropCatalog::resolvePreset(presetKey, fallbackTheme);
    const std::string cacheKey = std::string(preset.key) + "#" + std::to_string(safeW) + "x" + std::to_string(safeH);

    auto& thumbMtx = globalThumbCacheMutex();
    auto& thumbCache = globalThumbCacheMap();
    {
        std::lock_guard<std::mutex> lk(thumbMtx);
        const auto it = thumbCache.find(cacheKey);
        if (it != thumbCache.end() && !it->second.empty()) {
            return it->second;
        }
    }

    cv::Mat bgr;
    if (preset.wallpaperFilename && preset.wallpaperFilename[0] != L'\0') {
        const auto builtinPath = resolveBuiltinWallpaperPathByName(preset.wallpaperFilename);
        if (!builtinPath.empty()) {
            const cv::Mat fastImg = decodeWallpaperFastThumbBgr(builtinPath);
            if (!fastImg.empty()) {
                cropResizeCoverBgr(fastImg, bgr, safeW, safeH);
            }
        }
    }
    if (bgr.empty()) {
        renderArtisticBackdropBgr(bgr, safeW, safeH, preset.legacyTheme, "", preset.key);
    }

    cv::Mat bgra;
    cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);

    {
        std::lock_guard<std::mutex> lk(thumbMtx);
        if (thumbCache.size() >= 128) {
            thumbCache.clear();
        }
        thumbCache[cacheKey] = bgra;
    }
    return bgra;
}

void StudioVideoCompositor::clearCaches() noexcept {
    m_cachedBackdropBgr.release();
    m_cacheW = 0;
    m_cacheH = 0;
    m_cachePresetKey.clear();
    m_cachePresetKey.shrink_to_fit();
    m_cacheCustomBgPath.clear();
    m_cacheCustomBgPath.shrink_to_fit();
    m_cacheGlassTintHex.clear();
    m_cachedCaptionText.clear();
    m_cachedCaptionCanvasW = 0;
    m_cachedCaptionBgra.release();
}

void StudioVideoCompositor::clearGlobalCaches() noexcept {
    {
        std::lock_guard<std::mutex> lk(globalWallpaperCacheMutex());
        for (auto& sl : globalWallpaperCacheSlots()) {
            sl.path.clear();
            sl.bgr.release();
        }
        globalWallpaperCacheNextSlot() = 0;
    }
    {
        std::lock_guard<std::mutex> lk(globalThumbCacheMutex());
        globalThumbCacheMap().clear();
    }
}

cv::Mat StudioVideoCompositor::renderCustomWallpaperThumbnailBgra(
    const std::string& customPathUtf8,
    int width,
    int height) {
    const int safeW = (std::max)(16, width);
    const int safeH = (std::max)(12, height);
    cv::Mat bgr;
    if (!customPathUtf8.empty()) {
        const auto wpath = tools3000::core::WinUtils::utf8ToWstring(customPathUtf8);
        if (!wpath.empty()) {
            const cv::Mat fastImg = decodeWallpaperFastThumbBgr(std::filesystem::path(wpath));
            if (!fastImg.empty()) {
                cropResizeCoverBgr(fastImg, bgr, safeW, safeH);
            }
        }
    }
    if (bgr.empty()) {
        renderArtisticBackdropBgr(bgr, safeW, safeH, BeautyBackgroundType::StudioSlate, customPathUtf8);
    }
    cv::Mat bgra;
    cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
    return bgra;
}

void StudioVideoCompositor::ensureBackdropCache(
    int outW, int outH,
    bool autoFrame,
    BeautyBackgroundType theme,
    const std::string& presetKey,
    const std::string& customBgPath,
    float bgBlur,
    float shadowIntensity,
    float glassBezelWidth,
    const std::string& glassTintHex,
    int padX, int padY,
    int innerW, int innerH,
    float radius) {
    const float safeBlur = std::clamp(bgBlur, 0.0f, 48.0f);
    const float safeShadow = std::clamp(shadowIntensity, 0.0f, 1.0f);
    const float safeBezelCfg = std::clamp(glassBezelWidth, 0.0f, 20.0f);
    const StudioBackdropPreset& resolvedPreset = StudioBackdropCatalog::resolvePreset(presetKey, theme);
    const std::string effectiveKey(resolvedPreset.key);
    if (!m_cachedBackdropBgr.empty() &&
        m_cacheW == outW && m_cacheH == outH &&
        m_cacheAutoFrame == autoFrame &&
        m_cacheTheme == theme &&
        m_cachePresetKey == effectiveKey &&
        m_cacheCustomBgPath == customBgPath &&
        std::abs(m_cacheBgBlur - safeBlur) < 0.25f &&
        std::abs(m_cacheShadowIntensity - safeShadow) < 0.015f &&
        std::abs(m_cacheGlassBezel - safeBezelCfg) < 0.15f &&
        m_cacheGlassTintHex == glassTintHex &&
        m_cachePadX == padX && m_cachePadY == padY &&
        m_cacheInnerW == innerW && m_cacheInnerH == innerH &&
        std::abs(m_cacheRadius - radius) < 0.2f) {
        return;
    }

    m_cacheW = outW;
    m_cacheH = outH;
    m_cacheAutoFrame = autoFrame;
    m_cacheTheme = theme;
    m_cachePresetKey = effectiveKey;
    m_cacheCustomBgPath = customBgPath;
    m_cacheBgBlur = safeBlur;
    m_cacheShadowIntensity = safeShadow;
    m_cacheGlassBezel = safeBezelCfg;
    m_cacheGlassTintHex = glassTintHex;
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

    renderArtisticBackdropBgr(m_cachedBackdropBgr, outW, outH, theme, customBgPath, effectiveKey);

    // 可调节背景景深高斯模糊 (Background Blur: 0 ~ 40px)
    if (safeBlur > 0.4f && !m_cachedBackdropBgr.empty()) {
        int bKernel = static_cast<int>(std::round(safeBlur * (static_cast<float>(outH) / 480.0f))) * 2 + 1;
        bKernel = std::clamp(bKernel, 3, 121);
        if ((bKernel & 1) == 0) ++bKernel;
        cv::GaussianBlur(m_cachedBackdropBgr, m_cachedBackdropBgr, cv::Size(bKernel, bKernel), 0.0);
    }

    // 保存未叠加投影的纯净流光底图副本，供 VisionOS 水晶玻璃外托框做真实物理折射采样
    const cv::Mat cleanBackdrop = m_cachedBackdropBgr.clone();
    const bool lightTheme = customBgPath.empty() && resolvedPreset.isLightTheme;

    // 计算同心圆角水晶玻璃外托框几何 (Outer Concentric Glass Matting Bezel)
    const float dpiScale = std::clamp(static_cast<float>(outH) / 1080.0f, 0.45f, 2.5f);
    const int maxAvailBezel = (std::max)(0, (std::min)(padX, padY) - 3);
    const int bezelPx = (safeBezelCfg > 0.35f && maxAvailBezel >= 2)
        ? std::clamp(static_cast<int>(std::round(safeBezelCfg * dpiScale)), 2, maxAvailBezel)
        : 0;
    const cv::Rect frameRc(padX - bezelPx, padY - bezelPx, innerW + bezelPx * 2, innerH + bezelPx * 2);
    const float outerRadius = (bezelPx > 0) ? (radius + static_cast<float>(bezelPx)) : radius;

    // 1. 三层电影级画廊空气悬浮深影 (3-Tier Gallery Floating Shadow: Contact + Mid + 150px Deep Diffusion)
    if (safeShadow > 0.01f && (padX > 2 || padY > 2)) {
        // 在 1/4 分辨率极速计算中层体积影与 150px 超广域空气漫射影
        const int sw = (std::max)(32, outW / 4);
        const int sh = (std::max)(24, outH / 4);
        const float sx = static_cast<float>(sw) / static_cast<float>(outW);
        const float sy = static_cast<float>(sh) / static_cast<float>(outH);

        auto makeLowResShadow = [&](float offYPx, int equivBlurPx) -> cv::Mat {
            cv::Mat m(sh, sw, CV_8UC1, cv::Scalar(0));
            const int rx = static_cast<int>(std::round(frameRc.x * sx));
            const int ry = static_cast<int>(std::round((frameRc.y + offYPx) * sy));
            const int rw = (std::max)(4, static_cast<int>(std::round(frameRc.width * sx)));
            const int rh = (std::max)(4, static_cast<int>(std::round(frameRc.height * sy)));
            fillRoundedRectMask(m, cv::Rect(rx, ry, rw, rh), outerRadius * sx);
            int k = (std::max)(5, static_cast<int>(std::round(equivBlurPx * sx)) | 1);
            if ((k & 1) == 0) ++k;
            cv::GaussianBlur(m, m, cv::Size(k, k), 0.0);
            cv::Mat up;
            cv::resize(m, up, cv::Size(outW, outH), 0.0, 0.0, cv::INTER_LINEAR);
            return up;
        };

        const cv::Mat deepShadow = makeLowResShadow(32.0f * dpiScale * (0.5f + 0.6f * safeShadow),
                                                    static_cast<int>(std::round(152.0f * dpiScale)));
        const cv::Mat midShadow  = makeLowResShadow(13.0f * dpiScale * (0.5f + 0.6f * safeShadow),
                                                    static_cast<int>(std::round(46.0f * dpiScale)));

        cv::Mat contactShadow(outH, outW, CV_8UC1, cv::Scalar(0));
        fillRoundedRectMask(
            contactShadow,
            cv::Rect(frameRc.x, (std::min)(outH - frameRc.height, frameRc.y + (std::max)(2, static_cast<int>(std::round(3.0f * dpiScale)))),
                     frameRc.width, frameRc.height),
            outerRadius);
        const int ck = (std::max)(7, static_cast<int>(std::round(13.0f * dpiScale)) | 1);
        cv::GaussianBlur(contactShadow, contactShadow, cv::Size(ck, ck), 0.0);

        const float baseMaxAlpha = lightTheme ? 0.36f : 0.68f;
        const float masterAlpha = baseMaxAlpha * safeShadow;

        for (int y = 0; y < outH; ++y) {
            uint8_t* bgrRow = m_cachedBackdropBgr.ptr<uint8_t>(y);
            const uint8_t* dRow = deepShadow.ptr<uint8_t>(y);
            const uint8_t* mRow = midShadow.ptr<uint8_t>(y);
            const uint8_t* cRow = contactShadow.ptr<uint8_t>(y);
            for (int x = 0; x < outW; ++x) {
                const float combined = (0.48f * dRow[x] + 0.32f * mRow[x] + 0.26f * cRow[x]) * (1.0f / 255.0f);
                const float shAlpha = std::clamp(combined * masterAlpha, 0.0f, 0.86f);
                if (shAlpha > 0.002f) {
                    const float keep = 1.0f - shAlpha;
                    // 深海冷调空气透乘，避免纯黑死灰脏边
                    bgrRow[x * 3 + 0] = static_cast<uint8_t>(bgrRow[x * 3 + 0] * (keep + shAlpha * 0.05f));
                    bgrRow[x * 3 + 1] = static_cast<uint8_t>(bgrRow[x * 3 + 1] * (keep + shAlpha * 0.03f));
                    bgrRow[x * 3 + 2] = static_cast<uint8_t>(bgrRow[x * 3 + 2] * keep);
                }
            }
        }
    }

    // 解析圆角矩形精确有符号距离场 (SDF < 0 在框内，SDF = 0 为物理边界)
    auto evalRoundRectSdf = [](float px, float py, const cv::Rect& rc, float rad) noexcept -> float {
        const float w = static_cast<float>(rc.width);
        const float h = static_cast<float>(rc.height);
        const float r = std::clamp(rad, 0.0f, (std::min)(w, h) * 0.5f);
        const float cx = static_cast<float>(rc.x) + w * 0.5f;
        const float cy = static_cast<float>(rc.y) + h * 0.5f;
        const float qx = std::abs(px - cx) - (w * 0.5f - r);
        const float qy = std::abs(py - cy) - (h * 0.5f - r);
        const float outDist = std::hypot((std::max)(qx, 0.0f), (std::max)(qy, 0.0f));
        const float inDist = (std::min)((std::max)(qx, qy), 0.0f);
        return outDist + inDist - r;
    };

    // 2. Apple VisionOS 真实物理折射磨砂水晶玻璃外托框 (Translucent Frosted Crystal Glass Matting)
    if (bezelPx >= 2) {
        cv::Mat frostedBg;
        const int fk = (std::max)(15, static_cast<int>(std::round(31.0f * dpiScale)) | 1);
        cv::GaussianBlur(cleanBackdrop, frostedBg, cv::Size(fk, fk), 0.0);

        const cv::Scalar tintScalar = parseHexBgrScalar(glassTintHex, cv::Scalar(255, 255, 255));
        const bool isDarkTint = (tintScalar[0] + tintScalar[1] + tintScalar[2]) < 180.0;
        const cv::Vec3f tintBgr(static_cast<float>(tintScalar[0]),
                                static_cast<float>(tintScalar[1]),
                                static_cast<float>(tintScalar[2]));
        const float tintMix = isDarkTint ? 0.52f : (lightTheme ? 0.44f : 0.22f);

        const int x0 = (std::max)(0, frameRc.x - 2);
        const int y0 = (std::max)(0, frameRc.y - 2);
        const int x1 = (std::min)(outW, frameRc.x + frameRc.width + 2);
        const int y1 = (std::min)(outH, frameRc.y + frameRc.height + 2);
        const float fcx = frameRc.x + frameRc.width * 0.5f;
        const float fcy = frameRc.y + frameRc.height * 0.5f;
        const float invDiag = 1.0f / (std::max)(1.0f, std::hypot(frameRc.width * 0.5f, frameRc.height * 0.5f));

        for (int y = y0; y < y1; ++y) {
            // 仅计算水晶外框四边环带区域，跳过内部即将被视频覆盖的中心大矩形以极致提速
            const bool insideInnerY = (y >= padY + static_cast<int>(radius) + 2 &&
                                       y < padY + innerH - static_cast<int>(radius) - 2);
            uint8_t* dstRow = m_cachedBackdropBgr.ptr<uint8_t>(y);
            const uint8_t* frRow = frostedBg.ptr<uint8_t>(y);
            const float py = static_cast<float>(y) + 0.5f;

            for (int x = x0; x < x1; ++x) {
                if (insideInnerY && x >= padX + 2 && x < padX + innerW - 2) {
                    x = padX + innerW - 3;
                    continue;
                }
                const float px = static_cast<float>(x) + 0.5f;
                const float sdfOuter = evalRoundRectSdf(px, py, frameRc, outerRadius);
                if (sdfOuter > 1.0f) continue;

                const float glassCoverage = std::clamp(0.5f - sdfOuter, 0.0f, 1.0f);
                // 左上 45 度天光方向性调制：左上边缘晶莹清亮，右下边缘温润沉稳
                const float dirLight = std::clamp(0.5f - ((px - fcx) + (py - fcy)) * invDiag * 0.55f, 0.0f, 1.0f);

                cv::Vec3f glassCol(frRow[x * 3 + 0] * 1.06f, frRow[x * 3 + 1] * 1.06f, frRow[x * 3 + 2] * 1.06f);
                const float localMix = tintMix * (0.78f + 0.42f * dirLight);
                glassCol = glassCol * (1.0f - localMix) + tintBgr * localMix;

                // 外沿 1.35px 亚像素方向性高光倒角棱线 (Directional Specular Rim)
                const float rimBand = std::clamp(1.35f - std::abs(sdfOuter + 0.55f), 0.0f, 1.0f);
                if (rimBand > 0.01f) {
                    const float rimAlpha = rimBand * (isDarkTint ? (0.14f + 0.28f * dirLight)
                                                                 : (0.20f + 0.58f * dirLight));
                    glassCol = glassCol * (1.0f - rimAlpha) + cv::Vec3f(255.0f, 255.0f, 255.0f) * rimAlpha;
                }

                const cv::Vec3f bgOrig(dstRow[x * 3 + 0], dstRow[x * 3 + 1], dstRow[x * 3 + 2]);
                const cv::Vec3f outCol = bgOrig * (1.0f - glassCoverage) + glassCol * glassCoverage;
                dstRow[x * 3 + 0] = static_cast<uint8_t>(std::clamp(outCol[0], 0.0f, 255.0f));
                dstRow[x * 3 + 1] = static_cast<uint8_t>(std::clamp(outCol[1], 0.0f, 255.0f));
                dstRow[x * 3 + 2] = static_cast<uint8_t>(std::clamp(outCol[2], 0.0f, 255.0f));
            }
        }
    } else if (padX >= 2 && padY >= 2) {
        // 无外框极简模式：仍绘制 1.25px 解析式 SDF 亚像素方向性高光微棱边（彻底消灭旧版粗糙折线错位）
        const cv::Rect innerRc(padX, padY, innerW, innerH);
        const int x0 = (std::max)(0, padX - 2);
        const int y0 = (std::max)(0, padY - 2);
        const int x1 = (std::min)(outW, padX + innerW + 2);
        const int y1 = (std::min)(outH, padY + innerH + 2);
        const float fcx = innerRc.x + innerRc.width * 0.5f;
        const float fcy = innerRc.y + innerRc.height * 0.5f;
        const float invDiag = 1.0f / (std::max)(1.0f, std::hypot(innerRc.width * 0.5f, innerRc.height * 0.5f));

        for (int y = y0; y < y1; ++y) {
            const bool insideCoreY = (y >= padY + static_cast<int>(radius) + 2 &&
                                      y < padY + innerH - static_cast<int>(radius) - 2);
            uint8_t* dstRow = m_cachedBackdropBgr.ptr<uint8_t>(y);
            const float py = static_cast<float>(y) + 0.5f;
            for (int x = x0; x < x1; ++x) {
                if (insideCoreY && x >= padX + 2 && x < padX + innerW - 2) {
                    x = padX + innerW - 3;
                    continue;
                }
                const float px = static_cast<float>(x) + 0.5f;
                const float sdf = evalRoundRectSdf(px, py, innerRc, radius);
                const float band = std::clamp(1.25f - std::abs(sdf + 0.5f), 0.0f, 1.0f);
                if (band > 0.01f) {
                    const float dirLight = std::clamp(0.5f - ((px - fcx) + (py - fcy)) * invDiag * 0.55f, 0.0f, 1.0f);
                    const float a = band * (lightTheme ? (0.30f + 0.45f * dirLight) : (0.14f + 0.38f * dirLight));
                    dstRow[x * 3 + 0] = static_cast<uint8_t>(std::clamp(dstRow[x * 3 + 0] * (1.0f - a) + 255.0f * a, 0.0f, 255.0f));
                    dstRow[x * 3 + 1] = static_cast<uint8_t>(std::clamp(dstRow[x * 3 + 1] * (1.0f - a) + 255.0f * a, 0.0f, 255.0f));
                    dstRow[x * 3 + 2] = static_cast<uint8_t>(std::clamp(dstRow[x * 3 + 2] * (1.0f - a) + 255.0f * a, 0.0f, 255.0f));
                }
            }
        }
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
            ? std::clamp(config.paddingRatio, 0.0f, 0.22f)
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
        config.backgroundPresetKey,
        config.customBackgroundPath,
        config.backgroundBlur,
        has3DTilt ? (config.shadowIntensity * 0.45f) : config.shadowIntensity,
        (!has3DTilt && config.autoFrameEnabled) ? config.insetBorderWidth : 0.0f,
        config.insetBorderColorHex,
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

    // 0.8 聚焦区间光学羽化聚光暗角 (Focus Spotlight Vignette)：保持焦点区域 100% 通透，柔和压暗外围上下文零空间割裂引导视线
    if (eval.spotlightAlpha > 0.004f && eval.spotlightHalfW > 1.0f && eval.spotlightHalfH > 1.0f &&
        !zoomedInner.empty() && zoomedInner.channels() == 3) {
        const float safeCropW = (eval.camera.cropW > 0.5f) ? eval.camera.cropW : refSrcW;
        const float safeCropH = (eval.camera.cropH > 0.5f) ? eval.camera.cropH : refSrcH;
        const float cxNorm = (eval.spotlightCenterX - eval.camera.cropX) / (std::max)(1.0f, safeCropW);
        const float cyNorm = (eval.spotlightCenterY - eval.camera.cropY) / (std::max)(1.0f, safeCropH);
        const float hwNorm = eval.spotlightHalfW / (std::max)(1.0f, safeCropW);
        const float hhNorm = eval.spotlightHalfH / (std::max)(1.0f, safeCropH);

        constexpr int kMaskW = 128;
        const int kMaskH = std::clamp(
            static_cast<int>(std::lround(static_cast<double>(kMaskW) * innerH / (std::max)(1, innerW))), 48, 128);
        cv::Mat maskSmall(kMaskH, kMaskW, CV_8UC1, cv::Scalar(0));

        const int mx0 = static_cast<int>(std::lround((cxNorm - hwNorm) * kMaskW));
        const int my0 = static_cast<int>(std::lround((cyNorm - hhNorm) * kMaskH));
        const int mx1 = static_cast<int>(std::lround((cxNorm + hwNorm) * kMaskW));
        const int my1 = static_cast<int>(std::lround((cyNorm + hhNorm) * kMaskH));
        const int rx0 = std::clamp(mx0, 0, kMaskW - 1);
        const int ry0 = std::clamp(my0, 0, kMaskH - 1);
        const int rx1 = std::clamp(mx1, rx0 + 1, kMaskW);
        const int ry1 = std::clamp(my1, ry0 + 1, kMaskH);
        const float mRad = (std::min)(14.0f, static_cast<float>((std::min)(rx1 - rx0, ry1 - ry0)) * 0.28f);
        fillRoundedRectMask(maskSmall, cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0), mRad);
        cv::GaussianBlur(maskSmall, maskSmall, cv::Size(25, 25), 0.0, 0.0, cv::BORDER_REPLICATE);

        cv::Mat maskFull;
        cv::resize(maskSmall, maskFull, cv::Size(innerW, innerH), 0.0, 0.0, cv::INTER_LINEAR);

        const int dim256 = std::clamp(static_cast<int>(std::lround(eval.spotlightAlpha * 256.0f)), 0, 176);
        if (dim256 > 0) {
            for (int r = 0; r < innerH; ++r) {
                cv::Vec3b* dstRow = zoomedInner.ptr<cv::Vec3b>(r);
                const uchar* mRow = maskFull.ptr<uchar>(r);
                for (int c = 0; c < innerW; ++c) {
                    const int outside = 255 - static_cast<int>(mRow[c]);
                    if (outside <= 0) continue;
                    const int gain = 256 - ((dim256 * outside) >> 8);
                    dstRow[c][0] = static_cast<uchar>((static_cast<int>(dstRow[c][0]) * gain) >> 8);
                    dstRow[c][1] = static_cast<uchar>((static_cast<int>(dstRow[c][1]) * gain) >> 8);
                    dstRow[c][2] = static_cast<uchar>((static_cast<int>(dstRow[c][2]) * gain) >> 8);
                }
            }
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

    // 1.5 内边缘微晶过渡暗镶线（外侧同心磨砂水晶玻璃托框已在 ensureBackdropCache 中物理折射合成，不再向画面内画死白粗线遮挡内容）
    if (config.autoFrameEnabled && config.insetBorderWidth > 0.35f && !has3DTilt) {
        cv::Mat innerRim = zoomedInner.clone();
        const cv::Rect bdRc(0, 0, (std::max)(4, innerW - 1), (std::max)(4, innerH - 1));
        drawRoundedBoxStrokeBgr(innerRim, bdRc, static_cast<int>(std::round(cornerR)), cv::Scalar(255, 255, 255), 1);
        cv::addWeighted(innerRim, 0.22, zoomedInner, 0.78, 0.0, zoomedInner);
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

} // namespace tools3000::capture

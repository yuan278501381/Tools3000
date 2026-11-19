// ─────────────────────────────────────────────────────────────────────────────
// TextReplaceEngine.cpp — 截图原位无痕改字核心引擎实现
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/TextReplaceEngine.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <winhttp.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <robuffer.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.AI.MachineLearning.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <fstream>
#include <memory>
#include <numeric>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
namespace waml = winrt::Windows::AI::MachineLearning;

namespace tools3000::capture {

namespace {

constexpr const wchar_t* kInpaintModelFileName = L"migan_pipeline_v2.onnx";
constexpr uint64_t kInpaintModelExpectedBytes = 28079181ULL;
constexpr uint64_t kInpaintModelMinValidBytes = 10'000'000ULL;

const std::array<const wchar_t*, 2> kInpaintModelUrls = {{
    L"https://hf-mirror.com/andraniksargsyan/migan/resolve/main/migan_pipeline_v2.onnx",
    L"https://huggingface.co/andraniksargsyan/migan/resolve/main/migan_pipeline_v2.onnx"
}};

// COM IMemoryBufferByteAccess 定义，用于零拷贝访问 WinML 张量缓冲区
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) IMemoryBufferByteAccessLocal : ::IUnknown {
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

struct WinMlByteBufferLock {
    winrt::Windows::Foundation::IMemoryBufferReference ref{nullptr};
    uint8_t* data = nullptr;
    uint32_t byteCount = 0;

    explicit WinMlByteBufferLock(const waml::TensorUInt8Bit& tensor) {
        try {
            auto memBuf = tensor.as<winrt::Windows::Foundation::IMemoryBuffer>();
            if (!memBuf) return;
            ref = memBuf.CreateReference();
            if (!ref) return;
            auto byteAccess = ref.try_as<IMemoryBufferByteAccessLocal>();
            if (!byteAccess) return;
            uint8_t* raw = nullptr;
            uint32_t cap = 0;
            if (SUCCEEDED(byteAccess->GetBuffer(&raw, &cap)) && raw && cap > 0) {
                data = raw;
                byteCount = cap;
            }
        } catch (...) {
            data = nullptr;
            byteCount = 0;
        }
    }

    ~WinMlByteBufferLock() {
        release();
    }

    void release() noexcept {
        if (ref) {
            try { ref.Close(); } catch (...) {}
            ref = nullptr;
        }
        data = nullptr;
        byteCount = 0;
    }
};

void ensureComInitOnThread() noexcept {
    thread_local bool s_inited = false;
    if (!s_inited) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
            s_inited = true;
        }
    }
}

cv::Mat ensureBgr3Mat(const cv::Mat& src) {
    if (src.empty()) return {};
    if (src.channels() == 3) return src;
    cv::Mat out;
    if (src.channels() == 4) {
        cv::cvtColor(src, out, cv::COLOR_BGRA2BGR);
    } else if (src.channels() == 1) {
        cv::cvtColor(src, out, cv::COLOR_GRAY2BGR);
    }
    return out;
}

bool containsCjkCodepoint(const std::wstring& s) noexcept {
    for (wchar_t ch : s) {
        if ((ch >= 0x4E00 && ch <= 0x9FFF) ||
            (ch >= 0x3400 && ch <= 0x4DBF) ||
            (ch >= 0x3000 && ch <= 0x303F) ||
            (ch >= 0xFF00 && ch <= 0xFFEF)) {
            return true;
        }
    }
    return false;
}

bool containsAsciiAscenderOrDigit(const std::wstring& s) noexcept {
    for (wchar_t ch : s) {
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') ||
            ch == L'b' || ch == L'd' || ch == L'f' || ch == L'h' ||
            ch == L'k' || ch == L'l' || ch == L't') {
            return true;
        }
    }
    return false;
}

// 解 3x3 对称线性方程组 A * x = b（高斯消元，稳定可靠）
bool solveLinear3x3(double A[3][3], double b[3], double x[3]) noexcept {
    double M[3][4];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) M[r][c] = A[r][c];
        M[r][3] = b[r];
    }
    for (int col = 0; col < 3; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 3; ++r) {
            if (std::abs(M[r][col]) > std::abs(M[pivot][col])) pivot = r;
        }
        if (std::abs(M[pivot][col]) < 1e-8) return false;
        if (pivot != col) {
            for (int c = col; c < 4; ++c) std::swap(M[col][c], M[pivot][c]);
        }
        const double invP = 1.0 / M[col][col];
        for (int c = col; c < 4; ++c) M[col][c] *= invP;
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            const double f = M[r][col];
            for (int c = col; c < 4; ++c) M[r][c] -= f * M[col][c];
        }
    }
    for (int r = 0; r < 3; ++r) x[r] = M[r][3];
    return true;
}

/// 拟合局部二维线性渐变背景平面，自动剔除边框/相邻文字离群像素
LinearBgPlane fitRobustLinearBgPlane(const cv::Mat& bgr, const cv::Rect& roi) {
    LinearBgPlane plane{};
    plane.originX = static_cast<float>(roi.x) + static_cast<float>(roi.width) * 0.5f;
    plane.originY = static_cast<float>(roi.y) + static_cast<float>(roi.height) * 0.5f;
    if (bgr.empty() || roi.width <= 0 || roi.height <= 0) return plane;

    struct Sample {
        float dx, dy;
        cv::Vec3b px;
    };
    std::vector<Sample> samples;
    samples.reserve(512);

    auto pushPt = [&](int x, int y) {
        if (x < 0 || x >= bgr.cols || y < 0 || y >= bgr.rows) return;
        samples.push_back(Sample{
            static_cast<float>(x) - plane.originX,
            static_cast<float>(y) - plane.originY,
            bgr.at<cv::Vec3b>(y, x)
        });
    };

    // 采样外围 2~7px 环带（避开内部文字）
    const int ringMin = 2;
    const int ringMax = 6;
    const int xStart = (std::max)(0, roi.x - ringMax);
    const int xEnd = (std::min)(bgr.cols - 1, roi.x + roi.width + ringMax);
    const int yStart = (std::max)(0, roi.y - ringMax);
    const int yEnd = (std::min)(bgr.rows - 1, roi.y + roi.height + ringMax);

    const int stepX = (std::max)(1, (xEnd - xStart) / 48);
    const int stepY = (std::max)(1, (yEnd - yStart) / 14);

    for (int x = xStart; x <= xEnd; x += stepX) {
        for (int dy = ringMin; dy <= ringMax; dy += 2) {
            pushPt(x, roi.y - dy);
            pushPt(x, roi.y + roi.height - 1 + dy);
        }
    }
    for (int y = (std::max)(0, roi.y); y < (std::min)(bgr.rows, roi.y + roi.height); y += stepY) {
        for (int dx = ringMin; dx <= ringMax; dx += 2) {
            pushPt(roi.x - dx, y);
            pushPt(roi.x + roi.width - 1 + dx, y);
        }
    }

    // 极端贴边兜底：采样 ROI 四条内缘
    if (samples.size() < 12) {
        for (int x = (std::max)(0, roi.x); x < (std::min)(bgr.cols, roi.x + roi.width); x += (std::max)(1, roi.width / 24)) {
            pushPt(x, (std::max)(0, roi.y));
            pushPt(x, (std::min)(bgr.rows - 1, roi.y + roi.height - 1));
        }
    }
    if (samples.empty()) return plane;

    // 1. 先求三通道中位数，剔除距离中位数过远的异常点（如 1px 表格线或相邻文字）
    std::vector<uint8_t> bv, gv, rv;
    bv.reserve(samples.size());
    gv.reserve(samples.size());
    rv.reserve(samples.size());
    for (const auto& s : samples) {
        bv.push_back(s.px[0]);
        gv.push_back(s.px[1]);
        rv.push_back(s.px[2]);
    }
    const size_t mid = bv.size() / 2;
    std::nth_element(bv.begin(), bv.begin() + static_cast<ptrdiff_t>(mid), bv.end());
    std::nth_element(gv.begin(), gv.begin() + static_cast<ptrdiff_t>(mid), gv.end());
    std::nth_element(rv.begin(), rv.begin() + static_cast<ptrdiff_t>(mid), rv.end());
    const int medB = bv[mid], medG = gv[mid], medR = rv[mid];

    std::vector<int> dists;
    dists.reserve(samples.size());
    for (const auto& s : samples) {
        const int d = (std::max)({
            std::abs(static_cast<int>(s.px[0]) - medB),
            std::abs(static_cast<int>(s.px[1]) - medG),
            std::abs(static_cast<int>(s.px[2]) - medR)
        });
        dists.push_back(d);
    }
    std::vector<int> sortedDists = dists;
    const size_t p75Idx = (sortedDists.size() * 3) / 4;
    std::nth_element(sortedDists.begin(), sortedDists.begin() + static_cast<ptrdiff_t>(p75Idx), sortedDists.end());
    const int inlierThresh = (std::max)(18, (std::min)(48, sortedDists[p75Idx] * 2 + 8));

    std::vector<Sample> inliers;
    inliers.reserve(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        if (dists[i] <= inlierThresh) {
            inliers.push_back(samples[i]);
        }
    }
    if (inliers.size() < 6) {
        inliers = samples;
    }

    // 2. 最小二乘平面拟合（加微弱岭正则防病态共线）
    double A[3][3]{};
    double rhs[3][3]{};
    for (const auto& s : inliers) {
        const double v[3] = {1.0, static_cast<double>(s.dx), static_cast<double>(s.dy)};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) A[r][c] += v[r] * v[c];
            for (int ch = 0; ch < 3; ++ch) rhs[ch][r] += v[r] * static_cast<double>(s.px[ch]);
        }
    }
    A[1][1] += 1e-3;
    A[2][2] += 1e-3;

    for (int ch = 0; ch < 3; ++ch) {
        double ACopy[3][3];
        double bCopy[3];
        double sol[3]{};
        std::memcpy(ACopy, A, sizeof(A));
        std::memcpy(bCopy, rhs[ch], sizeof(bCopy));
        if (solveLinear3x3(ACopy, bCopy, sol)) {
            plane.c0[ch] = std::clamp(static_cast<float>(sol[0]), 0.0f, 255.0f);
            plane.cx[ch] = std::clamp(static_cast<float>(sol[1]), -1.5f, 1.5f);
            plane.cy[ch] = std::clamp(static_cast<float>(sol[2]), -1.5f, 1.5f);
        } else {
            plane.c0[ch] = static_cast<float>(ch == 0 ? medB : (ch == 1 ? medG : medR));
            plane.cx[ch] = 0.0f;
            plane.cy[ch] = 0.0f;
        }
    }

    // 3. 仅当背景内点占绝对主导 (>= 64%) 且内点紧贴二维线性渐变平面 (inlierStd <= 8.5) 时，
    //    锁定为纯色/渐变平面（既彻底根治紧凑多行上下邻行文字拉高方差误走 Telea 留鬼影，
    //    又防止 50/50 棋盘格或复杂底纹照片被误判为纯色平面）
    double inlierSqSum = 0.0;
    for (const auto& s : inliers) {
        const cv::Vec3b pred = plane.evalAt(plane.originX + s.dx, plane.originY + s.dy);
        const double db = static_cast<double>(s.px[0]) - static_cast<double>(pred[0]);
        const double dg = static_cast<double>(s.px[1]) - static_cast<double>(pred[1]);
        const double dr = static_cast<double>(s.px[2]) - static_cast<double>(pred[2]);
        inlierSqSum += (db * db + dg * dg + dr * dr) / 3.0;
    }
    const float inlierStd = static_cast<float>(std::sqrt(inlierSqSum / static_cast<double>((std::max<size_t>)(1, inliers.size()))));
    const float inlierRatio = static_cast<float>(inliers.size()) / static_cast<float>((std::max<size_t>)(1, samples.size()));
    if (inlierRatio >= 0.64f && inlierStd <= 8.5f) {
        plane.complexity = inlierStd;
    } else {
        double allSqSum = 0.0;
        for (const auto& s : samples) {
            const cv::Vec3b pred = plane.evalAt(plane.originX + s.dx, plane.originY + s.dy);
            const double db = static_cast<double>(s.px[0]) - static_cast<double>(pred[0]);
            const double dg = static_cast<double>(s.px[1]) - static_cast<double>(pred[1]);
            const double dr = static_cast<double>(s.px[2]) - static_cast<double>(pred[2]);
            allSqSum += (db * db + dg * dg + dr * dr) / 3.0;
        }
        plane.complexity = (std::max)(inlierStd, static_cast<float>(std::sqrt(allSqSum / static_cast<double>((std::max<size_t>)(1, samples.size())))));
    }
    return plane;
}

uint64_t computeRoiContentHash(const cv::Mat& bgr, const cv::Rect& roi, int bgMode, bool aiInstalled) noexcept {
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) noexcept {
        h ^= v;
        h *= 1099511628211ULL;
    };
    mix(static_cast<uint64_t>(bgr.cols));
    mix(static_cast<uint64_t>(bgr.rows));
    mix(static_cast<uint64_t>(roi.x));
    mix(static_cast<uint64_t>(roi.y));
    mix(static_cast<uint64_t>(roi.width));
    mix(static_cast<uint64_t>(roi.height));
    mix(static_cast<uint64_t>(bgMode));
    mix(aiInstalled ? 1ULL : 0ULL);

    const cv::Rect safe = roi & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (safe.width > 0 && safe.height > 0) {
        const int sx = (std::max)(1, safe.width / 8);
        const int sy = (std::max)(1, safe.height / 4);
        for (int y = safe.y; y < safe.y + safe.height; y += sy) {
            for (int x = safe.x; x < safe.x + safe.width; x += sx) {
                const auto& p = bgr.at<cv::Vec3b>(y, x);
                mix(static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) | (static_cast<uint64_t>(p[2]) << 16));
            }
        }
    }
    return h;
}

struct CachedErasedPatch {
    uint64_t key = 0;
    cv::Rect roi;
    cv::Mat cleanPatchBgr;
};

std::mutex g_patchCacheMutex;
std::deque<CachedErasedPatch> g_patchCache;
constexpr size_t kMaxCachedPatches = 12;

bool lookupErasedPatchCache(uint64_t key, cv::Rect& outRoi, cv::Mat& outPatch) {
    std::lock_guard<std::mutex> lock(g_patchCacheMutex);
    for (const auto& entry : g_patchCache) {
        if (entry.key == key && !entry.cleanPatchBgr.empty()) {
            outRoi = entry.roi;
            outPatch = entry.cleanPatchBgr.clone();
            return true;
        }
    }
    return false;
}

void storeErasedPatchCache(uint64_t key, const cv::Rect& roi, const cv::Mat& patch) {
    if (patch.empty()) return;
    std::lock_guard<std::mutex> lock(g_patchCacheMutex);
    for (auto it = g_patchCache.begin(); it != g_patchCache.end(); ++it) {
        if (it->key == key) {
            g_patchCache.erase(it);
            break;
        }
    }
    g_patchCache.push_front(CachedErasedPatch{key, roi, patch.clone()});
    while (g_patchCache.size() > kMaxCachedPatches) {
        g_patchCache.pop_back();
    }
}

const wchar_t* resolveFontFamilyName(TextReplaceFontClass fc) noexcept {
    switch (fc) {
        case TextReplaceFontClass::Monospace: return L"Cascadia Mono";
        case TextReplaceFontClass::Serif:     return L"SimSun";
        case TextReplaceFontClass::UiSans:
        default:                              return L"Microsoft YaHei UI";
    }
}

const wchar_t* resolveFallbackFontFamilyName(TextReplaceFontClass fc) noexcept {
    switch (fc) {
        case TextReplaceFontClass::Monospace: return L"Consolas";
        case TextReplaceFontClass::Serif:     return L"Georgia";
        case TextReplaceFontClass::UiSans:
        default:                              return L"Segoe UI";
    }
}

DWRITE_FONT_WEIGHT toDWriteWeight(int weight) noexcept {
    if (weight >= 650) return DWRITE_FONT_WEIGHT_BOLD;
    if (weight >= 550) return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    if (weight >= 460) return DWRITE_FONT_WEIGHT_MEDIUM;
    return DWRITE_FONT_WEIGHT_REGULAR;
}

/// 线程局部复用 DirectWrite / Direct2D / WIC 工厂，避免每次改字重复 COM 初始化（提速 5x+）
struct ThreadTextFactories {
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IWICImagingFactory> wic;
    bool initialized = false;

    bool ensure() noexcept {
        if (initialized) return (dwrite && d2d && wic);
        initialized = true;
        ensureComInitOnThread();
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(wic.GetAddressOf()));
        return (dwrite && d2d && wic);
    }
};

ThreadTextFactories& threadFactories() noexcept {
    thread_local ThreadTextFactories s_f;
    s_f.ensure();
    return s_f;
}

/// 构造带感知墨水微调的 DirectWrite 渲染参数（弥补 Microsoft YaHei UI 缺少 500/600 实体字重，且对齐深/浅色屏幕 Gamma）
ComPtr<IDWriteRenderingParams> createPerceptualRenderingParams(IDWriteFactory* dwrite,
                                                               int fontWeight,
                                                               bool darkBackground) {
    ComPtr<IDWriteRenderingParams> params;
    if (!dwrite) return params;
    // 暗底浅字在 Windows/Chromium Skia 下需要更柔和的 Gamma，浅底深字保持标准清锐对比度；
    // 500 Medium / 600 SemiBold 在缺少实体中字重的字体上通过温和提升 enhancedContrast 实现真实视觉加粗阶梯
    float gamma = darkBackground ? 1.55f : 1.80f;
    float enhancedContrast = darkBackground ? 0.35f : 0.50f;
    if (fontWeight >= 550 && fontWeight < 650) {
        enhancedContrast = darkBackground ? 0.85f : 1.00f;
        gamma = darkBackground ? 1.35f : 1.60f;
    } else if (fontWeight >= 460 && fontWeight < 550) {
        enhancedContrast = darkBackground ? 0.60f : 0.75f;
        gamma = darkBackground ? 1.45f : 1.70f;
    }
    dwrite->CreateCustomRenderingParams(
        gamma,
        enhancedContrast,
        0.0f, // Grayscale 亚像素平滑（无彩边干扰）
        DWRITE_PIXEL_GEOMETRY_FLAT,
        DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
        params.GetAddressOf());
    return params;
}

ComPtr<IDWriteTextLayout> buildTextLayout(IDWriteFactory* dwrite,
                                          const std::wstring& text,
                                          TextReplaceFontClass fontClass,
                                          float fontSizePx,
                                          int fontWeight,
                                          float* outBaseline = nullptr,
                                          float* outAdvanceW = nullptr,
                                          float* outLayoutH = nullptr) {
    ComPtr<IDWriteTextLayout> layout;
    if (!dwrite) return layout;

    ComPtr<IDWriteTextFormat> fmt;
    const wchar_t* fam = resolveFontFamilyName(fontClass);
    if (FAILED(dwrite->CreateTextFormat(
            fam, nullptr, toDWriteWeight(fontWeight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSizePx, L"zh-CN", fmt.GetAddressOf())) || !fmt) {
        dwrite->CreateTextFormat(
            resolveFallbackFontFamilyName(fontClass), nullptr, toDWriteWeight(fontWeight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSizePx, L"zh-CN", fmt.GetAddressOf());
    }
    if (!fmt) return layout;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    const std::wstring sample = text.empty() ? L" " : text;
    if (FAILED(dwrite->CreateTextLayout(
            sample.c_str(), static_cast<UINT32>(sample.size()),
            fmt.Get(), 4096.0f, 512.0f, layout.GetAddressOf())) || !layout) {
        return {};
    }

    if (outAdvanceW || outLayoutH) {
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(layout->GetMetrics(&tm))) {
            if (outAdvanceW) *outAdvanceW = text.empty() ? 2.0f : (std::max)(2.0f, tm.widthIncludingTrailingWhitespace);
            if (outLayoutH) *outLayoutH = (std::max)(fontSizePx, tm.height);
        }
    }
    if (outBaseline) {
        DWRITE_LINE_METRICS lm{};
        UINT32 lineCnt = 0;
        if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &lineCnt)) && lineCnt >= 1 && lm.baseline > 0.5f) {
            *outBaseline = lm.baseline;
        } else {
            *outBaseline = fontSizePx * 0.82f;
        }
    }
    return layout;
}

/// 真实光栅化墨水几何探针结果（Render-and-Match 物理闭环核心）
struct RenderedGlyphProbe {
    bool valid = false;
    float inkLeft = 0.0f;
    float inkRight = 0.0f;
    float inkTop = 0.0f;
    float inkBottom = 0.0f;
    float inkWidth = 0.0f;
    float inkHeight = 0.0f;
    float inkCenterY = 0.0f;
    float baseline = 0.0f;
    float advanceW = 0.0f;
    float layoutH = 0.0f;
    float alphaMass = 0.0f; // sum(alpha / 255.0)
};

struct ProbeCacheEntry {
    uint64_t key = 0;
    RenderedGlyphProbe value;
};

std::mutex g_probeCacheMutex;
std::deque<ProbeCacheEntry> g_probeCache;
constexpr size_t kMaxProbeCache = 48;

uint64_t hashProbeKey(const std::wstring& text, TextReplaceFontClass fc, float fontSizePx, int fontWeight, bool darkBg) noexcept {
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) noexcept { h ^= v; h *= 1099511628211ULL; };
    mix(static_cast<uint64_t>(fc));
    mix(static_cast<uint64_t>(std::lround(fontSizePx * 4.0f)));
    mix(static_cast<uint64_t>(fontWeight));
    mix(darkBg ? 1ULL : 0ULL);
    for (wchar_t ch : text) mix(static_cast<uint64_t>(ch));
    return h;
}

RenderedGlyphProbe measureRenderedGlyphInk(const std::wstring& text,
                                           TextReplaceFontClass fontClass,
                                           float fontSizePx,
                                           int fontWeight,
                                           bool darkBackground) {
    RenderedGlyphProbe probe{};
    if (text.empty() || fontSizePx < 6.0f) return probe;

    const uint64_t key = hashProbeKey(text, fontClass, fontSizePx, fontWeight, darkBackground);
    {
        std::lock_guard<std::mutex> lock(g_probeCacheMutex);
        for (const auto& entry : g_probeCache) {
            if (entry.key == key) return entry.value;
        }
    }

    auto& fac = threadFactories();
    if (!fac.dwrite || !fac.d2d || !fac.wic) return probe;

    float base = fontSizePx * 0.82f;
    float advW = fontSizePx;
    float layH = fontSizePx * 1.25f;
    auto layout = buildTextLayout(fac.dwrite.Get(), text, fontClass, fontSizePx, fontWeight, &base, &advW, &layH);
    if (!layout) return probe;

    probe.baseline = base;
    probe.advanceW = advW;
    probe.layoutH = layH;

    const int pad = 4;
    const int bmpW = std::clamp(static_cast<int>(std::ceil(advW)) + pad * 2, 8, 1200);
    const int bmpH = std::clamp(static_cast<int>(std::ceil(layH)) + pad * 2, 8, 256);

    ComPtr<IWICBitmap> bmp;
    if (FAILED(fac.wic->CreateBitmap(static_cast<UINT>(bmpW), static_cast<UINT>(bmpH),
                                     GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                                     bmp.GetAddressOf())) || !bmp) {
        return probe;
    }

    const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(fac.d2d->CreateWicBitmapRenderTarget(bmp.Get(), rtProps, rt.GetAddressOf())) || !rt) {
        return probe;
    }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (auto rp = createPerceptualRenderingParams(fac.dwrite.Get(), fontWeight, darkBackground)) {
        rt->SetTextRenderingParams(rp.Get());
    }

    ComPtr<ID2D1SolidColorBrush> whiteBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteBrush.GetAddressOf());
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (whiteBrush) {
        rt->DrawTextLayout(D2D1::Point2F(static_cast<float>(pad), static_cast<float>(pad)),
                           layout.Get(), whiteBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    if (FAILED(rt->EndDraw())) return probe;

    WICRect rc{0, 0, bmpW, bmpH};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bmp->Lock(&rc, WICBitmapLockRead, lock.GetAddressOf())) || !lock) return probe;

    UINT stride = 0, bufSize = 0;
    BYTE* ptr = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bufSize, &ptr)) || !ptr) return probe;

    int minX = bmpW, maxX = -1, minY = bmpH, maxY = -1;
    double alphaSum = 0.0;
    for (int y = 0; y < bmpH; ++y) {
        const auto* row = reinterpret_cast<const cv::Vec4b*>(ptr + static_cast<size_t>(y) * stride);
        for (int x = 0; x < bmpW; ++x) {
            const uint8_t a = row[x][3];
            if (a > 0) {
                alphaSum += static_cast<double>(a) / 255.0;
            }
            // 与原图 extractStyle 的 42% 阈值保持一致，剔除最外围 1px 淡抗锯齿晕边
            if (a >= 96) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    if (maxX >= minX && maxY >= minY) {
        probe.valid = true;
        probe.inkLeft = static_cast<float>(minX - pad);
        probe.inkRight = static_cast<float>(maxX - pad + 1);
        probe.inkTop = static_cast<float>(minY - pad);
        probe.inkBottom = static_cast<float>(maxY - pad + 1);
        probe.inkWidth = (std::max)(1.0f, probe.inkRight - probe.inkLeft);
        probe.inkHeight = (std::max)(1.0f, probe.inkBottom - probe.inkTop);
        probe.inkCenterY = (probe.inkTop + probe.inkBottom) * 0.5f;
        probe.alphaMass = static_cast<float>(alphaSum);
    }

    {
        std::lock_guard<std::mutex> lockGuard(g_probeCacheMutex);
        g_probeCache.push_front(ProbeCacheEntry{key, probe});
        while (g_probeCache.size() > kMaxProbeCache) g_probeCache.pop_back();
    }
    return probe;
}

/// 统一精确计算文字绘制原点（渲染与编辑叠加 100% 共享，彻底根除向上漂移与左右错位）
struct ResolvedTextPlacement {
    float drawTextX = 0.0f;
    float drawTextY = 0.0f;
    float measuredTextW = 0.0f;
    float measuredTextH = 0.0f;
    float dwriteBaseline = 0.0f;
    ComPtr<IDWriteTextLayout> newLayout;
};

ResolvedTextPlacement resolveTextPlacement(const ExtractedTextStyle& extracted,
                                           const cv::Rect& origRoi,
                                           const std::wstring& wOrig,
                                           const std::wstring& wNew,
                                           TextReplaceFontClass fontClass,
                                           float fontSizePx,
                                           int fontWeight,
                                           float offsetX,
                                           float offsetY) {
    ResolvedTextPlacement res{};
    auto& fac = threadFactories();

    res.measuredTextW = static_cast<float>(origRoi.width);
    res.measuredTextH = static_cast<float>(origRoi.height);
    res.dwriteBaseline = fontSizePx * 0.82f;
    if (fac.dwrite) {
        res.newLayout = buildTextLayout(
            fac.dwrite.Get(), wNew, fontClass, fontSizePx, fontWeight,
            &res.dwriteBaseline, &res.measuredTextW, &res.measuredTextH);
    }

    // 用原文字（若空则用新文字或参考字符）在当前字体+字号+字重下实测真实墨水边界，
    // 实现与原图物理墨水中心/基线及左缘 0.1px 亚像素严丝合缝对齐
    const std::wstring alignSample = !wOrig.empty() ? wOrig : (!wNew.empty() ? wNew : L"中");
    const RenderedGlyphProbe alignProbe = measureRenderedGlyphInk(
        alignSample, fontClass, fontSizePx, fontWeight, extracted.darkBackground);

    const float origInkLeft = (extracted.inkRightX > extracted.inkLeftX)
        ? extracted.inkLeftX
        : static_cast<float>(origRoi.x);
    const float origInkCenterY = (extracted.inkBottomY > extracted.inkTopY)
        ? (extracted.inkTopY + extracted.inkBottomY) * 0.5f
        : (static_cast<float>(origRoi.y) + static_cast<float>(origRoi.height) * 0.5f);

    if (alignProbe.valid) {
        res.drawTextX = (origInkLeft - alignProbe.inkLeft) + offsetX;
        const bool origHasCjk = containsCjkCodepoint(alignSample);
        const bool origHasAsc = containsAsciiAscenderOrDigit(alignSample);
        if (origHasCjk || origHasAsc) {
            // 中文/混排/数字大写：按真实墨水垂直中心精确对齐（彻底根除微软雅黑汉字比拉丁基线低 2.5px 导致的向上飘移！）
            res.drawTextY = (origInkCenterY - alignProbe.inkCenterY) + offsetY;
        } else {
            res.drawTextY = (extracted.baselineY - alignProbe.baseline) + offsetY;
        }
    } else {
        res.drawTextX = origInkLeft + offsetX;
        res.drawTextY = (extracted.baselineY - res.dwriteBaseline) + offsetY;
    }

    return res;
}

bool downloadUrlToFileWithProgress(const std::wstring& url,
                                   const std::filesystem::path& destPath,
                                   std::atomic<bool>& cancelFlag,
                                   std::atomic<uint64_t>& outDownloaded,
                                   std::atomic<uint64_t>& outTotal,
                                   std::atomic<int>& outPercent,
                                   std::string& outError) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        outError = "invalid download URL";
        return false;
    }

    HINTERNET hSession = WinHttpOpen(
        L"Tools3000/1.0 (InpaintModelDownloader)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        outError = "WinHttpOpen failed";
        return false;
    }

    struct ScopedHttp {
        HINTERNET h = nullptr;
        ~ScopedHttp() { if (h) WinHttpCloseHandle(h); }
    };
    ScopedHttp sessGuard{hSession};

    WinHttpSetTimeouts(hSession, 15000, 15000, 30000, 30000);
    HINTERNET hConn = WinHttpConnect(hSession, hostBuf, uc.nPort, 0);
    if (!hConn) {
        outError = "WinHttpConnect failed";
        return false;
    }
    ScopedHttp connGuard{hConn};

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", pathBuf, nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        outError = "WinHttpOpenRequest failed";
        return false;
    }
    ScopedHttp reqGuard{hReq};

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr)) {
        outError = "network request failed";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode < 200 || statusCode >= 300) {
        outError = "HTTP status " + std::to_string(statusCode);
        return false;
    }

    wchar_t clBuf[64]{};
    DWORD clSize = sizeof(clBuf);
    uint64_t totalLen = kInpaintModelExpectedBytes;
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, clBuf, &clSize, WINHTTP_NO_HEADER_INDEX)) {
        const uint64_t parsed = _wcstoui64(clBuf, nullptr, 10);
        if (parsed >= kInpaintModelMinValidBytes) totalLen = parsed;
    }
    outTotal.store(totalLen, std::memory_order_release);

    std::error_code ec;
    std::filesystem::create_directories(destPath.parent_path(), ec);
    const std::filesystem::path partPath = destPath.wstring() + L".part";
    std::ofstream ofs(partPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        outError = "cannot open destination file";
        return false;
    }

    std::vector<char> buf(65536);
    uint64_t received = 0;
    while (true) {
        if (cancelFlag.load(std::memory_order_acquire)) {
            ofs.close();
            std::filesystem::remove(partPath, ec);
            outError = "cancelled";
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) {
            ofs.close();
            std::filesystem::remove(partPath, ec);
            outError = "WinHttpQueryDataAvailable failed";
            return false;
        }
        if (avail == 0) break;
        const DWORD toRead = (std::min<DWORD>)(avail, static_cast<DWORD>(buf.size()));
        DWORD readBytes = 0;
        if (!WinHttpReadData(hReq, buf.data(), toRead, &readBytes)) {
            ofs.close();
            std::filesystem::remove(partPath, ec);
            outError = "WinHttpReadData failed";
            return false;
        }
        if (readBytes == 0) break;
        ofs.write(buf.data(), static_cast<std::streamsize>(readBytes));
        if (!ofs.good()) {
            ofs.close();
            std::filesystem::remove(partPath, ec);
            outError = "disk write failed";
            return false;
        }
        received += readBytes;
        outDownloaded.store(received, std::memory_order_release);
        const int pct = (totalLen > 0)
            ? std::clamp(static_cast<int>((received * 100ULL) / totalLen), 0, 99)
            : 0;
        outPercent.store(pct, std::memory_order_release);
    }

    ofs.flush();
    ofs.close();

    if (received < kInpaintModelMinValidBytes) {
        std::filesystem::remove(partPath, ec);
        outError = "downloaded file too small";
        return false;
    }

    std::filesystem::remove(destPath, ec);
    ec.clear();
    std::filesystem::rename(partPath, destPath, ec);
    if (ec) {
        std::filesystem::copy_file(partPath, destPath, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(partPath, ec);
        if (ec) {
            outError = "atomic move failed";
            return false;
        }
    }
    outPercent.store(100, std::memory_order_release);
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// LinearBgPlane
// ─────────────────────────────────────────────────────────────────────────────
cv::Vec3b LinearBgPlane::evalAt(float x, float y) const noexcept {
    const float dx = x - originX;
    const float dy = y - originY;
    auto evalCh = [&](int ch) noexcept -> uint8_t {
        const float v = c0[ch] + cx[ch] * dx + cy[ch] * dy;
        return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    };
    return cv::Vec3b(evalCh(0), evalCh(1), evalCh(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// AiInpaintEngine (MI-GAN 512 ONNX · WinML DirectML GPU)
// ─────────────────────────────────────────────────────────────────────────────
struct AiInpaintEngine::WinMlInpaintContext {
    waml::LearningModel model{nullptr};
    waml::LearningModelDevice device{nullptr};
    waml::LearningModelSession session{nullptr};
    winrt::hstring imageInName;
    winrt::hstring maskInName;
    winrt::hstring outName;
    bool usingGpu = false;
};

AiInpaintEngine& AiInpaintEngine::instance() {
    static AiInpaintEngine inst;
    return inst;
}

AiInpaintEngine::~AiInpaintEngine() {
    cancelModelDownload();
    if (m_downloadWorker.joinable()) {
        m_downloadWorker.join();
    }
    releaseResources();
}

std::filesystem::path AiInpaintEngine::resolveModelDirectory(bool* outInstalled) const {
    const auto dir = tools3000::core::WinUtils::getAppDataDirectory() / L"models" / L"inpaint";
    if (outInstalled) {
        std::error_code ec;
        const auto modelPath = dir / kInpaintModelFileName;
        const bool ok = std::filesystem::exists(modelPath, ec) &&
                        std::filesystem::file_size(modelPath, ec) >= kInpaintModelMinValidBytes;
        *outInstalled = ok;
    }
    return dir;
}

bool AiInpaintEngine::isModelInstalled() const {
    bool installed = false;
    (void)resolveModelDirectory(&installed);
    return installed;
}

AiInpaintModelStatus AiInpaintEngine::getModelStatus() const {
    AiInpaintModelStatus st;
    bool installed = false;
    const auto dir = resolveModelDirectory(&installed);
    st.installed = installed;
    st.downloading = m_downloading.load(std::memory_order_acquire);
    st.progressPercent = st.installed ? 100 : m_progressPercent.load(std::memory_order_acquire);
    st.downloadedBytes = m_downloadedBytes.load(std::memory_order_acquire);
    st.totalBytes = m_totalBytes.load(std::memory_order_acquire);
    st.modelDir = tools3000::core::WinUtils::wstringToUtf8(dir.wstring());
    st.modelFile = tools3000::core::WinUtils::wstringToUtf8(std::wstring(kInpaintModelFileName));
    st.usingGpu = m_usingGpu.load(std::memory_order_acquire);

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        st.error = m_lastDownloadError;
    }

    if (st.downloading) {
        st.state = "downloading";
    } else if (st.installed) {
        st.state = "installed";
        std::error_code ec;
        const auto fsz = std::filesystem::file_size(dir / kInpaintModelFileName, ec);
        if (!ec && fsz > 0) {
            st.downloadedBytes = fsz;
            st.totalBytes = fsz;
        }
    } else if (!st.error.empty()) {
        st.state = "error";
    } else {
        st.state = "not_installed";
    }
    return st;
}

bool AiInpaintEngine::startModelDownload() {
    if (isModelInstalled()) return true;
    if (m_downloading.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    if (m_downloadWorker.joinable()) {
        m_downloadWorker.join();
    }

    m_cancelDownload.store(false, std::memory_order_release);
    m_progressPercent.store(0, std::memory_order_release);
    m_downloadedBytes.store(0, std::memory_order_release);
    m_totalBytes.store(kInpaintModelExpectedBytes, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_lastDownloadError.clear();
    }

    const auto destPath = resolveModelDirectory(nullptr) / kInpaintModelFileName;
    m_downloadWorker = std::thread([this, destPath]() {
        std::string lastErr;
        bool ok = false;
        for (const wchar_t* url : kInpaintModelUrls) {
            if (m_cancelDownload.load(std::memory_order_acquire)) break;
            lastErr.clear();
            if (downloadUrlToFileWithProgress(url, destPath, m_cancelDownload,
                                              m_downloadedBytes, m_totalBytes, m_progressPercent, lastErr)) {
                ok = true;
                break;
            }
            LOG_WARN("AiInpaintEngine: mirror download failed ({}), trying next mirror", lastErr);
        }

        if (ok) {
            LOG_INFO("AiInpaintEngine: MI-GAN 512 ONNX model installed successfully");
            TextReplaceEngine::clearPatchCache();
        } else if (m_cancelDownload.load(std::memory_order_acquire)) {
            LOG_INFO("AiInpaintEngine: model download cancelled by user");
        } else {
            std::lock_guard<std::mutex> lock(m_downloadMutex);
            m_lastDownloadError = lastErr.empty() ? "download failed" : lastErr;
            LOG_WARN("AiInpaintEngine: model download failed: {}", m_lastDownloadError);
        }
        m_downloading.store(false, std::memory_order_release);
    });
    return true;
}

void AiInpaintEngine::cancelModelDownload() {
    m_cancelDownload.store(true, std::memory_order_release);
}

bool AiInpaintEngine::deleteModel() {
    cancelModelDownload();
    if (m_downloadWorker.joinable()) {
        m_downloadWorker.join();
    }
    releaseResources();
    TextReplaceEngine::clearPatchCache();

    std::error_code ec;
    const auto dir = resolveModelDirectory(nullptr);
    std::filesystem::remove(dir / kInpaintModelFileName, ec);
    std::filesystem::remove(dir / (std::wstring(kInpaintModelFileName) + L".part"), ec);
    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_lastDownloadError.clear();
    }
    m_progressPercent.store(0, std::memory_order_release);
    m_downloadedBytes.store(0, std::memory_order_release);
    LOG_INFO("AiInpaintEngine: MI-GAN model deleted");
    return !isModelInstalled();
}

void AiInpaintEngine::releaseResources() {
    std::lock_guard<std::mutex> lock(m_inferMutex);
    m_winMl.reset();
    m_usingGpu.store(false, std::memory_order_release);
}

bool AiInpaintEngine::ensureSessionLoadedLocked(const std::filesystem::path& modelPath) {
    if (m_winMl && m_winMl->session) return true;
    ensureComInitOnThread();
    try {
        auto ctx = std::make_unique<WinMlInpaintContext>();
        ctx->model = waml::LearningModel::LoadFromFilePath(modelPath.wstring());
        if (!ctx->model) return false;

        for (const auto& feat : ctx->model.InputFeatures()) {
            const std::wstring n(feat.Name().c_str());
            if (n.find(L"mask") != std::wstring::npos || n.find(L"Mask") != std::wstring::npos) {
                ctx->maskInName = feat.Name();
            } else if (ctx->imageInName.empty()) {
                ctx->imageInName = feat.Name();
            }
        }
        for (const auto& feat : ctx->model.OutputFeatures()) {
            if (ctx->outName.empty()) {
                ctx->outName = feat.Name();
            }
        }
        if (ctx->imageInName.empty() || ctx->maskInName.empty() || ctx->outName.empty()) {
            LOG_WARN("AiInpaintEngine: unexpected ONNX tensor signatures");
            return false;
        }

        // 优先 DirectML GPU，不支持时自动无缝回退 CPU
        try {
            ctx->device = waml::LearningModelDevice(waml::LearningModelDeviceKind::DirectXHighPerformance);
            ctx->session = waml::LearningModelSession(ctx->model, ctx->device);
            ctx->usingGpu = true;
        } catch (...) {
            ctx->device = waml::LearningModelDevice(waml::LearningModelDeviceKind::Cpu);
            ctx->session = waml::LearningModelSession(ctx->model, ctx->device);
            ctx->usingGpu = false;
        }

        m_usingGpu.store(ctx->usingGpu, std::memory_order_release);
        m_winMl = std::move(ctx);
        LOG_INFO("AiInpaintEngine: MI-GAN 512 loaded via WinML ({})", m_usingGpu.load() ? "DirectML GPU" : "CPU");
        return true;
    } catch (const winrt::hresult_error& e) {
        LOG_WARN("AiInpaintEngine: WinML load failed: hr=0x{:08X}", static_cast<uint32_t>(e.code()));
        return false;
    } catch (const std::exception& e) {
        LOG_WARN("AiInpaintEngine: load exception: {}", e.what());
        return false;
    }
}

bool AiInpaintEngine::inpaintRegion(const cv::Mat& bgrCanvas,
                                    const cv::Rect& contextRoi,
                                    const cv::Mat& eraseMask255,
                                    cv::Mat& outRepairedRoiBgr) {
    bool installed = false;
    const auto dir = resolveModelDirectory(&installed);
    if (!installed || bgrCanvas.empty() || eraseMask255.empty()) return false;

    const cv::Rect safeRoi = contextRoi & cv::Rect(0, 0, bgrCanvas.cols, bgrCanvas.rows);
    if (safeRoi.width < 16 || safeRoi.height < 16) return false;
    if (eraseMask255.cols != safeRoi.width || eraseMask255.rows != safeRoi.height) return false;

    std::lock_guard<std::mutex> lock(m_inferMutex);
    if (!ensureSessionLoadedLocked(dir / kInpaintModelFileName)) {
        return false;
    }

    try {
        // MI-GAN pipeline v2 接受任意 8px 对齐 (建议封顶 512x512) uint8 RGB [1,3,H,W] 与 mask [1,1,H,W] (255=保留, 0=修复)
        const cv::Mat cropBgr = bgrCanvas(safeRoi);
        const int maxSide = (std::max)(safeRoi.width, safeRoi.height);
        const float scale = (maxSide > 512) ? (512.0f / static_cast<float>(maxSide)) : 1.0f;
        const int targetW = (std::max)(64, ((static_cast<int>(std::round(safeRoi.width * scale)) + 7) / 8) * 8);
        const int targetH = (std::max)(64, ((static_cast<int>(std::round(safeRoi.height * scale)) + 7) / 8) * 8);

        cv::Mat resizedBgr, resizedMask;
        if (targetW != safeRoi.width || targetH != safeRoi.height) {
            cv::resize(cropBgr, resizedBgr, cv::Size(targetW, targetH), 0.0, 0.0, cv::INTER_AREA);
            cv::resize(eraseMask255, resizedMask, cv::Size(targetW, targetH), 0.0, 0.0, cv::INTER_NEAREST);
        } else {
            resizedBgr = cropBgr;
            resizedMask = eraseMask255;
        }

        // 每次推理新建 TensorUInt8Bit（严格遵守 DirectML GPU 不可跨次复写同一 Tensor 铁律）
        auto imgTensor = waml::TensorUInt8Bit::Create(
            winrt::single_threaded_vector<int64_t>({1, 3, targetH, targetW}));
        auto maskTensor = waml::TensorUInt8Bit::Create(
            winrt::single_threaded_vector<int64_t>({1, 1, targetH, targetW}));

        const size_t plane = static_cast<size_t>(targetH) * static_cast<size_t>(targetW);
        {
            WinMlByteBufferLock imgLock(imgTensor);
            WinMlByteBufferLock maskLock(maskTensor);
            if (!imgLock.data || imgLock.byteCount < plane * 3 || !maskLock.data || maskLock.byteCount < plane) {
                return false;
            }
            uint8_t* rCh = imgLock.data;
            uint8_t* gCh = imgLock.data + plane;
            uint8_t* bCh = imgLock.data + plane * 2;
            uint8_t* mCh = maskLock.data;
            for (int y = 0; y < targetH; ++y) {
                const auto* bgrRow = resizedBgr.ptr<cv::Vec3b>(y);
                const auto* mRow = resizedMask.ptr<uint8_t>(y);
                const size_t base = static_cast<size_t>(y) * static_cast<size_t>(targetW);
                for (int x = 0; x < targetW; ++x) {
                    rCh[base + x] = bgrRow[x][2];
                    gCh[base + x] = bgrRow[x][1];
                    bCh[base + x] = bgrRow[x][0];
                    // MI-GAN 协议：255 = 保留原图，0 = 需要 AI 修复填充
                    mCh[base + x] = (mRow[x] >= 128) ? 0u : 255u;
                }
            }
        }

        waml::LearningModelBinding binding(m_winMl->session);
        binding.Bind(m_winMl->imageInName, imgTensor);
        binding.Bind(m_winMl->maskInName, maskTensor);
        auto evalRes = m_winMl->session.Evaluate(binding, L"");
        auto outTensor = evalRes.Outputs().Lookup(m_winMl->outName).as<waml::TensorUInt8Bit>();
        const auto shape = outTensor.Shape();
        if (shape.Size() < 4) return false;
        const int outH = static_cast<int>(shape.GetAt(2));
        const int outW = static_cast<int>(shape.GetAt(3));
        const size_t outPlane = static_cast<size_t>(outH) * static_cast<size_t>(outW);

        cv::Mat outBgr(outH, outW, CV_8UC3);
        {
            WinMlByteBufferLock outLock(outTensor);
            std::vector<uint8_t> hostBuf;
            const uint8_t* srcPtr = nullptr;
            if (outLock.data && outLock.byteCount >= outPlane * 3) {
                // 单次整块 memcpy 规避 D3D12 Readback 标量停顿
                hostBuf.resize(outPlane * 3);
                std::memcpy(hostBuf.data(), outLock.data, outPlane * 3);
                srcPtr = hostBuf.data();
            } else {
                outLock.release();
                hostBuf.resize(outPlane * 3);
                auto vv = outTensor.GetAsVectorView();
                vv.GetMany(0, winrt::array_view<uint8_t>(hostBuf.data(), hostBuf.data() + hostBuf.size()));
                srcPtr = hostBuf.data();
            }
            const uint8_t* rCh = srcPtr;
            const uint8_t* gCh = srcPtr + outPlane;
            const uint8_t* bCh = srcPtr + outPlane * 2;
            for (int y = 0; y < outH; ++y) {
                auto* dstRow = outBgr.ptr<cv::Vec3b>(y);
                const size_t base = static_cast<size_t>(y) * static_cast<size_t>(outW);
                for (int x = 0; x < outW; ++x) {
                    dstRow[x] = cv::Vec3b(bCh[base + x], gCh[base + x], rCh[base + x]);
                }
            }
        }

        if (outW != safeRoi.width || outH != safeRoi.height) {
            cv::resize(outBgr, outRepairedRoiBgr, cv::Size(safeRoi.width, safeRoi.height), 0.0, 0.0, cv::INTER_CUBIC);
        } else {
            outRepairedRoiBgr = std::move(outBgr);
        }
        return !outRepairedRoiBgr.empty();
    } catch (const winrt::hresult_error& e) {
        LOG_WARN("AiInpaintEngine: evaluate failed: hr=0x{:08X}", static_cast<uint32_t>(e.code()));
        return false;
    } catch (const std::exception& e) {
        LOG_WARN("AiInpaintEngine: evaluate exception: {}", e.what());
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TextReplaceEngine — 6 维视觉 DNA 逆向克隆 + 双引擎无痕擦除 + DirectWrite 渲染
// ─────────────────────────────────────────────────────────────────────────────
ExtractedTextStyle TextReplaceEngine::extractStyle(const cv::Mat& canvas,
                                                   const cv::Rect& sourceRect,
                                                   const std::vector<cv::Rect>& charBoxes,
                                                   const std::string& originalTextUtf8) {
    ExtractedTextStyle st{};
    if (canvas.empty() || sourceRect.width <= 0 || sourceRect.height <= 0) return st;

    const cv::Mat bgr = ensureBgr3Mat(canvas);
    const cv::Rect roi = sourceRect & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (roi.width <= 0 || roi.height <= 0) return st;

    // 1. 拟合外围二维线性渐变背景平面并评估复杂度
    st.bgPlane = fitRobustLinearBgPlane(bgr, roi);
    const cv::Vec3b centerBg = st.bgPlane.evalAt(st.bgPlane.originX, st.bgPlane.originY);
    const float bgLum = (0.114f * centerBg[0] + 0.587f * centerBg[1] + 0.299f * centerBg[2]) / 255.0f;
    st.darkBackground = (bgLum < 0.42f);
    st.complexBackground = (st.bgPlane.complexity > 11.5f);

    // 2. 提取前景字形对比度图与主体核心字色（避开边缘半透明抗锯齿，同时防止深底抓到单个极端刺眼过曝白点）
    cv::Mat diffMap(roi.height, roi.width, CV_8UC1, cv::Scalar(0));
    struct FgCandidate {
        int diff;
        cv::Vec3b bgr;
    };
    std::vector<FgCandidate> fgCands;
    fgCands.reserve(static_cast<size_t>(roi.width * roi.height / 3));

    int maxDiff = 0;
    for (int r = 0; r < roi.height; ++r) {
        const auto* row = bgr.ptr<cv::Vec3b>(roi.y + r) + roi.x;
        uint8_t* dRow = diffMap.ptr<uint8_t>(r);
        for (int c = 0; c < roi.width; ++c) {
            const cv::Vec3b bg = st.bgPlane.evalAt(static_cast<float>(roi.x + c), static_cast<float>(roi.y + r));
            const int d = (std::max)({
                std::abs(static_cast<int>(row[c][0]) - static_cast<int>(bg[0])),
                std::abs(static_cast<int>(row[c][1]) - static_cast<int>(bg[1])),
                std::abs(static_cast<int>(row[c][2]) - static_cast<int>(bg[2]))
            });
            dRow[c] = static_cast<uint8_t>((std::min)(255, d));
            if (d > maxDiff) maxDiff = d;
            if (d >= 24) {
                fgCands.push_back({d, row[c]});
            }
        }
    }

    int coreDiffRef = (std::max)(32, maxDiff);
    if (!fgCands.empty()) {
        const int coreCut = (std::max)(24, (maxDiff * (st.darkBackground ? 56 : 60)) / 100);
        std::vector<uint8_t> tb, tg, tr;
        std::vector<int> coreDiffs;
        tb.reserve(fgCands.size());
        tg.reserve(fgCands.size());
        tr.reserve(fgCands.size());
        coreDiffs.reserve(fgCands.size());
        for (const auto& fc : fgCands) {
            if (fc.diff >= coreCut) {
                tb.push_back(fc.bgr[0]);
                tg.push_back(fc.bgr[1]);
                tr.push_back(fc.bgr[2]);
                coreDiffs.push_back(fc.diff);
            }
        }
        if (tb.size() < 4) {
            std::sort(fgCands.begin(), fgCands.end(), [](const FgCandidate& a, const FgCandidate& b) {
                return a.diff > b.diff;
            });
            tb.clear(); tg.clear(); tr.clear(); coreDiffs.clear();
            const size_t topN = (std::max<size_t>)(4, (std::min<size_t>)(fgCands.size(), fgCands.size() / 3 + 1));
            for (size_t i = 0; i < topN; ++i) {
                tb.push_back(fgCands[i].bgr[0]);
                tg.push_back(fgCands[i].bgr[1]);
                tr.push_back(fgCands[i].bgr[2]);
                coreDiffs.push_back(fgCands[i].diff);
            }
        }
        const size_t m = tb.size() / 2;
        std::nth_element(tb.begin(), tb.begin() + static_cast<ptrdiff_t>(m), tb.end());
        std::nth_element(tg.begin(), tg.begin() + static_cast<ptrdiff_t>(m), tg.end());
        std::nth_element(tr.begin(), tr.begin() + static_cast<ptrdiff_t>(m), tr.end());
        std::nth_element(coreDiffs.begin(), coreDiffs.begin() + static_cast<ptrdiff_t>(m), coreDiffs.end());
        st.textBgr = cv::Vec3b(tb[m], tg[m], tr[m]);
        coreDiffRef = (std::max)(28, coreDiffs[m]);
    } else {
        st.textBgr = st.darkBackground ? cv::Vec3b(214, 218, 224) : cv::Vec3b(30, 41, 59);
    }

    // 3. 精准校准字形物理四边 [inkLeftX, inkRightX, inkTopY, inkBottomY] 与原图总墨水质量
    const uint8_t fgThresh = static_cast<uint8_t>(std::clamp(maxDiff * 40 / 100, 22, 92));
    cv::Mat fgMask;
    cv::threshold(diffMap, fgMask, fgThresh, 255, cv::THRESH_BINARY);

    int inkTop = 0;
    int inkBot = roi.height - 1;
    std::vector<int> rowCounts(static_cast<size_t>(roi.height), 0);
    int maxRowCount = 0;
    for (int r = 0; r < roi.height; ++r) {
        const int cnt = cv::countNonZero(fgMask.row(r));
        rowCounts[static_cast<size_t>(r)] = cnt;
        if (cnt > maxRowCount) maxRowCount = cnt;
    }
    if (maxRowCount > 0) {
        const int minRowPixels = (std::max)(1, maxRowCount / 14);
        while (inkTop < roi.height - 1 && rowCounts[static_cast<size_t>(inkTop)] < minRowPixels) ++inkTop;
        while (inkBot > inkTop && rowCounts[static_cast<size_t>(inkBot)] < minRowPixels) --inkBot;
    }

    int inkLeft = 0;
    int inkRight = roi.width - 1;
    std::vector<int> colCounts(static_cast<size_t>(roi.width), 0);
    int maxColCount = 0;
    for (int c = 0; c < roi.width; ++c) {
        const int cnt = cv::countNonZero(fgMask.col(c));
        colCounts[static_cast<size_t>(c)] = cnt;
        if (cnt > maxColCount) maxColCount = cnt;
    }
    if (maxColCount > 0) {
        const int minColPixels = (std::max)(1, maxColCount / 14);
        while (inkLeft < roi.width - 1 && colCounts[static_cast<size_t>(inkLeft)] < minColPixels) ++inkLeft;
        while (inkRight > inkLeft && colCounts[static_cast<size_t>(inkRight)] < minColPixels) --inkRight;
    }

    double actualInkMass = 0.0;
    const float invCoreDiff = 1.0f / static_cast<float>(coreDiffRef);
    for (int r = inkTop; r <= inkBot; ++r) {
        const uint8_t* dRow = diffMap.ptr<uint8_t>(r);
        for (int c = inkLeft; c <= inkRight; ++c) {
            if (dRow[c] >= 16) {
                actualInkMass += static_cast<double>(std::clamp(static_cast<float>(dRow[c]) * invCoreDiff, 0.0f, 1.0f));
            }
        }
    }
    st.inkCoverageSum = static_cast<float>(actualInkMass);

    const std::wstring wOrig = tools3000::core::WinUtils::utf8ToWstring(originalTextUtf8);
    const bool hasCjk = containsCjkCodepoint(wOrig);
    const bool hasAsc = containsAsciiAscenderOrDigit(wOrig);
    int baseRow = inkBot;
    if (!hasCjk && (inkBot - inkTop) >= 8) {
        const int denseThresh = (std::max)(2, maxRowCount / 4);
        int denseBot = inkBot;
        while (denseBot > inkTop + (inkBot - inkTop) / 2 && rowCounts[static_cast<size_t>(denseBot)] < denseThresh) {
            --denseBot;
        }
        baseRow = denseBot;
    }

    st.inkLeftX = static_cast<float>(roi.x + inkLeft);
    st.inkRightX = static_cast<float>(roi.x + inkRight + 1);
    st.inkTopY = static_cast<float>(roi.y + inkTop);
    st.inkBottomY = static_cast<float>(roi.y + inkBot + 1);
    st.baselineY = static_cast<float>(roi.y + baseRow + 1);

    // 4. 先推断字体族（检测 ASCII 字符框宽度一致性 -> 提前锁定 Monospace，供 Render-and-Match 闭环使用）
    if (!charBoxes.empty()) {
        std::vector<float> widths;
        widths.reserve(charBoxes.size());
        float advSum = 0.0f;
        int advCnt = 0;
        for (size_t i = 0; i < charBoxes.size(); ++i) {
            if (charBoxes[i].width > 1) {
                widths.push_back(static_cast<float>(charBoxes[i].width));
            }
            if (i + 1 < charBoxes.size()) {
                const float adv = static_cast<float>(charBoxes[i + 1].x - charBoxes[i].x);
                if (adv > 1.0f && adv < static_cast<float>(roi.height) * 2.5f) {
                    advSum += adv;
                    ++advCnt;
                }
            }
        }
        if (advCnt > 0) st.avgCharAdvancePx = advSum / static_cast<float>(advCnt);
        if (!hasCjk && widths.size() >= 4) {
            const float mean = std::accumulate(widths.begin(), widths.end(), 0.0f) / static_cast<float>(widths.size());
            float varSum = 0.0f;
            for (float w : widths) varSum += (w - mean) * (w - mean);
            const float cvVal = std::sqrt(varSum / static_cast<float>(widths.size())) / (std::max)(1.0f, mean);
            if (cvVal < 0.145f) {
                st.fontClass = TextReplaceFontClass::Monospace;
            }
        }
    }

    // 5. Render-and-Match 物理闭环反解精确字号（宽度 + 高度双约束，精度 ±0.25px，根治高估虚胖 20% 顽疾）
    const float actualInkW = (std::max)(1.0f, st.inkRightX - st.inkLeftX);
    const float actualInkH = (std::max)(6.0f, st.inkBottomY - st.inkTopY);
    const float visualInkH = (std::max)(6.0f, static_cast<float>(baseRow - inkTop + 1));

    float solvedEm = 0.0f;
    if (!wOrig.empty() && maxRowCount >= 3 && maxColCount >= 2) {
        constexpr float kRefEm = 20.0f;
        const RenderedGlyphProbe refProbe = measureRenderedGlyphInk(
            wOrig, st.fontClass, kRefEm, 400, st.darkBackground);
        if (refProbe.valid && refProbe.inkHeight >= 4.0f) {
            const float emFromH = kRefEm * (actualInkH / refProbe.inkHeight);
            const bool hasMultiSpaces = (wOrig.find(L"  ") != std::wstring::npos || wOrig.find(L'\t') != std::wstring::npos);
            if (wOrig.size() >= 2 && !hasMultiSpaces && refProbe.inkWidth >= 8.0f && actualInkW >= 8.0f) {
                const float emFromW = kRefEm * (actualInkW / refProbe.inkWidth);
                const float ratio = emFromW / (std::max)(1.0f, emFromH);
                if (ratio >= 0.75f && ratio <= 1.32f) {
                    // 多字符水平跨度大，抗锯齿 1px 误差占比极小，以宽度约束为主导实现像素级精确吻合
                    const float wWeight = (wOrig.size() >= 3) ? 0.68f : 0.55f;
                    solvedEm = emFromW * wWeight + emFromH * (1.0f - wWeight);
                } else {
                    solvedEm = emFromH * 0.80f + std::clamp(emFromW, emFromH * 0.78f, emFromH * 1.25f) * 0.20f;
                }
            } else {
                solvedEm = emFromH;
            }
        }
    }

    if (solvedEm <= 1.0f) {
        const float heightToEm = hasCjk ? 0.88f : (hasAsc ? 0.73f : 0.55f);
        const float estEm = visualInkH / heightToEm;
        const float rawBoxEm = static_cast<float>(roi.height) * 0.78f;
        solvedEm = (maxRowCount >= 3) ? (estEm * 0.85f + rawBoxEm * 0.15f) : rawBoxEm;
    }
    st.fontSizePx = std::clamp(std::round(solvedEm * 2.0f) * 0.5f, 9.0f, 128.0f);

    // 6. 墨水质量闭环匹配字重（实测 400/500/600/700 渲染总墨水能量，选出视觉粗细与原图最一致的字重）
    bool weightMatched = false;
    if (!wOrig.empty() && actualInkMass >= 12.0) {
        constexpr std::array<int, 4> kWeights = {{400, 500, 600, 700}};
        int bestW = 400;
        double bestErr = 1e18;
        for (int candW : kWeights) {
            const RenderedGlyphProbe wp = measureRenderedGlyphInk(
                wOrig, st.fontClass, st.fontSizePx, candW, st.darkBackground);
            if (!wp.valid || wp.alphaMass <= 1.0f) continue;
            const double err = std::abs(static_cast<double>(wp.alphaMass) - actualInkMass);
            if (err < bestErr) {
                bestErr = err;
                bestW = candW;
                weightMatched = true;
            }
        }
        if (weightMatched) {
            st.fontWeight = bestW;
        }
    }
    if (!weightMatched && cv::countNonZero(fgMask) >= 8) {
        cv::Mat distImg;
        cv::distanceTransform(fgMask, distImg, cv::DIST_L2, 3);
        std::vector<float> radii;
        radii.reserve(256);
        for (int r = 0; r < distImg.rows; ++r) {
            const float* dr = distImg.ptr<float>(r);
            for (int c = 0; c < distImg.cols; ++c) {
                if (dr[c] > 0.45f) radii.push_back(dr[c]);
            }
        }
        if (!radii.empty()) {
            const size_t p85 = (radii.size() * 85) / 100;
            std::nth_element(radii.begin(), radii.begin() + static_cast<ptrdiff_t>(p85), radii.end());
            const float strokeW = radii[p85] * 2.0f;
            const float ratio = strokeW / (std::max)(9.0f, st.fontSizePx);
            if (ratio >= 0.165f) st.fontWeight = 700;
            else if (ratio >= 0.132f) st.fontWeight = 600;
            else if (ratio >= 0.105f) st.fontWeight = 500;
            else st.fontWeight = 400;
        }
    }

    return st;
}

void TextReplaceEngine::clearPatchCache() {
    {
        std::lock_guard<std::mutex> lock(g_patchCacheMutex);
        g_patchCache.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_probeCacheMutex);
        g_probeCache.clear();
    }
}

cv::Rect TextReplaceEngine::renderReplacement(cv::Mat& canvas, const TextReplaceRenderSpec& spec) {
    if (canvas.empty() || spec.sourceRect.width <= 0 || spec.sourceRect.height <= 0 ||
        (canvas.channels() != 3 && canvas.channels() != 4)) {
        return {};
    }

    const cv::Mat bgrCanvas = ensureBgr3Mat(canvas);
    const cv::Rect origRoi = spec.sourceRect & cv::Rect(0, 0, bgrCanvas.cols, bgrCanvas.rows);
    if (origRoi.width <= 0 || origRoi.height <= 0) return {};

    const ExtractedTextStyle extracted = extractStyle(bgrCanvas, origRoi, spec.charBoxes, spec.originalTextUtf8);
    const cv::Vec3b inkBgr = spec.styleInitialized ? spec.textBgr : extracted.textBgr;
    const float fontSizePx = std::clamp(spec.styleInitialized ? spec.fontSizePx : extracted.fontSizePx, 8.0f, 160.0f);
    const int fontWeight = spec.styleInitialized ? spec.fontWeight : extracted.fontWeight;
    const TextReplaceFontClass fontClass = spec.styleInitialized ? spec.fontClass : extracted.fontClass;

    const std::wstring wOrig = tools3000::core::WinUtils::utf8ToWstring(spec.originalTextUtf8);
    const std::wstring wNew = tools3000::core::WinUtils::utf8ToWstring(spec.newTextUtf8);

    // 1. 统一物理墨水对齐测算（与编辑态 GPU 叠加层 100% 共享坐标）
    const ResolvedTextPlacement place = resolveTextPlacement(
        extracted, origRoi, wOrig, wNew, fontClass, fontSizePx, fontWeight, spec.offsetX, spec.offsetY);

    // 2. 计算擦除区域与背景还原（带 0ms 缓存，编辑打字 60FPS 零延迟）
    const bool useAiEngine =
        (spec.bgMode == TextReplaceBgMode::AiInpaint) ||
        (spec.bgMode == TextReplaceBgMode::Auto && extracted.complexBackground);
    const bool aiModelInstalled = AiInpaintEngine::instance().isModelInstalled();

    // 针对纯色/线性渐变背景的紧凑 OCR 框，在严格不改变水平边界与排版基线的前提下，
    // 向上/向下做最多 3px 像素级墨水连通探测（含 1px 孤立顶头点如 立/i/高），避免被改旧字顶部残留 1px 小点
    auto probeRowInkCount = [&](int y, int x0, int x1, const LinearBgPlane& plane, int diffThresh) -> int {
        if (y < 0 || y >= bgrCanvas.rows || x1 <= x0) return 0;
        const int cx0 = (std::max)(0, x0);
        const int cx1 = (std::min)(bgrCanvas.cols, x1);
        const auto* row = bgrCanvas.ptr<cv::Vec3b>(y);
        int cnt = 0;
        for (int x = cx0; x < cx1; ++x) {
            const cv::Vec3b bg = plane.evalAt(static_cast<float>(x), static_cast<float>(y));
            const int d = (std::max)({
                std::abs(static_cast<int>(row[x][0]) - static_cast<int>(bg[0])),
                std::abs(static_cast<int>(row[x][1]) - static_cast<int>(bg[1])),
                std::abs(static_cast<int>(row[x][2]) - static_cast<int>(bg[2]))
            });
            if (d >= diffThresh) ++cnt;
        }
        return cnt;
    };

    auto expandVerticalToSafeInkCorridor = [&](const cv::Rect& seedRect,
                                               const LinearBgPlane& plane,
                                               int maxUp,
                                               int maxDown,
                                               int diffThresh) -> cv::Rect {
        if (seedRect.width <= 0 || seedRect.height <= 0) return seedRect;
        const int x0 = seedRect.x;
        const int x1 = seedRect.x + seedRect.width;

        // 估算本行核心区平均每行前景像素数，用以区分“本行字符顶头点/胶囊顶边”与“上一行/下一行密集正文”
        int coreSum = 0;
        int coreNonZeroRows = 0;
        for (int y = seedRect.y; y < seedRect.y + seedRect.height; ++y) {
            const int c = probeRowInkCount(y, x0, x1, plane, diffThresh);
            if (c > 0) {
                coreSum += c;
                ++coreNonZeroRows;
            }
        }
        const int coreMeanInk = (coreNonZeroRows > 0) ? (coreSum / coreNonZeroRows) : (std::max)(8, seedRect.width / 4);
        const int capInkLimit = (std::max)(20, static_cast<int>(std::lround(coreMeanInk * 0.85)));

        auto scanDir = [&](int startY, int step, int maxDist) -> int {
            if (maxDist <= 0) return 0;
            std::vector<int> counts(static_cast<size_t>(maxDist + 2), 0);
            for (int d = 1; d <= maxDist + 1; ++d) {
                const int yy = startY + step * d;
                if (yy < 0 || yy >= bgrCanvas.rows) {
                    counts[static_cast<size_t>(d)] = 0;
                } else {
                    counts[static_cast<size_t>(d)] = probeRowInkCount(yy, x0, x1, plane, diffThresh);
                }
            }

            // 阶段 A：吸收紧贴 seedRect 边缘连续不断开的前景（如高字头/升部/代码胶囊圆角顶底边）
            int best = 0;
            int minInk = counts[1];
            int minInkDist = 0;
            bool hitZero = false;
            for (int d = 1; d <= maxDist; ++d) {
                const int c = counts[static_cast<size_t>(d)];
                if (c == 0) {
                    hitZero = true;
                    break;
                }
                // 防撞入紧邻上下行正文：若先下降到稀疏谷底后又突然暴涨，停在谷底处
                if (d >= 3 && c > minInk * 2 + 12 && minInk <= coreMeanInk / 3) {
                    best = minInkDist;
                    return best;
                }
                if (c < minInk) {
                    minInk = c;
                    minInkDist = d;
                }
                best = d;
            }

            // 阶段 B：若首次遇空白，继续向前窥探 1~2px 字内点隙（专门捕捉 立/高/音/i/j 悬空顶头小点），
            // 仅当该小点紧接着上方/下方存在纯空白分隔且厚度 <= 4px 时吸纳，绝不吸走相邻行文字！
            if (hitZero) {
                for (int gap = 1; gap <= 2; ++gap) {
                    const int dotStart = best + 1 + gap;
                    if (dotStart > maxDist) break;
                    if (counts[static_cast<size_t>(dotStart)] == 0) continue;

                    int dotEnd = dotStart;
                    int maxDotRowInk = counts[static_cast<size_t>(dotStart)];
                    while (dotEnd + 1 <= maxDist && counts[static_cast<size_t>(dotEnd + 1)] > 0) {
                        ++dotEnd;
                        maxDotRowInk = (std::max)(maxDotRowInk, counts[static_cast<size_t>(dotEnd)]);
                    }
                    const int dotThickness = dotEnd - dotStart + 1;
                    const bool separatedAfter = (dotEnd + 1 <= maxDist + 1 && counts[static_cast<size_t>(dotEnd + 1)] == 0);
                    if (separatedAfter && dotThickness <= 4 && maxDotRowInk <= capInkLimit) {
                        best = dotEnd;
                    }
                    break;
                }
            }
            return best;
        };

        const int growUp = scanDir(seedRect.y, -1, maxUp);
        const int growDown = scanDir(seedRect.y + seedRect.height - 1, +1, maxDown);
        const int newTop = (std::max)(0, seedRect.y - growUp);
        const int newBot = (std::min)(bgrCanvas.rows, seedRect.y + seedRect.height + growDown);
        return cv::Rect(seedRect.x, newTop, seedRect.width, (std::max)(1, newBot - newTop));
    };

    const cv::Rect effectiveOrigRoi = useAiEngine
        ? origRoi
        : expandVerticalToSafeInkCorridor(origRoi, extracted.bgPlane, 3, 3, 22);

    const int padX = useAiEngine ? 2 : 1;
    const int padY = useAiEngine ? 2 : 1;
    const cv::Rect eraseRoi = cv::Rect(
        effectiveOrigRoi.x - padX, effectiveOrigRoi.y - padY,
        effectiveOrigRoi.width + padX * 2, effectiveOrigRoi.height + padY * 2) & cv::Rect(0, 0, bgrCanvas.cols, bgrCanvas.rows);

    const uint64_t cacheKey = computeRoiContentHash(bgrCanvas, eraseRoi, static_cast<int>(spec.bgMode), aiModelInstalled);
    cv::Rect cachedRoi;
    cv::Mat cleanPatchBgr;
    if (!lookupErasedPatchCache(cacheKey, cachedRoi, cleanPatchBgr) || cachedRoi != eraseRoi) {
        cleanPatchBgr = bgrCanvas(eraseRoi).clone();

        // 构建紧致文字墨迹掩码（保护紧贴文字边缘的 1px 表格线或左右紧邻未选中文字！）
        cv::Mat inkMask(eraseRoi.height, eraseRoi.width, CV_8UC1, cv::Scalar(0));
        int maxLocalDiff = 0;
        for (int r = 0; r < eraseRoi.height; ++r) {
            const int gy = eraseRoi.y + r;
            const auto* row = bgrCanvas.ptr<cv::Vec3b>(gy) + eraseRoi.x;
            uint8_t* mRow = inkMask.ptr<uint8_t>(r);
            for (int c = 0; c < eraseRoi.width; ++c) {
                const int gx = eraseRoi.x + c;
                if (gx < effectiveOrigRoi.x || gx >= effectiveOrigRoi.x + effectiveOrigRoi.width ||
                    gy < effectiveOrigRoi.y || gy >= effectiveOrigRoi.y + effectiveOrigRoi.height) {
                    continue;
                }
                const cv::Vec3b bg = extracted.bgPlane.evalAt(static_cast<float>(gx), static_cast<float>(gy));
                const int d = (std::max)({
                    std::abs(static_cast<int>(row[c][0]) - static_cast<int>(bg[0])),
                    std::abs(static_cast<int>(row[c][1]) - static_cast<int>(bg[1])),
                    std::abs(static_cast<int>(row[c][2]) - static_cast<int>(bg[2]))
                });
                if (d > maxLocalDiff) maxLocalDiff = d;
                mRow[c] = static_cast<uint8_t>((std::min)(255, d));
            }
        }

        const uint8_t maskCut = static_cast<uint8_t>(std::clamp(maxLocalDiff * 16 / 100, 14, 42));
        cv::threshold(inkMask, inkMask, maskCut, 255, cv::THRESH_BINARY);

        if (useAiEngine) {
            const cv::Mat dilateKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
            cv::dilate(inkMask, inkMask, dilateKernel);
        } else {
            // 纯色/线性渐变模式：严格锁死在 effectiveOrigRoi 内部全覆旧字墨迹，绝不膨胀越界咬伤左右紧邻标点/文字（如 '+'）
            const cv::Rect innerCore(
                (std::max)(0, effectiveOrigRoi.x - eraseRoi.x),
                (std::max)(0, effectiveOrigRoi.y - eraseRoi.y),
                (std::min)(effectiveOrigRoi.width, eraseRoi.width),
                (std::min)(effectiveOrigRoi.height, eraseRoi.height));
            if (innerCore.width > 0 && innerCore.height > 0) {
                cv::rectangle(inkMask, innerCore, cv::Scalar(255), cv::FILLED);
            }
        }

        cv::Mat softMaskF;
        cv::GaussianBlur(inkMask, softMaskF, cv::Size(3, 3), 0.65);
        softMaskF.convertTo(softMaskF, CV_32FC1, 1.0 / 255.0);

        // 邻字零损伤护栏：对 effectiveOrigRoi 外部 1~2px 过渡边，若该像素本身是高对比度相邻文字/表格线（diff >= 30），强制 alpha = 0 保护！
        for (int r = 0; r < eraseRoi.height; ++r) {
            const int gy = eraseRoi.y + r;
            const auto* origRow = bgrCanvas.ptr<cv::Vec3b>(gy) + eraseRoi.x;
            float* aRow = softMaskF.ptr<float>(r);
            for (int c = 0; c < eraseRoi.width; ++c) {
                const int gx = eraseRoi.x + c;
                const bool insideOrig = (gx >= effectiveOrigRoi.x && gx < effectiveOrigRoi.x + effectiveOrigRoi.width &&
                                         gy >= effectiveOrigRoi.y && gy < effectiveOrigRoi.y + effectiveOrigRoi.height);
                if (!insideOrig) {
                    const cv::Vec3b bg = extracted.bgPlane.evalAt(static_cast<float>(gx), static_cast<float>(gy));
                    const int d = (std::max)({
                        std::abs(static_cast<int>(origRow[c][0]) - static_cast<int>(bg[0])),
                        std::abs(static_cast<int>(origRow[c][1]) - static_cast<int>(bg[1])),
                        std::abs(static_cast<int>(origRow[c][2]) - static_cast<int>(bg[2]))
                    });
                    if (d >= 30) {
                        aRow[c] = 0.0f;
                    }
                } else if (!useAiEngine) {
                    // 纯色/平滑渐变核心字区内 100% 彻底擦净，杜绝残留淡灰轮廓
                    const bool onInnerBorder = (gx == effectiveOrigRoi.x || gx == effectiveOrigRoi.x + effectiveOrigRoi.width - 1 ||
                                                gy == effectiveOrigRoi.y || gy == effectiveOrigRoi.y + effectiveOrigRoi.height - 1);
                    if (!onInnerBorder) {
                        aRow[c] = 1.0f;
                    }
                }
            }
        }

        bool aiSucceeded = false;
        if (useAiEngine) {
            const int ctxMargin = (std::max)(48, (std::max)(eraseRoi.width, eraseRoi.height));
            const cv::Rect ctxRoi = cv::Rect(
                eraseRoi.x - ctxMargin, eraseRoi.y - ctxMargin,
                eraseRoi.width + ctxMargin * 2, eraseRoi.height + ctxMargin * 2) &
                cv::Rect(0, 0, bgrCanvas.cols, bgrCanvas.rows);

            cv::Mat ctxMask(ctxRoi.height, ctxRoi.width, CV_8UC1, cv::Scalar(0));
            const cv::Rect relErase(eraseRoi.x - ctxRoi.x, eraseRoi.y - ctxRoi.y, eraseRoi.width, eraseRoi.height);
            inkMask.copyTo(ctxMask(relErase));

            cv::Mat repairedCtx;
            if (aiModelInstalled && AiInpaintEngine::instance().inpaintRegion(bgrCanvas, ctxRoi, ctxMask, repairedCtx)) {
                repairedCtx(relErase).copyTo(cleanPatchBgr);
                aiSucceeded = true;
            } else {
                cv::Mat teleaCtx;
                cv::inpaint(bgrCanvas(ctxRoi), ctxMask, teleaCtx, 3.0, cv::INPAINT_TELEA);
                teleaCtx(relErase).copyTo(cleanPatchBgr);
                aiSucceeded = true;
            }
        }

        if (!aiSucceeded) {
            for (int r = 0; r < eraseRoi.height; ++r) {
                auto* dstRow = cleanPatchBgr.ptr<cv::Vec3b>(r);
                const auto* origRow = bgrCanvas.ptr<cv::Vec3b>(eraseRoi.y + r) + eraseRoi.x;
                const float* aRow = softMaskF.ptr<float>(r);
                for (int c = 0; c < eraseRoi.width; ++c) {
                    const float a = std::clamp(aRow[c], 0.0f, 1.0f);
                    if (a <= 0.001f) continue;
                    const cv::Vec3b bg = extracted.bgPlane.evalAt(
                        static_cast<float>(eraseRoi.x + c), static_cast<float>(eraseRoi.y + r));
                    dstRow[c][0] = static_cast<uint8_t>(std::lround(bg[0] * a + origRow[c][0] * (1.0f - a)));
                    dstRow[c][1] = static_cast<uint8_t>(std::lround(bg[1] * a + origRow[c][1] * (1.0f - a)));
                    dstRow[c][2] = static_cast<uint8_t>(std::lround(bg[2] * a + origRow[c][2] * (1.0f - a)));
                }
            }
        } else {
            for (int r = 0; r < eraseRoi.height; ++r) {
                auto* dstRow = cleanPatchBgr.ptr<cv::Vec3b>(r);
                const auto* origRow = bgrCanvas.ptr<cv::Vec3b>(eraseRoi.y + r) + eraseRoi.x;
                const float* aRow = softMaskF.ptr<float>(r);
                for (int c = 0; c < eraseRoi.width; ++c) {
                    const float a = std::clamp(aRow[c], 0.0f, 1.0f);
                    dstRow[c][0] = static_cast<uint8_t>(std::lround(dstRow[c][0] * a + origRow[c][0] * (1.0f - a)));
                    dstRow[c][1] = static_cast<uint8_t>(std::lround(dstRow[c][1] * a + origRow[c][1] * (1.0f - a)));
                    dstRow[c][2] = static_cast<uint8_t>(std::lround(dstRow[c][2] * a + origRow[c][2] * (1.0f - a)));
                }
            }
        }

        storeErasedPatchCache(cacheKey, eraseRoi, cleanPatchBgr);
    }

    auto writePatchToCanvas = [&](const cv::Mat& patchBgr, const cv::Rect& dstRect) {
        if (canvas.channels() == 3) {
            patchBgr.copyTo(canvas(dstRect));
        } else {
            for (int r = 0; r < dstRect.height; ++r) {
                const auto* srcRow = patchBgr.ptr<cv::Vec3b>(r);
                auto* dstRow = canvas.ptr<cv::Vec4b>(dstRect.y + r) + dstRect.x;
                for (int c = 0; c < dstRect.width; ++c) {
                    dstRow[c][0] = srcRow[c][0];
                    dstRow[c][1] = srcRow[c][1];
                    dstRow[c][2] = srcRow[c][2];
                }
            }
        }
    };

    // 将无痕擦除后的纯净底图写回 canvas
    writePatchToCanvas(cleanPatchBgr, eraseRoi);

    // 2B. 同行右侧原图文字无缝顺延让位（Same-Line Auto-Reflow 四向像素级闭环）：
    //     1) 独立拟合右侧尾巴自身背景平面（防止原词位于代码胶囊内时误把胶囊底色铺到普通页面底上）；
    //     2) 先左右水平精修（左端严守不越旧字右缘并容忍 1~2px 偏旁/符号字内缝；右端吸收连续笔画并专项捕捉 3~5px 全角留白后的孤立行尾标点如 。/：/））；
    //     3) 再基于完整水平跨度执行顶/底双对称「墨水安全走廊扩张」（囊括 1~6px 顶头点/下伸部/右端新纳入高括号与胶囊圆角边，遇行间纯空白刹车）；
    //     4) 清平完整区域并将扩展带按前景像素精准透写至平移后目标区，四向 100% 零削切、原位 0 残点！
    if (spec.reflowRightTail && !useAiEngine &&
        spec.rightTailRect.width > 0 && spec.rightTailRect.height > 0 &&
        std::abs(spec.offsetY) <= static_cast<float>(origRoi.height) * 0.65f) {
        const cv::Rect rawTail = spec.rightTailRect & cv::Rect(0, 0, bgrCanvas.cols, bgrCanvas.rows);
        if (rawTail.width > 0 && rawTail.height > 0 && rawTail.x >= origRoi.x + origRoi.width - 2) {
            const LinearBgPlane fittedTailBg = fitRobustLinearBgPlane(bgrCanvas, rawTail);
            const LinearBgPlane& tailBgPlane = (fittedTailBg.complexity <= 11.5f) ? fittedTailBg : extracted.bgPlane;

            auto probeColInkCount = [&](int x, int y0, int y1, int diffThresh) -> int {
                if (x < 0 || x >= bgrCanvas.cols || y1 <= y0) return 0;
                const int cy0 = (std::max)(0, y0);
                const int cy1 = (std::min)(bgrCanvas.rows, y1);
                int cnt = 0;
                for (int y = cy0; y < cy1; ++y) {
                    const cv::Vec3b px = bgrCanvas.at<cv::Vec3b>(y, x);
                    const cv::Vec3b bg = tailBgPlane.evalAt(static_cast<float>(x), static_cast<float>(y));
                    const int d = (std::max)({
                        std::abs(static_cast<int>(px[0]) - static_cast<int>(bg[0])),
                        std::abs(static_cast<int>(px[1]) - static_cast<int>(bg[1])),
                        std::abs(static_cast<int>(px[2]) - static_cast<int>(bg[2]))
                    });
                    if (d >= diffThresh) ++cnt;
                }
                return cnt;
            };

            // 步骤 1：先做左右水平精修（使用略微放宽 2px 的安全竖向探针窗，确保不错过高括号/高标点）
            const int probeY0 = (std::max)(0, rawTail.y - 2);
            const int probeY1 = (std::min)(bgrCanvas.rows, rawTail.y + rawTail.height + 2);

            // 1a) 向左微扩（严格不越过 effectiveOrigRoi 右缘，容忍 1~2px 左偏旁/左引号字内窄缝，遇 3px 空白刹车）
            const int minAllowedLeft = (std::min)(rawTail.x, effectiveOrigRoi.x + effectiveOrigRoi.width);
            int bestLeft = rawTail.x;
            int leftEmptyRun = 0;
            for (int x = rawTail.x - 1; x >= minAllowedLeft; --x) {
                if (probeColInkCount(x, probeY0, probeY1, 18) > 0) {
                    bestLeft = x;
                    leftEmptyRun = 0;
                } else {
                    ++leftEmptyRun;
                    if (leftEmptyRun >= 3) break;
                }
            }

            // 1b) 向右精修：阶段 A 吸收连续列（容忍 1~2px 字缝）；
            //     阶段 B 若紧接 3~5px 全角留白后存在紧凑孤立标点（如 。/：/）且其右侧 >= 3px 纯空白），一并完整囊括
            const int maxRightGrow = std::clamp(rawTail.height + 6, 12, 26);
            const int rightBase = rawTail.x + rawTail.width;
            std::vector<int> rightCols(static_cast<size_t>(maxRightGrow + 4), 0);
            for (int step = 0; step < maxRightGrow + 4; ++step) {
                if (rightBase + step < bgrCanvas.cols) {
                    rightCols[static_cast<size_t>(step)] = probeColInkCount(rightBase + step, probeY0, probeY1, 18);
                }
            }
            int bestRightOffset = 0; // 相对 rightBase 的偏移长度
            int rightEmptyRun = 0;
            for (int step = 0; step < maxRightGrow; ++step) {
                if (rightCols[static_cast<size_t>(step)] > 0) {
                    bestRightOffset = step + 1;
                    rightEmptyRun = 0;
                } else {
                    ++rightEmptyRun;
                    if (rightEmptyRun >= 3) break;
                }
            }
            // 阶段 B：探测 3~5px 全角留白之后的孤立行尾标点（宽度上限 <= 行高，且其后必须有 >= 3px 纯空白防跨栏）
            const int maxPunctWidth = std::clamp(rawTail.height, 8, 18);
            for (int gap = 3; gap <= 5; ++gap) {
                const int pStart = bestRightOffset + gap;
                if (pStart >= maxRightGrow) break;
                if (rightCols[static_cast<size_t>(pStart)] == 0) continue;

                int pEnd = pStart;
                int innerGap = 0;
                while (pEnd + 1 < maxRightGrow) {
                    if (rightCols[static_cast<size_t>(pEnd + 1)] > 0) {
                        ++pEnd;
                        innerGap = 0;
                    } else if (innerGap == 0 && pEnd + 2 < maxRightGrow && rightCols[static_cast<size_t>(pEnd + 2)] > 0) {
                        // 允许标点内部 1px 空隙（如全角句号中空/双引号）
                        ++innerGap;
                        pEnd += 2;
                    } else {
                        break;
                    }
                }
                const int pWidth = pEnd - pStart + 1;
                const bool cleanAfter = (pEnd + 3 < static_cast<int>(rightCols.size())) &&
                                        (rightCols[static_cast<size_t>(pEnd + 1)] == 0) &&
                                        (rightCols[static_cast<size_t>(pEnd + 2)] == 0) &&
                                        (rightCols[static_cast<size_t>(pEnd + 3)] == 0);
                if (cleanAfter && pWidth <= maxPunctWidth) {
                    bestRightOffset = pEnd + 1;
                }
                break;
            }
            const int bestRight = (std::min)(bgrCanvas.cols, rightBase + bestRightOffset);

            // 步骤 2：基于完整左右水平跨度 [bestLeft, bestRight) 执行顶/底双对称安全走廊扩张，
            //         确保新纳入的左偏旁/右括号/全角标点/代码胶囊顶底同样 100% 完整无削切！
            const cv::Rect horizExpandedTail(bestLeft, rawTail.y, (std::max)(1, bestRight - bestLeft), rawTail.height);
            const int maxUp = std::clamp(rawTail.height / 2 + 1, 5, 11);
            const int maxDown = std::clamp(rawTail.height / 2 + 1, 5, 10);
            const cv::Rect safeTail = expandVerticalToSafeInkCorridor(horizExpandedTail, tailBgPlane, maxUp, maxDown, 16) &
                                      cv::Rect(0, 0, bgrCanvas.cols, bgrCanvas.rows);

            const float newRightEdge = place.drawTextX + place.measuredTextW + 3.0f;
            const int needShift = static_cast<int>(std::ceil(newRightEdge - static_cast<float>(safeTail.x)));
            const int maxShift = (std::max)(0, bgrCanvas.cols - (safeTail.x + safeTail.width) - 2);
            const int shiftX = std::clamp(needShift, 0, maxShift);
            if (shiftX > 1 && safeTail.width > 0 && safeTail.height > 0) {
                const cv::Mat tailPixels = bgrCanvas(safeTail).clone();
                // 用右侧尾巴独立拟合的背景平面彻底清平完整四向扩张区域（顶/底/左/右 0 残点、0 胶囊底色块！）
                cv::Mat clearTail(safeTail.height, safeTail.width, CV_8UC3);
                for (int r = 0; r < safeTail.height; ++r) {
                    auto* dstRow = clearTail.ptr<cv::Vec3b>(r);
                    for (int c = 0; c < safeTail.width; ++c) {
                        dstRow[c] = tailBgPlane.evalAt(
                            static_cast<float>(safeTail.x + c), static_cast<float>(safeTail.y + r));
                    }
                }
                writePatchToCanvas(clearTail, safeTail);
                // 将 100% 四向完整的右侧文字像素块无缝平移至目标位置：
                // 核心正文带全像素直写，上下扩展带按前景像素精准透写（完整保留顶头点/下伸部/胶囊边且绝不误擦相邻行）
                const cv::Rect movedTail(safeTail.x + shiftX, safeTail.y, safeTail.width, safeTail.height);
                for (int r = 0; r < movedTail.height; ++r) {
                    const int gy = movedTail.y + r;
                    const bool inCoreBand = (gy >= rawTail.y && gy < rawTail.y + rawTail.height);
                    const auto* srcRow = tailPixels.ptr<cv::Vec3b>(r);
                    for (int c = 0; c < movedTail.width; ++c) {
                        const int gx = movedTail.x + c;
                        if (!inCoreBand) {
                            const cv::Vec3b bg = tailBgPlane.evalAt(
                                static_cast<float>(safeTail.x + c), static_cast<float>(gy));
                            const int d = (std::max)({
                                std::abs(static_cast<int>(srcRow[c][0]) - static_cast<int>(bg[0])),
                                std::abs(static_cast<int>(srcRow[c][1]) - static_cast<int>(bg[1])),
                                std::abs(static_cast<int>(srcRow[c][2]) - static_cast<int>(bg[2]))
                            });
                            if (d < 12) continue;
                        }
                        if (canvas.channels() == 3) {
                            canvas.at<cv::Vec3b>(gy, gx) = srcRow[c];
                        } else {
                            auto& dst = canvas.at<cv::Vec4b>(gy, gx);
                            dst[0] = srcRow[c][0];
                            dst[1] = srcRow[c][1];
                            dst[2] = srcRow[c][2];
                        }
                    }
                }
            }
        }
    }

    // 3. 纯净绘制新文字（100% 无光标/焦框污染；renderRoi 紧贴新文字本体，供双击二次编辑精确命中）
    const float textWForBox = (std::max)(place.measuredTextW, fontSizePx * 0.6f);
    const int boxX1 = (std::max)(0, static_cast<int>(std::floor(place.drawTextX)) - 3);
    const int boxY1 = (std::max)(0, static_cast<int>(std::floor(place.drawTextY)) - 3);
    const int boxX2 = (std::min)(canvas.cols, static_cast<int>(std::ceil(place.drawTextX + textWForBox)) + 4);
    const int boxY2 = (std::min)(canvas.rows, static_cast<int>(std::ceil(place.drawTextY + place.measuredTextH)) + 4);
    const cv::Rect renderRoi(boxX1, boxY1, (std::max)(1, boxX2 - boxX1), (std::max)(1, boxY2 - boxY1));

    auto& fac = threadFactories();
    if (fac.d2d && fac.wic && fac.dwrite && place.newLayout && !wNew.empty() &&
        renderRoi.width > 0 && renderRoi.height > 0) {
        ComPtr<IWICBitmap> wicBmp;
        if (SUCCEEDED(fac.wic->CreateBitmap(static_cast<UINT>(renderRoi.width), static_cast<UINT>(renderRoi.height),
                                            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                                            wicBmp.GetAddressOf())) && wicBmp) {
            const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            ComPtr<ID2D1RenderTarget> rt;
            if (SUCCEEDED(fac.d2d->CreateWicBitmapRenderTarget(wicBmp.Get(), rtProps, rt.GetAddressOf())) && rt) {
                rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                if (auto rp = createPerceptualRenderingParams(fac.dwrite.Get(), fontWeight, extracted.darkBackground)) {
                    rt->SetTextRenderingParams(rp.Get());
                }

                const float localTextX = place.drawTextX - static_cast<float>(renderRoi.x);
                const float localTextY = place.drawTextY - static_cast<float>(renderRoi.y);

                rt->BeginDraw();
                rt->Clear(D2D1::ColorF(0, 0, 0, 0));

                ComPtr<ID2D1SolidColorBrush> textBrush;
                rt->CreateSolidColorBrush(
                    D2D1::ColorF(inkBgr[2] / 255.0f, inkBgr[1] / 255.0f, inkBgr[0] / 255.0f, 1.0f),
                    textBrush.GetAddressOf());
                if (textBrush) {
                    rt->DrawTextLayout(D2D1::Point2F(localTextX, localTextY), place.newLayout.Get(),
                                       textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                }

                if (SUCCEEDED(rt->EndDraw())) {
                    WICRect wicRc{0, 0, renderRoi.width, renderRoi.height};
                    ComPtr<IWICBitmapLock> bmpLock;
                    if (SUCCEEDED(wicBmp->Lock(&wicRc, WICBitmapLockRead, bmpLock.GetAddressOf())) && bmpLock) {
                        UINT stride = 0, bufSize = 0;
                        BYTE* srcBytes = nullptr;
                        if (SUCCEEDED(bmpLock->GetStride(&stride)) &&
                            SUCCEEDED(bmpLock->GetDataPointer(&bufSize, &srcBytes)) && srcBytes) {
                            const int ch = canvas.channels();
                            for (int r = 0; r < renderRoi.height; ++r) {
                                const auto* sRow = reinterpret_cast<const cv::Vec4b*>(srcBytes + static_cast<size_t>(r) * stride);
                                if (ch == 3) {
                                    auto* dRow = canvas.ptr<cv::Vec3b>(renderRoi.y + r) + renderRoi.x;
                                    for (int c = 0; c < renderRoi.width; ++c) {
                                        const uint32_t a = sRow[c][3];
                                        if (a == 0) continue;
                                        const uint32_t inv = 255u - a;
                                        dRow[c][0] = static_cast<uint8_t>((std::min)(255u, sRow[c][0] + (dRow[c][0] * inv + 127u) / 255u));
                                        dRow[c][1] = static_cast<uint8_t>((std::min)(255u, sRow[c][1] + (dRow[c][1] * inv + 127u) / 255u));
                                        dRow[c][2] = static_cast<uint8_t>((std::min)(255u, sRow[c][2] + (dRow[c][2] * inv + 127u) / 255u));
                                    }
                                } else {
                                    auto* dRow = canvas.ptr<cv::Vec4b>(renderRoi.y + r) + renderRoi.x;
                                    for (int c = 0; c < renderRoi.width; ++c) {
                                        const uint32_t a = sRow[c][3];
                                        if (a == 0) continue;
                                        const uint32_t inv = 255u - a;
                                        dRow[c][0] = static_cast<uint8_t>((std::min)(255u, sRow[c][0] + (dRow[c][0] * inv + 127u) / 255u));
                                        dRow[c][1] = static_cast<uint8_t>((std::min)(255u, sRow[c][1] + (dRow[c][1] * inv + 127u) / 255u));
                                        dRow[c][2] = static_cast<uint8_t>((std::min)(255u, sRow[c][2] + (dRow[c][2] * inv + 127u) / 255u));
                                        dRow[c][3] = static_cast<uint8_t>((std::min)(255u, a + (dRow[c][3] * inv + 127u) / 255u));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return renderRoi;
}

TextReplaceEditOverlayMetrics TextReplaceEngine::computeEditOverlayMetrics(const cv::Mat& cleanCanvas,
                                                                           const TextReplaceRenderSpec& spec) {
    TextReplaceEditOverlayMetrics out{};
    if (cleanCanvas.empty() || spec.sourceRect.width <= 0 || spec.sourceRect.height <= 0) return out;

    const cv::Mat bgrCanvas = ensureBgr3Mat(cleanCanvas);
    const cv::Rect origRoi = spec.sourceRect & cv::Rect(0, 0, bgrCanvas.cols, bgrCanvas.rows);
    if (origRoi.width <= 0 || origRoi.height <= 0) return out;

    const ExtractedTextStyle extracted = extractStyle(bgrCanvas, origRoi, spec.charBoxes, spec.originalTextUtf8);
    const float fontSizePx = std::clamp(spec.styleInitialized ? spec.fontSizePx : extracted.fontSizePx, 8.0f, 160.0f);
    const int fontWeight = spec.styleInitialized ? spec.fontWeight : extracted.fontWeight;
    const TextReplaceFontClass fontClass = spec.styleInitialized ? spec.fontClass : extracted.fontClass;

    const std::wstring wOrig = tools3000::core::WinUtils::utf8ToWstring(spec.originalTextUtf8);
    const std::wstring wNew = tools3000::core::WinUtils::utf8ToWstring(spec.newTextUtf8);
    const ResolvedTextPlacement place = resolveTextPlacement(
        extracted, origRoi, wOrig, wNew, fontClass, fontSizePx, fontWeight, spec.offsetX, spec.offsetY);

    out.valid = true;
    out.darkBackground = extracted.darkBackground;
    out.drawTextX = place.drawTextX;
    out.drawTextY = place.drawTextY;

    // 1:1 刚体紧贴当前新文字实际位置与尺寸（彻底解除与旧擦除框 origRoi 的绑死拉伸！拖到哪框就跟到哪）
    const std::wstring boxSample = !wNew.empty() ? wNew : (!wOrig.empty() ? wOrig : L"中");
    const RenderedGlyphProbe boxProbe = measureRenderedGlyphInk(
        boxSample, fontClass, fontSizePx, fontWeight, extracted.darkBackground);

    const float visualTop = boxProbe.valid
        ? (place.drawTextY + boxProbe.inkTop)
        : (place.drawTextY + fontSizePx * 0.14f);
    const float visualBot = boxProbe.valid
        ? (place.drawTextY + boxProbe.inkBottom)
        : (place.drawTextY + fontSizePx * 1.06f);
    const float visualW = (std::max)(place.measuredTextW, fontSizePx * 0.55f);

    out.boxLeft = (std::max)(0.0f, place.drawTextX - 3.5f);
    out.boxTop = (std::max)(0.0f, visualTop - 3.0f);
    out.boxRight = (std::min)(static_cast<float>(bgrCanvas.cols), place.drawTextX + visualW + 4.5f);
    out.boxBottom = (std::min)(static_cast<float>(bgrCanvas.rows), visualBot + 3.0f);

    out.caretX = place.drawTextX;
    out.caretTop = visualTop - 1.5f;
    out.caretBottom = visualBot + 1.5f;

    if (place.newLayout) {
        if (!wNew.empty()) {
            const UINT32 cIdx = static_cast<UINT32>((std::min)(spec.caretUtf16, wNew.size()));
            FLOAT cx = 0.0f, cy = 0.0f;
            DWRITE_HIT_TEST_METRICS cMetrics{};
            if (SUCCEEDED(place.newLayout->HitTestTextPosition(cIdx, FALSE, &cx, &cy, &cMetrics))) {
                out.caretX = place.drawTextX + cx;
            }

            if (spec.caretUtf16 != spec.selAnchorUtf16) {
                const size_t s0 = (std::min)(spec.caretUtf16, spec.selAnchorUtf16);
                const size_t s1 = (std::min)(wNew.size(), (std::max)(spec.caretUtf16, spec.selAnchorUtf16));
                if (s1 > s0) {
                    UINT32 hitCount = 0;
                    place.newLayout->HitTestTextRange(static_cast<UINT32>(s0), static_cast<UINT32>(s1 - s0),
                                                      place.drawTextX, place.drawTextY, nullptr, 0, &hitCount);
                    if (hitCount > 0) {
                        std::vector<DWRITE_HIT_TEST_METRICS> hits(hitCount);
                        if (SUCCEEDED(place.newLayout->HitTestTextRange(
                                static_cast<UINT32>(s0), static_cast<UINT32>(s1 - s0),
                                place.drawTextX, place.drawTextY, hits.data(), hitCount, &hitCount))) {
                            out.selectionRects.reserve(hitCount);
                            for (UINT32 i = 0; i < hitCount; ++i) {
                                const auto& hm = hits[i];
                                out.selectionRects.emplace_back(
                                    hm.left, out.caretTop, hm.width, (std::max)(4.0f, out.caretBottom - out.caretTop));
                            }
                        }
                    }
                }
            }
        }
    }
    return out;
}

size_t TextReplaceEngine::hitTestCaretIndex(const cv::Mat& cleanCanvas,
                                            const TextReplaceRenderSpec& spec,
                                            float imgX,
                                            float imgY) {
    if (cleanCanvas.empty() || spec.sourceRect.width <= 0 || spec.sourceRect.height <= 0) return 0;
    const std::wstring wNew = tools3000::core::WinUtils::utf8ToWstring(spec.newTextUtf8);
    if (wNew.empty()) return 0;

    const cv::Mat bgrCanvas = ensureBgr3Mat(cleanCanvas);
    const cv::Rect origRoi = spec.sourceRect & cv::Rect(0, 0, bgrCanvas.cols, bgrCanvas.rows);
    if (origRoi.width <= 0 || origRoi.height <= 0) return 0;

    const ExtractedTextStyle extracted = extractStyle(bgrCanvas, origRoi, spec.charBoxes, spec.originalTextUtf8);
    const float fontSizePx = std::clamp(spec.styleInitialized ? spec.fontSizePx : extracted.fontSizePx, 8.0f, 160.0f);
    const int fontWeight = spec.styleInitialized ? spec.fontWeight : extracted.fontWeight;
    const TextReplaceFontClass fontClass = spec.styleInitialized ? spec.fontClass : extracted.fontClass;

    const std::wstring wOrig = tools3000::core::WinUtils::utf8ToWstring(spec.originalTextUtf8);
    const ResolvedTextPlacement place = resolveTextPlacement(
        extracted, origRoi, wOrig, wNew, fontClass, fontSizePx, fontWeight, spec.offsetX, spec.offsetY);
    if (!place.newLayout) return wNew.size();

    BOOL isTrailing = FALSE;
    BOOL isInside = FALSE;
    DWRITE_HIT_TEST_METRICS hm{};
    const float relX = imgX - place.drawTextX;
    const float relY = std::clamp(imgY - place.drawTextY, 0.0f, (std::max)(1.0f, place.measuredTextH - 1.0f));
    if (FAILED(place.newLayout->HitTestPoint(relX, relY, &isTrailing, &isInside, &hm))) {
        return wNew.size();
    }
    const size_t idx = static_cast<size_t>(hm.textPosition) + (isTrailing ? static_cast<size_t>(hm.length) : 0u);
    return (std::min)(wNew.size(), idx);
}

} // namespace tools3000::capture

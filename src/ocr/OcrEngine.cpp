// ─────────────────────────────────────────────────────────────────────────────
// OcrEngine.cpp — 双引擎高精度离线文字识别 (PP-OCRv4 OpenCV DNN + 增强版 Windows.Media.Ocr)
//                 及 OpenCV quirc 二维码检测、字符级 charBoxes 切分与 XY-Cut 分栏重排
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/OcrEngine.h"
#include "ocr/DeepOcrEngine.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/objdetect/barcode.hpp>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>

#include <unknwn.h>
#include <windows.h>
#include <objidl.h>
#include <MemoryBuffer.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string_view>
#include <thread>
#include <utility>

namespace tools3000::ocr {

namespace wf  = winrt::Windows::Foundation;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wmo = winrt::Windows::Media::Ocr;
namespace wgl = winrt::Windows::Globalization;

namespace {

/// 检测当前线程是否已绑定在单线程单元 (STA / MainSTA)，以防直接调用 WinRT IAsyncOperation::get() 触发 hresult_illegal_method_call。
bool isCurrentThreadSta() noexcept {
    APTTYPE aptType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER aptQualifier = APTTYPEQUALIFIER_NONE;
    if (FAILED(CoGetApartmentType(&aptType, &aptQualifier))) {
        return false;
    }
    return aptType == APTTYPE_STA || aptType == APTTYPE_MAINSTA;
}

/// 确保当前线程已进入 COM/WinRT 多线程公寓 (MTA)。
void ensureApartment() {
    if (isCurrentThreadSta()) {
        return;
    }
    static thread_local bool inited = false;
    if (inited) return;
    inited = true;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const&) {
        // 线程已初始化为 MTA，忽略。
    }
}

/// 若调用方处于 STA 线程，自动派发至独立后台 MTA 线程同步执行，彻底防止 WinRT .get() 抛异常或消息泵死锁。
template <typename Fn>
auto runInMtaSafe(Fn&& fn) -> decltype(fn()) {
    using Ret = decltype(fn());
    if (!isCurrentThreadSta()) {
        ensureApartment();
        return fn();
    }
    const std::string parentTrace = tools3000::core::TraceId::current();
    std::packaged_task<Ret()> task([func = std::forward<Fn>(fn), parentTrace]() mutable -> Ret {
        if (!parentTrace.empty()) {
            tools3000::core::TraceId::Scope inherited(parentTrace);
            ensureApartment();
            return func();
        }
        ensureApartment();
        return func();
    });
    std::future<Ret> fut = task.get_future();
    std::thread worker(std::move(task));
    worker.join();
    return fut.get();
}

/// cv::Mat → SoftwareBitmap(Bgra8)。失败返回 nullptr。
wgi::SoftwareBitmap matToBitmap(const cv::Mat& src) {
    cv::Mat bgra;
    switch (src.channels()) {
        case 4: bgra = src; break;
        case 3: cv::cvtColor(src, bgra, cv::COLOR_BGR2BGRA); break;
        case 1: cv::cvtColor(src, bgra, cv::COLOR_GRAY2BGRA); break;
        default: return nullptr;
    }
    if (!bgra.isContinuous()) bgra = bgra.clone();

    wgi::SoftwareBitmap bitmap(wgi::BitmapPixelFormat::Bgra8, bgra.cols, bgra.rows,
                               wgi::BitmapAlphaMode::Premultiplied);
    {
        wgi::BitmapBuffer buffer = bitmap.LockBuffer(wgi::BitmapBufferAccessMode::Write);
        wf::IMemoryBufferReference ref = buffer.CreateReference();
        auto byteAccess = ref.as<::Windows::Foundation::IMemoryBufferByteAccess>();

        uint8_t* data = nullptr;
        uint32_t capacity = 0;
        winrt::check_hresult(byteAccess->GetBuffer(&data, &capacity));

        wgi::BitmapPlaneDescription desc = buffer.GetPlaneDescription(0);
        const size_t rowBytes = static_cast<size_t>(bgra.cols) * 4;
        for (int y = 0; y < bgra.rows; ++y) {
            std::memcpy(data + desc.StartIndex + static_cast<size_t>(desc.Stride) * y,
                        bgra.ptr(y), rowBytes);
        }
    }
    return bitmap;
}

std::string toLowerAsciiCopy(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

/// 创建一个 OCR 引擎: 优先读取 /ocr/language 配置（支持 auto 及指定语种 tag，缺失对应语言包时平滑回退可用语种绝不崩溃）。
wmo::OcrEngine createEngine() {
    std::string langPref;
    try {
        langPref = tools3000::core::ConfigManager::instance().get<std::string>("/ocr/language", "auto");
    } catch (...) {
        langPref = "auto";
    }

    const std::string lowerPref = toLowerAsciiCopy(langPref);
    if (!lowerPref.empty() && lowerPref != "auto" && lowerPref != "system") {
        const std::wstring wideTag = tools3000::core::WinUtils::utf8ToWstring(langPref);
        if (!wideTag.empty()) {
            try {
                wgl::Language requestedLang(wideTag);
                if (wmo::OcrEngine::IsLanguageSupported(requestedLang)) {
                    if (auto exact = wmo::OcrEngine::TryCreateFromLanguage(requestedLang)) {
                        return exact;
                    }
                }
            } catch (...) {
                // Invalid or unsupported language tag; try prefix match among installed recognizer languages.
            }

            try {
                const auto installed = wmo::OcrEngine::AvailableRecognizerLanguages();
                std::string primaryPrefix = lowerPref;
                if (const auto dashPos = primaryPrefix.find('-'); dashPos != std::string::npos) {
                    if (primaryPrefix.rfind("zh-hans", 0) == 0 || primaryPrefix == "zh-cn" || primaryPrefix == "zh-sg") {
                        primaryPrefix = "zh-hans";
                    } else if (primaryPrefix.rfind("zh-hant", 0) == 0 || primaryPrefix == "zh-tw" ||
                               primaryPrefix == "zh-hk" || primaryPrefix == "zh-mo") {
                        primaryPrefix = "zh-hant";
                    } else {
                        primaryPrefix = primaryPrefix.substr(0, dashPos);
                    }
                }

                for (const auto& cand : installed) {
                    const std::string candTag = toLowerAsciiCopy(tools3000::core::WinUtils::wstringToUtf8(cand.LanguageTag().c_str()));
                    bool matched = (candTag == lowerPref) ||
                                   (candTag.rfind(primaryPrefix + "-", 0) == 0) ||
                                   (candTag == primaryPrefix);
                    if (!matched && primaryPrefix == "zh-hans" && (candTag == "zh-cn" || candTag == "zh-sg")) {
                        matched = true;
                    }
                    if (!matched && primaryPrefix == "zh-hant" &&
                        (candTag == "zh-tw" || candTag == "zh-hk" || candTag == "zh-mo")) {
                        matched = true;
                    }
                    if (matched) {
                        if (auto fallbackMatch = wmo::OcrEngine::TryCreateFromLanguage(cand)) {
                            return fallbackMatch;
                        }
                    }
                }
            } catch (...) {
                // Ignore enumeration error and fall through to profile/available language fallback.
            }

            LOG_WARN("Requested OCR language '{}' is not installed on this machine; falling back to available OCR language",
                     langPref);
        }
    }

    try {
        if (auto e = wmo::OcrEngine::TryCreateFromUserProfileLanguages()) return e;
    } catch (...) {}

    try {
        const auto installed = wmo::OcrEngine::AvailableRecognizerLanguages();
        for (const auto& cand : installed) {
            if (auto firstAvail = wmo::OcrEngine::TryCreateFromLanguage(cand)) {
                return firstAvail;
            }
        }
    } catch (...) {}

    try {
        return wmo::OcrEngine::TryCreateFromLanguage(wgl::Language(L"en-US"));
    } catch (...) {
        return nullptr;
    }
}

uint32_t decodeSingleUtf8Codepoint(std::string_view sv, size_t& cursor) noexcept {
    if (cursor >= sv.size()) return 0;
    const uint8_t b0 = static_cast<uint8_t>(sv[cursor]);
    if (b0 < 0x80u) {
        ++cursor;
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && cursor + 1 < sv.size()) {
        const uint8_t b1 = static_cast<uint8_t>(sv[cursor + 1]);
        if ((b1 & 0xC0u) == 0x80u) {
            cursor += 2;
            return ((static_cast<uint32_t>(b0 & 0x1Fu) << 6) | static_cast<uint32_t>(b1 & 0x3Fu));
        }
    } else if ((b0 & 0xF0u) == 0xE0u && cursor + 2 < sv.size()) {
        const uint8_t b1 = static_cast<uint8_t>(sv[cursor + 1]);
        const uint8_t b2 = static_cast<uint8_t>(sv[cursor + 2]);
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
            cursor += 3;
            return ((static_cast<uint32_t>(b0 & 0x0Fu) << 12) |
                    (static_cast<uint32_t>(b1 & 0x3Fu) << 6) |
                    static_cast<uint32_t>(b2 & 0x3Fu));
        }
    } else if ((b0 & 0xF8u) == 0xF0u && cursor + 3 < sv.size()) {
        const uint8_t b1 = static_cast<uint8_t>(sv[cursor + 1]);
        const uint8_t b2 = static_cast<uint8_t>(sv[cursor + 2]);
        const uint8_t b3 = static_cast<uint8_t>(sv[cursor + 3]);
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
            cursor += 4;
            return ((static_cast<uint32_t>(b0 & 0x07u) << 18) |
                    (static_cast<uint32_t>(b1 & 0x3Fu) << 12) |
                    (static_cast<uint32_t>(b2 & 0x3Fu) << 6) |
                    static_cast<uint32_t>(b3 & 0x3Fu));
        }
    }
    ++cursor;
    return b0;
}

bool isCjkOrFullwidthCodepoint(uint32_t cp) noexcept {
    return (cp >= 0x2E80u && cp <= 0x9FFFu) ||
           (cp >= 0xAC00u && cp <= 0xD7AFu) ||
           (cp >= 0xF900u && cp <= 0xFAFFu) ||
           (cp >= 0xFE10u && cp <= 0xFE6Fu) ||
           (cp >= 0xFF01u && cp <= 0xFF60u) ||
           (cp >= 0xFFE0u && cp <= 0xFFE6u) ||
           (cp >= 0x20000u && cp <= 0x323AFu);
}

float estimateGlyphVisualWeight(uint32_t cp) noexcept {
    if (cp == '\t') return 1.25f;
    if (cp == ' ') return 0.42f;
    if (isCjkOrFullwidthCodepoint(cp)) return 1.0f;
    switch (cp) {
        case 'i': case 'l': case 'I': case '1': case '!': case '|':
        case '.': case ',': case ':': case ';': case '\'': case '`':
        case '(': case ')': case '[': case ']': case '{': case '}':
            return 0.38f;
        case 'm': case 'w': case 'M': case 'W': case '@': case '%':
            return 0.78f;
        default:
            return 0.56f;
    }
}

bool isCjkOrFullwidth(wchar_t ch) noexcept {
    return (ch >= 0x3400 && ch <= 0x4DBF) ||
           (ch >= 0x4E00 && ch <= 0x9FFF) ||
           (ch >= 0x2E80 && ch <= 0x2FDF) ||
           (ch >= 0x3000 && ch <= 0x33FF) ||
           (ch >= 0xAC00 && ch <= 0xD7AF) ||
           (ch >= 0xF900 && ch <= 0xFAFF) ||
           (ch >= 0xFE10 && ch <= 0xFE6F) ||
           (ch >= 0xFF00 && ch <= 0xFFEF) ||
           (ch >= 0xD840 && ch <= 0xD884) ||
           (ch >= 0xDC00 && ch <= 0xDFFF);
}

bool isTightConnectorLeft(wchar_t ch) noexcept {
    return ch == L'/' || ch == L'\\' || ch == L'.' || ch == L'_' || ch == L'-' ||
           ch == L'#' || ch == L'(' || ch == L'[' || ch == L'{' || ch == L'@' ||
           ch == L':' || ch == L'—' || ch == L'–';
}

bool isTightConnectorRight(wchar_t ch) noexcept {
    return ch == L'/' || ch == L'\\' || ch == L'.' || ch == L'_' || ch == L'-' ||
           ch == L'#' || ch == L')' || ch == L']' || ch == L'}' || ch == L':' ||
           ch == L',' || ch == L';' || ch == L'—' || ch == L'–';
}

bool isAsciiDigit(wchar_t ch) noexcept {
    return ch >= L'0' && ch <= L'9';
}

bool isAsciiAlpha(wchar_t ch) noexcept {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
}

bool isSentenceTerminalPunct(wchar_t ch) noexcept {
    return ch == L'。' || ch == L'！' || ch == L'？' || ch == L'；' || ch == L'：' ||
           ch == L'.'  || ch == L'!'  || ch == L'?'  || ch == L';'  || ch == L':';
}

bool looksLikeListOrHeadingLine(std::wstring_view trimmed) noexcept {
    if (trimmed.empty()) return false;
    const wchar_t c0 = trimmed.front();
    if (c0 == L'#' || c0 == L'•' || c0 == L'·' || c0 == L'●' || c0 == L'○' ||
        c0 == L'▪' || c0 == L'▫' || c0 == L'—') {
        return true;
    }
    if ((c0 == L'-' || c0 == L'*' || c0 == L'+') && trimmed.size() >= 2 && trimmed[1] == L' ') {
        return true;
    }
    size_t idx = 0;
    if (c0 == L'(' || c0 == L'（' || c0 == L'[') {
        ++idx;
    }
    size_t digits = 0;
    while (idx < trimmed.size() && isAsciiDigit(trimmed[idx]) && digits < 4) {
        ++idx;
        ++digits;
    }
    if (digits > 0 && idx < trimmed.size()) {
        const wchar_t sep = trimmed[idx];
        if (sep == L'.' || sep == L')' || sep == L'）' || sep == L']' || sep == L'、') {
            if (idx + 1 >= trimmed.size() || trimmed[idx + 1] == L' ' || isCjkOrFullwidth(trimmed[idx + 1])) {
                return true;
            }
        }
    }
    return false;
}

struct AtomicLayoutBlock {
    std::wstring wtext;
    std::string utf8;
    float confidence = 1.0f;
    cv::Rect box;
    std::vector<cv::Rect> charBoxes;

    float left() const noexcept { return static_cast<float>(box.x); }
    float right() const noexcept { return static_cast<float>(box.x + (std::max)(1, box.width)); }
    float top() const noexcept { return static_cast<float>(box.y); }
    float bottom() const noexcept { return static_cast<float>(box.y + (std::max)(1, box.height)); }
    float cx() const noexcept { return static_cast<float>(box.x) + static_cast<float>((std::max)(1, box.width)) * 0.5f; }
    float cy() const noexcept { return static_cast<float>(box.y) + static_cast<float>((std::max)(1, box.height)) * 0.5f; }
    float w() const noexcept { return static_cast<float>((std::max)(1, box.width)); }
    float h() const noexcept { return static_cast<float>((std::max)(1, box.height)); }
};

cv::Rect mapTokenBoxToOrigRect(
    float x1, float y1, float x2, float y2,
    float safeScale, int borderPad,
    int maxImageWidth, int maxImageHeight) noexcept {
    int ox1 = static_cast<int>(std::round(x1 / safeScale)) - borderPad;
    int oy1 = static_cast<int>(std::round(y1 / safeScale)) - borderPad;
    int ox2 = static_cast<int>(std::round(x2 / safeScale)) - borderPad;
    int oy2 = static_cast<int>(std::round(y2 / safeScale)) - borderPad;
    if (maxImageWidth > 0 && maxImageHeight > 0) {
        ox1 = std::clamp(ox1, 0, maxImageWidth);
        oy1 = std::clamp(oy1, 0, maxImageHeight);
        ox2 = std::clamp(ox2, ox1, maxImageWidth);
        oy2 = std::clamp(oy2, oy1, maxImageHeight);
    } else {
        ox1 = (std::max)(0, ox1);
        oy1 = (std::max)(0, oy1);
        ox2 = (std::max)(ox1, ox2);
        oy2 = (std::max)(oy1, oy2);
    }
    return cv::Rect(ox1, oy1, (std::max)(1, ox2 - ox1), (std::max)(1, oy2 - oy1));
}

float computeMedianBlockHeight(const std::vector<AtomicLayoutBlock>& blocks) {
    if (blocks.empty()) return 16.0f;
    std::vector<float> heights;
    heights.reserve(blocks.size());
    for (const auto& b : blocks) {
        if (b.box.height > 1) heights.push_back(static_cast<float>(b.box.height));
    }
    if (heights.empty()) return 16.0f;
    const size_t mid = heights.size() / 2;
    std::nth_element(heights.begin(), heights.begin() + static_cast<std::ptrdiff_t>(mid), heights.end());
    return (std::max)(8.0f, heights[mid]);
}

std::vector<float> clusterRowCentersStrict(const std::vector<AtomicLayoutBlock>& subset, float medH) {
    if (subset.empty()) return {};
    std::vector<float> cys;
    cys.reserve(subset.size());
    for (const auto& b : subset) cys.push_back(b.cy());
    std::sort(cys.begin(), cys.end());

    std::vector<float> centers;
    centers.reserve(cys.size());
    float groupStart = cys.front();
    float sum = cys.front();
    size_t cnt = 1;
    const float tol = (std::max)(4.0f, medH * 0.34f);
    for (size_t i = 1; i < cys.size(); ++i) {
        if (cys[i] - groupStart <= tol) {
            sum += cys[i];
            ++cnt;
        } else {
            centers.push_back(sum / static_cast<float>(cnt));
            groupStart = cys[i];
            sum = cys[i];
            cnt = 1;
        }
    }
    centers.push_back(sum / static_cast<float>(cnt));
    return centers;
}

/// 扫描原图中的长竖向 UI 窗格分割线（如 ERP 侧边栏边框、分栏竖线），仅在未穿过任何文字块内部的空白列触发
std::vector<float> detectVerticalUiDividers(
    const cv::Mat& sourceImage,
    const std::vector<AtomicLayoutBlock>& blocks) {
    std::vector<float> dividers;
    if (sourceImage.empty() || sourceImage.cols < 64 || sourceImage.rows < 64 || blocks.size() < 2) {
        return dividers;
    }

    cv::Mat gray;
    if (sourceImage.channels() == 1) {
        gray = sourceImage;
    } else if (sourceImage.channels() == 3) {
        cv::cvtColor(sourceImage, gray, cv::COLOR_BGR2GRAY);
    } else if (sourceImage.channels() == 4) {
        cv::cvtColor(sourceImage, gray, cv::COLOR_BGRA2GRAY);
    } else {
        return dividers;
    }

    const int imgW = gray.cols;
    const int imgH = gray.rows;
    std::vector<uint8_t> textOccupiedX(static_cast<size_t>(imgW), 0);
    for (const auto& b : blocks) {
        const int x0 = std::clamp(b.box.x + 2, 0, imgW - 1);
        const int x1 = std::clamp(b.box.x + b.box.width - 2, 0, imgW - 1);
        for (int x = x0; x <= x1; ++x) {
            textOccupiedX[static_cast<size_t>(x)] = 1;
        }
    }

    const int yStep = (imgH > 900) ? 2 : 1;
    const int minSamplesNeeded = (std::max)(48, (imgH / yStep) * 32 / 100);
    std::vector<int> rawCandidates;

    for (int x = 8; x + 8 < imgW; ++x) {
        if (textOccupiedX[static_cast<size_t>(x)] != 0) continue;
        int edgeHits = 0;
        int maxRun = 0;
        int curRun = 0;
        int missAllowance = 0;
        for (int y = yStep; y + yStep < imgH; y += yStep) {
            const uint8_t* row = gray.ptr<uint8_t>(y);
            const uint8_t* prevRow = gray.ptr<uint8_t>(y - yStep);
            const int c = row[x];
            const int horizContrast = (std::max)(std::abs(c - static_cast<int>(row[x - 1])),
                                                 std::abs(c - static_cast<int>(row[x + 1])));
            const int vertContinuity = std::abs(c - static_cast<int>(prevRow[x]));
            if (horizContrast >= 9 && vertContinuity <= 5) {
                ++edgeHits;
                ++curRun;
                missAllowance = 3;
                if (curRun > maxRun) maxRun = curRun;
            } else if (missAllowance > 0) {
                --missAllowance;
            } else {
                curRun = 0;
            }
        }
        if (edgeHits >= minSamplesNeeded && maxRun >= minSamplesNeeded / 2) {
            rawCandidates.push_back(x);
        }
    }

    // 合并相邻 6px 内的双边框/滚动条竖线簇
    for (size_t i = 0; i < rawCandidates.size();) {
        size_t j = i + 1;
        int sumX = rawCandidates[i];
        while (j < rawCandidates.size() && rawCandidates[j] - rawCandidates[j - 1] <= 6) {
            sumX += rawCandidates[j];
            ++j;
        }
        dividers.push_back(static_cast<float>(sumX) / static_cast<float>(j - i));
        i = j;
    }
    return dividers;
}

struct LayoutVerticalSplit {
    bool valid = false;
    bool hasUiDivider = false;
    float splitX = 0.0f;
    float gapWidth = 0.0f;
    float score = 0.0f;
};

size_t countMultiColumnRowsInSide(
    const std::vector<AtomicLayoutBlock>& sideBlocks,
    float medH,
    bool mergeWordTokens) {
    if (sideBlocks.size() < 2) return 0;
    const float rowTol = (std::max)(3.8f, medH * 0.28f);
    const float subColGap = mergeWordTokens
        ? (std::max)(12.0f, medH * 0.85f)
        : (std::max)(6.0f, medH * 0.38f);

    std::vector<size_t> idx(sideBlocks.size());
    for (size_t i = 0; i < sideBlocks.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&sideBlocks](size_t a, size_t b) {
        return sideBlocks[a].cy() < sideBlocks[b].cy();
    });

    size_t multiColRows = 0;
    size_t i = 0;
    while (i < idx.size()) {
        const float baseCy = sideBlocks[idx[i]].cy();
        std::vector<size_t> rowGroup = {idx[i]};
        size_t j = i + 1;
        while (j < idx.size() && (sideBlocks[idx[j]].cy() - baseCy) <= rowTol) {
            rowGroup.push_back(idx[j]);
            ++j;
        }
        if (rowGroup.size() >= 2) {
            std::sort(rowGroup.begin(), rowGroup.end(), [&sideBlocks](size_t a, size_t b) {
                return sideBlocks[a].left() < sideBlocks[b].left();
            });
            for (size_t k = 0; k + 1 < rowGroup.size(); ++k) {
                if (sideBlocks[rowGroup[k + 1]].left() - sideBlocks[rowGroup[k]].right() >= subColGap) {
                    ++multiColRows;
                    break;
                }
            }
        }
        i = j;
    }
    return multiColRows;
}

LayoutVerticalSplit findBestLayoutVerticalSplit(
    const std::vector<AtomicLayoutBlock>& blocks,
    float medH,
    const std::vector<float>& dividerXs,
    bool mergeWordTokens) {
    LayoutVerticalSplit best{};
    if (blocks.size() < 2) return best;

    std::vector<std::pair<float, float>> intervals;
    intervals.reserve(blocks.size());
    float minX = FLT_MAX;
    float maxX = -FLT_MAX;
    for (const auto& b : blocks) {
        const float l = b.left();
        const float r = (std::max)(l + 1.0f, b.right());
        intervals.emplace_back(l, r);
        minX = (std::min)(minX, l);
        maxX = (std::max)(maxX, r);
    }
    if (maxX - minX < medH * 4.0f) {
        return best;
    }

    std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) {
        return (a.first != b.first) ? (a.first < b.first) : (a.second < b.second);
    });

    std::vector<std::pair<float, float>> merged;
    merged.reserve(intervals.size());
    for (const auto& iv : intervals) {
        if (merged.empty() || iv.first > merged.back().second) {
            merged.push_back(iv);
        } else {
            merged.back().second = (std::max)(merged.back().second, iv.second);
        }
    }
    if (merged.size() < 2) {
        return best;
    }

    const float minCandidateGap = (std::max)(10.0f, medH * 0.70f);
    const float classicCorridorMinW = (std::max)(14.0f, medH * 1.55f);
    const float syncTol = (std::max)(3.6f, medH * 0.24f);

    for (size_t i = 0; i + 1 < merged.size(); ++i) {
        const float gapL = merged[i].second;
        const float gapR = merged[i + 1].first;
        const float gapW = gapR - gapL;
        if (gapW < minCandidateGap) continue;

        const float midX = (gapL + gapR) * 0.5f;
        bool hasUiDivider = false;
        for (float dx : dividerXs) {
            if (dx >= gapL - 3.0f && dx <= gapR + 3.0f) {
                hasUiDivider = true;
                break;
            }
        }

        std::vector<AtomicLayoutBlock> leftBlocks;
        std::vector<AtomicLayoutBlock> rightBlocks;
        leftBlocks.reserve(blocks.size());
        rightBlocks.reserve(blocks.size());
        float leftMinX = FLT_MAX, leftMaxX = -FLT_MAX;
        float rightMinX = FLT_MAX, rightMaxX = -FLT_MAX;
        float leftMinY = FLT_MAX, leftMaxY = -FLT_MAX;
        float rightMinY = FLT_MAX, rightMaxY = -FLT_MAX;
        for (const auto& b : blocks) {
            if (b.cx() < midX) {
                leftBlocks.push_back(b);
                leftMinX = (std::min)(leftMinX, b.left());
                leftMaxX = (std::max)(leftMaxX, b.right());
                leftMinY = (std::min)(leftMinY, b.top());
                leftMaxY = (std::max)(leftMaxY, b.bottom());
            } else {
                rightBlocks.push_back(b);
                rightMinX = (std::min)(rightMinX, b.left());
                rightMaxX = (std::max)(rightMaxX, b.right());
                rightMinY = (std::min)(rightMinY, b.top());
                rightMaxY = (std::max)(rightMaxY, b.bottom());
            }
        }
        if (leftBlocks.empty() || rightBlocks.empty()) continue;

        // 若无物理竖分割线，且左右两侧在 Y 轴上根本处于上下不同楼层（零垂直跨度重叠），应交由水平切分处理而非竖劈
        const float vertSpanOverlap = (std::min)(leftMaxY, rightMaxY) - (std::max)(leftMinY, rightMinY);
        if (!hasUiDivider && vertSpanOverlap <= 0.0f) {
            continue;
        }

        const auto leftCys = clusterRowCentersStrict(leftBlocks, medH);
        const auto rightCys = clusterRowCentersStrict(rightBlocks, medH);
        const size_t leftLines = leftCys.size();
        const size_t rightLines = rightCys.size();

        // 双指针计算左右两侧视觉行的 1:1 严格水平基线对齐率
        size_t li = 0, ri = 0, matched = 0;
        while (li < leftCys.size() && ri < rightCys.size()) {
            const float diff = leftCys[li] - rightCys[ri];
            if (std::abs(diff) <= syncTol) {
                ++matched;
                ++li;
                ++ri;
            } else if (diff < 0.0f) {
                ++li;
            } else {
                ++ri;
            }
        }
        const size_t maxRows = (std::max)(leftLines, rightLines);
        const size_t minRows = (std::min)(leftLines, rightLines);
        const float globalSyncRatio = (maxRows > 0) ? (static_cast<float>(matched) / static_cast<float>(maxRows)) : 0.0f;
        const float minSyncRatio = (minRows > 0) ? (static_cast<float>(matched) / static_cast<float>(minRows)) : 0.0f;
        const float leftSpanW = (leftMaxX > leftMinX) ? (leftMaxX - leftMinX) : 0.0f;
        const float rightSpanW = (rightMaxX > rightMinX) ? (rightMaxX - rightMinX) : 0.0f;

        // 区分「左右并排的双宽栏文档/双面板」与「多列同步网格数据表 / 紧凑 Key-Value 表单」
        const size_t leftMultiColRows = countMultiColumnRowsInSide(leftBlocks, medH, mergeWordTokens);
        const bool isWideMultiColGridTable =
            (leftMultiColRows >= 2) &&
            (leftLines == rightLines) &&
            (globalSyncRatio >= 0.90f);
        const bool looksLikeTwoColumnLayout =
            !isWideMultiColGridTable &&
            (minRows >= 2) &&
            (gapW >= medH * 3.2f) &&
            (leftSpanW >= medH * 8.2f) &&
            (rightSpanW >= medH * 8.2f);

        const bool isSynchronizedTableOrForm =
            !hasUiDivider &&
            !looksLikeTwoColumnLayout &&
            (minRows >= 2) &&
            (globalSyncRatio >= 0.65f) &&
            (minSyncRatio >= 0.85f);

        bool shouldSplit = false;
        if (gapW >= (std::max)(440.0f, medH * 24.0f)) {
            // 1. 横跨半屏以上的超巨型水平空白鸿沟（如左上表单 vs 右上表单、左下备注 vs 右下金额区）：无条件隔离为独立分区
            shouldSplit = true;
        } else if (hasUiDivider && globalSyncRatio < 0.68f && maxRows >= 2) {
            // 2. 原图存在贯穿竖向 UI 分割线且两侧行高/行数异步（如主窗口 vs 右侧自定义字段侧边栏）：隔离为独立窗格
            shouldSplit = true;
        } else if (minRows >= 2) {
            // 垂直分栏（非半屏鸿沟、非物理分割线）要求左右两侧均至少具备 2 行，严禁单行伪分栏撕裂表格末行或段落末行
            if (gapW >= (std::max)(260.0f, medH * 16.0f)) {
                // 3. 260~440px 宽空白：仅当两侧行数与基线 1:1 严丝合缝对齐（宽表中间空缺列）时保留同表，否则切分
                if (!isSynchronizedTableOrForm || leftLines != rightLines || globalSyncRatio < 0.90f) {
                    shouldSplit = true;
                }
            } else if (gapW >= (std::max)(165.0f, medH * 10.5f) && !isSynchronizedTableOrForm) {
                // 4. 中大跨度空白且两侧并非行行严丝合缝对齐的数据表：隔离为独立分区
                shouldSplit = true;
            } else if (gapW >= classicCorridorMinW && !isSynchronizedTableOrForm) {
                // 5. 经典多栏垂直走廊：保护同步对齐的数据表与紧凑键值对表单，果断切分双栏正文与异构侧边栏
                shouldSplit = true;
            }
        }

        if (shouldSplit) {
            const float candidateScore =
                gapW +
                (hasUiDivider ? medH * 8.0f : 0.0f) +
                (1.0f - globalSyncRatio) * medH * 3.5f;
            if (candidateScore > best.score) {
                best.valid = true;
                best.hasUiDivider = hasUiDivider;
                best.splitX = midX;
                best.gapWidth = gapW;
                best.score = candidateScore;
            }
        }
    }
    return best;
}

void clusterAlignedRowsInRegion(
    std::vector<AtomicLayoutBlock> blocks,
    int regionId,
    int& ioNextAlignedRowId,
    bool mergeWordTokens,
    std::vector<OcrResult>& outResults) {
    if (blocks.empty()) return;

    // Step 1: 针对逐词分词引擎 (mergeWordTokens=true) 将同句紧邻单词缝合为单元格短语；
    //         针对已分块引擎 (PP-OCRv4 DBNet / 预切单元格) 仅缝合紧贴断片，保留相邻表格列独立性
    std::sort(blocks.begin(), blocks.end(), [](const AtomicLayoutBlock& a, const AtomicLayoutBlock& b) {
        if (std::abs(a.cy() - b.cy()) > 4.0f) return a.cy() < b.cy();
        return a.left() < b.left();
    });

    std::vector<AtomicLayoutBlock> phraseCells;
    phraseCells.reserve(blocks.size());
    std::vector<bool> consumed(blocks.size(), false);

    // 按 X 左边界索引以便同行向右贪心缝合紧邻词
    std::vector<size_t> byX(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) byX[i] = i;
    std::sort(byX.begin(), byX.end(), [&blocks](size_t a, size_t b) {
        if (blocks[a].box.x != blocks[b].box.x) return blocks[a].box.x < blocks[b].box.x;
        return blocks[a].box.y < blocks[b].box.y;
    });

    auto isSharedColumnCorridor = [&blocks](float gapL, float gapR, float curCy, float localH) -> bool {
        if (gapR - gapL < (std::max)(6.0f, localH * 0.32f)) return false;
        bool otherRowHasLeftEdge = false;
        bool otherRowHasRightCol = false;
        for (const auto& other : blocks) {
            if (std::abs(other.cy() - curCy) <= localH * 0.65f) continue;
            // 若其他行有文字直接横穿该间隙，则不是垂直列走廊
            if (other.left() < gapR - 1.5f && other.right() > gapL + 1.5f) {
                return false;
            }
            if (std::abs(other.right() - gapL) <= localH * 0.75f) {
                otherRowHasLeftEdge = true;
            }
            if (std::abs(other.left() - gapR) <= localH * 0.75f) {
                otherRowHasRightCol = true;
            }
        }
        return otherRowHasLeftEdge && otherRowHasRightCol;
    };

    for (size_t orderIdx = 0; orderIdx < byX.size(); ++orderIdx) {
        const size_t seedIdx = byX[orderIdx];
        if (consumed[seedIdx]) continue;
        consumed[seedIdx] = true;

        AtomicLayoutBlock cur = std::move(blocks[seedIdx]);
        if (cur.wtext.empty()) continue;

        bool extended = true;
        while (extended) {
            extended = false;
            int bestNextOrder = -1;
            float bestGap = FLT_MAX;
            for (size_t probe = orderIdx + 1; probe < byX.size(); ++probe) {
                const size_t candIdx = byX[probe];
                if (consumed[candIdx]) continue;
                const auto& cand = blocks[candIdx];
                if (cand.wtext.empty()) continue;

                const float localH = (std::max)(10.0f, (cur.h() + cand.h()) * 0.5f);
                const float minH = (std::max)(8.0f, (std::min)(cur.h(), cand.h()));
                const float maxMergeGap = mergeWordTokens
                    ? (std::max)(9.0f, localH * 0.78f)
                    : (std::max)(3.0f, minH * 0.18f);
                const float gap = cand.left() - cur.right();
                if (gap > maxMergeGap) {
                    break; // 后续 X 更大，无需继续向后扫描
                }
                if (gap < -minH * 0.45f) continue;

                const float dy = std::abs(cand.cy() - cur.cy());
                const float overlapY = (std::min)(cur.bottom(), cand.bottom()) - (std::max)(cur.top(), cand.top());
                if (dy <= (std::max)(3.6f, minH * 0.28f) && overlapY >= minH * 0.55f) {
                    if (mergeWordTokens && gap >= 5.5f &&
                        isSharedColumnCorridor(cur.right(), cand.left(), cur.cy(), localH)) {
                        continue; // 贯穿多行的表格列走廊留给 Step 2 以 \t 分列
                    }
                    if (gap < bestGap) {
                        bestGap = gap;
                        bestNextOrder = static_cast<int>(probe);
                    }
                }
            }

            if (bestNextOrder >= 0) {
                const size_t attachIdx = byX[static_cast<size_t>(bestNextOrder)];
                consumed[attachIdx] = true;
                auto& nextBlk = blocks[attachIdx];

                const float localH = (std::max)(10.0f, (cur.h() + nextBlk.h()) * 0.5f);
                const float gap = nextBlk.left() - cur.right();
                const wchar_t lastCh = cur.wtext.back();
                const wchar_t firstCh = nextBlk.wtext.front();
                bool addSpace = false;
                if (isCjkOrFullwidth(lastCh) || isCjkOrFullwidth(firstCh)) {
                    if (gap > localH * 0.65f) addSpace = true;
                } else if (isTightConnectorLeft(lastCh) || isTightConnectorRight(firstCh)) {
                    if (gap > localH * 0.55f) addSpace = true;
                } else if (isAsciiDigit(lastCh) && isAsciiDigit(firstCh) && gap < localH * 0.32f) {
                    addSpace = false;
                } else if (gap >= localH * 0.14f) {
                    addSpace = true;
                }

                if (addSpace) {
                    cur.wtext.push_back(L' ');
                    cur.utf8.push_back(' ');
                    const int sx1 = (std::min)(cur.box.x + cur.box.width, nextBlk.box.x);
                    const int sx2 = (std::max)(sx1 + 1, nextBlk.box.x);
                    const int sy1 = (std::min)(cur.box.y, nextBlk.box.y);
                    const int sy2 = (std::max)(cur.box.y + cur.box.height, nextBlk.box.y + nextBlk.box.height);
                    cur.charBoxes.emplace_back(sx1, sy1, sx2 - sx1, (std::max)(1, sy2 - sy1));
                }

                cur.wtext += nextBlk.wtext;
                cur.utf8 += nextBlk.utf8;
                cur.confidence = (cur.confidence + nextBlk.confidence) * 0.5f;
                cur.charBoxes.insert(cur.charBoxes.end(), nextBlk.charBoxes.begin(), nextBlk.charBoxes.end());

                const int nx1 = (std::min)(cur.box.x, nextBlk.box.x);
                const int ny1 = (std::min)(cur.box.y, nextBlk.box.y);
                const int nx2 = (std::max)(cur.box.x + cur.box.width, nextBlk.box.x + nextBlk.box.width);
                const int ny2 = (std::max)(cur.box.y + cur.box.height, nextBlk.box.y + nextBlk.box.height);
                cur.box = cv::Rect(nx1, ny1, (std::max)(1, nx2 - nx1), (std::max)(1, ny2 - ny1));
                extended = true;
            }
        }

        phraseCells.push_back(std::move(cur));
    }

    if (phraseCells.empty()) return;

    // Step 2: 严苛水平基线锁同行分组（拒绝错位滚雪球漂移，仅真正对齐的单元格归为同一逻辑行）
    std::sort(phraseCells.begin(), phraseCells.end(), [](const AtomicLayoutBlock& a, const AtomicLayoutBlock& b) {
        return a.cy() < b.cy();
    });

    struct AlignedRowCandidate {
        std::vector<AtomicLayoutBlock> cells;
        float minCy = FLT_MAX;
        float maxCy = -FLT_MAX;
        float minBot = FLT_MAX;
        float maxBot = -FLT_MAX;
        float minTop = FLT_MAX;
        float maxBotAll = -FLT_MAX;
        float sumCy = 0.0f;
        float sumH = 0.0f;

        float avgCy() const noexcept { return cells.empty() ? 0.0f : sumCy / static_cast<float>(cells.size()); }
        float avgH() const noexcept { return cells.empty() ? 16.0f : sumH / static_cast<float>(cells.size()); }
    };

    std::vector<AlignedRowCandidate> candidateRows;
    for (auto& cell : phraseCells) {
        int bestRow = -1;
        float bestDy = FLT_MAX;
        for (int r = static_cast<int>(candidateRows.size()) - 1; r >= 0; --r) {
            const auto& row = candidateRows[static_cast<size_t>(r)];
            const float rowH = row.avgH();
            const float cellH = cell.h();
            const float minH = (std::max)(8.0f, (std::min)(rowH, cellH));
            const float maxH = (std::max)(rowH, cellH);
            // 字号差异过大（如大标题与旁侧角标）不绑为同一对齐行
            if (minH < maxH * 0.58f) continue;

            // 同一行内 X 投影不得重叠（重叠说明是上下两行）
            bool horizClash = false;
            for (const auto& existing : row.cells) {
                const float ox = (std::min)(existing.right(), cell.right()) - (std::max)(existing.left(), cell.left());
                const float minW = (std::min)(existing.w(), cell.w());
                if (ox > (std::max)(4.0f, minW * 0.22f)) {
                    horizClash = true;
                    break;
                }
            }
            if (horizClash) continue;

            // 整行全局水平极差锁：杜绝从左到右逐格倾斜爬坡漂移
            const float newMinCy = (std::min)(row.minCy, cell.cy());
            const float newMaxCy = (std::max)(row.maxCy, cell.cy());
            const float newMinBot = (std::min)(row.minBot, cell.bottom());
            const float newMaxBot = (std::max)(row.maxBot, cell.bottom());
            const float cySpan = newMaxCy - newMinCy;
            const float botSpan = newMaxBot - newMinBot;

            const float strictTol = (std::max)(3.8f, minH * 0.25f);
            const bool centerAligned = (cySpan <= strictTol);
            const bool baselineAligned = (botSpan <= strictTol) && (cySpan <= (std::max)(5.0f, minH * 0.32f));
            if (!centerAligned && !baselineAligned) continue;

            const float overlapY = (std::min)(row.maxBotAll, cell.bottom()) - (std::max)(row.minTop, cell.top());
            if (overlapY < minH * 0.60f) continue;

            const float dy = std::abs(cell.cy() - row.avgCy());
            if (dy < bestDy) {
                bestDy = dy;
                bestRow = r;
            }
        }

        if (bestRow >= 0) {
            auto& dst = candidateRows[static_cast<size_t>(bestRow)];
            dst.minCy = (std::min)(dst.minCy, cell.cy());
            dst.maxCy = (std::max)(dst.maxCy, cell.cy());
            dst.minBot = (std::min)(dst.minBot, cell.bottom());
            dst.maxBot = (std::max)(dst.maxBot, cell.bottom());
            dst.minTop = (std::min)(dst.minTop, cell.top());
            dst.maxBotAll = (std::max)(dst.maxBotAll, cell.bottom());
            dst.sumCy += cell.cy();
            dst.sumH += cell.h();
            dst.cells.push_back(std::move(cell));
        } else {
            AlignedRowCandidate nr;
            nr.minCy = nr.maxCy = cell.cy();
            nr.minBot = nr.maxBot = nr.maxBotAll = cell.bottom();
            nr.minTop = cell.top();
            nr.sumCy = cell.cy();
            nr.sumH = cell.h();
            nr.cells.push_back(std::move(cell));
            candidateRows.push_back(std::move(nr));
        }
    }

    std::sort(candidateRows.begin(), candidateRows.end(), [](const AlignedRowCandidate& a, const AlignedRowCandidate& b) {
        return a.avgCy() < b.avgCy();
    });

    // Step 3: 反跨骑冲突校验与超远非表格空隙切分，输出带 layoutRegionId 与 alignedRowId 的结果
    auto hasBridgeOrMultiRowTableSpan = [&](size_t selfRowIdx, float gapLeft, float gapRight) -> bool {
        int coSpanningRows = 0;
        for (size_t r = 0; r < candidateRows.size(); ++r) {
            if (r == selfRowIdx) continue;
            bool hasLeftSide = false;
            bool hasRightSide = false;
            for (const auto& c : candidateRows[r].cells) {
                if (c.right() > gapLeft + 4.0f && c.left() < gapRight - 4.0f) {
                    return true; // 其他行在该空白处有填充单元格（标准空缺格桥接）
                }
                if (c.right() <= gapLeft + 36.0f) hasLeftSide = true;
                if (c.left() >= gapRight - 36.0f) hasRightSide = true;
            }
            if (hasLeftSide && hasRightSide) {
                ++coSpanningRows;
            }
        }
        return coSpanningRows >= 1;
    };

    auto straddlesAdjacentRow = [&](size_t selfRowIdx, const AtomicLayoutBlock& cell) -> bool {
        for (size_t r = 0; r < candidateRows.size(); ++r) {
            if (r == selfRowIdx) continue;
            const auto& other = candidateRows[r];
            // 仅检查水平上与 cell 分处不同列的其他行
            const float minH = (std::max)(8.0f, (std::min)(other.avgH(), cell.h()));
            const float overlapY = (std::min)(other.maxBotAll, cell.bottom()) - (std::max)(other.minTop, cell.top());
            if (overlapY >= minH * 0.32f &&
                std::abs(cell.cy() - other.avgCy()) + 0.8f < std::abs(cell.cy() - candidateRows[selfRowIdx].avgCy())) {
                return true;
            }
        }
        return false;
    };

    auto emitSubRow = [&](std::vector<AtomicLayoutBlock>& subCells) {
        if (subCells.empty()) return;
        const int curAlignedRowId = ioNextAlignedRowId++;
        std::string lineUtf8;
        std::vector<cv::Rect> lineCharBoxes;
        int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
        float confSum = 0.0f;

        for (size_t i = 0; i < subCells.size(); ++i) {
            auto& c = subCells[i];
            if (c.utf8.empty()) continue;
            if (!lineUtf8.empty() && i > 0) {
                const auto& prev = subCells[i - 1];
                lineUtf8.push_back('\t');
                const int gx1 = (std::min)(prev.box.x + prev.box.width, c.box.x);
                const int gx2 = (std::max)(gx1 + 1, c.box.x);
                const int gy1 = (std::min)(prev.box.y, c.box.y);
                const int gy2 = (std::max)(prev.box.y + prev.box.height, c.box.y + c.box.height);
                lineCharBoxes.emplace_back(gx1, gy1, gx2 - gx1, (std::max)(1, gy2 - gy1));
            }

            minX = (std::min)(minX, c.box.x);
            minY = (std::min)(minY, c.box.y);
            maxX = (std::max)(maxX, c.box.x + c.box.width);
            maxY = (std::max)(maxY, c.box.y + c.box.height);
            confSum += c.confidence;
            lineUtf8 += c.utf8;

            const size_t expectedCellCps = OcrEngine::countUtf8Codepoints(c.utf8);
            if (c.charBoxes.size() == expectedCellCps) {
                lineCharBoxes.insert(lineCharBoxes.end(), c.charBoxes.begin(), c.charBoxes.end());
            } else {
                auto fallbackBoxes = OcrEngine::buildProportionalCharBoxes(c.utf8, c.box);
                lineCharBoxes.insert(lineCharBoxes.end(), fallbackBoxes.begin(), fallbackBoxes.end());
            }
        }
        if (lineUtf8.empty() || minX > maxX || minY > maxY) return;

        OcrResult r;
        r.text = std::move(lineUtf8);
        r.confidence = confSum / static_cast<float>(subCells.size());
        r.box = cv::Rect(minX, minY, (std::max)(1, maxX - minX), (std::max)(1, maxY - minY));
        const size_t totalCps = OcrEngine::countUtf8Codepoints(r.text);
        if (lineCharBoxes.size() == totalCps) {
            r.charBoxes = std::move(lineCharBoxes);
        } else {
            r.charBoxes = OcrEngine::buildProportionalCharBoxes(r.text, r.box);
        }
        r.layoutRegionId = regionId;
        r.alignedRowId = curAlignedRowId;
        outResults.push_back(std::move(r));
    };

    for (size_t r = 0; r < candidateRows.size(); ++r) {
        auto& cells = candidateRows[r].cells;
        if (cells.empty()) continue;
        std::sort(cells.begin(), cells.end(), [](const AtomicLayoutBlock& a, const AtomicLayoutBlock& b) {
            return a.left() < b.left();
        });

        std::vector<AtomicLayoutBlock> curGroup;
        curGroup.reserve(cells.size());
        for (size_t i = 0; i < cells.size(); ++i) {
            if (!curGroup.empty()) {
                const auto& prev = curGroup.back();
                const float refH = (std::max)(10.0f, (prev.h() + cells[i].h()) * 0.5f);
                const float gap = cells[i].left() - prev.right();
                const bool unbridgedHugeGap =
                    (gap > (std::max)(440.0f, refH * 24.0f)) ||
                    ((gap > (std::max)(195.0f, refH * 12.0f)) &&
                     !hasBridgeOrMultiRowTableSpan(r, prev.right(), cells[i].left()));
                if (unbridgedHugeGap || straddlesAdjacentRow(r, cells[i])) {
                    emitSubRow(curGroup);
                    curGroup.clear();
                }
            }
            curGroup.push_back(std::move(cells[i]));
        }
        emitSubRow(curGroup);
    }
}

void recursiveLayoutXYCut(
    std::vector<AtomicLayoutBlock> blocks,
    float medH,
    const std::vector<float>& dividerXs,
    int depth,
    bool mergeWordTokens,
    int& ioNextRegionId,
    int& ioNextAlignedRowId,
    std::vector<OcrResult>& outResults) {
    if (blocks.empty()) return;
    if (depth >= 10 || blocks.size() < 2) {
        clusterAlignedRowsInRegion(
            std::move(blocks), ioNextRegionId++, ioNextAlignedRowId, mergeWordTokens, outResults);
        return;
    }

    const LayoutVerticalSplit vSplit = findBestLayoutVerticalSplit(blocks, medH, dividerXs, mergeWordTokens);
    if (vSplit.valid && vSplit.hasUiDivider) {
        // 1. 存在物理竖向 UI 窗格分割线（如主窗体 vs 右侧侧边栏）：最高优先级隔离左右窗格
        std::vector<AtomicLayoutBlock> leftCol;
        std::vector<AtomicLayoutBlock> rightCol;
        leftCol.reserve(blocks.size());
        rightCol.reserve(blocks.size());
        for (auto& b : blocks) {
            if (b.cx() < vSplit.splitX) {
                leftCol.push_back(std::move(b));
            } else {
                rightCol.push_back(std::move(b));
            }
        }
        recursiveLayoutXYCut(std::move(leftCol), medH, dividerXs, depth + 1, mergeWordTokens, ioNextRegionId, ioNextAlignedRowId, outResults);
        recursiveLayoutXYCut(std::move(rightCol), medH, dividerXs, depth + 1, mergeWordTokens, ioNextRegionId, ioNextAlignedRowId, outResults);
        return;
    }

    // 2. 扫描全宽水平空白缝：若存在大跨度上下独立面板空白（>= 5.2*H），或切开可解锁局部垂直分栏走廊，优先水平切分面板
    std::vector<size_t> byY(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) byY[i] = i;
    std::sort(byY.begin(), byY.end(), [&blocks](size_t a, size_t b) {
        return blocks[a].top() < blocks[b].top();
    });

    std::vector<float> prefixMaxBottom(blocks.size(), 0.0f);
    float runningBottom = -FLT_MAX;
    for (size_t i = 0; i < byY.size(); ++i) {
        runningBottom = (std::max)(runningBottom, blocks[byY[i]].bottom());
        prefixMaxBottom[i] = runningBottom;
    }

    size_t bestMajorPanelHIdx = 0;
    float bestMajorPanelHGap = 0.0f;
    const float hugePanelHGap = (std::max)(90.0f, medH * 5.2f);
    for (size_t i = 0; i + 1 < byY.size(); ++i) {
        const float hGap = blocks[byY[i + 1]].top() - prefixMaxBottom[i];
        if (hGap >= hugePanelHGap && hGap > bestMajorPanelHGap) {
            bestMajorPanelHGap = hGap;
            bestMajorPanelHIdx = i + 1;
        }
    }
    if (bestMajorPanelHIdx > 0 && bestMajorPanelHIdx < byY.size()) {
        std::vector<AtomicLayoutBlock> topBlocks;
        std::vector<AtomicLayoutBlock> botBlocks;
        topBlocks.reserve(bestMajorPanelHIdx);
        botBlocks.reserve(byY.size() - bestMajorPanelHIdx);
        for (size_t k = 0; k < bestMajorPanelHIdx; ++k) topBlocks.push_back(std::move(blocks[byY[k]]));
        for (size_t k = bestMajorPanelHIdx; k < byY.size(); ++k) botBlocks.push_back(std::move(blocks[byY[k]]));
        recursiveLayoutXYCut(std::move(topBlocks), medH, dividerXs, depth + 1, mergeWordTokens, ioNextRegionId, ioNextAlignedRowId, outResults);
        recursiveLayoutXYCut(std::move(botBlocks), medH, dividerXs, depth + 1, mergeWordTokens, ioNextRegionId, ioNextAlignedRowId, outResults);
        return;
    }

    if (vSplit.valid) {
        // 3. 当前面板内部存在有效垂直分栏走廊：切分左右分栏
        std::vector<AtomicLayoutBlock> leftCol;
        std::vector<AtomicLayoutBlock> rightCol;
        leftCol.reserve(blocks.size());
        rightCol.reserve(blocks.size());
        for (auto& b : blocks) {
            if (b.cx() < vSplit.splitX) {
                leftCol.push_back(std::move(b));
            } else {
                rightCol.push_back(std::move(b));
            }
        }
        recursiveLayoutXYCut(std::move(leftCol), medH, dividerXs, depth + 1, mergeWordTokens, ioNextRegionId, ioNextAlignedRowId, outResults);
        recursiveLayoutXYCut(std::move(rightCol), medH, dividerXs, depth + 1, mergeWordTokens, ioNextRegionId, ioNextAlignedRowId, outResults);
        return;
    }

    size_t bestHIdx = 0;
    float bestHScore = 0.0f;
    const float minHGap = (std::max)(4.0f, medH * 0.35f);

    for (size_t i = 0; i + 1 < byY.size(); ++i) {
        const float hGap = blocks[byY[i + 1]].top() - prefixMaxBottom[i];
        if (hGap < minHGap) continue;

        std::vector<AtomicLayoutBlock> topPart;
        std::vector<AtomicLayoutBlock> botPart;
        topPart.reserve(i + 1);
        botPart.reserve(byY.size() - (i + 1));
        for (size_t k = 0; k <= i; ++k) topPart.push_back(blocks[byY[k]]);
        for (size_t k = i + 1; k < byY.size(); ++k) botPart.push_back(blocks[byY[k]]);

        const auto topV = findBestLayoutVerticalSplit(topPart, medH, dividerXs, mergeWordTokens);
        const auto botV = findBestLayoutVerticalSplit(botPart, medH, dividerXs, mergeWordTokens);
        bool effectiveTopSplit = topV.valid;
        bool effectiveBotSplit = botV.valid;

        if (effectiveBotSplit && !topPart.empty()) {
            float maxTopCy = -FLT_MAX;
            for (const auto& b : topPart) maxTopCy = (std::max)(maxTopCy, b.cy());
            std::vector<AtomicLayoutBlock> botWithBoundary = botPart;
            for (const auto& b : topPart) {
                if (maxTopCy - b.cy() <= medH * 0.70f) botWithBoundary.push_back(b);
            }
            if (findBestLayoutVerticalSplit(botWithBoundary, medH, dividerXs, mergeWordTokens).valid) {
                effectiveBotSplit = false;
            }
        }
        if (effectiveTopSplit && !botPart.empty()) {
            float minBotCy = FLT_MAX;
            for (const auto& b : botPart) minBotCy = (std::min)(minBotCy, b.cy());
            std::vector<AtomicLayoutBlock> topWithBoundary = topPart;
            for (const auto& b : botPart) {
                if (b.cy() - minBotCy <= medH * 0.70f) topWithBoundary.push_back(b);
            }
            if (findBestLayoutVerticalSplit(topWithBoundary, medH, dividerXs, mergeWordTokens).valid) {
                effectiveTopSplit = false;
            }
        }

        if (effectiveTopSplit || effectiveBotSplit) {
            const float score = hGap * 2.0f +
                                (effectiveTopSplit ? topV.score : 0.0f) +
                                (effectiveBotSplit ? botV.score : 0.0f);
            if (score > bestHScore) {
                bestHScore = score;
                bestHIdx = i + 1;
            }
        }
    }

    if (bestHIdx > 0 && bestHIdx < byY.size()) {
        std::vector<AtomicLayoutBlock> topBlocks;
        std::vector<AtomicLayoutBlock> botBlocks;
        topBlocks.reserve(bestHIdx);
        botBlocks.reserve(byY.size() - bestHIdx);
        for (size_t k = 0; k < bestHIdx; ++k) topBlocks.push_back(std::move(blocks[byY[k]]));
        for (size_t k = bestHIdx; k < byY.size(); ++k) botBlocks.push_back(std::move(blocks[byY[k]]));
        recursiveLayoutXYCut(std::move(topBlocks), medH, dividerXs, depth + 1, mergeWordTokens, ioNextRegionId, ioNextAlignedRowId, outResults);
        recursiveLayoutXYCut(std::move(botBlocks), medH, dividerXs, depth + 1, mergeWordTokens, ioNextRegionId, ioNextAlignedRowId, outResults);
        return;
    }

    clusterAlignedRowsInRegion(
        std::move(blocks), ioNextRegionId++, ioNextAlignedRowId, mergeWordTokens, outResults);
}

struct LuminanceStats {
    int p10 = 0;
    int p50 = 128;
    int p90 = 255;
    int p95 = 255;
};

LuminanceStats computeLuminancePercentiles(const cv::Mat& image) noexcept {
    LuminanceStats stats{};
    if (image.empty()) return stats;

    cv::Mat gray;
    if (image.channels() == 1) {
        gray = image;
    } else if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else if (image.channels() == 4) {
        cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    } else {
        return stats;
    }

    cv::Mat sample = gray;
    if (gray.cols > 320 || gray.rows > 320) {
        const double sc = 320.0 / static_cast<double>((std::max)(gray.cols, gray.rows));
        cv::resize(gray, sample, cv::Size(), sc, sc, cv::INTER_AREA);
    }

    std::array<uint32_t, 256> hist{};
    uint32_t total = 0;
    for (int r = 0; r < sample.rows; ++r) {
        const uint8_t* ptr = sample.ptr<uint8_t>(r);
        for (int c = 0; c < sample.cols; ++c) {
            ++hist[ptr[c]];
            ++total;
        }
    }
    if (total == 0) return stats;

    auto percentileAt = [&](double q) -> int {
        const uint32_t target = static_cast<uint32_t>(std::round(static_cast<double>(total) * q));
        uint32_t acc = 0;
        for (int i = 0; i < 256; ++i) {
            acc += hist[static_cast<size_t>(i)];
            if (acc >= target) return i;
        }
        return 255;
    };

    stats.p10 = percentileAt(0.10);
    stats.p50 = percentileAt(0.50);
    stats.p90 = percentileAt(0.90);
    stats.p95 = percentileAt(0.95);
    return stats;
}

size_t scoreRecognizedTokensQuality(const std::vector<OcrWordToken>& tokens) noexcept {
    size_t score = 0;
    for (const auto& t : tokens) {
        for (wchar_t ch : t.text) {
            if (ch <= L' ') continue;
            if (isCjkOrFullwidth(ch) || isAsciiAlpha(ch) || isAsciiDigit(ch)) {
                score += 3;
            } else {
                score += 1;
            }
        }
    }
    return score;
}

bool runWinOcrOnPreparedMat(
    wmo::OcrEngine const& engine,
    const cv::Mat& srcImg,
    int maxDim,
    std::vector<OcrWordToken>& outTokens,
    float& outScaleFactor,
    int& outBorderPad) {
    outTokens.clear();
    outBorderPad = 0;
    outScaleFactor = 1.0f;

    cv::Mat prepared = srcImg;
    const int effectiveWinMaxDim = (std::min)(maxDim, 1680);
    if (srcImg.cols >= 8 && srcImg.rows >= 8 && srcImg.cols + 24 < effectiveWinMaxDim && srcImg.rows + 24 < effectiveWinMaxDim) {
        outBorderPad = 12;
        cv::copyMakeBorder(srcImg, prepared, outBorderPad, outBorderPad, outBorderPad, outBorderPad, cv::BORDER_REPLICATE);
        const int maxPreparedSide = (std::max)(prepared.cols, prepared.rows);
        if (maxPreparedSide < 1280) {
            const float maxAllowedScale = static_cast<float>(effectiveWinMaxDim) / static_cast<float>((std::max)(1, maxPreparedSide));
            const float desiredScale = (std::min)(2.0f, std::floor(maxAllowedScale * 10.0f) * 0.1f);
            if (desiredScale >= 1.35f) {
                const int upW = (std::min)(effectiveWinMaxDim, (std::max)(1, static_cast<int>(std::round(static_cast<float>(prepared.cols) * desiredScale))));
                const int upH = (std::min)(effectiveWinMaxDim, (std::max)(1, static_cast<int>(std::round(static_cast<float>(prepared.rows) * desiredScale))));
                outScaleFactor = static_cast<float>(upW) / static_cast<float>(prepared.cols);
                cv::resize(prepared, prepared, cv::Size(upW, upH), 0.0, 0.0, cv::INTER_CUBIC);
                // 温和反锐化掩模 (Unsharp Mask)：收紧小尺寸低分屏灰字与浅色胶囊数字边缘
                cv::Mat blurred;
                cv::GaussianBlur(prepared, blurred, cv::Size(0, 0), 0.85);
                cv::addWeighted(prepared, 1.24, blurred, -0.24, 0.0, prepared);
            }
        }
    } else if (srcImg.cols > effectiveWinMaxDim) {
        outScaleFactor = static_cast<float>(effectiveWinMaxDim) / static_cast<float>(srcImg.cols);
        const int targetW = (std::min)(effectiveWinMaxDim, (std::max)(1, static_cast<int>(std::floor(srcImg.cols * outScaleFactor))));
        const int targetH = (std::max)(1, static_cast<int>(std::floor(srcImg.rows * outScaleFactor)));
        outScaleFactor = static_cast<float>(targetW) / static_cast<float>(srcImg.cols);
        cv::resize(srcImg, prepared, cv::Size(targetW, targetH), 0.0, 0.0, cv::INTER_AREA);
    }

    if (prepared.rows <= maxDim) {
        wgi::SoftwareBitmap bitmap = matToBitmap(prepared);
        if (!bitmap) return false;
        wmo::OcrResult result = engine.RecognizeAsync(bitmap).get();
        for (auto const& line : result.Lines()) {
            for (auto const& w : line.Words()) {
                std::wstring wt(w.Text().c_str());
                if (wt.empty()) continue;
                wf::Rect br = w.BoundingRect();
                outTokens.push_back(OcrWordToken{std::move(wt), br.X, br.Y, br.Width, br.Height});
            }
        }
    } else {
        const int overlap = (std::min)(96, maxDim / 8);
        const int step = (std::max)(64, maxDim - overlap);
        for (int tileTop = 0; tileTop < prepared.rows; tileTop += step) {
            const int tileH = (std::min)(maxDim, prepared.rows - tileTop);
            if (tileH < 8) break;
            cv::Mat tile = prepared(cv::Rect(0, tileTop, prepared.cols, tileH));
            wgi::SoftwareBitmap bitmap = matToBitmap(tile);
            if (!bitmap) return false;
            const size_t prevTokenCount = outTokens.size();
            wmo::OcrResult result = engine.RecognizeAsync(bitmap).get();
            for (auto const& line : result.Lines()) {
                for (auto const& w : line.Words()) {
                    std::wstring wt(w.Text().c_str());
                    if (wt.empty()) continue;
                    wf::Rect br = w.BoundingRect();
                    const float globalY = br.Y + static_cast<float>(tileTop);
                    if (tileTop > 0 && br.Y < static_cast<float>(overlap) + br.Height) {
                        bool duplicate = false;
                        const float candCy = globalY + br.Height * 0.5f;
                        for (size_t k = 0; k < prevTokenCount; ++k) {
                            const auto& existing = outTokens[k];
                            if (existing.y + existing.h < static_cast<float>(tileTop) - 4.0f) continue;
                            if (existing.text == wt &&
                                std::abs(existing.cy() - candCy) <= (std::max)(6.0f, br.Height * 0.6f) &&
                                std::abs(existing.x - br.X) <= (std::max)(8.0f, br.Width * 0.5f)) {
                                duplicate = true;
                                break;
                            }
                        }
                        if (duplicate) continue;
                    }
                    outTokens.push_back(OcrWordToken{std::move(wt), br.X, globalY, br.Width, br.Height});
                }
            }
            if (tileTop + tileH >= prepared.rows) break;
        }
    }
    return true;
}

}  // namespace

std::vector<std::string> OcrEngine::splitUtf8Codepoints(std::string_view utf8) {
    std::vector<std::string> out;
    if (utf8.empty()) return out;
    out.reserve(utf8.size());
    size_t cursor = 0;
    while (cursor < utf8.size()) {
        const size_t start = cursor;
        (void)decodeSingleUtf8Codepoint(utf8, cursor);
        out.emplace_back(utf8.substr(start, cursor - start));
    }
    return out;
}

size_t OcrEngine::countUtf8Codepoints(std::string_view utf8) noexcept {
    size_t count = 0;
    size_t cursor = 0;
    while (cursor < utf8.size()) {
        (void)decodeSingleUtf8Codepoint(utf8, cursor);
        ++count;
    }
    return count;
}

std::string OcrEngine::sliceUtf8Codepoints(std::string_view utf8, size_t startIndex, size_t endExclusive) {
    if (utf8.empty() || startIndex >= endExclusive) return {};
    size_t idx = 0;
    size_t cursor = 0;
    size_t byteStart = utf8.size();
    size_t byteEnd = utf8.size();

    while (cursor < utf8.size()) {
        if (idx == startIndex) byteStart = cursor;
        if (idx == endExclusive) {
            byteEnd = cursor;
            break;
        }
        (void)decodeSingleUtf8Codepoint(utf8, cursor);
        ++idx;
    }
    if (idx <= startIndex) return {};
    if (byteEnd <= byteStart) return {};
    return std::string(utf8.substr(byteStart, byteEnd - byteStart));
}

std::vector<cv::Rect> OcrEngine::buildProportionalCharBoxes(std::string_view utf8, const cv::Rect& bounds) {
    std::vector<cv::Rect> boxes;
    if (utf8.empty()) return boxes;

    std::vector<float> weights;
    weights.reserve(utf8.size());
    size_t cursor = 0;
    float totalWeight = 0.0f;
    while (cursor < utf8.size()) {
        const uint32_t cp = decodeSingleUtf8Codepoint(utf8, cursor);
        const float w = estimateGlyphVisualWeight(cp);
        weights.push_back(w);
        totalWeight += w;
    }
    if (weights.empty()) return boxes;

    const float safeTotalW = (totalWeight > 1e-4f) ? totalWeight : static_cast<float>(weights.size());
    const float baseLeft = static_cast<float>(bounds.x);
    const float totalSpanPx = static_cast<float>((std::max)(1, bounds.width));
    const int boxY = bounds.y;
    const int boxH = (std::max)(1, bounds.height);

    boxes.reserve(weights.size());
    float accWeight = 0.0f;
    int prevRight = bounds.x;
    const int maxRight = bounds.x + (std::max)(1, bounds.width);

    for (size_t i = 0; i < weights.size(); ++i) {
        accWeight += weights[i];
        const int x0 = (i == 0) ? bounds.x : prevRight;
        int x1 = (i + 1 == weights.size())
            ? maxRight
            : static_cast<int>(std::round(baseLeft + totalSpanPx * (accWeight / safeTotalW)));
        x1 = std::clamp(x1, x0, maxRight);
        const int cellW = (std::max)(1, x1 - x0);
        boxes.emplace_back(x0, boxY, cellW, boxH);
        prevRight = x1;
    }
    return boxes;
}

bool OcrEngine::isDarkBackgroundImage(const cv::Mat& image) noexcept {
    if (image.empty()) return false;
    const auto st = computeLuminancePercentiles(image);
    return (st.p50 < 116) && (st.p95 >= st.p50 + 16);
}

cv::Mat OcrEngine::enhanceImageForRecognition(const cv::Mat& image, bool* outInverted) {
    if (outInverted) *outInverted = false;
    if (image.empty()) return {};

    const auto lum = computeLuminancePercentiles(image);
    const bool darkBg = (lum.p50 < 116) && (lum.p95 >= lum.p50 + 16);

    cv::Mat bgr;
    if (image.channels() == 3) {
        bgr = image.clone();
    } else if (image.channels() == 4) {
        cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
    } else if (image.channels() == 1) {
        cv::cvtColor(image, bgr, cv::COLOR_GRAY2BGR);
    } else {
        return image.clone();
    }

    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

    if (darkBg) {
        // 针对黑底白字 / IDE 暗色语法高亮 / 低对比暗色毛玻璃：
        // 融合三通道最大色偏信号，避免暗蓝/暗红关键字在普通灰度化后与黑底融为一体
        cv::Mat channels[3];
        cv::split(bgr, channels);
        const double medB = cv::mean(channels[0])[0];
        const double medG = cv::mean(channels[1])[0];
        const double medR = cv::mean(channels[2])[0];

        cv::Mat diffB, diffG, diffR, maxColorDiff;
        cv::absdiff(channels[0], cv::Scalar(medB), diffB);
        cv::absdiff(channels[1], cv::Scalar(medG), diffG);
        cv::absdiff(channels[2], cv::Scalar(medR), diffR);
        cv::max(diffB, diffG, maxColorDiff);
        cv::max(maxColorDiff, diffR, maxColorDiff);

        cv::Mat boostedDark = cv::max(gray, maxColorDiff);
        auto clahe = cv::createCLAHE(2.4, cv::Size(8, 8));
        cv::Mat localEnhanced;
        clahe->apply(boostedDark, localEnhanced);

        // 反相为标准白底深墨字迹，完美契合 Windows.Media.Ocr 二值化先验
        cv::Mat inverted = 255 - localEnhanced;
        cv::Mat outBgr;
        cv::cvtColor(inverted, outBgr, cv::COLOR_GRAY2BGR);
        if (outInverted) *outInverted = true;
        return outBgr;
    }

    // 亮底但局部对比度偏低（如浅色毛玻璃悬浮窗、水印灰字）：执行温和 CLAHE 拉升笔画锐度
    if ((lum.p90 - lum.p10) < 95) {
        auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
        cv::Mat enhancedGray;
        clahe->apply(gray, enhancedGray);
        cv::Mat outBgr;
        cv::cvtColor(enhancedGray, outBgr, cv::COLOR_GRAY2BGR);
        return outBgr;
    }

    return bgr;
}

std::vector<OcrQrResult> OcrEngine::detectQrCodes(const cv::Mat& image) {
    std::vector<OcrQrResult> qrResults;
    if (image.empty() || image.cols < 16 || image.rows < 16) {
        return qrResults;
    }

    try {
        cv::Mat gray;
        if (image.channels() == 1) {
            gray = image;
        } else if (image.channels() == 3) {
            cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
        } else if (image.channels() == 4) {
            cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
        } else {
            return qrResults;
        }

        float invScale = 1.0f;
        cv::Mat work = gray;
        const int maxSide = (std::max)(gray.cols, gray.rows);
        const int minSide = (std::min)(gray.cols, gray.rows);
        if (maxSide > 1280) {
            const float sc = 1120.0f / static_cast<float>(maxSide);
            cv::resize(gray, work, cv::Size(), sc, sc, cv::INTER_AREA);
            invScale = 1.0f / sc;
        } else if (minSide < 140) {
            cv::resize(gray, work, cv::Size(), 2.0, 2.0, cv::INTER_CUBIC);
            invScale = 0.5f;
        }

        cv::QRCodeDetector detector;
        auto appendDecoded = [&](const std::string& text, const cv::Mat& points, int rowIdx, float passInvScale,
                                 const std::string& format = "QR_CODE") {
            if (text.empty()) return;
            for (const auto& existing : qrResults) {
                if (existing.text == text) return;
            }

            OcrQrResult item;
            item.text = text;
            item.format = format.empty() ? "QR_CODE" : format;

            float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
            bool validPts = false;
            if (rowIdx >= 0 && points.rows > rowIdx && points.cols == 4 && points.type() == CV_32FC2) {
                const auto* rowPts = points.ptr<cv::Point2f>(rowIdx);
                for (size_t k = 0; k < 4; ++k) {
                    const float px = std::clamp(rowPts[k].x * passInvScale, 0.0f, static_cast<float>(image.cols));
                    const float py = std::clamp(rowPts[k].y * passInvScale, 0.0f, static_cast<float>(image.rows));
                    item.corners[k] = {px, py};
                    minX = (std::min)(minX, px);
                    minY = (std::min)(minY, py);
                    maxX = (std::max)(maxX, px);
                    maxY = (std::max)(maxY, py);
                }
                validPts = (maxX > minX) && (maxY > minY);
            } else if (points.total() == 4 && points.type() == CV_32FC2) {
                const auto* rawPts = points.ptr<cv::Point2f>();
                for (size_t k = 0; k < 4; ++k) {
                    const float px = std::clamp(rawPts[k].x * passInvScale, 0.0f, static_cast<float>(image.cols));
                    const float py = std::clamp(rawPts[k].y * passInvScale, 0.0f, static_cast<float>(image.rows));
                    item.corners[k] = {px, py};
                    minX = (std::min)(minX, px);
                    minY = (std::min)(minY, py);
                    maxX = (std::max)(maxX, px);
                    maxY = (std::max)(maxY, py);
                }
                validPts = (maxX > minX) && (maxY > minY);
            }

            if (validPts) {
                const int rx = std::clamp(static_cast<int>(std::floor(minX)), 0, image.cols - 1);
                const int ry = std::clamp(static_cast<int>(std::floor(minY)), 0, image.rows - 1);
                const int rw = std::clamp(static_cast<int>(std::ceil(maxX)) - rx, 1, image.cols - rx);
                const int rh = std::clamp(static_cast<int>(std::ceil(maxY)) - ry, 1, image.rows - ry);
                item.boundingBox = cv::Rect(rx, ry, rw, rh);
            } else {
                item.boundingBox = cv::Rect(0, 0, image.cols, image.rows);
            }
            qrResults.push_back(std::move(item));
        };

        auto runPass = [&](const cv::Mat& passGray, float passInvScale, bool tryMulti) {
            if (tryMulti) {
                std::vector<std::string> decoded;
                cv::Mat points;
                try {
                    (void)detector.detectAndDecodeMulti(passGray, decoded, points);
                } catch (...) {
                }
                const int numCodes = static_cast<int>(decoded.size());
                for (int i = 0; i < numCodes; ++i) {
                    appendDecoded(decoded[static_cast<size_t>(i)], points, i, passInvScale);
                }
            }
            if (qrResults.empty()) {
                cv::Mat singlePoints;
                std::string singleDecoded;
                try {
                    singleDecoded = detector.detectAndDecode(passGray, singlePoints);
                } catch (...) {
                }
                appendDecoded(singleDecoded, singlePoints, 0, passInvScale);
            }
        };

        runPass(work, invScale, /*tryMulti=*/true);

        // 工业实拍/高分屏中占画面面积极小 (< 90px) 的倾斜二维码在 1x 像素下每模块不足 2.5px，
        // 仅在 1x 未检出时以 2x 三次插值放大配合单码强透视检测器补齐召回（并行于 OCR 零感知延迟）。
        if (qrResults.empty() && invScale >= 0.99f &&
            static_cast<int64_t>(work.cols) * static_cast<int64_t>(work.rows) <= 1600LL * 1600LL) {
            cv::Mat up2;
            cv::resize(work, up2, cv::Size(), 2.0, 2.0, cv::INTER_CUBIC);
            runPass(up2, invScale * 0.5f, /*tryMulti=*/false);
        }

        if (qrResults.empty() && isDarkBackgroundImage(image)) {
            cv::Mat invWork = 255 - work;
            runPass(invWork, invScale, /*tryMulti=*/true);
        }

        // 并联扫描一维商品/工业条形码 (OpenCV 4.12 原生 BarcodeDetector: EAN-13 / EAN-8 / UPC-A / UPC-E 等)
        try {
            cv::barcode::BarcodeDetector barcodeDetector;
            std::vector<std::string> barInfos;
            std::vector<std::string> barTypes;
            cv::Mat barPoints;
            bool barDecoded = barcodeDetector.detectAndDecodeWithType(work, barInfos, barTypes, barPoints);
            if (!barDecoded && work.cols >= 80 && work.rows >= 32 &&
                work.cols <= 960 && work.rows <= 640) {
                // 紧贴条形码裁剪的小图缺乏周边大面积留白时，结构张量定位器易漏检，补一次 <1ms 整图直解兜底
                cv::Mat wholePts(1, 4, CV_32FC2);
                auto* wp = wholePts.ptr<cv::Point2f>(0);
                wp[0] = cv::Point2f(0.0f, static_cast<float>(work.rows - 1));
                wp[1] = cv::Point2f(0.0f, 0.0f);
                wp[2] = cv::Point2f(static_cast<float>(work.cols - 1), 0.0f);
                wp[3] = cv::Point2f(static_cast<float>(work.cols - 1), static_cast<float>(work.rows - 1));
                barInfos.clear();
                barTypes.clear();
                if (barcodeDetector.decodeWithType(work, wholePts, barInfos, barTypes)) {
                    barPoints = wholePts;
                    barDecoded = true;
                }
            }
            if (barDecoded) {
                const size_t nBars = barInfos.size();
                for (size_t bi = 0; bi < nBars; ++bi) {
                    const std::string fmt = (bi < barTypes.size() && !barTypes[bi].empty())
                        ? barTypes[bi]
                        : std::string("BARCODE");
                    appendDecoded(barInfos[bi], barPoints, static_cast<int>(bi), invScale, fmt);
                }
            }
        } catch (...) {
        }
    } catch (const std::exception& e) {
        LOG_WARN("QR detection skipped due to exception: {}", e.what());
    }
    return qrResults;
}

OcrEngine& OcrEngine::instance() {
    static OcrEngine inst;
    return inst;
}

bool OcrEngine::isAvailable() const {
    return m_available || DeepOcrEngine::instance().isModelInstalled();
}

bool OcrEngine::initialize() {
    try {
        m_available = runInMtaSafe([]() -> bool {
            return static_cast<bool>(createEngine());
        });
        if (m_available) {
            LOG_INFO("OCR 引擎已就绪 (Windows.Media.Ocr)");
        } else {
            LOG_WARN("未检测到 OCR 语言包，OCR 功能不可用。可在系统设置中添加语言的“可选功能 → 光学字符识别”");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("OCR 引擎初始化异常: {}", e.what());
        m_available = false;
    }
    return isAvailable();
}

namespace {

struct CachedOcrEntry {
    uint64_t key = 0;
    int width = 0;
    int height = 0;
    bool hasQrChecked = false;
    OcrPageExtraction page;
};

constexpr size_t kMaxRecentOcrCacheEntries = 8;
std::mutex g_ocrCacheMutex;
std::vector<CachedOcrEntry> g_ocrRecentCache;

uint64_t computeImageOcrCacheKey(const cv::Mat& image, std::string_view effectiveRoute) noexcept {
    if (image.empty() || image.cols <= 0 || image.rows <= 0) return 0;
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) noexcept {
        h ^= v;
        h *= 1099511628211ULL;
    };
    mix(static_cast<uint64_t>(image.cols));
    mix(static_cast<uint64_t>(image.rows));
    mix(static_cast<uint64_t>(image.type()));
    for (unsigned char ch : effectiveRoute) {
        mix(ch);
    }

    const int elemBytes = static_cast<int>(image.elemSize());
    const int gridY = (std::min)(image.rows, 40);
    const int gridX = (std::min)(image.cols, 40);
    for (int gy = 0; gy < gridY; ++gy) {
        const int r = (gy * (image.rows - 1)) / (std::max)(1, gridY - 1);
        const uint8_t* rowPtr = image.ptr<uint8_t>(r);
        for (int gx = 0; gx < gridX; ++gx) {
            const int c = (gx * (image.cols - 1)) / (std::max)(1, gridX - 1);
            const uint8_t* px = rowPtr + static_cast<size_t>(c * elemBytes);
            uint64_t packed = 0;
            for (int b = 0; b < (std::min)(elemBytes, 4); ++b) {
                packed |= (static_cast<uint64_t>(px[b]) << (b * 8));
            }
            mix(packed);
        }
    }
    return h == 0 ? 1ULL : h;
}

bool lookupRecentOcrCache(uint64_t key, int w, int h, bool requireQr, OcrPageExtraction& outPage) {
    if (key == 0) return false;
    std::lock_guard<std::mutex> lock(g_ocrCacheMutex);
    for (size_t i = 0; i < g_ocrRecentCache.size(); ++i) {
        const auto& e = g_ocrRecentCache[i];
        if (e.key == key && e.width == w && e.height == h && (!requireQr || e.hasQrChecked)) {
            outPage = e.page;
            if (i + 1 < g_ocrRecentCache.size()) {
                CachedOcrEntry promoted = std::move(g_ocrRecentCache[i]);
                g_ocrRecentCache.erase(g_ocrRecentCache.begin() + static_cast<std::ptrdiff_t>(i));
                g_ocrRecentCache.push_back(std::move(promoted));
            }
            return true;
        }
    }
    return false;
}

void storeRecentOcrCache(uint64_t key, int w, int h, bool hasQrChecked, const OcrPageExtraction& page) {
    if (key == 0) return;
    std::lock_guard<std::mutex> lock(g_ocrCacheMutex);
    for (size_t i = 0; i < g_ocrRecentCache.size(); ++i) {
        if (g_ocrRecentCache[i].key == key && g_ocrRecentCache[i].width == w && g_ocrRecentCache[i].height == h) {
            if (!hasQrChecked && g_ocrRecentCache[i].hasQrChecked) {
                return;
            }
            g_ocrRecentCache.erase(g_ocrRecentCache.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (g_ocrRecentCache.size() >= kMaxRecentOcrCacheEntries) {
        g_ocrRecentCache.erase(g_ocrRecentCache.begin());
    }
    g_ocrRecentCache.push_back(CachedOcrEntry{key, w, h, hasQrChecked, page});
}

void clearRecentOcrCache() {
    std::lock_guard<std::mutex> lock(g_ocrCacheMutex);
    g_ocrRecentCache.clear();
}

} // namespace

void OcrEngine::shutdown() {
    clearRecentOcrCache();
    DeepOcrEngine::instance().shutdown();
    winrt::clear_factory_cache();
    LOG_INFO("OCR 引擎已关闭");
}

std::vector<OcrResult> OcrEngine::extractWithWindowsOcr(const cv::Mat& image) {
    if (isCurrentThreadSta()) {
        return runInMtaSafe([this, &image]() -> std::vector<OcrResult> {
            return extractWithWindowsOcr(image);
        });
    }

    std::vector<OcrResult> results;
    try {
        ensureApartment();
        wmo::OcrEngine engine = createEngine();
        if (!engine) {
            LOG_ERROR("OCR 不可用: 无语言包");
            return results;
        }

        const int maxDim = (std::max)(256, static_cast<int>(wmo::OcrEngine::MaxImageDimension()));
        const bool darkBg = isDarkBackgroundImage(image);

        std::vector<OcrWordToken> chosenTokens;
        float chosenScale = 1.0f;
        int chosenPad = 0;

        if (darkBg) {
            // 深底浅字 / 黑底代码截图：同时执行 CLAHE+反相增强路与原图路择优融合
            bool inverted = false;
            cv::Mat enhanced = enhanceImageForRecognition(image, &inverted);

            std::vector<OcrWordToken> enhTokens;
            float enhScale = 1.0f;
            int enhPad = 0;
            runWinOcrOnPreparedMat(engine, enhanced, maxDim, enhTokens, enhScale, enhPad);

            std::vector<OcrWordToken> rawTokens;
            float rawScale = 1.0f;
            int rawPad = 0;
            if (!runWinOcrOnPreparedMat(engine, image, maxDim, rawTokens, rawScale, rawPad)) {
                LOG_ERROR("OCR: 不支持的图像通道数: {}", image.channels());
                return results;
            }

            if (scoreRecognizedTokensQuality(enhTokens) >= scoreRecognizedTokensQuality(rawTokens)) {
                chosenTokens = std::move(enhTokens);
                chosenScale = enhScale;
                chosenPad = enhPad;
            } else {
                chosenTokens = std::move(rawTokens);
                chosenScale = rawScale;
                chosenPad = rawPad;
            }
        } else {
            if (!runWinOcrOnPreparedMat(engine, image, maxDim, chosenTokens, chosenScale, chosenPad)) {
                LOG_ERROR("OCR: 不支持的图像通道数: {}", image.channels());
                return results;
            }
            // 若亮底原图未检出文字但存在低对比度毛玻璃特征，自动补跑一次 CLAHE 增强兜底
            if (chosenTokens.empty()) {
                const auto lum = computeLuminancePercentiles(image);
                if ((lum.p90 - lum.p10) < 95) {
                    cv::Mat enhanced = enhanceImageForRecognition(image);
                    std::vector<OcrWordToken> enhTokens;
                    float enhScale = 1.0f;
                    int enhPad = 0;
                    if (runWinOcrOnPreparedMat(engine, enhanced, maxDim, enhTokens, enhScale, enhPad) &&
                        !enhTokens.empty()) {
                        chosenTokens = std::move(enhTokens);
                        chosenScale = enhScale;
                        chosenPad = enhPad;
                    }
                }
            }
        }

        if (chosenTokens.empty()) {
            LOG_INFO("OCR 完成, 识别行数: {}", results.size());
            return results;
        }

        results = assembleLinesWithXYCut(
            std::move(chosenTokens), chosenScale, chosenPad, image.cols, image.rows, image);

        LOG_INFO("OCR 完成, 识别行数: {}", results.size());
    } catch (winrt::hresult_error const& e) {
        LOG_ERROR("OCR WinRT 错误: 0x{:08X} {}",
                  static_cast<uint32_t>(e.code()),
                  tools3000::core::WinUtils::wstringToUtf8(e.message().c_str()));
    } catch (const std::exception& e) {
        LOG_ERROR("OCR 异常: {}", e.what());
    }
    return results;
}

std::vector<OcrResult> OcrEngine::extractText(const cv::Mat& image) {
    tools3000::core::TraceId::Scope scope;
    if (image.empty()) {
        LOG_WARN("OCR 输入图像为空");
        return {};
    }

    std::string enginePref = "auto";
    try {
        enginePref = toLowerAsciiCopy(
            tools3000::core::ConfigManager::instance().get<std::string>("/ocr/engine", "auto"));
    } catch (...) {
        enginePref = "auto";
    }

    const bool deepInstalled = DeepOcrEngine::instance().isModelInstalled();
    const bool allowDeepOcr = (enginePref == "auto" || enginePref == "ppocr" || enginePref == "ppocr_v4" || enginePref == "deep");
    std::string langPref = "auto";
    try {
        langPref = tools3000::core::ConfigManager::instance().get<std::string>("/ocr/language", "auto");
    } catch (...) {
    }
    const std::string tierTag = deepInstalled ? DeepOcrEngine::instance().effectiveModelTier() : "none";
    const std::string routeTag = ((allowDeepOcr && deepInstalled) ? ("deep:" + tierTag + ":") : "win:") + enginePref + ":" + langPref;
    const uint64_t cacheKey = computeImageOcrCacheKey(image, routeTag);

    OcrPageExtraction cached{};
    if (lookupRecentOcrCache(cacheKey, image.cols, image.rows, /*requireQr=*/false, cached)) {
        return cached.lines;
    }

    if (allowDeepOcr && deepInstalled) {
        auto deepResults = DeepOcrEngine::instance().extractText(image);
        if (!deepResults.empty() || enginePref == "ppocr_v4") {
            OcrPageExtraction entry;
            entry.lines = deepResults;
            entry.activeEngine = "ppocr_v4";
            storeRecentOcrCache(cacheKey, image.cols, image.rows, /*hasQrChecked=*/false, entry);
            return deepResults;
        }
    }

    auto winResults = extractWithWindowsOcr(image);
    {
        OcrPageExtraction entry;
        entry.lines = winResults;
        entry.activeEngine = "windows";
        storeRecentOcrCache(cacheKey, image.cols, image.rows, /*hasQrChecked=*/false, entry);
    }
    return winResults;
}

OcrPageExtraction OcrEngine::extractPage(const cv::Mat& image) {
    OcrPageExtraction page{};
    if (image.empty()) {
        return page;
    }

    std::string enginePref = "auto";
    try {
        enginePref = toLowerAsciiCopy(
            tools3000::core::ConfigManager::instance().get<std::string>("/ocr/engine", "auto"));
    } catch (...) {
        enginePref = "auto";
    }

    const bool deepInstalled = DeepOcrEngine::instance().isModelInstalled();
    const bool preferDeep = (enginePref == "auto" || enginePref == "ppocr" || enginePref == "ppocr_v4" || enginePref == "deep") &&
                            deepInstalled;
    std::string langPref = "auto";
    try {
        langPref = tools3000::core::ConfigManager::instance().get<std::string>("/ocr/language", "auto");
    } catch (...) {
    }
    const std::string tierTag = preferDeep ? DeepOcrEngine::instance().effectiveModelTier() : "none";
    const std::string routeTag = (preferDeep ? ("deep:" + tierTag + ":") : "win:") + enginePref + ":" + langPref;
    const uint64_t cacheKey = computeImageOcrCacheKey(image, routeTag);

    if (lookupRecentOcrCache(cacheKey, image.cols, image.rows, /*requireQr=*/true, page)) {
        return page;
    }

    // 若文字行已在缓存中命中（如刚执行过 extractText），仅补齐极速 QR 扫描即可 0ms 复用文字结果
    OcrPageExtraction textOnlyCached{};
    if (lookupRecentOcrCache(cacheKey, image.cols, image.rows, /*requireQr=*/false, textOnlyCached)) {
        textOnlyCached.qrCodes = detectQrCodes(image);
        storeRecentOcrCache(cacheKey, image.cols, image.rows, /*hasQrChecked=*/true, textOnlyCached);
        return textOnlyCached;
    }

    auto qrFuture = std::async(std::launch::async, [&image]() {
        return detectQrCodes(image);
    });

    if (preferDeep) {
        page.lines = DeepOcrEngine::instance().extractText(image);
        if (!page.lines.empty() || enginePref == "ppocr_v4") {
            page.qrCodes = qrFuture.get();
            page.activeEngine = "ppocr_v4";
            storeRecentOcrCache(cacheKey, image.cols, image.rows, /*hasQrChecked=*/true, page);
            return page;
        }
    }

    page.lines = extractWithWindowsOcr(image);
    page.qrCodes = qrFuture.get();
    page.activeEngine = "windows";
    storeRecentOcrCache(cacheKey, image.cols, image.rows, /*hasQrChecked=*/true, page);
    return page;
}

std::string OcrEngine::recognizeToText(const cv::Mat& image) {
    const auto lines = extractText(image);
    std::string text;
    const OcrResult* prevLine = nullptr;
    for (auto const& r : lines) {
        if (r.text.empty()) continue;
        if (!text.empty() && prevLine != nullptr) {
            if (!shouldSeamlessJoinSoftWrappedJsonLine(text, r.text, &prevLine->box, &r.box)) {
                text += '\n';
            }
        }
        text += r.text;
        prevLine = &r;
    }
    return text;
}

std::string OcrEngine::recognizeImageFile(const std::string& utf8Path) {
    constexpr std::uintmax_t kMaxOcrImageFileBytes = 64ULL * 1024ULL * 1024ULL;
    cv::Mat img;
    try {
        const std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(utf8Path);
        std::error_code ec;
        const std::filesystem::path fsPath(wpath);
        if (!std::filesystem::is_regular_file(fsPath, ec) || ec) {
            LOG_WARN("OCR: 无法加载图片 {}", utf8Path);
            return {};
        }
        const std::uintmax_t fileBytes = std::filesystem::file_size(fsPath, ec);
        if (ec || fileBytes == 0 || fileBytes > kMaxOcrImageFileBytes) {
            LOG_WARN("OCR image file skipped due to invalid or oversized file size: bytes={}",
                     ec ? 0ULL : static_cast<unsigned long long>(fileBytes));
            return {};
        }
        std::ifstream f(fsPath, std::ios::binary);
        if (f) {
            std::vector<uint8_t> buf(static_cast<size_t>(fileBytes));
            f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            if (static_cast<size_t>(f.gcount()) == buf.size() && !buf.empty()) {
                img = cv::imdecode(buf, cv::IMREAD_COLOR);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("OCR 读取图片失败: {} ({})", utf8Path, e.what());
    }
    if (img.empty()) {
        LOG_WARN("OCR: 无法加载图片 {}", utf8Path);
        return {};
    }
    return recognizeToText(img);
}

std::vector<OcrResult> OcrEngine::assembleRecognizedBlocksWithLayout(
    std::vector<OcrResult> rawBlocks,
    const cv::Mat& sourceImage) {
    if (rawBlocks.empty()) return {};

    std::vector<AtomicLayoutBlock> atomicBlocks;
    atomicBlocks.reserve(rawBlocks.size() * 2);

    auto pushAtomicSegment = [&](const std::string& segUtf8,
                                 const std::vector<cv::Rect>& segCharBoxes,
                                 const cv::Rect& fallbackBox,
                                 float conf) {
        if (segUtf8.empty()) return;
        std::wstring w = tools3000::core::WinUtils::utf8ToWstring(segUtf8);
        if (w.empty()) return;

        cv::Rect tightBox = fallbackBox;
        if (!segCharBoxes.empty()) {
            int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
            for (const auto& cb : segCharBoxes) {
                x1 = (std::min)(x1, cb.x);
                y1 = (std::min)(y1, cb.y);
                x2 = (std::max)(x2, cb.x + (std::max)(1, cb.width));
                y2 = (std::max)(y2, cb.y + (std::max)(1, cb.height));
            }
            if (x1 <= x2 && y1 <= y2) {
                tightBox = cv::Rect(x1, y1, (std::max)(1, x2 - x1), (std::max)(1, y2 - y1));
            }
        }
        if (tightBox.width <= 0) tightBox.width = 1;
        if (tightBox.height <= 0) tightBox.height = 16;

        AtomicLayoutBlock ab;
        ab.wtext = std::move(w);
        ab.utf8 = segUtf8;
        ab.confidence = conf;
        ab.box = tightBox;
        const size_t expectedCps = countUtf8Codepoints(segUtf8);
        ab.charBoxes = (segCharBoxes.size() == expectedCps)
            ? segCharBoxes
            : buildProportionalCharBoxes(segUtf8, tightBox);
        atomicBlocks.push_back(std::move(ab));
    };

    for (auto& blk : rawBlocks) {
        if (blk.text.empty()) continue;
        const auto cps = splitUtf8Codepoints(blk.text);
        if (cps.empty()) continue;

        if (blk.charBoxes.size() == cps.size()) {
            const int blkH = (std::max)(10, blk.box.height);
            const int splitGapPx = (std::max)(26, static_cast<int>(std::round(blkH * 2.15f)));
            std::string curText;
            std::vector<cv::Rect> curBoxes;
            int lastNonSpaceRight = -1;

            auto flushCur = [&]() {
                // 去除段首尾纯空格
                while (!curBoxes.empty() && !curText.empty() && curText.front() == ' ') {
                    curText.erase(curText.begin());
                    curBoxes.erase(curBoxes.begin());
                }
                while (!curBoxes.empty() && !curText.empty() && curText.back() == ' ') {
                    curText.pop_back();
                    curBoxes.pop_back();
                }
                if (!curText.empty()) {
                    pushAtomicSegment(curText, curBoxes, blk.box, blk.confidence);
                }
                curText.clear();
                curBoxes.clear();
                lastNonSpaceRight = -1;
            };

            for (size_t i = 0; i < cps.size(); ++i) {
                if (cps[i] == "\t") {
                    flushCur();
                    continue;
                }
                const cv::Rect& cb = blk.charBoxes[i];
                if (cps[i] != " ") {
                    if (lastNonSpaceRight >= 0 && (cb.x - lastNonSpaceRight) > splitGapPx) {
                        flushCur();
                    }
                    lastNonSpaceRight = cb.x + (std::max)(1, cb.width);
                }
                curText += cps[i];
                curBoxes.push_back(cb);
            }
            flushCur();
        } else {
            pushAtomicSegment(blk.text, {}, blk.box, blk.confidence);
        }
    }

    if (atomicBlocks.empty()) return {};

    const float medH = computeMedianBlockHeight(atomicBlocks);
    const std::vector<float> dividerXs = detectVerticalUiDividers(sourceImage, atomicBlocks);

    std::vector<OcrResult> results;
    results.reserve(atomicBlocks.size());
    int nextRegionId = 0;
    int nextAlignedRowId = 0;
    recursiveLayoutXYCut(
        std::move(atomicBlocks), medH, dividerXs, 0, /*mergeWordTokens=*/false, nextRegionId, nextAlignedRowId, results);
    return results;
}

std::vector<OcrResult> OcrEngine::splitIntoAtomicBlocks(const std::vector<OcrResult>& lines) {
    std::vector<OcrResult> out;
    out.reserve(lines.size() * 2);

    for (const auto& ln : lines) {
        if (ln.text.empty()) continue;
        if (ln.text.find('\t') == std::string::npos) {
            OcrResult copy = ln;
            const size_t cpCount = countUtf8Codepoints(copy.text);
            if (copy.charBoxes.size() != cpCount) {
                copy.charBoxes = buildProportionalCharBoxes(copy.text, copy.box);
            }
            out.push_back(std::move(copy));
            continue;
        }

        const auto cps = splitUtf8Codepoints(ln.text);
        std::vector<cv::Rect> cboxes = (ln.charBoxes.size() == cps.size())
            ? ln.charBoxes
            : buildProportionalCharBoxes(ln.text, ln.box);

        std::string segText;
        std::vector<cv::Rect> segBoxes;
        auto flushSeg = [&]() {
            while (!segBoxes.empty() && !segText.empty() && segText.front() == ' ') {
                segText.erase(segText.begin());
                segBoxes.erase(segBoxes.begin());
            }
            while (!segBoxes.empty() && !segText.empty() && segText.back() == ' ') {
                segText.pop_back();
                segBoxes.pop_back();
            }
            if (!segText.empty() && !segBoxes.empty()) {
                int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
                for (const auto& b : segBoxes) {
                    x1 = (std::min)(x1, b.x);
                    y1 = (std::min)(y1, b.y);
                    x2 = (std::max)(x2, b.x + (std::max)(1, b.width));
                    y2 = (std::max)(y2, b.y + (std::max)(1, b.height));
                }
                OcrResult item;
                item.text = std::move(segText);
                item.confidence = ln.confidence;
                item.box = cv::Rect(x1, y1, (std::max)(1, x2 - x1), (std::max)(1, y2 - y1));
                item.charBoxes = std::move(segBoxes);
                item.layoutRegionId = ln.layoutRegionId;
                item.alignedRowId = ln.alignedRowId;
                out.push_back(std::move(item));
            }
            segText.clear();
            segBoxes.clear();
        };

        for (size_t i = 0; i < cps.size(); ++i) {
            if (cps[i] == "\t") {
                flushSeg();
            } else {
                segText += cps[i];
                segBoxes.push_back(cboxes[i]);
            }
        }
        flushSeg();
    }
    return out;
}

std::vector<OcrResult> OcrEngine::assembleLinesWithXYCut(
    std::vector<OcrWordToken> tokens,
    float scaleFactor,
    int borderPad,
    int maxImageWidth,
    int maxImageHeight,
    const cv::Mat& sourceImage) {
    tokens.erase(
        std::remove_if(tokens.begin(), tokens.end(), [](const OcrWordToken& t) {
            return t.text.empty() || t.w < 0.0f || t.h < 0.0f;
        }),
        tokens.end());
    if (tokens.empty()) return {};

    const float safeScale = (scaleFactor > 0.0001f) ? scaleFactor : 1.0f;
    std::vector<AtomicLayoutBlock> wordBlocks;
    wordBlocks.reserve(tokens.size());
    for (const auto& tok : tokens) {
        std::string utf8 = tools3000::core::WinUtils::wstringToUtf8(tok.text);
        if (utf8.empty()) continue;
        const cv::Rect origRect = mapTokenBoxToOrigRect(
            tok.x, tok.y, tok.right(), tok.bottom(),
            safeScale, borderPad, maxImageWidth, maxImageHeight);
        AtomicLayoutBlock ab;
        ab.wtext = tok.text;
        ab.utf8 = std::move(utf8);
        ab.confidence = 1.0f;
        ab.box = origRect;
        ab.charBoxes = buildProportionalCharBoxes(ab.utf8, origRect);
        wordBlocks.push_back(std::move(ab));
    }
    if (wordBlocks.empty()) return {};

    const float medH = computeMedianBlockHeight(wordBlocks);
    const std::vector<float> dividerXs = detectVerticalUiDividers(sourceImage, wordBlocks);
    std::vector<OcrResult> results;
    results.reserve(wordBlocks.size());
    int nextRegionId = 0;
    int nextAlignedRowId = 0;
    recursiveLayoutXYCut(
        std::move(wordBlocks), medH, dividerXs, 0, /*mergeWordTokens=*/true, nextRegionId, nextAlignedRowId, results);
    return results;
}

bool OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
    std::string_view accumulatedText,
    std::string_view nextLineText,
    const cv::Rect* prevBox,
    const cv::Rect* nextBox) noexcept {
    if (accumulatedText.empty() || nextLineText.empty()) {
        return false;
    }

    // 1. 物理同栏邻接校验（避免把左栏底行与右栏顶行或跨越大幅留白的无关文本误粘）
    if (prevBox != nullptr && nextBox != nullptr &&
        prevBox->width > 0 && prevBox->height > 0 &&
        nextBox->width > 0 && nextBox->height > 0) {
        const int maxH = (std::max)(prevBox->height, nextBox->height);
        if (nextBox->y < prevBox->y - maxH / 2) {
            return false;
        }
        const int verticalGap = nextBox->y - (prevBox->y + prevBox->height);
        const int maxGap = (std::max)(22, static_cast<int>(std::lround(static_cast<float>(maxH) * 2.4f)));
        if (verticalGap > maxGap) {
            return false;
        }
        const int hOverlap = (std::min)(prevBox->x + prevBox->width, nextBox->x + nextBox->width) -
                             (std::max)(prevBox->x, nextBox->x);
        if (hOverlap <= 0) {
            return false;
        }
    }

    auto isJsonKeyStart = [](std::string_view s, size_t start) noexcept -> bool {
        if (start >= s.size() || s[start] != '"') return false;
        size_t j = start + 1;
        size_t keyLen = 0;
        while (j < s.size() && s[j] != '"' && s[j] != '\r' && s[j] != '\n') {
            const unsigned char ch = static_cast<unsigned char>(s[j]);
            if (ch < 0x20) return false;
            ++j;
            ++keyLen;
            if (keyLen > 96) return false;
        }
        if (keyLen == 0 || j >= s.size() || s[j] != '"') return false;
        size_t k = j + 1;
        while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
        return (k < s.size() && s[k] == ':');
    };

    // 2. 精确双引号状态机（跳过 \" / \\ 转义），仅当处于未闭合 JSON 字符串且具备 JSON 语法上下文时触发
    bool inString = false;
    bool escaped = false;
    bool hasJsonContext = false;
    bool lastOpenAfterJsonPunct = false;
    char lastNonSpaceOutside = '\0';

    for (size_t i = 0; i < accumulatedText.size(); ++i) {
        const char ch = accumulatedText[i];
        if (inString) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                inString = false;
                lastNonSpaceOutside = '"';
            }
            continue;
        }

        if (ch == '"') {
            if (!hasJsonContext && isJsonKeyStart(accumulatedText, i)) {
                hasJsonContext = true;
            }
            lastOpenAfterJsonPunct = (lastNonSpaceOutside == ':' ||
                                      lastNonSpaceOutside == ',' ||
                                      lastNonSpaceOutside == '[' ||
                                      lastNonSpaceOutside == '{');
            inString = true;
            escaped = false;
            continue;
        }

        if (ch == ':' && lastNonSpaceOutside == '"') {
            hasJsonContext = true;
        }
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            lastNonSpaceOutside = ch;
        }
    }

    if (inString && (hasJsonContext || lastOpenAfterJsonPunct)) {
        // 3. 防呆护栏：若前一行实际因 OCR 漏掉闭合引号，而 nextLineText 已直接以新的 JSON 键 `"Key":` 开头，严禁误合并
        size_t nextStart = 0;
        while (nextStart < nextLineText.size() &&
               (nextLineText[nextStart] == ' ' || nextLineText[nextStart] == '\t')) {
            ++nextStart;
        }
        if (nextStart < nextLineText.size() && !isJsonKeyStart(nextLineText, nextStart)) {
            return true;
        }
        return false;
    }

    // 4. 通用软折行续接扩展：长 URL (`https://...`) 与 Windows 本地/UNC 文件路径 (`C:\...` / `\\host\...`) 跨行折断
    {
        // 提取 accumulatedText 最后一行（最近一个换行符之后）
        size_t lastNl = accumulatedText.find_last_of("\r\n");
        std::string_view prevLine = (lastNl == std::string_view::npos)
            ? accumulatedText
            : accumulatedText.substr(lastNl + 1);
        while (!prevLine.empty() && (prevLine.back() == ' ' || prevLine.back() == '\t')) {
            prevLine.remove_suffix(1);
        }
        std::string_view nextTrim = nextLineText;
        while (!nextTrim.empty() && (nextTrim.front() == ' ' || nextTrim.front() == '\t')) {
            nextTrim.remove_prefix(1);
        }
        while (!nextTrim.empty() && (nextTrim.back() == ' ' || nextTrim.back() == '\t')) {
            nextTrim.remove_suffix(1);
        }
        if (prevLine.empty() || nextTrim.empty() || nextTrim.find('\t') != std::string_view::npos) {
            return false;
        }

        // 取 prevLine 末尾最后一个非空 token
        const size_t lastSpace = prevLine.find_last_of(" \t");
        const std::string_view prevTailToken = (lastSpace == std::string_view::npos)
            ? prevLine
            : prevLine.substr(lastSpace + 1);

        auto startsWithCaseInsensitive = [](std::string_view s, std::string_view prefix) noexcept -> bool {
            if (s.size() < prefix.size()) return false;
            for (size_t i = 0; i < prefix.size(); ++i) {
                const unsigned char a = static_cast<unsigned char>(s[i]);
                const unsigned char b = static_cast<unsigned char>(prefix[i]);
                const unsigned char la = (a >= 'A' && a <= 'Z') ? static_cast<unsigned char>(a + 32) : a;
                const unsigned char lb = (b >= 'A' && b <= 'Z') ? static_cast<unsigned char>(b + 32) : b;
                if (la != lb) return false;
            }
            return true;
        };

        auto isUrlChar = [](unsigned char c) noexcept -> bool {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '/' || c == ':' || c == '.' || c == '-' || c == '_' || c == '?' ||
                   c == '&' || c == '=' || c == '%' || c == '#' || c == '~' || c == '+' ||
                   c == '@' || c == '!' || c == '$' || c == ',' || c == ';' || c == '(' || c == ')';
        };

        // 4a. 长 URL 软折行（如 https://host/a/b/ + c/d?x=1），严禁合并两条独立的 URL
        const bool prevIsUrl =
            startsWithCaseInsensitive(prevTailToken, "https://") ||
            startsWithCaseInsensitive(prevTailToken, "http://");
        const bool nextStartsNewUrl =
            startsWithCaseInsensitive(nextTrim, "https://") ||
            startsWithCaseInsensitive(nextTrim, "http://");
        if (prevIsUrl && !nextStartsNewUrl && nextTrim.find(' ') == std::string_view::npos) {
            bool allUrlChars = true;
            bool nextHasUrlPunct = false;
            for (char ch : nextTrim) {
                const unsigned char uc = static_cast<unsigned char>(ch);
                if (!isUrlChar(uc)) {
                    allUrlChars = false;
                    break;
                }
                if (uc == '/' || uc == '?' || uc == '&' || uc == '=' || uc == '%' || uc == '#' || uc == '.') {
                    nextHasUrlPunct = true;
                }
            }
            const char tailCh = prevTailToken.back();
            const bool prevEndsWithUrlDelim =
                tailCh == '/' || tailCh == '?' || tailCh == '&' || tailCh == '=' ||
                tailCh == '%' || tailCh == '#' || tailCh == '-' || tailCh == '_';
            if (allUrlChars && (prevEndsWithUrlDelim || (prevTailToken.size() >= 28 && nextHasUrlPunct))) {
                return true;
            }
        }

        // 4b. Windows 本地/UNC 文件路径在反斜杠 `\` 后软折行（如 C:\repo\Tools3000\ + build\Tools3000.exe）
        const bool prevIsWinPath =
            (prevTailToken.size() >= 4 &&
             ((prevTailToken[0] >= 'A' && prevTailToken[0] <= 'Z') || (prevTailToken[0] >= 'a' && prevTailToken[0] <= 'z')) &&
             prevTailToken[1] == ':' && prevTailToken[2] == '\\') ||
            (prevTailToken.size() >= 4 && prevTailToken[0] == '\\' && prevTailToken[1] == '\\');
        const bool nextStartsNewWinPath =
            (nextTrim.size() >= 3 &&
             ((nextTrim[0] >= 'A' && nextTrim[0] <= 'Z') || (nextTrim[0] >= 'a' && nextTrim[0] <= 'z')) &&
             nextTrim[1] == ':' && nextTrim[2] == '\\') ||
            (nextTrim.size() >= 2 && nextTrim[0] == '\\' && nextTrim[1] == '\\');
        if (prevIsWinPath && prevTailToken.back() == '\\' && !nextStartsNewWinPath && !nextStartsNewUrl &&
            nextTrim.front() != '\\' && nextTrim.front() != '"') {
            return true;
        }
    }

    return false;
}

std::string OcrEngine::orderTextBlocksWithXYCut(
    const std::vector<OcrResult>& blocks,
    const char* lineEnding) {
    if (blocks.empty()) return {};
    const char* eol = (lineEnding != nullptr) ? lineEnding : "\r\n";

    const auto orderedLines = assembleRecognizedBlocksWithLayout(blocks);
    std::string combined;
    const OcrResult* prevLine = nullptr;
    for (size_t i = 0; i < orderedLines.size(); ++i) {
        if (orderedLines[i].text.empty()) continue;
        if (!combined.empty() && prevLine != nullptr) {
            if (!shouldSeamlessJoinSoftWrappedJsonLine(combined, orderedLines[i].text,
                                                      &prevLine->box, &orderedLines[i].box)) {
                combined += eol;
            }
        }
        combined += orderedLines[i].text;
        prevLine = &orderedLines[i];
    }
    return combined;
}

std::wstring OcrEngine::smartMergeParagraphs(const std::wstring& rawMultiLineText) {
    if (rawMultiLineText.empty()) return {};

    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos <= rawMultiLineText.size()) {
        const size_t nl = rawMultiLineText.find(L'\n', pos);
        std::wstring seg = (nl == std::wstring::npos)
            ? rawMultiLineText.substr(pos)
            : rawMultiLineText.substr(pos, nl - pos);
        if (!seg.empty() && seg.back() == L'\r') {
            seg.pop_back();
        }
        lines.push_back(std::move(seg));
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    if (lines.size() <= 1) return rawMultiLineText;

    size_t maxLen = 0;
    for (const auto& ln : lines) {
        if (ln.find(L'\t') == std::wstring::npos) {
            maxLen = (std::max)(maxLen, ln.size());
        }
    }

    auto trimView = [](const std::wstring& s) -> std::wstring_view {
        size_t a = 0;
        while (a < s.size() && (s[a] == L' ' || s[a] == L'\t')) ++a;
        size_t b = s.size();
        while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) --b;
        return std::wstring_view(s.data() + a, b - a);
    };

    std::wstring out;
    out.reserve(rawMultiLineText.size());
    bool prevWasEmpty = true;
    size_t prevRawLen = 0;
    bool prevHadTab = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring_view cur = trimView(lines[i]);
        if (cur.empty()) {
            if (!out.empty() && !prevWasEmpty) {
                out += L"\n\n";
            }
            prevWasEmpty = true;
            prevRawLen = 0;
            prevHadTab = false;
            continue;
        }

        const bool curHasTab = (cur.find(L'\t') != std::wstring_view::npos);
        const bool curIsList = looksLikeListOrHeadingLine(cur);
        const size_t leadingSpaces = lines[i].find_first_not_of(L' ');
        const bool hasDeepIndent = (leadingSpaces != std::wstring::npos && leadingSpaces >= 4);

        if (out.empty() || prevWasEmpty) {
            out.append(cur.data(), cur.size());
        } else {
            const wchar_t prevLast = out.back();
            const wchar_t curFirst = cur.front();
            const bool prevShortEnded = isSentenceTerminalPunct(prevLast) &&
                                        maxLen >= 16 &&
                                        prevRawLen * 100 < maxLen * 68;
            const std::string outUtf8 = tools3000::core::WinUtils::wstringToUtf8(out);
            const std::string curUtf8 = tools3000::core::WinUtils::wstringToUtf8(std::wstring(cur));
            if (shouldSeamlessJoinSoftWrappedJsonLine(outUtf8, curUtf8)) {
                out.append(cur.data(), cur.size());
            } else if (prevHadTab || curHasTab || curIsList || hasDeepIndent || prevShortEnded) {
                out += L'\n';
                out.append(cur.data(), cur.size());
            } else if (prevLast == L'-' && out.size() >= 2 &&
                       isAsciiAlpha(out[out.size() - 2]) &&
                       (curFirst >= L'a' && curFirst <= L'z')) {
                out.pop_back();
                out.append(cur.data(), cur.size());
            } else if (isCjkOrFullwidth(prevLast) || isCjkOrFullwidth(curFirst)) {
                out.append(cur.data(), cur.size());
            } else {
                out += L' ';
                out.append(cur.data(), cur.size());
            }
        }

        prevWasEmpty = false;
        prevRawLen = cur.size();
        prevHadTab = curHasTab;
    }

    return out;
}

std::string OcrEngine::smartMergeParagraphsUtf8(const std::string& rawMultiLineUtf8) {
    if (rawMultiLineUtf8.empty()) return {};
    const std::wstring w = tools3000::core::WinUtils::utf8ToWstring(rawMultiLineUtf8);
    return tools3000::core::WinUtils::wstringToUtf8(smartMergeParagraphs(w));
}

}  // namespace tools3000::ocr

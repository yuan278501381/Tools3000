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

float computeMedianHeight(const std::vector<OcrWordToken>& tokens) {
    if (tokens.empty()) return 16.0f;
    std::vector<float> heights;
    heights.reserve(tokens.size());
    for (const auto& t : tokens) {
        if (t.h > 1.0f) heights.push_back(t.h);
    }
    if (heights.empty()) return 16.0f;
    const size_t mid = heights.size() / 2;
    std::nth_element(heights.begin(), heights.begin() + static_cast<std::ptrdiff_t>(mid), heights.end());
    return (std::max)(8.0f, heights[mid]);
}

size_t estimateDistinctVisualLines(const std::vector<OcrWordToken>& subset, float medH) {
    if (subset.empty()) return 0;
    std::vector<float> cys;
    cys.reserve(subset.size());
    for (const auto& t : subset) {
        cys.push_back(t.cy());
    }
    std::sort(cys.begin(), cys.end());
    size_t lines = 1;
    float anchorCy = cys.front();
    const float thresh = (std::max)(6.0f, medH * 0.58f);
    for (size_t i = 1; i < cys.size(); ++i) {
        if (cys[i] - anchorCy > thresh) {
            ++lines;
            anchorCy = cys[i];
        }
    }
    return lines;
}

struct VerticalSplitCandidate {
    bool valid = false;
    float splitX = 0.0f;
    float gapWidth = 0.0f;
};

VerticalSplitCandidate findBestVerticalCorridor(const std::vector<OcrWordToken>& tokens, float medH) {
    VerticalSplitCandidate best{};
    if (tokens.size() < 2) return best;

    std::vector<std::pair<float, float>> intervals;
    intervals.reserve(tokens.size());
    float minX = FLT_MAX;
    float maxX = -FLT_MAX;
    for (const auto& t : tokens) {
        const float l = t.x;
        const float r = (std::max)(t.x + 1.0f, t.right());
        intervals.emplace_back(l, r);
        minX = (std::min)(minX, l);
        maxX = (std::max)(maxX, r);
    }
    const float totalSpanW = maxX - minX;
    if (totalSpanW < medH * 4.0f) {
        return best;
    }

    std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
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

    const float minCorridorW = (std::max)(14.0f, medH * 1.55f);
    for (size_t i = 0; i + 1 < merged.size(); ++i) {
        const float gapL = merged[i].second;
        const float gapR = merged[i + 1].first;
        const float gapW = gapR - gapL;
        if (gapW < minCorridorW) continue;

        const float midX = (gapL + gapR) * 0.5f;
        std::vector<OcrWordToken> leftTokens;
        std::vector<OcrWordToken> rightTokens;
        leftTokens.reserve(tokens.size());
        rightTokens.reserve(tokens.size());
        for (const auto& t : tokens) {
            if (t.cy() == t.cy() && (t.x + t.w * 0.5f) < midX) {
                leftTokens.push_back(t);
            } else {
                rightTokens.push_back(t);
            }
        }
        if (leftTokens.empty() || rightTokens.empty()) continue;

        const size_t leftLines = estimateDistinctVisualLines(leftTokens, medH);
        const size_t rightLines = estimateDistinctVisualLines(rightTokens, medH);
        if (leftLines >= 2 && rightLines >= 2) {
            if (gapW > best.gapWidth) {
                best.valid = true;
                best.splitX = midX;
                best.gapWidth = gapW;
            }
        }
    }
    return best;
}

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

void clusterSingleColumnLines(
    std::vector<OcrWordToken> tokens,
    float scaleFactor,
    int borderPad,
    int maxImageWidth,
    int maxImageHeight,
    std::vector<OcrResult>& outResults) {
    if (tokens.empty()) return;

    std::sort(tokens.begin(), tokens.end(), [](const OcrWordToken& a, const OcrWordToken& b) {
        return a.cy() < b.cy();
    });

    struct VisualLine {
        std::vector<OcrWordToken> words;
        float sumCy = 0.0f;
        float sumH = 0.0f;
        float avgCy() const noexcept { return words.empty() ? 0.0f : sumCy / static_cast<float>(words.size()); }
        float avgH() const noexcept { return words.empty() ? 16.0f : sumH / static_cast<float>(words.size()); }
    };

    std::vector<VisualLine> visualLines;
    for (auto& tok : tokens) {
        int bestIdx = -1;
        float bestDist = FLT_MAX;
        for (int i = static_cast<int>(visualLines.size()) - 1; i >= 0; --i) {
            const float refH = (std::max)(8.0f, (std::min)(visualLines[i].avgH(), tok.h));
            const float dy = std::abs(tok.cy() - visualLines[i].avgCy());
            if (dy <= refH * 0.58f && dy < bestDist) {
                bestDist = dy;
                bestIdx = i;
            }
        }
        if (bestIdx >= 0) {
            visualLines[bestIdx].sumCy += tok.cy();
            visualLines[bestIdx].sumH += tok.h;
            visualLines[bestIdx].words.push_back(std::move(tok));
        } else {
            VisualLine vl;
            vl.sumCy = tok.cy();
            vl.sumH = tok.h;
            vl.words.push_back(std::move(tok));
            visualLines.push_back(std::move(vl));
        }
    }

    std::sort(visualLines.begin(), visualLines.end(), [](const VisualLine& a, const VisualLine& b) {
        return a.avgCy() < b.avgCy();
    });

    const float safeScale = (scaleFactor > 0.0001f) ? scaleFactor : 1.0f;
    for (auto& vl : visualLines) {
        if (vl.words.empty()) continue;
        std::sort(vl.words.begin(), vl.words.end(), [](const OcrWordToken& a, const OcrWordToken& b) {
            return a.x < b.x;
        });

        std::wstring lineW;
        std::vector<cv::Rect> lineCharBoxes;
        float minX = FLT_MAX, minY = FLT_MAX, maxX = 0.0f, maxY = 0.0f;
        const float lineH = (std::max)(10.0f, vl.avgH());

        for (size_t i = 0; i < vl.words.size(); ++i) {
            const auto& cur = vl.words[i];
            if (cur.text.empty()) continue;
            minX = (std::min)(minX, cur.x);
            minY = (std::min)(minY, cur.y);
            maxX = (std::max)(maxX, cur.right());
            maxY = (std::max)(maxY, cur.bottom());

            if (!lineW.empty() && i > 0) {
                const auto& prev = vl.words[i - 1];
                if (!prev.text.empty()) {
                    const float gap = cur.x - prev.right();
                    const wchar_t lastCh = prev.text.back();
                    const wchar_t firstCh = cur.text.front();
                    wchar_t sepChar = 0;

                    if (gap > lineH * 1.35f) {
                        sepChar = L'\t';
                    } else if (isCjkOrFullwidth(lastCh) || isCjkOrFullwidth(firstCh)) {
                        if (gap > lineH * 0.65f) sepChar = L' ';
                    } else if (isTightConnectorLeft(lastCh) || isTightConnectorRight(firstCh)) {
                        if (gap > lineH * 0.55f) sepChar = L' ';
                    } else if (isAsciiDigit(lastCh) && isAsciiDigit(firstCh) && gap < lineH * 0.32f) {
                        sepChar = 0;
                    } else if (gap >= lineH * 0.14f) {
                        sepChar = L' ';
                    }

                    if (sepChar != 0) {
                        lineW += sepChar;
                        const float gx1 = (std::min)(prev.right(), cur.x);
                        const float gx2 = (std::max)(gx1 + 1.0f, cur.x);
                        const float gy1 = (std::min)(prev.y, cur.y);
                        const float gy2 = (std::max)(prev.bottom(), cur.bottom());
                        lineCharBoxes.push_back(mapTokenBoxToOrigRect(
                            gx1, gy1, gx2, gy2, safeScale, borderPad, maxImageWidth, maxImageHeight));
                    }
                }
            }

            lineW += cur.text;
            const std::string wordUtf8 = tools3000::core::WinUtils::wstringToUtf8(cur.text);
            const cv::Rect wordOrigRect = mapTokenBoxToOrigRect(
                cur.x, cur.y, cur.right(), cur.bottom(),
                safeScale, borderPad, maxImageWidth, maxImageHeight);
            auto wordSubBoxes = OcrEngine::buildProportionalCharBoxes(wordUtf8, wordOrigRect);
            lineCharBoxes.insert(lineCharBoxes.end(), wordSubBoxes.begin(), wordSubBoxes.end());
        }
        if (lineW.empty()) continue;

        OcrResult r;
        r.text = tools3000::core::WinUtils::wstringToUtf8(lineW);
        r.confidence = 1.0f;

        int ox1 = static_cast<int>(std::round(minX / safeScale)) - borderPad;
        int oy1 = static_cast<int>(std::round(minY / safeScale)) - borderPad;
        int ox2 = static_cast<int>(std::round(maxX / safeScale)) - borderPad;
        int oy2 = static_cast<int>(std::round(maxY / safeScale)) - borderPad;
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
        r.box = cv::Rect(ox1, oy1, ox2 - ox1, oy2 - oy1);

        const size_t expectedChars = OcrEngine::countUtf8Codepoints(r.text);
        if (lineCharBoxes.size() == expectedChars) {
            r.charBoxes = std::move(lineCharBoxes);
        } else {
            r.charBoxes = OcrEngine::buildProportionalCharBoxes(r.text, r.box);
        }

        outResults.push_back(std::move(r));
    }
}

void recursiveXYCut(
    std::vector<OcrWordToken> tokens,
    float medH,
    float scaleFactor,
    int borderPad,
    int maxImageWidth,
    int maxImageHeight,
    int depth,
    std::vector<OcrResult>& outResults) {
    if (tokens.empty()) return;
    if (depth >= 8 || tokens.size() < 4) {
        clusterSingleColumnLines(
            std::move(tokens), scaleFactor, borderPad, maxImageWidth, maxImageHeight, outResults);
        return;
    }

    const VerticalSplitCandidate vSplit = findBestVerticalCorridor(tokens, medH);
    if (vSplit.valid) {
        std::vector<OcrWordToken> leftCol;
        std::vector<OcrWordToken> rightCol;
        leftCol.reserve(tokens.size());
        rightCol.reserve(tokens.size());
        for (auto& t : tokens) {
            if ((t.x + t.w * 0.5f) < vSplit.splitX) {
                leftCol.push_back(std::move(t));
            } else {
                rightCol.push_back(std::move(t));
            }
        }
        recursiveXYCut(std::move(leftCol), medH, scaleFactor, borderPad, maxImageWidth, maxImageHeight, depth + 1, outResults);
        recursiveXYCut(std::move(rightCol), medH, scaleFactor, borderPad, maxImageWidth, maxImageHeight, depth + 1, outResults);
        return;
    }

    std::vector<size_t> byY(tokens.size());
    for (size_t i = 0; i < tokens.size(); ++i) byY[i] = i;
    std::sort(byY.begin(), byY.end(), [&tokens](size_t a, size_t b) {
        return tokens[a].y < tokens[b].y;
    });

    std::vector<float> prefixMaxBottom(tokens.size(), 0.0f);
    float runningBottom = -FLT_MAX;
    for (size_t i = 0; i < byY.size(); ++i) {
        runningBottom = (std::max)(runningBottom, tokens[byY[i]].bottom());
        prefixMaxBottom[i] = runningBottom;
    }

    size_t bestHIdx = 0;
    float bestHScore = 0.0f;
    const float minHGap = (std::max)(4.0f, medH * 0.35f);
    for (size_t i = 0; i + 1 < byY.size(); ++i) {
        const float hGap = tokens[byY[i + 1]].y - prefixMaxBottom[i];
        if (hGap < minHGap) continue;

        std::vector<OcrWordToken> topPart;
        std::vector<OcrWordToken> botPart;
        topPart.reserve(i + 1);
        botPart.reserve(byY.size() - (i + 1));
        for (size_t k = 0; k <= i; ++k) topPart.push_back(tokens[byY[k]]);
        for (size_t k = i + 1; k < byY.size(); ++k) botPart.push_back(tokens[byY[k]]);

        const auto topV = findBestVerticalCorridor(topPart, medH);
        const auto botV = findBestVerticalCorridor(botPart, medH);
        bool effectiveTopSplit = topV.valid;
        bool effectiveBotSplit = botV.valid;

        if (effectiveBotSplit && !topPart.empty()) {
            float maxTopCy = -FLT_MAX;
            for (const auto& t : topPart) maxTopCy = (std::max)(maxTopCy, t.cy());
            std::vector<OcrWordToken> botWithBoundary = botPart;
            for (const auto& t : topPart) {
                if (maxTopCy - t.cy() <= medH * 0.70f) {
                    botWithBoundary.push_back(t);
                }
            }
            if (findBestVerticalCorridor(botWithBoundary, medH).valid) {
                effectiveBotSplit = false;
            }
        }
        if (effectiveTopSplit && !botPart.empty()) {
            float minBotCy = FLT_MAX;
            for (const auto& t : botPart) minBotCy = (std::min)(minBotCy, t.cy());
            std::vector<OcrWordToken> topWithBoundary = topPart;
            for (const auto& t : botPart) {
                if (t.cy() - minBotCy <= medH * 0.70f) {
                    topWithBoundary.push_back(t);
                }
            }
            if (findBestVerticalCorridor(topWithBoundary, medH).valid) {
                effectiveTopSplit = false;
            }
        }

        if (effectiveTopSplit || effectiveBotSplit) {
            const float score = hGap * 2.0f +
                                (effectiveTopSplit ? topV.gapWidth : 0.0f) +
                                (effectiveBotSplit ? botV.gapWidth : 0.0f);
            if (score > bestHScore) {
                bestHScore = score;
                bestHIdx = i + 1;
            }
        }
    }

    if (bestHIdx > 0 && bestHIdx < byY.size()) {
        std::vector<OcrWordToken> topTokens;
        std::vector<OcrWordToken> botTokens;
        topTokens.reserve(bestHIdx);
        botTokens.reserve(byY.size() - bestHIdx);
        for (size_t k = 0; k < bestHIdx; ++k) topTokens.push_back(std::move(tokens[byY[k]]));
        for (size_t k = bestHIdx; k < byY.size(); ++k) botTokens.push_back(std::move(tokens[byY[k]]));
        recursiveXYCut(std::move(topTokens), medH, scaleFactor, borderPad, maxImageWidth, maxImageHeight, depth + 1, outResults);
        recursiveXYCut(std::move(botTokens), medH, scaleFactor, borderPad, maxImageWidth, maxImageHeight, depth + 1, outResults);
        return;
    }

    clusterSingleColumnLines(
        std::move(tokens), scaleFactor, borderPad, maxImageWidth, maxImageHeight, outResults);
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
    if (srcImg.cols >= 8 && srcImg.rows >= 8 && srcImg.cols + 24 < maxDim && srcImg.rows + 24 < maxDim) {
        outBorderPad = 12;
        cv::copyMakeBorder(srcImg, prepared, outBorderPad, outBorderPad, outBorderPad, outBorderPad, cv::BORDER_REPLICATE);
        const int maxPreparedSide = (std::max)(prepared.cols, prepared.rows);
        if (maxPreparedSide < 2600) {
            const float maxAllowedScale = static_cast<float>(maxDim) / static_cast<float>((std::max)(1, maxPreparedSide));
            const float desiredScale = (std::min)(2.0f, std::floor(maxAllowedScale * 10.0f) * 0.1f);
            if (desiredScale >= 1.4f) {
                const int upW = (std::min)(maxDim, (std::max)(1, static_cast<int>(std::round(static_cast<float>(prepared.cols) * desiredScale))));
                const int upH = (std::min)(maxDim, (std::max)(1, static_cast<int>(std::round(static_cast<float>(prepared.rows) * desiredScale))));
                outScaleFactor = static_cast<float>(upW) / static_cast<float>(prepared.cols);
                cv::resize(prepared, prepared, cv::Size(upW, upH), 0.0, 0.0, cv::INTER_LANCZOS4);
                // 温和反锐化掩模 (Unsharp Mask)：收紧高分屏次像素渲染灰字与浅色胶囊小数字边缘
                cv::Mat blurred;
                cv::GaussianBlur(prepared, blurred, cv::Size(0, 0), 0.85);
                cv::addWeighted(prepared, 1.26, blurred, -0.26, 0.0, prepared);
            }
        }
    } else if (srcImg.cols > maxDim) {
        outScaleFactor = static_cast<float>(maxDim) / static_cast<float>(srcImg.cols);
        const int targetW = (std::min)(maxDim, (std::max)(1, static_cast<int>(std::floor(srcImg.cols * outScaleFactor))));
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
        if (maxSide > 2000) {
            const float sc = 1600.0f / static_cast<float>(maxSide);
            cv::resize(gray, work, cv::Size(), sc, sc, cv::INTER_AREA);
            invScale = 1.0f / sc;
        } else if (minSide < 140) {
            cv::resize(gray, work, cv::Size(), 2.0, 2.0, cv::INTER_CUBIC);
            invScale = 0.5f;
        }

        cv::QRCodeDetector detector;
        auto runPass = [&](const cv::Mat& passGray) {
            std::vector<std::string> decoded;
            cv::Mat points;
            bool ok = false;
            try {
                ok = detector.detectAndDecodeMulti(passGray, decoded, points);
            } catch (...) {
                ok = false;
            }
            if (!ok || decoded.empty() || points.empty()) {
                // 单码快速兜底
                cv::Mat singlePts;
                std::string singleTxt;
                try {
                    singleTxt = detector.detectAndDecode(passGray, singlePts);
                } catch (...) {}
                if (!singleTxt.empty() && !singlePts.empty()) {
                    decoded.push_back(std::move(singleTxt));
                    points = singlePts.reshape(2, 1);
                }
            }

            const int numCodes = static_cast<int>(decoded.size());
            for (int i = 0; i < numCodes; ++i) {
                if (decoded[static_cast<size_t>(i)].empty()) continue;

                // 避免双路重复收录同一个二维码
                bool dup = false;
                for (const auto& existing : qrResults) {
                    if (existing.text == decoded[static_cast<size_t>(i)]) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;

                OcrQrResult item;
                item.text = decoded[static_cast<size_t>(i)];

                float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
                bool validPts = false;
                if (points.rows > i && points.cols == 4 && points.type() == CV_32FC2) {
                    const auto* rowPts = points.ptr<cv::Point2f>(i);
                    for (size_t k = 0; k < 4; ++k) {
                        const float px = std::clamp(rowPts[k].x * invScale, 0.0f, static_cast<float>(image.cols));
                        const float py = std::clamp(rowPts[k].y * invScale, 0.0f, static_cast<float>(image.rows));
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
                        const float px = std::clamp(rawPts[k].x * invScale, 0.0f, static_cast<float>(image.cols));
                        const float py = std::clamp(rawPts[k].y * invScale, 0.0f, static_cast<float>(image.rows));
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
            }
        };

        runPass(work);
        if (qrResults.empty() && isDarkBackgroundImage(image)) {
            cv::Mat invWork = 255 - work;
            runPass(invWork);
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

void OcrEngine::shutdown() {
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
            std::move(chosenTokens), chosenScale, chosenPad, image.cols, image.rows);

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

    const bool allowDeepOcr = (enginePref == "auto" || enginePref == "ppocr" || enginePref == "ppocr_v4" || enginePref == "deep");
    if (allowDeepOcr && DeepOcrEngine::instance().isModelInstalled()) {
        auto deepResults = DeepOcrEngine::instance().extractText(image);
        if (!deepResults.empty()) {
            return deepResults;
        }
        // 若用户强制锁定 ppocr_v4 且模型真实加载成功但图中确实无字，无需重复跑 Windows OCR
        if (enginePref == "ppocr_v4" && DeepOcrEngine::instance().isModelInstalled()) {
            return deepResults;
        }
    }

    return extractWithWindowsOcr(image);
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

    const bool preferDeep = (enginePref == "auto" || enginePref == "ppocr" || enginePref == "ppocr_v4" || enginePref == "deep") &&
                            DeepOcrEngine::instance().isModelInstalled();

    page.qrCodes = detectQrCodes(image);
    if (preferDeep) {
        page.lines = DeepOcrEngine::instance().extractText(image);
        if (!page.lines.empty() || enginePref == "ppocr_v4") {
            page.activeEngine = "ppocr_v4";
            return page;
        }
    }

    page.lines = extractWithWindowsOcr(image);
    page.activeEngine = "windows";
    return page;
}

std::string OcrEngine::recognizeToText(const cv::Mat& image) {
    std::string text;
    for (auto const& r : extractText(image)) {
        if (!text.empty()) text += '\n';
        text += r.text;
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

std::vector<OcrResult> OcrEngine::assembleLinesWithXYCut(
    std::vector<OcrWordToken> tokens,
    float scaleFactor,
    int borderPad,
    int maxImageWidth,
    int maxImageHeight) {
    tokens.erase(
        std::remove_if(tokens.begin(), tokens.end(), [](const OcrWordToken& t) {
            return t.text.empty() || t.w < 0.0f || t.h < 0.0f;
        }),
        tokens.end());
    if (tokens.empty()) return {};

    const float medH = computeMedianHeight(tokens);
    std::vector<OcrResult> results;
    results.reserve(tokens.size());
    recursiveXYCut(
        std::move(tokens), medH, scaleFactor, borderPad, maxImageWidth, maxImageHeight, 0, results);
    return results;
}

std::string OcrEngine::orderTextBlocksWithXYCut(
    const std::vector<OcrResult>& blocks,
    const char* lineEnding) {
    if (blocks.empty()) return {};
    const char* eol = (lineEnding != nullptr) ? lineEnding : "\r\n";

    std::vector<OcrWordToken> tokens;
    tokens.reserve(blocks.size());
    for (const auto& b : blocks) {
        if (b.text.empty()) continue;
        std::wstring wt = tools3000::core::WinUtils::utf8ToWstring(b.text);
        if (wt.empty()) continue;
        tokens.push_back(OcrWordToken{
            std::move(wt),
            static_cast<float>(b.box.x),
            static_cast<float>(b.box.y),
            static_cast<float>((std::max)(1, b.box.width)),
            static_cast<float>((std::max)(1, b.box.height))
        });
    }
    if (tokens.empty()) return {};

    const auto orderedLines = assembleLinesWithXYCut(std::move(tokens), 1.0f, 0, 0, 0);
    std::string combined;
    for (size_t i = 0; i < orderedLines.size(); ++i) {
        if (orderedLines[i].text.empty()) continue;
        if (!combined.empty()) {
            combined += eol;
        }
        combined += orderedLines[i].text;
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
            if (prevHadTab || curHasTab || curIsList || hasDeepIndent || prevShortEnded) {
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

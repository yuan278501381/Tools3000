// ─────────────────────────────────────────────────────────────────────────────
// OcrEngine.cpp — 基于 Windows.Media.Ocr (WinRT) 的离线文字识别
//
// 选型理由 (相较 PaddleOCR Lite 外部进程方案):
//   • 完全离线、随系统内置，无需打包 ~30MB 模型与 exe;
//   • 官方 API，支持 25+ 语言 (依赖已安装的语言包);
//   • 与需求最低版本 Windows 10 1903 完全吻合。
//
// cv::Mat (BGR/BGRA/灰度) → SoftwareBitmap(Bgra8) → OcrEngine.RecognizeAsync。
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/OcrEngine.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

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

/// 创建一个 OCR 引擎: 优先读取 /ocr/language 配置（支持 auto 及指定语种 tag，缺失对应语言包时平滑回退可用语种绝不崩溃）。
wmo::OcrEngine createEngine() {
    std::string langPref;
    try {
        langPref = tools3000::core::ConfigManager::instance().get<std::string>("/ocr/language", "auto");
    } catch (...) {
        langPref = "auto";
    }

    auto toLowerAscii = [](std::string s) {
        for (char& ch : s) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        return s;
    };

    const std::string lowerPref = toLowerAscii(langPref);
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
                    // Preserve script subtags for Chinese (zh-hans vs zh-hant) while allowing region fallback.
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
                    const std::string candTag = toLowerAscii(tools3000::core::WinUtils::wstringToUtf8(cand.LanguageTag().c_str()));
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

bool isCjkOrFullwidth(wchar_t ch) noexcept {
    return (ch >= 0x3400 && ch <= 0x4DBF) || // CJK Unified Ideographs Extension A
           (ch >= 0x4E00 && ch <= 0x9FFF) || // CJK Unified Ideographs
           (ch >= 0x2E80 && ch <= 0x2FDF) || // CJK Radicals Supplement & Kangxi Radicals
           (ch >= 0x3000 && ch <= 0x33FF) || // CJK Symbols/Punctuation, Hiragana, Katakana, Bopomofo, Compatibility
           (ch >= 0xAC00 && ch <= 0xD7AF) || // Hangul Syllables
           (ch >= 0xF900 && ch <= 0xFAFF) || // CJK Compatibility Ideographs
           (ch >= 0xFE10 && ch <= 0xFE6F) || // Vertical Forms, CJK Compatibility Forms & Small Form Variants
           (ch >= 0xFF00 && ch <= 0xFFEF) || // Halfwidth and Fullwidth Forms
           (ch >= 0xD840 && ch <= 0xD884) || // UTF-16 High Surrogates for Plane 2/3 CJK Extensions
           (ch >= 0xDC00 && ch <= 0xDFFF);   // UTF-16 Low Surrogates
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

    // 投影全部词块到 X 轴区间并合并重叠区间，寻找零词块穿过的垂直空白走廊
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
        // 贯穿多行准则：左右两侧各自至少包含 2 个视觉行（防止将单行键值或短表头误切为上下两栏）
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

                    if (gap > lineH * 1.35f) {
                        // 栏内表格式列间距统一采用制表符 \t 无损对齐拼接
                        lineW += L'\t';
                    } else if (isCjkOrFullwidth(lastCh) || isCjkOrFullwidth(firstCh)) {
                        if (gap > lineH * 0.65f) {
                            lineW += L' ';
                        }
                    } else if (isTightConnectorLeft(lastCh) || isTightConnectorRight(firstCh)) {
                        if (gap > lineH * 0.55f) {
                            lineW += L' ';
                        }
                    } else if (isAsciiDigit(lastCh) && isAsciiDigit(firstCh) && gap < lineH * 0.32f) {
                        // 被拆碎的连续数字直接拼接
                    } else if (gap >= lineH * 0.14f) {
                        lineW += L' ';
                    }
                }
            }
            lineW += cur.text;
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

    // 1. 优先尝试垂直空白走廊切分 (V-Cut)：直接分离独立左右阅读栏（双栏论文 / 三栏排版 / IDE 分屏代码）
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

    // 2. 尝试水平投影切分 (H-Cut)：分离横跨多栏的顶部通栏标题、底部通栏脚注或大段垂直空白区域，
    //    仅当水平切开后能在任一子块中暴露出有效垂直分栏走廊（或遇到超大章节空白）时执行切分。
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

        // 走廊阻塞最小边界不变量：仅当紧邻切口的跨栏行确实阻塞了另一侧的分栏走廊时才切分，
        // 严禁在多栏正文内部因某几行略短而将双栏拦腰切断。
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

    // 3. 叶子单栏区域：执行 Y 轴视觉行聚类与表格式间距 \t 拼接
    clusterSingleColumnLines(
        std::move(tokens), scaleFactor, borderPad, maxImageWidth, maxImageHeight, outResults);
}

}  // namespace

OcrEngine& OcrEngine::instance() {
    static OcrEngine inst;
    return inst;
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
    return m_available;
}

void OcrEngine::shutdown() {
    LOG_INFO("OCR 引擎已关闭");
}

std::vector<OcrResult> OcrEngine::extractText(const cv::Mat& image) {
    tools3000::core::TraceId::Scope scope;
    if (image.empty()) {
        LOG_WARN("OCR 输入图像为空");
        return {};
    }
    if (isCurrentThreadSta()) {
        return runInMtaSafe([&image]() -> std::vector<OcrResult> {
            return OcrEngine::instance().extractText(image);
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

        // 图像预处理：
        //   1) 中小尺寸截图：边缘安全留白 + 2x 双三次放大，提升小字号识别率
        //   2) 超宽截图 (> MaxImageDimension)：按比例等比缩宽至上限以内，防止 WinRT E_INVALIDARG 崩溃
        //   3) 超高长截图 (> MaxImageDimension)：保持清晰字号按重叠分片纵向逐块识别并去重拼接
        cv::Mat prepared = image;
        int borderPad = 0;
        float scaleFactor = 1.0f;
        const int maxDim = std::max(256, static_cast<int>(wmo::OcrEngine::MaxImageDimension()));
        if (image.cols >= 8 && image.rows >= 8 && image.cols + 24 < maxDim && image.rows + 24 < maxDim) {
            borderPad = 12;
            cv::copyMakeBorder(image, prepared, borderPad, borderPad, borderPad, borderPad, cv::BORDER_REPLICATE);
            if (std::max(prepared.cols, prepared.rows) < 1800 &&
                prepared.cols * 2 <= maxDim && prepared.rows * 2 <= maxDim) {
                scaleFactor = 2.0f;
                cv::resize(prepared, prepared, cv::Size(), scaleFactor, scaleFactor, cv::INTER_CUBIC);
            }
        } else if (image.cols > maxDim) {
            scaleFactor = static_cast<float>(maxDim) / static_cast<float>(image.cols);
            const int targetW = std::min(maxDim, std::max(1, static_cast<int>(std::floor(image.cols * scaleFactor))));
            const int targetH = std::max(1, static_cast<int>(std::floor(image.rows * scaleFactor)));
            scaleFactor = static_cast<float>(targetW) / static_cast<float>(image.cols);
            cv::resize(image, prepared, cv::Size(targetW, targetH), 0.0, 0.0, cv::INTER_AREA);
        }

        // 收集所有识别词块并按 XY-Cut 分栏、视觉行聚类与智能无损拼接
        std::vector<OcrWordToken> tokens;
        if (prepared.rows <= maxDim) {
            wgi::SoftwareBitmap bitmap = matToBitmap(prepared);
            if (!bitmap) {
                LOG_ERROR("OCR: 不支持的图像通道数: {}", image.channels());
                return results;
            }
            wmo::OcrResult result = engine.RecognizeAsync(bitmap).get();
            for (auto const& line : result.Lines()) {
                for (auto const& w : line.Words()) {
                    std::wstring wt(w.Text().c_str());
                    if (wt.empty()) continue;
                    wf::Rect br = w.BoundingRect();
                    tokens.push_back(OcrWordToken{std::move(wt), br.X, br.Y, br.Width, br.Height});
                }
            }
        } else {
            const int overlap = std::min(96, maxDim / 8);
            const int step = std::max(64, maxDim - overlap);
            for (int tileTop = 0; tileTop < prepared.rows; tileTop += step) {
                const int tileH = std::min(maxDim, prepared.rows - tileTop);
                if (tileH < 8) break;
                cv::Mat tile = prepared(cv::Rect(0, tileTop, prepared.cols, tileH));
                wgi::SoftwareBitmap bitmap = matToBitmap(tile);
                if (!bitmap) {
                    LOG_ERROR("OCR: 不支持的图像通道数: {}", image.channels());
                    return results;
                }
                const size_t prevTokenCount = tokens.size();
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
                                const auto& existing = tokens[k];
                                if (existing.y + existing.h < static_cast<float>(tileTop) - 4.0f) continue;
                                if (existing.text == wt &&
                                    std::abs(existing.cy() - candCy) <= std::max(6.0f, br.Height * 0.6f) &&
                                    std::abs(existing.x - br.X) <= std::max(8.0f, br.Width * 0.5f)) {
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (duplicate) continue;
                        }
                        tokens.push_back(OcrWordToken{std::move(wt), br.X, globalY, br.Width, br.Height});
                    }
                }
                if (tileTop + tileH >= prepared.rows) break;
            }
        }

        if (tokens.empty()) {
            LOG_INFO("OCR 完成, 识别行数: {}", results.size());
            return results;
        }

        results = assembleLinesWithXYCut(
            std::move(tokens), scaleFactor, borderPad, image.cols, image.rows);

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

std::string OcrEngine::recognizeToText(const cv::Mat& image) {
    std::string text;
    for (auto const& r : extractText(image)) {
        if (!text.empty()) text += '\n';
        text += r.text;
    }
    return text;
}

std::string OcrEngine::recognizeImageFile(const std::string& utf8Path) {
    constexpr std::uintmax_t kMaxOcrImageFileBytes = 64ULL * 1024ULL * 1024ULL; // 64 MiB 安全上限
    cv::Mat img;
    try {
        // OpenCV imread 需要本地编码路径; 用宽字符校验文件体积后一次性高速读取解码以支持非 ASCII 路径。
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

    // 拆分行（兼容 \r\n 与 \n）
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

    // 估算非空普通正文行的典型视觉字符长度
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
            // 若上一行是表格行、短句终止行，或当前行是列表项/缩进代码/表格行，则保留换行分段
            const bool prevShortEnded = isSentenceTerminalPunct(prevLast) &&
                                        maxLen >= 16 &&
                                        prevRawLen * 100 < maxLen * 68;
            if (prevHadTab || curHasTab || curIsList || hasDeepIndent || prevShortEnded) {
                out += L'\n';
                out.append(cur.data(), cur.size());
            } else if (prevLast == L'-' && out.size() >= 2 &&
                       isAsciiAlpha(out[out.size() - 2]) &&
                       (curFirst >= L'a' && curFirst <= L'z')) {
                // 西文行末断词连字符（如 inter-\nnational -> international）
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

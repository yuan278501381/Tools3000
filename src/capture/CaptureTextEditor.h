#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTURETEXTEDITOR_H
#define TOOLS3000_CAPTURE_CAPTURETEXTEDITOR_H

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace tools3000::capture {

bool initializeMarkupTextRenderer();

/// 原地多行文本标注的排版度量快照（与 GDI+ GraphicsPath::AddString 100% 度量同构）
struct TextLineLayout {
    size_t startIndex = 0;          // 本视觉行首字符在 wstring 中的索引
    size_t endIndex = 0;            // 本视觉行末字符后一位索引（不含硬换行 '\n'）
    bool softWrapped = false;       // true 表示行尾因到达最大宽度自动软换行（下一行 startIndex == endIndex）
    float topY = 0.0f;              // 相对于文本框左上角的 Y 偏移
    float height = 0.0f;            // 单行视觉高度
    std::vector<float> charOffsets; // 长度为 (endIndex - startIndex + 1)，记录每个字符边界相对于 textX 的像素偏移
};

struct MultilineTextLayout {
    float textOriginX = 6.0f;
    float textOriginY = 4.0f;
    float lineStep = 20.0f;
    float glyphHeight = 18.0f;
    float maxContentWidth = 0.0f;   // 本次排版生效的文本内容区最大宽度（<= 0.0f 表示不限宽）
    float contentWidth = 0.0f;      // 全部视觉行的最大可视像素宽度
    float contentHeight = 0.0f;     // 全部视觉行占用的总内容像素高度
    std::vector<TextLineLayout> lines;
};

struct TextHighlightSpan {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct TextCaretVisual {
    float x = 6.0f;
    float y = 4.0f;
    float height = 18.0f;
};

/// 原地多行文本编辑器纯逻辑与度量引擎（SSOT，支持鼠标划词、键盘导航、剪贴板选区复制/剪切/粘贴）
class CaptureTextEditor {
public:
    static bool isHighSurrogateChar(wchar_t ch) noexcept {
        return ch >= 0xD800 && ch <= 0xDBFF;
    }

    static bool isLowSurrogateChar(wchar_t ch) noexcept {
        return ch >= 0xDC00 && ch <= 0xDFFF;
    }

    static size_t clampIndex(const std::wstring& wstr, size_t idx) noexcept {
        if (idx >= wstr.size()) return wstr.size();
        if (idx > 0 && isLowSurrogateChar(wstr[idx]) && isHighSurrogateChar(wstr[idx - 1])) {
            return idx - 1;
        }
        return idx;
    }

    static size_t prevClusterIndex(const std::wstring& wstr, size_t idx) noexcept {
        idx = std::min(idx, wstr.size());
        if (idx == 0) return 0;
        --idx;
        if (idx > 0 && isLowSurrogateChar(wstr[idx]) && isHighSurrogateChar(wstr[idx - 1])) {
            --idx;
        }
        return idx;
    }

    static size_t nextClusterIndex(const std::wstring& wstr, size_t idx) noexcept {
        if (idx >= wstr.size()) return wstr.size();
        ++idx;
        if (idx < wstr.size() && isHighSurrogateChar(wstr[idx - 1]) && isLowSurrogateChar(wstr[idx])) {
            ++idx;
        }
        return idx;
    }

    static int charCategory(wchar_t ch) noexcept {
        if (ch == L'\n' || ch == L'\r') return 0;
        if (std::iswspace(ch)) return 1;
        if (std::iswalnum(ch) || ch == L'_') return 2;
        if (ch >= 0x4E00 && ch <= 0x9FFF) return 3; // CJK 表意文字
        return 4;                                   // 标点与符号
    }

    static size_t wordBoundaryLeft(const std::wstring& wstr, size_t idx) noexcept {
        idx = clampIndex(wstr, idx);
        if (idx == 0) return 0;
        size_t cur = prevClusterIndex(wstr, idx);
        while (cur > 0 && wstr[cur] != L'\n' && std::iswspace(wstr[cur])) {
            cur = prevClusterIndex(wstr, cur);
        }
        if (wstr[cur] == L'\n') return cur;
        const int cat = charCategory(wstr[cur]);
        while (cur > 0) {
            const size_t prev = prevClusterIndex(wstr, cur);
            if (charCategory(wstr[prev]) != cat) break;
            cur = prev;
        }
        return cur;
    }

    static size_t wordBoundaryRight(const std::wstring& wstr, size_t idx) noexcept {
        idx = clampIndex(wstr, idx);
        const size_t n = wstr.size();
        if (idx >= n) return n;
        size_t cur = idx;
        if (wstr[cur] == L'\n') return nextClusterIndex(wstr, cur);
        while (cur < n && wstr[cur] != L'\n' && std::iswspace(wstr[cur])) {
            cur = nextClusterIndex(wstr, cur);
        }
        if (cur >= n || wstr[cur] == L'\n') return cur;
        const int cat = charCategory(wstr[cur]);
        while (cur < n && charCategory(wstr[cur]) == cat) {
            cur = nextClusterIndex(wstr, cur);
        }
        return cur;
    }

    static bool hasSelection(size_t caret, size_t anchor) noexcept {
        return caret != anchor;
    }

    static std::pair<size_t, size_t> normalizedRange(const std::wstring& wstr, size_t caret, size_t anchor) noexcept {
        const size_t c = clampIndex(wstr, caret);
        const size_t a = clampIndex(wstr, anchor);
        return {std::min(c, a), std::max(c, a)};
    }

    static std::wstring selectedText(const std::wstring& wstr, size_t caret, size_t anchor) {
        const auto [s, e] = normalizedRange(wstr, caret, anchor);
        if (e <= s) return {};
        return wstr.substr(s, e - s);
    }

    static void selectAll(const std::wstring& wstr, size_t& caret, size_t& anchor) noexcept {
        anchor = 0;
        caret = wstr.size();
    }

    static void selectWordAt(const std::wstring& wstr, size_t hitIdx, size_t& caret, size_t& anchor) noexcept {
        if (wstr.empty()) {
            caret = anchor = 0;
            return;
        }
        size_t probe = std::min(hitIdx, wstr.size() - 1);
        probe = clampIndex(wstr, probe);
        if (wstr[probe] == L'\n') {
            anchor = probe;
            caret = nextClusterIndex(wstr, probe);
            return;
        }
        const int cat = charCategory(wstr[probe]);
        size_t left = probe;
        while (left > 0) {
            const size_t prev = prevClusterIndex(wstr, left);
            if (charCategory(wstr[prev]) != cat) break;
            left = prev;
        }
        size_t right = nextClusterIndex(wstr, probe);
        while (right < wstr.size() && charCategory(wstr[right]) == cat) {
            right = nextClusterIndex(wstr, right);
        }
        anchor = left;
        caret = right;
    }

    /// 在当前光标或选区处插入文本（自动替换已选中的子串）
    static void insertText(std::wstring& wstr, size_t& caret, size_t& anchor, const std::wstring& inserted) {
        const auto [s, e] = normalizedRange(wstr, caret, anchor);
        wstr.replace(s, e - s, inserted);
        caret = s + inserted.size();
        anchor = caret;
    }

    static bool backspace(std::wstring& wstr, size_t& caret, size_t& anchor, bool ctrlWord = false) {
        const auto [s, e] = normalizedRange(wstr, caret, anchor);
        if (e > s) {
            wstr.erase(s, e - s);
            caret = anchor = s;
            return true;
        }
        if (s == 0) return false;
        const size_t delStart = ctrlWord ? wordBoundaryLeft(wstr, s) : prevClusterIndex(wstr, s);
        wstr.erase(delStart, s - delStart);
        caret = anchor = delStart;
        return true;
    }

    static bool deleteForward(std::wstring& wstr, size_t& caret, size_t& anchor, bool ctrlWord = false) {
        const auto [s, e] = normalizedRange(wstr, caret, anchor);
        if (e > s) {
            wstr.erase(s, e - s);
            caret = anchor = s;
            return true;
        }
        if (s >= wstr.size()) return false;
        const size_t delEnd = ctrlWord ? wordBoundaryRight(wstr, s) : nextClusterIndex(wstr, s);
        wstr.erase(s, delEnd - s);
        caret = anchor = s;
        return true;
    }

    static void moveLeft(const std::wstring& wstr, size_t& caret, size_t& anchor, bool shiftExtend, bool ctrlWord) noexcept {
        caret = clampIndex(wstr, caret);
        anchor = clampIndex(wstr, anchor);
        if (!shiftExtend && caret != anchor) {
            caret = anchor = std::min(caret, anchor);
            return;
        }
        caret = ctrlWord ? wordBoundaryLeft(wstr, caret) : prevClusterIndex(wstr, caret);
        if (!shiftExtend) anchor = caret;
    }

    static void moveRight(const std::wstring& wstr, size_t& caret, size_t& anchor, bool shiftExtend, bool ctrlWord) noexcept {
        caret = clampIndex(wstr, caret);
        anchor = clampIndex(wstr, anchor);
        if (!shiftExtend && caret != anchor) {
            caret = anchor = std::max(caret, anchor);
            return;
        }
        caret = ctrlWord ? wordBoundaryRight(wstr, caret) : nextClusterIndex(wstr, caret);
        if (!shiftExtend) anchor = caret;
    }

    static void moveHome(const std::wstring& wstr, size_t& caret, size_t& anchor, bool shiftExtend, bool ctrlDoc) noexcept {
        caret = clampIndex(wstr, caret);
        anchor = clampIndex(wstr, anchor);
        if (ctrlDoc) {
            caret = 0;
        } else {
            size_t pos = caret;
            while (pos > 0 && wstr[pos - 1] != L'\n') {
                --pos;
            }
            caret = clampIndex(wstr, pos);
        }
        if (!shiftExtend) anchor = caret;
    }

    static void moveEnd(const std::wstring& wstr, size_t& caret, size_t& anchor, bool shiftExtend, bool ctrlDoc) noexcept {
        caret = clampIndex(wstr, caret);
        anchor = clampIndex(wstr, anchor);
        if (ctrlDoc) {
            caret = wstr.size();
        } else {
            size_t pos = caret;
            while (pos < wstr.size() && wstr[pos] != L'\n') {
                ++pos;
            }
            caret = clampIndex(wstr, pos);
        }
        if (!shiftExtend) anchor = caret;
    }

    /// 定位给定光标索引所在的视觉行编号（软换行边界归属下一视觉行行首）
    static size_t findVisualLineIndex(const MultilineTextLayout& layout, size_t clampedCaret) noexcept {
        for (size_t i = 0; i < layout.lines.size(); ++i) {
            const auto& line = layout.lines[i];
            const bool onLine = line.softWrapped
                ? (clampedCaret >= line.startIndex && clampedCaret < line.endIndex)
                : (clampedCaret >= line.startIndex && clampedCaret <= line.endIndex);
            if (onLine) return i;
        }
        return layout.lines.empty() ? 0 : (layout.lines.size() - 1);
    }

    /// 视觉行感知 Home（支持自动软换行行首定位，Ctrl+Home 跳至全文首）
    static void moveHomeInLayout(const MultilineTextLayout& layout, const std::wstring& wstr,
                                 size_t& caret, size_t& anchor, bool shiftExtend, bool ctrlDoc) noexcept {
        if (ctrlDoc || layout.lines.empty()) {
            moveHome(wstr, caret, anchor, shiftExtend, ctrlDoc);
            return;
        }
        caret = clampIndex(wstr, caret);
        anchor = clampIndex(wstr, anchor);
        const size_t lineIdx = findVisualLineIndex(layout, caret);
        caret = clampIndex(wstr, layout.lines[lineIdx].startIndex);
        if (!shiftExtend) anchor = caret;
    }

    /// 视觉行感知 End（支持自动软换行行尾定位，Ctrl+End 跳至全文尾）
    static void moveEndInLayout(const MultilineTextLayout& layout, const std::wstring& wstr,
                                size_t& caret, size_t& anchor, bool shiftExtend, bool ctrlDoc) noexcept {
        if (ctrlDoc || layout.lines.empty()) {
            moveEnd(wstr, caret, anchor, shiftExtend, ctrlDoc);
            return;
        }
        caret = clampIndex(wstr, caret);
        anchor = clampIndex(wstr, anchor);
        const size_t lineIdx = findVisualLineIndex(layout, caret);
        const auto& line = layout.lines[lineIdx];
        if (line.softWrapped && line.endIndex > line.startIndex) {
            // 软换行行末尾字符之后就是下一视觉行行首；停在最后一个字簇前以保持视觉光标留在本行内
            caret = prevClusterIndex(wstr, line.endIndex);
        } else {
            caret = clampIndex(wstr, line.endIndex);
        }
        if (!shiftExtend) anchor = caret;
    }

    /// 解析截图标注体系的全局统一无衬线字体回退栈（SSOT：Noto Sans SC -> Source Han Sans SC -> Segoe UI / Microsoft YaHei UI）
    static const wchar_t* resolveMarkupFontFamilyName(bool preferLatinDigits = false) {
        initializeMarkupTextRenderer();
        static const wchar_t* s_cjkResolved = []() -> const wchar_t* {
            static constexpr const wchar_t* kCjkStack[] = {
                L"Noto Sans SC",
                L"Source Han Sans SC",
                L"Microsoft YaHei UI",
                L"Segoe UI"
            };
            for (const wchar_t* name : kCjkStack) {
                Gdiplus::FontFamily fam(name);
                if (fam.IsAvailable()) return name;
            }
            return L"Microsoft YaHei UI";
        }();
        static const wchar_t* s_latinResolved = []() -> const wchar_t* {
            static constexpr const wchar_t* kLatinStack[] = {
                L"Noto Sans SC",
                L"Source Han Sans SC",
                L"Segoe UI",
                L"Microsoft YaHei UI"
            };
            for (const wchar_t* name : kLatinStack) {
                Gdiplus::FontFamily fam(name);
                if (fam.IsAvailable()) return name;
            }
            return L"Segoe UI";
        }();
        return preferLatinDigits ? s_latinResolved : s_cjkResolved;
    }

    /// 计算文本标注高反差外扩描边线宽（与 MarkupEngine 渲染 100% 单一事实源同构）
    static float computeOutlineStrokeWidth(int fontSize) noexcept {
        const int safeFontSize = (std::max)(12, fontSize);
        return (std::max)(3.4f, static_cast<float>(safeFontSize) * 0.18f);
    }

    /// 根据字号与外扩描边半宽动态计算文本绘制起点安全边距，彻底杜绝大字号描边被位图边缘切平
    static std::pair<float, float> computeTextOrigin(int fontSize) noexcept {
        const float strokeHalf = computeOutlineStrokeWidth(fontSize) * 0.5f;
        const float strokePad = std::ceil(strokeHalf + 2.0f);
        const float originX = (std::max)(6.0f, strokePad + 2.0f);
        const float originY = (std::max)(4.0f, strokePad);
        return {originX, originY};
    }

    /// 文本框水平与垂直总内边距（SSOT：度量、换行可用宽度、渲染与命中测试完全共用）
    static int computeBoxHorizontalPadding(int fontSize) noexcept {
        const auto [originX, originY] = computeTextOrigin((std::max)(12, fontSize));
        (void)originY;
        return static_cast<int>(std::ceil(originX * 2.0f + 4.0f));
    }

    static int computeBoxVerticalPadding(int fontSize) noexcept {
        const auto [originX, originY] = computeTextOrigin((std::max)(12, fontSize));
        (void)originX;
        return static_cast<int>(std::ceil(originY * 2.0f + 2.0f));
    }

    static constexpr float kRightBoundarySafetyMarginPx = 6.0f;
    static constexpr float kBottomBoundarySafetyMarginPx = 6.0f;

    /// 根据截图画布宽度与文本框左起点，计算不越出右边界的最大可用外框宽度（<= 0.0f 表示不限宽）
    static float computeAvailableBoxWidth(int canvasWidth, int boxStartX, int fontSize,
                                          float rightSafetyMargin = kRightBoundarySafetyMarginPx) noexcept {
        if (canvasWidth <= 0) return 0.0f;
        const int safeFontSize = (std::max)(12, fontSize);
        const float padW = static_cast<float>(computeBoxHorizontalPadding(safeFontSize));
        const float minReadableBoxW = (std::max)(44.0f, static_cast<float>(safeFontSize) * 1.8f + padW);
        const float canvasF = static_cast<float>(canvasWidth);
        const float clampedMinW = (std::min)(canvasF, minReadableBoxW);
        const float remaining = canvasF - static_cast<float>(boxStartX) - rightSafetyMargin;
        if (remaining <= clampedMinW) {
            return clampedMinW;
        }
        return remaining;
    }

    /// 将外框最大宽度换算为排版内容区最大宽度（扣除左右描边与聚焦框安全内边距）
    static float computeContentWidthFromBoxWidth(float maxBoxWidth, int fontSize) noexcept {
        if (maxBoxWidth <= 0.0f) return 0.0f;
        const int safeFontSize = (std::max)(12, fontSize);
        const float padW = static_cast<float>(computeBoxHorizontalPadding(safeFontSize));
        return (std::max)(static_cast<float>(safeFontSize) * 1.15f, maxBoxWidth - padW);
    }

    /// 根据排版快照计算外框像素尺寸（SSOT：与渲染画布大小 100% 一致；支持用户自定义独立拉宽与拉长）
    static std::pair<int, int> computeBoxSize(const MultilineTextLayout& layout, int fontSize,
                                              float maxBoxWidth = 0.0f,
                                              bool fixedBoxWidth = false,
                                              float minBoxHeight = 0.0f) noexcept {
        const int safeFontSize = (std::max)(12, fontSize);
        const int padW = computeBoxHorizontalPadding(safeFontSize);
        const int padH = computeBoxVerticalPadding(safeFontSize);
        int w = static_cast<int>(std::ceil(layout.contentWidth)) + padW;
        if (maxBoxWidth > 0.0f) {
            const int maxWInt = static_cast<int>(std::ceil(maxBoxWidth));
            // 仅夹取行尾空白或小数舍入误差；若单字本身宽度超过极窄框，保留完整字形不裁切
            const int minSingleGlyphBox = static_cast<int>(std::ceil(static_cast<float>(safeFontSize) * 1.15f)) + padW;
            const int clampedTargetW = (std::max)(maxWInt, minSingleGlyphBox);
            if (fixedBoxWidth) {
                w = (std::max)(w, clampedTargetW);
            } else {
                w = (std::min)(w, clampedTargetW);
            }
        }
        int h = static_cast<int>(std::ceil(layout.contentHeight)) + padH;
        if (minBoxHeight > 0.0f) {
            h = (std::max)(h, static_cast<int>(std::ceil(minBoxHeight)));
        }
        return {(std::max)(w, 24), (std::max)(h, safeFontSize + padH)};
    }

    /// 新建文本框时对紧贴截图右/底边缘的点击落点进行温和礼让，避免首行仅剩极窄列宽或掉出底边
    static std::pair<int, int> computeSmartTextStartPoint(int clickX, int clickY,
                                                          int canvasWidth, int canvasHeight,
                                                          int fontSize) noexcept {
        if (canvasWidth <= 0 || canvasHeight <= 0) {
            return {clickX, clickY};
        }
        const int safeFontSize = (std::max)(12, fontSize);
        const int padW = computeBoxHorizontalPadding(safeFontSize);
        const int padH = computeBoxVerticalPadding(safeFontSize);
        const int margin = 4;
        const int preferredMinWidth = static_cast<int>(std::ceil(
            (std::max)(140.0f, static_cast<float>(safeFontSize) * 4.5f + static_cast<float>(padW))));
        const int minInitialHeight = static_cast<int>(std::ceil(
            static_cast<float>(safeFontSize) * 1.45f + static_cast<float>(padH)));

        int outX = clickX;
        int outY = clickY;
        const int maxComfortX = canvasWidth - static_cast<int>(std::ceil(kRightBoundarySafetyMarginPx)) - preferredMinWidth;
        if (outX > maxComfortX) {
            outX = (std::max)(margin, maxComfortX);
        }
        const int maxComfortY = canvasHeight - static_cast<int>(std::ceil(kBottomBoundarySafetyMarginPx)) - minInitialHeight;
        if (outY > maxComfortY) {
            outY = (std::max)(margin, maxComfortY);
        }
        return {outX, outY};
    }

    /// 自动换行增高时，将越过截图底边界的文本框温和上抬，确保正在输入的新行始终完整可见
    static int computeBottomSafeStartY(int currentStartY, int boxHeight, int canvasHeight) noexcept {
        if (canvasHeight <= 0 || boxHeight <= 0) return currentStartY;
        const int bottomLimit = canvasHeight - static_cast<int>(std::ceil(kBottomBoundarySafetyMarginPx));
        if (currentStartY + boxHeight <= bottomLimit) {
            return currentStartY;
        }
        const int lifted = bottomLimit - boxHeight;
        return (std::max)(4, lifted);
    }

    static bool isLatinWordChar(wchar_t ch) noexcept {
        return ch == L'_' || ch == L'\'' ||
               (ch >= L'0' && ch <= L'9') ||
               (ch >= L'A' && ch <= L'Z') ||
               (ch >= L'a' && ch <= L'z') ||
               (ch >= 0x00C0 && ch <= 0x024F) ||
               (ch >= 0x0400 && ch <= 0x04FF);
    }

    static bool isLineStartProhibitedPunct(wchar_t ch) noexcept {
        switch (ch) {
            case L',': case L'.': case L'!': case L'?': case L';': case L':':
            case L')': case L']': case L'}': case L'%':
            case L'\uFF0C': case L'\u3002': case L'\uFF01': case L'\uFF1F':
            case L'\uFF1B': case L'\uFF1A': case L'\u3001': case L'\uFF09':
            case L'\u3011': case L'\u300B': case L'\u300D': case L'\u300F':
            case L'\u201D': case L'\u2019': case L'\u2026':
                return true;
            default:
                return false;
        }
    }

    /// 构建与 GDI+ GraphicsPath::AddString 完全对齐的多行字符边界度量表（支持可选 maxContentWidth 边界自动软换行）
    static MultilineTextLayout buildLayout(const std::wstring& wtext, int fontSize, float maxContentWidth = 0.0f) {
        const int safeFontSize = (std::max)(12, fontSize);
        const float safeMaxContentW = (maxContentWidth > 0.5f) ? maxContentWidth : 0.0f;
        const int maxWidthTenths = (safeMaxContentW > 0.0f)
            ? static_cast<int>(std::lround(safeMaxContentW * 10.0f))
            : 0;

        struct CachedLayoutEntry {
            std::wstring text;
            int fontPx = 0;
            int maxWidthTenths = 0;
            MultilineTextLayout layout;
        };
        static std::mutex s_cacheMutex;
        static std::deque<CachedLayoutEntry> s_layoutCache;
        {
            std::lock_guard<std::mutex> lock(s_cacheMutex);
            for (auto it = s_layoutCache.begin(); it != s_layoutCache.end(); ++it) {
                if (it->fontPx == safeFontSize &&
                    it->maxWidthTenths == maxWidthTenths &&
                    it->text == wtext) {
                    MultilineTextLayout hit = it->layout;
                    if (it != s_layoutCache.begin()) {
                        CachedLayoutEntry moved = std::move(*it);
                        s_layoutCache.erase(it);
                        s_layoutCache.push_front(std::move(moved));
                    }
                    return hit;
                }
            }
        }

        initializeMarkupTextRenderer();

        MultilineTextLayout layout;
        const float fSize = static_cast<float>(safeFontSize);
        const auto [originX, originY] = computeTextOrigin(safeFontSize);
        layout.textOriginX = originX;
        layout.textOriginY = originY;
        layout.glyphHeight = fSize * 1.16f;
        layout.lineStep = fSize * 1.32f;
        layout.maxContentWidth = safeMaxContentW;

        Gdiplus::FontFamily primaryFamily(resolveMarkupFontFamilyName(false));
        Gdiplus::FontFamily fallbackFamily(L"Segoe UI");
        const Gdiplus::FontFamily* activeFamily = primaryFamily.IsAvailable()
            ? &primaryFamily
            : (fallbackFamily.IsAvailable() ? &fallbackFamily : nullptr);

        if (activeFamily && activeFamily->IsAvailable()) {
            const UINT16 emHeight = activeFamily->GetEmHeight(Gdiplus::FontStyleBold);
            const UINT16 lineSpacing = activeFamily->GetLineSpacing(Gdiplus::FontStyleBold);
            const UINT16 cellAscent = activeFamily->GetCellAscent(Gdiplus::FontStyleBold);
            const UINT16 cellDescent = activeFamily->GetCellDescent(Gdiplus::FontStyleBold);
            if (emHeight > 0) {
                layout.lineStep = fSize * static_cast<float>(lineSpacing) / static_cast<float>(emHeight);
                layout.glyphHeight = fSize * static_cast<float>(cellAscent + cellDescent) / static_cast<float>(emHeight);
            }
        }

        Gdiplus::Bitmap tmpBmp(1, 1, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&tmpBmp);
        std::unique_ptr<Gdiplus::Font> font;
        if (activeFamily && activeFamily->IsAvailable()) {
            font = std::make_unique<Gdiplus::Font>(activeFamily, fSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        }

        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetAlignment(Gdiplus::StringAlignmentNear);
        format.SetLineAlignment(Gdiplus::StringAlignmentNear);
        format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

        float sentinelW = 0.0f;
        if (font && font->IsAvailable()) {
            Gdiplus::RectF sRect{};
            g.MeasureString(L"|", 1, font.get(), Gdiplus::PointF(0.0f, 0.0f), &format, &sRect);
            sentinelW = sRect.Width;
        }

        std::wstring probe;
        probe.reserve(std::min<size_t>(wtext.size() + 2, 1024));
        auto measureSpanWidth = [&](const std::wstring& src, size_t startOff, size_t len) -> float {
            if (len == 0) return 0.0f;
            if (!font || !font->IsAvailable()) {
                float approx = 0.0f;
                for (size_t i = 0; i < len; ++i) {
                    approx += (src[startOff + i] > 0x7F) ? fSize : (fSize * 0.56f);
                }
                return approx;
            }
            // 哨兵差分法：确保末尾连续空格、制表符及 CJK/Latin 混排字距 100% 精确不丢失
            probe.clear();
            probe.append(src, startOff, len);
            probe.push_back(L'|');
            Gdiplus::RectF bRect{};
            g.MeasureString(probe.c_str(), static_cast<INT>(probe.size()), font.get(),
                            Gdiplus::PointF(0.0f, 0.0f), &format, &bRect);
            return std::max(0.0f, bRect.Width - sentinelW);
        };

        auto emitVisualLine = [&](size_t startIdx, size_t endIdx, bool softWrapped, size_t lineIdx) {
            TextLineLayout line;
            line.startIndex = startIdx;
            line.endIndex = endIdx;
            line.softWrapped = softWrapped;
            line.topY = layout.textOriginY + static_cast<float>(lineIdx) * layout.lineStep;
            line.height = layout.glyphHeight;

            const size_t len = endIdx - startIdx;
            line.charOffsets.resize(len + 1, 0.0f);
            float prevX = 0.0f;
            for (size_t k = 1; k <= len; ++k) {
                if (isLowSurrogateChar(wtext[startIdx + k - 1]) && k >= 2 &&
                    isHighSurrogateChar(wtext[startIdx + k - 2])) {
                    line.charOffsets[k] = prevX;
                    continue;
                }
                prevX = std::max(prevX, measureSpanWidth(wtext, startIdx, k));
                line.charOffsets[k] = prevX;
            }
            // 对齐高代理项的中间位置至完整代理对末尾，防止落入半个码点
            for (size_t k = 1; k < len; ++k) {
                if (isHighSurrogateChar(wtext[startIdx + k - 1]) && isLowSurrogateChar(wtext[startIdx + k])) {
                    const float endX = measureSpanWidth(wtext, startIdx, k + 1);
                    line.charOffsets[k] = endX;
                    line.charOffsets[k + 1] = endX;
                }
            }

            float visualWidth = line.charOffsets.back();
            if (softWrapped && safeMaxContentW > 0.0f && visualWidth > safeMaxContentW) {
                // 软换行行尾被吸附的纯空白不撑破右边界，仅保留可见墨水宽度与限宽中的较大值
                size_t trimmedLen = len;
                while (trimmedLen > 0 && std::iswspace(wtext[startIdx + trimmedLen - 1])) {
                    --trimmedLen;
                }
                const float inkWidth = line.charOffsets[trimmedLen];
                visualWidth = std::max(inkWidth, std::min(visualWidth, safeMaxContentW));
            }
            layout.contentWidth = std::max(layout.contentWidth, visualWidth);
            layout.lines.push_back(std::move(line));
        };

        size_t pos = 0;
        size_t lineIndex = 0;
        while (true) {
            const size_t nl = wtext.find(L'\n', pos);
            const size_t paraEnd = (nl == std::wstring::npos) ? wtext.size() : nl;
            const size_t paraLen = paraEnd - pos;

            if (safeMaxContentW <= 0.0f || paraLen == 0) {
                emitVisualLine(pos, paraEnd, false, lineIndex);
                ++lineIndex;
            } else {
                const float wrapLimit = safeMaxContentW + 0.25f;
                size_t segStart = pos;
                while (segStart < paraEnd) {
                    const size_t remLen = paraEnd - segStart;
                    if (measureSpanWidth(wtext, segStart, remLen) <= wrapLimit) {
                        emitVisualLine(segStart, paraEnd, false, lineIndex);
                        ++lineIndex;
                        segStart = paraEnd;
                        break;
                    }

                    // 保底每条视觉行至少消费 1 个完整 UTF-16 字簇，防止极窄框死循环
                    const size_t minNext = std::min(paraEnd, nextClusterIndex(wtext, segStart));
                    size_t fitEnd = minNext;
                    if (minNext < paraEnd && measureSpanWidth(wtext, segStart, minNext - segStart) <= wrapLimit) {
                        size_t lo = minNext;
                        size_t hi = paraEnd;
                        while (true) {
                            const size_t nextLo = nextClusterIndex(wtext, lo);
                            if (nextLo >= hi) break;
                            size_t mid = clampIndex(wtext, lo + (hi - lo) / 2);
                            if (mid <= lo) mid = nextLo;
                            if (mid >= hi) break;
                            if (measureSpanWidth(wtext, segStart, mid - segStart) <= wrapLimit) {
                                lo = mid;
                            } else {
                                hi = mid;
                            }
                        }
                        fitEnd = lo;
                    }

                    size_t breakEnd = fitEnd;
                    if (breakEnd < paraEnd) {
                        if (std::iswspace(wtext[breakEnd])) {
                            // 吸附紧随其后的空格留在当前行尾，杜绝下一行产生莫名行首缩进
                            while (breakEnd < paraEnd && std::iswspace(wtext[breakEnd])) {
                                breakEnd = std::min(paraEnd, nextClusterIndex(wtext, breakEnd));
                            }
                        } else if (breakEnd > segStart &&
                                   isLatinWordChar(wtext[breakEnd - 1]) &&
                                   isLatinWordChar(wtext[breakEnd])) {
                            // 英文/西里尔/数字单词不断腰：向前寻找最近的空白、连字符或非词边界
                            size_t scan = prevClusterIndex(wtext, breakEnd);
                            while (scan > segStart) {
                                const wchar_t sc = wtext[scan];
                                if (std::iswspace(sc) || sc == L'-' || sc == L'/' || sc == L'\\') {
                                    const size_t cand = std::min(paraEnd, nextClusterIndex(wtext, scan));
                                    if (cand > segStart && cand < breakEnd) {
                                        breakEnd = cand;
                                    }
                                    break;
                                }
                                if (!isLatinWordChar(sc)) {
                                    const size_t cand = std::min(paraEnd, nextClusterIndex(wtext, scan));
                                    if (cand > segStart && cand < breakEnd) {
                                        breakEnd = cand;
                                    }
                                    break;
                                }
                                scan = prevClusterIndex(wtext, scan);
                            }
                        } else if (isLineStartProhibitedPunct(wtext[breakEnd])) {
                            // 简易避头标点保护：避免下一视觉行以孤立的逗号、句号或右括号开头
                            const size_t prevPos = prevClusterIndex(wtext, breakEnd);
                            if (prevPos > segStart &&
                                !std::iswspace(wtext[prevPos]) &&
                                !isLineStartProhibitedPunct(wtext[prevPos])) {
                                breakEnd = prevPos;
                            }
                        }
                    }

                    if (breakEnd <= segStart) {
                        breakEnd = minNext;
                    }
                    const bool isSoft = (breakEnd < paraEnd);
                    emitVisualLine(segStart, breakEnd, isSoft, lineIndex);
                    ++lineIndex;
                    segStart = breakEnd;
                }
            }

            if (nl == std::wstring::npos) break;
            pos = nl + 1;
        }

        if (layout.lines.empty()) {
            TextLineLayout emptyLine;
            emptyLine.topY = layout.textOriginY;
            emptyLine.height = layout.glyphHeight;
            emptyLine.charOffsets = {0.0f};
            layout.lines.push_back(std::move(emptyLine));
        }

        layout.contentHeight =
            static_cast<float>(layout.lines.size() - 1) * layout.lineStep + layout.glyphHeight;

        if (wtext.size() <= 8192) {
            std::lock_guard<std::mutex> lock(s_cacheMutex);
            s_layoutCache.push_front(CachedLayoutEntry{wtext, safeFontSize, maxWidthTenths, layout});
            while (s_layoutCache.size() > 16) {
                s_layoutCache.pop_back();
            }
        }

        return layout;
    }

    /// 根据文本框局部相对像素坐标 (relX, relY) 反算最贴近的光标字符索引
    static size_t hitTestCaretIndex(const MultilineTextLayout& layout, const std::wstring& wtext,
                                    float relX, float relY) noexcept {
        if (layout.lines.empty() || wtext.empty()) return 0;

        int lineIdx = 0;
        if (relY >= layout.textOriginY) {
            lineIdx = static_cast<int>(std::floor((relY - layout.textOriginY) / std::max(1.0f, layout.lineStep)));
        }
        lineIdx = std::clamp(lineIdx, 0, static_cast<int>(layout.lines.size()) - 1);
        const auto& line = layout.lines[static_cast<size_t>(lineIdx)];
        const float targetX = relX - layout.textOriginX;
        if (targetX <= 0.0f || line.endIndex <= line.startIndex) {
            return line.startIndex;
        }

        const size_t lineLen = line.endIndex - line.startIndex;
        size_t bestCol = lineLen;
        for (size_t col = 0; col < lineLen;) {
            size_t nextCol = col + 1;
            if (line.startIndex + col + 1 < line.endIndex &&
                isHighSurrogateChar(wtext[line.startIndex + col]) &&
                isLowSurrogateChar(wtext[line.startIndex + col + 1])) {
                nextCol = col + 2;
            }
            const float x0 = line.charOffsets[col];
            const float x1 = line.charOffsets[nextCol];
            const float mid = (x0 + x1) * 0.5f;
            if (targetX < mid) {
                bestCol = col;
                break;
            }
            col = nextCol;
        }
        return clampIndex(wstrOrEmpty(wtext), line.startIndex + bestCol);
    }

    /// 计算当前光标的精确像素视觉坐标
    static TextCaretVisual computeCaretVisual(const MultilineTextLayout& layout, const std::wstring& wtext,
                                              size_t caret) noexcept {
        TextCaretVisual vis;
        vis.x = layout.textOriginX + 1.0f;
        vis.y = layout.textOriginY + 1.0f;
        vis.height = layout.glyphHeight;
        if (layout.lines.empty()) return vis;

        const size_t clamped = clampIndex(wtext, caret);
        const size_t lineIdx = findVisualLineIndex(layout, clamped);
        const auto& line = layout.lines[lineIdx];
        const size_t col = (clamped >= line.startIndex) ? (clamped - line.startIndex) : 0;
        const float dx = (col < line.charOffsets.size()) ? line.charOffsets[col] : line.charOffsets.back();
        vis.x = layout.textOriginX + dx + 1.0f;
        vis.y = line.topY + 1.0f;
        vis.height = line.height;
        return vis;
    }

    /// 上下方向键跨行移动光标（保持期望水平像素列对齐 preferredX，原生支持自动软换行视觉行）
    static void moveVertical(const MultilineTextLayout& layout, const std::wstring& wtext,
                             size_t& caret, size_t& anchor, int lineDelta,
                             bool shiftExtend, float& preferredX) noexcept {
        if (layout.lines.empty()) return;
        caret = clampIndex(wtext, caret);
        anchor = clampIndex(wtext, anchor);

        if (!shiftExtend && caret != anchor) {
            caret = anchor = (lineDelta < 0) ? std::min(caret, anchor) : std::max(caret, anchor);
            preferredX = -1.0f;
            return;
        }

        const size_t curLine = findVisualLineIndex(layout, caret);

        if (preferredX < 0.0f) {
            const auto vis = computeCaretVisual(layout, wtext, caret);
            preferredX = vis.x;
        }

        const int targetLine = static_cast<int>(curLine) + lineDelta;
        if (targetLine < 0) {
            caret = 0;
        } else if (targetLine >= static_cast<int>(layout.lines.size())) {
            caret = wtext.size();
        } else {
            const auto& dstLine = layout.lines[static_cast<size_t>(targetLine)];
            size_t nextCaret = hitTestCaretIndex(layout, wtext, preferredX, dstLine.topY + dstLine.height * 0.5f);
            // 若目标视觉行是软换行行且落点超过行尾，夹回该行末字簇前，防止弹回下一视觉行行首导致上下键卡滞
            if (dstLine.softWrapped && nextCaret >= dstLine.endIndex && dstLine.endIndex > dstLine.startIndex) {
                nextCaret = prevClusterIndex(wtext, dstLine.endIndex);
            }
            caret = nextCaret;
        }

        if (!shiftExtend) anchor = caret;
    }

    /// 生成多行选区高亮圆角底块列表（精确区分硬换行与自动软换行）
    static std::vector<TextHighlightSpan> computeSelectionHighlights(
        const MultilineTextLayout& layout, const std::wstring& wtext,
        size_t caret, size_t anchor) {
        std::vector<TextHighlightSpan> spans;
        const auto [selStart, selEnd] = normalizedRange(wtext, caret, anchor);
        if (selEnd <= selStart || layout.lines.empty()) return spans;

        for (size_t i = 0; i < layout.lines.size(); ++i) {
            const auto& line = layout.lines[i];
            const bool hasHardNewline = (!line.softWrapped) && (i + 1 < layout.lines.size());
            const size_t lineMaxWithNewline = hasHardNewline ? (line.endIndex + 1) : line.endIndex;
            if (selEnd <= line.startIndex || selStart >= lineMaxWithNewline) continue;

            const size_t segStart = std::clamp(selStart, line.startIndex, line.endIndex) - line.startIndex;
            const size_t segEnd = std::clamp(selEnd, line.startIndex, line.endIndex) - line.startIndex;
            const float x0 = line.charOffsets[std::min(segStart, line.charOffsets.size() - 1)];
            float x1 = line.charOffsets[std::min(segEnd, line.charOffsets.size() - 1)];

            // 仅当选区跨过真实硬换行符 '\n' 时附加 6px 换行提示宽；软换行视觉折行紧贴字尾无冗余尾巴
            if (hasHardNewline && selEnd > line.endIndex) {
                x1 += 6.0f;
            }
            if (x1 > x0 + 0.5f) {
                spans.push_back({layout.textOriginX + x0, line.topY, x1 - x0, line.height});
            }
        }
        return spans;
    }

private:
    static const std::wstring& wstrOrEmpty(const std::wstring& s) noexcept { return s; }
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_CAPTURETEXTEDITOR_H

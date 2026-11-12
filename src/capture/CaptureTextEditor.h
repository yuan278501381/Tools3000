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
    size_t startIndex = 0;          // 本行首字符在 wstring 中的索引
    size_t endIndex = 0;            // 本行末字符后一位索引（不含 '\n'）
    float topY = 0.0f;              // 相对于文本框左上角的 Y 偏移
    float height = 0.0f;            // 单行视觉高度
    std::vector<float> charOffsets; // 长度为 (endIndex - startIndex + 1)，记录每个字符边界相对于 textX 的像素偏移
};

struct MultilineTextLayout {
    float textOriginX = 6.0f;
    float textOriginY = 4.0f;
    float lineStep = 20.0f;
    float glyphHeight = 18.0f;
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

    /// 构建与 GDI+ GraphicsPath::AddString 完全对齐的多行字符边界度量表
    static MultilineTextLayout buildLayout(const std::wstring& wtext, int fontSize) {
        const int safeFontSize = (std::max)(12, fontSize);

        struct CachedLayoutEntry {
            std::wstring text;
            int fontPx = 0;
            MultilineTextLayout layout;
        };
        static std::mutex s_cacheMutex;
        static std::deque<CachedLayoutEntry> s_layoutCache;
        {
            std::lock_guard<std::mutex> lock(s_cacheMutex);
            for (auto it = s_layoutCache.begin(); it != s_layoutCache.end(); ++it) {
                if (it->fontPx == safeFontSize && it->text == wtext) {
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
        auto measurePrefixWidth = [&](const std::wstring& lineStr, size_t prefixLen) -> float {
            if (prefixLen == 0) return 0.0f;
            if (!font || !font->IsAvailable()) {
                float approx = 0.0f;
                for (size_t i = 0; i < prefixLen; ++i) {
                    approx += (lineStr[i] > 0x7F) ? fSize : (fSize * 0.56f);
                }
                return approx;
            }
            // 哨兵差分法：确保末尾连续空格、制表符及 CJK/Latin 混排字距 100% 精确不丢失
            probe.clear();
            probe.append(lineStr, 0, prefixLen);
            probe.push_back(L'|');
            Gdiplus::RectF bRect{};
            g.MeasureString(probe.c_str(), static_cast<INT>(probe.size()), font.get(),
                            Gdiplus::PointF(0.0f, 0.0f), &format, &bRect);
            return std::max(0.0f, bRect.Width - sentinelW);
        };

        size_t pos = 0;
        size_t lineIndex = 0;
        while (true) {
            const size_t nl = wtext.find(L'\n', pos);
            const size_t end = (nl == std::wstring::npos) ? wtext.size() : nl;
            TextLineLayout line;
            line.startIndex = pos;
            line.endIndex = end;
            line.topY = layout.textOriginY + static_cast<float>(lineIndex) * layout.lineStep;
            line.height = layout.glyphHeight;

            const size_t len = end - pos;
            const std::wstring lineSub = wtext.substr(pos, len);
            line.charOffsets.resize(len + 1, 0.0f);
            float prevX = 0.0f;
            for (size_t k = 1; k <= len; ++k) {
                if (isLowSurrogateChar(lineSub[k - 1]) && k >= 2 && isHighSurrogateChar(lineSub[k - 2])) {
                    line.charOffsets[k] = prevX;
                    continue;
                }
                prevX = std::max(prevX, measurePrefixWidth(lineSub, k));
                line.charOffsets[k] = prevX;
            }
            // 对齐高代理项的中间位置至完整代理对末尾，防止落入半个码点
            for (size_t k = 1; k < len; ++k) {
                if (isHighSurrogateChar(lineSub[k - 1]) && isLowSurrogateChar(lineSub[k])) {
                    const float endX = measurePrefixWidth(lineSub, k + 1);
                    line.charOffsets[k] = endX;
                    line.charOffsets[k + 1] = endX;
                }
            }

            layout.lines.push_back(std::move(line));
            if (nl == std::wstring::npos) break;
            pos = nl + 1;
            ++lineIndex;
        }

        if (layout.lines.empty()) {
            TextLineLayout emptyLine;
            emptyLine.topY = layout.textOriginY;
            emptyLine.height = layout.glyphHeight;
            emptyLine.charOffsets = {0.0f};
            layout.lines.push_back(std::move(emptyLine));
        }

        if (wtext.size() <= 8192) {
            std::lock_guard<std::mutex> lock(s_cacheMutex);
            s_layoutCache.push_front(CachedLayoutEntry{wtext, safeFontSize, layout});
            while (s_layoutCache.size() > 8) {
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
        for (size_t i = 0; i < layout.lines.size(); ++i) {
            const auto& line = layout.lines[i];
            if (clamped >= line.startIndex && clamped <= line.endIndex) {
                const size_t col = clamped - line.startIndex;
                const float dx = (col < line.charOffsets.size()) ? line.charOffsets[col] : line.charOffsets.back();
                vis.x = layout.textOriginX + dx + 1.0f;
                vis.y = line.topY + 1.0f;
                vis.height = line.height;
                return vis;
            }
        }
        const auto& last = layout.lines.back();
        vis.x = layout.textOriginX + last.charOffsets.back() + 1.0f;
        vis.y = last.topY + 1.0f;
        vis.height = last.height;
        return vis;
    }

    /// 上下方向键跨行移动光标（保持期望水平像素列对齐 preferredX）
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

        size_t curLine = 0;
        for (size_t i = 0; i < layout.lines.size(); ++i) {
            if (caret >= layout.lines[i].startIndex && caret <= layout.lines[i].endIndex) {
                curLine = i;
                break;
            }
        }

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
            caret = hitTestCaretIndex(layout, wtext, preferredX, dstLine.topY + dstLine.height * 0.5f);
        }

        if (!shiftExtend) anchor = caret;
    }

    /// 生成多行选区高亮圆角底块列表
    static std::vector<TextHighlightSpan> computeSelectionHighlights(
        const MultilineTextLayout& layout, const std::wstring& wtext,
        size_t caret, size_t anchor) {
        std::vector<TextHighlightSpan> spans;
        const auto [selStart, selEnd] = normalizedRange(wtext, caret, anchor);
        if (selEnd <= selStart || layout.lines.empty()) return spans;

        for (size_t i = 0; i < layout.lines.size(); ++i) {
            const auto& line = layout.lines[i];
            const size_t lineMaxWithNewline = (i + 1 < layout.lines.size()) ? (line.endIndex + 1) : line.endIndex;
            if (selEnd <= line.startIndex || selStart >= lineMaxWithNewline) continue;

            const size_t segStart = std::clamp(selStart, line.startIndex, line.endIndex) - line.startIndex;
            const size_t segEnd = std::clamp(selEnd, line.startIndex, line.endIndex) - line.startIndex;
            const float x0 = line.charOffsets[std::min(segStart, line.charOffsets.size() - 1)];
            float x1 = line.charOffsets[std::min(segEnd, line.charOffsets.size() - 1)];

            // 若选区跨过了本行末尾换行符，附加 6px 换行选区提示宽，清晰反馈空行/换行已被包含在复制范围内
            if (selEnd > line.endIndex && i + 1 < layout.lines.size()) {
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

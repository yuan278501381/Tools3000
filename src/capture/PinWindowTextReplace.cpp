// ─────────────────────────────────────────────────────────────────────────────
// PinWindowTextReplace.cpp — 贴图窗口原位无痕改字编辑子模块
//
// 职责:
//   - 原位改字编辑态启动、二次编辑、提交与取消回滚
//   - IME 候选框坐标锚定、Unicode 字符输入与全套键位控制（选区/撤销/剪贴板/导航）
//   - 原位改字精调浮岛热区重置与 Direct2D 叠加参数构造
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinWindowInternal.h"
#include "capture/TextReplaceEngine.h"
#include "core/events/EventBus.h"
#include "core/logger/Logger.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"
#include "ocr/OcrEngine.h"

#include <imm.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace tools3000::capture {

using namespace pin_detail;

void PinWindow::clearTextReplaceToolbarRects() noexcept {
    m_btnTrFontDecRect = {};
    m_btnTrFontIncRect = {};
    m_btnTrWeightRect = {};
    m_btnTrFamilyRect = {};
    m_btnTrBgModeRect = {};
    m_btnTrReflowRect = {};
    m_btnTrDoneRect = {};
    m_trDragHandleRect = {};
    m_trEditBoxClientRect = {};
}

namespace pin_detail {
TextReplaceRenderSpec buildTextReplaceSpecForOverlay(const MarkupElement& el) {
    TextReplaceRenderSpec spec;
    spec.sourceRect = el.replaceSourceRect;
    spec.charBoxes = el.replaceCharBoxes;
    spec.originalTextUtf8 = el.replaceOriginalText;
    spec.newTextUtf8 = el.text;
    spec.textBgr = cv::Vec3b(el.color.b, el.color.g, el.color.r);
    spec.fontSizePx = el.fontSize;
    spec.fontWeight = el.replaceFontWeight;
    spec.fontClass = static_cast<TextReplaceFontClass>(std::clamp(el.replaceFontClass, 0, 2));
    spec.offsetX = el.replaceOffsetX;
    spec.offsetY = el.replaceOffsetY;
    spec.bgMode = static_cast<TextReplaceBgMode>(std::clamp(el.replaceBgMode, 0, 2));
    spec.styleInitialized = el.replaceStyleInitialized;
    spec.reflowRightTail = el.replaceReflowRight;
    spec.rightTailRect = el.replaceRightTailRect;
    spec.isEditing = true;
    spec.caretUtf16 = el.textCaret;
    spec.selAnchorUtf16 = el.textSelAnchor;
    spec.lastCaretTick = el.textLastCaretTick;
    spec.showCaretBlink = true;
    return spec;
}
} // namespace pin_detail

void PinWindow::updateImeCompositionAnchor() noexcept {
    if (!m_hwnd || m_editingTextReplaceIdx < 0 ||
        m_editingTextReplaceIdx >= static_cast<int>(m_markupElements.size())) {
        return;
    }
    HIMC himc = ImmGetContext(m_hwnd);
    if (!himc) return;
    const auto& el = m_markupElements[static_cast<size_t>(m_editingTextReplaceIdx)];
    const auto ov = TextReplaceEngine::computeEditOverlayMetrics(
        m_cleanBaseImage, buildTextReplaceSpecForOverlay(el));
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    if (ov.valid) {
        cf.ptCurrentPos.x = static_cast<LONG>(std::lround(ov.caretX * m_scale));
        cf.ptCurrentPos.y = static_cast<LONG>(std::lround((ov.caretBottom + 3.0f) * m_scale));
    } else {
        const cv::Rect& rc = el.replaceSourceRect;
        cf.ptCurrentPos.x = static_cast<LONG>(std::lround((static_cast<float>(rc.x) + el.replaceOffsetX) * m_scale));
        cf.ptCurrentPos.y = static_cast<LONG>(std::lround((static_cast<float>(rc.y + rc.height) + el.replaceOffsetY + 4.0f) * m_scale));
    }
    ImmSetCompositionWindow(himc, &cf);
    ImmReleaseContext(m_hwnd, himc);
}

bool PinWindow::startInPlaceTextReplace() {
    if (m_folded || !m_pinnedSourceText.empty()) return false;
    ensureImagesMaterialized();
    if (m_cleanBaseImage.empty()) return false;
    if (m_editingTextReplaceIdx >= 0) {
        commitInPlaceTextReplace(true);
    }

    std::vector<cv::Rect> spans;
    std::vector<cv::Rect> selCharBoxes;
    std::string origText;
    cv::Rect detectedRightTail{0, 0, 0, 0};
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        spans = collectCoalescedSelectedTextRectsLocked();
        origText = assembleSelectedSpansText(m_ocrBlocks, m_selectedBlockIndices, m_selectedCharSpans);
        // 收集对应原始字符框供等宽/字距克隆参考，并同步探测同行右侧剩余原图文字（供改长自动顺延让位）
        int anchorRegionId = -1;
        int anchorRowId = -1;
        const cv::Rect firstSpan = spans.empty() ? cv::Rect() : spans.front();
        const int targetRightX = firstSpan.x + firstSpan.width;
        const int targetTopY = firstSpan.y;
        const int targetBotY = firstSpan.y + firstSpan.height;
        auto unionTail = [&](const cv::Rect& r) {
            if (r.width <= 0 || r.height <= 0) return;
            if (detectedRightTail.width <= 0 || detectedRightTail.height <= 0) {
                detectedRightTail = r;
            } else {
                detectedRightTail |= r;
            }
        };

        if (!m_selectedCharSpans.empty()) {
            const auto& firstSp = m_selectedCharSpans.front();
            if (firstSp.blockIndex < m_ocrBlocks.size()) {
                const auto& blk = m_ocrBlocks[firstSp.blockIndex];
                anchorRegionId = blk.layoutRegionId;
                anchorRowId = blk.alignedRowId;
                for (size_t c = firstSp.startChar; c < firstSp.endExclusive && c < blk.charBoxes.size(); ++c) {
                    selCharBoxes.push_back(blk.charBoxes[c]);
                }
                // 同一 OCR 块内选中后半段之前若仍有未选中的右侧字符，纳入同行右尾顺延区域
                for (size_t c = firstSp.endExclusive; c < blk.charBoxes.size(); ++c) {
                    const cv::Rect& cb = blk.charBoxes[c];
                    if (cb.x >= targetRightX - 2) {
                        unionTail(cb);
                    }
                }
            }
        } else if (!m_selectedBlockIndices.empty() && m_selectedBlockIndices.front() < m_ocrBlocks.size()) {
            const auto& blk = m_ocrBlocks[m_selectedBlockIndices.front()];
            anchorRegionId = blk.layoutRegionId;
            anchorRowId = blk.alignedRowId;
            selCharBoxes = blk.charBoxes;
        }

        // 合并同一版面分区内、同一水平行且位于目标选区右侧的其余 OCR 文本块
        if (!spans.empty() && firstSpan.height > 0) {
            for (const auto& blk : m_ocrBlocks) {
                if (anchorRegionId >= 0 && blk.layoutRegionId != anchorRegionId) continue;
                const cv::Rect& br = blk.origBox;
                if (br.width <= 0 || br.height <= 0) continue;
                if (br.x < targetRightX - 2) continue;
                const int ovTop = (std::max)(targetTopY, br.y);
                const int ovBot = (std::min)(targetBotY, br.y + br.height);
                const int ovH = (std::max)(0, ovBot - ovTop);
                const int minH = (std::max)(1, (std::min)(firstSpan.height, br.height));
                const bool sameRow = (anchorRowId >= 0 && blk.alignedRowId == anchorRowId) ||
                                     (static_cast<float>(ovH) / static_cast<float>(minH) >= 0.55f);
                if (!sameRow) continue;
                unionTail(br);
            }
        }
    }
    if (spans.empty()) return false;

    // 合并为首个目标改字框（支持同行/多词连续替换），若原位已存在改字图元直接重新唤醒二次编辑
    const cv::Rect targetRect = spans.front();
    for (size_t i = 0; i < m_markupElements.size(); ++i) {
        const auto& ex = m_markupElements[i];
        if (ex.tool == MarkupTool::TextReplace) {
            const cv::Rect inter = ex.replaceSourceRect & targetRect;
            if (inter.area() * 2 >= (std::min)(ex.replaceSourceRect.area(), targetRect.area())) {
                return startInPlaceTextReplaceOnElement(i);
            }
        }
    }

    // 去除多行选中时的换行符，聚焦首行精准替换
    const size_t nlPos = origText.find('\n');
    if (nlPos != std::string::npos) {
        origText = origText.substr(0, nlPos);
    }
    while (!origText.empty() && (origText.back() == '\r' || origText.back() == ' ')) {
        origText.pop_back();
    }

    const ExtractedTextStyle dna = TextReplaceEngine::extractStyle(
        m_cleanBaseImage, targetRect, selCharBoxes, origText);

    m_editingTextReplaceBackup = m_markupElements;
    m_editingTextReplaceWasNew = true;

    uint32_t nextId = 1;
    for (const auto& ex : m_markupElements) {
        nextId = (std::max)(nextId, ex.id + 1);
    }

    MarkupElement elem;
    elem.id = nextId;
    elem.tool = MarkupTool::TextReplace;
    elem.startPt = cv::Point(targetRect.x, targetRect.y);
    elem.endPt = cv::Point(targetRect.x + targetRect.width, targetRect.y + targetRect.height);
    elem.replaceOriginalText = origText;
    elem.text = origText;
    elem.replaceSourceRect = targetRect;
    elem.replaceCharBoxes = std::move(selCharBoxes);
    elem.color = MarkupColor{dna.textBgr[2], dna.textBgr[1], dna.textBgr[0], 255};
    elem.fontSize = dna.fontSizePx;
    elem.replaceFontWeight = dna.fontWeight;
    elem.replaceFontClass = static_cast<int>(dna.fontClass);
    elem.replaceOffsetX = 0.0f;
    elem.replaceOffsetY = 0.0f;
    elem.replaceBgMode = static_cast<int>(TextReplaceBgMode::Auto);
    elem.replaceStyleInitialized = true;
    elem.replaceRightTailRect = detectedRightTail & cv::Rect(0, 0, m_cleanBaseImage.cols, m_cleanBaseImage.rows);
    // 纯色/线性渐变背景且检测到同行右侧原图文字时，默认开启智能顺延让位，改长绝不压盖后字
    elem.replaceReflowRight = (!dna.complexBackground &&
                               elem.replaceRightTailRect.width > 0 &&
                               elem.replaceRightTailRect.height > 0);
    elem.isEditing = true;
    const std::wstring wInit = tools3000::core::WinUtils::utf8ToWstring(elem.text);
    elem.textSelAnchor = 0;
    elem.textCaret = wInit.size();
    elem.textLastCaretTick = GetTickCount();

    m_markupElements.push_back(std::move(elem));
    m_editingTextReplaceIdx = static_cast<int>(m_markupElements.size()) - 1;
    m_ocrMarkupPreviewClean = true;

    if (m_hwnd && IsWindow(m_hwnd)) {
        focusWindow();
        SetTimer(m_hwnd, 3, 530, nullptr);
        updateImeCompositionAnchor();
    }
    recompositeMarkupPreservingOcr();
    return true;
}

bool PinWindow::startInPlaceTextReplaceOnElement(size_t elementIdx) {
    ensureImagesMaterialized();
    if (elementIdx >= m_markupElements.size()) return false;
    if (m_markupElements[elementIdx].tool != MarkupTool::TextReplace) return false;
    if (m_editingTextReplaceIdx >= 0 && m_editingTextReplaceIdx != static_cast<int>(elementIdx)) {
        commitInPlaceTextReplace(true);
    }
    if (elementIdx >= m_markupElements.size()) return false;

    m_editingTextReplaceBackup = m_markupElements;
    m_editingTextReplaceWasNew = false;
    m_editingTextReplaceIdx = static_cast<int>(elementIdx);
    auto& el = m_markupElements[elementIdx];
    el.isEditing = true;
    const std::wstring wCur = tools3000::core::WinUtils::utf8ToWstring(el.text);
    el.textSelAnchor = 0;
    el.textCaret = wCur.size();
    el.textLastCaretTick = GetTickCount();
    m_ocrMarkupPreviewClean = true;

    if (m_hwnd && IsWindow(m_hwnd)) {
        focusWindow();
        SetTimer(m_hwnd, 3, 530, nullptr);
        updateImeCompositionAnchor();
    }
    recompositeMarkupPreservingOcr();
    return true;
}

void PinWindow::commitInPlaceTextReplace(bool keepChanges) {
    if (m_editingTextReplaceIdx < 0) return;
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, 3);
    }

    const int idx = m_editingTextReplaceIdx;
    m_editingTextReplaceIdx = -1;
    m_isSelectingReplaceText = false;
    m_isDraggingReplacePos = false;
    m_hoverReplaceDragHandle = false;
    m_hoveredTrBtnId = -1;
    clearTextReplaceToolbarRects();

    if (!keepChanges) {
        m_markupElements = std::move(m_editingTextReplaceBackup);
        m_editingTextReplaceBackup.clear();
        m_editingTextReplaceWasNew = false;
        recompositeMarkupPreservingOcr();
        return;
    }

    if (idx >= 0 && idx < static_cast<int>(m_markupElements.size())) {
        m_markupElements[static_cast<size_t>(idx)].isEditing = false;
    }
    // 压入无损撤销栈（保存编辑前完整快照，支持 Ctrl+Z 一步还原）
    m_pinUndoStack.push_back(std::move(m_editingTextReplaceBackup));
    if (m_pinUndoStack.size() > MarkupEngine::kMaxUndoSteps) {
        m_pinUndoStack.pop_front();
    }
    m_pinRedoStack.clear();
    m_editingTextReplaceBackup.clear();
    m_editingTextReplaceWasNew = false;
    m_ocrMarkupPreviewClean = true;
    recompositeMarkupPreservingOcr();
}

bool PinWindow::handleTextReplaceChar(wchar_t ch) {
    if (m_editingTextReplaceIdx < 0 ||
        m_editingTextReplaceIdx >= static_cast<int>(m_markupElements.size())) {
        return false;
    }
    if (ch < 0x20 || ch == 0x7F) return true; // 控制字符由 WM_KEYDOWN 统一处理

    auto& el = m_markupElements[static_cast<size_t>(m_editingTextReplaceIdx)];
    std::wstring w = tools3000::core::WinUtils::utf8ToWstring(el.text);
    const size_t s0 = (std::min)(w.size(), (std::min)(el.textCaret, el.textSelAnchor));
    const size_t s1 = (std::min)(w.size(), (std::max)(el.textCaret, el.textSelAnchor));
    w.erase(s0, s1 - s0);
    if (w.size() < 240) {
        w.insert(s0, 1, ch);
        el.textCaret = s0 + 1;
        el.textSelAnchor = el.textCaret;
    }
    el.text = tools3000::core::WinUtils::wstringToUtf8(w);
    el.textLastCaretTick = GetTickCount();
    recompositeMarkupPreservingOcr();
    updateImeCompositionAnchor();
    return true;
}

bool PinWindow::handleTextReplaceKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) {
    if (m_editingTextReplaceIdx < 0 ||
        m_editingTextReplaceIdx >= static_cast<int>(m_markupElements.size())) {
        return false;
    }

    auto& el = m_markupElements[static_cast<size_t>(m_editingTextReplaceIdx)];
    std::wstring w = tools3000::core::WinUtils::utf8ToWstring(el.text);
    el.textCaret = (std::min)(el.textCaret, w.size());
    el.textSelAnchor = (std::min)(el.textSelAnchor, w.size());

    if (vk == VK_RETURN) {
        commitInPlaceTextReplace(true);
        return true;
    }
    if (vk == VK_ESCAPE) {
        commitInPlaceTextReplace(false);
        return true;
    }

    // Alt + 方向键：1px 像素级精准微调新文字偏移位置
    if (alt && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)) {
        const float step = shift ? 3.0f : 1.0f;
        if (vk == VK_LEFT)  el.replaceOffsetX -= step;
        if (vk == VK_RIGHT) el.replaceOffsetX += step;
        if (vk == VK_UP)    el.replaceOffsetY -= step;
        if (vk == VK_DOWN)  el.replaceOffsetY += step;
        el.textLastCaretTick = GetTickCount();
        recompositeMarkupPreservingOcr();
        return true;
    }

    // Ctrl + +/-：0.5px 字号精调
    if (ctrl && (vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS || vk == VK_SUBTRACT)) {
        const float delta = (vk == VK_OEM_PLUS || vk == VK_ADD) ? 0.5f : -0.5f;
        el.fontSize = std::clamp(el.fontSize + delta, 8.0f, 128.0f);
        el.textLastCaretTick = GetTickCount();
        recompositeMarkupPreservingOcr();
        return true;
    }

    if (ctrl && vk == 'A') {
        el.textSelAnchor = 0;
        el.textCaret = w.size();
        el.textLastCaretTick = GetTickCount();
        render();
        return true;
    }
    if (ctrl && vk == 'C') {
        const size_t s0 = (std::min)(el.textCaret, el.textSelAnchor);
        const size_t s1 = (std::max)(el.textCaret, el.textSelAnchor);
        if (s1 > s0) {
            tools3000::core::WinUtils::copyToClipboard(w.substr(s0, s1 - s0));
        }
        return true;
    }
    if (ctrl && vk == 'V') {
        if (OpenClipboard(m_hwnd)) {
            if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* ptr = static_cast<const wchar_t*>(GlobalLock(hData))) {
                    std::wstring clip;
                    for (size_t i = 0; ptr[i] != L'\0' && clip.size() < 160; ++i) {
                        if (ptr[i] >= 0x20) clip.push_back(ptr[i]);
                    }
                    GlobalUnlock(hData);
                    if (!clip.empty()) {
                        const size_t s0 = (std::min)(el.textCaret, el.textSelAnchor);
                        const size_t s1 = (std::max)(el.textCaret, el.textSelAnchor);
                        w.erase(s0, s1 - s0);
                        w.insert(s0, clip);
                        el.textCaret = s0 + clip.size();
                        el.textSelAnchor = el.textCaret;
                        el.text = tools3000::core::WinUtils::wstringToUtf8(w);
                        el.textLastCaretTick = GetTickCount();
                        recompositeMarkupPreservingOcr();
                    }
                }
            }
            CloseClipboard();
        }
        return true;
    }

    if (vk == VK_BACK) {
        const size_t s0 = (std::min)(el.textCaret, el.textSelAnchor);
        const size_t s1 = (std::max)(el.textCaret, el.textSelAnchor);
        if (s1 > s0) {
            w.erase(s0, s1 - s0);
            el.textCaret = s0;
            el.textSelAnchor = s0;
        } else if (el.textCaret > 0) {
            w.erase(el.textCaret - 1, 1);
            --el.textCaret;
            el.textSelAnchor = el.textCaret;
        }
        el.text = tools3000::core::WinUtils::wstringToUtf8(w);
        el.textLastCaretTick = GetTickCount();
        recompositeMarkupPreservingOcr();
        return true;
    }
    if (vk == VK_DELETE) {
        const size_t s0 = (std::min)(el.textCaret, el.textSelAnchor);
        const size_t s1 = (std::max)(el.textCaret, el.textSelAnchor);
        if (s1 > s0) {
            w.erase(s0, s1 - s0);
            el.textCaret = s0;
            el.textSelAnchor = s0;
        } else if (el.textCaret < w.size()) {
            w.erase(el.textCaret, 1);
        }
        el.text = tools3000::core::WinUtils::wstringToUtf8(w);
        el.textLastCaretTick = GetTickCount();
        recompositeMarkupPreservingOcr();
        return true;
    }
    if (vk == VK_LEFT) {
        if (!shift && el.textCaret != el.textSelAnchor) {
            el.textCaret = (std::min)(el.textCaret, el.textSelAnchor);
        } else if (el.textCaret > 0) {
            --el.textCaret;
        }
        if (!shift) el.textSelAnchor = el.textCaret;
        el.textLastCaretTick = GetTickCount();
        updateImeCompositionAnchor();
        render();
        return true;
    }
    if (vk == VK_RIGHT) {
        if (!shift && el.textCaret != el.textSelAnchor) {
            el.textCaret = (std::max)(el.textCaret, el.textSelAnchor);
        } else if (el.textCaret < w.size()) {
            ++el.textCaret;
        }
        if (!shift) el.textSelAnchor = el.textCaret;
        el.textLastCaretTick = GetTickCount();
        updateImeCompositionAnchor();
        render();
        return true;
    }
    if (vk == VK_HOME) {
        el.textCaret = 0;
        if (!shift) el.textSelAnchor = 0;
        el.textLastCaretTick = GetTickCount();
        updateImeCompositionAnchor();
        render();
        return true;
    }
    if (vk == VK_END) {
        el.textCaret = w.size();
        if (!shift) el.textSelAnchor = w.size();
        el.textLastCaretTick = GetTickCount();
        updateImeCompositionAnchor();
        render();
        return true;
    }
    return true;
}
} // namespace tools3000::capture

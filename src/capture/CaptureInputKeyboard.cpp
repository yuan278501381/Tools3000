// ─────────────────────────────────────────────────────────────────────────────
// CaptureInputKeyboard.cpp — 截图键盘快捷键、IME 输入法、滚轮与渲染定时器消息子模块
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include "core/i18n/UiI18n.h"
#include <windowsx.h>
#include "capture/CaptureInputInternal.h"
#include "capture/CaptureOverlay.h"
#include "capture/CaptureHistory.h"
#include "capture/CaptureRegionHistory.h"
#include "capture/ClipboardUtils.h"
#include "capture/PinWindow.h"
#include "capture/ScrollCapture.h"
#include "capture/ScrollCaptureOverlay.h"
#include "capture/ScrollCaptureEditorOverlay.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/CornerRadiusHelper.h"
#include "capture/CaptureToolbarLayout.h"
#include "capture/CaptureToolbarAccessibility.h"
#include "capture/CaptureInteractionPolicy.h"
#include "capture/CaptureShortcutPolicy.h"
#include "capture/CaptureTextEditor.h"
#include "capture/CaptureDropdownMenu.h"
#include "capture/CaptureHudPalette.h"
#include "capture/MarkupBaseHelper.h"
#include "capture/WatermarkRenderer.h"
#include "capture/TextAnnotationHelper.h"
#include "ocr/OcrEngine.h"
#include "core/config/ConfigManager.h"
#include "core/ipc/MessageBridge.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"
#include "core/events/EventBus.h"
#include <windows.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <imm.h>
#include <array>
#include <cwchar>
#include <cwctype>
#include <cmath>
#include <filesystem>

namespace tools3000::capture {

using namespace input_detail;

bool CaptureInput::applyExplicitRegionSelection(
    HWND hwnd,
    const RECT& rawScreenRect,
    const std::string& labelUtf8) {
    if (!m_state || !m_renderer) {
        return false;
    }

    int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int surfaceW = m_state->frozenScreen.empty()
        ? GetSystemMetrics(SM_CXVIRTUALSCREEN)
        : m_state->frozenScreen.cols;
    int surfaceH = m_state->frozenScreen.empty()
        ? GetSystemMetrics(SM_CYVIRTUALSCREEN)
        : m_state->frozenScreen.rows;

    RECT winRc{};
    if (hwnd && IsWindow(hwnd) && GetWindowRect(hwnd, &winRc)) {
        originX = winRc.left;
        originY = winRc.top;
        if (m_state->frozenScreen.empty()) {
            surfaceW = (std::max)(0L, winRc.right - winRc.left);
            surfaceH = (std::max)(0L, winRc.bottom - winRc.top);
        }
    }

    if (surfaceW < CaptureRegionHistory::kMinDimension ||
        surfaceH < CaptureRegionHistory::kMinDimension) {
        return false;
    }

    const RECT virtualBounds{originX, originY, originX + surfaceW, originY + surfaceH};
    const RECT primaryBounds{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    RECT screenRect{};
    if (!CaptureRegionHistory::sanitizeToBounds(rawScreenRect, virtualBounds, screenRect, &primaryBounds)) {
        return false;
    }

    const LONG localLeft = std::clamp<LONG>(screenRect.left - originX, 0, surfaceW);
    const LONG localTop = std::clamp<LONG>(screenRect.top - originY, 0, surfaceH);
    const LONG localRight = std::clamp<LONG>(screenRect.right - originX, 0, surfaceW);
    const LONG localBottom = std::clamp<LONG>(screenRect.bottom - originY, 0, surfaceH);
    if ((localRight - localLeft) < CaptureRegionHistory::kMinDimension ||
        (localBottom - localTop) < CaptureRegionHistory::kMinDimension) {
        return false;
    }

    if (HMONITOR hMon = MonitorFromRect(&screenRect, MONITOR_DEFAULTTONEAREST)) {
        const UINT dpiX = tools3000::core::dpi::effectiveDpiForMonitor(hMon);
        const float newScale = tools3000::core::dpi::scaleForDpi(dpiX);
        if (std::abs(newScale - m_state->dpiScale) >= 0.01f) {
            m_state->dpiScale = newScale;
            if (!m_renderer->updateDpiScale(newScale)) {
                LOG_WARN("Failed to update capture overlay DPI resources for explicit region: dpi={}", dpiX);
            }
        }
    }

    closeAllCapturePopups(*m_state, false);
    if (m_state->activeElement) {
        m_state->activeElement->isActive = false;
        m_state->activeElement->isEditing = false;
        if (m_state->activeElement->tool == MarkupTool::Text &&
            m_state->activeElement->text.empty()) {
            m_state->markup.removeElement(m_state->activeElement->id);
        }
        m_state->activeElement = nullptr;
        resetCoalescedUndo(*m_state);
    }

    m_state->dragging = false;
    m_state->isMarking = false;
    m_state->isManipulating = false;
    m_state->hasManipulateStartElement = false;
    m_state->isAdjustingSelection = false;
    m_state->hasSelAdjustStart = false;
    m_state->isAdjustingCornerRadius = false;
    m_state->isDraggingToolbar = false;
    m_state->isSelectingText = false;
    m_state->isTextMarkupStreaming = false;
    m_state->detectedWindow = {};
    m_state->animDetectedWindowRect = {};
    m_state->targetDetectedWindowRect = {};
    m_state->animVelocity = {};
    m_state->isHierarchyAnimating = false;
    m_state->hierarchyBadgeFadeOutUntil = 0;
    m_state->hierarchyBadgeText.clear();

    m_state->dragStart = {localLeft, localTop};
    m_state->dragEnd = {localRight, localBottom};
    m_state->hasSavedSelectionBeforeInvert = false;
    m_state->hasCustomToolbarPos = false;
    m_state->detectedQrText.clear();
    m_state->qrChipRect = {0, 0, 0, 0};
    m_state->isQrChipHovered = false;
    m_state->state = OverlayState::Selected;
    m_state->isMarkupToolActive = false;
    m_state->toolbarLayoutValid = false;

    prepareMarkupBase();
    rebuildToolbarButtons(currentSelectionRect());

    const bool zh = tools3000::core::isUiLanguageChinese();
    const int selW = static_cast<int>(localRight - localLeft);
    const int selH = static_cast<int>(localBottom - localTop);
    const std::wstring wName = labelUtf8.empty()
        ? std::wstring{}
        : tools3000::core::WinUtils::utf8ToWstring(labelUtf8);
    m_state->loupeToastUntil = GetTickCount() + 1400;
    m_state->loupeToastMessage =
        std::wstring(tools3000::core::trWCstrFor(zh, L"历史选区 ", L"Recent Region ")) +
        (wName.empty() ? L"" : (wName + L" ")) +
        L"(" + std::to_wstring(selW) + L"x" + std::to_wstring(selH) + L")";
    if (hwnd && IsWindow(hwnd)) {
        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
    }

    m_renderer->markMarkupDirty();
    m_renderer->invalidate();
    updateHoverCursor(m_state->currentCursor);
    syncShortcutHintContext();
    return true;
}

bool CaptureInput::applyHistoryRegionSelection(HWND hwnd, bool stepNewer) {
    if (!m_state || !m_renderer) {
        return false;
    }

    int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int surfaceW = m_state->frozenScreen.empty()
        ? GetSystemMetrics(SM_CXVIRTUALSCREEN)
        : m_state->frozenScreen.cols;
    int surfaceH = m_state->frozenScreen.empty()
        ? GetSystemMetrics(SM_CYVIRTUALSCREEN)
        : m_state->frozenScreen.rows;

    RECT winRc{};
    if (hwnd && IsWindow(hwnd) && GetWindowRect(hwnd, &winRc)) {
        originX = winRc.left;
        originY = winRc.top;
        if (m_state->frozenScreen.empty()) {
            surfaceW = (std::max)(0L, winRc.right - winRc.left);
            surfaceH = (std::max)(0L, winRc.bottom - winRc.top);
        }
    }

    if (surfaceW < CaptureRegionHistory::kMinDimension ||
        surfaceH < CaptureRegionHistory::kMinDimension) {
        return false;
    }

    const RECT virtualBounds{
        originX,
        originY,
        originX + surfaceW,
        originY + surfaceH
    };
    const RECT primaryBounds{
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)
    };

    auto& history = CaptureRegionHistory::instance();
    RECT screenRect{};
    std::string regionName;
    const bool stepped = stepNewer
        ? history.stepNewer(virtualBounds, screenRect, &primaryBounds, &regionName)
        : history.stepOlder(virtualBounds, screenRect, &primaryBounds, &regionName);
    if (!stepped) {
        return false;
    }

    if (!applyExplicitRegionSelection(hwnd, screenRect, regionName)) {
        return false;
    }

    const bool zh = tools3000::core::isUiLanguageChinese();
    const int curIdx = history.cursorIndex();
    const std::size_t total = history.size();
    const int selW = static_cast<int>(m_state->dragEnd.x - m_state->dragStart.x);
    const int selH = static_cast<int>(m_state->dragEnd.y - m_state->dragStart.y);
    const std::wstring wName = regionName.empty()
        ? std::wstring{}
        : (L" · " + tools3000::core::WinUtils::utf8ToWstring(regionName));
    m_state->loupeToastUntil = GetTickCount() + 1400;
    m_state->loupeToastMessage =
        std::wstring(tools3000::core::trWCstrFor(zh, L"历史选区 ", L"Recent Region ")) +
        std::to_wstring((curIdx >= 0) ? (curIdx + 1) : 1) + L"/" +
        std::to_wstring((std::max<std::size_t>)(1, total)) +
        wName +
        L" (" + std::to_wstring(selW) + L"x" + std::to_wstring(selH) + L")";
    m_renderer->invalidate();
    return true;
}

void CaptureInput::showNoHistoryRegionToast(HWND hwnd) {
    if (!m_state || !m_renderer) return;
    const bool zh = tools3000::core::isUiLanguageChinese();
    m_state->loupeToastUntil = GetTickCount() + 1200;
    m_state->loupeToastMessage = tools3000::core::trWCstrFor(
        zh, L"暂无历史截图区域", L"No recent capture regions");
    if (hwnd && IsWindow(hwnd)) {
        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
    }
    m_renderer->invalidate();
}

LRESULT CaptureInput::handleKeyAndWheelMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_IME_STARTCOMPOSITION: {
            if (this && m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                syncImeCompositionWindow(hwnd);
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_IME_COMPOSITION: {
            if (this && m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                if (lParam & GCS_RESULTSTR) {
                    HIMC hImc = ImmGetContext(hwnd);
                    if (hImc) {
                        LONG bytes = ImmGetCompositionStringW(hImc, GCS_RESULTSTR, nullptr, 0);
                        if (bytes > 0) {
                            std::wstring resultStr(bytes / sizeof(wchar_t), L'\0');
                            ImmGetCompositionStringW(hImc, GCS_RESULTSTR, &resultStr[0], bytes);

                            pushCoalescedElementUndo(*m_state, 650);
                            std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                            CaptureTextEditor::insertText(
                                wstr,
                                m_state->activeElement->textCaret,
                                m_state->activeElement->textSelAnchor,
                                resultStr);
                            m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                            m_state->activeElement->textLastCaretTick = GetTickCount();
                            m_state->textPreferredCaretX = -1.0f;
                            ensureActiveTextBoundsAfterEdit(*m_state->activeElement);
                            m_renderer->markMarkupDirty();
                            m_renderer->invalidate();
                        }
                        ImmReleaseContext(hwnd, hImc);
                        syncImeCompositionWindow(hwnd);
                        return 0; // 消费输入法上屏文字，防止 DefWindowProcW 再次派发导致重复
                    }
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_IME_CHAR: {
            return 0;
        }

        case WM_CHAR: {
            if (this && m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                wchar_t ch = static_cast<wchar_t>(wParam);
                if (ch == 0x08 || ch == 0x7F) { // Backspace / Ctrl+Backspace
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    if (!wstr.empty() || CaptureTextEditor::hasSelection(m_state->activeElement->textCaret, m_state->activeElement->textSelAnchor)) {
                        pushCoalescedElementUndo(*m_state, 650);
                    }
                    if (CaptureTextEditor::backspace(
                            wstr,
                            m_state->activeElement->textCaret,
                            m_state->activeElement->textSelAnchor,
                            ch == 0x7F)) {
                        m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                        ensureActiveTextBoundsAfterEdit(*m_state->activeElement);
                    }
                    m_state->activeElement->textLastCaretTick = GetTickCount();
                    m_state->textPreferredCaretX = -1.0f;
                    syncImeCompositionWindow(hwnd);
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                } else if (ch == 0x0D) { // Enter: 在当前光标处插入换行符（或替换选区为换行）
                    pushCoalescedElementUndo(*m_state, 650);
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    CaptureTextEditor::insertText(
                        wstr,
                        m_state->activeElement->textCaret,
                        m_state->activeElement->textSelAnchor,
                        L"\n");
                    m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                    m_state->activeElement->textLastCaretTick = GetTickCount();
                    m_state->textPreferredCaretX = -1.0f;
                    ensureActiveTextBoundsAfterEdit(*m_state->activeElement);
                    syncImeCompositionWindow(hwnd);
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                } else if (ch == 0x0A) { // Ctrl+Enter: 结束文本编辑
                    m_state->activeElement->isEditing = false;
                    m_state->isSelectingText = false;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                    syncShortcutHintContext();
                } else if (ch >= 0x20 || ch == 0x09) {
                    pushCoalescedElementUndo(*m_state, 650);
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    CaptureTextEditor::insertText(
                        wstr,
                        m_state->activeElement->textCaret,
                        m_state->activeElement->textSelAnchor,
                        std::wstring(1, ch));
                    m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                    m_state->activeElement->textLastCaretTick = GetTickCount();
                    m_state->textPreferredCaretX = -1.0f;
                    ensureActiveTextBoundsAfterEdit(*m_state->activeElement);
                    syncImeCompositionWindow(hwnd);
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                }
                return 0;
            }
            break;
        }

        case WM_MOUSEWHEEL: {
            if (!this) break;
            CaptureShortcutPolicy::cancelShiftTapCandidate(m_state->shiftTapCandidate);
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);

            // 1. 选区前/未拖拽状态：滚轮切换探测窗口层级 (子控件 <-> 父容器 <-> 顶级窗口 <-> 全屏)
            if (((int)m_state->state.load() == (int)OverlayState::Idle ||
                 (int)m_state->state.load() == (int)OverlayState::Selecting) && !m_state->dragging) {
                if (!m_state->detectedWindowHierarchy.empty()) {
                    if (zDelta != 0) {
                        stepDetectedWindowHierarchy(hwnd, zDelta > 0 ? 1 : -1);
                    }
                    return 0;
                }
            }

            POINT wheelPt = m_state->currentCursor;
            if (lParam != 0) {
                POINT screenWheelPt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (hwnd && IsWindow(hwnd)) {
                    ScreenToClient(hwnd, &screenWheelPt);
                    wheelPt = screenWheelPt;
                } else {
                    wheelPt = screenWheelPt;
                }
            }

            const ToolbarButton* hoveredColorBtn = hitTestToolbar(wheelPt);
            const bool ctrlDown = ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0) ||
                                  ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
            const bool shiftDown = ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0) ||
                                   ((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            const bool ctrlOrShiftWheel = ctrlDown || shiftDown;
            const float wheelDpi = (m_state->dpiScale > 0.0f) ? m_state->dpiScale : 1.0f;

            if (zDelta != 0 && hoveredColorBtn != nullptr &&
                (hoveredColorBtn->command == ToolbarCommand::ChooseCustomColor ||
                 hoveredColorBtn->command == ToolbarCommand::SelectColor)) {
                const int delta = (zDelta > 0) ? 1 : -1;
                pushCoalescedElementUndo(*m_state, 450);
                adjustPaletteColorByWheel(*m_state, hoveredColorBtn->tool, delta, ctrlDown);
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                return 0;
            }

            auto showWheelFeedbackToast = [&](const std::wstring& msg) {
                if (msg.empty()) return;
                m_state->loupeToastMessage = msg;
                m_state->loupeToastUntil = GetTickCount() + 950;
                if (hwnd && IsWindow(hwnd)) {
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                }
            };

            const ToolbarButton* wheelBtn = hoveredColorBtn ? hoveredColorBtn : hitTestToolbar(wheelPt);
            bool runAttributeAdjustment = false;
            bool wheelOnActiveElement = false;

            if (m_state->isLongImageMode()) {
                const auto route = resolveLongImageWheelRoute(
                    *m_state, wheelPt, wheelBtn != nullptr, ctrlDown, shiftDown);
                if (route == LongImageWheelRoute::CanvasZoom ||
                    route == LongImageWheelRoute::CanvasScrollHorizontal ||
                    route == LongImageWheelRoute::CanvasScrollVertical) {
                    const float wx = static_cast<float>(wheelPt.x);
                    const float wy = static_cast<float>(wheelPt.y);
                    if (route == LongImageWheelRoute::CanvasZoom) {
                        const auto anchorDoc = m_state->screenToDoc(wx, wy);
                        const float factor = (zDelta > 0) ? 1.15f : (1.0f / 1.15f);
                        const float newZoom = std::clamp(m_state->longCanvas.zoom * factor, 0.15f, 4.0f);
                        m_state->longCanvas.zoom = newZoom;
                        const auto& stg = m_state->longCanvas.stageRect;
                        const float pad = 20.0f * wheelDpi;
                        m_state->longCanvas.scrollX = stg.left + pad + anchorDoc.x * newZoom - wx;
                        m_state->longCanvas.scrollY = stg.top + pad + anchorDoc.y * newZoom - wy;
                        m_state->clampLongCanvasScroll();
                    } else if (route == LongImageWheelRoute::CanvasScrollHorizontal) {
                        m_state->longCanvas.scrollX -= (static_cast<float>(zDelta) / 120.0f) * 148.0f * wheelDpi;
                        m_state->clampLongCanvasScroll();
                    } else {
                        m_state->longCanvas.scrollY -= (static_cast<float>(zDelta) / 120.0f) * 148.0f * wheelDpi;
                        m_state->clampLongCanvasScroll();
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                if ((route == LongImageWheelRoute::ActiveMagnifier ||
                     route == LongImageWheelRoute::ActiveElementAttribute) &&
                    m_state->activeElement && zDelta != 0) {
                    const int delta = (zDelta > 0) ? 1 : -1;
                    pushCoalescedElementUndo(*m_state, 450);
                    const auto outcome = applyWheelAdjustmentToActiveElement(
                        *m_state, *m_state->activeElement, delta, shiftDown, wheelDpi);
                    if (outcome.kind == WheelAdjustedKind::SpotlightDim) {
                        tools3000::core::ConfigManager::instance().set<int>(
                            "/capture/markup/spotlightDim", outcome.intValue);
                    } else if (outcome.kind == WheelAdjustedKind::TextFontSize) {
                        ensureActiveTextBoundsAfterEdit(*m_state->activeElement);
                        tools3000::core::ConfigManager::instance().set<double>(
                            "/capture/markup/text/fontSize", static_cast<double>(outcome.floatValue));
                    }
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    switch (outcome.kind) {
                        case WheelAdjustedKind::MagnifierRadius:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"放大镜半径: ", L"Magnifier Radius: ")) +
                                std::to_wstring(outcome.intValue) + L" px");
                            break;
                        case WheelAdjustedKind::MagnifierScale: {
                            wchar_t scaleBuf[32]{};
                            swprintf_s(scaleBuf, L"%.2fx", outcome.floatValue);
                            showWheelFeedbackToast((tools3000::core::trWCstrFor(zh, L"放大倍率: ", L"Magnifier: ")) + std::wstring(scaleBuf));
                            break;
                        }
                        case WheelAdjustedKind::CornerRadius:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"圆角半径: ", L"Corner Radius: ")) +
                                std::to_wstring(outcome.intValue) + L" px");
                            break;
                        case WheelAdjustedKind::SpotlightDim:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"聚光灯暗度: ", L"Spotlight Dim: ")) +
                                std::to_wstring(outcome.intValue) + L"%");
                            break;
                        case WheelAdjustedKind::TextFontSize:
                        case WheelAdjustedKind::TextReplaceFontSize:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"文字字号: ", L"Font Size: ")) +
                                std::to_wstring(outcome.intValue) + L" px");
                            break;
                        case WheelAdjustedKind::StepSize:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"序号大小: ", L"Step Size: ")) + std::to_wstring(outcome.intValue));
                            break;
                        case WheelAdjustedKind::MosaicSize:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"马赛克强度: ", L"Mosaic Size: ")) +
                                std::to_wstring(outcome.intValue) + L" px");
                            break;
                        case WheelAdjustedKind::BlurStrength:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"模糊强度: ", L"Blur Strength: ")) +
                                std::to_wstring(outcome.intValue) + L" px");
                            break;
                        case WheelAdjustedKind::InpaintRadius:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"消除笔半径: ", L"Inpaint Radius: ")) +
                                std::to_wstring(outcome.intValue) + L" px");
                            break;
                        case WheelAdjustedKind::WatermarkSize:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"水印大小: ", L"Watermark Size: ")) +
                                std::to_wstring(outcome.intValue) + L" px");
                            break;
                        case WheelAdjustedKind::StrokeWidth:
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"线条粗细: ", L"Stroke Width: ")) +
                                std::to_wstring(outcome.intValue) + L" px");
                            break;
                        case WheelAdjustedKind::None:
                        default:
                            break;
                    }
                    m_renderer->markMarkupDirty();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                if (zDelta != 0) {
                    runAttributeAdjustment = true;
                    wheelOnActiveElement = false;
                }
            } else {
                MarkupElement* hoveredMag = isPointInSelection(wheelPt)
                    ? findHoveredMagnifierElement(*m_state, toMarkupPoint(wheelPt))
                    : nullptr;

                if (hoveredMag != nullptr ||
                    (!m_state->isMarkupToolActive && m_state->activeElement &&
                     m_state->activeElement->tool == MarkupTool::Magnifier)) {
                    MarkupElement* targetMag = hoveredMag ? hoveredMag : m_state->activeElement;
                    if (m_state->activeElement && m_state->activeElement != targetMag) {
                        m_state->activeElement->isActive = false;
                        m_state->activeElement->isEditing = false;
                    }
                    m_state->activeElement = targetMag;
                    targetMag->isActive = true;
                    pushCoalescedElementUndo(*m_state, 450);
                    if (ctrlOrShiftWheel) {
                        const int stepPx = (std::max)(4, static_cast<int>(std::lround(6.0f * wheelDpi)));
                        const int minR = static_cast<int>(std::lround(24.0f * wheelDpi));
                        const int oldRx = targetMag->effectiveMagnifierRx();
                        const int oldRy = targetMag->effectiveMagnifierRy();
                        const int deltaR = (zDelta > 0) ? stepPx : -stepPx;
                        targetMag->magnifierRadius = std::clamp(oldRx + deltaR, minR, 420);
                        targetMag->magnifierRadiusY = std::clamp(oldRy + deltaR, minR, 420);
                    } else {
                        targetMag->magnifierScale = std::clamp(
                            targetMag->magnifierScale + ((zDelta > 0) ? 0.25f : -0.25f), 1.5f, 6.0f);
                        m_state->dynamicMagnifierScale = targetMag->magnifierScale;
                    }
                    m_renderer->markMarkupDirty();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                if (m_state->isMarkupToolActive && m_state->currentTool == MarkupTool::Magnifier &&
                    !m_state->isMarking && !m_state->isManipulating) {
                    if (ctrlOrShiftWheel) {
                        m_state->dynamicMagnifierRadius = std::clamp(
                            m_state->dynamicMagnifierRadius + ((zDelta > 0) ? 6 : -6), 28, 240);
                    } else {
                        m_state->dynamicMagnifierScale = std::clamp(
                            m_state->dynamicMagnifierScale + ((zDelta > 0) ? 0.25f : -0.25f), 1.5f, 6.0f);
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                const bool activeSupportsWheel =
                    (m_state->activeElement != nullptr && m_state->activeElement->isActive &&
                     supportsWheelAttributeAdjustment(m_state->activeElement->tool) &&
                     m_state->activeElement->tool != MarkupTool::Magnifier);
                runAttributeAdjustment =
                    (zDelta != 0) &&
                    (m_state->sliderPopup.type != SliderPopupType::None ||
                     wheelBtn != nullptr ||
                     ((int)m_state->state.load() == (int)OverlayState::Selected &&
                      ((supportsWheelAttributeAdjustment(m_state->currentTool) &&
                        m_state->currentTool != MarkupTool::Magnifier) ||
                       activeSupportsWheel)));
                wheelOnActiveElement =
                    (m_state->sliderPopup.type == SliderPopupType::None && wheelBtn == nullptr && activeSupportsWheel);
            }

            if (runAttributeAdjustment) {
                const int delta = (zDelta > 0) ? 1 : -1;
                const bool showWheelToast = m_state->isLongImageMode() && wheelOnActiveElement;
                const bool zh = showWheelToast ? tools3000::core::isUiLanguageChinese() : false;
                const ToolbarCommand wheelCmd = wheelBtn ? wheelBtn->command : ToolbarCommand::SelectTool;
                const MarkupTool effectiveTool =
                    (wheelOnActiveElement && m_state->activeElement != nullptr)
                        ? m_state->activeElement->tool
                        : m_state->currentTool;

                if (wheelCmd == ToolbarCommand::CycleBeautyRadius) {
                    float newRad = std::clamp(m_state->beautyShell.cornerRadius + static_cast<float>(delta * 2), 0.0f, 32.0f);
                    m_state->beautyShell.cornerRadius = newRad;
                    tools3000::core::ConfigManager::instance().set<int>(
                        "/capture/beautyShellRadius", static_cast<int>(std::round(newRad)));
                    prepareMarkupBase();
                } else if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius ||
                           wheelCmd == ToolbarCommand::SideToggleCornerRadius ||
                           wheelCmd == ToolbarCommand::ToggleCornerRadius) {
                    if (m_state->beautyShell.enabled) {
                        float newRad = std::clamp(m_state->beautyShell.cornerRadius + static_cast<float>(delta * 2), 0.0f, 32.0f);
                        m_state->beautyShell.cornerRadius = newRad;
                        tools3000::core::ConfigManager::instance().set<int>(
                            "/capture/beautyShellRadius", static_cast<int>(std::round(newRad)));
                    } else {
                        float newRad = std::clamp(m_state->cornerRadius + static_cast<float>(delta * 2), 0.0f, 80.0f);
                        m_state->cornerRadius = newRad;
                        tools3000::core::ConfigManager::instance().set<double>(
                            "/screenshot/cornerRadius", static_cast<double>(newRad));
                    }
                    prepareMarkupBase();
                } else if (m_state->sliderPopup.type == SliderPopupType::CornerRadius ||
                           wheelCmd == ToolbarCommand::CycleElementCornerRadius ||
                           (shiftDown && (!wheelOnActiveElement || supportsWheelCornerRadiusAdjustment(effectiveTool)))) {
                    const float baseRad = (m_state->activeElement && supportsWheelCornerRadiusAdjustment(m_state->activeElement->tool))
                        ? m_state->activeElement->cornerRadius
                        : m_state->currentElementCornerRadius;
                    const float newRad = std::clamp(baseRad + static_cast<float>(delta * 2), 0.0f, 40.0f);
                    m_state->currentElementCornerRadius = newRad;
                    if (m_state->activeElement) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->cornerRadius = newRad;
                    }
                    if (showWheelToast) {
                        showWheelFeedbackToast(
                            (tools3000::core::trWCstrFor(zh, L"圆角半径: ", L"Corner Radius: ")) +
                            std::to_wstring(static_cast<int>(std::lround(newRad))) + L" px");
                    }
                } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim ||
                           wheelCmd == ToolbarCommand::CycleSpotlightDim ||
                           (wheelCmd != ToolbarCommand::CycleStrokeWidth && effectiveTool == MarkupTool::Spotlight)) {
                    const float baseAlpha = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Spotlight)
                        ? m_state->activeElement->spotlightDimAlpha
                        : m_state->currentSpotlightDimAlpha;
                    const int curPct = static_cast<int>(std::round(baseAlpha * 100.0f));
                    const int newPct = std::clamp(curPct + delta * 3, 15, 85);
                    m_state->currentSpotlightDimAlpha = static_cast<float>(newPct) / 100.0f;
                    tools3000::core::ConfigManager::instance().set<int>("/capture/markup/spotlightDim", newPct);
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Spotlight) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->spotlightDimAlpha = m_state->currentSpotlightDimAlpha;
                    }
                    if (showWheelToast) {
                        showWheelFeedbackToast(
                            (tools3000::core::trWCstrFor(zh, L"聚光灯暗度: ", L"Spotlight Dim: ")) +
                            std::to_wstring(newPct) + L"%");
                    }
                } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth ||
                           wheelCmd == ToolbarCommand::ToggleTextOutline) {
                    const int curOutW = (m_state->activeElement && m_state->activeElement->hasCustomOutline)
                        ? static_cast<int>(std::round(m_state->activeElement->outlineThickness))
                        : m_state->currentOutlineWidth;
                    const int newOutW = std::clamp(curOutW + delta, 0, 12);
                    m_state->currentOutlineWidth = newOutW;
                    if (effectiveTool == MarkupTool::Text) {
                        m_state->currentTextOutline = (newOutW > 0);
                    }
                    if (m_state->activeElement) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->outlineThickness = static_cast<float>(newOutW);
                        m_state->activeElement->hasCustomOutline = true;
                        m_state->activeElement->textOutline = (newOutW > 0);
                        if (m_state->activeElement->tool == MarkupTool::Text) {
                            m_state->activeElement->textRenderSize = cv::Size(0, 0);
                        }
                    }
                } else if (effectiveTool == MarkupTool::Magnifier &&
                           m_state->sliderPopup.type == SliderPopupType::None &&
                           wheelCmd != ToolbarCommand::CycleStrokeWidth) {
                    if (ctrlOrShiftWheel) {
                        m_state->dynamicMagnifierRadius = std::clamp(
                            m_state->dynamicMagnifierRadius + ((zDelta > 0) ? 6 : -6), 28, 240);
                    } else {
                        m_state->dynamicMagnifierScale = std::clamp(
                            m_state->dynamicMagnifierScale + ((zDelta > 0) ? 0.25f : -0.25f), 1.5f, 6.0f);
                    }
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                        pushCoalescedElementUndo(*m_state, 450);
                        if (ctrlOrShiftWheel) {
                            const int stepPx = (std::max)(4, static_cast<int>(std::lround(6.0f * wheelDpi)));
                            const int minR = static_cast<int>(std::lround(24.0f * wheelDpi));
                            const int oldRx = m_state->activeElement->effectiveMagnifierRx();
                            const int oldRy = m_state->activeElement->effectiveMagnifierRy();
                            const int deltaR = (zDelta > 0) ? stepPx : -stepPx;
                            m_state->activeElement->magnifierRadius = std::clamp(oldRx + deltaR, minR, 420);
                            m_state->activeElement->magnifierRadiusY = std::clamp(oldRy + deltaR, minR, 420);
                        } else {
                            m_state->activeElement->magnifierScale = m_state->dynamicMagnifierScale;
                        }
                    }
                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize ||
                           effectiveTool == MarkupTool::Text ||
                           effectiveTool == MarkupTool::TextReplace) {
                    const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                        pushCoalescedElementUndo(*m_state, 450);
                        const float newPhysSize = std::clamp(
                            m_state->activeElement->fontSize + static_cast<float>(delta * 2),
                            10.0f, 144.0f * curDpi);
                        m_state->activeElement->fontSize = newPhysSize;
                        ensureActiveTextBoundsAfterEdit(*m_state->activeElement);
                        const double logicalSize = std::clamp(static_cast<double>(newPhysSize / curDpi), 10.0, 144.0);
                        tools3000::core::ConfigManager::instance().set<double>(
                            "/capture/markup/text/fontSize", logicalSize);
                        if (showWheelToast) {
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"文字字号: ", L"Font Size: ")) +
                                std::to_wstring(static_cast<int>(std::lround(logicalSize))) + L" px");
                        }
                    } else if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::TextReplace) {
                        pushCoalescedElementUndo(*m_state, 450);
                        const float newPhysSize = std::clamp(
                            m_state->activeElement->fontSize + static_cast<float>(delta),
                            8.0f, 144.0f * curDpi);
                        m_state->activeElement->fontSize = newPhysSize;
                        if (showWheelToast) {
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"文字字号: ", L"Font Size: ")) +
                                std::to_wstring(static_cast<int>(std::lround(newPhysSize))) + L" px");
                        }
                    } else {
                        const float curLogical = static_cast<float>(
                            tools3000::core::ConfigManager::instance().get<double>("/capture/markup/text/fontSize", 18.0));
                        const float newLogical = std::clamp(curLogical + static_cast<float>(delta * 2), 10.0f, 144.0f);
                        tools3000::core::ConfigManager::instance().set<double>(
                            "/capture/markup/text/fontSize", static_cast<double>(newLogical));
                    }
                } else {
                    const int maxW = (m_state->sliderPopup.type == SliderPopupType::MosaicBlockSize ||
                                      effectiveTool == MarkupTool::Mosaic ||
                                      effectiveTool == MarkupTool::Blur) ? 32 : 28;
                    int baseW = m_state->currentStrokeWidth;
                    if (m_state->activeElement &&
                        m_state->activeElement->tool != MarkupTool::Text &&
                        m_state->activeElement->tool != MarkupTool::TextReplace &&
                        m_state->activeElement->tool != MarkupTool::Magnifier &&
                        m_state->activeElement->tool != MarkupTool::Highlight) {
                        baseW = resolveActiveElementStrokeBaseWidth(
                            *m_state->activeElement, m_state->currentStrokeWidth, maxW);
                    }
                    const int newW = std::clamp(baseW + delta, 1, maxW);
                    m_state->currentStrokeWidth = newW;
                    if (m_state->activeElement) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->thickness = static_cast<float>(newW);
                        if (m_state->activeElement->tool == MarkupTool::Mosaic) {
                            m_state->activeElement->mosaicBlockSize = (std::max)(4, newW * 2);
                        } else if (m_state->activeElement->tool == MarkupTool::Blur) {
                            m_state->activeElement->mosaicBlockSize = (std::max)(5, newW * 3);
                        } else if (m_state->activeElement->tool == MarkupTool::Inpaint) {
                            m_state->activeElement->inpaintRadius = std::clamp(newW, 2, 16);
                        } else if (m_state->activeElement->tool == MarkupTool::Watermark) {
                            const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                            m_state->activeElement->fontSize = std::clamp(12.0f + static_cast<float>(newW) * 1.5f, 12.0f, 64.0f) * curDpi;
                        }
                    }
                    if (showWheelToast) {
                        if (effectiveTool == MarkupTool::Number) {
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"序号大小: ", L"Step Size: ")) + std::to_wstring(newW));
                        } else if (effectiveTool == MarkupTool::Mosaic) {
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"马赛克强度: ", L"Mosaic Size: ")) +
                                std::to_wstring((std::max)(4, newW * 2)) + L" px");
                        } else if (effectiveTool == MarkupTool::Blur) {
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"模糊强度: ", L"Blur Strength: ")) +
                                std::to_wstring((std::max)(5, newW * 3)) + L" px");
                        } else if (effectiveTool == MarkupTool::Inpaint) {
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"消除笔半径: ", L"Inpaint Radius: ")) +
                                std::to_wstring(std::clamp(newW, 2, 16)) + L" px");
                        } else {
                            showWheelFeedbackToast(
                                (tools3000::core::trWCstrFor(zh, L"线条粗细: ", L"Stroke Width: ")) +
                                std::to_wstring(newW) + L" px");
                        }
                    }
                }
                m_renderer->markMarkupDirty();
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            }
            return 0;
        }


        case WM_SYSKEYUP:
        case WM_KEYUP: {
            if (!this) break;
            const bool allowTextInput = isTextInputActive();
            m_imeGuard.sync(hwnd, allowTextInput);
            wParam = tools3000::core::WindowImeGuard::resolveShortcutVirtualKey(
                hwnd, wParam, lParam, allowTextInput);
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (msg == WM_SYSKEYUP) || ((GetKeyState(VK_MENU) & 0x8000) != 0);
            const auto keyCtx = CaptureShortcutPolicy::snapshotFromState(*m_state);
            const auto decision = CaptureShortcutPolicy::resolveKeyUp(
                m_state->shiftTapCandidate, keyCtx, wParam, ctrl, alt);
            if (decision.action == CaptureShortcutAction::CycleColorFormat) {
                const int nextIdx = (static_cast<int>(m_state->colorFormat) + 1) %
                                    static_cast<int>(ColorFormatType::COUNT);
                m_state->colorFormat = static_cast<ColorFormatType>(nextIdx);
                m_state->colorFormatHex = (m_state->colorFormat == ColorFormatType::HEX);
                const bool zh = tools3000::core::isUiLanguageChinese();
                const wchar_t* fmtName = CaptureShortcutPolicy::colorFormatShortName(m_state->colorFormat);
                m_state->loupeToastUntil = GetTickCount() + 1200;
                m_state->loupeToastMessage = (tools3000::core::trWCstrFor(zh, L"取色格式: ", L"Color Format: ")) +
                                             std::wstring(fmtName);
                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                m_renderer->invalidate();
            }
            return 0;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            if (!this) break;

            // 非文字编辑态下防御性同步关闭 IME，并兜底将残余 VK_PROCESSKEY (0xE5) 反解为物理按键（如 R / Shift+R）
            const bool allowTextInput = isTextInputActive();
            m_imeGuard.sync(hwnd, allowTextInput);
            wParam = tools3000::core::WindowImeGuard::resolveShortcutVirtualKey(
                hwnd, wParam, lParam, allowTextInput);

            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (msg == WM_SYSKEYDOWN) || ((GetKeyState(VK_MENU) & 0x8000) != 0);
            const bool isAutoRepeat = ((lParam & (static_cast<LPARAM>(1) << 30)) != 0);

            CaptureShortcutPolicy::updateShiftTapOnKeyDown(
                m_state->shiftTapCandidate, wParam, isAutoRepeat, ctrl, alt);

            const auto keyCtx = CaptureShortcutPolicy::snapshotFromState(*m_state);
            if (m_state->isDrawingMultiPointLine && !ctrl && !alt) {
                if (wParam == VK_BACK || wParam == VK_DELETE) {
                    if (MultiPointLinePolicy::popLastVertex(*m_state)) {
                        finalizeMultiPointLine(false);
                    } else {
                        m_renderer->invalidate();
                    }
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
                if (wParam == VK_RETURN || wParam == VK_SPACE) {
                    finalizeMultiPointLine(m_state->multiPointDocVertices.size() >= 2);
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
                if (wParam == VK_ESCAPE) {
                    finalizeMultiPointLine(false);
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
            }
            if (m_state->isLongImageMode() && !keyCtx.editingText && !ctrl && !alt) {
                const auto& stg = m_state->longCanvas.stageRect;
                const float pageStep = (std::max)(120.0f, (stg.bottom - stg.top) * 0.82f);
                if (wParam == VK_HOME) {
                    m_state->scrollLongCanvasToDocRow(static_cast<float>(std::min(m_state->dragStart.y, m_state->dragEnd.y)), 0.12f);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                if (wParam == VK_END) {
                    m_state->scrollLongCanvasToDocRow(static_cast<float>(std::max(m_state->dragStart.y, m_state->dragEnd.y)), 0.82f);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                if (wParam == VK_PRIOR || wParam == VK_NEXT) {
                    m_state->longCanvas.scrollY += (wParam == VK_PRIOR) ? -pageStep : pageStep;
                    m_state->clampLongCanvasScroll();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                if (wParam == VK_SPACE) {
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
            }
            const bool hasSelection = (keyCtx.overlayState == OverlayState::Selected ||
                                       keyCtx.overlayState == OverlayState::Marking);
            const std::string configuredCycleKey =
                tools3000::core::ConfigManager::instance().get<std::string>("/capture/regionHistoryKey", "R");
            const WPARAM regionCycleVk = CaptureRegionHistory::parseCycleKeyVk(configuredCycleKey);
            if (!ctrl && !alt && regionCycleVk != 0 && wParam == regionCycleVk &&
                (CaptureRegionHistory::instance().maxEntries() > 0 || CaptureRegionHistory::instance().hasHistory()) &&
                keyCtx.overlayState != OverlayState::Idle &&
                !keyCtx.editingText &&
                !m_state->historyMode &&
                !m_state->isLongImageMode() &&
                !CaptureOverlay::instance().isEditingPinned() &&
                !m_state->dragging &&
                !m_state->isMarking &&
                !m_state->isDrawingMultiPointLine &&
                !m_state->isManipulating &&
                !m_state->isAdjustingSelection &&
                !m_state->isAdjustingCornerRadius &&
                !m_state->isDraggingToolbar &&
                !m_state->sliderPopup.isDragging &&
                !m_state->isSelectingText &&
                !m_state->isTextMarkupStreaming &&
                !m_state->isMarkupToolActive) {
                if (CaptureRegionHistory::instance().hasHistory()) {
                    applyHistoryRegionSelection(hwnd, shift);
                    return 0;
                }
                if (!hasSelection || shift) {
                    showNoHistoryRegionToast(hwnd);
                    return 0;
                }
            }
            const auto decision = CaptureShortcutPolicy::resolveKeyDown(keyCtx, wParam, ctrl, shift, alt);

            switch (decision.action) {
                case CaptureShortcutAction::None:
                    if (msg == WM_SYSKEYDOWN && (wParam == VK_MENU || wParam == VK_F10)) {
                        return 0; // 抑制 Win32 系统菜单焦点抢占
                    }
                    return (msg == WM_SYSKEYDOWN) ? DefWindowProcW(hwnd, msg, wParam, lParam) : 0;

                case CaptureShortcutAction::ConsumeInHistoryMode:
                case CaptureShortcutAction::CycleColorFormat:
                    return 0;

                case CaptureShortcutAction::ToggleHistoryMode:
                    m_state->historyMode = !m_state->historyMode;
                    if (m_state->historyMode) {
                        m_state->historyIndex = 0;
                        m_renderer->updateHistoryBitmap(*m_state);
                    }
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::StepHistoryPrev:
                case CaptureShortcutAction::StepHistoryNext: {
                    const int total = CaptureHistory::instance().count();
                    if (total > 0) {
                        if (!m_state->historyMode) {
                            m_state->historyMode = true;
                            m_state->historyIndex = 0;
                        } else if (decision.action == CaptureShortcutAction::StepHistoryPrev) {
                            if (m_state->historyIndex < total - 1) m_state->historyIndex++;
                        } else {
                            if (m_state->historyIndex > 0) m_state->historyIndex--;
                        }
                        m_renderer->updateHistoryBitmap(*m_state);
                        m_renderer->invalidate();
                    }
                    return 0;
                }

                case CaptureShortcutAction::ExitHistoryMode:
                    m_state->historyMode = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::MoveCursorPixels: {
                    POINT screenPt{};
                    GetCursorPos(&screenPt);
                    screenPt.x += decision.dx;
                    screenPt.y += decision.dy;
                    SetCursorPos(screenPt.x, screenPt.y);

                    POINT clientPt = screenPt;
                    ScreenToClient(hwnd, &clientPt);
                    const POINT prevCursor = m_state->currentCursor;
                    m_state->currentCursor = clientPt;

                    if (m_state->dragging) {
                        if ((GetKeyState(VK_SPACE) & 0x8000) != 0) {
                            const int pdx = clientPt.x - prevCursor.x;
                            const int pdy = clientPt.y - prevCursor.y;
                            m_state->dragStart.x += pdx;
                            m_state->dragStart.y += pdy;
                            m_state->dragEnd.x += pdx;
                            m_state->dragEnd.y += pdy;
                        } else {
                            m_state->dragEnd = clientPt;
                            const float targetRatio = getTargetAspectRatio(m_state->aspectRatio);
                            if (targetRatio > 0.0f) {
                                const float rdx = static_cast<float>(m_state->dragEnd.x - m_state->dragStart.x);
                                const float rdy = static_cast<float>(m_state->dragEnd.y - m_state->dragStart.y);
                                const float signX = (rdx >= 0.0f) ? 1.0f : -1.0f;
                                const float signY = (rdy >= 0.0f) ? 1.0f : -1.0f;
                                const float curW = std::abs(rdx);
                                const float curH = std::abs(rdy);
                                if (curW / targetRatio >= curH) {
                                    m_state->dragEnd.y = static_cast<LONG>(m_state->dragStart.y + signY * (curW / targetRatio));
                                } else {
                                    m_state->dragEnd.x = static_cast<LONG>(m_state->dragStart.x + signX * (curH * targetRatio));
                                }
                            }
                        }
                        m_state->lastMousePos = clientPt;
                    } else if (m_state->options.autoDetectWindow &&
                               m_state->state.load() == OverlayState::Selecting) {
                        bool preserveHierarchy = false;
                        if (m_state->detectedWindowHierarchyIndex > 0 &&
                            m_state->detectedWindow.right > m_state->detectedWindow.left &&
                            m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                            RECT guardRect = m_state->detectedWindow;
                            guardRect.left -= 6;
                            guardRect.top -= 6;
                            guardRect.right += 6;
                            guardRect.bottom += 6;
                            if (PtInRect(&guardRect, clientPt)) {
                                preserveHierarchy = true;
                            }
                        }
                        if (!preserveHierarchy) {
                            updateWindowDetectionAtCursor(hwnd, screenPt, /*allowAsyncRefine=*/true);
                        }
                        m_state->lastMousePos = clientPt;
                    }
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::NudgeSelectionOrElement:
                    if (m_state->activeElement) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->moveBy(decision.dx, decision.dy);
                        m_renderer->markMarkupDirty();
                    } else {
                        adjustSelection(HitArea::Body, decision.dx, decision.dy);
                        prepareMarkupBase();
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::ResizeSelectionByKey:
                    adjustSelection(decision.resizeHandle, decision.dx, decision.dy);
                    prepareMarkupBase();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::StepWindowHierarchy:
                    stepDetectedWindowHierarchy(hwnd, decision.intDelta);
                    return 0;

                case CaptureShortcutAction::CopyCurrentPixelColor:
                case CaptureShortcutAction::CopyAlternatePixelColor: {
                    int cr = 0, cg = 0, cb = 0;
                    const POINT samplePt = m_state->screenToDocPoint(m_state->currentCursor);
                    if (m_renderer->sampleScreenColor(
                            samplePt.x, samplePt.y, cr, cg, cb, *m_state)) {
                        const ColorFormatType fmt =
                            (decision.action == CaptureShortcutAction::CopyAlternatePixelColor)
                                ? CaptureShortcutPolicy::alternateColorFormat(m_state->colorFormat)
                                : m_state->colorFormat;
                        const std::string colorText = formatColorText(fmt, cr, cg, cb);
                        tools3000::core::WinUtils::copyToClipboard(colorText);
                        m_state->loupeToastUntil = GetTickCount() + 1500;
                        const bool zh = tools3000::core::isUiLanguageChinese();
                        const std::wstring wColorText = tools3000::core::WinUtils::utf8ToWstring(colorText);
                        const wchar_t* fmtName = CaptureShortcutPolicy::colorFormatShortName(fmt);
                        m_state->loupeToastMessage = (tools3000::core::trWCstrFor(zh, L"已复制 ", L"Copied ")) +
                                                     std::wstring(fmtName) + L": " + wColorText;
                        if (hasSelection) {
                            m_state->currentColor = MarkupColor(
                                static_cast<uint8_t>(cr), static_cast<uint8_t>(cg), static_cast<uint8_t>(cb));
                            if (m_state->activeElement) {
                                m_state->markup.pushUndoSnapshot();
                                resetCoalescedUndo(*m_state);
                                m_state->activeElement->color = m_state->currentColor;
                                m_renderer->markMarkupDirty();
                            }
                            m_state->toolbarLayoutValid = false;
                        }
                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                        m_renderer->invalidate();
                    }
                    return 0;
                }

                case CaptureShortcutAction::SelectFullScreen:
                    m_state->dragging = false;
                    m_state->dragStart = {0, 0};
                    m_state->dragEnd = {m_state->frozenScreen.cols, m_state->frozenScreen.rows};
                    m_state->state = OverlayState::Selected;
                    m_state->isMarkupToolActive = false;
                    m_state->toolbarLayoutValid = false;
                    prepareMarkupBase();
                    m_renderer->invalidate();
                    syncShortcutHintContext();
                    return 0;

                case CaptureShortcutAction::LockAspectRatio:
                    m_state->aspectRatio = decision.aspectPreset;
                    enforceAspectRatioOnCurrentSelection();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::AdjustNumberCounter: {
                    const NumberFormat targetFmt = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number)
                        ? m_state->activeElement->numberFormat
                        : m_state->currentNumberFormat;
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->numberValue =
                            (std::max)(1, m_state->activeElement->numberValue + decision.intDelta);
                        m_renderer->markMarkupDirty();
                    } else {
                        const int nextNum = (std::max)(1, m_state->markup.currentNumber(targetFmt) + decision.intDelta);
                        m_state->markup.resetNumber(nextNum, targetFmt);
                    }
                    const int shownNum = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number)
                        ? m_state->activeElement->numberValue
                        : m_state->markup.currentNumber(targetFmt);
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    const std::wstring shownLabel = formatNumberBadgeLabel(shownNum, targetFmt);
                    m_state->loupeToastUntil = GetTickCount() + 1200;
                    m_state->loupeToastMessage = (tools3000::core::trWCstrFor(zh, L"当前序号: ", L"Step ")) + shownLabel;
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::AdjustCornerRadius: {
                    if (decision.intDelta < 0) {
                        if (m_state->beautyShell.enabled) {
                            static const std::array<float, 7> radiuses = {0.0f, 8.0f, 10.0f, 12.0f, 16.0f, 24.0f, 32.0f};
                            for (int i = static_cast<int>(radiuses.size()) - 1; i >= 0; --i) {
                                if (radiuses[i] < m_state->beautyShell.cornerRadius - 0.5f) {
                                    m_state->beautyShell.cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == 0) m_state->beautyShell.cornerRadius = 0.0f;
                            }
                            tools3000::core::ConfigManager::instance().set<int>(
                                "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                        } else {
                            const float cfgMaxRadius = static_cast<float>(
                                tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                            static const std::array<float, 6> radiuses = {0.0f, 8.0f, 14.0f, 24.0f, 40.0f, 60.0f};
                            for (int i = static_cast<int>(radiuses.size()) - 1; i >= 0; --i) {
                                if (radiuses[i] <= cfgMaxRadius && radiuses[i] < m_state->cornerRadius - 0.5f) {
                                    m_state->cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == 0) m_state->cornerRadius = 0.0f;
                            }
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                        }
                    } else {
                        if (m_state->beautyShell.enabled) {
                            static const std::array<float, 7> radiuses = {0.0f, 8.0f, 10.0f, 12.0f, 16.0f, 24.0f, 32.0f};
                            for (size_t i = 0; i < radiuses.size(); ++i) {
                                if (radiuses[i] > m_state->beautyShell.cornerRadius + 0.5f) {
                                    m_state->beautyShell.cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == radiuses.size() - 1) m_state->beautyShell.cornerRadius = 32.0f;
                            }
                            tools3000::core::ConfigManager::instance().set<int>(
                                "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                        } else {
                            const float cfgMaxRadius = static_cast<float>(
                                tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                            static const std::array<float, 6> radiuses = {0.0f, 8.0f, 14.0f, 24.0f, 40.0f, 60.0f};
                            for (size_t i = 0; i < radiuses.size(); ++i) {
                                if (radiuses[i] <= cfgMaxRadius && radiuses[i] > m_state->cornerRadius + 0.5f) {
                                    m_state->cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == radiuses.size() - 1) m_state->cornerRadius = (std::min)(60.0f, cfgMaxRadius);
                            }
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                        }
                    }
                    prepareMarkupBase();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::AdjustStrokeWidth: {
                    if (m_state->currentTool == MarkupTool::Text ||
                        (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text)) {
                        const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                        if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                            pushCoalescedElementUndo(*m_state, 450);
                            const float newPhysSize = std::clamp(
                                m_state->activeElement->fontSize + static_cast<float>(decision.intDelta * 2),
                                10.0f, 144.0f * curDpi);
                            m_state->activeElement->fontSize = newPhysSize;
                            m_state->activeElement->textRenderSize = cv::Size(0, 0);
                            m_renderer->markMarkupDirty();
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/capture/markup/text/fontSize",
                                std::clamp(static_cast<double>(newPhysSize / curDpi), 10.0, 144.0));
                        } else {
                            const float curLogical = static_cast<float>(
                                tools3000::core::ConfigManager::instance().get<double>(
                                    "/capture/markup/text/fontSize", 18.0));
                            const float newLogical = std::clamp(
                                curLogical + static_cast<float>(decision.intDelta * 2), 10.0f, 144.0f);
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/capture/markup/text/fontSize", static_cast<double>(newLogical));
                        }
                    } else {
                        m_state->currentStrokeWidth = std::clamp(m_state->currentStrokeWidth + decision.intDelta, 1, 60);
                        if (m_state->activeElement) {
                            pushCoalescedElementUndo(*m_state, 450);
                            const int val = m_state->currentStrokeWidth;
                            m_state->activeElement->thickness = static_cast<float>(val);
                            if (m_state->activeElement->tool == MarkupTool::Mosaic) {
                                m_state->activeElement->mosaicBlockSize = (std::max)(4, val * 2);
                            } else if (m_state->activeElement->tool == MarkupTool::Blur) {
                                m_state->activeElement->mosaicBlockSize = (std::max)(5, val * 3);
                            } else if (m_state->activeElement->tool == MarkupTool::Inpaint) {
                                m_state->activeElement->inpaintRadius = std::clamp(val, 2, 16);
                            } else if (m_state->activeElement->tool == MarkupTool::Watermark) {
                                const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                                m_state->activeElement->fontSize =
                                    std::clamp(12.0f + static_cast<float>(val) * 1.5f, 12.0f, 64.0f) * curDpi;
                            }
                            m_renderer->markMarkupDirty();
                        }
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::ToggleMarkupTool:
                    toggleOrSetTool(decision.tool);
                    return 0;

                case CaptureShortcutAction::SelectPresetColor:
                    if (m_state->currentTool == MarkupTool::Magnifier ||
                        (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier)) {
                        m_state->currentMagnifierColor = decision.color;
                        if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                            m_state->markup.pushUndoSnapshot();
                            resetCoalescedUndo(*m_state);
                            m_state->activeElement->color = decision.color;
                            m_renderer->markMarkupDirty();
                        }
                    } else {
                        m_state->currentColor = decision.color;
                        if (m_state->activeElement) {
                            m_state->markup.pushUndoSnapshot();
                            resetCoalescedUndo(*m_state);
                            m_state->activeElement->color = decision.color;
                            m_renderer->markMarkupDirty();
                        }
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::ToggleBeautyShell: {
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    m_state->beautyShell.enabled = !m_state->beautyShell.enabled;
                    tools3000::core::ConfigManager::instance().set<bool>(
                        "/capture/beautyShellEnabled", m_state->beautyShell.enabled);
                    if (!m_state->beautyShell.enabled &&
                        (m_state->dropdownMenu.type == DropdownType::BeautyShellBg ||
                         m_state->dropdownMenu.type == DropdownType::BeautyShellPadding ||
                         m_state->dropdownMenu.type == DropdownType::BeautyShellRadius)) {
                        closeDropdownMenu(*m_state, false);
                    }
                    m_state->loupeToastUntil = GetTickCount() + 1500;
                    m_state->loupeToastMessage = m_state->beautyShell.enabled
                        ? (tools3000::core::trWCstrFor(zh, L"已开启美化外壳", L"Beauty Shell enabled"))
                        : (tools3000::core::trWCstrFor(zh, L"已关闭美化外壳", L"Beauty Shell disabled"));
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::CycleBeautyShellTheme: {
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    const int cur = static_cast<int>(m_state->beautyShell.bgType);
                    const int next = (cur + 1) % static_cast<int>(BeautyBackgroundType::COUNT);
                    m_state->beautyShell.bgType = static_cast<BeautyBackgroundType>(next);
                    m_state->beautyShell.enabled = true;
                    static const char* themeKeys[] = {
                        "aurora_purple", "sunrise_coral", "emerald_sea", "obsidian_dark", "minimal_light"
                    };
                    tools3000::core::ConfigManager::instance().set<std::string>(
                        "/capture/beautyShellTheme", (next >= 0 && next < 5) ? themeKeys[next] : "aurora_purple");
                    tools3000::core::ConfigManager::instance().set<bool>("/capture/beautyShellEnabled", true);
                    static const wchar_t* bgNamesZh[] = { L"极光紫", L"日落珊瑚", L"翡翠海", L"曜石黑", L"极简浅灰" };
                    static const wchar_t* bgNamesEn[] = { L"Aurora Purple", L"Sunrise Coral", L"Emerald Sea", L"Obsidian Dark", L"Minimal Light" };
                    const std::wstring name = (next >= 0 && next < 5)
                        ? tools3000::core::trWCstrFor(zh, bgNamesZh[next], bgNamesEn[next])
                        : tools3000::core::trWCstrFor(zh, L"自定义", L"Custom");
                    m_state->loupeToastUntil = GetTickCount() + 1500;
                    m_state->loupeToastMessage = (tools3000::core::trWCstrFor(zh, L"外壳质感: ", L"Shell Theme: ")) + name;
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::CycleBeautyShellPadding: {
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    static constexpr std::array<int, 6> kPaddings = {16, 24, 32, 48, 64, 72};
                    int nextPad = kPaddings[2];
                    for (size_t i = 0; i < kPaddings.size(); ++i) {
                        if (m_state->beautyShell.padding <= kPaddings[i]) {
                            nextPad = kPaddings[(i + 1) % kPaddings.size()];
                            break;
                        }
                        if (i + 1 == kPaddings.size()) {
                            nextPad = kPaddings[0];
                        }
                    }
                    m_state->beautyShell.padding = nextPad;
                    m_state->beautyShell.enabled = true;
                    tools3000::core::ConfigManager::instance().set<int>("/capture/beautyShellPadding", nextPad);
                    tools3000::core::ConfigManager::instance().set<bool>("/capture/beautyShellEnabled", true);
                    m_state->loupeToastUntil = GetTickCount() + 1400;
                    m_state->loupeToastMessage = (tools3000::core::trWCstrFor(zh, L"外壳留白: ", L"Shell Padding: ")) +
                                                 std::to_wstring(nextPad) + L"px";
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::ReEditSelectedText: {
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                        const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                        m_state->activeElement->isEditing = true;
                        m_state->isSelectingText = false;
                        if (decision.selectAllOnTextEdit && !wstr.empty()) {
                            CaptureTextEditor::selectAll(
                                wstr, m_state->activeElement->textCaret, m_state->activeElement->textSelAnchor);
                        } else {
                            m_state->activeElement->textCaret = wstr.size();
                            m_state->activeElement->textSelAnchor = wstr.size();
                        }
                        m_state->activeElement->textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                        syncShortcutHintContext();
                    }
                    return 0;
                }

                case CaptureShortcutAction::DeleteSelectedElement:
                    if (m_state->activeElement) {
                        resetCoalescedUndo(*m_state);
                        m_state->markup.removeElement(m_state->activeElement->id);
                        m_state->activeElement = nullptr;
                        m_renderer->markMarkupDirty();
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    }
                    return 0;

                case CaptureShortcutAction::UndoMarkup:
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement = nullptr;
                    m_state->isManipulating = false;
                    m_state->hasManipulateStartElement = false;
                    m_state->dragHandle = HitArea::None;
                    m_state->markup.undo();
                    for (const auto& el : m_state->markup.elements()) {
                        if (el) {
                            el->isActive = false;
                            el->isEditing = false;
                        }
                    }
                    m_renderer->markMarkupDirty();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::RedoMarkup:
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement = nullptr;
                    m_state->isManipulating = false;
                    m_state->hasManipulateStartElement = false;
                    m_state->dragHandle = HitArea::None;
                    m_state->markup.redo();
                    for (const auto& el : m_state->markup.elements()) {
                        if (el) {
                            el->isActive = false;
                            el->isEditing = false;
                        }
                    }
                    m_renderer->markMarkupDirty();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::ClearAllMarkup:
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement = nullptr;
                    m_state->isManipulating = false;
                    m_state->hasManipulateStartElement = false;
                    m_state->dragHandle = HitArea::None;
                    m_state->markup.clearElementsUndoable();
                    m_renderer->markMarkupDirty();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::ExtractTextFromSelection: {
                    ToolbarButton ocrBtn;
                    ocrBtn.command = ToolbarCommand::ExtractText;
                    executeToolbarCommand(ocrBtn);
                    return 0;
                }

                case CaptureShortcutAction::PinSelection: {
                    ToolbarButton pinBtn;
                    pinBtn.command = ToolbarCommand::PinWindow;
                    executeToolbarCommand(pinBtn);
                    return 0;
                }

                case CaptureShortcutAction::TranslateSelection: {
                    ToolbarButton trBtn;
                    trBtn.command = ToolbarCommand::Translate;
                    executeToolbarCommand(trBtn);
                    return 0;
                }

                case CaptureShortcutAction::SaveSelectionAs: {
                    triggerSaveSelectionAs(hwnd);
                    return 0;
                }

                case CaptureShortcutAction::QuickSaveSelection: {
                    triggerQuickSave();
                    return 0;
                }

                case CaptureShortcutAction::ConfirmOrCommitUi:
                    if (m_state->dropdownMenu.type != DropdownType::None) {
                        if (m_state->dropdownMenu.hoveredIndex >= 0 &&
                            m_state->dropdownMenu.hoveredIndex < static_cast<int>(m_state->dropdownMenu.items.size())) {
                            commitDropdownSelection(
                                m_state->dropdownMenu.type,
                                m_state->dropdownMenu.items[m_state->dropdownMenu.hoveredIndex].id);
                        } else {
                            closeDropdownMenu(*m_state, false);
                        }
                        m_renderer->invalidate();
                        return 0;
                    }
                    if (hasAnyCapturePopupOpen(*m_state)) {
                        closeAllCapturePopups(*m_state, false);
                        m_renderer->invalidate();
                        return 0;
                    }
                    if (m_state->isMarking) {
                        finishMarkup(m_state->currentCursor);
                    }
                    if (hasSelection) {
                        if (m_state->mode == OverlayMode::RecordRegion) {
                            if (m_confirmCb) m_confirmCb({});
                        } else {
                            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Default});
                        }
                        return 0;
                    }
                    if (m_state->detectedWindow.right > m_state->detectedWindow.left &&
                        m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                        m_state->dragStart = {static_cast<LONG>(m_state->detectedWindow.left),
                                             static_cast<LONG>(m_state->detectedWindow.top)};
                        m_state->dragEnd = {static_cast<LONG>(m_state->detectedWindow.right),
                                           static_cast<LONG>(m_state->detectedWindow.bottom)};
                        m_state->cornerRadius = m_state->detectedWindowCornerRadius;
                        m_state->state = OverlayState::Selected;
                        prepareMarkupBase();
                        if (m_state->mode == OverlayMode::RecordRegion) {
                            if (m_confirmCb) m_confirmCb({});
                        } else {
                            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Default});
                        }
                    }
                    return 0;

                case CaptureShortcutAction::CopySelectionAndFinish:
                    if (m_state->isMarking) {
                        finishMarkup(m_state->currentCursor);
                    }
                    if (hasSelection) {
                        if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                    } else if (m_state->dragging) {
                        m_state->dragging = false;
                        m_state->state = OverlayState::Selected;
                        prepareMarkupBase();
                        if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                    } else if (m_state->detectedWindow.right > m_state->detectedWindow.left &&
                               m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                        m_state->dragStart = {static_cast<LONG>(m_state->detectedWindow.left),
                                             static_cast<LONG>(m_state->detectedWindow.top)};
                        m_state->dragEnd = {static_cast<LONG>(m_state->detectedWindow.right),
                                           static_cast<LONG>(m_state->detectedWindow.bottom)};
                        m_state->cornerRadius = m_state->detectedWindowCornerRadius;
                        m_state->state = OverlayState::Selected;
                        prepareMarkupBase();
                        if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                    }
                    return 0;

                case CaptureShortcutAction::HierarchicalEscape:
                    if (hasAnyCapturePopupOpen(*m_state)) {
                        closeAllCapturePopups(*m_state, true);
                        m_renderer->invalidate();
                    } else if (keyCtx.editingText && m_state->activeElement) {
                        m_state->activeElement->isEditing = false;
                        m_state->isSelectingText = false;
                        if (m_state->activeElement->text.empty()) {
                            m_state->markup.removeElement(m_state->activeElement->id);
                            m_state->activeElement = nullptr;
                        }
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    } else if (m_state->activeElement) {
                        m_state->activeElement->isActive = false;
                        m_state->activeElement = nullptr;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    } else if (m_state->isMarkupToolActive) {
                        m_state->isMarkupToolActive = false;
                        m_state->openSubmenu = SubmenuType::None;
                        m_state->sliderPopup.type = SliderPopupType::None;
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                        updateHoverCursor(m_state->currentCursor);
                        syncShortcutHintContext();
                    } else if (m_state->dragging) {
                        m_state->dragging = false;
                        m_state->state = OverlayState::Selecting;
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    } else if (hasSelection && !m_state->isLongImageMode()) {
                        m_state->state = OverlayState::Selecting;
                        m_state->dragStart = {0, 0};
                        m_state->dragEnd = {0, 0};
                        m_state->isHierarchyAnimating = false;
                        m_state->animDetectedWindowRect = {};
                        m_state->targetDetectedWindowRect = {};
                        m_state->primaryToolbarRect = {};
                        m_state->secondaryToolbarRect = {};
                        m_state->selectionSideRect = {};
                        m_state->toolbarLayoutValid = false;
                        m_state->markup.clearAll();
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    } else {
                        ShortcutHintOverlay::instance().hide();
                        if (m_cancelCb) m_cancelCb();
                    }
                    return 0;

                case CaptureShortcutAction::DelegateToTextEditor: {
                    if (!m_state->activeElement) return 0;
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    auto& elem = *m_state->activeElement;
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(elem.text);

                    if (ctrl && (wParam == 'A' || wParam == 'a')) {
                        CaptureTextEditor::selectAll(wstr, elem.textCaret, elem.textSelAnchor);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (ctrl && (wParam == 'C' || wParam == 'c')) {
                        if (CaptureTextEditor::hasSelection(elem.textCaret, elem.textSelAnchor)) {
                            const std::wstring sub = CaptureTextEditor::selectedText(wstr, elem.textCaret, elem.textSelAnchor);
                            if (!sub.empty()) {
                                tools3000::core::WinUtils::copyToClipboard(sub, hwnd);
                                m_state->loupeToastUntil = GetTickCount() + 1400;
                                m_state->loupeToastMessage = tools3000::core::trWCstrFor(zh, L"已复制选中文字", L"Selected text copied");
                                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                                m_renderer->invalidate();
                            }
                        } else if (!wstr.empty()) {
                            tools3000::core::WinUtils::copyToClipboard(wstr, hwnd);
                            m_state->loupeToastUntil = GetTickCount() + 1400;
                            m_state->loupeToastMessage = tools3000::core::trWCstrFor(zh, L"已复制标注文字", L"Annotation text copied");
                            SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                            m_renderer->invalidate();
                        }
                        return 0;
                    }

                    if (ctrl && (wParam == 'X' || wParam == 'x')) {
                        if (CaptureTextEditor::hasSelection(elem.textCaret, elem.textSelAnchor)) {
                            const std::wstring sub = CaptureTextEditor::selectedText(wstr, elem.textCaret, elem.textSelAnchor);
                            if (!sub.empty()) {
                                m_state->markup.pushUndoSnapshot();
                                resetCoalescedUndo(*m_state);
                                tools3000::core::WinUtils::copyToClipboard(sub, hwnd);
                                CaptureTextEditor::insertText(wstr, elem.textCaret, elem.textSelAnchor, L"");
                                elem.text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                                elem.textRenderSize = cv::Size(0, 0);
                                elem.textLastCaretTick = GetTickCount();
                                m_state->textPreferredCaretX = -1.0f;
                                m_state->loupeToastUntil = GetTickCount() + 1400;
                                m_state->loupeToastMessage = tools3000::core::trWCstrFor(zh, L"已剪切选中文字", L"Selected text cut");
                                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                                m_renderer->markMarkupDirty();
                                m_renderer->invalidate();
                            }
                        }
                        return 0;
                    }

                    if (ctrl && (wParam == 'V' || wParam == 'v')) {
                        std::wstring clipWstr;
                        if (OpenClipboard(nullptr)) {
                            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                                if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                                    if (const wchar_t* pText = static_cast<const wchar_t*>(GlobalLock(hData))) {
                                        clipWstr = pText;
                                        GlobalUnlock(hData);
                                    }
                                }
                            }
                            CloseClipboard();
                        }
                        if (!clipWstr.empty()) {
                            m_state->markup.pushUndoSnapshot();
                            resetCoalescedUndo(*m_state);
                            std::wstring normalized;
                            normalized.reserve(clipWstr.size());
                            for (size_t i = 0; i < clipWstr.size(); ++i) {
                                if (clipWstr[i] == L'\r') {
                                    if (i + 1 < clipWstr.size() && clipWstr[i + 1] == L'\n') {
                                        continue;
                                    }
                                    normalized.push_back(L'\n');
                                } else {
                                    normalized.push_back(clipWstr[i]);
                                }
                            }
                            CaptureTextEditor::insertText(wstr, elem.textCaret, elem.textSelAnchor, normalized);
                            elem.text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                            ensureActiveTextBoundsAfterEdit(elem);
                            elem.textLastCaretTick = GetTickCount();
                            m_state->textPreferredCaretX = -1.0f;
                            syncImeCompositionWindow(hwnd);
                            m_renderer->markMarkupDirty();
                            m_renderer->invalidate();
                        }
                        return 0;
                    }

                    if (wParam == VK_LEFT) {
                        CaptureTextEditor::moveLeft(wstr, elem.textCaret, elem.textSelAnchor, shift, ctrl);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        syncImeCompositionWindow(hwnd);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_RIGHT) {
                        CaptureTextEditor::moveRight(wstr, elem.textCaret, elem.textSelAnchor, shift, ctrl);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        syncImeCompositionWindow(hwnd);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_HOME) {
                        const float maxContentW = syncTextElementWrapWidth(elem);
                        const auto layout = CaptureTextEditor::buildLayout(
                            wstr, elem.fontSize, maxContentW);
                        CaptureTextEditor::moveHomeInLayout(layout, wstr, elem.textCaret, elem.textSelAnchor, shift, ctrl);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        syncImeCompositionWindow(hwnd);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_END) {
                        const float maxContentW = syncTextElementWrapWidth(elem);
                        const auto layout = CaptureTextEditor::buildLayout(
                            wstr, elem.fontSize, maxContentW);
                        CaptureTextEditor::moveEndInLayout(layout, wstr, elem.textCaret, elem.textSelAnchor, shift, ctrl);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        syncImeCompositionWindow(hwnd);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_UP || wParam == VK_DOWN) {
                        const float maxContentW = syncTextElementWrapWidth(elem);
                        const auto layout = CaptureTextEditor::buildLayout(
                            wstr, elem.fontSize, maxContentW);
                        CaptureTextEditor::moveVertical(
                            layout, wstr, elem.textCaret, elem.textSelAnchor,
                            (wParam == VK_UP) ? -1 : 1, shift, m_state->textPreferredCaretX);
                        elem.textLastCaretTick = GetTickCount();
                        syncImeCompositionWindow(hwnd);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_DELETE) {
                        if (!wstr.empty() || CaptureTextEditor::hasSelection(elem.textCaret, elem.textSelAnchor)) {
                            pushCoalescedElementUndo(*m_state, 650);
                        }
                        if (CaptureTextEditor::deleteForward(wstr, elem.textCaret, elem.textSelAnchor, ctrl)) {
                            elem.text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                            ensureActiveTextBoundsAfterEdit(elem);
                        }
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        syncImeCompositionWindow(hwnd);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }
                    return 0;
                }
            }
            return 0;
        }

        case WM_TIMER: {
            if (this && wParam == RENDER_TIMER_ID) {
                if (m_state->isFadingOut) {
                    DWORD elapsed = GetTickCount() - m_state->fadeOutStart;
                    if (elapsed >= 150) {
                        if (m_cancelCb) m_cancelCb();
                    } else {
                        float alpha = 1.0f - (elapsed / 150.0f);
                        SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha * 255), LWA_ALPHA);
                    }
                    return 0;
                }

                // 智能选区阻尼弹簧微动效 (高频利落 Spring Damping Lerp，跟手不粘滞)
                if (m_state->isHierarchyAnimating) {
                    constexpr float k = 380.0f;
                    constexpr float d = 36.0f;
                    constexpr float dt = 0.016f;

                    auto stepSpring = [&](float& pos, float target, float& vel) -> bool {
                        float force = -k * (pos - target) - d * vel;
                        vel += force * dt;
                        pos += vel * dt;
                        if (std::abs(pos - target) < 0.5f && std::abs(vel) < 1.0f) {
                            pos = target;
                            vel = 0.0f;
                            return true;
                        }
                        return false;
                    };

                    bool lOk = stepSpring(m_state->animDetectedWindowRect.left, m_state->targetDetectedWindowRect.left, m_state->animVelocity.left);
                    bool tOk = stepSpring(m_state->animDetectedWindowRect.top, m_state->targetDetectedWindowRect.top, m_state->animVelocity.top);
                    bool rOk = stepSpring(m_state->animDetectedWindowRect.right, m_state->targetDetectedWindowRect.right, m_state->animVelocity.right);
                    bool bOk = stepSpring(m_state->animDetectedWindowRect.bottom, m_state->targetDetectedWindowRect.bottom, m_state->animVelocity.bottom);

                    if (lOk && tOk && rOk && bOk) {
                        m_state->isHierarchyAnimating = false;
                        m_state->animDetectedWindowRect = m_state->targetDetectedWindowRect;
                    }
                    m_renderer->invalidate();
                }
                
                // 长图 Smartisan 首尾裁切标尺拖至上下视口边缘时 60fps 平滑自动滚屏与切口同步推进
                bool edgeAutoScrolling = false;
                if (m_state->isLongImageMode() && m_state->isAdjustingSelection &&
                    (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                    const float curScale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                    const float edgeZone = 44.0f * curScale;
                    const auto& stg = m_state->longCanvas.stageRect;
                    const float cy = static_cast<float>(m_state->currentCursor.y);
                    float scrollDelta = 0.0f;
                    if (cy < stg.top + edgeZone) {
                        const float ratio = std::clamp((stg.top + edgeZone - cy) / edgeZone, 0.0f, 1.0f);
                        scrollDelta = -(3.5f + ratio * 24.0f) * curScale;
                    } else if (cy > stg.bottom - edgeZone) {
                        const float ratio = std::clamp((cy - (stg.bottom - edgeZone)) / edgeZone, 0.0f, 1.0f);
                        scrollDelta = (3.5f + ratio * 24.0f) * curScale;
                    }
                    if (std::abs(scrollDelta) > 0.1f) {
                        edgeAutoScrolling = true;
                        const float prevScrollY = m_state->longCanvas.scrollY;
                        m_state->longCanvas.scrollY += scrollDelta;
                        m_state->clampLongCanvasScroll();
                        if (std::abs(m_state->longCanvas.scrollY - prevScrollY) > 0.1f) {
                            const auto curDoc = m_state->screenToDoc(
                                static_cast<float>(m_state->currentCursor.x), cy);
                            if (m_state->hasSelAdjustStart) {
                                const int totalDx = static_cast<int>(std::lround(curDoc.x - m_state->selAdjustStartDocPt.x));
                                const int totalDy = static_cast<int>(std::lround(curDoc.y - m_state->selAdjustStartDocPt.y));
                                adjustSelectionFromSnapshot(m_state->selAdjustStartRect, m_state->selAdjustHandle, totalDx, totalDy);
                                m_state->longCanvas.lastSelAdjustDocPt = curDoc;
                            } else {
                                const int dx = static_cast<int>(std::trunc(curDoc.x - m_state->longCanvas.lastSelAdjustDocPt.x));
                                const int dy = static_cast<int>(std::trunc(curDoc.y - m_state->longCanvas.lastSelAdjustDocPt.y));
                                if (dx != 0 || dy != 0) {
                                    adjustSelection(m_state->selAdjustHandle, dx, dy);
                                    m_state->longCanvas.lastSelAdjustDocPt.x += static_cast<float>(dx);
                                    m_state->longCanvas.lastSelAdjustDocPt.y += static_cast<float>(dy);
                                }
                            }
                            m_renderer->invalidate();
                        }
                    }
                }

                // 取色放大镜：复制成功提示的存续期间持续重绘，到期后再渲染一帧将其清除
                if (m_state->loupeToastUntil != 0) {
                    m_renderer->invalidate();
                    if (GetTickCount() >= m_state->loupeToastUntil) m_state->loupeToastUntil = 0;
                }
                // 文字编辑期间仅当 480ms 光标显隐状态发生翻转时重算标注图层，避免每 16ms 全画布克隆重绘。
                bool editingText = m_state->activeElement &&
                                   m_state->activeElement->tool == MarkupTool::Text &&
                                   m_state->activeElement->isEditing;
                if (editingText) {
                    const auto* elem = m_state->activeElement;
                    const uint32_t nowTick = GetTickCount();
                    const bool recentlyMoved = (elem->textLastCaretTick != 0) &&
                                               ((nowTick - elem->textLastCaretTick) < 520u);
                    const bool showCursor = recentlyMoved || ((nowTick / 480u) % 2u == 0u) || elem->text.empty();
                    if (m_renderer->needsRender()) {
                        m_lastEditedElementId = elem->id;
                        m_lastTextCaretVisible = showCursor;
                    } else if (elem->id != m_lastEditedElementId || showCursor != m_lastTextCaretVisible) {
                        m_lastEditedElementId = elem->id;
                        m_lastTextCaretVisible = showCursor;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                    }
                } else {
                    m_lastEditedElementId = 0;
                }
                if (m_renderer->needsRender()) {
                    m_renderer->clearNeedsRender();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                // 动效已结束且微晶徽章已过淡出时间、无取色提示、无文字编辑，销毁计时器实现零空闲开销
                if (!m_state->isHierarchyAnimating &&
                    !edgeAutoScrolling &&
                    GetTickCount() >= m_state->hierarchyBadgeFadeOutUntil &&
                    m_state->loupeToastUntil == 0 && !editingText) {
                    KillTimer(hwnd, RENDER_TIMER_ID);
                }
            }
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture

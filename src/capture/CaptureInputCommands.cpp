// ─────────────────────────────────────────────────────────────────────────────
// CaptureInputCommands.cpp — 截图工具栏命令执行与下拉菜单提交子模块
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <windowsx.h>
#include "capture/CaptureInputInternal.h"
#include "capture/CaptureOverlay.h"
#include "capture/CaptureHistory.h"
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

void CaptureInput::executeToolbarCommand(const ToolbarButton& button) {
    // CaptureInput only receives window messages on the overlay UI thread. The
    // announcement therefore never calls UIA/Win32 from a hook or worker
    // thread, and remains invisible to sighted users.
    tools3000::core::accessibility::announceOverlay(
        m_hwnd, toolbarButtonAccessibleName(button));

    if (m_state->isDrawingMultiPointLine) {
        const bool keepMultiPointAlive =
            (button.command == ToolbarCommand::SelectColor ||
             button.command == ToolbarCommand::ChooseCustomColor ||
             button.command == ToolbarCommand::CycleStrokeWidth ||
             button.command == ToolbarCommand::ToggleLineStyleDropdown ||
             button.command == ToolbarCommand::SelectLineStyle ||
             button.command == ToolbarCommand::ToggleCurveMode ||
             button.command == ToolbarCommand::ToggleLineArrow ||
             button.command == ToolbarCommand::ToggleStartCapDropdown ||
             button.command == ToolbarCommand::SelectStartCap ||
             button.command == ToolbarCommand::ToggleEndCapDropdown ||
             button.command == ToolbarCommand::SelectEndCap);
        if (!keepMultiPointAlive) {
            const bool shouldCommit =
                (button.command != ToolbarCommand::Cancel &&
                 button.command != ToolbarCommand::Clear);
            finalizeMultiPointLine(shouldCommit);
        }
    }

    // 仅在切换其他工具或执行重置/确认时退出选中态；改颜色/属性或选择相同工具时保留当前激活元素
    if (button.command != ToolbarCommand::SelectColor &&
        button.command != ToolbarCommand::ChooseCustomColor &&
        button.command != ToolbarCommand::ToggleFill &&
        button.command != ToolbarCommand::ToggleTextOutline &&
        button.command != ToolbarCommand::SelectTextOutlineColor &&
        button.command != ToolbarCommand::CycleStrokeWidth &&
        button.command != ToolbarCommand::CycleElementCornerRadius &&
        button.command != ToolbarCommand::ToggleLineStyleDropdown &&
        button.command != ToolbarCommand::SelectLineStyle &&
        button.command != ToolbarCommand::ToggleArrowStyleDropdown &&
        button.command != ToolbarCommand::SelectArrowStyle &&
        button.command != ToolbarCommand::ToggleCurveMode &&
        button.command != ToolbarCommand::ToggleLineArrow &&
        button.command != ToolbarCommand::ToggleStartCapDropdown &&
        button.command != ToolbarCommand::SelectStartCap &&
        button.command != ToolbarCommand::ToggleEndCapDropdown &&
        button.command != ToolbarCommand::SelectEndCap &&
        button.command != ToolbarCommand::SelectMosaicType &&
        button.command != ToolbarCommand::SelectTextMarkupMode &&
        button.command != ToolbarCommand::ToggleNumberShape &&
        button.command != ToolbarCommand::ToggleNumberFormatDropdown &&
        button.command != ToolbarCommand::ToggleSpotlightShape &&
        button.command != ToolbarCommand::CycleSpotlightDim &&
        button.command != ToolbarCommand::CycleMagnifierScale &&
        button.command != ToolbarCommand::ToggleMagnifierCallout &&
        button.command != ToolbarCommand::ToggleMagnifierCalloutStyleDropdown &&
        button.command != ToolbarCommand::ResetNumberCounter &&
        button.command != ToolbarCommand::ToggleBeautyShell &&
        button.command != ToolbarCommand::CycleBeautyBg &&
        button.command != ToolbarCommand::CycleBeautyPadding &&
        button.command != ToolbarCommand::CycleBeautyRadius &&
        button.command != ToolbarCommand::SideCycleAspectRatio &&
        button.command != ToolbarCommand::SideInvertSelection &&
        button.command != ToolbarCommand::RecordToggleKeycast &&
        button.command != ToolbarCommand::RecordToggleSystemAudio &&
        button.command != ToolbarCommand::RecordToggleMicrophone &&
        button.command != ToolbarCommand::RecordSelectMicSource &&
        button.command != ToolbarCommand::RecordToggleFormat &&
        button.command != ToolbarCommand::RecordCycleFps &&
        button.command != ToolbarCommand::RecordCycleQuality &&
        button.command != ToolbarCommand::RecordCycleClickEffect &&
        button.command != ToolbarCommand::RecordToggleSettingsMenu &&
        button.command != ToolbarCommand::SelectTool) {
        if (m_state->activeElement) {
            m_state->activeElement->isActive = false;
            m_state->activeElement->isEditing = false;
            if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                m_state->markup.removeElement(m_state->activeElement->id);
            }
            m_state->activeElement = nullptr;
            resetCoalescedUndo(*m_state);
            m_renderer->markMarkupDirty();
        }
    }

    switch (button.command) {
        case ToolbarCommand::SelectTool:
            if (!button.isSecondary) {
                toggleOrSetTool(button.tool);
            } else {
                setCurrentTool(button.tool);
            }
            return;

        case ToolbarCommand::SelectColor: {
            const bool altHeld = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool editBorder = (m_state->isEditingOutlineColor || altHeld) &&
                                    button.tool != MarkupTool::Magnifier &&
                                    m_state->currentTool != MarkupTool::Magnifier;
            if (editBorder) {
                m_state->currentTextOutlineColor = button.color;
                if (m_state->currentOutlineWidth <= 0) {
                    m_state->currentOutlineWidth = 2;
                }
                if (m_state->activeElement) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->textOutlineColor = button.color;
                    m_state->activeElement->hasCustomOutline = true;
                    m_state->activeElement->textOutline = true;
                    if (m_state->activeElement->outlineThickness <= 0.05f) {
                        m_state->activeElement->outlineThickness = static_cast<float>(m_state->currentOutlineWidth);
                    }
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                }
            } else {
                if (button.tool == MarkupTool::Magnifier || m_state->currentTool == MarkupTool::Magnifier) {
                    m_state->currentMagnifierColor = button.color;
                } else {
                    m_state->currentColor = button.color;
                }
                if (m_state->activeElement && m_state->activeElement->color != button.color) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->color = button.color;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                }
            }
            m_state->toolbarLayoutValid = false;
            break;
        }

        case ToolbarCommand::ChooseCustomColor: {
            static COLORREF customCustColors[16] = {
                RGB(239, 68, 68),   RGB(249, 115, 22),  RGB(234, 179, 8),   RGB(34, 197, 94),
                RGB(59, 130, 246),  RGB(139, 92, 246),  RGB(236, 72, 153),  RGB(20, 184, 166),
                RGB(15, 23, 42),    RGB(51, 65, 85),    RGB(100, 116, 139), RGB(255, 255, 255),
                RGB(124, 58, 237),  RGB(225, 29, 72),   RGB(2, 132, 199),   RGB(16, 185, 129)
            };
            struct ColorDialogAnchor {
                POINT btnCenterScreen{};
                int toolbarTopScreen = 0;
                int toolbarBottomScreen = 0;

                static UINT_PTR CALLBACK hook(HWND hdlg, UINT uiMsg, WPARAM /*wParam*/, LPARAM lParam) {
                    if (uiMsg == WM_INITDIALOG && lParam != 0) {
                        const auto* pccData = reinterpret_cast<const CHOOSECOLORW*>(lParam);
                        const auto* pAnchor = reinterpret_cast<const ColorDialogAnchor*>(pccData->lCustData);
                        if (pAnchor) {
                            RECT dlgRc{};
                            if (GetWindowRect(hdlg, &dlgRc)) {
                                const int dlgW = dlgRc.right - dlgRc.left;
                                const int dlgH = dlgRc.bottom - dlgRc.top;
                                HMONITOR hMon = MonitorFromPoint(pAnchor->btnCenterScreen, MONITOR_DEFAULTTONEAREST);
                                MONITORINFO mi{};
                                mi.cbSize = sizeof(mi);
                                RECT workArea{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
                                if (hMon && GetMonitorInfoW(hMon, &mi)) {
                                    workArea = mi.rcWork;
                                }
                                const int margin = 10;
                                int targetX = pAnchor->btnCenterScreen.x - dlgW / 2;
                                targetX = std::clamp(targetX, static_cast<int>(workArea.left) + margin,
                                                     std::max(static_cast<int>(workArea.left) + margin,
                                                              static_cast<int>(workArea.right) - dlgW - margin));

                                // 优先紧贴工具栏下方弹出；若下方空间不足，则紧贴主工具栏上方就近弹出
                                int targetY = pAnchor->toolbarBottomScreen + margin;
                                if (targetY + dlgH > workArea.bottom - margin) {
                                    targetY = pAnchor->toolbarTopScreen - dlgH - margin;
                                }
                                targetY = std::clamp(targetY, static_cast<int>(workArea.top) + margin,
                                                     std::max(static_cast<int>(workArea.top) + margin,
                                                              static_cast<int>(workArea.bottom) - dlgH - margin));

                                SetWindowPos(hdlg, HWND_TOPMOST, targetX, targetY, 0, 0,
                                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                            }
                        }
                    }
                    return 0;
                }
            };

            int virX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int virY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            RECT winRc{};
            if (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &winRc)) {
                virX = winRc.left;
                virY = winRc.top;
            }
            ColorDialogAnchor anchor{};
            if (button.rect.right > button.rect.left) {
                anchor.btnCenterScreen.x = virX + static_cast<int>((button.rect.left + button.rect.right) * 0.5f);
                anchor.btnCenterScreen.y = virY + static_cast<int>((button.rect.top + button.rect.bottom) * 0.5f);
            } else {
                GetCursorPos(&anchor.btnCenterScreen);
            }
            const float topLocal = (m_state->primaryToolbarRect.bottom > m_state->primaryToolbarRect.top)
                ? std::min(m_state->primaryToolbarRect.top,
                           (m_state->secondaryToolbarRect.bottom > m_state->secondaryToolbarRect.top)
                               ? m_state->secondaryToolbarRect.top
                               : m_state->primaryToolbarRect.top)
                : static_cast<float>(anchor.btnCenterScreen.y - virY - 24);
            const float bottomLocal = (m_state->secondaryToolbarRect.bottom > m_state->secondaryToolbarRect.top)
                ? std::max(m_state->primaryToolbarRect.bottom, m_state->secondaryToolbarRect.bottom)
                : ((m_state->primaryToolbarRect.bottom > m_state->primaryToolbarRect.top)
                       ? m_state->primaryToolbarRect.bottom
                       : static_cast<float>(anchor.btnCenterScreen.y - virY + 24));
            anchor.toolbarTopScreen = virY + static_cast<int>(topLocal);
            anchor.toolbarBottomScreen = virY + static_cast<int>(bottomLocal);

            const bool forMagnifier = (button.tool == MarkupTool::Magnifier || m_state->currentTool == MarkupTool::Magnifier);
            const bool altHeld = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool editBorder = (m_state->isEditingOutlineColor || altHeld) && !forMagnifier;
            MarkupColor initCol = forMagnifier ? m_state->currentMagnifierColor : m_state->currentColor;
            if (editBorder) {
                if (m_state->activeElement && !m_state->activeElement->textOutlineColor.isAuto()) {
                    initCol = m_state->activeElement->textOutlineColor;
                } else if (!m_state->currentTextOutlineColor.isAuto()) {
                    initCol = m_state->currentTextOutlineColor;
                } else {
                    initCol = MarkupColor::White();
                }
            } else if (m_state->activeElement != nullptr) {
                initCol = m_state->activeElement->color;
            }
            CHOOSECOLORW cc{};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = m_hwnd;
            cc.lpCustColors = customCustColors;
            cc.rgbResult = RGB(initCol.r, initCol.g, initCol.b);
            cc.lCustData = reinterpret_cast<LPARAM>(&anchor);
            cc.lpfnHook = &ColorDialogAnchor::hook;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR | CC_ENABLEHOOK;

            ReleaseCapture();
            const BOOL chosen = ChooseColorW(&cc);
            if (m_hwnd && IsWindow(m_hwnd)) {
                SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetForegroundWindow(m_hwnd);
                SetFocus(m_hwnd);
            }
            if (chosen) {
                const uint8_t r = GetRValue(cc.rgbResult);
                const uint8_t g = GetGValue(cc.rgbResult);
                const uint8_t b = GetBValue(cc.rgbResult);
                const uint8_t keepAlpha = (initCol.a > 0) ? initCol.a : 255;
                m_state->customColor = MarkupColor(r, g, b, keepAlpha);
                m_state->hasCustomColor = true;
                if (editBorder) {
                    m_state->currentTextOutlineColor = m_state->customColor;
                    if (m_state->currentOutlineWidth <= 0) {
                        m_state->currentOutlineWidth = 2;
                    }
                    if (m_state->activeElement) {
                        m_state->markup.pushUndoSnapshot();
                        resetCoalescedUndo(*m_state);
                        m_state->activeElement->textOutlineColor = m_state->customColor;
                        m_state->activeElement->hasCustomOutline = true;
                        m_state->activeElement->textOutline = true;
                        if (m_state->activeElement->outlineThickness <= 0.05f) {
                            m_state->activeElement->outlineThickness = static_cast<float>(m_state->currentOutlineWidth);
                        }
                        m_renderer->markMarkupDirty();
                    }
                } else {
                    if (forMagnifier) {
                        m_state->currentMagnifierColor = m_state->customColor;
                    } else {
                        m_state->currentColor = m_state->customColor;
                    }
                    if (m_state->activeElement && m_state->activeElement->color != m_state->customColor) {
                        m_state->markup.pushUndoSnapshot();
                        resetCoalescedUndo(*m_state);
                        m_state->activeElement->color = m_state->customColor;
                        m_renderer->markMarkupDirty();
                    }
                }
                char hexBuf[16]{};
                snprintf(hexBuf, sizeof(hexBuf), "#%02x%02x%02x", r, g, b);
                tools3000::core::ConfigManager::instance().set<std::string>(
                    "/capture/customColor", hexBuf);
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleFill:
            if (button.tool == MarkupTool::Magnifier || m_state->currentTool == MarkupTool::Magnifier) {
                m_state->magnifierShowBadge = !m_state->magnifierShowBadge;
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->fill = m_state->magnifierShowBadge;
                    m_renderer->markMarkupDirty();
                }
            } else if (button.tool == MarkupTool::Spotlight || m_state->currentTool == MarkupTool::Spotlight) {
                m_state->currentSpotlightBorder = !m_state->currentSpotlightBorder;
                tools3000::core::ConfigManager::instance().set<bool>(
                    "/capture/markup/spotlightBorder", m_state->currentSpotlightBorder);
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Spotlight) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->spotlightShowBorder = m_state->currentSpotlightBorder;
                    m_renderer->markMarkupDirty();
                }
            } else {
                m_state->currentFillMode = !m_state->currentFillMode;
                if (!m_state->currentFillMode) {
                    m_state->isEditingOutlineColor = false;
                    if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                        m_state->sliderPopup.type = SliderPopupType::None;
                    }
                }
                if (m_state->activeElement) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->fill = m_state->currentFillMode;
                    m_renderer->markMarkupDirty();
                }
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleTextOutline: {
            const bool isShapeOrArrowBorder = (button.tool == MarkupTool::Arrow) ||
                ((button.tool == MarkupTool::Rectangle || button.tool == MarkupTool::Ellipse) && m_state->currentFillMode);
            const bool isTextTool = !isShapeOrArrowBorder;
            const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool zh = tools3000::core::isUiLanguageChinese();
            if (isTextTool && !shiftHeld) {
                m_state->currentTextOutline = !m_state->currentTextOutline;
                if (!m_state->currentTextOutline) {
                    m_state->isEditingOutlineColor = false;
                } else {
                    m_state->currentTextOutlineColor = MarkupColor::Auto();
                }
                tools3000::core::ConfigManager::instance().set<bool>(
                    "/capture/markup/text/outline", m_state->currentTextOutline);
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->textOutline = m_state->currentTextOutline;
                    if (m_state->currentTextOutline) {
                        m_state->activeElement->textOutlineColor = MarkupColor::Auto();
                    }
                    m_renderer->markMarkupDirty();
                }
                m_state->loupeToastUntil = GetTickCount() + 1500;
                m_state->loupeToastMessage = m_state->currentTextOutline
                    ? (zh ? L"文字描边: 已开启" : L"Text Outline: ON")
                    : (zh ? L"文字描边: 已关闭" : L"Text Outline: OFF");
            } else {
                m_state->isEditingOutlineColor = !m_state->isEditingOutlineColor;
                if (m_state->isEditingOutlineColor) {
                    if (isTextTool) {
                        m_state->currentTextOutline = true;
                        if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text &&
                            !m_state->activeElement->textOutline) {
                            m_state->markup.pushUndoSnapshot();
                            resetCoalescedUndo(*m_state);
                            m_state->activeElement->textOutline = true;
                            m_renderer->markMarkupDirty();
                        }
                    }
                    closeAllCapturePopups(*m_state, false);
                    m_state->sliderPopup.type = SliderPopupType::OutlineWidth;
                } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                    m_state->sliderPopup.type = SliderPopupType::None;
                }
                m_state->loupeToastUntil = GetTickCount() + 1800;
                m_state->loupeToastMessage = m_state->isEditingOutlineColor
                    ? (zh ? L"边框模式已开启：右侧色块改边框色，拖动滑块或滚轮调边框粗细"
                          : L"Border Mode ON: Pick border color on right, scroll/drag for border width")
                    : (zh ? L"已切回主填充色模式" : L"Switched back to Fill Color mode");
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SelectTextOutlineColor: {
            m_state->currentTextOutlineColor = button.color;
            if (button.color.isAuto()) {
                tools3000::core::ConfigManager::instance().set<std::string>(
                    "/capture/markup/text/outlineColor", "auto");
            } else if (button.color == MarkupColor::Black()) {
                tools3000::core::ConfigManager::instance().set<std::string>(
                    "/capture/markup/text/outlineColor", "black");
            } else if (button.color == MarkupColor::White()) {
                tools3000::core::ConfigManager::instance().set<std::string>(
                    "/capture/markup/text/outlineColor", "white");
            }
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text &&
                m_state->activeElement->textOutlineColor != button.color) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->textOutlineColor = button.color;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleStrokeWidth: {
            if (m_state->sliderPopup.type == SliderPopupType::StrokeWidth ||
                m_state->sliderPopup.type == SliderPopupType::MosaicBlockSize ||
                m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                m_state->sliderPopup.type = SliderPopupType::None;
            } else {
                closeAllCapturePopups(*m_state, false);
                m_state->sliderPopup.type = (m_state->currentTool == MarkupTool::Mosaic || m_state->currentTool == MarkupTool::Blur)
                    ? SliderPopupType::MosaicBlockSize
                    : (m_state->currentTool == MarkupTool::Text)
                    ? SliderPopupType::TextFontSize
                    : SliderPopupType::StrokeWidth;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SideToggleCornerRadius: {
            if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
                m_state->sliderPopup.type = SliderPopupType::None;
            } else {
                closeAllCapturePopups(*m_state, false);
                m_state->sliderPopup.type = SliderPopupType::SelectionCornerRadius;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SideResetSelection: {
            m_state->cornerRadius = 0.0f;
            if (m_state->beautyShell.enabled) {
                m_state->beautyShell.cornerRadius = 0.0f;
            }
            m_state->aspectRatio = AspectRatioPreset::Free;
            if (m_state->isLongImageMode()) {
                m_state->dragStart = {0, 0};
                m_state->dragEnd = {m_state->longCanvas.docWidth, m_state->longCanvas.docHeight};
                const bool zh = tools3000::core::isUiLanguageChinese();
                m_state->loupeToastMessage = zh ? L"已恢复完整长图与直角输出" : L"Restored full long image";
                m_state->loupeToastUntil = GetTickCount() + 1400;
            }
            closeAllCapturePopups(*m_state, false);
            m_state->toolbarLayoutValid = false;
            prepareMarkupBase();
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SideInvertSelection: {
            // 全屏/全幅长图扩展与原选区双向切换
            const int virX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int virY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int curLeft = std::min(m_state->dragStart.x, m_state->dragEnd.x);
            const int curTop = std::min(m_state->dragStart.y, m_state->dragEnd.y);
            const int curRight = std::max(m_state->dragStart.x, m_state->dragEnd.x);
            const int curBottom = std::max(m_state->dragStart.y, m_state->dragEnd.y);

            RECT fullLocal{0, 0,
                           m_state->frozenScreen.empty() ? GetSystemMetrics(SM_CXVIRTUALSCREEN) : m_state->frozenScreen.cols,
                           m_state->frozenScreen.empty() ? GetSystemMetrics(SM_CYVIRTUALSCREEN) : m_state->frozenScreen.rows};
            if (m_state->isLongImageMode()) {
                fullLocal = {0, 0, m_state->longCanvas.docWidth, m_state->longCanvas.docHeight};
            } else {
                POINT centerScreen{virX + (curLeft + curRight) / 2, virY + (curTop + curBottom) / 2};
                HMONITOR hMon = MonitorFromPoint(centerScreen, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (hMon && GetMonitorInfoW(hMon, &mi)) {
                    fullLocal.left = mi.rcMonitor.left - virX;
                    fullLocal.top = mi.rcMonitor.top - virY;
                    fullLocal.right = mi.rcMonitor.right - virX;
                    fullLocal.bottom = mi.rcMonitor.bottom - virY;
                }
            }

            const bool isAlreadyFullMonitor =
                std::abs(curLeft - fullLocal.left) <= 2 &&
                std::abs(curTop - fullLocal.top) <= 2 &&
                std::abs(curRight - fullLocal.right) <= 2 &&
                std::abs(curBottom - fullLocal.bottom) <= 2;

            const bool zh = tools3000::core::isUiLanguageChinese();
            if (isAlreadyFullMonitor && m_state->hasSavedSelectionBeforeInvert &&
                m_state->savedSelectionBeforeInvert.right > m_state->savedSelectionBeforeInvert.left &&
                m_state->savedSelectionBeforeInvert.bottom > m_state->savedSelectionBeforeInvert.top) {
                m_state->dragStart = {m_state->savedSelectionBeforeInvert.left, m_state->savedSelectionBeforeInvert.top};
                m_state->dragEnd = {m_state->savedSelectionBeforeInvert.right, m_state->savedSelectionBeforeInvert.bottom};
                m_state->hasSavedSelectionBeforeInvert = false;
                m_state->loupeToastMessage = zh ? L"已还原上次选区" : L"Restored previous selection";
            } else {
                m_state->savedSelectionBeforeInvert = {curLeft, curTop, curRight, curBottom};
                m_state->hasSavedSelectionBeforeInvert = true;
                m_state->dragStart = {fullLocal.left, fullLocal.top};
                m_state->dragEnd = {fullLocal.right, fullLocal.bottom};
                m_state->loupeToastMessage = zh ? L"已选中当前屏幕" : L"Selected current screen";
            }
            m_state->loupeToastUntil = GetTickCount() + 1400;
            m_state->toolbarLayoutValid = false;
            prepareMarkupBase();
            m_renderer->markMarkupDirty();
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleElementCornerRadius: {
            if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                m_state->sliderPopup.type = SliderPopupType::None;
            } else {
                closeAllCapturePopups(*m_state, false);
                m_state->sliderPopup.type = SliderPopupType::CornerRadius;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleLineStyleDropdown: {
            const bool reopeningSame = (m_state->openSubmenu == SubmenuType::LineStyle);
            closeAllCapturePopups(*m_state, false);
            m_state->openSubmenu = reopeningSame ? SubmenuType::None : SubmenuType::LineStyle;
            break;
        }

        case ToolbarCommand::SelectLineStyle:
            m_state->currentLineStyle = button.lineStyleParam;
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->activeElement && m_state->activeElement->lineStyle != m_state->currentLineStyle) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->lineStyle = m_state->currentLineStyle;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            break;

        case ToolbarCommand::ToggleArrowStyleDropdown: {
            const bool reopeningSame = (m_state->openSubmenu == SubmenuType::ArrowStyle);
            closeAllCapturePopups(*m_state, false);
            m_state->openSubmenu = reopeningSame ? SubmenuType::None : SubmenuType::ArrowStyle;
            break;
        }

        case ToolbarCommand::SelectArrowStyle:
            m_state->currentArrowStyle = button.arrowStyleParam;
            switch (m_state->currentArrowStyle) {
                case ArrowStyle::Thin:
                    m_state->currentStartCap = LineCapStyle::None;
                    m_state->currentEndCap = LineCapStyle::OpenArrow;
                    break;
                case ArrowStyle::Tech:
                case ArrowStyle::CampaignMarch:
                case ArrowStyle::MarchDoubleTrack:
                    m_state->currentStartCap = LineCapStyle::None;
                    m_state->currentEndCap = LineCapStyle::StealthArrow;
                    break;
                case ArrowStyle::DoubleEnded:
                    m_state->currentStartCap = LineCapStyle::SolidTriangle;
                    m_state->currentEndCap = LineCapStyle::SolidTriangle;
                    break;
                case ArrowStyle::Standard:
                default:
                    m_state->currentStartCap = LineCapStyle::None;
                    m_state->currentEndCap = LineCapStyle::SolidTriangle;
                    break;
            }
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->activeElement) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->arrowStyle = m_state->currentArrowStyle;
                if (m_state->activeElement->hasCustomCaps) {
                    m_state->activeElement->startCap = m_state->currentStartCap;
                    m_state->activeElement->endCap = m_state->currentEndCap;
                }
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleCurveMode:
            MultiPointLinePolicy::applyCurveModeButton(*m_state, button.intParam);
            if (m_state->activeElement &&
                (m_state->activeElement->tool == MarkupTool::Line ||
                 m_state->activeElement->tool == MarkupTool::Arrow ||
                 m_state->activeElement->tool == MarkupTool::Pen)) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->smoothCurve = m_state->currentSmoothCurve;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleLineArrow:
            m_state->currentLineArrowEnabled = !m_state->currentLineArrowEnabled;
            if (m_state->currentLineArrowEnabled &&
                m_state->currentStartCap == LineCapStyle::None &&
                m_state->currentEndCap == LineCapStyle::None) {
                m_state->currentEndCap = LineCapStyle::StealthArrow;
            }
            if (!m_state->currentLineArrowEnabled &&
                (m_state->openSubmenu == SubmenuType::StartCapStyle ||
                 m_state->openSubmenu == SubmenuType::EndCapStyle)) {
                m_state->openSubmenu = SubmenuType::None;
            }
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Line) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->hasCustomCaps = m_state->currentLineArrowEnabled;
                m_state->activeElement->startCap = m_state->currentStartCap;
                m_state->activeElement->endCap = m_state->currentEndCap;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleStartCapDropdown: {
            const bool reopeningSame = (m_state->openSubmenu == SubmenuType::StartCapStyle);
            closeAllCapturePopups(*m_state, false);
            m_state->openSubmenu = reopeningSame ? SubmenuType::None : SubmenuType::StartCapStyle;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SelectStartCap:
        case ToolbarCommand::SelectEndCap: {
            const LineCapStyle chosenCap = static_cast<LineCapStyle>(std::clamp(button.intParam, 0, 9));
            if (button.command == ToolbarCommand::SelectStartCap) {
                m_state->currentStartCap = chosenCap;
            } else {
                m_state->currentEndCap = chosenCap;
            }
            m_state->currentLineArrowEnabled = true;
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->currentStartCap == LineCapStyle::None && m_state->currentEndCap == LineCapStyle::OpenArrow) {
                m_state->currentArrowStyle = ArrowStyle::Thin;
            } else if (m_state->currentStartCap == LineCapStyle::None && m_state->currentEndCap == LineCapStyle::StealthArrow) {
                m_state->currentArrowStyle = ArrowStyle::Tech;
            } else if (m_state->currentStartCap != LineCapStyle::None && m_state->currentEndCap != LineCapStyle::None) {
                m_state->currentArrowStyle = ArrowStyle::DoubleEnded;
            } else {
                m_state->currentArrowStyle = ArrowStyle::Standard;
            }
            if (m_state->activeElement &&
                (m_state->activeElement->tool == MarkupTool::Line ||
                 m_state->activeElement->tool == MarkupTool::Arrow)) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->hasCustomCaps = true;
                m_state->activeElement->startCap = m_state->currentStartCap;
                m_state->activeElement->endCap = m_state->currentEndCap;
                m_state->activeElement->arrowStyle = m_state->currentArrowStyle;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleEndCapDropdown: {
            const bool reopeningSame = (m_state->openSubmenu == SubmenuType::EndCapStyle);
            closeAllCapturePopups(*m_state, false);
            m_state->openSubmenu = reopeningSame ? SubmenuType::None : SubmenuType::EndCapStyle;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SelectMosaicType:
            m_state->currentMosaicType = button.intParam;
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->currentTool == MarkupTool::Mosaic || m_state->currentTool == MarkupTool::Blur) {
                m_state->currentTool = (button.intParam == 1) ? MarkupTool::Blur : MarkupTool::Mosaic;
            }
            if (m_state->activeElement &&
                (m_state->activeElement->tool == MarkupTool::Mosaic || m_state->activeElement->tool == MarkupTool::Blur)) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->tool = (button.intParam == 1) ? MarkupTool::Blur : MarkupTool::Mosaic;
                m_state->activeElement->mosaicBlockSize = (button.intParam == 1)
                    ? (std::max)(5, m_state->currentStrokeWidth * 3)
                    : (std::max)(4, m_state->currentStrokeWidth * 2);
                m_state->currentTool = m_state->activeElement->tool;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::SelectTextMarkupMode: {
            m_state->currentTextMarkupMode = static_cast<TextMarkupMode>(
                std::clamp(button.intParam, 0, static_cast<int>(TextMarkupMode::RectRegion)));
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->currentTextMarkupMode != TextMarkupMode::RectRegion) {
                ensureSelectionOcrForTextMarkup();
            }
            if (m_state->activeElement && !m_state->activeElement->textSpans.empty() &&
                m_state->currentTextMarkupMode != TextMarkupMode::RectRegion) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                if (m_state->currentTextMarkupMode == TextMarkupMode::Highlight) {
                    m_state->activeElement->tool = MarkupTool::Highlight;
                    m_state->activeElement->color.a = 220;
                } else {
                    m_state->activeElement->tool = MarkupTool::Line;
                    m_state->activeElement->color.a = 255;
                    m_state->activeElement->lineStyle =
                        (m_state->currentTextMarkupMode == TextMarkupMode::WavyUnderline) ? LineStyle::Wavy : LineStyle::Solid;
                }
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            updateHoverCursor(m_state->currentCursor);
            break;
        }

        case ToolbarCommand::ToggleBeautyShell: {
            m_state->beautyShell.enabled = !m_state->beautyShell.enabled;
            tools3000::core::ConfigManager::instance().set<bool>(
                "/capture/beautyShellEnabled", m_state->beautyShell.enabled);
            if (!m_state->beautyShell.enabled &&
                (m_state->dropdownMenu.type == DropdownType::BeautyShellBg ||
                 m_state->dropdownMenu.type == DropdownType::BeautyShellPadding ||
                 m_state->dropdownMenu.type == DropdownType::BeautyShellRadius)) {
                closeDropdownMenu(*m_state, false);
            }
            const bool zh = tools3000::core::isUiLanguageChinese();
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = m_state->beautyShell.enabled
                ? (zh ? L"已开启美化外壳" : L"Beauty Shell enabled")
                : (zh ? L"已关闭美化外壳" : L"Beauty Shell disabled");
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleBeautyBg: {
            openDropdownMenu(DropdownType::BeautyShellBg, button.rect);
            break;
        }

        case ToolbarCommand::CycleBeautyPadding: {
            openDropdownMenu(DropdownType::BeautyShellPadding, button.rect);
            break;
        }

        case ToolbarCommand::CycleBeautyRadius: {
            openDropdownMenu(DropdownType::BeautyShellRadius, button.rect);
            break;
        }

        case ToolbarCommand::RecordToggleFormat: {
            openDropdownMenu(DropdownType::RecordFormat, button.rect);
            break;
        }

        case ToolbarCommand::RecordCycleFps: {
            openDropdownMenu(DropdownType::RecordFps, button.rect);
            break;
        }

        case ToolbarCommand::RecordCycleQuality: {
            openDropdownMenu(DropdownType::RecordQuality, button.rect);
            break;
        }

        case ToolbarCommand::RecordCycleClickEffect: {
            openDropdownMenu(DropdownType::RecordClickEffect, button.rect);
            break;
        }

        case ToolbarCommand::RecordToggleKeycast: {
            m_state->recordSetup.includeKeycast = !m_state->recordSetup.includeKeycast;
            tools3000::core::ConfigManager::instance().set<bool>(
                "/recording/includeKeycast", m_state->recordSetup.includeKeycast);
            const bool zh = tools3000::core::isUiLanguageChinese();
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = m_state->recordSetup.includeKeycast
                ? (zh ? L"按键回显: 已开启" : L"Keycast: ON")
                : (zh ? L"按键回显: 已关闭" : L"Keycast: OFF");
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::RecordToggleSystemAudio: {
            if (m_state->recordSetup.format != RecordFormat::GIF) {
                m_state->recordSetup.captureSystemAudio = !m_state->recordSetup.captureSystemAudio;
                tools3000::core::ConfigManager::instance().set<bool>(
                    "/recording/captureSystemAudio", m_state->recordSetup.captureSystemAudio);
                const bool zh = tools3000::core::isUiLanguageChinese();
                m_state->loupeToastUntil = GetTickCount() + 1500;
                m_state->loupeToastMessage = m_state->recordSetup.captureSystemAudio
                    ? (zh ? L"系统声音: 已开启" : L"System Audio: ON")
                    : (zh ? L"系统声音: 已关闭" : L"System Audio: OFF");
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            }
            break;
        }

        case ToolbarCommand::RecordToggleMicrophone: {
            if (m_state->recordSetup.format != RecordFormat::GIF) {
                m_state->recordSetup.captureMicrophone = !m_state->recordSetup.captureMicrophone;
                tools3000::core::ConfigManager::instance().set<bool>(
                    "/recording/captureMicrophone", m_state->recordSetup.captureMicrophone);
                const bool zh = tools3000::core::isUiLanguageChinese();
                m_state->loupeToastUntil = GetTickCount() + 1500;
                m_state->loupeToastMessage = m_state->recordSetup.captureMicrophone
                    ? (zh ? L"麦克风: 已开启" : L"Microphone: ON")
                    : (zh ? L"麦克风: 已静音" : L"Microphone: Muted");
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            }
            break;
        }

        case ToolbarCommand::RecordSelectMicSource: {
            if (m_state->recordSetup.format != RecordFormat::GIF) {
                D2D1_RECT_F splitAnchor = button.rect;
                for (const auto& tb : m_state->toolbarButtons) {
                    if (tb.command == ToolbarCommand::RecordToggleMicrophone) {
                        splitAnchor.left = (std::min)(splitAnchor.left, tb.rect.left);
                        splitAnchor.right = (std::max)(splitAnchor.right, tb.rect.right);
                        break;
                    }
                }
                openDropdownMenu(DropdownType::RecordMicSource, splitAnchor);
            }
            break;
        }

        case ToolbarCommand::RecordToggleSettingsMenu: {
            openDropdownMenu(DropdownType::RecordMoreSettings, button.rect);
            break;
        }

        case ToolbarCommand::RecordStartConfirm: {
            if (m_confirmCb) m_confirmCb({});
            break;
        }

        case ToolbarCommand::SideCycleAspectRatio: {
            int cur = static_cast<int>(m_state->aspectRatio);
            int next = (cur + 1) % static_cast<int>(AspectRatioPreset::COUNT);
            m_state->aspectRatio = static_cast<AspectRatioPreset>(next);
            enforceAspectRatioOnCurrentSelection();
            const bool zh = tools3000::core::isUiLanguageChinese();
            static const wchar_t* ratioNamesZh[] = { L"自由比例", L"16:9 宽屏", L"4:3 标清", L"1:1 正方形", L"黄金比例 (1.618:1)" };
            static const wchar_t* ratioNamesEn[] = { L"Free", L"16:9 Widescreen", L"4:3 Standard", L"1:1 Square", L"Golden Ratio (1.618:1)" };
            const wchar_t* rname = (next >= 0 && next < 5)
                ? (zh ? ratioNamesZh[next] : ratioNamesEn[next])
                : (zh ? L"自由比例" : L"Free");
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = (zh ? L"选区比例: " : L"Aspect Ratio: ") + std::wstring(rname);
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleNumberShape: {
            if (button.tool == MarkupTool::Magnifier || m_state->currentTool == MarkupTool::Magnifier) {
                m_state->currentMagnifierShape = (m_state->currentMagnifierShape == NumberBadgeShape::Circle)
                    ? NumberBadgeShape::RoundedSquare
                    : NumberBadgeShape::Circle;
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->numberShape = m_state->currentMagnifierShape;
                    m_renderer->markMarkupDirty();
                }
            } else {
                if (m_state->currentNumberShape == NumberBadgeShape::Circle) {
                    m_state->currentNumberShape = NumberBadgeShape::RoundedSquare;
                } else {
                    m_state->currentNumberShape = NumberBadgeShape::Circle;
                }
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->numberShape = m_state->currentNumberShape;
                    m_renderer->markMarkupDirty();
                }
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleNumberFormatDropdown: {
            m_state->sliderPopup.type = SliderPopupType::None;
            openDropdownMenu(DropdownType::NumberFormat, button.rect);
            break;
        }

        case ToolbarCommand::ToggleMagnifierCalloutStyleDropdown: {
            m_state->sliderPopup.type = SliderPopupType::None;
            openDropdownMenu(DropdownType::MagnifierCalloutStyle, button.rect);
            break;
        }

        case ToolbarCommand::ToggleSpotlightShape: {
            m_state->currentSpotlightEllipse = !m_state->currentSpotlightEllipse;
            tools3000::core::ConfigManager::instance().set<bool>(
                "/capture/markup/spotlightEllipse", m_state->currentSpotlightEllipse);
            if (m_state->currentSpotlightEllipse && m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                m_state->sliderPopup.type = SliderPopupType::None;
            }
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Spotlight) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->fill = m_state->currentSpotlightEllipse;
                m_state->activeElement->spotlightEllipse = m_state->currentSpotlightEllipse;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleSpotlightDim: {
            if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                m_state->sliderPopup.type = SliderPopupType::None;
            } else {
                closeAllCapturePopups(*m_state, false);
                m_state->sliderPopup.type = SliderPopupType::SpotlightDim;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleMagnifierScale: {
            const float curScale = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier)
                ? m_state->activeElement->magnifierScale
                : m_state->dynamicMagnifierScale;
            float nextScale = 2.0f;
            if (curScale < 1.75f) nextScale = 2.0f;
            else if (curScale < 2.5f) nextScale = 3.0f;
            else if (curScale < 3.5f) nextScale = 4.0f;
            else nextScale = 1.5f;

            m_state->dynamicMagnifierScale = nextScale;
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->magnifierScale = nextScale;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleMagnifierCallout: {
            const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
            const bool zh = tools3000::core::isUiLanguageChinese();
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                auto* elem = m_state->activeElement;
                const bool unsetEnd = !elem->hasLeaderArrow && elem->endPt.x == 0 && elem->endPt.y == 0 &&
                                      (elem->startPt.x != 0 || elem->startPt.y != 0);
                const cv::Point lc = unsetEnd ? elem->startPt : elem->endPt;
                const bool currentlyOffset = std::hypot(static_cast<double>(lc.x - elem->startPt.x),
                                                        static_cast<double>(lc.y - elem->startPt.y)) > 8.0;
                if (currentlyOffset) {
                    elem->endPt = elem->startPt;
                    elem->hasLeaderArrow = false;
                    m_state->magnifierCalloutMode = false;
                } else {
                    const auto sel = currentSelectionRect();
                    const int selW = std::max(1, static_cast<int>(std::round(sel.right - sel.left)));
                    const int selH = std::max(1, static_cast<int>(std::round(sel.bottom - sel.top)));
                    elem->endPt = computeSmartMagnifierCalloutCenter(
                        elem->startPt, elem->magnifierRadius, elem->magnifierScale, selW, selH, curDpi);
                    elem->hasLeaderArrow = std::hypot(static_cast<double>(elem->endPt.x - elem->startPt.x),
                                                      static_cast<double>(elem->endPt.y - elem->startPt.y)) > 8.0;
                    m_state->magnifierCalloutMode = true;
                }
                m_renderer->markMarkupDirty();
            } else {
                m_state->magnifierCalloutMode = !m_state->magnifierCalloutMode;
            }
            tools3000::core::ConfigManager::instance().set<bool>(
                "/capture/markup/magnifier/calloutMode", m_state->magnifierCalloutMode);
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = m_state->magnifierCalloutMode
                ? (zh ? L"已切换为引出放大镜" : L"Callout magnifier")
                : (zh ? L"已切换为原位放大镜" : L"In-place magnifier");
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ResetNumberCounter: {
            const NumberFormat targetFmt = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number)
                ? m_state->activeElement->numberFormat
                : m_state->currentNumberFormat;
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number) {
                m_state->activeElement->isActive = false;
                m_state->activeElement = nullptr;
                m_renderer->markMarkupDirty();
            }
            m_state->markup.resetNumber(1, targetFmt);
            const bool zh = tools3000::core::isUiLanguageChinese();
            const std::wstring firstLabel = formatNumberBadgeLabel(1, targetFmt);
            m_state->loupeToastUntil = GetTickCount() + 1400;
            m_state->loupeToastMessage = zh
                ? (L"当前序列起始已重置为 " + firstLabel)
                : (L"Sequence reset to " + firstLabel);
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::Undo:
            m_state->activeElement = nullptr;
            m_state->isManipulating = false;
            m_state->hasManipulateSnapshot = false;
            m_state->dragHandle = HitArea::None;
            resetCoalescedUndo(*m_state);
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
            break;

        case ToolbarCommand::Redo:
            m_state->activeElement = nullptr;
            m_state->isManipulating = false;
            m_state->hasManipulateSnapshot = false;
            m_state->dragHandle = HitArea::None;
            resetCoalescedUndo(*m_state);
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
            break;

        case ToolbarCommand::Clear:
            m_state->activeElement = nullptr;
            m_state->isManipulating = false;
            m_state->hasManipulateSnapshot = false;
            m_state->dragHandle = HitArea::None;
            resetCoalescedUndo(*m_state);
            m_state->markup.clearElementsUndoable();
            m_renderer->markMarkupDirty();
            m_state->toolbarLayoutValid = false;
            prepareMarkupBase();
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleCornerRadius: {
            if (m_state->beautyShell.enabled) {
                static const std::array<float, 7> radiuses = {0.0f, 8.0f, 10.0f, 12.0f, 16.0f, 24.0f, 32.0f};
                auto it = std::find_if(radiuses.begin(), radiuses.end(), [&](float r) {
                    return std::abs(r - m_state->beautyShell.cornerRadius) < 1.0f;
                });
                if (it == radiuses.end()) {
                    m_state->beautyShell.cornerRadius = 16.0f;
                } else {
                    size_t nextIdx = (std::distance(radiuses.begin(), it) + 1) % radiuses.size();
                    m_state->beautyShell.cornerRadius = radiuses[nextIdx];
                }
                tools3000::core::ConfigManager::instance().set<int>(
                    "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
            } else {
                float cfgMaxRadius = static_cast<float>(tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                static const std::array<float, 6> radiuses = {0.0f, 8.0f, 14.0f, 24.0f, 40.0f, 60.0f};
                auto it = std::find_if(radiuses.begin(), radiuses.end(), [&](float r) {
                    return std::abs(r - m_state->cornerRadius) < 1.0f;
                });
                if (it == radiuses.end()) {
                    m_state->cornerRadius = 8.0f;
                } else {
                    size_t nextIdx = (std::distance(radiuses.begin(), it) + 1) % radiuses.size();
                    m_state->cornerRadius = std::min(radiuses[nextIdx], cfgMaxRadius);
                }
                tools3000::core::ConfigManager::instance().set<double>(
                    "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
            }
            prepareMarkupBase();
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ExtractText:
            if (m_state->ocrCallback) {
                int x1 = std::min(m_state->dragStart.x, m_state->dragEnd.x);
                int y1 = std::min(m_state->dragStart.y, m_state->dragEnd.y);
                int w = std::abs(m_state->dragEnd.x - m_state->dragStart.x);
                int h = std::abs(m_state->dragEnd.y - m_state->dragStart.y);
                if (w <= 0 || h <= 0) break;
                prepareMarkupBase();

                cv::Mat cropped;
                if (m_state->markup.elementCount() > 0) cropped = m_state->markup.getCompositeImage();
                else cropped = cropMarkupBase(m_state->frozenScreen, cv::Rect(x1, y1, w, h));

                int offsetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                int offsetY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                RECT winRc{};
                if (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &winRc)) {
                    offsetX = winRc.left;
                    offsetY = winRc.top;
                }
                CaptureRegion region{x1 + offsetX, y1 + offsetY, w, h};
                if (m_state->isLongImageMode()) {
                    const D2D1_RECT_F anchor = m_state->toolbarAnchorRect();
                    region = CaptureRegion{
                        offsetX + static_cast<int>(std::lround(anchor.left)),
                        offsetY + static_cast<int>(std::lround(anchor.top)),
                        std::max(120, static_cast<int>(std::lround(anchor.right - anchor.left))),
                        std::max(120, static_cast<int>(std::lround(anchor.bottom - anchor.top)))
                    };
                }
                auto ocrCb = m_state->ocrCallback;
                if(m_cancelCb) m_cancelCb(); 
                ocrCb(region, cropped);
            }
            break;

        case ToolbarCommand::PinWindow: {
            if (m_state->isMarking) {
                finishMarkup(m_state->currentCursor);
            }
            if (m_state->isLongImageMode() || CaptureOverlay::instance().isEditingPinned()) {
                if (m_confirmCb) {
                    m_confirmCb({CaptureCompletionAction::Pin});
                }
                break;
            }
            int x1 = std::min(m_state->dragStart.x, m_state->dragEnd.x);
            int y1 = std::min(m_state->dragStart.y, m_state->dragEnd.y);
            int x2 = std::max(m_state->dragStart.x, m_state->dragEnd.x);
            int y2 = std::max(m_state->dragStart.y, m_state->dragEnd.y);
            int w = x2 - x1;
            int h = y2 - y1;
            if (w <= 0 || h <= 0) break;
            prepareMarkupBase();

            cv::Mat cleanBase = cropMarkupBase(m_state->frozenScreen, cv::Rect(x1, y1, w, h));
            std::vector<MarkupElement> initialElements = m_state->markup.exportElements();
            cv::Mat cropped;
            if (m_state->markup.elementCount() > 0) cropped = m_state->markup.getCompositeImage();
            else cropped = cleanBase;
            int origX = x1 + GetSystemMetrics(SM_XVIRTUALSCREEN);
            int origY = y1 + GetSystemMetrics(SM_YVIRTUALSCREEN);
            CaptureRegion reg{origX, origY, w, h, m_state->cornerRadius};

            const bool bakedDecorations =
                (m_state->beautyShell.enabled || m_state->cornerRadius > 0.5f) && !cropped.empty();
            if (m_state->beautyShell.enabled && !cropped.empty()) {
                cropped = applyBeautyShell(cropped, m_state->beautyShell);
            } else if (m_state->cornerRadius > 0.5f && !cropped.empty()) {
                cropped = applyRoundedCorners(cropped, m_state->cornerRadius);
            }
            if (bakedDecorations) {
                cleanBase = cropped;
                initialElements.clear();
            }

            int padX = (cropped.cols > reg.width && reg.width > 0) ? (cropped.cols - reg.width) / 2 : 0;
            int padY = (cropped.rows > reg.height && reg.height > 0) ? (cropped.rows - reg.height) / 2 : 0;

            // 写入剪贴板与历史记录（贴图窗口保留纯净无水印底图，写入剪贴板的外发副本按配置自动压印水印）
            CaptureHistory::instance().push(cropped, reg);
            const cv::Mat clipboardCopy = WatermarkRenderer::applyGlobalCaptureWatermarkIfEnabled(cropped);
            ClipboardUtils::copyImageToClipboard(clipboardCopy, L"", nullptr, &reg, padX, padY);

            // 计算智能贴图坐标（原位对齐并避让重叠）
            POINT spawnPos = PinWindow::calculateSmartSpawnPosition(cropped.cols, cropped.rows, nullptr, &reg, padX, padY);

            // 先关闭截图覆盖层
            if (m_cancelCb) m_cancelCb();

            // 精确原位贴图（携带 1:1 无损底图与独立矢量图元，支持后续任意缩放态无损增删改）
            PinWindow::create(cropped, spawnPos.x, spawnPos.y, 0.0f, {}, cleanBase, std::move(initialElements));
            break;
        }

        case ToolbarCommand::ScrollCapture: {
            int x1 = std::min(m_state->dragStart.x, m_state->dragEnd.x);
            int y1 = std::min(m_state->dragStart.y, m_state->dragEnd.y);
            int w = std::abs(m_state->dragEnd.x - m_state->dragStart.x);
            int h = std::abs(m_state->dragEnd.y - m_state->dragStart.y);

            int offsetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int offsetY = GetSystemMetrics(SM_YVIRTUALSCREEN);

            RECT capRect = {x1 + offsetX, y1 + offsetY, x1 + offsetX + w, y1 + offsetY + h};

            // 先关闭常规覆盖层与提示浮窗，长截图统一使用带硬件防截隔离的专属交互式控制台
            ShortcutHintOverlay::instance().hide();
            if (m_cancelCb) m_cancelCb();

            ScrollCaptureOptions opts;
            opts.captureRect = capRect;
            opts.mode = ScrollMode::Auto;
            opts.interactiveHoldAtBottom = true;
            opts.isUiHotZone = [](POINT pt) {
                return ScrollCaptureOverlay::instance().isPointInControlZone(pt);
            };
            ScrollCaptureOverlay::instance().show(capRect);
            ScrollCapture::instance().start(opts);
            if (!ScrollCapture::instance().isRunning()) {
                ScrollCaptureOverlay::instance().hide();
            }
            break;
        }

        case ToolbarCommand::StartRecord: {
            if (m_state->mode == OverlayMode::RecordRegion) {
                if (m_confirmCb) m_confirmCb({});
            } else {
                transitionToRecordMode(*m_state);
                if (m_renderer) {
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                }
            }
            break;
        }

        case ToolbarCommand::Copy:
            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
            break;

        case ToolbarCommand::Confirm:
            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Default});
            break;

        case ToolbarCommand::Cancel:
            if(m_cancelCb) m_cancelCb();
            break;
    }
}


void CaptureInput::openDropdownMenu(DropdownType type, const D2D1_RECT_F& anchorRect) {
    if (!m_state) return;
    tools3000::capture::openDropdownMenu(*m_state, type, anchorRect);
    if (m_renderer) m_renderer->invalidate();
}

void CaptureInput::commitDropdownSelection(DropdownType type, int id) {
    if (!m_state) return;
    tools3000::capture::commitDropdownSelection(*m_state, type, id);
    if (m_renderer) {
        if (type == DropdownType::MagnifierCalloutStyle && m_state->activeElement) {
            m_renderer->markMarkupDirty();
        }
        m_renderer->invalidate();
    }
}

} // namespace tools3000::capture

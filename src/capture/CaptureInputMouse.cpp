// ─────────────────────────────────────────────────────────────────────────────
// CaptureInputMouse.cpp — 截图鼠标事件（左/中/右键、拖拽选区、双击与悬浮光标）子模块
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include "core/i18n/UiI18n.h"
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

LRESULT CaptureInput::handleMouseMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SETCURSOR: {
            if (!this || !m_state) break;
            ensureCursorVisible();
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            updateHoverCursor(pt);
            return TRUE;
        }
        case WM_MBUTTONDOWN: {
            if (this && m_state && m_state->isLongImageMode()) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                m_state->longCanvas.middleButtonPanning = true;
                m_state->longCanvas.panLastCursor = pt;
                updateHoverCursor(pt);
                return 0;
            }
            break;
        }
        case WM_MBUTTONUP: {
            if (this && m_state && m_state->longCanvas.middleButtonPanning) {
                m_state->longCanvas.middleButtonPanning = false;
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                updateHoverCursor(pt);
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            if (!this) break;
            CaptureShortcutPolicy::cancelShiftTapCandidate(m_state->shiftTapCandidate);
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if ((int)m_state->state.load() == (int)OverlayState::Selected || (int)m_state->state.load() == (int)OverlayState::Marking) {
                // 0. 如果滑块微浮岛弹窗处于展开状态，优先响应无级滑块轨道点击与拖拽
                if (m_state->sliderPopup.type != SliderPopupType::None) {
                    float px = static_cast<float>(point.x);
                    float py = static_cast<float>(point.y);
                    auto pr = m_state->sliderPopup.popupRect;
                    if (px >= pr.left && px <= pr.right && py >= pr.top && py <= pr.bottom) {
                        // 检测是否点击或拖拽滑动轨道
                        auto tr = m_state->sliderPopup.trackRect;
                        if (px >= tr.left - 8.0f && px <= tr.right + 8.0f && py >= tr.top - 12.0f && py <= tr.bottom + 12.0f) {
                            if (m_state->activeElement &&
                                m_state->sliderPopup.type != SliderPopupType::SelectionCornerRadius) {
                                m_state->markup.pushUndoSnapshot();
                                resetCoalescedUndo(*m_state);
                            }
                            m_state->sliderPopup.isDragging = true;
                            float pct = std::clamp((px - tr.left) / (tr.right - tr.left), 0.0f, 1.0f);
                            int val = m_state->sliderPopup.minValue + static_cast<int>(std::round(pct * (m_state->sliderPopup.maxValue - m_state->sliderPopup.minValue)));
                            if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
                                if (m_state->beautyShell.enabled) {
                                    m_state->beautyShell.cornerRadius = static_cast<float>(val);
                                    tools3000::core::ConfigManager::instance().set<int>("/capture/beautyShellRadius", val);
                                } else {
                                    m_state->cornerRadius = static_cast<float>(val);
                                    tools3000::core::ConfigManager::instance().set<double>("/screenshot/cornerRadius", static_cast<double>(val));
                                }
                                prepareMarkupBase();
                            } else if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                                m_state->currentElementCornerRadius = static_cast<float>(val);
                            } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                                m_state->currentSpotlightDimAlpha = static_cast<float>(std::clamp(val, 15, 85)) / 100.0f;
                                tools3000::core::ConfigManager::instance().set<int>("/capture/markup/spotlightDim", std::clamp(val, 15, 85));
                            } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                                    m_state->activeElement->fontSize = static_cast<float>(val);
                                    ensureActiveTextBoundsAfterEdit(*m_state->activeElement);
                                }
                                const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                                tools3000::core::ConfigManager::instance().set<double>("/capture/markup/text/fontSize", static_cast<double>(val) / curDpi);
                            } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                                m_state->currentOutlineWidth = std::clamp(val, 0, 12);
                                if (m_state->currentTool == MarkupTool::Text) {
                                    m_state->currentTextOutline = (m_state->currentOutlineWidth > 0);
                                }
                            } else {
                                m_state->currentStrokeWidth = val;
                            }
                            if (m_state->activeElement) {
                                if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                                    m_state->activeElement->cornerRadius = static_cast<float>(val);
                                } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                                    if (m_state->activeElement->tool == MarkupTool::Spotlight) {
                                        m_state->activeElement->spotlightDimAlpha = m_state->currentSpotlightDimAlpha;
                                    }
                                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                                    m_state->activeElement->fontSize = static_cast<float>(val);
                                    m_state->activeElement->textRenderSize = cv::Size(0, 0);
                                } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                                    m_state->activeElement->outlineThickness = static_cast<float>(std::clamp(val, 0, 12));
                                    m_state->activeElement->hasCustomOutline = true;
                                    m_state->activeElement->textOutline = (val > 0);
                                    if (m_state->activeElement->tool == MarkupTool::Text) {
                                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                                    }
                                } else if (m_state->sliderPopup.type != SliderPopupType::SelectionCornerRadius) {
                                    m_state->activeElement->thickness = static_cast<float>(val);
                                    if (m_state->activeElement->tool == MarkupTool::Mosaic) {
                                        m_state->activeElement->mosaicBlockSize = (std::max)(4, val * 2);
                                    } else if (m_state->activeElement->tool == MarkupTool::Blur) {
                                        m_state->activeElement->mosaicBlockSize = (std::max)(5, val * 3);
                                    } else if (m_state->activeElement->tool == MarkupTool::Inpaint) {
                                        m_state->activeElement->inpaintRadius = std::clamp(val, 2, 16);
                                    } else if (m_state->activeElement->tool == MarkupTool::Watermark) {
                                        const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                                        m_state->activeElement->fontSize = std::clamp(12.0f + static_cast<float>(val) * 1.5f, 12.0f, 64.0f) * curDpi;
                                    }
                                }
                                m_renderer->markMarkupDirty();
                            }
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                            return 0;
                        }
                        return 0;
                    } else {
                        // 点击滑块外部区域，优雅收起
                        m_state->sliderPopup.type = SliderPopupType::None;
                        m_state->sliderPopup.isDragging = false;
                        m_renderer->invalidate();
                    }
                }

                float px = static_cast<float>(point.x);
                float py = static_cast<float>(point.y);

                // 1. 严格按渲染 Z 序自顶向下检测已展开弹出层（尺寸菜单 / 下拉菜单 / 样式子菜单），优先级高于底层 HUD 与二维码胶囊
                if (m_state->isSizeMenuOpen) {
                    auto mr = m_state->sizeMenuRect;
                    if (px >= mr.left && px <= mr.right && py >= mr.top && py <= mr.bottom) {
                        const float scale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                        const auto lay = computeSizeMenuLayoutFromRect(mr, scale);
                        const auto inR = [&](const D2D1_RECT_F& r) {
                            return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
                        };

                        if (inR(lay.segmentTrackRect)) {
                            const float segMidX = (lay.segmentTrackRect.left + lay.segmentTrackRect.right) * 0.5f;
                            m_state->sizeUnit = (px <= segMidX)
                                ? CaptureState::SizeUnit::Pixel
                                : CaptureState::SizeUnit::DeviceIndependentPixel;
                            tools3000::core::ConfigManager::instance().set<std::string>(
                                "/capture/sizeUnit",
                                m_state->sizeUnit == CaptureState::SizeUnit::DeviceIndependentPixel ? "dp" : "px");
                        } else if (inR(lay.positionRowRect)) {
                            m_state->showPositionInHud = !m_state->showPositionInHud;
                            tools3000::core::ConfigManager::instance().set<bool>(
                                "/capture/showSizePosition", m_state->showPositionInHud);
                        } else if (inR(lay.unitRowRect)) {
                            m_state->showUnitInHud = !m_state->showUnitInHud;
                            tools3000::core::ConfigManager::instance().set<bool>(
                                "/capture/showSizeUnit", m_state->showUnitInHud);
                        }
                        m_renderer->invalidate();
                        return 0;
                    } else {
                        auto hr = m_state->sizeHudRect;
                        m_state->isSizeMenuOpen = false;
                        m_renderer->invalidate();
                        // 若点击的就是尺寸胶囊自身，则已完成 Toggle 收起，直接返回
                        if (px >= hr.left && px <= hr.right && py >= hr.top && py <= hr.bottom) {
                            return 0;
                        }
                    }
                }

                // 1.2 微晶下拉菜单（聚光灯/几何图形、序号样式、美化外壳、录屏设置及二级级联菜单）点击与面板内边距防穿透
                if (m_state->dropdownMenu.type != DropdownType::None) {
                    auto handlePostDropdownCommit = [&]() {
                        if (m_state->recordSetup.requestOpenMoreSettings) {
                            m_state->recordSetup.requestOpenMoreSettings = false;
                            tools3000::capture::closeDropdownMenu(*m_state, false);
                            if (m_cancelCb) m_cancelCb();
                            tools3000::core::MainThreadDispatcher::instance().post([]() {
                                tools3000::core::MessageBridge::instance().invokeHandler(
                                    "tray.action", {{"action", "openSettings"}, {"page", "capture"}});
                            });
                            return;
                        }
                        m_renderer->invalidate();
                    };

                    if (m_state->dropdownMenu.activeParentIndex >= 0 &&
                        m_state->dropdownMenu.activeParentIndex < static_cast<int>(m_state->dropdownMenu.items.size())) {
                        const auto& parent = m_state->dropdownMenu.items[m_state->dropdownMenu.activeParentIndex];
                        if (parent.hasChildMenu && !parent.childItems.empty()) {
                            for (const auto& cItem : parent.childItems) {
                                if (px >= cItem.rect.left && px <= cItem.rect.right &&
                                    py >= cItem.rect.top && py <= cItem.rect.bottom) {
                                    commitDropdownSelection(m_state->dropdownMenu.type, cItem.id);
                                    handlePostDropdownCommit();
                                    return 0;
                                }
                            }
                            const auto cmRect = m_state->dropdownMenu.childMenuRect;
                            if (cmRect.right > cmRect.left &&
                                px >= cmRect.left && px <= cmRect.right &&
                                py >= cmRect.top && py <= cmRect.bottom) {
                                return 0;
                            }
                        }
                    }

                    int hitItem = -1;
                    for (int i = 0; i < static_cast<int>(m_state->dropdownMenu.items.size()); ++i) {
                        const auto& item = m_state->dropdownMenu.items[i];
                        if (px >= item.rect.left && px <= item.rect.right &&
                            py >= item.rect.top && py <= item.rect.bottom) {
                            hitItem = i;
                            break;
                        }
                    }
                    if (hitItem >= 0) {
                        const auto& item = m_state->dropdownMenu.items[hitItem];
                        if (item.isSeparator) {
                            return 0;
                        }
                        if (item.hasChildMenu) {
                            m_state->dropdownMenu.activeParentIndex = hitItem;
                            m_state->dropdownMenu.hoveredIndex = hitItem;
                            m_state->dropdownMenu.hoveredChildIndex = -1;
                            m_renderer->invalidate();
                            return 0;
                        }
                        commitDropdownSelection(m_state->dropdownMenu.type, item.id);
                        handlePostDropdownCommit();
                        return 0;
                    }
                    const auto dmRect = m_state->dropdownMenu.menuRect;
                    if (dmRect.right > dmRect.left && px >= dmRect.left && px <= dmRect.right &&
                        py >= dmRect.top && py <= dmRect.bottom) {
                        return 0; // 点击在下拉面板内边距中，消费事件防止穿透底层控件
                    }
                    const auto anchorRc = m_state->dropdownMenu.anchorButtonRect;
                    tools3000::capture::closeDropdownMenu(*m_state, true);
                    m_renderer->invalidate();
                    if (anchorRc.right > anchorRc.left && px >= anchorRc.left && px <= anchorRc.right &&
                        py >= anchorRc.top && py <= anchorRc.bottom) {
                        return 0; // 点击原触发按钮完成 Toggle 收起，避免立刻重开或误切换工具
                    }
                }

                // 1.3 线型 / 箭头样式子菜单点击与外部自动收起
                SubmenuType dismissedSubmenu = SubmenuType::None;
                if (m_state->openSubmenu != SubmenuType::None) {
                    for (const auto& sbtn : m_state->submenuButtons) {
                        if (px >= sbtn.rect.left && px <= sbtn.rect.right &&
                            py >= sbtn.rect.top && py <= sbtn.rect.bottom) {
                            const ToolbarButton copied = sbtn;
                            executeToolbarCommand(copied);
                            m_renderer->invalidate();
                            return 0;
                        }
                    }
                    const auto smRect = m_state->openSubmenuRect;
                    if (smRect.right > smRect.left && px >= smRect.left && px <= smRect.right &&
                        py >= smRect.top && py <= smRect.bottom) {
                        return 0;
                    }
                    dismissedSubmenu = m_state->openSubmenu;
                    m_state->openSubmenu = SubmenuType::None;
                    m_renderer->invalidate();
                }

                // 2. 检测工具栏手柄与工具栏按钮（高于尺寸胶囊与二维码胶囊，防止拖拽后的工具栏被底层胶囊抢走点击）
                HitArea selHit = HitArea::None;
                rebuildToolbarButtons(currentSelectionRect());
                if (m_state->toolbarGripRect.right > m_state->toolbarGripRect.left &&
                    px >= m_state->toolbarGripRect.left && px <= m_state->toolbarGripRect.right &&
                    py >= m_state->toolbarGripRect.top && py <= m_state->toolbarGripRect.bottom) {
                    closeAllCapturePopups(*m_state, true);
                    m_state->isDraggingToolbar = true;
                    m_state->toolbarDragOffset = D2D1::Point2F(
                        px - m_state->primaryToolbarRect.left,
                        py - m_state->primaryToolbarRect.top);
                    updateHoverCursor(point);
                    m_renderer->invalidate();
                    return 0;
                }
                if (auto* button = hitTestToolbar(point)) {
                    if (dismissedSubmenu != SubmenuType::None) {
                        const bool sameSubmenuTrigger =
                            (dismissedSubmenu == SubmenuType::LineStyle && button->command == ToolbarCommand::ToggleLineStyleDropdown) ||
                            (dismissedSubmenu == SubmenuType::ArrowStyle && button->command == ToolbarCommand::ToggleArrowStyleDropdown) ||
                            (dismissedSubmenu == SubmenuType::StartCapStyle && button->command == ToolbarCommand::ToggleStartCapDropdown) ||
                            (dismissedSubmenu == SubmenuType::EndCapStyle && button->command == ToolbarCommand::ToggleEndCapDropdown);
                        if (sameSubmenuTrigger) {
                            m_renderer->invalidate();
                            return 0;
                        }
                    }
                    // 主工具栏形状组按钮（第 1 个）：已激活形状/聚光灯时点击右下角小箭头区，直接弹出「几何图形 / 聚光灯」切换菜单
                    if (!button->isSecondary && !m_state->toolbarButtons.empty() &&
                        button == &m_state->toolbarButtons[0] && button->hasDropdown &&
                        m_state->mode == OverlayMode::Screenshot) {
                        const float curScale = (m_state->dpiScale > 0.0f) ? m_state->dpiScale : 1.0f;
                        const bool isShapeOrSpotActive = m_state->isMarkupToolActive &&
                            (m_state->currentTool == MarkupTool::Rectangle ||
                             m_state->currentTool == MarkupTool::Ellipse ||
                             m_state->currentTool == MarkupTool::Line ||
                             m_state->currentTool == MarkupTool::Spotlight);
                        if (isShapeOrSpotActive && px >= button->rect.right - 11.0f * curScale) {
                            openDropdownMenu(DropdownType::ShapeMode, button->rect);
                            m_renderer->invalidate();
                            return 0;
                        }
                    }
                    // 如果正在编辑文字且点击的不是文本属性设置按钮，退出编辑
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                        if (button->command != ToolbarCommand::SelectColor &&
                            button->command != ToolbarCommand::ChooseCustomColor &&
                            button->command != ToolbarCommand::ToggleFill &&
                            button->command != ToolbarCommand::CycleStrokeWidth &&
                            button->command != ToolbarCommand::ToggleTextOutline &&
                            button->command != ToolbarCommand::SelectTextOutlineColor) {
                            m_state->activeElement->isEditing = false;
                            m_state->isSelectingText = false;
                            if (m_state->activeElement->text.empty()) {
                                m_state->markup.removeElement(m_state->activeElement->id);
                                m_state->activeElement = nullptr;
                            }
                            m_renderer->markMarkupDirty();
                            syncShortcutHintContext();
                        }
                    }
                    executeToolbarCommand(*button);
                    return 0;
                }

                // 若点击落在悬浮工具栏玻璃底板的按钮间隙内，直接消费事件，防止穿透到底层画布误画标注或挪动选区
                auto inToolbarPanel = [&](const D2D1_RECT_F& rc) {
                    return rc.right > rc.left && px >= rc.left && px <= rc.right && py >= rc.top && py <= rc.bottom;
                };
                if (inToolbarPanel(m_state->primaryToolbarRect) ||
                    inToolbarPanel(m_state->secondaryToolbarRect) ||
                    inToolbarPanel(m_state->selectionSideRect)) {
                    return 0;
                }

                // 2.3. 长图精修工作台专属导航舱（顶部首尾速跳/重置/缩放胶囊 + 右侧全景 Minimap + Space 临时抓手）
                if (m_state->isLongImageMode()) {
                    const float curScale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                    if (inToolbarPanel(m_state->longCanvas.topHeaderRect)) {
                        closeAllCapturePopups(*m_state, true);
                        if (inToolbarPanel(m_state->longCanvas.btnJumpTopRect)) {
                            const float topDocRow = static_cast<float>(std::min(m_state->dragStart.y, m_state->dragEnd.y));
                            m_state->scrollLongCanvasToDocRow(topDocRow, 0.12f);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        } else if (inToolbarPanel(m_state->longCanvas.btnJumpBottomRect)) {
                            const float botDocRow = static_cast<float>(std::max(m_state->dragStart.y, m_state->dragEnd.y));
                            m_state->scrollLongCanvasToDocRow(botDocRow, 0.82f);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        } else if (inToolbarPanel(m_state->longCanvas.btnResetCropRect)) {
                            m_state->dragStart = {0, 0};
                            m_state->dragEnd = {m_state->longCanvas.docWidth, m_state->longCanvas.docHeight};
                            prepareMarkupBase();
                            const bool zh = tools3000::core::isUiLanguageChinese();
                            m_state->loupeToastMessage = tools3000::core::trWCstrFor(zh, L"已无损恢复完整长图", L"Restored full long image");
                            m_state->loupeToastUntil = GetTickCount() + 1400;
                            SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        } else if (inToolbarPanel(m_state->longCanvas.btnZoomToggleRect)) {
                            const auto& stg = m_state->longCanvas.stageRect;
                            const auto centerDoc = m_state->screenToDoc(
                                (stg.left + stg.right) * 0.5f,
                                (stg.top + stg.bottom) * 0.5f);
                            const float fitZ = std::clamp(m_state->longCanvas.fitZoom, 0.15f, 4.0f);
                            if (std::abs(m_state->longCanvas.zoom - 1.0f) > 0.03f) {
                                m_state->longCanvas.zoom = 1.0f;
                            } else {
                                m_state->longCanvas.zoom = (std::abs(fitZ - 1.0f) > 0.03f) ? fitZ : 0.75f;
                            }
                            m_state->longCanvas.scrollX = 0.0f;
                            m_state->scrollLongCanvasToDocRow(centerDoc.y, 0.5f);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        }
                        updateHoverCursor(point);
                        return 0;
                    }

                    if (inToolbarPanel(m_state->longCanvas.minimapPanelRect)) {
                        closeAllCapturePopups(*m_state, true);
                        const auto& mmImg = m_state->longCanvas.minimapImageRect;
                        const int curTop = std::min(m_state->dragStart.y, m_state->dragEnd.y);
                        const int curBot = std::max(m_state->dragStart.y, m_state->dragEnd.y);
                        const float mmTopY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(
                            curTop, mmImg, m_state->longCanvas.docHeight);
                        const float mmBotY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(
                            curBot, mmImg, m_state->longCanvas.docHeight);
                        const float hitBand = 9.5f * curScale;

                        if (px >= mmImg.left - 10.0f * curScale && px <= mmImg.right + 10.0f * curScale &&
                            std::abs(py - mmTopY) <= hitBand) {
                            m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::MinimapCropTop;
                        } else if (px >= mmImg.left - 10.0f * curScale && px <= mmImg.right + 10.0f * curScale &&
                                   std::abs(py - mmBotY) <= hitBand) {
                            m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::MinimapCropBottom;
                        } else {
                            m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::MinimapViewport;
                            const int targetRow = ScrollCaptureEditorOverlay::mapMinimapYToImageRow(
                                py, mmImg, m_state->longCanvas.docHeight);
                            m_state->scrollLongCanvasToDocRow(static_cast<float>(targetRow), 0.5f);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        }
                        updateHoverCursor(point);
                        return 0;
                    }

                    if (((GetKeyState(VK_SPACE) & 0x8000) != 0) && inToolbarPanel(m_state->longCanvas.stageRect)) {
                        m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::PanCanvas;
                        m_state->longCanvas.panLastCursor = point;
                        updateHoverCursor(point);
                        return 0;
                    }
                }

                // 2.4. 检测是否点击尺寸胶囊（HUD）以展开菜单（互斥收起其他弹出层）
                auto hr = m_state->sizeHudRect;
                if (px >= hr.left && px <= hr.right && py >= hr.top && py <= hr.bottom) {
                    const bool willOpen = !m_state->isSizeMenuOpen;
                    closeAllCapturePopups(*m_state, true);
                    m_state->isSizeMenuOpen = willOpen;
                    m_renderer->invalidate();
                    return 0;
                }

                // 2.5. 检测是否点击二维码胶囊 Chip (一键复制到剪贴板)
                if (!m_state->detectedQrText.empty() && m_state->qrChipRect.right > m_state->qrChipRect.left) {
                    if (px >= m_state->qrChipRect.left && px <= m_state->qrChipRect.right &&
                        py >= m_state->qrChipRect.top && py <= m_state->qrChipRect.bottom) {
                        tools3000::core::WinUtils::copyToClipboard(tools3000::core::WinUtils::utf8ToWstring(m_state->detectedQrText), hwnd);
                        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                            tools3000::core::trWCstr(L"已复制二维码内容到剪贴板", L"Copied QR code content to clipboard")});
                        return 0;
                    }
                }

                const bool inSelection = isPointInSelection(point);
                cv::Point local = inSelection ? toMarkupPoint(point) : cv::Point{0, 0};

                // 3. 选区控制点/外框拉伸手柄检测 (优先级高于画布标注元素，彻底解决正在编辑文本时外框手柄被文本框抢占的严重缺陷)
                int cornerIdx = -1;
                selHit = hitTestSelectionBox(point, &cornerIdx);

                if (selHit == HitArea::CornerRadius || (selHit != HitArea::None && selHit != HitArea::Body)) {
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                        m_state->activeElement->isEditing = false;
                        if (m_state->activeElement->text.empty()) {
                            m_state->markup.removeElement(m_state->activeElement->id);
                            m_state->activeElement = nullptr;
                        }
                        m_renderer->markMarkupDirty();
                        syncShortcutHintContext();
                    }
                    if (selHit == HitArea::CornerRadius) {
                        m_state->isAdjustingCornerRadius = true;
                        m_state->cornerDragStartRadius = m_state->effectiveCornerRadius();
                        m_state->cornerDragStartPos = point;
                        m_state->cornerDragIndex = (cornerIdx >= 0) ? cornerIdx : 0;
                        updateHoverCursor(point);
                        return 0;
                    }
                    m_state->isAdjustingSelection = true;
                    m_state->selAdjustHandle = selHit;
                    m_state->selAdjustLast = point;
                    m_state->longCanvas.lastSelAdjustDocPt = m_state->screenToDoc(px, py);
                    updateHoverCursor(point);
                    return 0;
                }

                // 录屏选区模式下：选区边框带 (HitArea::Body) 用于整体移动录屏选区
                if (m_state->mode == OverlayMode::RecordRegion && selHit == HitArea::Body) {
                    m_state->isAdjustingSelection = true;
                    m_state->selAdjustHandle = HitArea::Body;
                    m_state->selAdjustLast = point;
                    updateHoverCursor(point);
                    return 0;
                }

                if (!inSelection && selHit == HitArea::Body) {
                    if (m_state->isDrawingMultiPointLine) {
                        finalizeMultiPointLine(true);
                    }
                    m_state->isAdjustingSelection = true;
                    m_state->selAdjustHandle = HitArea::Body;
                    m_state->selAdjustLast = point;
                    updateHoverCursor(point);
                    return 0;
                }

                // 3.5 多点连线创作态优先消费左键落点（不被画布既有标注图元拦截抢焦）
                if (m_state->isDrawingMultiPointLine) {
                    if (!inSelection) {
                        finalizeMultiPointLine(m_state->multiPointDocVertices.size() >= 2);
                        updateHoverCursor(point);
                        return 0;
                    }
                    const POINT rawDocPt = m_state->screenToDocPoint(point);
                    const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (MultiPointLinePolicy::appendVertexOrCloseLoop(*m_state, rawDocPt, shiftHeld)) {
                        finalizeMultiPointLine(true);
                        updateHoverCursor(point);
                        return 0;
                    }
                    m_renderer->invalidate();
                    updateHoverCursor(point);
                    return 0;
                }

                // 4. 检测当前处于激活态的标注元素（包括矩形四角内向微晶圆角把手与 8 方向拉伸手柄或文本框）
                if (inSelection && m_state->activeElement && m_state->activeElement->isActive) {
                    HitArea activeElemHit = m_state->activeElement->hitTestEx(local);
                    if (activeElemHit != HitArea::None) {
                        if (m_state->activeElement->tool == MarkupTool::Text) {
                            const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
                            if (m_state->activeElement->isEditing && activeElemHit == HitArea::Body && !altDown) {
                                // 编辑态下点击文字内容区：定位字符级光标，开启按住左键拖拽划词选中（绝不拖走整个文本框）
                                const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                                const float maxContentW = syncTextElementWrapWidth(*m_state->activeElement);
                                const auto layout = CaptureTextEditor::buildLayout(
                                    wstr, static_cast<int>(m_state->activeElement->fontSize), maxContentW);
                                const float relX = static_cast<float>(local.x - m_state->activeElement->startPt.x);
                                const float relY = static_cast<float>(local.y - m_state->activeElement->startPt.y);
                                const size_t hitIdx = CaptureTextEditor::hitTestCaretIndex(layout, wstr, relX, relY);
                                const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                                m_state->activeElement->textCaret = hitIdx;
                                if (!shiftDown) {
                                    m_state->activeElement->textSelAnchor = hitIdx;
                                }
                                m_state->activeElement->textLastCaretTick = GetTickCount();
                                m_state->textPreferredCaretX = -1.0f;
                                m_state->isSelectingText = true;
                                m_state->isManipulating = false;
                                m_state->lastMousePos = point;
                                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                                syncImeCompositionWindow(hwnd);
                                m_renderer->markMarkupDirty();
                                m_renderer->invalidate();
                                updateHoverCursor(point);
                                return 0;
                            }
                            if (activeElemHit == HitArea::TextMoveBorder ||
                                (m_state->activeElement->isEditing && altDown && activeElemHit == HitArea::Body)) {
                                // 点击外围聚焦虚线框环带（或按住 Alt 拖内部）：保持编辑态并平移整个文本框
                                m_state->isSelectingText = false;
                                m_state->manipulateStartSnapshot = m_state->markup.captureSnapshot();
                                m_state->hasManipulateSnapshot = true;
                                resetCoalescedUndo(*m_state);
                                m_state->dragHandle = HitArea::TextMoveBorder;
                                m_state->isManipulating = true;
                                m_state->lastMousePos = point;
                                m_state->longCanvas.lastManipulateDocPt = m_state->screenToDoc(px, py);
                                m_renderer->invalidate();
                                updateHoverCursor(point);
                                return 0;
                            }
                            if (!m_state->activeElement->isEditing && activeElemHit == HitArea::Body) {
                                m_state->isSelectingText = false;
                                m_state->manipulateStartSnapshot = m_state->markup.captureSnapshot();
                                m_state->hasManipulateSnapshot = true;
                                resetCoalescedUndo(*m_state);
                                m_state->textManipulateStart = point;
                                m_state->dragHandle = HitArea::None;
                                m_state->isManipulating = true;
                                m_state->lastMousePos = point;
                                m_state->longCanvas.lastManipulateDocPt = m_state->screenToDoc(px, py);
                                m_renderer->invalidate();
                                updateHoverCursor(point);
                                return 0;
                            }
                            m_state->isSelectingText = false;
                        }

                        if (activeElemHit == HitArea::CornerRadius) {
                            m_state->cornerDragStartPos = point;
                            m_state->cornerDragStartRadius = m_state->activeElement->cornerRadius;
                            int x1 = (std::min)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                            int y1 = (std::min)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                            int x2 = (std::max)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                            int y2 = (std::max)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                            float w = static_cast<float>(x2 - x1);
                            float h = static_cast<float>(y2 - y1);
                            bool isSmall = (w < 80.0f || h < 80.0f);
                            int hitIdx = CornerRadiusHelper::hitTestHandles(
                                static_cast<float>(x1), static_cast<float>(y1),
                                static_cast<float>(x2), static_cast<float>(y2),
                                m_state->activeElement->cornerRadius,
                                static_cast<float>(local.x), static_cast<float>(local.y),
                                1.0f, 9.0f, isSmall);
                            m_state->cornerDragIndex = (hitIdx >= 0) ? hitIdx : 0;
                        }

                        LOG_DEBUG("CaptureInput: active element handle selected hitArea={}, tool={}",
                                  static_cast<int>(activeElemHit), static_cast<int>(m_state->activeElement->tool));
                        m_state->manipulateStartSnapshot = m_state->markup.captureSnapshot();
                        m_state->hasManipulateSnapshot = true;
                        resetCoalescedUndo(*m_state);
                        m_state->dragHandle = (activeElemHit == HitArea::Body) ? HitArea::None : activeElemHit;
                        m_state->isManipulating = true;
                        m_state->lastMousePos = point;
                        m_state->longCanvas.lastManipulateDocPt = m_state->screenToDoc(px, py);
                        m_renderer->invalidate();
                        updateHoverCursor(point);
                        return 0;
                    }
                }

                // 5. 选区内部点击流转：未激活元素选中 vs 空白区域开始绘制
                if (inSelection) {
                    // 测试是否命中画布上的其他标注元素（空心未填充矩形只命中描边轮廓，不遮挡内部绘制）
                    HitResult hit = m_state->markup.getElementAtEx(local);
                    if (hit.element) {
                        if (m_state->activeElement && m_state->activeElement != hit.element) {
                            m_state->activeElement->isActive = false;
                            m_state->activeElement->isEditing = false;
                            if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                                m_state->markup.removeElement(m_state->activeElement->id);
                            }
                        }

                        m_state->activeElement = hit.element;
                        m_state->activeElement->isActive = true;
                        m_state->isSelectingText = false;

                        // 全标注工具二次编辑框架：同步选中元素的工具类型与全部属性至全局状态
                        m_state->isMarkupToolActive = true;
                        m_state->currentTool = hit.element->tool;
                        m_state->currentStrokeWidth = static_cast<int>(std::round(hit.element->thickness));
                        m_state->currentLineStyle = hit.element->lineStyle;
                        m_state->currentArrowStyle = hit.element->arrowStyle;
                        m_state->currentSmoothCurve = hit.element->smoothCurve;
                        if (hit.element->tool == MarkupTool::Line) {
                            m_state->currentLineArrowEnabled = hit.element->hasCustomCaps;
                        }
                        if (hit.element->hasCustomCaps) {
                            m_state->currentStartCap = hit.element->startCap;
                            m_state->currentEndCap = hit.element->endCap;
                        } else if (hit.element->tool == MarkupTool::Arrow) {
                            switch (hit.element->arrowStyle) {
                                case ArrowStyle::Thin:
                                    m_state->currentStartCap = LineCapStyle::None;
                                    m_state->currentEndCap = LineCapStyle::OpenArrow;
                                    break;
                                case ArrowStyle::Tech:
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
                        }
                        m_state->currentElementCornerRadius = hit.element->cornerRadius;
                        if (hit.element->tool == MarkupTool::Magnifier) {
                            m_state->currentMagnifierColor = hit.element->color;
                            m_state->magnifierShowBadge = hit.element->fill;
                            m_state->currentMagnifierShape = hit.element->numberShape;
                            m_state->dynamicMagnifierScale = hit.element->magnifierScale;
                            const bool unsetEnd = !hit.element->hasLeaderArrow &&
                                                  hit.element->endPt.x == 0 && hit.element->endPt.y == 0 &&
                                                  (hit.element->startPt.x != 0 || hit.element->startPt.y != 0);
                            const cv::Point lc = unsetEnd ? hit.element->startPt : hit.element->endPt;
                            m_state->magnifierCalloutMode =
                                std::hypot(static_cast<double>(lc.x - hit.element->startPt.x),
                                           static_cast<double>(lc.y - hit.element->startPt.y)) > 8.0;
                        } else if (hit.element->tool == MarkupTool::Spotlight) {
                            m_state->currentColor = hit.element->color;
                            m_state->currentSpotlightEllipse = hit.element->fill;
                            m_state->currentSpotlightDimAlpha = hit.element->spotlightDimAlpha;
                            m_state->currentSpotlightBorder = hit.element->spotlightShowBorder;
                        } else {
                            m_state->currentColor = hit.element->color;
                            m_state->currentFillMode = hit.element->fill;
                            m_state->currentNumberShape = hit.element->numberShape;
                            if (hit.element->tool == MarkupTool::Number) {
                                m_state->currentNumberFormat = hit.element->numberFormat;
                            }
                        }
                        if (hit.element->tool == MarkupTool::Blur) {
                            m_state->currentMosaicType = 1;
                        } else if (hit.element->tool == MarkupTool::Mosaic) {
                            m_state->currentMosaicType = 0;
                        }
                        if (hit.element->tool == MarkupTool::Text) {
                            m_state->currentTextOutline = hit.element->textOutline;
                            m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                        } else if (hit.element->hasCustomOutline) {
                            m_state->currentOutlineWidth = static_cast<int>(std::round(hit.element->outlineThickness));
                            m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                        }
                        m_state->toolbarLayoutValid = false;

                        m_state->activeElement->isEditing = false;
                        if (hit.element->tool == MarkupTool::Text) {
                            m_state->textManipulateStart = point;
                        }

                        if (hit.area == HitArea::CornerRadius) {
                            m_state->cornerDragStartPos = point;
                            m_state->cornerDragStartRadius = m_state->activeElement->cornerRadius;
                            if (m_state->activeElement->tool == MarkupTool::Rectangle) {
                                int x1 = (std::min)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                                int y1 = (std::min)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                                int x2 = (std::max)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                                int y2 = (std::max)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                                float w = static_cast<float>(x2 - x1);
                                float h = static_cast<float>(y2 - y1);
                                bool isSmall = (w < 80.0f || h < 80.0f);
                                int hitIdx = CornerRadiusHelper::hitTestHandles(
                                    static_cast<float>(x1), static_cast<float>(y1),
                                    static_cast<float>(x2), static_cast<float>(y2),
                                    m_state->activeElement->cornerRadius,
                                    static_cast<float>(local.x), static_cast<float>(local.y),
                                    1.0f, 9.0f, isSmall);
                                m_state->cornerDragIndex = (hitIdx >= 0) ? hitIdx : 0;
                            }
                        }

                        m_state->manipulateStartSnapshot = m_state->markup.captureSnapshot();
                        m_state->hasManipulateSnapshot = true;
                        resetCoalescedUndo(*m_state);
                        m_state->dragHandle = (hit.area == HitArea::Body || hit.area == HitArea::TextMoveBorder)
                            ? HitArea::None
                            : hit.area;
                        m_state->isManipulating = true;
                        m_state->lastMousePos = point;
                        m_state->longCanvas.lastManipulateDocPt = m_state->screenToDoc(px, py);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        updateHoverCursor(point);
                        return 0;
                    }

                    // 点击在空白区域：
                    // 若此前正在编辑文本，点击空白区域应提交并退出文本编辑
                    bool wasEditingText = false;
                    m_state->isSelectingText = false;
                    if (m_state->activeElement) {
                        if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                            wasEditingText = true;
                        }
                        m_state->activeElement->isActive = false;
                        m_state->activeElement->isEditing = false;
                        if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                            m_state->markup.removeElement(m_state->activeElement->id);
                        }
                        m_state->activeElement = nullptr;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                    }

                    // 若刚退出文本编辑态，仅提交并恢复就绪态，不立即连续落点新建第二个文本框
                    if (wasEditingText && m_state->currentTool == MarkupTool::Text) {
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        updateHoverCursor(point);
                        syncShortcutHintContext();
                        return 0;
                    }
                    if (wasEditingText) {
                        syncShortcutHintContext();
                    }

                    // 若未激活标注工具：长图模式下抓手上下平移画布，普通截图下整体拖拽调整选区位置
                    if (!m_state->isMarkupToolActive) {
                        if (m_state->isLongImageMode()) {
                            m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::PanCanvas;
                            m_state->longCanvas.panLastCursor = point;
                        } else {
                            m_state->isAdjustingSelection = true;
                            m_state->selAdjustHandle = HitArea::Body;
                            m_state->selAdjustLast = point;
                        }
                        updateHoverCursor(point);
                        return 0;
                    }

                    // 在选区内部空白区域立即启动新标注绘制流程
                    beginMarkup(point);
                    return 0;
                }
                // 长图模式下点击舞台内已裁除暗罩或两侧留白区域：取消编辑/选中并开启抓手浏览画布
                if (m_state->isLongImageMode() && inToolbarPanel(m_state->longCanvas.stageRect)) {
                    if (m_state->activeElement) {
                        m_state->activeElement->isActive = false;
                        m_state->activeElement->isEditing = false;
                        if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                            m_state->markup.removeElement(m_state->activeElement->id);
                        }
                        m_state->activeElement = nullptr;
                        m_renderer->markMarkupDirty();
                        syncShortcutHintContext();
                    }
                    m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::PanCanvas;
                    m_state->longCanvas.panLastCursor = point;
                    m_renderer->invalidate();
                    updateHoverCursor(point);
                }
                return 0;
            }

            m_state->dragStart = point;
            m_state->dragEnd = m_state->dragStart;
            m_state->lastMousePos = point;
            m_state->dragging = true;
            m_state->state = OverlayState::Selecting;
            m_state->isMarkupToolActive = false;
            if (m_state->options.showShortcutHints) {
                ShortcutHintOverlay::instance().show(
                    m_state->mode == OverlayMode::RecordRegion
                        ? ShortcutHintContext::RecordSelecting
                        : ShortcutHintContext::CaptureSelecting);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!this) break;

            // 高回报率鼠标 (1000Hz~4000Hz) 队列合并：非自由画笔涂鸦态下直接吸收消息队列中积压的陈旧过期 WM_MOUSEMOVE，
            // 确保每帧 D2D 合成始终直达物理光标最新坐标，彻底消除长时运行或 4K 多屏下的拖尾粘滞。
            if (!(m_state->isMarking && m_state->currentTool == MarkupTool::Pen)) {
                MSG coalesced{};
                int drained = 0;
                while (drained < 32 &&
                       PeekMessageW(&coalesced, hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE)) {
                    wParam = coalesced.wParam;
                    lParam = coalesced.lParam;
                    ++drained;
                }
            }

            m_state->currentCursor = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // Mouse coordinates are client-relative while monitor APIs require
            // virtual-desktop screen coordinates (which may also be negative).
            POINT screenPoint = m_state->currentCursor;
            ClientToScreen(hwnd, &screenPoint);
            HMONITOR hMon = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
            if (hMon != m_lastDpiMonitor) {
                m_lastDpiMonitor = hMon;
                const UINT dpiX = tools3000::core::dpi::effectiveDpiForMonitor(hMon);
                const float newScale = tools3000::core::dpi::scaleForDpi(dpiX);
                if (std::abs(newScale - m_state->dpiScale) >= 0.01f) {
                    m_state->dpiScale = newScale;
                    m_state->toolbarLayoutValid = false;
                    if (!m_renderer->updateDpiScale(newScale)) {
                        LOG_WARN("截图覆盖层 DPI 文本资源更新失败: dpi={}", dpiX);
                    }
                    m_renderer->invalidate();
                }
            }

            if ((int)m_state->state.load() == (int)OverlayState::Selected ||
                (int)m_state->state.load() == (int)OverlayState::Marking) {
                const auto& gr = m_state->toolbarGripRect;
                const bool gripHover = (gr.right > gr.left &&
                                        m_state->currentCursor.x >= gr.left &&
                                        m_state->currentCursor.x <= gr.right &&
                                        m_state->currentCursor.y >= gr.top &&
                                        m_state->currentCursor.y <= gr.bottom);
                if (gripHover != m_state->isToolbarGripHovered) {
                    m_state->isToolbarGripHovered = gripHover;
                    m_renderer->invalidate();
                }

                if (m_state->isLongImageMode()) {
                    const float cpx = static_cast<float>(m_state->currentCursor.x);
                    const float cpy = static_cast<float>(m_state->currentCursor.y);
                    const auto inBtn = [&](const D2D1_RECT_F& rc) {
                        return rc.right > rc.left && cpx >= rc.left && cpx <= rc.right && cpy >= rc.top && cpy <= rc.bottom;
                    };
                    int hovHdr = -1;
                    if (inBtn(m_state->longCanvas.btnJumpTopRect)) hovHdr = 0;
                    else if (inBtn(m_state->longCanvas.btnJumpBottomRect)) hovHdr = 1;
                    else if (inBtn(m_state->longCanvas.btnResetCropRect)) hovHdr = 2;
                    else if (inBtn(m_state->longCanvas.btnZoomToggleRect)) hovHdr = 3;
                    if (hovHdr != m_state->longCanvas.hoveredHeaderBtn) {
                        m_state->longCanvas.hoveredHeaderBtn = hovHdr;
                        m_renderer->invalidate();
                    }
                }
            }

            if (m_state->isLongImageMode() &&
                (m_state->longCanvas.middleButtonPanning ||
                 m_state->longCanvas.dragMode != CaptureState::LongCanvasDragMode::None)) {
                const bool mbHeld = (wParam & MK_MBUTTON) != 0;
                const bool lbHeld = (wParam & MK_LBUTTON) != 0;
                if (m_state->longCanvas.middleButtonPanning && !mbHeld) {
                    m_state->longCanvas.middleButtonPanning = false;
                }
                if (m_state->longCanvas.dragMode != CaptureState::LongCanvasDragMode::None && !lbHeld) {
                    m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::None;
                }

                if (m_state->longCanvas.middleButtonPanning ||
                    m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::PanCanvas) {
                    const float dx = static_cast<float>(m_state->currentCursor.x - m_state->longCanvas.panLastCursor.x);
                    const float dy = static_cast<float>(m_state->currentCursor.y - m_state->longCanvas.panLastCursor.y);
                    m_state->longCanvas.scrollX -= dx;
                    m_state->longCanvas.scrollY -= dy;
                    m_state->clampLongCanvasScroll();
                    m_state->longCanvas.panLastCursor = m_state->currentCursor;
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }

                if (m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapViewport) {
                    const int targetRow = ScrollCaptureEditorOverlay::mapMinimapYToImageRow(
                        static_cast<float>(m_state->currentCursor.y),
                        m_state->longCanvas.minimapImageRect,
                        m_state->longCanvas.docHeight);
                    m_state->scrollLongCanvasToDocRow(static_cast<float>(targetRow), 0.5f);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }

                if (m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapCropTop ||
                    m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapCropBottom) {
                    const bool isTop = (m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapCropTop);
                    const int mappedRow = ScrollCaptureEditorOverlay::mapMinimapYToImageRow(
                        static_cast<float>(m_state->currentCursor.y),
                        m_state->longCanvas.minimapImageRect,
                        m_state->longCanvas.docHeight);
                    ScrollCropRange cand{
                        std::min(m_state->dragStart.y, m_state->dragEnd.y),
                        std::max(m_state->dragStart.y, m_state->dragEnd.y),
                        std::min(m_state->dragStart.x, m_state->dragEnd.x),
                        std::max(m_state->dragStart.x, m_state->dragEnd.x)
                    };
                    if (isTop) cand.topRow = mappedRow;
                    else cand.bottomRow = mappedRow;
                    const auto clamped = ScrollCaptureEditorOverlay::clampScrollCropRange(
                        cand, m_state->longCanvas.docWidth, m_state->longCanvas.docHeight,
                        ScrollCaptureEditorOverlay::kMinCropDimensionPx, isTop ? 1 : 2);
                    m_state->dragStart = {clamped.leftCol, clamped.topRow};
                    m_state->dragEnd = {clamped.rightCol, clamped.bottomRow};
                    m_state->scrollLongCanvasToDocRow(
                        static_cast<float>(isTop ? clamped.topRow : clamped.bottomRow),
                        isTop ? 0.24f : 0.76f);
                    if (m_state->markup.hasAnyMarkup()) {
                        rebuildMarkupBase();
                        m_renderer->markMarkupDirty();
                    } else {
                        m_state->markupBaseReady = false;
                        m_state->markupBaseRect = {0, 0, 0, 0};
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
            }

            if (m_state->isDraggingToolbar) {
                if ((wParam & MK_LBUTTON) == 0) {
                    m_state->isDraggingToolbar = false;
                } else {
                    m_state->hasCustomToolbarPos = true;
                    m_state->customToolbarOrigin = D2D1::Point2F(
                        static_cast<float>(m_state->currentCursor.x) - m_state->toolbarDragOffset.x,
                        static_cast<float>(m_state->currentCursor.y) - m_state->toolbarDragOffset.y);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    m_state->lastMousePos = m_state->currentCursor;
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
            }

            if (m_state->dropdownMenu.type != DropdownType::None) {
                const float mx = static_cast<float>(m_state->currentCursor.x);
                const float my = static_cast<float>(m_state->currentCursor.y);
                const auto cmRect = m_state->dropdownMenu.childMenuRect;
                const bool inChildMenu =
                    cmRect.right > cmRect.left &&
                    mx >= cmRect.left && mx <= cmRect.right &&
                    my >= cmRect.top && my <= cmRect.bottom;
                if (inChildMenu &&
                    m_state->dropdownMenu.activeParentIndex >= 0 &&
                    m_state->dropdownMenu.activeParentIndex < static_cast<int>(m_state->dropdownMenu.items.size())) {
                    int hoverChild = -1;
                    const auto& cItems = m_state->dropdownMenu.items[m_state->dropdownMenu.activeParentIndex].childItems;
                    for (int c = 0; c < static_cast<int>(cItems.size()); ++c) {
                        const auto& ci = cItems[c];
                        if (mx >= ci.rect.left && mx <= ci.rect.right &&
                            my >= ci.rect.top && my <= ci.rect.bottom) {
                            hoverChild = c;
                            break;
                        }
                    }
                    m_state->dropdownMenu.hoveredChildIndex = hoverChild;
                    tools3000::capture::updateDropdownHover(*m_state, m_state->dropdownMenu.activeParentIndex);
                } else {
                    int hoverItem = -1;
                    for (int i = 0; i < static_cast<int>(m_state->dropdownMenu.items.size()); ++i) {
                        const auto& item = m_state->dropdownMenu.items[i];
                        if (item.isSeparator) continue;
                        if (mx >= item.rect.left && mx <= item.rect.right &&
                            my >= item.rect.top && my <= item.rect.bottom) {
                            hoverItem = i;
                            break;
                        }
                    }
                    if (hoverItem >= 0) {
                        m_state->dropdownMenu.hoveredChildIndex = -1;
                        m_state->dropdownMenu.activeParentIndex =
                            m_state->dropdownMenu.items[hoverItem].hasChildMenu ? hoverItem : -1;
                        tools3000::capture::updateDropdownHover(*m_state, hoverItem);
                    } else {
                        // 主菜单与子菜单之间的横向跨越桥接防抖走廊，避免移向子菜单途中闪断收起
                        bool inBridgeGap = false;
                        if (cmRect.right > cmRect.left && m_state->dropdownMenu.activeParentIndex >= 0) {
                            const auto dmRect = m_state->dropdownMenu.menuRect;
                            const float gapLeft = (std::min)(dmRect.left, cmRect.left) - 4.0f;
                            const float gapRight = (std::max)(dmRect.right, cmRect.right) + 4.0f;
                            const float gapTop = (std::min)(dmRect.top, cmRect.top) - 4.0f;
                            const float gapBottom = (std::max)(dmRect.bottom, cmRect.bottom) + 4.0f;
                            if (mx >= gapLeft && mx <= gapRight && my >= gapTop && my <= gapBottom) {
                                inBridgeGap = true;
                            }
                        }
                        if (!inBridgeGap) {
                            m_state->dropdownMenu.hoveredChildIndex = -1;
                            tools3000::capture::updateDropdownHover(*m_state, -1);
                        }
                    }
                }
                m_renderer->invalidate();
            }

            if (m_state->sliderPopup.isDragging) {
                float px = static_cast<float>(m_state->currentCursor.x);
                auto tr = m_state->sliderPopup.trackRect;
                float pct = std::clamp((px - tr.left) / (tr.right - tr.left), 0.0f, 1.0f);
                int val = m_state->sliderPopup.minValue + static_cast<int>(std::round(pct * (m_state->sliderPopup.maxValue - m_state->sliderPopup.minValue)));
                if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
                    if (m_state->beautyShell.enabled) {
                        m_state->beautyShell.cornerRadius = static_cast<float>(val);
                        tools3000::core::ConfigManager::instance().set<int>("/capture/beautyShellRadius", val);
                    } else {
                        m_state->cornerRadius = static_cast<float>(val);
                        tools3000::core::ConfigManager::instance().set<double>("/screenshot/cornerRadius", static_cast<double>(val));
                    }
                    prepareMarkupBase();
                } else if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                    m_state->currentElementCornerRadius = static_cast<float>(val);
                } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                    m_state->currentSpotlightDimAlpha = static_cast<float>(std::clamp(val, 15, 85)) / 100.0f;
                    tools3000::core::ConfigManager::instance().set<int>("/capture/markup/spotlightDim", std::clamp(val, 15, 85));
                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                        m_state->activeElement->fontSize = static_cast<float>(val);
                        ensureActiveTextBoundsAfterEdit(*m_state->activeElement);
                    }
                    const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    tools3000::core::ConfigManager::instance().set<double>("/capture/markup/text/fontSize", static_cast<double>(val) / curDpi);
                } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                    m_state->currentOutlineWidth = std::clamp(val, 0, 12);
                    if (m_state->currentTool == MarkupTool::Text) {
                        m_state->currentTextOutline = (m_state->currentOutlineWidth > 0);
                    }
                } else {
                    m_state->currentStrokeWidth = val;
                }
                if (m_state->activeElement) {
                    if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                        m_state->activeElement->cornerRadius = static_cast<float>(val);
                    } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                        if (m_state->activeElement->tool == MarkupTool::Spotlight) {
                            m_state->activeElement->spotlightDimAlpha = m_state->currentSpotlightDimAlpha;
                        }
                    } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                        m_state->activeElement->fontSize = static_cast<float>(val);
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                    } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                        m_state->activeElement->outlineThickness = static_cast<float>(std::clamp(val, 0, 12));
                        m_state->activeElement->hasCustomOutline = true;
                        m_state->activeElement->textOutline = (val > 0);
                        if (m_state->activeElement->tool == MarkupTool::Text) {
                            m_state->activeElement->textRenderSize = cv::Size(0, 0);
                        }
                    } else if (m_state->sliderPopup.type != SliderPopupType::SelectionCornerRadius) {
                        m_state->activeElement->thickness = static_cast<float>(val);
                        if (m_state->activeElement->tool == MarkupTool::Mosaic) {
                            m_state->activeElement->mosaicBlockSize = (std::max)(4, val * 2);
                        } else if (m_state->activeElement->tool == MarkupTool::Blur) {
                            m_state->activeElement->mosaicBlockSize = (std::max)(5, val * 3);
                        } else if (m_state->activeElement->tool == MarkupTool::Inpaint) {
                            m_state->activeElement->inpaintRadius = std::clamp(val, 2, 16);
                        } else if (m_state->activeElement->tool == MarkupTool::Watermark) {
                            const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                            m_state->activeElement->fontSize = std::clamp(12.0f + static_cast<float>(val) * 1.5f, 12.0f, 64.0f) * curDpi;
                        }
                    }
                    m_renderer->markMarkupDirty();
                }
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                return 0;
            }

            if (m_state->isSelectingText) {
                if ((wParam & MK_LBUTTON) == 0 || !m_state->activeElement ||
                    m_state->activeElement->tool != MarkupTool::Text || !m_state->activeElement->isEditing) {
                    m_state->isSelectingText = false;
                } else {
                    const cv::Point localPt = toMarkupPoint(m_state->currentCursor);
                    const float relX = static_cast<float>(localPt.x - m_state->activeElement->startPt.x);
                    const float relY = static_cast<float>(localPt.y - m_state->activeElement->startPt.y);
                    const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    const float maxContentW = syncTextElementWrapWidth(*m_state->activeElement);
                    const auto layout = CaptureTextEditor::buildLayout(
                        wstr, static_cast<int>(m_state->activeElement->fontSize), maxContentW);
                    const size_t newCaret = CaptureTextEditor::hitTestCaretIndex(layout, wstr, relX, relY);
                    if (newCaret != m_state->activeElement->textCaret) {
                        m_state->activeElement->textCaret = newCaret;
                        m_state->activeElement->textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                    }
                    m_state->lastMousePos = m_state->currentCursor;
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
            }

            if (m_state->isAdjustingCornerRadius) {
                // 以选区中心为基准：往内拉增加圆角，往外推减小圆角（兼容长图缩放矩阵）
                const float zoom = m_state->isLongImageMode() ? (std::max)(0.05f, m_state->longCanvas.zoom) : 1.0f;
                auto r = m_state->docRectToScreen(currentSelectionRect());
                float cfgMaxRadius = m_state->beautyShell.enabled ? 32.0f : static_cast<float>(tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                float newScreenR = CornerRadiusHelper::calculateDraggedRadius(
                    r.left, r.top, r.right, r.bottom,
                    static_cast<float>(m_state->cornerDragStartPos.x),
                    static_cast<float>(m_state->cornerDragStartPos.y),
                    m_state->cornerDragStartRadius * zoom,
                    static_cast<float>(m_state->currentCursor.x),
                    static_cast<float>(m_state->currentCursor.y),
                    cfgMaxRadius * zoom,
                    m_state->cornerDragIndex,
                    currentDpiScale);
                float newR = std::clamp(newScreenR / zoom, 0.0f, cfgMaxRadius);

                const bool isZh = tools3000::core::isUiLanguageChinese();
                if (m_state->beautyShell.enabled) {
                    m_state->beautyShell.cornerRadius = std::round(newR);
                    m_state->loupeToastMessage = CornerRadiusHelper::getToastMessage(
                        CornerRadiusTarget::BeautifyShell, m_state->beautyShell.cornerRadius, isZh);
                } else {
                    m_state->cornerRadius = std::round(newR);
                    m_state->loupeToastMessage = CornerRadiusHelper::getToastMessage(
                        CornerRadiusTarget::Selection, m_state->cornerRadius, isZh);
                }
                m_state->loupeToastUntil = GetTickCount() + 1200;
                m_renderer->invalidate();
                return 0;
            } else if (m_state->isAdjustingSelection) {
                if (m_state->isLongImageMode()) {
                    const auto curDoc = m_state->screenToDoc(
                        static_cast<float>(m_state->currentCursor.x),
                        static_cast<float>(m_state->currentCursor.y));
                    const int dx = static_cast<int>(std::trunc(curDoc.x - m_state->longCanvas.lastSelAdjustDocPt.x));
                    const int dy = static_cast<int>(std::trunc(curDoc.y - m_state->longCanvas.lastSelAdjustDocPt.y));
                    if (dx != 0 || dy != 0) {
                        adjustSelection(m_state->selAdjustHandle, dx, dy);
                        m_state->longCanvas.lastSelAdjustDocPt.x += static_cast<float>(dx);
                        m_state->longCanvas.lastSelAdjustDocPt.y += static_cast<float>(dy);
                    }
                    // 当推拉顶/底裁切横梁靠近舞台视口上下边缘时，启动 60fps 平滑自动滚屏推进
                    const float curScale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                    const float edgeZone = 44.0f * curScale;
                    const auto& stg = m_state->longCanvas.stageRect;
                    if (m_state->currentCursor.y < stg.top + edgeZone ||
                        m_state->currentCursor.y > stg.bottom - edgeZone) {
                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    }
                } else {
                    int dx = m_state->currentCursor.x - m_state->selAdjustLast.x;
                    int dy = m_state->currentCursor.y - m_state->selAdjustLast.y;
                    adjustSelection(m_state->selAdjustHandle, dx, dy);
                }
                m_state->selAdjustLast = m_state->currentCursor;
                m_renderer->invalidate();
            } else if (m_state->isManipulating && m_state->activeElement) {
                if (m_state->dragHandle == HitArea::CornerRadius && m_state->activeElement->tool == MarkupTool::Rectangle) {
                    const float zoom = m_state->isLongImageMode() ? (std::max)(0.05f, m_state->longCanvas.zoom) : 1.0f;
                    auto r = currentSelectionRect();
                    int x1 = (std::min)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                    int y1 = (std::min)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                    int x2 = (std::max)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                    int y2 = (std::max)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                    const auto scrBox = m_state->docRectToScreen(D2D1::RectF(
                        r.left + static_cast<float>(x1),
                        r.top + static_cast<float>(y1),
                        r.left + static_cast<float>(x2),
                        r.top + static_cast<float>(y2)));
                    const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    float newScreenR = CornerRadiusHelper::calculateDraggedRadius(
                        scrBox.left, scrBox.top, scrBox.right, scrBox.bottom,
                        static_cast<float>(m_state->cornerDragStartPos.x),
                        static_cast<float>(m_state->cornerDragStartPos.y),
                        m_state->cornerDragStartRadius * zoom,
                        static_cast<float>(m_state->currentCursor.x),
                        static_cast<float>(m_state->currentCursor.y),
                        0.0f,
                        m_state->cornerDragIndex,
                        currentDpiScale);
                    float newR = (std::max)(0.0f, newScreenR / zoom);
                    m_state->activeElement->cornerRadius = std::round(newR);
                    m_state->currentElementCornerRadius = m_state->activeElement->cornerRadius;
                    m_state->toolbarLayoutValid = false;
                    const bool isZh = tools3000::core::isUiLanguageChinese();
                    m_state->loupeToastMessage = CornerRadiusHelper::getToastMessage(
                        CornerRadiusTarget::RectangleMarkup, m_state->activeElement->cornerRadius, isZh);
                    m_state->loupeToastUntil = GetTickCount() + 1200;
                    m_state->lastMousePos = m_state->currentCursor;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                } else {
                    int dx = m_state->currentCursor.x - m_state->lastMousePos.x;
                    int dy = m_state->currentCursor.y - m_state->lastMousePos.y;
                    if (m_state->isLongImageMode()) {
                        const auto curDoc = m_state->screenToDoc(
                            static_cast<float>(m_state->currentCursor.x),
                            static_cast<float>(m_state->currentCursor.y));
                        dx = static_cast<int>(std::trunc(curDoc.x - m_state->longCanvas.lastManipulateDocPt.x));
                        dy = static_cast<int>(std::trunc(curDoc.y - m_state->longCanvas.lastManipulateDocPt.y));
                        m_state->longCanvas.lastManipulateDocPt.x += static_cast<float>(dx);
                        m_state->longCanvas.lastManipulateDocPt.y += static_cast<float>(dy);
                    }
                    LOG_TRACE("CaptureInput: manipulating element dragHandle={}, dx={}, dy={}",
                              static_cast<int>(m_state->dragHandle), dx, dy);
                    if (m_state->dragHandle == HitArea::None) {
                        m_state->activeElement->moveBy(dx, dy);
                        if (m_state->activeElement->tool == MarkupTool::Text) {
                            syncTextElementWrapWidth(*m_state->activeElement);
                        }
                    } else {
                        m_state->activeElement->resize(dx, dy, m_state->dragHandle);
                        if (m_state->activeElement->tool == MarkupTool::Text) {
                            syncTextElementWrapWidth(*m_state->activeElement);
                        }
                        if (m_state->activeElement->tool == MarkupTool::Number && m_state->dragHandle == HitArea::RT) {
                            m_state->currentStrokeWidth = static_cast<int>(std::round(m_state->activeElement->thickness));
                            m_state->toolbarLayoutValid = false;
                        } else if (m_state->activeElement->tool == MarkupTool::Magnifier &&
                                   (m_state->dragHandle == HitArea::LensOffset || m_state->dragHandle == HitArea::FocusSource)) {
                            const auto sel = currentSelectionRect();
                            const int selW = std::max(1, static_cast<int>(std::round(sel.right - sel.left)));
                            const int selH = std::max(1, static_cast<int>(std::round(sel.bottom - sel.top)));
                            if (m_state->dragHandle == HitArea::LensOffset) {
                                const int rx = m_state->activeElement->effectiveMagnifierRx();
                                const int ry = m_state->activeElement->effectiveMagnifierRy();
                                const int loX = std::min(rx, selW / 2);
                                const int hiX = std::max(loX, selW - rx);
                                const int loY = std::min(ry, selH / 2);
                                const int hiY = std::max(loY, selH - ry);
                                m_state->activeElement->endPt.x = std::clamp(m_state->activeElement->endPt.x, loX, hiX);
                                m_state->activeElement->endPt.y = std::clamp(m_state->activeElement->endPt.y, loY, hiY);
                            } else {
                                m_state->activeElement->startPt.x = std::clamp(m_state->activeElement->startPt.x, 1, std::max(1, selW - 1));
                                m_state->activeElement->startPt.y = std::clamp(m_state->activeElement->startPt.y, 1, std::max(1, selH - 1));
                            }
                            m_state->toolbarLayoutValid = false;
                        }
                    }
                    m_state->lastMousePos = m_state->currentCursor;
                    m_renderer->markMarkupDirty();   // 元素几何变了，合成缓存失效
                    m_renderer->invalidate();        // 立即触发 Direct2D 实时重绘
                }
            } else if (m_state->isMarking) {
                updateMarkup(m_state->currentCursor);
                m_renderer->invalidate();        // 拖拽预览走 D2D，合成图不变
            } else if (m_state->dragging) {
                // 拖拽框选过程中立即隐藏快捷键提示层，把全屏视野还给用户
                ShortcutHintOverlay::instance().hide();
                const bool spaceDown = (GetKeyState(VK_SPACE) & 0x8000) != 0;
                if (spaceDown) {
                    // Space 键按住时：平移当前选区（Snipaste 核心交互机制）
                    int dx = m_state->currentCursor.x - m_state->lastMousePos.x;
                    int dy = m_state->currentCursor.y - m_state->lastMousePos.y;
                    m_state->dragStart.x += dx;
                    m_state->dragStart.y += dy;
                    m_state->dragEnd.x += dx;
                    m_state->dragEnd.y += dy;
                } else {
                    m_state->dragEnd = m_state->currentCursor;
                    float targetRatio = getTargetAspectRatio(m_state->aspectRatio);
                    if (targetRatio > 0.0f) {
                        float dx = static_cast<float>(m_state->dragEnd.x - m_state->dragStart.x);
                        float dy = static_cast<float>(m_state->dragEnd.y - m_state->dragStart.y);
                        float signX = (dx >= 0.0f) ? 1.0f : -1.0f;
                        float signY = (dy >= 0.0f) ? 1.0f : -1.0f;
                        float curW = std::abs(dx);
                        float curH = std::abs(dy);
                        if (curW / targetRatio >= curH) {
                            float newH = curW / targetRatio;
                            m_state->dragEnd.y = static_cast<LONG>(m_state->dragStart.y + signY * newH);
                        } else {
                            float newW = curH * targetRatio;
                            m_state->dragEnd.x = static_cast<LONG>(m_state->dragStart.x + signX * newW);
                        }
                    }
                }
                m_state->lastMousePos = m_state->currentCursor;
                m_renderer->invalidate();
            } else {
                if ((int)m_state->state.load() == (int)OverlayState::Selecting && !m_state->dragging) {
                    if (m_state->options.autoDetectWindow) {
                        POINT screenPt = m_state->currentCursor;
                        int offX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                        int offY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                        screenPt.x += offX;
                        screenPt.y += offY;

                        // 光标防抖锁定：
                        // 1. 距离上次有效探测点微移 < 3px 时跳过重复探测，彻底消除 1~2px 手抖触发边缘高频跳变；
                        // 2. 已处于滚轮/Tab 手动锁定的高层级且仍在选区包围盒内（含 6px 容差），保留当前层级。
                        bool preserveHierarchy = false;
                        const bool withinMicroJitter =
                            !m_state->detectedWindowHierarchy.empty() &&
                            m_lastDetectScreenPt.x != INT_MIN &&
                            (std::abs(screenPt.x - m_lastDetectScreenPt.x) +
                             std::abs(screenPt.y - m_lastDetectScreenPt.y)) < 3;
                        if (withinMicroJitter ||
                            (m_state->currentCursor.x == m_state->lastMousePos.x &&
                             m_state->currentCursor.y == m_state->lastMousePos.y &&
                             !m_state->detectedWindowHierarchy.empty())) {
                            preserveHierarchy = true;
                        } else if (m_state->detectedWindowHierarchyIndex > 0 &&
                            m_state->detectedWindow.right > m_state->detectedWindow.left &&
                            m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                            RECT guardRect = m_state->detectedWindow;
                            guardRect.left -= 6;
                            guardRect.top -= 6;
                            guardRect.right += 6;
                            guardRect.bottom += 6;
                            POINT clientPt = m_state->currentCursor;
                            if (PtInRect(&guardRect, clientPt)) {
                                preserveHierarchy = true;
                            }
                        }

                        if (!preserveHierarchy) {
                            updateWindowDetectionAtCursor(hwnd, screenPt, /*allowAsyncRefine=*/true);
                        }
                    } else {
                        m_state->detectedWindow = {};
                        m_state->detectedWindowCornerRadius = 0.0f;
                        m_state->animDetectedWindowRect = {};
                        m_state->targetDetectedWindowRect = {};
                        m_state->isHierarchyAnimating = false;
                        m_state->detectedWindowHierarchy.clear();
                        m_state->detectedWindowHierarchyDescriptions.clear();
                        m_state->detectedWindowHierarchyRadii.clear();
                    }
                }

                // 探测二维码悬停
                if (!m_state->detectedQrText.empty() && m_state->qrChipRect.right > m_state->qrChipRect.left) {
                    bool hover = (m_state->currentCursor.x >= m_state->qrChipRect.left &&
                                  m_state->currentCursor.x <= m_state->qrChipRect.right &&
                                  m_state->currentCursor.y >= m_state->qrChipRect.top &&
                                  m_state->currentCursor.y <= m_state->qrChipRect.bottom);
                    if (hover != m_state->isQrChipHovered) {
                        m_state->isQrChipHovered = hover;
                        m_renderer->invalidate();
                    }
                }
                m_renderer->invalidate();        // 十字准星/动态放大镜跟随光标
            }

            m_state->lastMousePos = m_state->currentCursor;
            updateHoverCursor(m_state->currentCursor);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (!this) break;

            if (m_state->isLongImageMode() &&
                m_state->longCanvas.dragMode != CaptureState::LongCanvasDragMode::None) {
                const auto prevDrag = m_state->longCanvas.dragMode;
                m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::None;
                if (prevDrag == CaptureState::LongCanvasDragMode::MinimapCropTop ||
                    prevDrag == CaptureState::LongCanvasDragMode::MinimapCropBottom) {
                    prepareMarkupBase();
                }
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isDraggingToolbar) {
                m_state->isDraggingToolbar = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->sliderPopup.isDragging) {
                m_state->sliderPopup.isDragging = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isAdjustingCornerRadius) {
                m_state->isAdjustingCornerRadius = false;
                if (m_state->beautyShell.enabled) {
                    tools3000::core::ConfigManager::instance().set<int>(
                        "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                } else {
                    tools3000::core::ConfigManager::instance().set<double>(
                        "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                }
                prepareMarkupBase();
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isAdjustingSelection) {
                m_state->isAdjustingSelection = false;
                m_state->selAdjustHandle = HitArea::None;
                prepareMarkupBase();
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isSelectingText) {
                m_state->isSelectingText = false;
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isManipulating) {
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                    const float dpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    float unscaled = m_state->activeElement->fontSize / dpiScale;
                    tools3000::core::ConfigManager::instance().set<double>(
                        "/capture/markup/text/fontSize", static_cast<double>(unscaled));
                    const POINT releasePt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    const double moveDist = std::hypot(
                        static_cast<double>(releasePt.x - m_state->textManipulateStart.x),
                        static_cast<double>(releasePt.y - m_state->textManipulateStart.y));
                    if (!m_state->activeElement->isEditing && m_state->dragHandle == HitArea::None && moveDist <= 4.0) {
                        // 原地点击非编辑态文本框：进入编辑态并将光标精准定位到点击字缝
                        m_state->activeElement->isEditing = true;
                        m_state->textEditEnteredTick = GetTickCount();
                        const cv::Point local = toMarkupPoint(releasePt);
                        const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                        const float maxContentW = syncTextElementWrapWidth(*m_state->activeElement);
                        const auto layout = CaptureTextEditor::buildLayout(
                            wstr, static_cast<int>(m_state->activeElement->fontSize), maxContentW);
                        const float relX = static_cast<float>(local.x - m_state->activeElement->startPt.x);
                        const float relY = static_cast<float>(local.y - m_state->activeElement->startPt.y);
                        const size_t hitIdx = CaptureTextEditor::hitTestCaretIndex(layout, wstr, relX, relY);
                        m_state->activeElement->textCaret = hitIdx;
                        m_state->activeElement->textSelAnchor = hitIdx;
                        m_state->activeElement->textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                        syncImeCompositionWindow(hwnd);
                        m_renderer->markMarkupDirty();
                    }
                }
                if (m_state->hasManipulateSnapshot) {
                    m_state->hasManipulateSnapshot = false;
                    if (!MarkupEngine::elementsEquivalent(
                            m_state->manipulateStartSnapshot.elements,
                            m_state->markup.exportElements())) {
                        m_state->markup.commitUndoSnapshot(std::move(m_state->manipulateStartSnapshot));
                        m_renderer->markMarkupDirty();
                    }
                }
                m_state->isManipulating = false;
                m_state->dragHandle = HitArea::None;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isMarking) {
                if (m_state->isDrawingMultiPointLine) {
                    // 多点连线已在 WM_LBUTTONDOWN 记录锚点，抬起左键保持橡皮筋继续延伸至下一点
                    m_renderer->invalidate();
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
                finishMarkup({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (!m_state->dragging) break;
            m_state->dragEnd = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            m_state->dragging = false;

            int w = std::abs(m_state->dragEnd.x - m_state->dragStart.x);
            int h = std::abs(m_state->dragEnd.y - m_state->dragStart.y);

            auto showSelectedShortcutHints = [&]() {
                if (!m_state->options.showShortcutHints || m_state->state.load() == OverlayState::Idle) return;
                const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                RECT sel = {
                    std::min(m_state->dragStart.x, m_state->dragEnd.x) + vx,
                    std::min(m_state->dragStart.y, m_state->dragEnd.y) + vy,
                    std::max(m_state->dragStart.x, m_state->dragEnd.x) + vx,
                    std::max(m_state->dragStart.y, m_state->dragEnd.y) + vy
                };
                MONITORINFO mi{sizeof(mi)};
                const HMONITOR hMon = MonitorFromRect(&sel, MONITOR_DEFAULTTONEAREST);
                RECT workArea{
                    vx, vy,
                    vx + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                    vy + GetSystemMetrics(SM_CYVIRTUALSCREEN)
                };
                if (hMon && GetMonitorInfoW(hMon, &mi)) {
                    workArea = mi.rcWork;
                }
                const LONG monW = std::max<LONG>(1, workArea.right - workArea.left);
                const LONG monH = std::max<LONG>(1, workArea.bottom - workArea.top);
                const bool isLargeWindowOrScreenSelection =
                    (sel.right - sel.left) >= (monW * 55) / 100 ||
                    (sel.bottom - sel.top) >= (monH * 60) / 100;
                std::vector<RECT> initialAvoid;
                if (!isLargeWindowOrScreenSelection) {
                    initialAvoid.push_back(sel);
                }
                ShortcutHintOverlay::instance().show(
                    m_state->mode == OverlayMode::RecordRegion
                        ? ShortcutHintContext::RecordSelecting
                        : ShortcutHintContext::CaptureSelected,
                    {LONG_MIN, LONG_MIN},
                    initialAvoid);
            };

            if (w > 3 && h > 3) {
                m_state->state = OverlayState::Selected;
                m_state->isMarkupToolActive = false;
                prepareMarkupBase();
                showSelectedShortcutHints();
            } else {
                // 拖拽太小，视为点击——吸附到检测的窗口
                if (m_state->detectedWindow.right > m_state->detectedWindow.left &&
                    m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                    m_state->isHierarchyAnimating = false;
                    m_state->animDetectedWindowRect = {};
                    m_state->targetDetectedWindowRect = {};
                    KillTimer(hwnd, RENDER_TIMER_ID);
                    m_state->dragStart = {static_cast<LONG>(m_state->detectedWindow.left),
                                         static_cast<LONG>(m_state->detectedWindow.top)};
                    m_state->dragEnd = {static_cast<LONG>(m_state->detectedWindow.right),
                                       static_cast<LONG>(m_state->detectedWindow.bottom)};
                    m_state->cornerRadius = m_state->detectedWindowCornerRadius; // 自动继承现代 Win11 窗口圆角
                    m_state->state = OverlayState::Selected;
                    m_state->isMarkupToolActive = false;
                    prepareMarkupBase();
                    showSelectedShortcutHints();
                } else {
                    if (m_cancelCb) m_cancelCb();
                }
            }
            updateHoverCursor(m_state->currentCursor);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (this && ((int)m_state->state.load() == (int)OverlayState::Selected ||
                         (int)m_state->state.load() == (int)OverlayState::Marking)) {
                m_renderer->markMarkupDirty();
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (m_state->isLongImageMode()) {
                    const auto inPanel = [&](const D2D1_RECT_F& rc) {
                        return rc.right > rc.left && pt.x >= rc.left && pt.x <= rc.right &&
                               pt.y >= rc.top && pt.y <= rc.bottom;
                    };
                    if (inPanel(m_state->longCanvas.topHeaderRect) ||
                        inPanel(m_state->longCanvas.minimapPanelRect)) {
                        return handleMessage(hwnd, WM_LBUTTONDOWN, wParam, lParam);
                    }
                }
                rebuildToolbarButtons(currentSelectionRect());
                if (m_state->toolbarGripRect.right > m_state->toolbarGripRect.left &&
                    pt.x >= m_state->toolbarGripRect.left && pt.x <= m_state->toolbarGripRect.right &&
                    pt.y >= m_state->toolbarGripRect.top && pt.y <= m_state->toolbarGripRect.bottom) {
                    m_state->hasCustomToolbarPos = false;
                    m_state->isDraggingToolbar = false;
                    m_state->toolbarLayoutValid = false;
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    m_state->loupeToastUntil = GetTickCount() + 1400;
                    m_state->loupeToastMessage = tools3000::core::trWCstrFor(zh, L"已恢复工具栏自动跟随", L"Toolbar auto-follow restored");
                    m_renderer->invalidate();
                    updateHoverCursor(pt);
                    return 0;
                }
                auto* hitBtn = hitTestToolbar(pt);
                if (m_state->isDrawingMultiPointLine && hitBtn == nullptr &&
                    !isPointInsideRectF(pt, m_state->primaryToolbarRect) &&
                    !isPointInsideRectF(pt, m_state->secondaryToolbarRect)) {
                    finalizeMultiPointLine(m_state->multiPointDocVertices.size() >= 2);
                    updateHoverCursor(pt);
                    return 0;
                }
                const bool inSel = isPointInSelection(pt);
                const HitArea selBoxHit = hitTestSelectionBox(pt);
                const cv::Point local = inSel ? toMarkupPoint(pt) : cv::Point{0, 0};

                const bool activeElemHit = inSel && m_state->activeElement && m_state->activeElement->isActive &&
                                           (m_state->activeElement->hitTestEx(local) != HitArea::None);
                HitResult hit = (inSel && !activeElemHit) ? m_state->markup.getElementAtEx(local) : HitResult{};

                const DoubleClickIntent intent = resolveDoubleClickIntent(
                    *m_state, pt, inSel, selBoxHit, activeElemHit || (hit.element != nullptr), hitBtn);

                switch (intent) {
                    case DoubleClickIntent::ForwardPointerDown:
                        return handleMessage(hwnd, WM_LBUTTONDOWN, wParam, lParam);

                    case DoubleClickIntent::RepeatToolbarAction:
                        if (hitBtn) executeToolbarCommand(*hitBtn);
                        return 0;

                    case DoubleClickIntent::ConsumeNoOp:
                        return 0;

                    case DoubleClickIntent::ToggleSelectionCornerRadius:
                        if (m_state->beautyShell.enabled) {
                            m_state->beautyShell.cornerRadius = (m_state->beautyShell.cornerRadius > 0.0f) ? 0.0f : 16.0f;
                            tools3000::core::ConfigManager::instance().set<int>(
                                "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                        } else {
                            m_state->cornerRadius = (m_state->cornerRadius > 0.0f) ? 0.0f : 12.0f;
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                        }
                        prepareMarkupBase();
                        m_renderer->invalidate();
                        return 0;

                    case DoubleClickIntent::EditOrSelectMarkupElement: {
                        auto enterTextWordSelection = [&](MarkupElement& elem) {
                            const bool wasAlreadyEditing = elem.isEditing;
                            const DWORD nowTick = GetTickCount();
                            const bool justEnteredFromFirstClick =
                                wasAlreadyEditing &&
                                m_state->textEditEnteredTick != 0 &&
                                (nowTick - m_state->textEditEnteredTick) <= GetDoubleClickTime();
                            elem.isEditing = true;
                            m_state->textEditEnteredTick = 0;
                            m_state->isSelectingText = false;
                            m_state->isManipulating = false;
                            const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(elem.text);
                            const float maxContentW = syncTextElementWrapWidth(elem);
                            const auto layout = CaptureTextEditor::buildLayout(
                                wstr, static_cast<int>(elem.fontSize), maxContentW);
                            const float relX = static_cast<float>(local.x - elem.startPt.x);
                            const float relY = static_cast<float>(local.y - elem.startPt.y);
                            const size_t hitIdx = CaptureTextEditor::hitTestCaretIndex(layout, wstr, relX, relY);
                            if (!wasAlreadyEditing || justEnteredFromFirstClick) {
                                // 首次双击非编辑态文本框进入编辑：按鼠标点击落点精准定位插入光标，不整词选中、不跳末尾
                                elem.textCaret = hitIdx;
                                elem.textSelAnchor = hitIdx;
                            } else {
                                const auto [curS, curE] = CaptureTextEditor::normalizedRange(wstr, elem.textCaret, elem.textSelAnchor);
                                if (curE > curS && !(curS == 0 && curE == wstr.size())) {
                                    // 已选中单个词块时再次双击：平滑升级为全选当前文本框全文
                                    CaptureTextEditor::selectAll(wstr, elem.textCaret, elem.textSelAnchor);
                                } else {
                                    CaptureTextEditor::selectWordAt(wstr, hitIdx, elem.textCaret, elem.textSelAnchor);
                                }
                            }
                            elem.textLastCaretTick = nowTick;
                            m_state->textPreferredCaretX = -1.0f;
                            SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                            syncShortcutHintContext();
                        };

                        if (activeElemHit) {
                            if (m_state->activeElement->tool == MarkupTool::Text) {
                                enterTextWordSelection(*m_state->activeElement);
                                m_state->currentTool = MarkupTool::Text;
                                m_state->currentColor = m_state->activeElement->color;
                                m_state->currentTextOutline = m_state->activeElement->textOutline;
                                m_state->currentTextOutlineColor = m_state->activeElement->textOutlineColor;
                                m_state->toolbarLayoutValid = false;
                                m_renderer->markMarkupDirty();
                                m_renderer->invalidate();
                            }
                            return 0;
                        }
                        if (hit.element) {
                            if (m_state->activeElement && m_state->activeElement != hit.element) {
                                m_state->activeElement->isActive = false;
                                m_state->activeElement->isEditing = false;
                            }
                            m_state->activeElement = hit.element;
                            m_state->activeElement->isActive = true;
                            m_state->isMarkupToolActive = true;
                            m_state->currentTool = hit.element->tool;
                            m_state->currentStrokeWidth = static_cast<int>(std::round(hit.element->thickness));
                            m_state->currentLineStyle = hit.element->lineStyle;
                            m_state->currentArrowStyle = hit.element->arrowStyle;
                            m_state->currentSmoothCurve = hit.element->smoothCurve;
                            if (hit.element->tool == MarkupTool::Line) {
                                m_state->currentLineArrowEnabled = hit.element->hasCustomCaps;
                            }
                            if (hit.element->hasCustomCaps) {
                                m_state->currentStartCap = hit.element->startCap;
                                m_state->currentEndCap = hit.element->endCap;
                            } else if (hit.element->tool == MarkupTool::Arrow) {
                                switch (hit.element->arrowStyle) {
                                    case ArrowStyle::Thin:
                                        m_state->currentStartCap = LineCapStyle::None;
                                        m_state->currentEndCap = LineCapStyle::OpenArrow;
                                        break;
                                    case ArrowStyle::Tech:
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
                            }
                            m_state->currentElementCornerRadius = hit.element->cornerRadius;
                            if (hit.element->tool == MarkupTool::Magnifier) {
                                m_state->currentMagnifierColor = hit.element->color;
                                m_state->magnifierShowBadge = hit.element->fill;
                                m_state->currentMagnifierShape = hit.element->numberShape;
                                m_state->dynamicMagnifierScale = hit.element->magnifierScale;
                                const bool unsetEnd = !hit.element->hasLeaderArrow &&
                                                      hit.element->endPt.x == 0 && hit.element->endPt.y == 0 &&
                                                      (hit.element->startPt.x != 0 || hit.element->startPt.y != 0);
                                const cv::Point lc = unsetEnd ? hit.element->startPt : hit.element->endPt;
                                m_state->magnifierCalloutMode =
                                    std::hypot(static_cast<double>(lc.x - hit.element->startPt.x),
                                               static_cast<double>(lc.y - hit.element->startPt.y)) > 8.0;
                            } else if (hit.element->tool == MarkupTool::Spotlight) {
                                m_state->currentColor = hit.element->color;
                                m_state->currentSpotlightEllipse = hit.element->fill;
                                m_state->currentSpotlightDimAlpha = hit.element->spotlightDimAlpha;
                                m_state->currentSpotlightBorder = hit.element->spotlightShowBorder;
                            } else {
                                m_state->currentColor = hit.element->color;
                                m_state->currentFillMode = hit.element->fill;
                                m_state->currentNumberShape = hit.element->numberShape;
                                if (hit.element->tool == MarkupTool::Number) {
                                    m_state->currentNumberFormat = hit.element->numberFormat;
                                }
                            }
                            if (hit.element->tool == MarkupTool::Blur) {
                                m_state->currentMosaicType = 1;
                            } else if (hit.element->tool == MarkupTool::Mosaic) {
                                m_state->currentMosaicType = 0;
                            }
                            if (hit.element->tool == MarkupTool::Text) {
                                m_state->currentTextOutline = hit.element->textOutline;
                                m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                                enterTextWordSelection(*m_state->activeElement);
                            } else {
                                if (hit.element->hasCustomOutline) {
                                    m_state->currentOutlineWidth = static_cast<int>(std::round(hit.element->outlineThickness));
                                    m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                                }
                                m_state->activeElement->isEditing = false;
                            }
                            m_state->toolbarLayoutValid = false;
                            m_renderer->markMarkupDirty();
                            m_renderer->invalidate();
                        }
                        return 0;
                    }

                    case DoubleClickIntent::BeginAnnotationAtPoint:
                        beginMarkup(pt);
                        m_renderer->invalidate();
                        updateHoverCursor(pt);
                        return 0;

                    case DoubleClickIntent::ConfirmSelectionAndCopy:
                        if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Default});
                        return 0;
                }
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            if (!this) break;
            CaptureShortcutPolicy::cancelShiftTapCandidate(m_state->shiftTapCandidate);

            // 0. 已选区截图态下右键点击主工具栏：
            //    - 第 1 个形状组按钮：弹出「几何图形 / 聚光灯」模式切换菜单
            //    - 「快速保存」按钮：弹出系统另存为对话框 (SaveAs)
            if (m_state->mode == OverlayMode::Screenshot &&
                ((int)m_state->state.load() == (int)OverlayState::Selected ||
                 (int)m_state->state.load() == (int)OverlayState::Marking)) {
                POINT rpt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                rebuildToolbarButtons(currentSelectionRect());
                const bool blockedByDropdown =
                    (m_state->dropdownMenu.type != DropdownType::None &&
                     (isPointInsideRectF(rpt, m_state->dropdownMenu.menuRect) ||
                      isPointInsideRectF(rpt, m_state->dropdownMenu.childMenuRect))) ||
                    (m_state->sliderPopup.type != SliderPopupType::None &&
                     isPointInsideRectF(rpt, m_state->sliderPopup.popupRect)) ||
                    (m_state->openSubmenu != SubmenuType::None &&
                     isPointInsideRectF(rpt, m_state->openSubmenuRect));
                if (!blockedByDropdown && !m_state->toolbarButtons.empty()) {
                    const auto& shapeBtn = m_state->toolbarButtons[0];
                    if (rpt.x >= shapeBtn.rect.left && rpt.x <= shapeBtn.rect.right &&
                        rpt.y >= shapeBtn.rect.top && rpt.y <= shapeBtn.rect.bottom) {
                        openDropdownMenu(DropdownType::ShapeMode, shapeBtn.rect);
                        m_renderer->invalidate();
                        return 0;
                    }
                    for (const auto& btn : m_state->toolbarButtons) {
                        if (rpt.x >= btn.rect.left && rpt.x <= btn.rect.right &&
                            rpt.y >= btn.rect.top && rpt.y <= btn.rect.bottom) {
                            const auto rightAction = resolveToolbarCompletionAction(btn.command, /*isRightClick=*/true);
                            if (rightAction && *rightAction == CaptureCompletionAction::SaveAs) {
                                triggerSaveSelectionAs(hwnd);
                                return 0;
                            }
                        }
                    }
                }
            } else if (m_state->mode == OverlayMode::RecordRegion &&
                       m_state->recordSetup.format != RecordFormat::GIF &&
                       ((int)m_state->state.load() == (int)OverlayState::Selected ||
                        (int)m_state->state.load() == (int)OverlayState::Marking)) {
                // 录屏选区态下右键点击麦克风分体胶囊：直接原位弹出「麦克风输入源」设备选择菜单
                POINT rpt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                rebuildToolbarButtons(currentSelectionRect());
                D2D1_RECT_F micUnion{};
                bool hasMicUnion = false;
                for (const auto& btn : m_state->toolbarButtons) {
                    if (btn.command == ToolbarCommand::RecordToggleMicrophone ||
                        btn.command == ToolbarCommand::RecordSelectMicSource) {
                        if (!hasMicUnion) {
                            micUnion = btn.rect;
                            hasMicUnion = true;
                        } else {
                            micUnion.left = (std::min)(micUnion.left, btn.rect.left);
                            micUnion.right = (std::max)(micUnion.right, btn.rect.right);
                        }
                    }
                }
                if (hasMicUnion &&
                    rpt.x >= micUnion.left && rpt.x <= micUnion.right &&
                    rpt.y >= micUnion.top && rpt.y <= micUnion.bottom) {
                    openDropdownMenu(DropdownType::RecordMicSource, micUnion);
                    m_renderer->invalidate();
                    return 0;
                }
            }
            
            // 0.5 若有任何悬浮弹出菜单处于展开态（下拉菜单 / 滑块 / 子菜单 / 尺寸菜单），右键优先收起菜单，绝不误退标注或清空选区
            if (hasAnyCapturePopupOpen(*m_state)) {
                closeAllCapturePopups(*m_state, true);
                m_renderer->invalidate();
                return 0;
            }

            // 0.8 多点连线创作态下：右键一键完成已点击的多点折线/曲线（若仅 1 个点则干净取消草稿）
            if (m_state->isDrawingMultiPointLine) {
                finalizeMultiPointLine(m_state->multiPointDocVertices.size() >= 2);
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            // 1. 正在拖拽选区中：右键取消本次拖拽
            if (m_state->dragging) {
                m_state->dragging = false;
                m_state->state = OverlayState::Selecting;
                m_renderer->invalidate();
                syncShortcutHintContext();
                return 0;
            }

            // 2. 正在编辑文字：右键退出文字编辑
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                m_state->activeElement->isEditing = false;
                m_state->isSelectingText = false;
                if (m_state->activeElement->text.empty()) {
                    m_state->markup.removeElement(m_state->activeElement->id);
                    m_state->activeElement = nullptr;
                }
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                syncShortcutHintContext();
                return 0;
            }

            // 3. 有激活选中的标注元素：取消选中
            if (m_state->activeElement) {
                m_state->activeElement->isActive = false;
                m_state->activeElement = nullptr;
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                syncShortcutHintContext();
                return 0;
            }

            // 3.5 若激活了标注工具：右键取消当前工具，返回选区调整模式
            if (m_state->isMarkupToolActive) {
                m_state->isMarkupToolActive = false;
                m_state->openSubmenu = SubmenuType::None;
                m_state->sliderPopup.type = SliderPopupType::None;
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                syncShortcutHintContext();
                return 0;
            }

            // 4. 处于选区态（Selected/Marking）：长图模式下右键保持在浏览裁切态（绝不清空长图或标注），普通截图下重置选区
            if ((int)m_state->state.load() == (int)OverlayState::Selected || (int)m_state->state.load() == (int)OverlayState::Marking) {
                if (m_state->isLongImageMode()) {
                    return 0;
                }
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
                return 0;
            }

            // 5. 无选区状态下：鼠标右键直接退出截图
            ShortcutHintOverlay::instance().hide();
            if (m_cancelCb) {
                m_cancelCb();
            }
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture

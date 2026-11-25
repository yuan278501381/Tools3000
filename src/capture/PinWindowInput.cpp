// ─────────────────────────────────────────────────────────────────────────────
// PinWindowInput.cpp — 贴图窗口输入交互、按钮命中、右键菜单与消息循环子模块
//
// 职责:
//   - 右键菜单状态快照构造、菜单命令执行与分组弹出菜单入口
//   - 悬停工具栏/悬浮工坊/原位改字精调岛/二维码徽章热区命中检测与交互执行
//   - Win32 窗口过程 pinWndProc（鼠标拖拽/缩放/滚轮/实景选字/IME/键盘快捷键/DPI 变更）
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinWindowInternal.h"
#include "core/i18n/UiI18n.h"
#include "capture/CaptureHistory.h"
#include "capture/CaptureOverlay.h"
#include "capture/PinContextMenu.h"
#include "capture/PinGroupBarOverlay.h"
#include "capture/PinGroupManager.h"
#include "capture/PinInputController.h"
#include "capture/TextAnnotationHelper.h"
#include "capture/TextReplaceEngine.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"
#include "ocr/DeepOcrEngine.h"

#include <d2d1_1.h>
#include <imm.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tools3000::capture {

using namespace pin_detail;

namespace pin_detail {
PinContextMenuSnapshot buildPinContextMenuSnapshot(const PinWindow* self) {
    PinContextMenuSnapshot snap;
    if (!self) return snap;

    const bool isZh = tools3000::core::isUiLanguageChinese();
    snap.isChinese = isZh;
    snap.isFolded = self->isFolded();
    snap.isInverted = self->isInverted();
    snap.isGrayscale = self->isGrayscale();
    snap.isClickThrough = self->isClickThrough();
    snap.isTextSelectionMode = self->isTextSelectionMode();
    snap.hasSelectedText = (self->selectedTextBlockCount() > 0);
    snap.hasPinnedSourceText = !self->pinnedSourceText().empty();
    snap.hasTranslation = self->hasTranslationResult();
    snap.isShowingTranslation = self->isShowingTranslation();
    snap.isTranslationInFlight = self->isTranslationInFlight();
    snap.activeOcrScriptMode = self->effectiveOcrScriptMode();
    snap.hasOcrScriptOverride = self->hasOcrScriptModeOverride();
    {
        const auto& deepOcr = tools3000::ocr::DeepOcrEngine::instance();
        snap.engineIsV6 = (deepOcr.installedEngineGeneration() == "v6");
        snap.scriptPackJapanInstalled = deepOcr.isScriptPackInstalled("japan");
        snap.scriptPackKoreanInstalled = deepOcr.isScriptPackInstalled("korean");
        snap.scriptPackCyrillicInstalled = deepOcr.isScriptPackInstalled("cyrillic");
        snap.scriptPackLatinInstalled = deepOcr.isScriptPackInstalled("latin");
        snap.scriptPackArabicInstalled = deepOcr.isScriptPackInstalled("arabic");
        snap.scriptPackThaiInstalled = deepOcr.isScriptPackInstalled("thai");
    }
    snap.scale = self->scale();
    snap.opacity = self->opacity();

    snap.currentGroupId = self->groupId();
    snap.activeGroupFilter = PinWindow::activeGroupFilter();
    snap.nextAvailableGroupId = PinWindow::nextAvailableGroupId();
    snap.totalPinCount = PinWindow::count();
    snap.ungroupedPinCount = PinWindow::countInGroup(PinGroupConstants::kUngroupedId);
    snap.allPinsHidden = PinWindow::isAllHidden();

    if (snap.currentGroupId >= PinGroupConstants::kMinGroupId) {
        snap.currentGroupHasCustomization = PinGroupPalette::hasCustomization(snap.currentGroupId);
        snap.currentGroupShortLabel = PinGroupPalette::shortLabel(snap.currentGroupId, isZh);
        snap.currentGroupAccentColor = PinGroupPalette::accentColor(snap.currentGroupId, 1.0f);

        const std::string curHex = PinGroupPalette::accentHex(snap.currentGroupId);
        const auto& presets = PinGroupPalette::presetPaletteHexes();
        snap.currentGroupColorPresets.reserve(presets.size());
        for (int idx = 0; idx < static_cast<int>(presets.size()); ++idx) {
            PinContextMenuSnapshot::ColorPresetInfo pInfo;
            pInfo.slotIndex = idx + 1;
            pInfo.name = PinGroupPalette::presetColorName(idx + 1, isZh);
            pInfo.color = PinGroupPalette::defaultColor(idx + 1, 1.0f);
            pInfo.isCurrent = (curHex == presets[static_cast<size_t>(idx)]);
            snap.currentGroupColorPresets.push_back(std::move(pInfo));
        }
    }

    snap.groups.reserve(PinGroupConstants::kMaxGroupId);
    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        const size_t cnt = PinWindow::countInGroup(g);
        const bool explicitCreated = PinWindow::isExplicitGroupCreated(g);
        const bool customized = PinGroupPalette::hasCustomization(g);
        const bool showAssign = (g <= PinGroupConstants::kDefaultQuickGroupCount) ||
                                (cnt > 0) ||
                                explicitCreated ||
                                (snap.currentGroupId == g) ||
                                (snap.activeGroupFilter == g) ||
                                customized;
        const bool showView = (cnt > 0) || explicitCreated || (snap.activeGroupFilter == g);
        if (!showAssign && !showView && g != snap.nextAvailableGroupId) {
            continue;
        }

        PinContextMenuSnapshot::GroupEntryInfo entry;
        entry.groupId = g;
        entry.count = cnt;
        entry.showInAssignList = showAssign;
        entry.showInViewList = showView;
        entry.shortLabel = PinGroupPalette::shortLabel(g, isZh);
        entry.fullLabel = PinGroupPalette::fullLabel(g, isZh);
        entry.accentColor = PinGroupPalette::accentColor(g, 1.0f);
        snap.groups.push_back(std::move(entry));
    }

    return snap;
}

void executePinContextMenuCommand(PinWindow* self, HWND hwnd, const PinMenuCommand& cmd) {
    if (!self || !self->isAlive() || !cmd.isValid()) return;

    switch (cmd.action) {
        case PinMenuAction::CopyImage:
            self->copyPinImageWithFeedback();
            break;
        case PinMenuAction::CopySelectedText:
            self->copySelectedText();
            break;
        case PinMenuAction::CopyNoteText:
        case PinMenuAction::CopyAllText:
            self->copyAllText();
            break;
        case PinMenuAction::EditMarkup:
            self->editMarkup();
            break;
        case PinMenuAction::ExtractTextDialog:
            self->extractText();
            break;
        case PinMenuAction::TranslateInPlace:
            self->translateInPlace(self->selectedTextBlockCount() > 0, /*forceRetranslate=*/false);
            break;
        case PinMenuAction::RetranslateInPlace:
            self->translateInPlace(/*selectedOnly=*/false, /*forceRetranslate=*/true);
            break;
        case PinMenuAction::ToggleTranslationOverlay:
            self->toggleTranslationOverlay();
            break;
        case PinMenuAction::CopyTranslatedText:
            self->copyTranslatedText();
            break;
        case PinMenuAction::ToggleTextSelectionMode:
            self->toggleTextSelectionMode();
            break;
        case PinMenuAction::SelectAllText:
            self->selectAllText();
            break;
        case PinMenuAction::SetOcrScriptMode:
            self->setOcrScriptModeOverride(PinOcrScriptParam::toModeString(cmd.param));
            break;
        case PinMenuAction::SaveImage:
            if (hwnd && IsWindow(hwnd)) {
                savePinnedImage(hwnd, self->activeDisplayImage());
            }
            break;
        case PinMenuAction::ResetScale:
            self->resetScale();
            break;
        case PinMenuAction::RotateRight90:
            self->rotate(PinInteractionConstants::kRotateClockwiseDeg);
            break;
        case PinMenuAction::RotateLeft90:
            self->rotate(PinInteractionConstants::kRotateCounterClockwiseDeg);
            break;
        case PinMenuAction::FlipHorizontal:
            self->flip(true);
            break;
        case PinMenuAction::FlipVertical:
            self->flip(false);
            break;
        case PinMenuAction::ToggleInvert:
            self->invertColors();
            break;
        case PinMenuAction::ToggleGrayscale:
            self->toggleGrayscale();
            break;
        case PinMenuAction::ToggleFold:
            self->toggleFold();
            break;
        case PinMenuAction::ToggleClickThrough:
            self->setClickThrough(!self->isClickThrough());
            break;
        case PinMenuAction::ArrangeAllPins:
            PinWindow::arrangeAll();
            break;
        case PinMenuAction::ToggleHideAllPins:
            PinWindow::toggleHideAll();
            break;
        case PinMenuAction::CloseOtherPins:
            PinWindow::closeAllExcept(self);
            break;
        case PinMenuAction::CloseAllPins:
            PinWindow::closeAll();
            break;
        case PinMenuAction::ClosePin:
            self->close();
            break;

        case PinMenuAction::GroupAssign:
            self->toggleAssignGroup(cmd.param);
            break;
        case PinMenuAction::GroupCreateAndAssign:
            PinWindow::createAndActivateNewGroup(self);
            break;
        case PinMenuAction::GroupRemoveCurrent:
            self->setGroupId(PinGroupConstants::kUngroupedId);
            break;
        case PinMenuAction::GroupAutoGroupUngrouped:
            PinWindow::groupAllUngrouped();
            break;
        case PinMenuAction::GroupViewAll:
            PinWindow::setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
            break;
        case PinMenuAction::GroupViewOnly:
            if (PinWindow::activeGroupFilter() == cmd.param) {
                PinWindow::setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
            } else {
                PinWindow::setActiveGroupFilter(cmd.param);
            }
            break;
        case PinMenuAction::GroupArrangeCurrent:
            if (self->groupId() >= PinGroupConstants::kMinGroupId) {
                PinWindow::arrangeGroup(self->groupId());
            }
            break;
        case PinMenuAction::GroupDissolveCurrent:
            if (self->groupId() >= PinGroupConstants::kMinGroupId) {
                PinWindow::dissolveGroup(self->groupId());
            }
            break;
        case PinMenuAction::GroupCloseCurrent:
            if (self->groupId() >= PinGroupConstants::kMinGroupId) {
                PinWindow::closeGroup(self->groupId());
            }
            break;
        case PinMenuAction::GroupRenameCurrent:
            if (self->groupId() >= PinGroupConstants::kMinGroupId) {
                PinGroupBarOverlay::instance().beginInlineRename(self->groupId());
            }
            break;
        case PinMenuAction::GroupSetColorPreset: {
            const auto& presets = PinGroupPalette::presetPaletteHexes();
            const int slot = cmd.param - 1;
            if (self->groupId() >= PinGroupConstants::kMinGroupId &&
                slot >= 0 && static_cast<size_t>(slot) < presets.size()) {
                if (PinGroupPalette::setGroupColor(self->groupId(), presets[static_cast<size_t>(slot)])) {
                    PinWindow::refreshAllVisualsAndGroupBar();
                }
            }
            break;
        }
        case PinMenuAction::GroupResetMeta:
            if (self->groupId() >= PinGroupConstants::kMinGroupId) {
                PinGroupPalette::resetGroupCustomization(self->groupId());
                PinWindow::refreshAllVisualsAndGroupBar();
            }
            break;
        case PinMenuAction::None:
        default:
            break;
    }
}

} // namespace pin_detail

void PinWindow::showGroupPopupMenu(POINT screenPt) {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    std::weak_ptr<PinWindow> weakSelf = weak_from_this();
    const PinContextMenuSnapshot snapshot = buildPinContextMenuSnapshot(this);
    const PinMenuCommand cmd = PinContextMenu::instance().showGroupMenu(
        m_hwnd,
        screenPt,
        snapshot,
        [weakSelf]() {
            const auto locked = weakSelf.lock();
            return locked && locked->isAlive() && locked->hwnd() && IsWindow(locked->hwnd());
        });
    if (auto aliveSelf = weakSelf.lock(); aliveSelf && aliveSelf->isAlive()) {
        executePinContextMenuCommand(aliveSelf.get(), aliveSelf->hwnd(), cmd);
    }
}

void PinWindow::forceHoverStateForTest(bool hovering) {
    m_isHovering = hovering;
    m_hoverAlpha = hovering ? 1.0f : 0.0f;
    if (!hovering) {
        clearHoverToolbarRects();
        m_hoverGroupBadge = false;
        m_hoverCopyFloating = false;
        m_hoverCopyTableFloating = false;
        m_hoverOpenPanelFloating = false;
        m_hoverTranslateFloating = false;
        m_hoverTranslateStatus = false;
        m_hoveredQrBadgeIndex = -1;
    }
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, 1);
    }
    render();
}

void PinWindow::clearHoverToolbarRects() noexcept {
    m_toolbarRect = {};
    m_btnFoldRect = {};
    m_btnRotateRect = {};
    m_btnInvertRect = {};
    m_btnEditRect = {};
    m_btnTextRect = {};
    m_btnTranslateRect = {};
    m_btnCopyRect = {};
    m_btnSaveRect = {};
    m_btnCloseRect = {};
    m_hoverFold = false;
    m_hoverRotate = false;
    m_hoverInvert = false;
    m_hoverEdit = false;
    m_hoverText = false;
    m_hoverTranslate = false;
    m_hoverCopy = false;
    m_hoverSave = false;
    m_hoverClose = false;
}

int PinWindow::hitTestInteractiveButton(int x, int y) const noexcept {
    if (m_folded) {
        return kPinBtnNone;
    }
    if (m_editingTextReplaceIdx >= 0) {
        if (isPointInRectF(m_btnTrFontDecRect, x, y)) return kPinBtnTrFontDec;
        if (isPointInRectF(m_btnTrFontIncRect, x, y)) return kPinBtnTrFontInc;
        if (isPointInRectF(m_btnTrWeightRect, x, y))  return kPinBtnTrWeight;
        if (isPointInRectF(m_btnTrFamilyRect, x, y))  return kPinBtnTrFamily;
        if (isPointInRectF(m_btnTrBgModeRect, x, y))  return kPinBtnTrBgMode;
        if (isPointInRectF(m_btnTrReflowRect, x, y))  return kPinBtnTrReflow;
        if (isPointInRectF(m_btnTrDoneRect, x, y))    return kPinBtnTrDone;
    }
    if (isPointInRectF(m_btnGroupBadgeRect, x, y)) {
        return kPinBtnGroupBadge;
    }
    if (!m_selectedBlockIndices.empty() && m_editingTextReplaceIdx < 0) {
        if (isPointInRectF(m_btnCopyFloatingRect, x, y)) {
            return kPinBtnCopyFloating;
        }
        if (isPointInRectF(m_btnCopyTableFloatingRect, x, y)) {
            return kPinBtnCopyTableFloating;
        }
        if (isPointInRectF(m_btnOpenPanelFloatingRect, x, y)) {
            return kPinBtnOpenPanelFloating;
        }
        if (isPointInRectF(m_btnTranslateFloatingRect, x, y)) {
            return kPinBtnTranslateFloating;
        }
        if (isPointInRectF(m_btnHighlightFloatingRect, x, y)) {
            return kPinBtnHighlightFloating;
        }
        if (isPointInRectF(m_btnWavyFloatingRect, x, y)) {
            return kPinBtnWavyFloating;
        }
        if (isPointInRectF(m_btnUnderlineFloatingRect, x, y)) {
            return kPinBtnUnderlineFloating;
        }
        if (isPointInRectF(m_btnTextReplaceFloatingRect, x, y)) {
            return kPinBtnTextReplaceFloating;
        }
        if (isPointInRectF(m_btnUndoFloatingRect, x, y)) {
            return kPinBtnUndoFloating;
        }
        for (size_t i = 0; i < m_btnColorDotFloatingRects.size(); ++i) {
            if (isPointInRectF(m_btnColorDotFloatingRects[i], x, y)) {
                return kPinBtnColorDotBase + static_cast<int>(i);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        for (size_t q = 0; q < m_qrCodes.size(); ++q) {
            if (isPointInRectF(m_qrCodes[q].badgeRect, x, y)) {
                return kPinBtnQrBadgeBase + static_cast<int>(q);
            }
        }
    }
    if (isPointInRectF(m_btnTranslateStatusRect, x, y)) {
        return kPinBtnTranslateStatus;
    }
    if (m_hoverAlpha > 0.0f && m_toolbarRect.right > m_toolbarRect.left) {
        if (isPointInRectF(m_btnFoldRect, x, y))      return kPinBtnFold;
        if (isPointInRectF(m_btnRotateRect, x, y))    return kPinBtnRotate;
        if (isPointInRectF(m_btnInvertRect, x, y))    return kPinBtnInvert;
        if (isPointInRectF(m_btnEditRect, x, y))      return kPinBtnEdit;
        if (isPointInRectF(m_btnTextRect, x, y))      return kPinBtnText;
        if (isPointInRectF(m_btnTranslateRect, x, y)) return kPinBtnTranslate;
        if (isPointInRectF(m_btnCopyRect, x, y))      return kPinBtnCopy;
        if (isPointInRectF(m_btnSaveRect, x, y))      return kPinBtnSave;
        if (isPointInRectF(m_btnCloseRect, x, y))     return kPinBtnClose;
    }
    return kPinBtnNone;
}

bool PinWindow::isInteractiveButtonHitWithSlop(int btnId, int x, int y, float slopPx) const noexcept {
    if (m_folded || btnId == kPinBtnNone) return false;
    auto hitExpanded = [&](const D2D1_RECT_F& rc) noexcept -> bool {
        if (rc.right <= rc.left || rc.bottom <= rc.top) return false;
        const float fx = static_cast<float>(x);
        const float fy = static_cast<float>(y);
        return fx >= rc.left - slopPx && fx <= rc.right + slopPx &&
               fy >= rc.top - slopPx && fy <= rc.bottom + slopPx;
    };
    if (btnId >= kPinBtnQrBadgeBase) {
        const size_t q = static_cast<size_t>(btnId - kPinBtnQrBadgeBase);
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        return q < m_qrCodes.size() && hitExpanded(m_qrCodes[q].badgeRect);
    }
    if (btnId >= kPinBtnColorDotBase && btnId < kPinBtnColorDotBase + 5) {
        const size_t ci = static_cast<size_t>(btnId - kPinBtnColorDotBase);
        return !m_selectedBlockIndices.empty() && ci < m_btnColorDotFloatingRects.size() &&
               hitExpanded(m_btnColorDotFloatingRects[ci]);
    }
    switch (btnId) {
        case kPinBtnGroupBadge:        return hitExpanded(m_btnGroupBadgeRect);
        case kPinBtnCopyFloating:      return !m_selectedBlockIndices.empty() && hitExpanded(m_btnCopyFloatingRect);
        case kPinBtnCopyTableFloating: return !m_selectedBlockIndices.empty() && hitExpanded(m_btnCopyTableFloatingRect);
        case kPinBtnOpenPanelFloating: return !m_selectedBlockIndices.empty() && hitExpanded(m_btnOpenPanelFloatingRect);
        case kPinBtnHighlightFloating: return !m_selectedBlockIndices.empty() && hitExpanded(m_btnHighlightFloatingRect);
        case kPinBtnWavyFloating:      return !m_selectedBlockIndices.empty() && hitExpanded(m_btnWavyFloatingRect);
        case kPinBtnUnderlineFloating: return !m_selectedBlockIndices.empty() && hitExpanded(m_btnUnderlineFloatingRect);
        case kPinBtnTextReplaceFloating: return !m_selectedBlockIndices.empty() && hitExpanded(m_btnTextReplaceFloatingRect);
        case kPinBtnTranslateFloating:   return !m_selectedBlockIndices.empty() && hitExpanded(m_btnTranslateFloatingRect);
        case kPinBtnTranslateStatus:     return hitExpanded(m_btnTranslateStatusRect);
        case kPinBtnUndoFloating:      return !m_selectedBlockIndices.empty() && hitExpanded(m_btnUndoFloatingRect);
        case kPinBtnTrFontDec:         return m_editingTextReplaceIdx >= 0 && hitExpanded(m_btnTrFontDecRect);
        case kPinBtnTrFontInc:         return m_editingTextReplaceIdx >= 0 && hitExpanded(m_btnTrFontIncRect);
        case kPinBtnTrWeight:          return m_editingTextReplaceIdx >= 0 && hitExpanded(m_btnTrWeightRect);
        case kPinBtnTrFamily:          return m_editingTextReplaceIdx >= 0 && hitExpanded(m_btnTrFamilyRect);
        case kPinBtnTrBgMode:          return m_editingTextReplaceIdx >= 0 && hitExpanded(m_btnTrBgModeRect);
        case kPinBtnTrReflow:          return m_editingTextReplaceIdx >= 0 && hitExpanded(m_btnTrReflowRect);
        case kPinBtnTrDone:            return m_editingTextReplaceIdx >= 0 && hitExpanded(m_btnTrDoneRect);
        // 右上角工具栏 9 键：只要按下时已锁定，抬手校验不受 m_hoverAlpha 定时器竞态淡出影响
        case kPinBtnFold:      return hitExpanded(m_btnFoldRect);
        case kPinBtnRotate:    return hitExpanded(m_btnRotateRect);
        case kPinBtnInvert:    return hitExpanded(m_btnInvertRect);
        case kPinBtnEdit:      return hitExpanded(m_btnEditRect);
        case kPinBtnText:      return hitExpanded(m_btnTextRect);
        case kPinBtnTranslate: return hitExpanded(m_btnTranslateRect);
        case kPinBtnCopy:      return hitExpanded(m_btnCopyRect);
        case kPinBtnSave:      return hitExpanded(m_btnSaveRect);
        case kPinBtnClose:     return hitExpanded(m_btnCloseRect);
        default:            return false;
    }
}

bool PinWindow::copyPinImageWithFeedback() {
    if (m_editingTextReplaceIdx >= 0) {
        commitInPlaceTextReplace(true);
    }
    const bool ok = copyImageToClipboard(activeDisplayImage(), m_hwnd);
    const bool isZh = tools3000::core::isUiLanguageChinese();
    tools3000::core::EventBus::instance().publish(
        tools3000::core::ShowToastEvent{
            ok ? (tools3000::core::trWCstrFor(isZh, L"已复制贴图到剪贴板", L"Copied pin to clipboard"))
               : (tools3000::core::trWCstrFor(isZh, L"剪贴板被其他程序占用，复制失败，请重试", L"Clipboard busy, copy failed — please retry"))});
    return ok;
}

void PinWindow::executeInteractiveButton(int btnId, int clientX, int clientY, bool shiftHeld) {
    if (m_folded) return;
    if (btnId >= kPinBtnQrBadgeBase) {
        copyQrCodeText(static_cast<size_t>(btnId - kPinBtnQrBadgeBase));
        return;
    }
    if (btnId >= kPinBtnColorDotBase && btnId < kPinBtnColorDotBase + 5) {
        int visibleDots = 0;
        for (const auto& rc : m_btnColorDotFloatingRects) {
            if (rc.right > rc.left) ++visibleDots;
        }
        if (visibleDots <= 1) {
            setTextMarkupColorIndex(
                (m_textMarkupColorIndex + 1) % static_cast<int>(TextAnnotationConstants::kPresetColorCount));
        } else {
            setTextMarkupColorIndex(btnId - kPinBtnColorDotBase);
        }
        return;
    }
    switch (btnId) {
        case kPinBtnGroupBadge: {
            if (m_hwnd && IsWindow(m_hwnd)) {
                POINT screenPt{clientX, clientY};
                ClientToScreen(m_hwnd, &screenPt);
                showGroupPopupMenu(screenPt);
            }
            break;
        }
        case kPinBtnCopyFloating:
            // 已对选中文字施加批注进入清透预览态后，悬浮舱首钮直接复制带标注高清贴图；
            // 未批注蓝选态则复制选中的纯文本，彻底消除标注后点复制按钮只复制文字的困惑
            if (m_ocrMarkupPreviewClean && m_pinnedSourceText.empty()) {
                copyPinImageWithFeedback();
            } else {
                copySelectedText();
            }
            break;
        case kPinBtnCopyTableFloating:
            copySelectedOrAllAsTable();
            break;
        case kPinBtnOpenPanelFloating:
            openOcrComparisonPanel();
            break;
        case kPinBtnHighlightFloating:
            annotateSelectedText(TextMarkupMode::Highlight);
            break;
        case kPinBtnWavyFloating:
            annotateSelectedText(TextMarkupMode::WavyUnderline);
            break;
        case kPinBtnUnderlineFloating:
            annotateSelectedText(TextMarkupMode::StraightUnderline);
            break;
        case kPinBtnTextReplaceFloating:
            startInPlaceTextReplace();
            break;
        case kPinBtnTranslateFloating:
            translateInPlace(/*selectedOnly=*/true, /*forceRetranslate=*/true);
            break;
        case kPinBtnTranslateStatus:
            toggleTranslationOverlay();
            break;
        case kPinBtnTrFontDec:
        case kPinBtnTrFontInc:
        case kPinBtnTrWeight:
        case kPinBtnTrFamily:
        case kPinBtnTrBgMode:
        case kPinBtnTrReflow: {
            if (m_editingTextReplaceIdx >= 0 &&
                m_editingTextReplaceIdx < static_cast<int>(m_markupElements.size())) {
                auto& el = m_markupElements[static_cast<size_t>(m_editingTextReplaceIdx)];
                if (btnId == kPinBtnTrFontDec) {
                    el.fontSize = std::clamp(el.fontSize - 0.5f, 8.0f, 128.0f);
                } else if (btnId == kPinBtnTrFontInc) {
                    el.fontSize = std::clamp(el.fontSize + 0.5f, 8.0f, 128.0f);
                } else if (btnId == kPinBtnTrWeight) {
                    el.replaceFontWeight = (el.replaceFontWeight < 500) ? 500
                        : ((el.replaceFontWeight < 600) ? 600
                        : ((el.replaceFontWeight < 700) ? 700 : 400));
                } else if (btnId == kPinBtnTrFamily) {
                    el.replaceFontClass = (el.replaceFontClass + 1) % 3;
                } else if (btnId == kPinBtnTrBgMode) {
                    el.replaceBgMode = (el.replaceBgMode + 1) % 3;
                    if (el.replaceBgMode == 2 && !AiInpaintEngine::instance().isModelInstalled()) {
                        const bool zh = tools3000::core::isUiLanguageChinese();
                        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                            tools3000::core::trWCstrFor(zh, L"当前使用内置快速纹理修复；可在「设置 -> 文字识别」按需下载 MI-GAN 512 AI 模型增强复杂底纹", L"Using built-in fast inpainting; download MI-GAN 512 in Settings > OCR for complex textures")});
                    }
                } else if (btnId == kPinBtnTrReflow) {
                    el.replaceReflowRight = !el.replaceReflowRight;
                }
                el.textLastCaretTick = GetTickCount();
                recompositeMarkupPreservingOcr();
            }
            break;
        }
        case kPinBtnTrDone:
            commitInPlaceTextReplace(true);
            break;
        case kPinBtnUndoFloating:
            undoPinMarkup();
            break;
        case kPinBtnFold:
            toggleFold();
            break;
        case kPinBtnRotate:
            rotate(shiftHeld ? 270 : 90);
            break;
        case kPinBtnInvert:
            invertColors();
            break;
        case kPinBtnEdit:
            editMarkup();
            break;
        case kPinBtnText:
            if (!m_selectedBlockIndices.empty()) {
                copySelectedText();
            } else if (!m_pinnedSourceText.empty()) {
                copyAllText();
            } else {
                toggleTextSelectionMode();
            }
            break;
        case kPinBtnTranslate:
            translateInPlace(/*selectedOnly=*/false, /*forceRetranslate=*/shiftHeld);
            break;
        case kPinBtnCopy:
            copyPinImageWithFeedback();
            break;
        case kPinBtnSave:
            if (m_hwnd && IsWindow(m_hwnd)) {
                savePinnedImage(m_hwnd, activeDisplayImage());
            }
            break;
        case kPinBtnClose:
            close();
            break;
        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口过程
// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK PinWindow::pinWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_MOUSEACTIVATE: {
            if (self) {
                self->focusWindow();
            }
            return MA_ACTIVATE;
        }

        case WM_SYSCOMMAND: {
            if ((wParam & 0xFFF0) == SC_KEYMENU || (wParam & 0xFFF0) == SC_CONTEXTHELP) {
                return 0;
            }
            break;
        }

        case WM_SETCURSOR: {
            if (self && !self->m_folded) {
                const bool hoverAnyBtn =
                    self->m_hoverGroupBadge || self->m_hoverCopyFloating ||
                    self->m_hoverCopyTableFloating || self->m_hoverOpenPanelFloating ||
                    self->m_hoverHighlightFloating || self->m_hoverWavyFloating ||
                    self->m_hoverUnderlineFloating || self->m_hoverTextReplaceFloating ||
                    self->m_hoverTranslateFloating || self->m_hoverTranslateStatus ||
                    self->m_hoverUndoFloating || self->m_hoveredColorDotIndex >= 0 ||
                    self->m_hoveredTrBtnId >= 0 ||
                    self->m_hoveredQrBadgeIndex >= 0 ||
                    self->m_hoverFold || self->m_hoverRotate || self->m_hoverInvert ||
                    self->m_hoverEdit || self->m_hoverText || self->m_hoverTranslate ||
                    self->m_hoverCopy || self->m_hoverSave || self->m_hoverClose;
                if (self->m_editingTextReplaceIdx >= 0 && !hoverAnyBtn) {
                    if (self->m_isDraggingReplacePos || self->m_hoverReplaceDragHandle) {
                        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                    } else {
                        SetCursor(LoadCursor(nullptr, IDC_IBEAM));
                    }
                    return TRUE;
                }
                const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

                PinLiveTextState cursorState;
                cursorState.isFolded = self->m_folded;
                cursorState.isDragging = self->m_isDragging;
                cursorState.isSelectingText = self->m_isSelectingText;
                cursorState.hoverInteractiveButton = hoverAnyBtn;
                cursorState.altHeld = alt;
                cursorState.textSelectionMode = self->m_textSelectionMode;
                cursorState.liveTextEnabled = self->isLiveTextEnabled();
                cursorState.ocrReady = self->m_ocrReady.load(std::memory_order_acquire);
                cursorState.hitTextBlock = (self->m_hoveredBlockIndex >= 0);

                switch (PinInputController::resolveCursorKind(cursorState)) {
                    case PinCursorKind::HandButton:
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return TRUE;
                    case PinCursorKind::TextBeam:
                        SetCursor(LoadCursor(nullptr, IDC_IBEAM));
                        return TRUE;
                    case PinCursorKind::MoveWindow:
                    default:
                        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                        return TRUE;
                }
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            if (!self) break;
            if (self->m_zoomAnimating || self->m_zoomStageActive) {
                self->stopZoomAnimation(true);
            }
            self->focusWindow();
            const int clickX = GET_X_LPARAM(lParam);
            const int clickY = GET_Y_LPARAM(lParam);

            self->m_pressedToolbarBtn = kPinBtnNone;
            self->m_pressStartClientPt = {clickX, clickY};
            self->m_dragOffset = {clickX, clickY};
            self->m_dragThresholdExceeded = false;
            self->m_isDragging = false;

            const int hitBtn = self->hitTestInteractiveButton(clickX, clickY);
            if (self->m_editingTextReplaceIdx >= 0) {
                if (hitBtn != kPinBtnNone) {
                    self->m_pressedToolbarBtn = hitBtn;
                    SetCapture(hwnd);
                    return 0;
                }
                // 1) 点击左侧 6 点微晶抓手、编辑框四边边框带，或按住 Alt 点击框内 -> 鼠标自由拖拽移动所改文字位置
                if (self->m_editingTextReplaceIdx < static_cast<int>(self->m_markupElements.size())) {
                    auto& edEl = self->m_markupElements[static_cast<size_t>(self->m_editingTextReplaceIdx)];
                    const float safeScale = (std::max)(0.05f, self->m_scale);
                    const float safeDpi = (self->m_dpiScale > 0.0f) ? self->m_dpiScale : 1.0f;
                    const bool altHeld = (GetKeyState(VK_MENU) & 0x8000) != 0;

                    auto isPointOnReplaceDragZone = [&](int px, int py) noexcept -> bool {
                        const D2D1_RECT_F gRc = self->m_trDragHandleRect;
                        const float gPad = 3.0f * safeDpi;
                        if (gRc.right > gRc.left &&
                            static_cast<float>(px) >= gRc.left - gPad && static_cast<float>(px) <= gRc.right + gPad &&
                            static_cast<float>(py) >= gRc.top - gPad && static_cast<float>(py) <= gRc.bottom + gPad) {
                            return true;
                        }
                        const D2D1_RECT_F bRc = self->m_trEditBoxClientRect;
                        if (bRc.right > bRc.left && bRc.bottom > bRc.top) {
                            const float outPad = 5.5f * safeDpi;
                            const float inPad = (std::min)(3.0f * safeDpi, (bRc.bottom - bRc.top) * 0.22f);
                            const bool inOuter =
                                static_cast<float>(px) >= bRc.left - outPad && static_cast<float>(px) <= bRc.right + outPad &&
                                static_cast<float>(py) >= bRc.top - outPad && static_cast<float>(py) <= bRc.bottom + outPad;
                            const bool inInner =
                                static_cast<float>(px) >= bRc.left + inPad && static_cast<float>(px) <= bRc.right - inPad &&
                                static_cast<float>(py) >= bRc.top + inPad && static_cast<float>(py) <= bRc.bottom - inPad;
                            if (inOuter && (!inInner || altHeld)) {
                                return true;
                            }
                        }
                        return false;
                    };

                    if (isPointOnReplaceDragZone(clickX, clickY)) {
                        self->m_isDraggingReplacePos = true;
                        self->m_hoverReplaceDragHandle = true;
                        self->m_replaceDragStartClientPt = {clickX, clickY};
                        self->m_replaceDragOrigOffsetX = edEl.replaceOffsetX;
                        self->m_replaceDragOrigOffsetY = edEl.replaceOffsetY;
                        SetCapture(hwnd);
                        self->render();
                        return 0;
                    }

                    // 2) 点击编辑框内部文字核心区 -> 精准落点光标并支持拖选局部字符；点击外部 -> 自动确认提交退出编辑态
                    const float imgXf = static_cast<float>(clickX) / safeScale;
                    const float imgYf = static_cast<float>(clickY) / safeScale;
                    const auto ov = TextReplaceEngine::computeEditOverlayMetrics(
                        self->m_cleanBaseImage, buildTextReplaceSpecForOverlay(edEl));
                    bool hitEditingBox = false;
                    if (ov.valid) {
                        hitEditingBox = (imgXf >= ov.boxLeft - 6.0f && imgXf <= ov.boxRight + 6.0f &&
                                         imgYf >= ov.boxTop - 6.0f && imgYf <= ov.boxBottom + 6.0f);
                    } else {
                        const cv::Rect hitBox = (edEl.replaceLastRenderRect.width > 0 && edEl.replaceLastRenderRect.height > 0)
                            ? edEl.replaceLastRenderRect
                            : edEl.replaceSourceRect;
                        hitEditingBox = cv::Rect(hitBox.x - 6, hitBox.y - 6, hitBox.width + 12, hitBox.height + 12)
                            .contains(cv::Point(static_cast<int>(std::lround(imgXf)), static_cast<int>(std::lround(imgYf))));
                    }
                    if (hitEditingBox) {
                        const size_t hitIdx = TextReplaceEngine::hitTestCaretIndex(
                            self->m_cleanBaseImage, buildTextReplaceSpecForOverlay(edEl), imgXf, imgYf);
                        const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        edEl.textCaret = hitIdx;
                        if (!shiftDown) {
                            edEl.textSelAnchor = hitIdx;
                        }
                        edEl.textLastCaretTick = GetTickCount();
                        self->m_isSelectingReplaceText = true;
                        SetCapture(hwnd);
                        self->updateImeCompositionAnchor();
                        self->render();
                        return 0;
                    }
                }
                self->commitInPlaceTextReplace(true);
                return 0;
            }
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool liveEnabled = self->isLiveTextEnabled();

            if (!self->m_folded && (alt || self->m_textSelectionMode) && !self->m_ocrReady.load()) {
                self->ensureOcrExecutedAsync(false);
            }

            int hitOcrBlock = -1;
            size_t hitCharIdx = 0;
            if (!self->m_folded && hitBtn == kPinBtnNone && self->m_ocrReady.load()) {
                std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                const float safeScale = (std::max)(0.05f, self->m_scale);
                const float mx = static_cast<float>(clickX) / safeScale;
                const float my = static_cast<float>(clickY) / safeScale;
                const cv::Point pt(static_cast<int>(mx), static_cast<int>(my));
                for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                    if (self->m_ocrBlocks[i].origBox.contains(pt)) {
                        hitOcrBlock = static_cast<int>(i);
                        hitCharIdx = locateNearestCharInBlock(self->m_ocrBlocks[i], mx);
                        break;
                    }
                }
            }

            PinLiveTextState pressState;
            pressState.isFolded = self->m_folded;
            pressState.altHeld = alt;
            pressState.textSelectionMode = self->m_textSelectionMode;
            pressState.liveTextEnabled = liveEnabled;
            pressState.ocrReady = self->m_ocrReady.load(std::memory_order_acquire);
            pressState.hitTextBlock = (hitOcrBlock >= 0);

            const PinLeftPressMode pressMode = PinInputController::resolveLeftPressMode(
                pressState, hitBtn != kPinBtnNone);

            if (pressMode == PinLeftPressMode::PressInteractiveButton) {
                self->m_pressedToolbarBtn = hitBtn;
                SetCapture(hwnd);
                return 0;
            }

            if (pressMode == PinLeftPressMode::StartLiveTextSelection) {
                self->m_ocrMarkupPreviewClean = false;
                self->m_isSelectingText = true;
                self->m_isCharStreamSelecting = (!alt && hitOcrBlock >= 0);
                self->m_anchorBlockIndex = hitOcrBlock;
                self->m_anchorCharIndex = hitCharIdx;
                self->m_selectStart = {clickX, clickY};
                self->m_selectEnd = {clickX, clickY};
                {
                    std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                    self->m_selectedBlockIndices.clear();
                    self->m_selectedCharSpans.clear();
                    if (hitOcrBlock >= 0 && !self->m_isCharStreamSelecting) {
                        self->m_selectedBlockIndices.push_back(static_cast<size_t>(hitOcrBlock));
                    }
                }
                SetCapture(hwnd);
                self->render();
                return 0;
            }

            // 空白区点击：若有选中文字则先清空，随即直接拖动贴图窗口
            {
                bool hadSel = false;
                {
                    std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                    hadSel = !self->m_selectedBlockIndices.empty();
                    self->m_selectedBlockIndices.clear();
                    self->m_selectedCharSpans.clear();
                }
                if (hadSel) {
                    self->render();
                }
            }

            self->m_isDragging = true;
            SetCapture(hwnd);
            return 0;
        }

        case WM_ACTIVATE: {
            if (self) {
                self->syncImeState(true);
            }
            break;
        }

        case WM_SETFOCUS: {
            if (self) {
                self->m_focused = true;
                self->syncImeState(true);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_KILLFOCUS: {
            if (self) {
                self->m_focused = false;
                if (self->m_zoomAnimating || self->m_zoomStageActive) {
                    self->stopZoomAnimation(true);
                } else {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_IME_SETCONTEXT:
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_CHAR: {
            if (self) {
                const bool allowText = self->isTextInputActive();
                LRESULT imeResult = 0;
                if (tools3000::core::WindowImeGuard::filterNonTextImeMessage(
                        hwnd, msg, wParam, lParam, allowText, imeResult)) {
                    return imeResult;
                }
                if (msg == WM_IME_STARTCOMPOSITION && allowText) {
                    self->updateImeCompositionAnchor();
                }
            }
            break;
        }

        case WM_CHAR: {
            if (self && self->m_editingTextReplaceIdx >= 0) {
                if (self->handleTextReplaceChar(static_cast<wchar_t>(wParam))) {
                    return 0;
                }
            }
            break;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            if (!self) break;
            const bool allowText = self->isTextInputActive();
            self->syncImeState(false);
            wParam = tools3000::core::WindowImeGuard::resolveShortcutVirtualKey(
                hwnd, wParam, lParam, allowText);
            const bool isPureModifier =
                wParam == VK_SHIFT || wParam == VK_CONTROL || wParam == VK_MENU ||
                wParam == VK_LSHIFT || wParam == VK_RSHIFT ||
                wParam == VK_LCONTROL || wParam == VK_RCONTROL ||
                wParam == VK_LMENU || wParam == VK_RMENU;
            if (!isPureModifier && (self->m_zoomAnimating || self->m_zoomStageActive)) {
                self->stopZoomAnimation(true);
            }
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (msg == WM_SYSKEYDOWN) || ((GetKeyState(VK_MENU) & 0x8000) != 0);
            if (self->m_editingTextReplaceIdx >= 0) {
                if (self->handleTextReplaceKeyDown(wParam, ctrl, shift, alt)) {
                    return 0;
                }
            }
            bool hasSelectedBlocks = false;
            {
                std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                hasSelectedBlocks = !self->m_selectedBlockIndices.empty();
            }
            if (hasSelectedBlocks && !ctrl && !alt && !shift && wParam == 'E') {
                if (self->startInPlaceTextReplace()) {
                    return 0;
                }
            }
            const bool hasTextContext = self->m_textSelectionMode || self->m_isSelectingText || hasSelectedBlocks;

            const auto decision = PinInputController::resolveKeyDown(wParam, ctrl, shift, alt, hasTextContext);
            switch (decision.action) {
                case PinShortcutAction::RotateClockwise90:
                case PinShortcutAction::RotateCounterClockwise90:
                    self->rotate(decision.rotateDegrees);
                    return 0;
                case PinShortcutAction::FlipHorizontal:
                    self->flip(true);
                    return 0;
                case PinShortcutAction::FlipVertical:
                    self->flip(false);
                    return 0;
                case PinShortcutAction::SetOpacity:
                    self->setOpacity(decision.opacityValue);
                    return 0;
                case PinShortcutAction::AdjustOpacity:
                    self->setOpacity(std::clamp(self->m_opacity + decision.opacityDelta,
                                                PinInteractionConstants::kMinPinOpacity,
                                                PinInteractionConstants::kMaxPinOpacity));
                    return 0;
                case PinShortcutAction::ResetScaleAndOpacity:
                    self->resetScale();
                    self->setOpacity(PinInteractionConstants::kMaxPinOpacity);
                    return 0;
                case PinShortcutAction::ZoomIn:
                case PinShortcutAction::ZoomOut:
                    self->setScale(self->m_scale * decision.scaleFactor);
                    return 0;
                case PinShortcutAction::Nudge: {
                    RECT rc{};
                    if (GetWindowRect(hwnd, &rc)) {
                        SetWindowPos(hwnd, nullptr,
                                     rc.left + decision.nudgeDx,
                                     rc.top + decision.nudgeDy,
                                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                    return 0;
                }
                case PinShortcutAction::EditMarkup:
                    self->editMarkup();
                    return 0;
                case PinShortcutAction::InvertColors:
                    self->invertColors();
                    return 0;
                case PinShortcutAction::ToggleGrayscale:
                    self->toggleGrayscale();
                    return 0;
                case PinShortcutAction::ToggleFold:
                    self->toggleFold();
                    return 0;
                case PinShortcutAction::ToggleClickThrough:
                    self->setClickThrough(!self->m_clickThrough);
                    return 0;
                case PinShortcutAction::PasteHistoryOrClipboard:
                    PinWindow::pasteNextHistoryOrClipboard();
                    return 0;
                case PinShortcutAction::SelectAllText:
                    self->selectAllText();
                    return 0;
                case PinShortcutAction::CopyAllText:
                    self->copyAllText();
                    return 0;
                case PinShortcutAction::CopySelectedOrImage:
                    // 仅当处于可见蓝底选字态（未落笔批注进入清透态）时，Ctrl+C 复制选中文字；
                    // 一旦对文字添加了荧光笔/波浪线/直线批注（m_ocrMarkupPreviewClean==true），
                    // 按 Ctrl+C 直接复制带全部最新标注的高清贴图，绝不再误复制成纯文本！
                    if (!self->m_selectedBlockIndices.empty() && !self->m_ocrMarkupPreviewClean) {
                        self->copySelectedText();
                        return 0;
                    }
                    if (!self->m_pinnedSourceText.empty()) {
                        self->copyAllText();
                        return 0;
                    }
                    self->copyPinImageWithFeedback();
                    return 0;
                case PinShortcutAction::ExtractText:
                    self->extractText();
                    return 0;
                case PinShortcutAction::TranslateInPlace:
                    self->translateInPlace(self->selectedTextBlockCount() > 0, /*forceRetranslate=*/false);
                    return 0;
                case PinShortcutAction::ToggleTranslationOverlay:
                    if (self->hasTranslationResult()) {
                        self->toggleTranslationOverlay();
                    }
                    return 0;
                case PinShortcutAction::SaveImage:
                    savePinnedImage(hwnd, self->activeDisplayImage());
                    return 0;
                case PinShortcutAction::HistoryPrev: {
                    const int totalHist = CaptureHistory::instance().count();
                    if (totalHist > 1) {
                        self->m_historyBrowseIndex = std::min(self->m_historyBrowseIndex + 1, totalHist - 1);
                        auto entry = CaptureHistory::instance().get(self->m_historyBrowseIndex);
                        if (entry.has_value() && !entry->image.empty()) {
                            self->updateImage(entry->image);
                            bool isZh = tools3000::core::isUiLanguageChinese();
                            std::wstring toastText = isZh
                                ? std::format(L"历史贴图 {}/{}", self->m_historyBrowseIndex + 1, totalHist)
                                : std::format(L"Pin history {}/{}", self->m_historyBrowseIndex + 1, totalHist);
                            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastText});
                        }
                    }
                    return 0;
                }
                case PinShortcutAction::HistoryNext: {
                    const int totalHist = CaptureHistory::instance().count();
                    if (totalHist > 1 && self->m_historyBrowseIndex > 0) {
                        self->m_historyBrowseIndex = std::max(0, self->m_historyBrowseIndex - 1);
                        auto entry = CaptureHistory::instance().get(self->m_historyBrowseIndex);
                        if (entry.has_value() && !entry->image.empty()) {
                            self->updateImage(entry->image);
                            bool isZh = tools3000::core::isUiLanguageChinese();
                            std::wstring toastText = isZh
                                ? std::format(L"历史贴图 {}/{}", self->m_historyBrowseIndex + 1, totalHist)
                                : std::format(L"Pin history {}/{}", self->m_historyBrowseIndex + 1, totalHist);
                            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastText});
                        }
                    }
                    return 0;
                }
                case PinShortcutAction::AssignOrToggleGroup:
                    self->toggleAssignGroup(decision.groupIndex);
                    return 0;
                case PinShortcutAction::RemoveFromGroup:
                    self->setGroupId(PinGroupConstants::kUngroupedId);
                    return 0;
                case PinShortcutAction::AutoGroupUngrouped:
                    if (PinWindow::countInGroup(PinGroupConstants::kUngroupedId) == 0 &&
                        self->m_groupId == PinGroupConstants::kUngroupedId) {
                        self->setGroupId(1);
                    } else {
                        PinWindow::groupAllUngrouped();
                    }
                    return 0;
                case PinShortcutAction::FilterByGroup:
                    if (PinWindow::activeGroupFilter() == decision.groupIndex) {
                        PinWindow::setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
                    } else {
                        PinWindow::setActiveGroupFilter(decision.groupIndex);
                    }
                    return 0;
                case PinShortcutAction::ShowAllGroups:
                    PinWindow::setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
                    return 0;
                case PinShortcutAction::AnnotateSelectedHighlight:
                    self->annotateSelectedText(TextMarkupMode::Highlight);
                    return 0;
                case PinShortcutAction::AnnotateSelectedWavy:
                    self->annotateSelectedText(TextMarkupMode::WavyUnderline);
                    return 0;
                case PinShortcutAction::AnnotateSelectedUnderline:
                    self->annotateSelectedText(TextMarkupMode::StraightUnderline);
                    return 0;
                case PinShortcutAction::UndoPinMarkup:
                    self->undoPinMarkup();
                    return 0;
                case PinShortcutAction::RedoPinMarkup:
                    self->redoPinMarkup();
                    return 0;
                case PinShortcutAction::EscapeOrClose:
                    if (!self->m_selectedBlockIndices.empty()) {
                        self->clearTextSelection();
                        self->render();
                        return 0;
                    }
                    if (self->m_textSelectionMode) {
                        self->m_textSelectionMode = false;
                        self->render();
                        return 0;
                    }
                    self->close();
                    return 0;
                case PinShortcutAction::ClosePin:
                    self->close();
                    return 0;
                case PinShortcutAction::None:
                default:
                    if (alt) return 0;
                    break;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (!self) break;
            if (!self->m_isHovering) {
                self->m_isHovering = true;
                self->m_hoverTime = GetTickCount64();
                self->m_lastInteractionTick = self->m_hoverTime;
                SetTimer(hwnd, 1, 16, nullptr);
                
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, HOVER_DEFAULT };
                TrackMouseEvent(&tme);

                // 4K 巨图 / 多贴图按需懒触发升格：用户鼠标首次移上贴图时立即以 Interactive (P0) 队首优先级启动实景识文，
                // 若该贴图此前正在队尾 SilentWarmup 排队，同样自动插队提权至队首秒级响应！
                if (!self->m_folded && self->m_pinnedSourceText.empty() &&
                    !self->m_ocrReady.load(std::memory_order_acquire) && self->isLiveTextEnabled() &&
                    !isAutomatedTestProcessMode()) {
                    if (self->m_ocrDeferredUntilHover.exchange(false, std::memory_order_acq_rel) ||
                        self->isOcrInFlight()) {
                        self->ensureOcrExecutedAsync(true);
                    }
                }
            }

            // 悬停即获焦（仅在非拖拽/非划选状态下触发，严禁在拖拽移动文字或划选期间调用 focusWindow() 破坏 SetCapture）
            const bool anyPointerDragActive =
                self->m_isDragging || self->m_isSelectingText ||
                self->m_isDraggingReplacePos || self->m_isSelectingReplaceText;
            const bool isActuallyFocused = self->m_focused && (GetFocus() == hwnd);
            if (!anyPointerDragActive && PinInputController::shouldAutoFocusOnHover(
                    isActuallyFocused, self->m_clickThrough, CaptureOverlay::instance().isOverlayVisible())) {
                self->focusWindow();
            }

            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const bool leftHeld = (wParam & MK_LBUTTON) != 0;

            // 原位改字编辑态：实时拖拽自由移动所改文字位置（0.5px 精度，按住 Shift 自动锁定水平/垂直轴）
            if (self->m_isDraggingReplacePos) {
                if (leftHeld && self->m_editingTextReplaceIdx >= 0 &&
                    self->m_editingTextReplaceIdx < static_cast<int>(self->m_markupElements.size())) {
                    auto& edEl = self->m_markupElements[static_cast<size_t>(self->m_editingTextReplaceIdx)];
                    const float safeScale = (std::max)(0.05f, self->m_scale);
                    float dxImg = static_cast<float>(x - self->m_replaceDragStartClientPt.x) / safeScale;
                    float dyImg = static_cast<float>(y - self->m_replaceDragStartClientPt.y) / safeScale;
                    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                        if (std::abs(dxImg) >= std::abs(dyImg)) dyImg = 0.0f;
                        else dxImg = 0.0f;
                    }
                    float rawOy = self->m_replaceDragOrigOffsetY + dyImg;
                    // 原基线 ±0.8px 轻柔磁吸（左右平移微调字距不跑偏；按住 Alt 随时绕过磁吸做 0.5px 任意垂直微调）
                    if (((GetKeyState(VK_MENU) & 0x8000) == 0) && std::abs(rawOy) <= 0.8f) {
                        rawOy = 0.0f;
                    }
                    const float nextOx = std::round((self->m_replaceDragOrigOffsetX + dxImg) * 2.0f) * 0.5f;
                    const float nextOy = std::round(rawOy * 2.0f) * 0.5f;
                    if (std::abs(nextOx - edEl.replaceOffsetX) >= 0.25f ||
                        std::abs(nextOy - edEl.replaceOffsetY) >= 0.25f) {
                        edEl.replaceOffsetX = nextOx;
                        edEl.replaceOffsetY = nextOy;
                        edEl.textLastCaretTick = GetTickCount();
                        self->recompositeMarkupPreservingOcr();
                        self->updateImeCompositionAnchor();
                    }
                    return 0;
                }
                self->m_isDraggingReplacePos = false;
            }

            // 原位改字编辑框内按住左键丝滑拖选部分字符
            if (self->m_isSelectingReplaceText) {
                if (leftHeld && self->m_editingTextReplaceIdx >= 0 &&
                    self->m_editingTextReplaceIdx < static_cast<int>(self->m_markupElements.size())) {
                    auto& edEl = self->m_markupElements[static_cast<size_t>(self->m_editingTextReplaceIdx)];
                    const float safeScale = (std::max)(0.05f, self->m_scale);
                    const size_t hitIdx = TextReplaceEngine::hitTestCaretIndex(
                        self->m_cleanBaseImage,
                        buildTextReplaceSpecForOverlay(edEl),
                        static_cast<float>(x) / safeScale,
                        static_cast<float>(y) / safeScale);
                    if (hitIdx != edEl.textCaret) {
                        edEl.textCaret = hitIdx;
                        edEl.textLastCaretTick = GetTickCount();
                        self->render();
                    }
                    return 0;
                }
                self->m_isSelectingReplaceText = false;
            }

            if (!leftHeld && (self->m_pressedToolbarBtn != kPinBtnNone || self->m_isDragging)) {
                self->m_pressedToolbarBtn = kPinBtnNone;
                self->m_isDragging = false;
                self->m_dragThresholdExceeded = false;
            }

            // Drag-First 阈值跃迁：在工具栏按钮上使用防手抖 10 DIP 阈值（避免点击小图标手轻晃 4~8px 误吞点击），
            // 超过 10 DIP 才切换为整窗拖拽移动，兼顾 100% 可靠点击与自由拖拽！
            if (leftHeld && self->m_pressedToolbarBtn != kPinBtnNone && !self->m_isDragging) {
                const float moveDist = std::hypot(
                    static_cast<float>(x - self->m_pressStartClientPt.x),
                    static_cast<float>(y - self->m_pressStartClientPt.y));
                const float safeScale = (self->m_dpiScale > 0.0f) ? self->m_dpiScale : 1.0f;
                const float btnDragCancelDip = (std::max)(10.0f, PinInteractionConstants::kDragStartThresholdDip * 2.5f);
                if (moveDist >= btnDragCancelDip * safeScale) {
                    self->m_pressedToolbarBtn = kPinBtnNone;
                    self->m_dragThresholdExceeded = true;
                    self->m_isDragging = true;
                }
            }

            if (self->m_isDragging) {
                if (self->m_zoomAnimating) {
                    self->stopZoomAnimation(true);
                }
                if (self->m_hoverGroupBadge || self->m_hoverFold || self->m_hoverRotate || self->m_hoverInvert ||
                    self->m_hoverEdit || self->m_hoverText || self->m_hoverTranslate || self->m_hoverCopy ||
                    self->m_hoverSave || self->m_hoverClose || self->m_hoverCopyFloating ||
                    self->m_hoverCopyTableFloating || self->m_hoverOpenPanelFloating ||
                    self->m_hoverTranslateFloating || self->m_hoverTranslateStatus ||
                    self->m_hoveredQrBadgeIndex >= 0) {
                    self->m_hoverGroupBadge = false;
                    self->m_hoverFold = false;
                    self->m_hoverRotate = false;
                    self->m_hoverInvert = false;
                    self->m_hoverEdit = false;
                    self->m_hoverText = false;
                    self->m_hoverTranslate = false;
                    self->m_hoverCopy = false;
                    self->m_hoverSave = false;
                    self->m_hoverClose = false;
                    self->m_hoverCopyFloating = false;
                    self->m_hoverCopyTableFloating = false;
                    self->m_hoverOpenPanelFloating = false;
                    self->m_hoverTranslateFloating = false;
                    self->m_hoverTranslateStatus = false;
                    self->m_hoveredQrBadgeIndex = -1;
                    self->render();
                }
                RECT winRc{};
                GetWindowRect(hwnd, &winRc);
                int curW = (std::max)(1, static_cast<int>(winRc.right - winRc.left));
                int curH = (std::max)(1, static_cast<int>(winRc.bottom - winRc.top));
                POINT targetPos = {
                    winRc.left + x - self->m_dragOffset.x,
                    winRc.top + y - self->m_dragOffset.y
                };
                targetPos = calculateMagneticSnap(hwnd, targetPos, curW, curH);
                SetWindowPos(hwnd, nullptr,
                             targetPos.x,
                             targetPos.y,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER);
                return 0;
            }
            
            if (self->m_folded) {
                if (self->m_hoverGroupBadge || self->m_hoverFold || self->m_hoverRotate || self->m_hoverInvert || self->m_hoverEdit ||
                    self->m_hoverText || self->m_hoverTranslate || self->m_hoverCopy || self->m_hoverSave || self->m_hoverClose ||
                    self->m_hoverTranslateStatus) {
                    self->m_hoverGroupBadge = false;
                    self->m_hoverFold = false;
                    self->m_hoverRotate = false;
                    self->m_hoverInvert = false;
                    self->m_hoverEdit = false;
                    self->m_hoverText = false;
                    self->m_hoverTranslate = false;
                    self->m_hoverCopy = false;
                    self->m_hoverSave = false;
                    self->m_hoverClose = false;
                    self->m_hoverTranslateStatus = false;
                    self->render();
                }
            } else {
                const bool hoverFold = isPointInRectF(self->m_btnFoldRect, x, y);
                const bool hoverRotate = isPointInRectF(self->m_btnRotateRect, x, y);
                const bool hoverInvert = isPointInRectF(self->m_btnInvertRect, x, y);
                const bool hoverEdit = isPointInRectF(self->m_btnEditRect, x, y);
                const bool hoverText = isPointInRectF(self->m_btnTextRect, x, y);
                const bool hoverTranslate = isPointInRectF(self->m_btnTranslateRect, x, y);
                const bool hoverCopy = isPointInRectF(self->m_btnCopyRect, x, y);
                const bool hoverSave = isPointInRectF(self->m_btnSaveRect, x, y);
                const bool hoverClose = isPointInRectF(self->m_btnCloseRect, x, y);
                const bool hoverGroupBadge = isPointInRectF(self->m_btnGroupBadgeRect, x, y);
                const bool hoverTranslateStatus = isPointInRectF(self->m_btnTranslateStatusRect, x, y);
                const bool hasSel = !self->m_selectedBlockIndices.empty() && self->m_editingTextReplaceIdx < 0;
                const bool hoverCopyFloating = (hasSel && isPointInRectF(self->m_btnCopyFloatingRect, x, y));
                const bool hoverCopyTableFloating = (hasSel && isPointInRectF(self->m_btnCopyTableFloatingRect, x, y));
                const bool hoverOpenPanelFloating = (hasSel && isPointInRectF(self->m_btnOpenPanelFloatingRect, x, y));
                const bool hoverHighlightFloating = (hasSel && isPointInRectF(self->m_btnHighlightFloatingRect, x, y));
                const bool hoverWavyFloating = (hasSel && isPointInRectF(self->m_btnWavyFloatingRect, x, y));
                const bool hoverUnderlineFloating = (hasSel && isPointInRectF(self->m_btnUnderlineFloatingRect, x, y));
                const bool hoverTextReplaceFloating = (hasSel && isPointInRectF(self->m_btnTextReplaceFloatingRect, x, y));
                const bool hoverTranslateFloating = (hasSel && isPointInRectF(self->m_btnTranslateFloatingRect, x, y));
                const bool hoverUndoFloating = (hasSel && isPointInRectF(self->m_btnUndoFloatingRect, x, y));
                int hoveredTrBtnId = -1;
                bool hoverReplaceDragHandle = false;
                if (self->m_editingTextReplaceIdx >= 0) {
                    if (isPointInRectF(self->m_btnTrFontDecRect, x, y)) hoveredTrBtnId = kPinBtnTrFontDec;
                    else if (isPointInRectF(self->m_btnTrFontIncRect, x, y)) hoveredTrBtnId = kPinBtnTrFontInc;
                    else if (isPointInRectF(self->m_btnTrWeightRect, x, y)) hoveredTrBtnId = kPinBtnTrWeight;
                    else if (isPointInRectF(self->m_btnTrFamilyRect, x, y)) hoveredTrBtnId = kPinBtnTrFamily;
                    else if (isPointInRectF(self->m_btnTrBgModeRect, x, y)) hoveredTrBtnId = kPinBtnTrBgMode;
                    else if (isPointInRectF(self->m_btnTrReflowRect, x, y)) hoveredTrBtnId = kPinBtnTrReflow;
                    else if (isPointInRectF(self->m_btnTrDoneRect, x, y)) hoveredTrBtnId = kPinBtnTrDone;

                    if (hoveredTrBtnId < 0) {
                        const float safeDpi = (self->m_dpiScale > 0.0f) ? self->m_dpiScale : 1.0f;
                        const D2D1_RECT_F gRc = self->m_trDragHandleRect;
                        const float gPad = 3.0f * safeDpi;
                        if (gRc.right > gRc.left &&
                            static_cast<float>(x) >= gRc.left - gPad && static_cast<float>(x) <= gRc.right + gPad &&
                            static_cast<float>(y) >= gRc.top - gPad && static_cast<float>(y) <= gRc.bottom + gPad) {
                            hoverReplaceDragHandle = true;
                        } else {
                            const D2D1_RECT_F bRc = self->m_trEditBoxClientRect;
                            if (bRc.right > bRc.left && bRc.bottom > bRc.top) {
                                const float outPad = 5.5f * safeDpi;
                                const float inPad = (std::min)(3.0f * safeDpi, (bRc.bottom - bRc.top) * 0.22f);
                                const bool inOuter =
                                    static_cast<float>(x) >= bRc.left - outPad && static_cast<float>(x) <= bRc.right + outPad &&
                                    static_cast<float>(y) >= bRc.top - outPad && static_cast<float>(y) <= bRc.bottom + outPad;
                                const bool inInner =
                                    static_cast<float>(x) >= bRc.left + inPad && static_cast<float>(x) <= bRc.right - inPad &&
                                    static_cast<float>(y) >= bRc.top + inPad && static_cast<float>(y) <= bRc.bottom - inPad;
                                if (inOuter && (!inInner || ((GetKeyState(VK_MENU) & 0x8000) != 0))) {
                                    hoverReplaceDragHandle = true;
                                }
                            }
                        }
                    }
                }
                int hoverColorDotIdx = -1;
                if (hasSel) {
                    for (size_t ci = 0; ci < self->m_btnColorDotFloatingRects.size(); ++ci) {
                        if (isPointInRectF(self->m_btnColorDotFloatingRects[ci], x, y)) {
                            hoverColorDotIdx = static_cast<int>(ci);
                            break;
                        }
                    }
                }

                int hoverQrIdx = -1;
                {
                    std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                    for (size_t q = 0; q < self->m_qrCodes.size(); ++q) {
                        if (isPointInRectF(self->m_qrCodes[q].badgeRect, x, y)) {
                            hoverQrIdx = static_cast<int>(q);
                            break;
                        }
                    }
                }
                
                if (hoverGroupBadge != self->m_hoverGroupBadge ||
                    hoverFold != self->m_hoverFold || hoverRotate != self->m_hoverRotate ||
                    hoverInvert != self->m_hoverInvert || hoverEdit != self->m_hoverEdit ||
                    hoverText != self->m_hoverText || hoverTranslate != self->m_hoverTranslate ||
                    hoverCopy != self->m_hoverCopy ||
                    hoverSave != self->m_hoverSave || hoverClose != self->m_hoverClose ||
                    hoverTranslateStatus != self->m_hoverTranslateStatus ||
                    hoverCopyFloating != self->m_hoverCopyFloating ||
                    hoverCopyTableFloating != self->m_hoverCopyTableFloating ||
                    hoverOpenPanelFloating != self->m_hoverOpenPanelFloating ||
                    hoverHighlightFloating != self->m_hoverHighlightFloating ||
                    hoverWavyFloating != self->m_hoverWavyFloating ||
                    hoverUnderlineFloating != self->m_hoverUnderlineFloating ||
                    hoverTextReplaceFloating != self->m_hoverTextReplaceFloating ||
                    hoverTranslateFloating != self->m_hoverTranslateFloating ||
                    hoverUndoFloating != self->m_hoverUndoFloating ||
                    hoveredTrBtnId != self->m_hoveredTrBtnId ||
                    hoverReplaceDragHandle != self->m_hoverReplaceDragHandle ||
                    hoverColorDotIdx != self->m_hoveredColorDotIndex ||
                    hoverQrIdx != self->m_hoveredQrBadgeIndex) {
                    self->m_hoverGroupBadge = hoverGroupBadge;
                    self->m_hoverFold = hoverFold;
                    self->m_hoverRotate = hoverRotate;
                    self->m_hoverInvert = hoverInvert;
                    self->m_hoverEdit = hoverEdit;
                    self->m_hoverText = hoverText;
                    self->m_hoverTranslate = hoverTranslate;
                    self->m_hoverCopy = hoverCopy;
                    self->m_hoverSave = hoverSave;
                    self->m_hoverClose = hoverClose;
                    self->m_hoverTranslateStatus = hoverTranslateStatus;
                    self->m_hoverCopyFloating = hoverCopyFloating;
                    self->m_hoverCopyTableFloating = hoverCopyTableFloating;
                    self->m_hoverOpenPanelFloating = hoverOpenPanelFloating;
                    self->m_hoverHighlightFloating = hoverHighlightFloating;
                    self->m_hoverWavyFloating = hoverWavyFloating;
                    self->m_hoverUnderlineFloating = hoverUnderlineFloating;
                    self->m_hoverTextReplaceFloating = hoverTextReplaceFloating;
                    self->m_hoverTranslateFloating = hoverTranslateFloating;
                    self->m_hoverUndoFloating = hoverUndoFloating;
                    self->m_hoveredTrBtnId = hoveredTrBtnId;
                    self->m_hoverReplaceDragHandle = hoverReplaceDragHandle;
                    self->m_hoveredColorDotIndex = hoverColorDotIdx;
                    self->m_hoveredQrBadgeIndex = hoverQrIdx;
                    self->render();
                }

                // 正在原图流式逐字拖选或拉框划选文字
                if (self->m_isSelectingText) {
                    self->m_selectEnd = {x, y};
                    const float safeScale = (std::max)(0.05f, self->m_scale);

                    if (self->m_isCharStreamSelecting && self->m_anchorBlockIndex >= 0) {
                        const float startMx = static_cast<float>(self->m_selectStart.x) / safeScale;
                        const float curMx = static_cast<float>(x) / safeScale;
                        const float curMy = static_cast<float>(y) / safeScale;
                        const float moveClientPx = std::hypot(
                            static_cast<float>(x - self->m_selectStart.x),
                            static_cast<float>(y - self->m_selectStart.y));

                        if (moveClientPx >= 1.5f) {
                            std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                            if (static_cast<size_t>(self->m_anchorBlockIndex) < self->m_ocrBlocks.size()) {
                                const auto& anchorBlk = self->m_ocrBlocks[static_cast<size_t>(self->m_anchorBlockIndex)];
                                const int anchorRegId = anchorBlk.layoutRegionId;
                                const float startMy = static_cast<float>(self->m_selectStart.y) / safeScale;

                                // 计算起点所在版面分区的水平包络，判断光标是否仍在该分区内操作
                                float regMinX = static_cast<float>(anchorBlk.origBox.x);
                                float regMaxX = static_cast<float>(anchorBlk.origBox.x + anchorBlk.origBox.width);
                                for (const auto& rb : self->m_ocrBlocks) {
                                    if (rb.layoutRegionId == anchorRegId) {
                                        regMinX = (std::min)(regMinX, static_cast<float>(rb.origBox.x));
                                        regMaxX = (std::max)(regMaxX, static_cast<float>(rb.origBox.x + rb.origBox.width));
                                    }
                                }
                                const float anchorH = static_cast<float>((std::max)(10, anchorBlk.origBox.height));
                                const bool cursorInAnchorRegionX =
                                    (curMx >= regMinX - anchorH * 2.5f && curMx <= regMaxX + anchorH * 2.5f);

                                // 寻找当前鼠标所在的阅读块（优先在同分区内匹配，防止同高度远端分栏抢夺焦点）
                                int targetBlock = -1;
                                float bestScore = FLT_MAX;
                                for (int pass = 0; pass < 2 && targetBlock < 0; ++pass) {
                                    const bool sameRegOnly = (pass == 0 && cursorInAnchorRegionX);
                                    if (pass == 0 && !cursorInAnchorRegionX) continue;
                                    for (size_t b = 0; b < self->m_ocrBlocks.size(); ++b) {
                                        const auto& cand = self->m_ocrBlocks[b];
                                        if (sameRegOnly && cand.layoutRegionId != anchorRegId) continue;
                                        const auto& ob = cand.origBox;
                                        const float padY = 4.0f;
                                        if (curMy >= static_cast<float>(ob.y) - padY &&
                                            curMy <= static_cast<float>(ob.y + ob.height) + padY) {
                                            float dx = 0.0f;
                                            if (curMx < static_cast<float>(ob.x)) dx = static_cast<float>(ob.x) - curMx;
                                            else if (curMx > static_cast<float>(ob.x + ob.width)) dx = curMx - static_cast<float>(ob.x + ob.width);
                                            if (dx < bestScore) {
                                                bestScore = dx;
                                                targetBlock = static_cast<int>(b);
                                            }
                                        }
                                    }
                                }
                                if (targetBlock < 0) {
                                    for (size_t b = 0; b < self->m_ocrBlocks.size(); ++b) {
                                        const auto& cand = self->m_ocrBlocks[b];
                                        if (cursorInAnchorRegionX && cand.layoutRegionId != anchorRegId) continue;
                                        const auto& ob = cand.origBox;
                                        const float cy = static_cast<float>(ob.y) + static_cast<float>(ob.height) * 0.5f;
                                        const float dy = std::abs(curMy - cy);
                                        if (dy < bestScore) {
                                            bestScore = dy;
                                            targetBlock = static_cast<int>(b);
                                        }
                                    }
                                }
                                if (targetBlock < 0) {
                                    for (size_t b = 0; b < self->m_ocrBlocks.size(); ++b) {
                                        const auto& ob = self->m_ocrBlocks[b].origBox;
                                        const float cy = static_cast<float>(ob.y) + static_cast<float>(ob.height) * 0.5f;
                                        const float dy = std::abs(curMy - cy);
                                        if (dy < bestScore) {
                                            bestScore = dy;
                                            targetBlock = static_cast<int>(b);
                                        }
                                    }
                                }

                                std::vector<size_t> newIndices;
                                std::vector<PinBlockCharSpan> newSpans;
                                self->m_showCrossRegionRectGuide = false;

                                if (targetBlock == self->m_anchorBlockIndex) {
                                    const auto& blk = self->m_ocrBlocks[static_cast<size_t>(targetBlock)];
                                    const float minMx = (std::min)(startMx, curMx);
                                    const float maxMx = (std::max)(startMx, curMx);
                                    size_t firstHit = blk.charBoxes.size();
                                    size_t lastHit = 0;
                                    for (size_t c = 0; c < blk.charBoxes.size(); ++c) {
                                        const auto& cb = blk.charBoxes[c];
                                        const float entryThresh = static_cast<float>(cb.x) + static_cast<float>(cb.width) * 0.28f;
                                        const float exitThresh = static_cast<float>(cb.x) + static_cast<float>(cb.width) * 0.72f;
                                        if (entryThresh <= maxMx && exitThresh >= minMx) {
                                            firstHit = (std::min)(firstHit, c);
                                            lastHit = (std::max)(lastHit, c + 1);
                                        }
                                    }
                                    if (firstHit >= lastHit && !blk.charBoxes.empty()) {
                                        const size_t fallbackC = (std::min)(self->m_anchorCharIndex, blk.charBoxes.size() - 1);
                                        firstHit = fallbackC;
                                        lastHit = fallbackC + 1;
                                    }
                                    if (firstHit < lastHit) {
                                        newIndices.push_back(static_cast<size_t>(targetBlock));
                                        newSpans.push_back(PinBlockCharSpan{static_cast<size_t>(targetBlock), firstHit, lastHit});
                                    }
                                } else if (targetBlock >= 0 &&
                                           self->m_ocrBlocks[static_cast<size_t>(targetBlock)].layoutRegionId == anchorRegId) {
                                    // 同版面分区内流式选字：严格限制在当前 layoutRegionId 内，并排除旁侧水平脱节的错位孤立标签
                                    const size_t caretChar = locateNearestCharInBlock(
                                        self->m_ocrBlocks[static_cast<size_t>(targetBlock)], curMx);
                                    int b1 = self->m_anchorBlockIndex;
                                    size_t c1 = self->m_anchorCharIndex;
                                    int b2 = targetBlock;
                                    size_t c2 = caretChar;
                                    if (b1 > b2) {
                                        std::swap(b1, b2);
                                        std::swap(c1, c2);
                                    }

                                    const auto& blk1 = self->m_ocrBlocks[static_cast<size_t>(b1)];
                                    const auto& blk2 = self->m_ocrBlocks[static_cast<size_t>(b2)];
                                    const float padX = static_cast<float>((std::max)(blk1.origBox.height, blk2.origBox.height)) * 1.5f;
                                    const float activeMinX = (std::min)({
                                        static_cast<float>(blk1.origBox.x),
                                        static_cast<float>(blk2.origBox.x),
                                        startMx, curMx
                                    }) - padX;
                                    const float activeMaxX = (std::max)({
                                        static_cast<float>(blk1.origBox.x + blk1.origBox.width),
                                        static_cast<float>(blk2.origBox.x + blk2.origBox.width),
                                        startMx, curMx
                                    }) + padX;

                                    for (int b = b1; b <= b2; ++b) {
                                        const auto& blk = self->m_ocrBlocks[static_cast<size_t>(b)];
                                        if (blk.layoutRegionId != anchorRegId) continue;

                                        if (b != b1 && b != b2 &&
                                            blk.alignedRowId != blk1.alignedRowId &&
                                            blk.alignedRowId != blk2.alignedRowId) {
                                            const float bx0 = static_cast<float>(blk.origBox.x);
                                            const float bx1 = static_cast<float>(blk.origBox.x + blk.origBox.width);
                                            if (bx1 < activeMinX || bx0 > activeMaxX) {
                                                // 检查该中间块是否为单块孤立行（如左侧垂直居中的“备注”标签），若是则不夹带进右侧多行输入框选区
                                                size_t rowPeers = 0;
                                                for (const auto& p : self->m_ocrBlocks) {
                                                    if (p.layoutRegionId == anchorRegId && p.alignedRowId == blk.alignedRowId) {
                                                        ++rowPeers;
                                                    }
                                                }
                                                if (rowPeers <= 1) continue;
                                            }
                                        }

                                        const size_t totalC = blk.charBoxes.size();
                                        if (totalC == 0) continue;
                                        size_t sc = 0;
                                        size_t ec = totalC;
                                        if (b == b1) sc = (std::min)(c1, totalC - 1);
                                        if (b == b2) ec = (std::min)(totalC, c2 + 1);
                                        if (sc < ec) {
                                            newIndices.push_back(static_cast<size_t>(b));
                                            newSpans.push_back(PinBlockCharSpan{static_cast<size_t>(b), sc, ec});
                                        }
                                    }
                                } else if (targetBlock >= 0) {
                                    // 判断起止分区是否为真正的「左右并排双栏」：
                                    // - 仅左右并排双栏（垂直显著重叠且水平分离）采用带可见引导框的二维围选，防止跨栏误卷无关列；
                                    // - 上下堆叠分段（如上方表格 + 下方装箱卡/标题 + 正文）维持自然阅读序流式选字，绝不静默腰斩右侧文字！
                                    const int targetRegId = self->m_ocrBlocks[static_cast<size_t>(targetBlock)].layoutRegionId;
                                    auto computeRegionBounds = [&](int rid) {
                                        float rx1 = FLT_MAX, ry1 = FLT_MAX, rx2 = -FLT_MAX, ry2 = -FLT_MAX;
                                        for (const auto& rb : self->m_ocrBlocks) {
                                            if (rb.layoutRegionId == rid) {
                                                rx1 = (std::min)(rx1, static_cast<float>(rb.origBox.x));
                                                ry1 = (std::min)(ry1, static_cast<float>(rb.origBox.y));
                                                rx2 = (std::max)(rx2, static_cast<float>(rb.origBox.x + rb.origBox.width));
                                                ry2 = (std::max)(ry2, static_cast<float>(rb.origBox.y + rb.origBox.height));
                                            }
                                        }
                                        return std::array<float, 4>{rx1, ry1, rx2, ry2};
                                    };
                                    const auto aB = computeRegionBounds(anchorRegId);
                                    const auto tB = computeRegionBounds(targetRegId);
                                    const float aW = (std::max)(1.0f, aB[2] - aB[0]);
                                    const float aH = (std::max)(1.0f, aB[3] - aB[1]);
                                    const float tW = (std::max)(1.0f, tB[2] - tB[0]);
                                    const float tH = (std::max)(1.0f, tB[3] - tB[1]);
                                    const float overlapX = (std::max)(0.0f, (std::min)(aB[2], tB[2]) - (std::max)(aB[0], tB[0]));
                                    const float overlapY = (std::max)(0.0f, (std::min)(aB[3], tB[3]) - (std::max)(aB[1], tB[1]));
                                    const bool sideBySideColumns =
                                        (overlapY / (std::min)(aH, tH) >= 0.40f) &&
                                        (overlapX / (std::min)(aW, tW) < 0.25f);

                                    if (sideBySideColumns) {
                                        self->m_showCrossRegionRectGuide = true;
                                        const float sx1 = (std::min)(startMx, curMx);
                                        const float sy1 = (std::min)(startMy, curMy);
                                        const float sx2 = (std::max)(startMx, curMx);
                                        const float sy2 = (std::max)(startMy, curMy);
                                        for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                                            const auto& blk = self->m_ocrBlocks[i];
                                            const auto& b = blk.origBox;
                                            if (static_cast<float>(b.x + b.width) < sx1 ||
                                                static_cast<float>(b.x) > sx2 ||
                                                static_cast<float>(b.y + b.height) < sy1 ||
                                                static_cast<float>(b.y) > sy2) {
                                                continue;
                                            }
                                            size_t firstC = blk.charBoxes.size();
                                            size_t lastC = 0;
                                            for (size_t c = 0; c < blk.charBoxes.size(); ++c) {
                                                const auto& cb = blk.charBoxes[c];
                                                const float cx = static_cast<float>(cb.x) + static_cast<float>(cb.width) * 0.5f;
                                                if (cx >= sx1 && cx <= sx2 &&
                                                    static_cast<float>(cb.y + cb.height) >= sy1 &&
                                                    static_cast<float>(cb.y) <= sy2) {
                                                    firstC = (std::min)(firstC, c);
                                                    lastC = (std::max)(lastC, c + 1);
                                                }
                                            }
                                            if (firstC < lastC) {
                                                newIndices.push_back(i);
                                                newSpans.push_back(PinBlockCharSpan{i, firstC, lastC});
                                            }
                                        }
                                    } else {
                                        // 上下堆叠跨区流式选字：按阅读序自然跨过全行内容，右侧字符完整保留
                                        const size_t caretChar = locateNearestCharInBlock(
                                            self->m_ocrBlocks[static_cast<size_t>(targetBlock)], curMx);
                                        int b1 = self->m_anchorBlockIndex;
                                        size_t c1 = self->m_anchorCharIndex;
                                        int b2 = targetBlock;
                                        size_t c2 = caretChar;
                                        if (b1 > b2) {
                                            std::swap(b1, b2);
                                            std::swap(c1, c2);
                                        }
                                        for (int b = b1; b <= b2; ++b) {
                                            const auto& blk = self->m_ocrBlocks[static_cast<size_t>(b)];
                                            const size_t totalC = blk.charBoxes.size();
                                            if (totalC == 0) continue;
                                            size_t sc = 0;
                                            size_t ec = totalC;
                                            if (b == b1) sc = (std::min)(c1, totalC - 1);
                                            if (b == b2) ec = (std::min)(totalC, c2 + 1);
                                            if (sc < ec) {
                                                newIndices.push_back(static_cast<size_t>(b));
                                                newSpans.push_back(PinBlockCharSpan{static_cast<size_t>(b), sc, ec});
                                            }
                                        }
                                    }
                                }

                                self->m_selectedBlockIndices = std::move(newIndices);
                                self->m_selectedCharSpans = std::move(newSpans);
                            }
                        }
                    } else {
                        // Alt 矩形拉框选字：精确截取落入拉框内的字符子包围盒
                        const float sx1 = static_cast<float>((std::min)(self->m_selectStart.x, self->m_selectEnd.x)) / safeScale;
                        const float sy1 = static_cast<float>((std::min)(self->m_selectStart.y, self->m_selectEnd.y)) / safeScale;
                        const float sx2 = static_cast<float>((std::max)(self->m_selectStart.x, self->m_selectEnd.x)) / safeScale;
                        const float sy2 = static_cast<float>((std::max)(self->m_selectStart.y, self->m_selectEnd.y)) / safeScale;

                        std::vector<size_t> newIndices;
                        std::vector<PinBlockCharSpan> newSpans;
                        {
                            std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                            for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                                const auto& blk = self->m_ocrBlocks[i];
                                const auto& b = blk.origBox;
                                const bool blockOverlap = !(static_cast<float>(b.x + b.width) < sx1 ||
                                                            static_cast<float>(b.x) > sx2 ||
                                                            static_cast<float>(b.y + b.height) < sy1 ||
                                                            static_cast<float>(b.y) > sy2);
                                if (!blockOverlap) continue;

                                if (blk.charBoxes.empty()) {
                                    newIndices.push_back(i);
                                    continue;
                                }

                                size_t firstC = blk.charBoxes.size();
                                size_t lastC = 0;
                                for (size_t c = 0; c < blk.charBoxes.size(); ++c) {
                                    const auto& cb = blk.charBoxes[c];
                                    const float cx = static_cast<float>(cb.x) + static_cast<float>(cb.width) * 0.5f;
                                    const float cy = static_cast<float>(cb.y) + static_cast<float>(cb.height) * 0.5f;
                                    const bool charHit = (cx >= sx1 && cx <= sx2 &&
                                                          static_cast<float>(cb.y + cb.height) >= sy1 &&
                                                          static_cast<float>(cb.y) <= sy2) ||
                                                         (sx2 - sx1 < 3.0f && sy2 - sy1 < 3.0f &&
                                                          cb.contains(cv::Point(static_cast<int>(sx1), static_cast<int>(sy1))));
                                    (void)cy;
                                    if (charHit) {
                                        firstC = (std::min)(firstC, c);
                                        lastC = (std::max)(lastC, c + 1);
                                    }
                                }
                                if (firstC < lastC) {
                                    newIndices.push_back(i);
                                    newSpans.push_back(PinBlockCharSpan{i, firstC, lastC});
                                }
                            }
                        }
                        self->m_selectedBlockIndices = std::move(newIndices);
                        self->m_selectedCharSpans = std::move(newSpans);
                    }
                    self->render();
                    return 0;
                }

                // 零门槛 Live Text 智能悬停检测（无需按住 Alt 或切模式，指向文字自动变 IDC_IBEAM）
                const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                const bool liveEnabled = self->isLiveTextEnabled();
                if ((alt || self->m_textSelectionMode) && !self->m_ocrReady.load()) {
                    self->ensureOcrExecutedAsync(false);
                } else if (!leftHeld && liveEnabled && !self->m_ocrReady.load() && !self->m_clickThrough) {
                    const bool allowHoverPreload = !isAutomatedTestProcessMode() ||
                        tools3000::core::ConfigManager::instance().get<bool>("/ocr/preloadOnHoverInTest", false);
                    if (allowHoverPreload) {
                        self->ensureOcrExecutedAsync(true);
                    }
                }

                if ((alt || self->m_textSelectionMode || liveEnabled) && self->m_ocrReady.load()) {
                    int hovered = -1;
                    {
                        std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                        const float safeScale = (std::max)(0.05f, self->m_scale);
                        const float mx = static_cast<float>(x) / safeScale;
                        const float my = static_cast<float>(y) / safeScale;
                        const cv::Point pt(static_cast<int>(mx), static_cast<int>(my));
                        for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                            if (self->m_ocrBlocks[i].origBox.contains(pt)) {
                                hovered = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    if (hovered != self->m_hoveredBlockIndex) {
                        self->m_hoveredBlockIndex = hovered;
                        self->render();
                    }
                } else if (self->m_hoveredBlockIndex >= 0) {
                    self->m_hoveredBlockIndex = -1;
                    self->render();
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (!self) break;
            self->m_isHovering = false;
            self->m_hoverTime = GetTickCount64();
            self->m_hoverGroupBadge = false;
            self->m_hoverFold = false;
            self->m_hoverRotate = false;
            self->m_hoverInvert = false;
            self->m_hoverEdit = false;
            self->m_hoverText = false;
            self->m_hoverTranslate = false;
            self->m_hoverCopy = false;
            self->m_hoverSave = false;
            self->m_hoverClose = false;
            self->m_hoverTranslateStatus = false;
            self->m_hoverCopyFloating = false;
            self->m_hoverCopyTableFloating = false;
            self->m_hoverOpenPanelFloating = false;
            self->m_hoverHighlightFloating = false;
            self->m_hoverWavyFloating = false;
            self->m_hoverUnderlineFloating = false;
            self->m_hoverTextReplaceFloating = false;
            self->m_hoverTranslateFloating = false;
            self->m_hoverUndoFloating = false;
            self->m_hoveredTrBtnId = -1;
            self->m_hoverReplaceDragHandle = false;
            self->m_hoveredColorDotIndex = -1;
            self->m_hoveredQrBadgeIndex = -1;
            self->m_hoveredBlockIndex = -1;
            SetTimer(hwnd, 1, 16, nullptr);
            return 0;
        }

        case WM_TIMER: {
            if (self && wParam == 1) {
                self->updateHoverAnimation();
            } else if (self && wParam == 2) {
                if (GetTickCount64() >= self->m_hudBadgeUntilTick) {
                    KillTimer(hwnd, 2);
                    self->m_hudBadgeText.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (self && wParam == 3) {
                if (self->m_editingTextReplaceIdx >= 0) {
                    self->render();
                } else {
                    KillTimer(hwnd, 3);
                }
            } else if (self && wParam == PinWindow::kTimerIdZoomAnim) {
                self->stepZoomAnimation();
            } else if (self && wParam == PinWindow::kTimerIdZoomStageIdleRelease) {
                KillTimer(hwnd, PinWindow::kTimerIdZoomStageIdleRelease);
                if (!self->m_zoomAnimating && !self->m_zoomStageActive) {
                    self->releaseZoomStage();
                }
            }
            return 0;
        }

        case WM_CANCELMODE:
        case WM_CAPTURECHANGED: {
            if (self) {
                self->m_pressedToolbarBtn = kPinBtnNone;
                self->m_isDragging = false;
                self->m_dragThresholdExceeded = false;
                self->m_isSelectingText = false;
                self->m_isSelectingReplaceText = false;
                self->m_isDraggingReplacePos = false;
                self->m_isCharStreamSelecting = false;
                self->m_showCrossRegionRectGuide = false;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (self) {
                const int upX = GET_X_LPARAM(lParam);
                const int upY = GET_Y_LPARAM(lParam);
                if (self->m_isDraggingReplacePos) {
                    self->m_isDraggingReplacePos = false;
                    ReleaseCapture();
                    self->updateImeCompositionAnchor();
                    self->render();
                    return 0;
                }
                if (self->m_isSelectingReplaceText) {
                    self->m_isSelectingReplaceText = false;
                    ReleaseCapture();
                    self->render();
                    return 0;
                }
                if (self->m_isSelectingText) {
                    self->m_isSelectingText = false;
                    self->m_isCharStreamSelecting = false;
                    self->m_showCrossRegionRectGuide = false;
                    ReleaseCapture();
                    self->render();
                    return 0;
                }
                const int pressedBtn = self->m_pressedToolbarBtn;
                const bool wasDragging = self->m_isDragging || self->m_dragThresholdExceeded;
                self->m_pressedToolbarBtn = kPinBtnNone;
                self->m_isDragging = false;
                self->m_dragThresholdExceeded = false;
                ReleaseCapture();

                const float clickSlopPx = 8.0f * ((self->m_dpiScale > 0.0f) ? self->m_dpiScale : 1.0f);
                if (!wasDragging && pressedBtn != kPinBtnNone &&
                    self->isInteractiveButtonHitWithSlop(pressedBtn, upX, upY, clickSlopPx)) {
                    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    self->executeInteractiveButton(pressedBtn, upX, upY, shift);
                }
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            // 双击：若折叠则展开；若点在按钮上走连击；若点在文字热区上自动双击选词；空白处进入标注编辑模式
            if (self) {
                if (self->m_zoomAnimating || self->m_zoomStageActive) {
                    self->stopZoomAnimation(true);
                }
                if (self->m_folded) {
                    self->toggleFold();
                } else {
                    const int dblX = GET_X_LPARAM(lParam);
                    const int dblY = GET_Y_LPARAM(lParam);
                    const int hitBtn = self->hitTestInteractiveButton(dblX, dblY);
                    if (hitBtn != kPinBtnNone) {
                        self->m_pressedToolbarBtn = hitBtn;
                        self->m_pressStartClientPt = {dblX, dblY};
                        self->m_dragOffset = {dblX, dblY};
                        self->m_dragThresholdExceeded = false;
                        self->m_isDragging = false;
                        SetCapture(hwnd);
                        return 0;
                    }
                    // 双击已修改过的原位文字图层 -> 立即无缝重新进入二次编辑
                    if (self->m_editingTextReplaceIdx < 0 && !self->m_markupElements.empty()) {
                        const float safeScale = (std::max)(0.05f, self->m_scale);
                        const cv::Point imgPt(
                            static_cast<int>(std::lround(static_cast<float>(dblX) / safeScale)),
                            static_cast<int>(std::lround(static_cast<float>(dblY) / safeScale)));
                        for (int i = static_cast<int>(self->m_markupElements.size()) - 1; i >= 0; --i) {
                            const auto& el = self->m_markupElements[static_cast<size_t>(i)];
                            if (el.tool != MarkupTool::TextReplace) continue;
                            const cv::Rect hitRc = (el.replaceLastRenderRect.width > 0 && el.replaceLastRenderRect.height > 0)
                                ? el.replaceLastRenderRect
                                : el.replaceSourceRect;
                            const cv::Rect expanded(hitRc.x - 4, hitRc.y - 4, hitRc.width + 8, hitRc.height + 8);
                            if (expanded.contains(imgPt)) {
                                self->startInPlaceTextReplaceOnElement(i);
                                return 0;
                            }
                        }
                    }
                    const auto configuredDbl = PinWindow::parseDoubleClickAction(
                        tools3000::core::ConfigManager::instance().get<std::string>(
                            "/capture/pinDoubleClickAction", "edit"));
                    const auto effectiveDbl = PinWindow::resolveEffectiveDoubleClickAction(
                        configuredDbl, self->m_folded, self->m_editingTextReplaceIdx >= 0);
                    if (effectiveDbl == PinWindow::DoubleClickAction::Close) {
                        self->close();
                        return 0;
                    }
                    if (effectiveDbl == PinWindow::DoubleClickAction::Fold) {
                        self->toggleFold();
                        return 0;
                    }
                    if (self->selectWordAtClientPoint(dblX, dblY)) {
                        return 0;
                    }
                    self->editMarkup();
                }
            }
            return 0;
        }

        case WM_MBUTTONUP: {
            // 鼠标中键单击：默认恢复 1:1 原始物理像素尺寸，支持按用户偏好配置为关闭或折叠（原位改字编辑中防误关）
            if (self) {
                if (self->m_zoomAnimating || self->m_zoomStageActive) {
                    self->stopZoomAnimation(false);
                }
                const auto configuredMid = PinWindow::parseMiddleClickAction(
                    tools3000::core::ConfigManager::instance().get<std::string>(
                        "/capture/pinMiddleClickAction", "reset_scale"));
                const auto effectiveMid = PinWindow::resolveEffectiveMiddleClickAction(
                    configuredMid, self->m_editingTextReplaceIdx >= 0);
                if (effectiveMid == PinWindow::MiddleClickAction::Close) {
                    self->close();
                } else if (effectiveMid == PinWindow::MiddleClickAction::Fold) {
                    self->toggleFold();
                } else {
                    self->focusWindow();
                    self->resetScale();
                }
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!self) break;
            self->handleMouseWheel(wParam, lParam);
            return 0;
        }

        case WM_RBUTTONUP: {
            // Direct2D 微晶自绘右键复合菜单（顶栏 6 枚快捷图标 + 实时不透明度滑块 + 精简列表）
            if (!self) break;
            if (self->m_zoomAnimating || self->m_zoomStageActive) {
                self->stopZoomAnimation(true);
            }
            self->focusWindow();

            POINT pt{};
            GetCursorPos(&pt);
            std::weak_ptr<PinWindow> weakSelf = self->weak_from_this();
            const PinContextMenuSnapshot snapshot = buildPinContextMenuSnapshot(self);

            const PinMenuCommand cmd = PinContextMenu::instance().showContextMenu(
                hwnd,
                pt,
                snapshot,
                [weakSelf](float liveOpacity) {
                    if (auto locked = weakSelf.lock(); locked && locked->isAlive()) {
                        locked->setOpacity(liveOpacity);
                    }
                },
                [weakSelf]() {
                    const auto locked = weakSelf.lock();
                    return locked && locked->isAlive() && locked->hwnd() && IsWindow(locked->hwnd());
                });

            if (auto aliveSelf = weakSelf.lock(); aliveSelf && aliveSelf->isAlive()) {
                executePinContextMenuCommand(aliveSelf.get(), aliveSelf->hwnd(), cmd);
            }
            return 0;
        }

        case WM_DPICHANGED: {
            if (!self) break;
            if (self->m_zoomAnimating || self->m_zoomStageActive) {
                self->stopZoomAnimation(true);
            }
            self->releaseZoomStage();
            const UINT newDpi = HIWORD(wParam);
            self->m_dpiScale = tools3000::core::dpi::scaleForDpi(newDpi);
            self->m_cachedTextScale = 0.0f;
            self->m_uiLeadTextFormat.Reset();
            self->m_uiCenterTextFormat.Reset();
            if (self->m_folded) {
                RECT curRc{};
                GetWindowRect(hwnd, &curRc);
                if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
                    curRc.left = suggested->left;
                    curRc.top = suggested->top;
                }
                const SIZE foldSz = self->computeFoldCapsuleSize(self->m_dpiScale);
                SetWindowPos(hwnd, nullptr, curRc.left, curRc.top, foldSz.cx, foldSz.cy,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                if (self->m_renderTarget) {
                    self->m_renderTarget->Resize(D2D1::SizeU(
                        static_cast<UINT>(foldSz.cx), static_cast<UINT>(foldSz.cy)));
                }
                tools3000::core::WinUtils::applyUniversalRoundedCorners(
                    hwnd, foldSz.cx, foldSz.cy,
                    static_cast<int>(std::round(8.0f * self->m_dpiScale)));
            } else if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                self->updateWindowCornerRegion();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_PAINT: {
            if (self) self->render();
            ValidateRect(hwnd, nullptr);
            return 0;
        }

        case WM_NCDESTROY: {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (self) {
                self->stopZoomAnimation(false);
                if (self->m_hwnd == hwnd) {
                    self->m_hwnd = nullptr;
                }
                if (self->m_pinInstanceId != 0) {
                    PinOcrScheduler::instance().cancelPending(self->m_pinInstanceId);
                }
                self->clearTranslationState();
                self->releaseGraphicsResources();
                {
                    std::lock_guard<std::mutex> vLock(self->m_imageVaultMutex);
                    self->m_sourceImage.release();
                    self->m_cleanBaseImage.release();
                    self->m_sourceVault.clear();
                    self->m_cleanBaseVault.clear();
                    self->m_vaultSharesCleanWithSource = false;
                }
                s_instances.erase(
                    std::remove_if(s_instances.begin(), s_instances.end(),
                                   [self](const auto& p) { return !p || p.get() == self || !p->isAlive(); }),
                    s_instances.end());
            }
            break;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace tools3000::capture

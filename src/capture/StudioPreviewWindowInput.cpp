// ─────────────────────────────────────────────────────────────────────────────
// StudioPreviewWindowInput.cpp — 演播室滑块/运镜预置、鼠标拖拽热区命中、键盘输入与 Win32 消息循环子模块
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioPreviewWindowInternal.h"
#include "capture/StudioBackdropCatalog.h"
#include "../resource.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "ui/KeyboardPipeline.h"

#include <opencv2/imgproc.hpp>
#include <d2d1_1.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

namespace tools3000::capture {

using Microsoft::WRL::ComPtr;
using namespace studio_detail;

void StudioPreviewWindow::applySliderAtX(SliderDragKind kind, float lx) {
    const auto& trk = m_sliderTrackRects[static_cast<size_t>(kind)];
    const float w = (std::max)(1.0f, trk.right - trk.left);
    const float ratio = std::clamp((lx - trk.left) / w, 0.0f, 1.0f);

    if (kind == SliderDragKind::Padding) {
        m_config.paddingRatio = ratio * 0.22f;
        m_config.autoFrameEnabled = true;
    } else if (kind == SliderDragKind::InsetBorder) {
        m_config.insetBorderWidth = std::round(ratio * 16.0f);
        m_config.autoFrameEnabled = true;
    } else if (kind == SliderDragKind::Radius) {
        m_config.cornerRadius = std::round(ratio * 36.0f);
        m_config.autoFrameEnabled = true;
    } else if (kind == SliderDragKind::Shadow) {
        m_config.shadowIntensity = ratio;
        m_config.autoFrameEnabled = true;
    } else if (kind == SliderDragKind::BgBlur) {
        m_config.backgroundBlur = std::round(ratio * 40.0f);
        m_config.autoFrameEnabled = true;
    } else if (kind == SliderDragKind::WatermarkScale) {
        m_config.watermark.scale = 0.35f + ratio * (2.80f - 0.35f);
        m_config.watermark.enabled = true;
        WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
    } else if (kind == SliderDragKind::WatermarkOpacity) {
        m_config.watermark.opacity = 0.04f + ratio * (1.00f - 0.04f);
        m_config.watermark.enabled = true;
        WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
    } else if (kind == SliderDragKind::ZoomFactor) {
        const float rawZ = 1.00f + ratio * 1.60f;
        const float snappedZ = std::clamp(std::round(rawZ * 20.0f) / 20.0f, 1.00f, 2.60f);
        m_config.autoZoomEnabled = true;
        if (m_config.zoomMode == StudioZoomMode::None) {
            m_config.zoomMode = StudioZoomMode::Zoom2D;
        }
        if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
            auto& seg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
            seg.targetZoom = snappedZ;
            seg.manualInserted = true;
        } else {
            m_config.defaultZoomFactor = snappedZ;
            for (auto& seg : m_timeline.zoomSegments) {
                if (!seg.manualInserted) {
                    seg.targetZoom = snappedZ;
                }
            }
        }
        rebakeTimelinePreview();
    } else if (kind == SliderDragKind::ZoomPacing) {
        m_config.zoomPacing = std::clamp(std::round(ratio * 100.0f) / 100.0f, 0.0f, 1.0f);
        m_config.autoZoomEnabled = true;
        if (m_config.zoomMode == StudioZoomMode::None) {
            m_config.zoomMode = StudioZoomMode::Zoom2D;
        }
        rebakeTimelinePreview();
    } else if (kind == SliderDragKind::ZoomSensitivity) {
        const float nextSens = std::clamp(std::round(ratio * 50.0f) / 50.0f, 0.0f, 1.0f);
        if (std::abs(nextSens - m_config.autoZoomSensitivity) >= 0.015f) {
            m_config.autoZoomSensitivity = nextSens;
            m_config.autoZoomEnabled = true;
            if (m_config.zoomMode == StudioZoomMode::None) {
                m_config.zoomMode = StudioZoomMode::Zoom2D;
            }
            rebuildAutoZoomSegmentsLive();
        }
    } else if (kind == SliderDragKind::FocusSpotlight) {
        m_config.focusSpotlightDim = std::clamp(std::round(ratio * 65.0f) / 100.0f, 0.0f, 0.65f);
        m_config.autoZoomEnabled = true;
        if (m_config.zoomMode == StudioZoomMode::None) {
            m_config.zoomMode = StudioZoomMode::Zoom2D;
        }
        rebakeTimelinePreview();
    } else if (kind == SliderDragKind::TimelineZoom) {
        const double keptDurSec = computeEffectiveDurationSec();
        const float targetZ = std::pow(24.0f, ratio);
        setTimelineZoomAnchored(targetZ, m_playheadSec, 0.5f, keptDurSec);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    } else if (kind == SliderDragKind::TimelinePan) {
        const double keptDurSec = computeEffectiveDurationSec();
        const double visSpan = visibleTimelineSpanSec(keptDurSec);
        const float thumbFrac = std::clamp(static_cast<float>(visSpan / (std::max)(0.2, keptDurSec)), 0.04f, 1.0f);
        const float thumbW = (std::max)(24.0f, w * thumbFrac);
        const float effRange = (std::max)(1.0f, w - thumbW);
        const float panRatio = std::clamp((lx - trk.left - thumbW * 0.5f) / effRange, 0.0f, 1.0f);
        m_timelineViewStartSec = static_cast<double>(panRatio) * (std::max)(0.0, keptDurSec - visSpan);
        clampTimelineViewWindow(keptDurSec);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    requestImmediatePreviewRefresh(false);
}

void StudioPreviewWindow::rebuildAutoZoomSegmentsLive() {
    StudioTimelineAnalyzer::rebuildAutoZoomSegmentsPreservingManual(
        m_timeline, m_config,
        m_previewDeckBgr.empty() ? nullptr : &m_previewDeckBgr,
        m_durationSec);
    if (m_selectedZoomIdx >= static_cast<int>(m_timeline.zoomSegments.size())) {
        m_selectedZoomIdx = -1;
    }
    rebakeTimelinePreview();
}

void StudioPreviewWindow::applyZoomPreset(StudioZoomPreset preset) {
    applyStudioZoomPreset(m_config, preset);
    m_config.autoZoomEnabled = true;
    if (m_config.zoomMode == StudioZoomMode::None) {
        m_config.zoomMode = StudioZoomMode::Zoom2D;
    }
    rebuildAutoZoomSegmentsLive();
    persistStudioConfigToSettings(m_config);
    requestImmediatePreviewRefresh(true);
}

void StudioPreviewWindow::handleMouseWheel(float lx, float ly, int wheelDelta) {
    if (m_exportModalOpen || m_preparingSession || wheelDelta == 0) return;
    if (pointInRectF(m_timelineCardRect, lx, ly)) {
        const double keptDurSec = computeEffectiveDurationSec();
        const float laneLeft = m_cutLaneRect.left;
        const float trackW = (std::max)(1.0f, m_cutLaneRect.right - laneLeft);
        const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shiftDown && m_timelineZoom > 1.01f) {
            const double visSpan = visibleTimelineSpanSec(keptDurSec);
            const double step = visSpan * 0.18 * ((wheelDelta < 0) ? 1.0 : -1.0);
            m_timelineViewStartSec += step;
            clampTimelineViewWindow(keptDurSec);
            if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        const float factor = (wheelDelta > 0) ? 1.26f : (1.0f / 1.26f);
        const bool onLanes = (lx >= laneLeft && lx <= m_cutLaneRect.right && ly >= m_allLanesHoverRect.top);
        const float anchorU = onLanes
            ? std::clamp((lx - laneLeft) / trackW, 0.0f, 1.0f)
            : 0.5f;
        const double anchorSec = onLanes
            ? mapTrackXToKeptSec(lx, laneLeft, trackW, keptDurSec)
            : m_playheadSec;
        setTimelineZoomAnchored(m_timelineZoom * factor, anchorSec, anchorU, keptDurSec);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    if (m_activeTab == LeftRailTab::CanvasBg && pointInRectF(m_wallpaperGridRect, lx, ly)) {
        const int perPage = static_cast<int>(kWallpaperSlotsPerPage);
        const int totalPages = (std::max)(1, static_cast<int>((static_cast<int>(m_filteredWallpaperIndices.size()) + perPage - 1) / perPage));
        if (totalPages > 1) {
            m_wallpaperPage = (wheelDelta < 0)
                ? ((m_wallpaperPage + 1) % totalPages)
                : ((m_wallpaperPage - 1 + totalPages) % totalPages);
            invalidateWallpaperPageBitmaps();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

void StudioPreviewWindow::handleMouseLeave() {
    m_trackingMouseLeave = false;
    bool needRepaint = false;
    if (m_hoveredCoachId != 0) {
        m_hoveredCoachId = 0;
        needRepaint = true;
    }
    if (m_hoveredHitId != Hit_None) {
        m_hoveredHitId = Hit_None;
        needRepaint = true;
    }
    if (m_hoveredStageAnnIdx != -1 || m_hoveredZoomFocusReticle ||
        m_hoveredWatermarkOnStage || m_hoveredWatermarkCorner) {
        m_hoveredStageAnnIdx = -1;
        m_hoveredStageAnnPart = StageAnnDragKind::None;
        m_hoveredZoomFocusReticle = false;
        m_hoveredWatermarkOnStage = false;
        m_hoveredWatermarkCorner = false;
        needRepaint = true;
    }
    if (m_hoverSkimming) {
        m_hoverSkimming = false;
        requestImmediatePreviewRefresh(true);
        return;
    }
    if (needRepaint && m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::handleRightMouseDown(float lx, float ly) {
    if (m_exportModalOpen || m_exporting.load(std::memory_order_acquire)) return;

    for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
        if (!pointInRectF(it->rect, lx, ly)) continue;
        const int id = it->id;
        if (id >= Hit_AnnSegmentBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_timeline.annotations.size())) {
                pushUndoSnapshot();
                m_timeline.annotations.erase(m_timeline.annotations.begin() + idx);
                m_selectedAnnIdx = -1;
                m_editingAnnotationText = false;
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id >= Hit_VideoClipBase && id < Hit_AnnSegmentBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_videoClips.size()) && m_videoClips.size() > 1) {
                m_selectedClipIdx = idx;
                m_selectedZoomIdx = -1;
                m_selectedAnnIdx = -1;
                deleteSelectedTimelineEntity();
            }
            return;
        }
        if (id >= Hit_ZoomSegmentBase && id < Hit_VideoClipBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_timeline.zoomSegments.size())) {
                pushUndoSnapshot();
                m_timeline.zoomSegments.erase(m_timeline.zoomSegments.begin() + idx);
                m_selectedZoomIdx = -1;
                rebakeTimelinePreview();
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
    }
}

void StudioPreviewWindow::handleMouseDown(float lx, float ly, bool isDoubleClick) {
    if (m_hoveredCoachId != 0) {
        m_hoveredCoachId = 0;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    if (m_preparingSession) {
        for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
            if (!pointInRectF(it->rect, lx, ly)) continue;
            if (it->id == Hit_MinimizeBtn) {
                if (m_hwnd) ShowWindow(m_hwnd, SW_MINIMIZE);
                return;
            }
            if (it->id == Hit_MaxRestoreBtn) {
                toggleMaximizeRestore();
                return;
            }
        }
        return;
    }

    const double keptDurSec = computeEffectiveDurationSec();

    for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
        if (!pointInRectF(it->rect, lx, ly)) continue;

        const int id = it->id;

        // ── 未导出防丢关闭确认模态框热区处理（最高优先拦截） ──
        if (m_closeConfirmModalOpen) {
            if (id == Hit_CloseConfirmBackdrop || id == Hit_CloseConfirmCancelBtn) {
                m_closeConfirmModalOpen = false;
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
            if (id == Hit_CloseConfirmCard) {
                return;
            }
            if (id == Hit_CloseConfirmSkipStudioCheck) {
                m_confirmSkipStudioNextTime = !m_confirmSkipStudioNextTime;
                tools3000::core::ConfigManager::instance().set<bool>(
                    "/recording/studioMode", !m_confirmSkipStudioNextTime);
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
            if (id == Hit_CloseConfirmDiscardBtn) {
                m_closeConfirmModalOpen = false;
                discardOriginalAndClose();
                return;
            }
            if (id == Hit_CloseConfirmSaveBtn) {
                m_closeConfirmModalOpen = false;
                if (!hasStudioEditsToRender()) {
                    keepOriginalAndClose();
                } else {
                    m_closeAfterSaveExport = true;
                    m_exportModalOpen = true;
                    m_exportCompletedCard = false;
                    m_editingExportName = false;
                    m_openDropdown = ExportDropdownKind::None;
                    ensureNonCollidingExportStem();
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    startAsyncExport();
                }
                return;
            }
            return;
        }

        // ── 导出模态窗热区处理（严格限定在模态窗打开态，严禁误吞 300..999 区间的抽屉控件 ID！） ──
        if (m_exportModalOpen && m_openDropdown != ExportDropdownKind::None &&
            id >= Hit_ModalDropdownItemBase && id < Hit_ModalDropdownItemBase + 32) {
            const int val = it->payload;
            if (m_openDropdown == ExportDropdownKind::Format) {
                applyFormatSmartDefaults(static_cast<StudioExportFormat>(val));
            } else if (m_openDropdown == ExportDropdownKind::Resolution) {
                m_exportOpts.resolution = static_cast<StudioExportResolution>(val);
            } else if (m_openDropdown == ExportDropdownKind::Fps) {
                m_exportOpts.fps = val;
            } else if (m_openDropdown == ExportDropdownKind::Quality) {
                m_exportOpts.quality = static_cast<StudioExportQuality>(val);
            }
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalBackdrop || id == Hit_ModalCloseBtn) {
            if (!m_exporting.load(std::memory_order_acquire)) {
                m_exportModalOpen = false;
                m_exportCompletedCard = false;
                m_editingExportName = false;
                m_openDropdown = ExportDropdownKind::None;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return;
        }
        if (id == Hit_ModalResultCopyFileBtn) {
            copyLastExportedToClipboard();
            return;
        }
        if (id == Hit_ModalResultOpenPlayBtn) {
            if (!m_lastExportedFilePath.empty()) {
                tools3000::core::WinUtils::openFile(
                    tools3000::core::WinUtils::utf8ToWstring(m_lastExportedFilePath));
            }
            return;
        }
        if (id == Hit_ModalResultRevealBtn) {
            if (!m_lastExportedFilePath.empty()) {
                tools3000::core::WinUtils::openFolderAndSelectItem(
                    tools3000::core::WinUtils::utf8ToWstring(m_lastExportedFilePath));
            }
            return;
        }
        if (id == Hit_ModalResultExportMoreBtn) {
            m_exportCompletedCard = false;
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            ensureNonCollidingExportStem();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalResultFinishCloseBtn) {
            finishExportedSessionAndClose();
            return;
        }
        if (id == Hit_ModalCardBody) {
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalNameInput) {
            m_editingExportName = true;
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalBrowseDirBtn) {
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            browseExportDirectoryAsync();
            return;
        }
        if (id == Hit_ModalFormatSelect || id == Hit_ModalResSelect ||
            id == Hit_ModalFpsSelect || id == Hit_ModalQualitySelect) {
            m_editingExportName = false;
            const ExportDropdownKind targetKind =
                (id == Hit_ModalFormatSelect) ? ExportDropdownKind::Format
                : (id == Hit_ModalResSelect) ? ExportDropdownKind::Resolution
                : (id == Hit_ModalFpsSelect) ? ExportDropdownKind::Fps
                : ExportDropdownKind::Quality;
            m_openDropdown = (m_openDropdown == targetKind) ? ExportDropdownKind::None : targetKind;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalSavePrefsCheck) {
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            m_saveExportPrefs = !m_saveExportPrefs;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalAutoCloseCheck) {
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            m_autoCloseAfterExport = !m_autoCloseAfterExport;
            tools3000::core::ConfigManager::instance().set<bool>(
                "/recording/studioExport/autoClose", m_autoCloseAfterExport);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalConfirmExportBtn) {
            startAsyncExport();
            return;
        }

        if (id != Hit_AnnTextInputBox && id != Hit_StageCanvas) {
            m_editingAnnotationText = false;
        }
        if (id != Hit_WatermarkTextInput &&
            id != Hit_WatermarkNewLine &&
            id != Hit_WatermarkPresetDualLine &&
            !(id >= Hit_WatermarkTokenBase && id < Hit_WatermarkTokenBase + 4)) {
            m_editingWatermarkText = false;
        }

        // ── 顶栏与导航热区 ──
        if (id == Hit_MinimizeBtn) {
            if (m_hwnd) ShowWindow(m_hwnd, SW_MINIMIZE);
            return;
        }
        if (id == Hit_MaxRestoreBtn) {
            toggleMaximizeRestore();
            return;
        }
        if (id == Hit_CloseBtn) {
            if (m_exporting.load(std::memory_order_acquire)) {
                m_cancelExport.store(true, std::memory_order_release);
                joinExportWorkerIfDone(true);
                m_exporting.store(false, std::memory_order_release);
                m_closeAfterSaveExport = false;
            }
            requestCloseWithConfirm();
            return;
        }
        if (id == Hit_ToggleRawModeBtn) {
            toggleRawOutputPreset();
            return;
        }
        if (id == Hit_KeepOriginalBtn) {
            if (m_exporting.load(std::memory_order_acquire)) {
                m_cancelExport.store(true, std::memory_order_release);
                joinExportWorkerIfDone(true);
                m_exporting.store(false, std::memory_order_release);
                m_closeAfterSaveExport = false;
            }
            keepOriginalAndClose();
            return;
        }
        if (id == Hit_OpenExportModalBtn) {
            m_playing = false;
            m_hoverSkimming = false;
            m_exportModalOpen = true;
            m_exportCompletedCard = false;
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            ensureNonCollidingExportStem();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id >= Hit_LeftRailTabBase && id < Hit_LeftRailTabBase + 5) {
            m_activeTab = static_cast<LeftRailTab>(it->payload);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_QuickOpenAnnotations) {
            m_activeTab = LeftRailTab::Annotations;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_SplitAtPlayhead) {
            splitClipAtCurrentCursor(false);
            return;
        }
        if (id == Hit_DeleteSelectedItem) {
            deleteSelectedTimelineEntity();
            return;
        }
        if (id == Hit_CycleClipSpeed) {
            cycleSelectedClipSpeed();
            return;
        }
        if (id == Hit_UndoBtn) {
            performUndo();
            return;
        }
        if (id == Hit_RedoBtn) {
            performRedo();
            return;
        }
        if (id == Hit_TransportJumpStart) {
            m_playing = false;
            m_hoverSkimming = false;
            m_playheadSec = 0.0;
            m_timelineViewStartSec = 0.0;
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_TransportPrevFrame || id == Hit_TransportNextFrame) {
            m_playing = false;
            m_hoverSkimming = false;
            const double frameStep = (id == Hit_TransportNextFrame) ? (1.0 / 30.0) : (-1.0 / 30.0);
            m_playheadSec = std::clamp(m_playheadSec + frameStep, 0.0, keptDurSec);
            if (m_timelineZoom > 1.01f) {
                const double visSpan = visibleTimelineSpanSec(keptDurSec);
                if (m_playheadSec < m_timelineViewStartSec) {
                    m_timelineViewStartSec = m_playheadSec;
                } else if (m_playheadSec > m_timelineViewStartSec + visSpan) {
                    m_timelineViewStartSec = m_playheadSec - visSpan;
                }
                clampTimelineViewWindow(keptDurSec);
            }
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_PlayPauseBtn) {
            m_hoverSkimming = false;
            m_playing = !m_playing;
            m_lastTickMs = GetTickCount();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_TimelineZoomFit) {
            if (m_timelineZoom > 1.02f) {
                setTimelineZoomAnchored(1.0f, m_playheadSec, 0.5f, keptDurSec);
            } else {
                setTimelineZoomAnchored(2.5f, m_playheadSec, 0.5f, keptDurSec);
            }
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_TimelineZoomOut || id == Hit_TimelineZoomIn) {
            const float mul = (id == Hit_TimelineZoomIn) ? 1.45f : (1.0f / 1.45f);
            setTimelineZoomAnchored(m_timelineZoom * mul, m_playheadSec, 0.5f, keptDurSec);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_TimelineZoomSlider || id == Hit_TimelinePanBar) {
            m_activeSlider = (id == Hit_TimelineZoomSlider)
                ? SliderDragKind::TimelineZoom
                : SliderDragKind::TimelinePan;
            SetCapture(m_hwnd);
            applySliderAtX(m_activeSlider, lx);
            return;
        }
        if (id == Hit_TimelineTopBarScrub || id == Hit_TimelineRulerScrub) {
            m_playing = false;
            m_hoverSkimming = false;
            m_scrubbingTimeline = true;
            m_scrubOnLaneArea = true;
            SetCapture(m_hwnd);
            const float laneLeft = m_cutLaneRect.left;
            const float trackW = (std::max)(1.0f, m_cutLaneRect.right - laneLeft);
            m_playheadSec = mapTrackXToKeptSec(lx, laneLeft, trackW, keptDurSec);
            requestImmediatePreviewRefresh(true);
            return;
        }

        // ── 企业与品牌水印抽屉交互热区 ──
        if (id == Hit_WatermarkToggle) {
            m_config.watermark.enabled = !m_config.watermark.enabled;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id >= Hit_WatermarkModeBase && id < Hit_WatermarkModeBase + 3) {
            const auto newMode = static_cast<WatermarkMode>(it->payload);
            m_config.watermark.mode = newMode;
            m_config.watermark.enabled = true;
            if (newMode == WatermarkMode::ImageLogo &&
                m_config.watermark.imagePathUtf8.empty() &&
                !m_recentLogoPaths.empty()) {
                m_config.watermark.imagePathUtf8 = m_recentLogoPaths.front();
            }
            if (newMode == WatermarkMode::TiledTraceability && m_config.watermark.opacity > 0.36f) {
                m_config.watermark.opacity = 0.16f;
            } else if (newMode != WatermarkMode::TiledTraceability && m_config.watermark.opacity < 0.30f) {
                m_config.watermark.opacity = 0.86f;
            }
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_WatermarkPickImage) {
            browseWatermarkLogoImageAsync();
            return;
        }
        if (id >= Hit_WatermarkRecentBase && id < Hit_WatermarkRecentBase + 6) {
            const int rIdx = it->payload;
            if (rIdx >= 0 && rIdx < static_cast<int>(m_recentLogoPaths.size())) {
                m_config.watermark.imagePathUtf8 = m_recentLogoPaths[static_cast<size_t>(rIdx)];
                m_config.watermark.mode = WatermarkMode::ImageLogo;
                m_config.watermark.enabled = true;
                if (m_config.watermark.opacity < 0.30f) m_config.watermark.opacity = 0.88f;
                WatermarkRenderer::pushStudioRecentLogoPath(m_config.watermark.imagePathUtf8);
                WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
                m_recentLogoPaths = WatermarkRenderer::loadStudioRecentLogoPaths();
                m_recentLogoThumbsDirty = true;
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id == Hit_WatermarkTextInput) {
            m_playing = false;
            m_editingWatermarkText = true;
            m_config.watermark.enabled = true;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_WatermarkNewLine) {
            std::string& tpl = m_config.watermark.textTemplate;
            while (!tpl.empty() && tpl.back() == ' ') tpl.pop_back();
            if (tpl.size() + 2 <= 140) {
                tpl += "\\n";
            }
            m_config.watermark.enabled = true;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_WatermarkPresetDualLine) {
            m_config.watermark.textTemplate = "{user}\\n{date} {time}";
            m_config.watermark.enabled = true;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id >= Hit_WatermarkTokenBase && id < Hit_WatermarkTokenBase + 4) {
            const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();
            static const char* kTokensZh[4] = { "{user}", "{host}", "{date} {time}", "内部资料 严禁外传" };
            static const char* kTokensEn[4] = { "{user}", "{host}", "{date} {time}", "CONFIDENTIAL" };
            const int k = it->payload;
            if (k >= 0 && k < 4) {
                std::string& tpl = m_config.watermark.textTemplate;
                auto eraseSubstrWithSep = [&](std::string_view needle) -> bool {
                    const size_t pos = tpl.find(needle);
                    if (pos == std::string::npos) return false;
                    tpl.erase(pos, needle.size());
                    // 清理残留的前后分隔符 " · "
                    while (true) {
                        const size_t dbl = tpl.find(" ·  · ");
                        if (dbl != std::string::npos) { tpl.replace(dbl, 8, " · "); continue; }
                        break;
                    }
                    if (tpl.rfind(" · ", 0) == 0) tpl.erase(0, 4);
                    if (tpl.size() >= 4 && tpl.compare(tpl.size() - 4, 4, " · ") == 0) tpl.erase(tpl.size() - 4);
                    while (!tpl.empty() && tpl.front() == ' ') tpl.erase(0, 1);
                    while (!tpl.empty() && tpl.back() == ' ') tpl.pop_back();
                    return true;
                };

                bool removed = false;
                if (k == 0) {
                    removed = eraseSubstrWithSep("{user}") || eraseSubstrWithSep("{USER}");
                } else if (k == 1) {
                    removed = eraseSubstrWithSep("{host}") || eraseSubstrWithSep("{HOST}");
                } else if (k == 2) {
                    const bool rDt = eraseSubstrWithSep("{date} {time}") || eraseSubstrWithSep("{datetime}");
                    const bool rD  = eraseSubstrWithSep("{date}");
                    const bool rT  = eraseSubstrWithSep("{time}");
                    removed = rDt || rD || rT;
                } else {
                    removed = eraseSubstrWithSep("内部资料 严禁外传") ||
                              eraseSubstrWithSep("严禁外传") ||
                              eraseSubstrWithSep("CONFIDENTIAL");
                }

                if (!removed) {
                    const char* tok = isZh ? kTokensZh[k] : kTokensEn[k];
                    const bool endsWithNl =
                        (!tpl.empty() && tpl.back() == '\n') ||
                        (tpl.size() >= 2 && tpl.compare(tpl.size() - 2, 2, "\\n") == 0);
                    if (!tpl.empty() && tpl.back() != ' ' && !endsWithNl) {
                        tpl += " · ";
                    }
                    tpl += tok;
                    if (tpl.size() > 140) {
                        tpl.resize(140);
                    }
                }
                m_config.watermark.enabled = true;
                WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id >= Hit_WatermarkAnchorBase && id < Hit_WatermarkAnchorBase + 5) {
            m_config.watermark.anchor = static_cast<WatermarkAnchor>(it->payload);
            m_config.watermark.enabled = true;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_WatermarkSliderScale || id == Hit_WatermarkSliderOpacity) {
            m_activeSlider = (id == Hit_WatermarkSliderScale)
                ? SliderDragKind::WatermarkScale
                : SliderDragKind::WatermarkOpacity;
            SetCapture(m_hwnd);
            applySliderAtX(m_activeSlider, lx);
            return;
        }

        // ── 视频标注卡片与属性热区（支持单击添加或按住直接拖拽至右侧舞台目标点放置） ──
        if (id >= Hit_AnnCardBase && id < Hit_AnnCardBase + 19) {
            m_pressedPresetCardIdx = it->payload;
            m_draggingPresetToStage = false;
            m_presetDragStartLx = lx;
            m_presetDragStartLy = ly;
            m_lastMouseLx = lx;
            m_lastMouseLy = ly;
            SetCapture(m_hwnd);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id >= Hit_AnnColorBase && id < Hit_AnnColorBase + static_cast<int>(kAnnotationPaletteHex.size())) {
            if (m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
                pushUndoSnapshot();
                m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)].colorHex =
                    kAnnotationPaletteHex[static_cast<size_t>(it->payload)];
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id == Hit_AnnTextInputBox) {
            m_playing = false;
            m_editingAnnotationText = true;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }

        // ── 预览舞台点击：单点水印拖拽/缩放 -> 标注拖拽/缩放 -> 运镜段焦点拖拽 ──
        if (id == Hit_StageCanvas) {
            if (m_config.watermark.enabled && m_config.watermark.mode != WatermarkMode::TiledTraceability &&
                m_lastWatermarkStageRect.right > m_lastWatermarkStageRect.left + 4.0f &&
                m_lastBitmapDrawRect.right > m_lastBitmapDrawRect.left + 16.0f) {
                const float handleX = m_lastWatermarkStageRect.right + 4.0f;
                const float handleY = m_lastWatermarkStageRect.bottom + 4.0f;
                const D2D1_RECT_F wmHit = D2D1::RectF(
                    m_lastWatermarkStageRect.left - 6.0f, m_lastWatermarkStageRect.top - 6.0f,
                    m_lastWatermarkStageRect.right + 6.0f, m_lastWatermarkStageRect.bottom + 6.0f);
                if (std::hypot(lx - handleX, ly - handleY) <= 13.0f) {
                    m_playing = false;
                    m_activeTab = LeftRailTab::Watermark;
                    m_watermarkStageDragMode = 2;
                    m_wmDragStartLx = lx;
                    m_wmDragStartLy = ly;
                    m_wmOrigScale = m_config.watermark.scale;
                    SetCapture(m_hwnd);
                    SetCursor(resolveDesiredCursor(lx, ly));
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;
                }
                if (pointInRectF(wmHit, lx, ly)) {
                    m_playing = false;
                    m_activeTab = LeftRailTab::Watermark;
                    if (isDoubleClick && m_config.watermark.mode == WatermarkMode::CornerText) {
                        m_editingWatermarkText = true;
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                        return;
                    }
                    const float bmpDrawW = (std::max)(1.0f, m_lastBitmapDrawRect.right - m_lastBitmapDrawRect.left);
                    const float bmpDrawH = (std::max)(1.0f, m_lastBitmapDrawRect.bottom - m_lastBitmapDrawRect.top);
                    const float wcx = (m_lastWatermarkStageRect.left + m_lastWatermarkStageRect.right) * 0.5f;
                    const float wcy = (m_lastWatermarkStageRect.top + m_lastWatermarkStageRect.bottom) * 0.5f;
                    m_wmOrigNormX = std::clamp((wcx - m_lastBitmapDrawRect.left) / bmpDrawW, 0.03f, 0.97f);
                    m_wmOrigNormY = std::clamp((wcy - m_lastBitmapDrawRect.top) / bmpDrawH, 0.03f, 0.97f);
                    m_config.watermark.customNormX = m_wmOrigNormX;
                    m_config.watermark.customNormY = m_wmOrigNormY;
                    m_config.watermark.anchor = WatermarkAnchor::CustomFree;
                    m_watermarkStageDragMode = 1;
                    m_wmDragStartLx = lx;
                    m_wmDragStartLy = ly;
                    SetCapture(m_hwnd);
                    SetCursor(resolveDesiredCursor(lx, ly));
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;
                }
            }

            float nx = 0.5f, ny = 0.5f;
            if (!mapStagePointToSourceNorm(lx, ly, nx, ny)) return;

            int hitAi = -1;
            StageAnnDragKind hitPart = StageAnnDragKind::None;
            if (hitTestStageAnnotation(lx, ly, hitAi, hitPart) && hitAi >= 0) {
                const auto& ann = m_timeline.annotations[static_cast<size_t>(hitAi)];
                m_playing = false;
                m_annDemoStartTick = 0;
                m_selectedAnnIdx = hitAi;
                m_selectedZoomIdx = -1;
                m_selectedClipIdx = -1;
                m_activeTab = LeftRailTab::Annotations;
                if (hitPart == StageAnnDragKind::MoveBody && isDoubleClick && isTextLikeAnnotation(ann.kind)) {
                    m_editingAnnotationText = true;
                } else {
                    pushUndoSnapshot();
                    m_stageAnnDrag = hitPart;
                    m_annDragStartNormX = nx;
                    m_annDragStartNormY = ny;
                    m_annOrigX0 = ann.x0; m_annOrigY0 = ann.y0;
                    m_annOrigX1 = ann.x1; m_annOrigY1 = ann.y1;
                    SetCapture(m_hwnd);
                }
                SetCursor(resolveDesiredCursor(lx, ly));
                requestImmediatePreviewRefresh(true);
                return;
            }

            if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
                pushUndoSnapshot();
                m_playing = false;
                m_hoverSkimming = false;
                auto& seg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
                const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
                const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
                float curSx = 0.0f, curSy = 0.0f;
                const bool nearExistingReticle =
                    mapSourceNormToStagePoint(seg.focusX / srcW, seg.focusY / srcH, curSx, curSy) &&
                    (std::hypot(lx - curSx, ly - curSy) <= 18.0f);
                if (!nearExistingReticle) {
                    seg.focusX = nx * srcW;
                    seg.focusY = ny * srcH;
                }
                seg.manualInserted = true;
                m_draggingZoomFocus = true;
                SetCapture(m_hwnd);
                SetCursor(resolveDesiredCursor(lx, ly));
                // 按下瞬间切换至 100% 全景底图，拖拽全程由 Direct2D 0ms 实时绘制光学取景框与 1:1 贴手准星
                updateCurrentComposedBitmap();
                RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            return;
        }

        // ── 点击视频切片：选中该切片 + 定格绯红主指针 + 支持按住左键继续丝滑拖指针看画面！ ──
        if (id >= Hit_VideoClipBase && id < Hit_AnnSegmentBase) {
            const int clipIdx = it->payload;
            m_playing = false;
            m_hoverSkimming = false;
            m_selectedClipIdx = clipIdx;
            m_selectedZoomIdx = -1;
            m_selectedAnnIdx = -1;
            m_scrubbingTimeline = true;
            m_scrubOnLaneArea = true;
            SetCapture(m_hwnd);

            const float laneLeft = m_cutLaneRect.left;
            const float w = (std::max)(1.0f, m_cutLaneRect.right - laneLeft);
            m_playheadSec = mapTrackXToKeptSec(lx, laneLeft, w, keptDurSec);
            requestImmediatePreviewRefresh(true);
            return;
        }

        // ── 时间轴插入运镜与轨道拖拽 ──
        const int64_t rawTotalMs = (std::max<int64_t>)(400, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
        const int64_t keptTotalMsDown = (std::max<int64_t>)(100, static_cast<int64_t>(std::llround(keptDurSec * 1000.0)));
        if (id == Hit_AddZoomAtPlayhead || id == Hit_ZoomLaneEmpty) {
            pushUndoSnapshot();
            m_playing = false;
            m_hoverSkimming = false;
            const float laneW = (std::max)(1.0f, m_zoomLaneRect.right - m_zoomLaneRect.left);
            const double visSpanMsDown = (std::max)(100.0, visibleTimelineSpanSec(keptDurSec) * 1000.0);
            int64_t clickKeptMs = (id == Hit_ZoomLaneEmpty)
                ? static_cast<int64_t>(std::llround(mapTrackXToKeptSec(lx, m_zoomLaneRect.left, laneW, keptDurSec) * 1000.0))
                : static_cast<int64_t>(std::llround(m_playheadSec * 1000.0));

            // 智能贴边插入：若点击/播放头处于视频拆分点或边界 8.5 DIP 磁性区内，自动吸附起点精准对齐拆分点并向后延伸（不跨越切缝）
            const auto insertAnchors = StudioTimelineAnalyzer::collectTimelineSnapAnchors(
                m_videoClips, m_timeline.zoomSegments, m_timeline.annotations,
                -1, -1, -1, false);
            const int64_t insertSnapWinMs = static_cast<int64_t>(std::llround((8.5 / laneW) * visSpanMsDown));
            const auto ptSnap = StudioTimelineAnalyzer::snapTimelinePoint(
                clickKeptMs, insertAnchors, insertSnapWinMs, keptTotalMsDown);
            const bool alignedToAnchor = ptSnap.snapped;
            if (alignedToAnchor) {
                clickKeptMs = ptSnap.snappedStartMs;
            }

            const int64_t defaultDurKept = (std::min<int64_t>)(2200, (std::max<int64_t>)(500, keptTotalMsDown / 3));
            int64_t keptSt = alignedToAnchor
                ? std::clamp<int64_t>(clickKeptMs, 0, (std::max<int64_t>)(0, keptTotalMsDown - 350))
                : std::clamp<int64_t>(clickKeptMs - defaultDurKept / 4, 0, (std::max<int64_t>)(0, keptTotalMsDown - 350));
            int64_t keptEd = (std::min<int64_t>)(keptTotalMsDown, keptSt + defaultDurKept);

            // 若右侧紧邻已有运镜块或视频拆分点，智能收束终点避免重叠压盖
            for (const auto& existingZ : m_timeline.zoomSegments) {
                const int64_t eks = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, existingZ.startMs);
                if (eks > keptSt + 350 && eks < keptEd) {
                    keptEd = eks;
                }
            }

            int64_t st = StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, keptSt);
            int64_t ed = StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, keptEd);
            if (ed <= st + 260) {
                ed = (std::min)(rawTotalMs, st + 400);
            }

            const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
            const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
            float fxPx = srcW * 0.5f, fyPx = srcH * 0.5f;
            if (!m_evaluatedFrames.empty()) {
                const size_t eIdx = (std::min)(
                    m_evaluatedFrames.size() - 1,
                    static_cast<size_t>(std::llround((st / 1000.0) * m_previewEvalFps)));
                fxPx = std::clamp(m_evaluatedFrames[eIdx].smoothCursorX, srcW * 0.12f, srcW * 0.88f);
                fyPx = std::clamp(m_evaluatedFrames[eIdx].smoothCursorY, srcH * 0.12f, srcH * 0.88f);
            }

            StudioZoomSegment z{};
            z.startMs = st;
            z.endMs = ed;
            z.focusX = fxPx;
            z.focusY = fyPx;
            z.targetZoom = m_config.defaultZoomFactor;
            z.enabled = true;
            z.manualInserted = true;
            m_config.autoZoomEnabled = true;
            if (m_config.zoomMode == StudioZoomMode::None) {
                m_config.zoomMode = StudioZoomMode::Zoom2D;
            }
            m_timeline.zoomSegments.push_back(z);
            m_selectedZoomIdx = static_cast<int>(m_timeline.zoomSegments.size()) - 1;
            m_selectedClipIdx = -1;
            m_selectedAnnIdx = -1;
            m_playheadSec = std::clamp(keptSt / 1000.0, 0.0, keptDurSec);
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }

        if (id >= Hit_AnnSegmentBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_timeline.annotations.size())) {
                m_playing = false;
                m_hoverSkimming = false;
                m_selectedAnnIdx = idx;
                m_selectedZoomIdx = -1;
                m_selectedClipIdx = -1;
                m_activeTab = LeftRailTab::Annotations;
                auto& ann = m_timeline.annotations[static_cast<size_t>(idx)];
                if (isDoubleClick && isTextLikeAnnotation(ann.kind)) {
                    m_editingAnnotationText = true;
                } else {
                    pushUndoSnapshot();
                    const float edgeTol = (std::min)(7.0f, (it->rect.right - it->rect.left) * 0.28f);
                    m_trackDragKind = (lx <= it->rect.left + edgeTol) ? TrackDragKind::AnnLeft
                        : ((lx >= it->rect.right - edgeTol) ? TrackDragKind::AnnRight : TrackDragKind::AnnMove);
                    m_trackDragIndex = idx;
                    m_trackDragStartX = lx;
                    m_trackDragOrigKeptStartMs = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.startMs);
                    m_trackDragOrigKeptEndMs = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.endMs);
                    m_trackDragCurrentKeptStartMs = m_trackDragOrigKeptStartMs;
                    m_trackDragCurrentKeptEndMs = m_trackDragOrigKeptEndMs;
                    m_trackDragOrigPlayheadKeptMs = static_cast<int64_t>(std::llround(m_playheadSec * 1000.0));
                    m_timelineSnapActive = false;
                    SetCapture(m_hwnd);
                }
                m_playheadSec = std::clamp(
                    StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.startMs) / 1000.0 + 0.05,
                    0.0, keptDurSec);
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id >= Hit_ZoomSegmentBase && id < Hit_VideoClipBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_timeline.zoomSegments.size())) {
                m_playing = false;
                m_hoverSkimming = false;
                m_selectedZoomIdx = idx;
                m_selectedClipIdx = -1;
                m_selectedAnnIdx = -1;
                auto& zseg = m_timeline.zoomSegments[static_cast<size_t>(idx)];
                pushUndoSnapshot();
                if (isDoubleClick) {
                    zseg.enabled = !zseg.enabled;
                    rebakeTimelinePreview();
                } else {
                    const float edgeTol = (std::min)(7.0f, (it->rect.right - it->rect.left) * 0.28f);
                    m_trackDragKind = (lx <= it->rect.left + edgeTol) ? TrackDragKind::ZoomLeft
                        : ((lx >= it->rect.right - edgeTol) ? TrackDragKind::ZoomRight : TrackDragKind::ZoomMove);
                    m_trackDragIndex = idx;
                    m_trackDragStartX = lx;
                    m_trackDragOrigKeptStartMs = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, zseg.startMs);
                    m_trackDragOrigKeptEndMs = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, zseg.endMs);
                    m_trackDragCurrentKeptStartMs = m_trackDragOrigKeptStartMs;
                    m_trackDragCurrentKeptEndMs = m_trackDragOrigKeptEndMs;
                    m_trackDragOrigPlayheadKeptMs = static_cast<int64_t>(std::llround(m_playheadSec * 1000.0));
                    m_timelineSnapActive = false;
                    SetCapture(m_hwnd);
                }
                const int64_t steadyOffsetMs = (std::min<int64_t>)(420, (std::max<int64_t>)(80, (zseg.endMs - zseg.startMs) / 2));
                m_playheadSec = std::clamp(
                    StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, zseg.startMs + steadyOffsetMs) / 1000.0,
                    0.0, keptDurSec);
                requestImmediatePreviewRefresh(true);
            }
            return;
        }

        // ── 抽屉面板控制项响应 ──
        if (id == Hit_ZoomMode_None || id == Hit_ZoomMode_2D || id == Hit_ZoomMode_3D) {
            if (id == Hit_ZoomMode_None) {
                m_config.autoZoomEnabled = false;
                m_config.zoomMode = StudioZoomMode::None;
            } else {
                m_config.autoZoomEnabled = true;
                m_config.zoomMode = (id == Hit_ZoomMode_3D) ? StudioZoomMode::Motion3D : StudioZoomMode::Zoom2D;
            }
            persistStudioConfigToSettings(m_config);
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ToggleLockCamera) {
            m_config.lockCameraWhileZoomed = !m_config.lockCameraWhileZoomed;
            persistStudioConfigToSettings(m_config);
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ToggleMotionBlur) {
            m_config.motionBlurEnabled = !m_config.motionBlurEnabled;
            persistStudioConfigToSettings(m_config);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ZoomPreset_Gentle || id == Hit_ZoomPreset_Balanced || id == Hit_ZoomPreset_Dynamic) {
            const StudioZoomPreset chosen = (id == Hit_ZoomPreset_Gentle)
                ? StudioZoomPreset::Gentle
                : ((id == Hit_ZoomPreset_Balanced) ? StudioZoomPreset::Balanced : StudioZoomPreset::Dynamic);
            applyZoomPreset(chosen);
            return;
        }
        if (id == Hit_SliderZoomFactor || id == Hit_SliderZoomPacing ||
            id == Hit_SliderZoomSensitivity || id == Hit_SliderFocusSpotlight) {
            m_activeSlider = (id == Hit_SliderZoomFactor) ? SliderDragKind::ZoomFactor
                : (id == Hit_SliderZoomPacing) ? SliderDragKind::ZoomPacing
                : (id == Hit_SliderZoomSensitivity) ? SliderDragKind::ZoomSensitivity
                : SliderDragKind::FocusSpotlight;
            SetCapture(m_hwnd);
            applySliderAtX(m_activeSlider, lx);
            return;
        }
        if (id == Hit_ToggleSmoothCursor) {
            m_config.smoothCursorEnabled = !m_config.smoothCursorEnabled;
            persistStudioConfigToSettings(m_config);
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_CursorScale_100 || id == Hit_CursorScale_135 || id == Hit_CursorScale_170) {
            m_config.cursorScale = (id == Hit_CursorScale_100) ? 1.00f
                : ((id == Hit_CursorScale_135) ? 1.35f : 1.70f);
            m_config.smoothCursorEnabled = true;
            persistStudioConfigToSettings(m_config);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ToggleCursorHalo) {
            m_config.cursorHighlightHalo = !m_config.cursorHighlightHalo;
            m_config.smoothCursorEnabled = true;
            persistStudioConfigToSettings(m_config);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id >= Hit_ClickStyleBase && id < Hit_ClickStyleBase + 4) {
            const auto chosen = static_cast<StudioClickEffectStyle>(id - Hit_ClickStyleBase);
            m_config.clickEffectStyle = chosen;
            m_config.showClickRipple = (chosen != StudioClickEffectStyle::TactileOnly);
            m_config.smoothCursorEnabled = true;
            persistStudioConfigToSettings(m_config);
            rebakeTimelinePreview();
            m_clickDemoStartTick = GetTickCount64();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ToggleAutoFrame) {
            m_config.autoFrameEnabled = !m_config.autoFrameEnabled;
            persistStudioConfigToSettings(m_config);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id >= Hit_AspectBase && id < Hit_AspectBase + 5) {
            m_config.aspectRatio = static_cast<StudioAspectRatio>(id - Hit_AspectBase);
            persistStudioConfigToSettings(m_config);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_CycleInsetBorderColor) {
            size_t nextIdx = 0;
            for (size_t c = 0; c < kInsetBorderColors.size(); ++c) {
                if (m_config.insetBorderColorHex == kInsetBorderColors[c]) {
                    nextIdx = (c + 1) % kInsetBorderColors.size();
                    break;
                }
            }
            m_config.insetBorderColorHex = kInsetBorderColors[nextIdx];
            if (m_config.insetBorderWidth < 1.0f) m_config.insetBorderWidth = 2.0f;
            m_config.autoFrameEnabled = true;
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_SliderPadding || id == Hit_SliderInsetBorder ||
            id == Hit_SliderRadius || id == Hit_SliderShadow || id == Hit_SliderBgBlur) {
            m_activeSlider = (id == Hit_SliderPadding) ? SliderDragKind::Padding
                : (id == Hit_SliderInsetBorder) ? SliderDragKind::InsetBorder
                : (id == Hit_SliderRadius) ? SliderDragKind::Radius
                : (id == Hit_SliderShadow) ? SliderDragKind::Shadow
                : SliderDragKind::BgBlur;
            SetCapture(m_hwnd);
            applySliderAtX(m_activeSlider, lx);
            return;
        }
        if (id >= Hit_WallCatBase && id < Hit_WallCatBase + 6) {
            m_wallpaperCategory = id - Hit_WallCatBase;
            m_wallpaperPage = 0;
            rebuildFilteredWallpapers();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_WallPrevPage || id == Hit_WallNextPage) {
            const int perPage = static_cast<int>(kWallpaperSlotsPerPage);
            const int totalPages = (std::max)(1, static_cast<int>((static_cast<int>(m_filteredWallpaperIndices.size()) + perPage - 1) / perPage));
            m_wallpaperPage = (id == Hit_WallPrevPage)
                ? ((m_wallpaperPage - 1 + totalPages) % totalPages)
                : ((m_wallpaperPage + 1) % totalPages);
            invalidateWallpaperPageBitmaps();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_WallPickCustom) {
            browseCustomWallpaperImageAsync();
            return;
        }
        if (id >= Hit_ThemeSlotBase && id < Hit_ThemeSlotBase + static_cast<int>(kWallpaperSlotsPerPage)) {
            const int entryIdx = it->payload;
            if (entryIdx >= 0 && entryIdx < static_cast<int>(m_allWallpapers.size())) {
                const auto& entry = m_allWallpapers[static_cast<size_t>(entryIdx)];
                if (entry.isBuiltin) {
                    m_config.backgroundPresetKey = entry.presetKey;
                    m_config.backgroundTheme = entry.builtinTheme;
                    m_config.customBackgroundPath.clear();
                } else {
                    m_config.customBackgroundPath = entry.filePathUtf8;
                }
                m_config.autoFrameEnabled = true;
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id == Hit_ToggleCaptions) {
            m_config.autoCaptionsEnabled = !m_config.autoCaptionsEnabled;
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }
    }

    if (!m_exportModalOpen && ly <= 46.0f && m_hwnd) {
        if (isDoubleClick) {
            toggleMaximizeRestore();
            return;
        }
        m_draggingWindow = true;
        SetCapture(m_hwnd);
        GetCursorPos(&m_dragStartScreen);
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        m_windowStartPos = {wr.left, wr.top};
    }
}

bool StudioPreviewWindow::hitTestStageAnnotation(
    float lx, float ly, int& outAnnIdx, StageAnnDragKind& outPart) const {
    outAnnIdx = -1;
    outPart = StageAnnDragKind::None;
    if (!pointInRectF(m_lastBitmapDrawRect, lx, ly)) {
        return false;
    }

    const int64_t curSrcMs = static_cast<int64_t>(std::llround(currentDisplayedSourceSec() * 1000.0));
    auto testSingleIndex = [&](int ai) -> bool {
        if (ai < 0 || ai >= static_cast<int>(m_timeline.annotations.size())) return false;
        const auto& ann = m_timeline.annotations[static_cast<size_t>(ai)];
        if (!ann.enabled || curSrcMs < ann.startMs || curSrcMs > ann.endMs) return false;

        float p0x = 0.0f, p0y = 0.0f, p1x = 0.0f, p1y = 0.0f;
        if (!mapSourceNormToStagePoint(ann.x0, ann.y0, p0x, p0y) ||
            !mapSourceNormToStagePoint(ann.x1, ann.y1, p1x, p1y)) {
            return false;
        }

        if (!isTextLikeAnnotation(ann.kind)) {
            const float endHx = isPointDirectedAnnotation(ann.kind) ? p1x : (std::max)(p0x, p1x) + 4.0f;
            const float endHy = isPointDirectedAnnotation(ann.kind) ? p1y : (std::max)(p0y, p1y) + 4.0f;
            if (std::hypot(lx - endHx, ly - endHy) <= 13.0f) {
                outAnnIdx = ai;
                outPart = StageAnnDragKind::DragEndPoint;
                return true;
            }
            const float startHx = isPointDirectedAnnotation(ann.kind) ? p0x : (std::min)(p0x, p1x) - 4.0f;
            const float startHy = isPointDirectedAnnotation(ann.kind) ? p0y : (std::min)(p0y, p1y) - 4.0f;
            if (std::hypot(lx - startHx, ly - startHy) <= 13.0f) {
                outAnnIdx = ai;
                outPart = StageAnnDragKind::DragStartPoint;
                return true;
            }
        }

        const D2D1_RECT_F hitBox = D2D1::RectF(
            (std::min)(p0x, p1x) - 22.0f, (std::min)(p0y, p1y) - 18.0f,
            (std::max)(p0x, p1x) + 22.0f, (std::max)(p0y, p1y) + 18.0f);
        if (pointInRectF(hitBox, lx, ly)) {
            outAnnIdx = ai;
            outPart = StageAnnDragKind::MoveBody;
            return true;
        }
        return false;
    };

    // 优先测试当前选中的标注控制柄，再从顶层向底层扫描其他可见标注
    if (m_selectedAnnIdx >= 0 && testSingleIndex(m_selectedAnnIdx)) {
        return true;
    }
    for (int ai = static_cast<int>(m_timeline.annotations.size()) - 1; ai >= 0; --ai) {
        if (ai == m_selectedAnnIdx) continue;
        if (testSingleIndex(ai)) return true;
    }
    return false;
}

HCURSOR StudioPreviewWindow::resolveDesiredCursor(float lx, float ly) const {
    (void)ly;
    // 1. 正在进行中的交互捕获状态优先锁定对应功能光标（即便滑出控件热区也稳态不跳）
    if (m_draggingWindow) {
        return LoadCursorW(nullptr, IDC_SIZEALL);
    }
    if (m_watermarkStageDragMode == 2) {
        return LoadCursorW(nullptr, IDC_SIZENWSE);
    }
    if (m_watermarkStageDragMode == 1) {
        return LoadCursorW(nullptr, IDC_SIZEALL);
    }
    if (m_draggingPresetToStage) {
        return LoadCursorW(nullptr, IDC_SIZEALL);
    }
    if (m_stageAnnDrag != StageAnnDragKind::None) {
        if (m_stageAnnDrag == StageAnnDragKind::MoveBody) {
            return LoadCursorW(nullptr, IDC_SIZEALL);
        }
        if (m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size()) &&
            isPointDirectedAnnotation(m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)].kind)) {
            return LoadCursorW(nullptr, IDC_CROSS);
        }
        return LoadCursorW(nullptr, IDC_SIZENWSE);
    }
    if (m_draggingZoomFocus) {
        return LoadCursorW(nullptr, IDC_SIZEALL);
    }
    if (m_activeSlider != SliderDragKind::None || m_scrubbingTimeline) {
        return LoadCursorW(nullptr, IDC_SIZEWE);
    }
    if (m_trackDragKind != TrackDragKind::None) {
        return (m_trackDragKind == TrackDragKind::ZoomMove || m_trackDragKind == TrackDragKind::AnnMove)
            ? LoadCursorW(nullptr, IDC_SIZEALL)
            : LoadCursorW(nullptr, IDC_SIZEWE);
    }

    // 2. 悬停预判态光标矩阵
    if (m_hoveredHitId == Hit_ModalNameInput || m_hoveredHitId == Hit_AnnTextInputBox ||
        m_hoveredHitId == Hit_WatermarkTextInput) {
        return LoadCursorW(nullptr, IDC_IBEAM);
    }
    if (m_hoveredHitId == Hit_SliderPadding || m_hoveredHitId == Hit_SliderInsetBorder ||
        m_hoveredHitId == Hit_SliderRadius || m_hoveredHitId == Hit_SliderShadow ||
        m_hoveredHitId == Hit_SliderBgBlur ||
        m_hoveredHitId == Hit_WatermarkSliderScale || m_hoveredHitId == Hit_WatermarkSliderOpacity ||
        m_hoveredHitId == Hit_TimelineZoomSlider || m_hoveredHitId == Hit_TimelinePanBar ||
        m_hoveredHitId == Hit_TimelineTopBarScrub || m_hoveredHitId == Hit_TimelineRulerScrub) {
        return LoadCursorW(nullptr, IDC_SIZEWE);
    }
    if ((m_hoveredHitId >= Hit_ZoomSegmentBase && m_hoveredHitId < Hit_VideoClipBase) ||
        m_hoveredHitId >= Hit_AnnSegmentBase) {
        for (const auto& hz : m_hitZones) {
            if (hz.id == m_hoveredHitId) {
                const float edgeTol = (std::min)(7.0f, (hz.rect.right - hz.rect.left) * 0.28f);
                return (lx <= hz.rect.left + edgeTol || lx >= hz.rect.right - edgeTol)
                    ? LoadCursorW(nullptr, IDC_SIZEWE)
                    : LoadCursorW(nullptr, IDC_SIZEALL);
            }
        }
    }
    if (m_hoveredHitId == Hit_StageCanvas) {
        if (m_hoveredWatermarkCorner) {
            return LoadCursorW(nullptr, IDC_SIZENWSE);
        }
        if (m_hoveredWatermarkOnStage) {
            return LoadCursorW(nullptr, IDC_SIZEALL);
        }
        if (m_hoveredStageAnnIdx >= 0 && m_hoveredStageAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
            if (m_hoveredStageAnnPart == StageAnnDragKind::MoveBody) {
                return LoadCursorW(nullptr, IDC_SIZEALL);
            }
            const auto& ann = m_timeline.annotations[static_cast<size_t>(m_hoveredStageAnnIdx)];
            return isPointDirectedAnnotation(ann.kind)
                ? LoadCursorW(nullptr, IDC_CROSS)
                : LoadCursorW(nullptr, IDC_SIZENWSE);
        }
        if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
            return m_hoveredZoomFocusReticle
                ? LoadCursorW(nullptr, IDC_SIZEALL)
                : LoadCursorW(nullptr, IDC_CROSS);
        }
        return LoadCursorW(nullptr, IDC_ARROW);
    }
    if (m_hoveredHitId != Hit_None &&
        m_hoveredHitId != Hit_ModalBackdrop &&
        m_hoveredHitId != Hit_ModalCardBody &&
        m_hoveredHitId != Hit_CloseConfirmBackdrop &&
        m_hoveredHitId != Hit_CloseConfirmCard) {
        return LoadCursorW(nullptr, IDC_HAND);
    }
    return LoadCursorW(nullptr, IDC_ARROW);
}

void StudioPreviewWindow::handleMouseMove(float lx, float ly, bool leftButtonDown) {
    m_lastMouseLx = lx;
    m_lastMouseLy = ly;

    if (!m_trackingMouseLeave && m_hwnd) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = m_hwnd;
        if (TrackMouseEvent(&tme)) {
            m_trackingMouseLeave = true;
        }
    }

    if (m_pressedPresetCardIdx >= 0 && leftButtonDown) {
        if (!m_draggingPresetToStage &&
            std::hypot(lx - m_presetDragStartLx, ly - m_presetDragStartLy) >= 5.0f) {
            m_draggingPresetToStage = true;
            m_hoveredCoachId = 0;
        }
        if (m_draggingPresetToStage) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
    }

    if (m_draggingWindow && leftButtonDown && m_hwnd) {
        POINT cur{};
        GetCursorPos(&cur);
        if (m_isMaximized) {
            if (std::hypot(static_cast<double>(cur.x - m_dragStartScreen.x),
                           static_cast<double>(cur.y - m_dragStartScreen.y)) >= 6.0) {
                const HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
                const RECT work = tools3000::core::dpi::workArea(mon);
                int rw = m_restoreWindowRect.right - m_restoreWindowRect.left;
                int rh = m_restoreWindowRect.bottom - m_restoreWindowRect.top;
                if (rw < 640 || rh < 480) {
                    rw = tools3000::core::dpi::scaleMetric(m_logicalWidth, m_dpiScale);
                    rh = tools3000::core::dpi::scaleMetric(m_logicalHeight, m_dpiScale);
                }
                const double anchorRatio = std::clamp(
                    static_cast<double>(cur.x - work.left) / (std::max)(1.0, static_cast<double>(work.right - work.left)),
                    0.12, 0.88);
                const int newX = static_cast<int>(cur.x) - static_cast<int>(std::round(rw * anchorRatio));
                const int newY = (std::max<int>)(
                    static_cast<int>(work.top),
                    static_cast<int>(cur.y) - tools3000::core::dpi::scaleMetric(22, m_dpiScale));
                m_isMaximized = false;
                tools3000::core::ConfigManager::instance().set<bool>("/recording/studioWindowMaximized", false);
                m_dragStartScreen = cur;
                m_windowStartPos = {newX, newY};
                SetWindowPos(m_hwnd, nullptr, newX, newY, rw, rh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                applyWindowRoundingForCurrentState(rw, rh);
            }
            return;
        }
        SetWindowPos(
            m_hwnd, nullptr,
            m_windowStartPos.x + (cur.x - m_dragStartScreen.x),
            m_windowStartPos.y + (cur.y - m_dragStartScreen.y),
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    if (m_watermarkStageDragMode != 0 && leftButtonDown) {
        if (m_watermarkStageDragMode == 1) {
            const float bmpDrawW = (std::max)(1.0f, m_lastBitmapDrawRect.right - m_lastBitmapDrawRect.left);
            const float bmpDrawH = (std::max)(1.0f, m_lastBitmapDrawRect.bottom - m_lastBitmapDrawRect.top);
            m_config.watermark.customNormX = std::clamp(m_wmOrigNormX + (lx - m_wmDragStartLx) / bmpDrawW, 0.03f, 0.97f);
            m_config.watermark.customNormY = std::clamp(m_wmOrigNormY + (ly - m_wmDragStartLy) / bmpDrawH, 0.03f, 0.97f);
            m_config.watermark.anchor = WatermarkAnchor::CustomFree;
        } else if (m_watermarkStageDragMode == 2) {
            const float delta = ((lx - m_wmDragStartLx) + (ly - m_wmDragStartLy)) * 0.5f;
            m_config.watermark.scale = std::clamp(m_wmOrigScale * (1.0f + delta / 110.0f), 0.35f, 2.80f);
        }
        requestImmediatePreviewRefresh(false);
        return;
    }

    if (m_activeSlider != SliderDragKind::None && leftButtonDown) {
        applySliderAtX(m_activeSlider, lx);
        return;
    }

    if (m_draggingZoomFocus && leftButtonDown &&
        m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
        float nx = 0.5f, ny = 0.5f;
        if (mapStagePointToSourceNorm(lx, ly, nx, ny)) {
            auto& seg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
            seg.focusX = nx * static_cast<float>((std::max)(1, m_timeline.sourceWidth));
            seg.focusY = ny * static_cast<float>((std::max)(1, m_timeline.sourceHeight));
            seg.manualInserted = true;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    if (m_stageAnnDrag != StageAnnDragKind::None && leftButtonDown &&
        m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
        float nx = 0.5f, ny = 0.5f;
        if (mapStagePointToSourceNorm(lx, ly, nx, ny)) {
            auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
            const float dx = nx - m_annDragStartNormX;
            const float dy = ny - m_annDragStartNormY;
            if (m_stageAnnDrag == StageAnnDragKind::MoveBody) {
                const float w = m_annOrigX1 - m_annOrigX0;
                const float h = m_annOrigY1 - m_annOrigY0;
                ann.x0 = std::clamp(m_annOrigX0 + dx, 0.0f, 1.0f - std::abs(w));
                ann.y0 = std::clamp(m_annOrigY0 + dy, 0.0f, 1.0f - std::abs(h));
                ann.x1 = std::clamp(ann.x0 + w, 0.0f, 1.0f);
                ann.y1 = std::clamp(ann.y0 + h, 0.0f, 1.0f);
            } else if (m_stageAnnDrag == StageAnnDragKind::DragStartPoint) {
                ann.x0 = std::clamp(nx, 0.02f, 0.98f);
                ann.y0 = std::clamp(ny, 0.02f, 0.98f);
            } else {
                ann.x1 = std::clamp(nx, 0.02f, 0.98f);
                ann.y1 = std::clamp(ny, 0.02f, 0.98f);
            }
            requestImmediatePreviewRefresh(false);
        }
        return;
    }

    const double keptDurSec = computeEffectiveDurationSec();
    const int64_t keptTotalMs = (std::max<int64_t>)(100, static_cast<int64_t>(std::llround(keptDurSec * 1000.0)));
    const double visSpanMs = (std::max)(100.0, visibleTimelineSpanSec(keptDurSec) * 1000.0);

    if (m_trackDragKind != TrackDragKind::None && leftButtonDown) {
        const float laneW = (std::max)(1.0f, m_zoomLaneRect.right - m_zoomLaneRect.left);
        const int64_t deltaKeptMs = static_cast<int64_t>(std::llround(((lx - m_trackDragStartX) / laneW) * visSpanMs));
        const int64_t minSpan = 220;
        const bool altBypassSnap = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const int64_t snapWindowMs = altBypassSnap
            ? 0
            : static_cast<int64_t>(std::llround((8.0 / laneW) * visSpanMs));

        auto applyKeptDragToSourceBounds = [&](int64_t& srcSt, int64_t& srcEd,
                                               TrackDragKind moveK, TrackDragKind leftK,
                                               int excludeZoomIdx, int excludeAnnIdx) -> int64_t {
            int64_t rawKeptSt = m_trackDragOrigKeptStartMs;
            int64_t rawKeptEd = m_trackDragOrigKeptEndMs;
            int edgeMode = 0;
            if (m_trackDragKind == moveK) {
                edgeMode = 0;
                const int64_t len = (std::max<int64_t>)(minSpan, m_trackDragOrigKeptEndMs - m_trackDragOrigKeptStartMs);
                rawKeptSt = m_trackDragOrigKeptStartMs + deltaKeptMs;
                rawKeptEd = rawKeptSt + len;
            } else if (m_trackDragKind == leftK) {
                edgeMode = 1;
                rawKeptSt = m_trackDragOrigKeptStartMs + deltaKeptMs;
            } else {
                edgeMode = 2;
                rawKeptEd = m_trackDragOrigKeptEndMs + deltaKeptMs;
            }

            const auto anchors = StudioTimelineAnalyzer::collectTimelineSnapAnchors(
                m_videoClips, m_timeline.zoomSegments, m_timeline.annotations,
                m_trackDragOrigPlayheadKeptMs, excludeZoomIdx, excludeAnnIdx, m_trackDragOrigPlayheadKeptMs >= 0);
            const auto snapRes = StudioTimelineAnalyzer::snapTimelineRangeDrag(
                rawKeptSt, rawKeptEd, edgeMode, anchors, snapWindowMs, minSpan, keptTotalMs);

            const int64_t newKeptSt = snapRes.snappedStartMs;
            const int64_t newKeptEd = snapRes.snappedEndMs;
            m_trackDragCurrentKeptStartMs = newKeptSt;
            m_trackDragCurrentKeptEndMs = newKeptEd;
            m_timelineSnapActive = snapRes.snapped;
            m_timelineSnapLineKeptMs = snapRes.snapLineKeptMs;
            m_timelineSnapTargetKind = snapRes.targetKind;

            srcSt = StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, newKeptSt);
            srcEd = StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, newKeptEd);
            if (srcEd <= srcSt + minSpan) {
                srcEd = (std::min<int64_t>)(static_cast<int64_t>(std::llround(m_durationSec * 1000.0)), srcSt + minSpan);
            }
            return (m_trackDragKind == TrackDragKind::ZoomRight || m_trackDragKind == TrackDragKind::AnnRight)
                ? newKeptEd : newKeptSt;
        };

        if (m_trackDragKind == TrackDragKind::ZoomMove ||
            m_trackDragKind == TrackDragKind::ZoomLeft ||
            m_trackDragKind == TrackDragKind::ZoomRight) {
            if (m_trackDragIndex >= 0 && m_trackDragIndex < static_cast<int>(m_timeline.zoomSegments.size())) {
                auto& z = m_timeline.zoomSegments[static_cast<size_t>(m_trackDragIndex)];
                const int64_t previewKeptMs = applyKeptDragToSourceBounds(
                    z.startMs, z.endMs, TrackDragKind::ZoomMove, TrackDragKind::ZoomLeft, m_trackDragIndex, -1);
                m_playheadSec = std::clamp(previewKeptMs / 1000.0, 0.0, keptDurSec);
                rebakeTimelinePreview();
                requestImmediatePreviewRefresh(false);
            }
        } else if (m_trackDragKind == TrackDragKind::AnnMove ||
                   m_trackDragKind == TrackDragKind::AnnLeft ||
                   m_trackDragKind == TrackDragKind::AnnRight) {
            if (m_trackDragIndex >= 0 && m_trackDragIndex < static_cast<int>(m_timeline.annotations.size())) {
                auto& a = m_timeline.annotations[static_cast<size_t>(m_trackDragIndex)];
                const int64_t previewKeptMs = applyKeptDragToSourceBounds(
                    a.startMs, a.endMs, TrackDragKind::AnnMove, TrackDragKind::AnnLeft, -1, m_trackDragIndex);
                resequenceAutoStepBadges();
                m_playheadSec = std::clamp(previewKeptMs / 1000.0, 0.0, keptDurSec);
                requestImmediatePreviewRefresh(false);
            }
        }
        return;
    }

    // 按住左键在标尺或轨道区域拖拽主指针 (Active Scrubbing，靠近拆分点/运镜/标注边界时柔和磁吸，按 Alt 自由微调)
    if (m_scrubbingTimeline && leftButtonDown) {
        const float laneLeft = m_cutLaneRect.left;
        const float w = (std::max)(1.0f, m_cutLaneRect.right - laneLeft);
        const int64_t rawKeptMs = static_cast<int64_t>(std::llround(mapTrackXToKeptSec(lx, laneLeft, w, keptDurSec) * 1000.0));
        const bool altBypassSnap = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const int64_t scrubSnapWinMs = altBypassSnap
            ? 0
            : static_cast<int64_t>(std::llround((6.5 / w) * visSpanMs));
        const auto anchors = StudioTimelineAnalyzer::collectTimelineSnapAnchors(
            m_videoClips, m_timeline.zoomSegments, m_timeline.annotations,
            -1, -1, -1, false);
        const auto ptSnap = StudioTimelineAnalyzer::snapTimelinePoint(rawKeptMs, anchors, scrubSnapWinMs, keptTotalMs);
        m_timelineSnapActive = ptSnap.snapped;
        m_timelineSnapLineKeptMs = ptSnap.snapLineKeptMs;
        m_timelineSnapTargetKind = ptSnap.targetKind;
        m_playheadSec = std::clamp(static_cast<double>(ptSnap.snappedStartMs) / 1000.0, 0.0, keptDurSec);
        m_hoverSkimming = false;
        // 同步高亮拖动所经位置的视频切片
        int clipAtCursor = -1;
        StudioTimelineAnalyzer::mapKeptMsToSourceMs(
            m_videoClips, ptSnap.snappedStartMs, &clipAtCursor);
        if (m_scrubOnLaneArea && clipAtCursor >= 0) {
            m_selectedClipIdx = clipAtCursor;
        }
        requestImmediatePreviewRefresh(false);
        return;
    }

    int newHover = Hit_None;
    int newZoomHover = -1;
    for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
        if (pointInRectF(it->rect, lx, ly)) {
            newHover = it->id;
            if (it->id >= Hit_ZoomSegmentBase && it->id < Hit_VideoClipBase) {
                newZoomHover = it->payload;
            }
            break;
        }
    }

    // 实时感知舞台可见水印角标、标注主体/缩放柄与运镜准星悬停态，驱动拖动光标与虚线感应框
    bool newHoverWm = false;
    bool newHoverWmCorner = false;
    int newStageAnnIdx = -1;
    StageAnnDragKind newStageAnnPart = StageAnnDragKind::None;
    bool newHoverZoomReticle = false;
    if (!m_exportModalOpen && newHover == Hit_StageCanvas) {
        if (m_config.watermark.enabled && m_config.watermark.mode != WatermarkMode::TiledTraceability &&
            m_lastWatermarkStageRect.right > m_lastWatermarkStageRect.left + 4.0f) {
            const float handleX = m_lastWatermarkStageRect.right + 4.0f;
            const float handleY = m_lastWatermarkStageRect.bottom + 4.0f;
            const D2D1_RECT_F wmHit = D2D1::RectF(
                m_lastWatermarkStageRect.left - 6.0f, m_lastWatermarkStageRect.top - 6.0f,
                m_lastWatermarkStageRect.right + 6.0f, m_lastWatermarkStageRect.bottom + 6.0f);
            if (std::hypot(lx - handleX, ly - handleY) <= 13.0f) {
                newHoverWm = true;
                newHoverWmCorner = true;
            } else if (pointInRectF(wmHit, lx, ly)) {
                newHoverWm = true;
            }
        }
        if (!newHoverWm) {
            hitTestStageAnnotation(lx, ly, newStageAnnIdx, newStageAnnPart);
            if (newStageAnnIdx < 0 && m_selectedZoomIdx >= 0 &&
                m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
                const auto& zseg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
                const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
                const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
                float sx = 0.0f, sy = 0.0f;
                if (mapSourceNormToStagePoint(zseg.focusX / srcW, zseg.focusY / srcH, sx, sy)) {
                    newHoverZoomReticle = (std::hypot(lx - sx, ly - sy) <= 15.0f);
                }
            }
        }
    }
    const bool stageHoverChanged =
        (newHoverWm != m_hoveredWatermarkOnStage) ||
        (newHoverWmCorner != m_hoveredWatermarkCorner) ||
        (newStageAnnIdx != m_hoveredStageAnnIdx) ||
        (newStageAnnPart != m_hoveredStageAnnPart) ||
        (newHoverZoomReticle != m_hoveredZoomFocusReticle);
    m_hoveredWatermarkOnStage = newHoverWm;
    m_hoveredWatermarkCorner = newHoverWmCorner;
    m_hoveredStageAnnIdx = newStageAnnIdx;
    m_hoveredStageAnnPart = newStageAnnPart;
    m_hoveredZoomFocusReticle = newHoverZoomReticle;

    int newCoachHover = 0;
    if (!m_exportModalOpen && !leftButtonDown) {
        for (const auto& cz : m_coachHoverZones) {
            if (pointInRectF(cz.rect, lx, ly)) {
                newCoachHover = cz.id;
                break;
            }
        }
    }
    bool coachChanged = false;
    if (newCoachHover != m_hoveredCoachId) {
        const ULONGLONG now64 = GetTickCount64();
        if (newCoachHover != 0) {
            const bool warmChain = (m_hoveredCoachId != 0) ||
                                   (m_coachLastActiveTick > 0 && now64 >= m_coachLastActiveTick &&
                                    (now64 - m_coachLastActiveTick) <= 380);
            m_coachHoverEnterTick = warmChain ? ((now64 > 260) ? (now64 - 260) : 0) : now64;
        }
        m_hoveredCoachId = newCoachHover;
        coachChanged = true;
    }

    // ── FocuSee 级无按键悬停实时轨道掠览 (Live Hover Skimming) ──
    bool skimChanged = false;
    if (!m_exportModalOpen && !leftButtonDown && pointInRectF(m_allLanesHoverRect, lx, ly)) {
        const float laneW = (std::max)(1.0f, m_allLanesHoverRect.right - m_allLanesHoverRect.left);
        const double targetSkimSec = mapTrackXToKeptSec(lx, m_allLanesHoverRect.left, laneW, keptDurSec);
        if (!m_hoverSkimming || std::abs(targetSkimSec - m_hoverSkimSec) > 0.002) {
            m_hoverSkimming = true;
            m_hoverSkimSec = targetSkimSec;
            skimChanged = true;
        }
    } else if (m_hoverSkimming) {
        m_hoverSkimming = false;
        skimChanged = true;
    }

    if (skimChanged) {
        m_hoveredHitId = newHover;
        m_hoveredZoomIdx = newZoomHover;
        requestImmediatePreviewRefresh(!m_hoverSkimming);
        return;
    }

    if (newHover != m_hoveredHitId || newZoomHover != m_hoveredZoomIdx || coachChanged || stageHoverChanged) {
        m_hoveredHitId = newHover;
        m_hoveredZoomIdx = newZoomHover;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::handleMouseUp() {
    if (m_pressedPresetCardIdx >= 0) {
        const int cardIdx = m_pressedPresetCardIdx;
        const bool wasDragging = m_draggingPresetToStage;
        m_pressedPresetCardIdx = -1;
        m_draggingPresetToStage = false;
        ReleaseCapture();

        if (wasDragging) {
            float dropNx = 0.5f, dropNy = 0.5f;
            if (pointInRectF(m_lastBitmapDrawRect, m_lastMouseLx, m_lastMouseLy) &&
                mapStagePointToSourceNorm(m_lastMouseLx, m_lastMouseLy, dropNx, dropNy)) {
                insertAnnotationCardPreset(cardIdx, &dropNx, &dropNy);
            } else {
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        } else {
            insertAnnotationCardPreset(cardIdx, nullptr, nullptr);
        }
        SetCursor(resolveDesiredCursor(m_lastMouseLx, m_lastMouseLy));
        return;
    }

    const bool hadWmDrag = (m_watermarkStageDragMode != 0);
    const bool hadZoomFocusDrag = m_draggingZoomFocus;
    const SliderDragKind endedSlider = m_activeSlider;
    const bool hadCapture = m_scrubbingTimeline || m_draggingWindow ||
                            m_activeSlider != SliderDragKind::None ||
                            m_trackDragKind != TrackDragKind::None ||
                            m_stageAnnDrag != StageAnnDragKind::None ||
                            m_draggingZoomFocus || hadWmDrag;
    m_scrubbingTimeline = false;
    m_scrubOnLaneArea = false;
    m_draggingWindow = false;
    m_activeSlider = SliderDragKind::None;
    m_trackDragKind = TrackDragKind::None;
    m_trackDragIndex = -1;
    m_trackDragOrigPlayheadKeptMs = -1;
    m_timelineSnapActive = false;
    m_timelineSnapTargetKind = TimelineSnapTargetKind::None;
    m_stageAnnDrag = StageAnnDragKind::None;
    m_draggingZoomFocus = false;
    m_watermarkStageDragMode = 0;
    if (hadWmDrag) {
        WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
    }
    if (endedSlider == SliderDragKind::Padding ||
        endedSlider == SliderDragKind::InsetBorder ||
        endedSlider == SliderDragKind::Radius ||
        endedSlider == SliderDragKind::Shadow ||
        endedSlider == SliderDragKind::BgBlur ||
        endedSlider == SliderDragKind::ZoomFactor ||
        endedSlider == SliderDragKind::ZoomPacing ||
        endedSlider == SliderDragKind::ZoomSensitivity ||
        endedSlider == SliderDragKind::FocusSpotlight) {
        persistStudioConfigToSettings(m_config);
    }
    if (hadZoomFocusDrag) {
        rebakeTimelinePreview();
    }
    if (hadCapture) {
        ReleaseCapture();
        resequenceAutoStepBadges();
        SetCursor(resolveDesiredCursor(m_lastMouseLx, m_lastMouseLy));
        requestImmediatePreviewRefresh(true);
    }
}

void StudioPreviewWindow::handleCharInput(wchar_t ch) {
    if (m_preparingSession || m_exporting.load(std::memory_order_acquire)) return;

    if (m_exportModalOpen && m_editingExportName) {
        if (ch == L'\b') {
            if (!m_exportFileNameStem.empty()) {
                m_exportFileNameStem.pop_back();
                m_userEditedExportStem = true;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return;
        }
        if (ch == L'\r' || ch == L'\n') {
            m_editingExportName = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (isValidFileNameChar(ch) && m_exportFileNameStem.size() < 96) {
            m_exportFileNameStem.push_back(ch);
            m_userEditedExportStem = true;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    if (!m_exportModalOpen && m_editingWatermarkText) {
        std::wstring wtxt = tools3000::core::WinUtils::utf8ToWstring(m_config.watermark.textTemplate);
        if (ch == L'\b') {
            if (!wtxt.empty()) {
                wtxt.pop_back();
                m_config.watermark.textTemplate = tools3000::core::WinUtils::wstringToUtf8(wtxt);
                WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (ch == L'\r' || ch == L'\n') {
            m_editingWatermarkText = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (ch >= 32 && wtxt.size() < 110) {
            wtxt.push_back(ch);
            m_config.watermark.textTemplate = tools3000::core::WinUtils::wstringToUtf8(wtxt);
            m_config.watermark.enabled = true;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
        }
        return;
    }

    if (!m_exportModalOpen && m_editingAnnotationText &&
        m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
        auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
        std::wstring wtxt = tools3000::core::WinUtils::utf8ToWstring(ann.text);
        if (ch == L'\b') {
            if (!wtxt.empty()) {
                wtxt.pop_back();
                ann.text = tools3000::core::WinUtils::wstringToUtf8(wtxt);
                if (ann.kind == StudioAnnotationKind::StepBadge) {
                    ann.customStepText = !ann.text.empty();
                    if (!ann.customStepText) resequenceAutoStepBadges();
                }
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (ch == L'\r' || ch == L'\n') {
            m_editingAnnotationText = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (ch >= 32 && wtxt.size() < 64) {
            wtxt.push_back(ch);
            ann.text = tools3000::core::WinUtils::wstringToUtf8(wtxt);
            if (ann.kind == StudioAnnotationKind::StepBadge) {
                ann.customStepText = true;
            }
            requestImmediatePreviewRefresh(true);
        }
    }
}

void StudioPreviewWindow::handleKeyDown(WPARAM vk) {
    if (m_preparingSession || m_exporting.load(std::memory_order_acquire)) return;

    const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    if (m_closeConfirmModalOpen) {
        if (vk == VK_ESCAPE) {
            m_closeConfirmModalOpen = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    if (m_exportModalOpen) {
        if (m_exportCompletedCard) {
            if (vk == VK_ESCAPE) {
                m_exportModalOpen = false;
                m_exportCompletedCard = false;
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
            if (ctrlDown && vk == 'C') {
                copyLastExportedToClipboard();
                return;
            }
            if (vk == VK_RETURN) {
                finishExportedSessionAndClose();
                return;
            }
            return;
        }

        if (vk == VK_ESCAPE) {
            if (m_openDropdown != ExportDropdownKind::None) {
                m_openDropdown = ExportDropdownKind::None;
            } else if (m_editingExportName) {
                m_editingExportName = false;
            } else {
                m_exportModalOpen = false;
            }
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (m_editingExportName && ctrlDown && vk == 'V') {
            if (OpenClipboard(m_hwnd)) {
                if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                    if (const auto* wptr = static_cast<const wchar_t*>(GlobalLock(hData))) {
                        for (size_t i = 0; wptr[i] != L'\0' && m_exportFileNameStem.size() < 96; ++i) {
                            if (isValidFileNameChar(wptr[i])) {
                                m_exportFileNameStem.push_back(wptr[i]);
                                m_userEditedExportStem = true;
                            }
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return;
        }
        if (!m_editingExportName && vk == VK_RETURN) {
            startAsyncExport();
        }
        return;
    }

    if (m_editingWatermarkText) {
        if (vk == VK_ESCAPE) {
            m_editingWatermarkText = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (ctrlDown && vk == 'V') {
            if (OpenClipboard(m_hwnd)) {
                if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                    if (const auto* wptr = static_cast<const wchar_t*>(GlobalLock(hData))) {
                        std::wstring cur = tools3000::core::WinUtils::utf8ToWstring(m_config.watermark.textTemplate);
                        for (size_t i = 0; wptr[i] != L'\0' && cur.size() < 110; ++i) {
                            if (wptr[i] >= 32) cur.push_back(wptr[i]);
                        }
                        m_config.watermark.textTemplate = tools3000::core::WinUtils::wstringToUtf8(cur);
                        m_config.watermark.enabled = true;
                        WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        return;
    }

    if (m_editingAnnotationText) {
        if (vk == VK_ESCAPE) {
            m_editingAnnotationText = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (ctrlDown && vk == 'V' &&
            m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
            if (OpenClipboard(m_hwnd)) {
                if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                    if (const auto* wptr = static_cast<const wchar_t*>(GlobalLock(hData))) {
                        auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
                        std::wstring cur = tools3000::core::WinUtils::utf8ToWstring(ann.text);
                        for (size_t i = 0; wptr[i] != L'\0' && cur.size() < 64; ++i) {
                            if (wptr[i] >= 32) cur.push_back(wptr[i]);
                        }
                        ann.text = tools3000::core::WinUtils::wstringToUtf8(cur);
                        if (ann.kind == StudioAnnotationKind::StepBadge) {
                            ann.customStepText = !ann.text.empty();
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        return;
    }

    // Ctrl+E 或 Ctrl+Enter：一键呼出演播室导出配置模态框
    if (ctrlDown && (vk == 'E' || vk == VK_RETURN)) {
        m_playing = false;
        m_hoverSkimming = false;
        m_exportModalOpen = true;
        m_exportCompletedCard = false;
        m_editingExportName = false;
        m_openDropdown = ExportDropdownKind::None;
        ensureNonCollidingExportStem();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Ctrl+Z 撤销 / Ctrl+Y 重做
    if (ctrlDown && vk == 'Z') {
        performUndo();
        return;
    }
    if (ctrlDown && vk == 'Y') {
        performRedo();
        return;
    }

    // S 或 Ctrl+B：在当前指针（若正在轨道上悬停掠览则优先在悬停位置）一刀拆分切片
    if ((!ctrlDown && vk == 'S') || (ctrlDown && vk == 'B')) {
        splitClipAtCurrentCursor(true);
        return;
    }

    if (vk == VK_DELETE || vk == VK_BACK) {
        deleteSelectedTimelineEntity();
        return;
    }

    if (vk == VK_HOME) {
        m_playing = false;
        m_hoverSkimming = false;
        m_playheadSec = 0.0;
        m_timelineViewStartSec = 0.0;
        requestImmediatePreviewRefresh(true);
        return;
    }

    if (vk == VK_UP || vk == VK_DOWN || vk == VK_OEM_4 || vk == VK_OEM_6) {
        // NLE 关键剪辑点秒跳：Up / '[' 跳转至上一个拆分点/运镜/标注边界，Down / ']' 跳转至下一个关键点
        m_playing = false;
        m_hoverSkimming = false;
        const double keptDurSec = computeEffectiveDurationSec();
        const int64_t curKeptMs = static_cast<int64_t>(std::llround(m_playheadSec * 1000.0));
        const auto anchors = StudioTimelineAnalyzer::collectTimelineSnapAnchors(
            m_videoClips, m_timeline.zoomSegments, m_timeline.annotations, -1, -1, -1, false);
        const int dir = (vk == VK_DOWN || vk == VK_OEM_6) ? +1 : -1;
        const int64_t nextMs = StudioTimelineAnalyzer::findNextTimelineKeyframeMs(curKeptMs, dir, anchors);
        if (nextMs >= 0) {
            m_playheadSec = std::clamp(static_cast<double>(nextMs) / 1000.0, 0.0, keptDurSec);
            if (m_timelineZoom > 1.01f) {
                const double visSpan = visibleTimelineSpanSec(keptDurSec);
                if (m_playheadSec < m_timelineViewStartSec || m_playheadSec > m_timelineViewStartSec + visSpan) {
                    m_timelineViewStartSec = m_playheadSec - visSpan * 0.25;
                    clampTimelineViewWindow(keptDurSec);
                }
            }
            requestImmediatePreviewRefresh(true);
        }
        return;
    }

    if (vk == VK_LEFT || vk == VK_RIGHT) {
        m_playing = false;
        m_hoverSkimming = false;
        const double keptDurSec = computeEffectiveDurationSec();
        const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const double step = (shiftDown ? 0.5 : (1.0 / 30.0)) * ((vk == VK_RIGHT) ? 1.0 : -1.0);
        m_playheadSec = std::clamp(m_playheadSec + step, 0.0, keptDurSec);
        if (m_timelineZoom > 1.01f) {
            const double visSpan = visibleTimelineSpanSec(keptDurSec);
            if (m_playheadSec < m_timelineViewStartSec) m_timelineViewStartSec = m_playheadSec;
            else if (m_playheadSec > m_timelineViewStartSec + visSpan) m_timelineViewStartSec = m_playheadSec - visSpan;
            clampTimelineViewWindow(keptDurSec);
        }
        requestImmediatePreviewRefresh(true);
        return;
    }

    if (ctrlDown && (vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS || vk == VK_SUBTRACT || vk == '0')) {
        const double keptDurSec = computeEffectiveDurationSec();
        if (vk == '0') {
            setTimelineZoomAnchored(1.0f, m_playheadSec, 0.5f, keptDurSec);
        } else {
            const bool zoomIn = (vk == VK_OEM_PLUS || vk == VK_ADD);
            setTimelineZoomAnchored(m_timelineZoom * (zoomIn ? 1.45f : (1.0f / 1.45f)),
                                    m_playheadSec, 0.5f, keptDurSec);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    if (vk == VK_SPACE) {
        m_hoverSkimming = false;
        m_playing = !m_playing;
        m_lastTickMs = GetTickCount();
        requestImmediatePreviewRefresh(true);
        return;
    }
    if (vk == VK_ESCAPE) {
        requestCloseWithConfirm();
    }
}

LRESULT CALLBACK StudioPreviewWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (tools3000::ui::KeyboardPipeline::filterWindowMessage(hwnd, msg, wParam, lParam)) {
        return 0;
    }

    StudioPreviewWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<StudioPreviewWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<StudioPreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_STUDIO_CUSTOM_BG_PICKED:
            self->onCustomWallpaperPicked();
            return 0;
        case WM_STUDIO_EXPORT_DIR_PICKED:
            self->onExportDirectoryPicked();
            return 0;
        case WM_STUDIO_WATERMARK_IMG_PICKED:
            self->onWatermarkLogoPicked();
            return 0;
        case WM_NCCALCSIZE: {
            if (wParam) {
                if (IsZoomed(hwnd)) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                    const HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    if (hMon && GetMonitorInfoW(hMon, &mi)) {
                        params->rgrc[0] = mi.rcWork;
                    }
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (mmi) {
                const float s = (std::max)(1.0f, self->m_dpiScale);
                const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (mon && GetMonitorInfoW(mon, &mi)) {
                    const int workW = (std::max)(640, static_cast<int>(mi.rcWork.right - mi.rcWork.left));
                    const int workH = (std::max)(480, static_cast<int>(mi.rcWork.bottom - mi.rcWork.top));
                    mmi->ptMinTrackSize.x = (std::min<LONG>)(tools3000::core::dpi::scaleMetric(1020, s), static_cast<LONG>(workW * 0.92f));
                    mmi->ptMinTrackSize.y = (std::min<LONG>)(tools3000::core::dpi::scaleMetric(650, s), static_cast<LONG>(workH * 0.92f));
                    mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                    mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                    mmi->ptMaxSize.x = workW;
                    mmi->ptMaxSize.y = workH;
                } else {
                    mmi->ptMinTrackSize.x = tools3000::core::dpi::scaleMetric(1020, s);
                    mmi->ptMinTrackSize.y = tools3000::core::dpi::scaleMetric(650, s);
                }
            }
            return 0;
        }
        case WM_NCHITTEST: {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const int sx = GET_X_LPARAM(lParam);
            const int sy = GET_Y_LPARAM(lParam);
            const float s = (std::max)(1.0f, self->m_dpiScale);
            const bool zoomed = (IsZoomed(hwnd) != FALSE);

            if (!zoomed && !self->m_exportModalOpen) {
                const int border = tools3000::core::dpi::scaleMetric(8, s);
                const int corner = tools3000::core::dpi::scaleMetric(16, s);

                const bool onLeft   = (sx >= wr.left && sx < wr.left + border);
                const bool onRight  = (sx <= wr.right && sx > wr.right - border);
                const bool onTop    = (sy >= wr.top && sy < wr.top + border);
                const bool onBottom = (sy <= wr.bottom && sy > wr.bottom - border);

                if ((sx < wr.left + corner && onTop) || (sy < wr.top + corner && onLeft)) return HTTOPLEFT;
                if ((sx > wr.right - corner && onTop) || (sy < wr.top + corner && onRight)) return HTTOPRIGHT;
                if ((sx < wr.left + corner && onBottom) || (sy > wr.bottom - corner && onLeft)) return HTBOTTOMLEFT;
                if ((sx > wr.right - corner && onBottom) || (sy > wr.bottom - corner && onRight)) return HTBOTTOMRIGHT;
                if (onLeft) return HTLEFT;
                if (onRight) return HTRIGHT;
                if (onTop) return HTTOP;
                if (onBottom) return HTBOTTOM;
            }

            if (!self->m_exportModalOpen) {
                POINT cliPt{sx, sy};
                ScreenToClient(hwnd, &cliPt);
                const float lx = cliPt.x / s;
                const float ly = cliPt.y / s;
                const float logicalW = (wr.right - wr.left) / s;
                // 顶部标题栏空白区返回原生 HTCAPTION：支持 Aero Snap 边缘贴靠、双击最大化与 Win+方向键分屏
                if (ly >= 8.0f && ly <= 45.0f && lx >= 8.0f && lx < (logicalW - 324.0f)) {
                    return HTCAPTION;
                }
            }
            return HTCLIENT;
        }
        case WM_ENTERSIZEMOVE:
            self->m_inInteractiveSizing = true;
            return 0;
        case WM_EXITSIZEMOVE: {
            self->m_inInteractiveSizing = false;
            if (!IsZoomed(hwnd)) {
                RECT wr{};
                if (GetWindowRect(hwnd, &wr)) {
                    const int pw = wr.right - wr.left;
                    const int ph = wr.bottom - wr.top;
                    if (pw >= 640 && ph >= 480) {
                        const float s = (std::max)(1.0f, self->m_dpiScale);
                        self->m_logicalWidth = std::clamp(static_cast<int>(std::round(pw / s)), 1020, 3840);
                        self->m_logicalHeight = std::clamp(static_cast<int>(std::round(ph / s)), 650, 2160);
                        self->m_restoreWindowRect = wr;
                        auto& cm = tools3000::core::ConfigManager::instance();
                        cm.set<int>("/recording/studioWindowWidth", self->m_logicalWidth);
                        cm.set<int>("/recording/studioWindowHeight", self->m_logicalHeight);
                    }
                    self->applyWindowRoundingForCurrentState(pw, ph);
                }
            }
            // 窗口拖拽缩放停稳瞬间，按最新舞台物理像素尺寸立即重烘 100% 视网膜原生超清画面
            self->updateCurrentComposedBitmap();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND: {
            if (!self->m_renderTarget && wParam) {
                RECT rc{};
                if (GetClientRect(hwnd, &rc)) {
                    const bool dark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
                    HBRUSH bg = CreateSolidBrush(dark ? RGB(14, 18, 27) : RGB(247, 249, 252));
                    FillRect(reinterpret_cast<HDC>(wParam), &rc, bg);
                    DeleteObject(bg);
                }
            }
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            self->render();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kPlaybackTimerId) {
                self->onPlaybackTick();
                return 0;
            }
            break;
        case WM_MOUSELEAVE:
            self->handleMouseLeave();
            return 0;
        case WM_DPICHANGED: {
            self->m_dpiScale = tools3000::core::dpi::scaleForDpi(HIWORD(wParam));
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                const int w = suggested->right - suggested->left;
                const int h = suggested->bottom - suggested->top;
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                self->applyWindowRoundingForCurrentState(w, h);
            }
            self->discardDeviceResources();
            self->updateCurrentComposedBitmap();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE: {
            if (wParam != SIZE_MINIMIZED) {
                const int pw = LOWORD(lParam);
                const int ph = HIWORD(lParam);
                const bool zoomed = (wParam == SIZE_MAXIMIZED || IsZoomed(hwnd) != FALSE);
                if (zoomed != self->m_isMaximized) {
                    self->m_isMaximized = zoomed;
                    tools3000::core::ConfigManager::instance().set<bool>("/recording/studioWindowMaximized", zoomed);
                }
                if (self->m_renderTarget && pw > 0 && ph > 0) {
                    self->m_renderTarget->Resize(D2D1::SizeU(static_cast<UINT32>(pw), static_cast<UINT32>(ph)));
                }
                if (!zoomed && pw >= 640 && ph >= 480) {
                    const float s = (std::max)(1.0f, self->m_dpiScale);
                    self->m_logicalWidth = std::clamp(static_cast<int>(std::round(pw / s)), 1020, 3840);
                    self->m_logicalHeight = std::clamp(static_cast<int>(std::round(ph / s)), 650, 2160);
                    GetWindowRect(hwnd, &self->m_restoreWindowRect);
                }
                self->m_stageViewportPhysW = (std::max)(320, pw - tools3000::core::dpi::scaleMetric(400, self->m_dpiScale));
                self->m_stageViewportPhysH = (std::max)(240, ph - tools3000::core::dpi::scaleMetric(256, self->m_dpiScale));
                self->applyWindowRoundingForCurrentState(pw, ph);
                if (!self->m_inInteractiveSizing && !self->m_previewDeckBgr.empty()) {
                    self->updateCurrentComposedBitmap();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleMouseWheel(pt.x / s, pt.y / s, GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleMouseDown(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s, false);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleMouseDown(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s, true);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleRightMouseDown(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s);
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(self->resolveDesiredCursor(self->m_lastMouseLx, self->m_lastMouseLy));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            const float lx = GET_X_LPARAM(lParam) / s;
            const float ly = GET_Y_LPARAM(lParam) / s;
            self->handleMouseMove(lx, ly, (wParam & MK_LBUTTON) != 0);
            SetCursor(self->resolveDesiredCursor(lx, ly));
            return 0;
        }
        case WM_LBUTTONUP:
            self->handleMouseUp();
            return 0;
        case WM_CHAR:
            self->handleCharInput(static_cast<wchar_t>(wParam));
            return 0;
        case WM_KEYDOWN:
            self->handleKeyDown(wParam);
            return 0;
        case WM_CLOSE:
            if (!self->m_exporting.load(std::memory_order_acquire)) {
                self->requestCloseWithConfirm();
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture

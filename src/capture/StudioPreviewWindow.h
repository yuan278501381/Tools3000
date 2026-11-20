#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioPreviewWindow.h — 录屏结束瞬时弹出的演播室非线性编辑与多格式导出工作台
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOPREVIEWWINDOW_H
#define TOOLS3000_CAPTURE_STUDIOPREVIEWWINDOW_H

#include "capture/StudioTimeline.h"
#include "capture/StudioVideoCompositor.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tools3000::capture {

class StudioPreviewWindow {
public:
    using CompletionHandler = std::function<void(const std::string& savedPath, double durationSec, bool studioExported)>;
    static constexpr size_t kWallpaperCols = 7;
    static constexpr size_t kWallpaperRows = 4;
    static constexpr size_t kWallpaperSlotsPerPage = kWallpaperCols * kWallpaperRows; // 28 个紧凑正方形缩略图

    static StudioPreviewWindow& instance();

    /// 停止录制瞬间 0ms 立即弹出 FocuSee 级微晶流光「正在准备演播室」等待界面（支持超长录屏后台封装期间 60FPS 丝滑反馈）
    bool showPreparingOverlay(double recordedDurationSec);
    void dismissPreparingOverlay();

    /// 在 UI 主线程瞬时打开演播室实时预览与编辑窗口（支持直传内存预存预览牌组实现 0ms 秒开）
    bool showSession(
        const std::string& rawVideoPathUtf8,
        StudioTimelineData timeline,
        const StudioRenderConfig& initialConfig,
        int bitrateMbps,
        CompletionHandler onFinished = nullptr,
        std::vector<cv::Mat> preloadedDeck = {});

    /// 关闭窗口并安全回收后台导出线程、STA 选图线程与预览帧工作集
    void shutdown();

    bool isVisible() const noexcept;
    bool isCloseConfirmModalOpen() const noexcept { return m_closeConfirmModalOpen; }
    void reloadTheme();

    /// 判断给定渲染配置是否处于「纯净原样输出」预设（无背景边框、无自动运镜、原始画布比例）
    static bool isRawOutputModeConfig(const StudioRenderConfig& cfg) noexcept;

    /// 判断当前会话是否存在需要重新编码的演播室美化、运镜、标注、水印、字幕或剪辑改动
    static bool hasStudioEditsToRender(
        const StudioRenderConfig& cfg,
        const StudioTimelineData& timeline,
        const std::vector<StudioVideoClip>& clips,
        int64_t rawTotalMs) noexcept;

    /// 离屏渲染底部工具按钮微缩剧场卡片（供自动化测试与视觉快照校验，coachSlot = 0..6）
    static cv::Mat renderCoachCardPreviewForTest(int coachSlot, float phase01, bool dark, bool isZh = true);

    /// 离屏渲染完整时间轴与胶片声波主轨快照（供视觉回归审查与真实音频包络检验）
    static cv::Mat renderTimelineStripPreviewForTest(
        const std::vector<cv::Mat>& deckBgr,
        const std::vector<float>& audioPeaks,
        double durationSec,
        double playheadSec,
        bool dark,
        bool isZh = true);

private:
    StudioPreviewWindow() = default;
    ~StudioPreviewWindow() { shutdown(); }
    StudioPreviewWindow(const StudioPreviewWindow&) = delete;
    StudioPreviewWindow& operator=(const StudioPreviewWindow&) = delete;

    struct HitZone {
        int id = 0;
        D2D1_RECT_F rect{};
        int payload = -1;
    };

    struct TimelineUndoSnapshot {
        std::vector<StudioVideoClip> videoClips;
        std::vector<StudioZoomSegment> zoomSegments;
        std::vector<StudioAnnotationItem> annotations;
        double playheadKeptSec = 0.0;
    };

    enum class LeftRailTab : int {
        CanvasBg     = 0, // 视频尺寸 / 内边距 / 内边框 / 圆角 / 阴影 / 背景模糊 / 28 宫格背景
        Annotations  = 1, // 文本(6款) / 形状(9款) / 遮罩与放大(4款) 可视化标注库
        CameraCursor = 2, // 镜头 2D/3D 运镜 / 倍率 / 动感模糊 / 矢量光标 / 聚光环
        Watermark    = 3, // 企业溯源斜铺水印 / 品牌图片 Logo 导入与四角吸附定位
        Captions     = 4, // 离线语音字幕对齐与导出选项
    };

    enum class ExportDropdownKind : int {
        None       = 0,
        Format     = 1,
        Resolution = 2,
        Fps        = 3,
        Quality    = 4,
    };

    enum class TrackDragKind : int {
        None      = 0,
        ZoomMove  = 1,
        ZoomLeft  = 2,
        ZoomRight = 3,
        AnnMove   = 4,
        AnnLeft   = 5,
        AnnRight  = 6,
    };

    enum class StageAnnDragKind : int {
        None           = 0,
        MoveBody       = 1,
        DragEndPoint   = 2,
        DragStartPoint = 3,
    };

    enum class SliderDragKind : int {
        None             = 0,
        Padding          = 1,
        InsetBorder      = 2,
        Radius           = 3,
        Shadow           = 4,
        BgBlur           = 5,
        WatermarkScale   = 6,
        WatermarkOpacity = 7,
        TimelineZoom     = 8,
        TimelinePan      = 9,
        ZoomFactor       = 10,
        ZoomPacing       = 11,
        ZoomSensitivity  = 12,
        FocusSpotlight   = 13,
    };

    struct WallpaperEntry {
        bool isBuiltin = true;
        BeautyBackgroundType builtinTheme = BeautyBackgroundType::StudioSlate;
        std::string filePathUtf8;
        std::wstring displayName;
        int category = 0; // 0: 默认, 1: 沉稳(暗色), 2: 轻盈(浅色), 3: 风景, 4: 自定义
    };

    bool ensureWindowCreated();
    void toggleMaximizeRestore();
    void applyWindowRoundingForCurrentState(int physW, int physH);
    bool ensureRenderResources();
    void discardDeviceResources();
    void discoverLocalWallpapers();
    void rebuildFilteredWallpapers();
    void invalidateWallpaperPageBitmaps();
    void browseCustomWallpaperImageAsync();
    void browseWatermarkLogoImageAsync();
    void browseExportDirectoryAsync();
    void joinFileDialogWorkerIfDone(bool wait);
    void onCustomWallpaperPicked();
    void onWatermarkLogoPicked();
    void onExportDirectoryPicked();

    // 磁性波纹切片剪辑 (Split -> Select -> Delete / Speed) 与 Undo/Redo
    void pushUndoSnapshot();
    void performUndo();
    void performRedo();
    void syncCutSegmentsFromVideoClips();
    void splitClipAtCurrentCursor(bool preferSkimmer = false);
    void deleteSelectedTimelineEntity();
    void cycleSelectedClipSpeed();
    double currentDisplayedSourceSec() const noexcept;
    void requestImmediatePreviewRefresh(bool forceNow = false);

    void resequenceAutoStepBadges();
    void insertAnnotationCardPreset(int cardIndex, const float* dropNormX = nullptr, const float* dropNormY = nullptr);
    bool hitTestStageAnnotation(float lx, float ly, int& outAnnIdx, StageAnnDragKind& outPart) const;
    HCURSOR resolveDesiredCursor(float lx, float ly) const;
    void rebakeTimelinePreview();
    void updateCurrentComposedBitmap();
    double computeEffectiveDurationSec() const;
    double visibleTimelineSpanSec(double keptDurSec) const noexcept;
    void clampTimelineViewWindow(double keptDurSec) noexcept;
    void setTimelineZoomAnchored(float newZoom, double anchorKeptSec, float anchorNorm01, double keptDurSec) noexcept;
    float mapKeptSecToTrackX(double keptSec, float laneLeft, float trackW, double keptDurSec) const noexcept;
    double mapTrackXToKeptSec(float lx, float laneLeft, float trackW, double keptDurSec) const noexcept;
    bool mapStagePointToSourceNorm(float lx, float ly, float& outNormX, float& outNormY) const;
    bool mapSourceNormToStagePoint(float normX, float normY, float& outLx, float& outLy) const;
    void applySliderAtX(SliderDragKind kind, float lx);
    void applyZoomPreset(StudioZoomPreset preset);
    void rebuildAutoZoomSegmentsLive();
    void initExportModalDefaults();
    void applyFormatSmartDefaults(StudioExportFormat newFmt);
    void ensureNonCollidingExportStem();
    void persistExportPreferencesIfChecked();
    std::string buildFinalExportPathUtf8() const;
    void render();
    void renderLeftRailAndDrawer(const D2D1_RECT_F& railRc, const D2D1_RECT_F& drawerRc,
                                 bool dark, bool isZh, ID2D1SolidColorBrush* brush);
    void renderAnnotationDrawer(float padL, float padR, float startY, float bottomLimit,
                                bool dark, bool isZh, ID2D1SolidColorBrush* brush);
    void renderWatermarkDrawer(float padL, float padR, float startY, float bottomLimit,
                               bool dark, bool isZh, ID2D1SolidColorBrush* brush);
    void renderCoachTooltipOverlay(float W, float H, float minLeft,
                                   bool dark, bool isZh, ID2D1SolidColorBrush* brush);
    void renderExportModalOverlay(float W, float H, bool dark, bool isZh, ID2D1SolidColorBrush* brush);
    void renderUnsavedCloseModalOverlay(float W, float H, bool dark, bool isZh, ID2D1SolidColorBrush* brush);
    void renderPreparingOverlay(float W, float H, bool dark, bool isZh, ID2D1SolidColorBrush* brush);
    void onPlaybackTick();
    void startAsyncExport();
    void copyLastExportedToClipboard();
    bool hasStudioEditsToRender() const noexcept;
    bool isRawOutputModeActive() const noexcept;
    void toggleRawOutputPreset();
    void finishExportedSessionAndClose();
    void closeOrKeepBySessionState();
    void requestCloseWithConfirm();
    void keepOriginalAndClose();
    void discardOriginalAndClose();
    void closeSessionAndCleanup();
    void joinExportWorkerIfDone(bool wait);
    void handleMouseDown(float lx, float ly, bool isDoubleClick = false);
    void handleRightMouseDown(float lx, float ly);
    void handleMouseMove(float lx, float ly, bool leftButtonDown);
    void handleMouseUp();
    void handleMouseWheel(float lx, float ly, int wheelDelta);
    void handleMouseLeave();
    void handleCharInput(wchar_t ch);
    void handleKeyDown(WPARAM vk);

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;
    int m_logicalWidth = 1260;
    int m_logicalHeight = 776;
    bool m_isMaximized = false;
    RECT m_restoreWindowRect{0, 0, 0, 0};

    std::string m_rawVideoPath;
    std::string m_masterVideoPath; // 会话期只读无壁纸纯净母带路径（同名导出前自动原子隔离保护）
    bool m_masterIsIsolatedTemp = false;
    StudioTimelineData m_timeline;
    StudioRenderConfig m_config;
    int m_bitrateMbps = 10;
    double m_durationSec = 0.0; // 原始录屏底片总时长 (秒)
    CompletionHandler m_onFinished;

    // 磁性波纹视频切片列表与 40 步无损撤销/重做栈
    std::vector<StudioVideoClip> m_videoClips;
    std::vector<TimelineUndoSnapshot> m_undoStack;
    std::vector<TimelineUndoSnapshot> m_redoStack;

    // 高帧率轻量预览帧牌组、驻留式原片全清解码器与预烘焙时间轴状态
    std::vector<cv::Mat> m_previewDeckBgr;
    StudioHiResFrameReader m_hiResReader;
    int m_origWidth = 0;
    int m_origHeight = 0;
    int m_stageViewportPhysW = 1280;
    int m_stageViewportPhysH = 720;
    bool m_inInteractiveSizing = false;
    bool m_lastComposedUsedProxy = false;
    ULONGLONG m_lastFastScrubTick = 0;
    int m_previewEvalFps = 30;
    std::vector<StudioEvaluatedFrame> m_evaluatedFrames;
    StudioVideoCompositor m_previewCompositor;
    cv::Mat m_currentComposedBgra;
    bool m_composedDirty = true;
    int64_t m_lastComposeQpcTick = 0;
    bool m_pendingSkimCompose = false;

    // 舞台几何映射缓存
    D2D1_RECT_F m_lastBitmapDrawRect{};
    D2D1_RECT_F m_wallpaperGridRect{};
    D2D1_RECT_F m_allLanesHoverRect{};
    int m_lastInnerPadX = 0;
    int m_lastInnerPadY = 0;
    int m_lastInnerW = 0;
    int m_lastInnerH = 0;
    StudioCameraFrameState m_lastEvalCamera{};

    // 左侧导航栏与舞台标注交互状态
    LeftRailTab m_activeTab = LeftRailTab::CanvasBg;
    int m_nextStepBadgeNumber = 1;
    bool m_editingAnnotationText = false;
    StageAnnDragKind m_stageAnnDrag = StageAnnDragKind::None;
    int m_hoveredStageAnnIdx = -1;
    StageAnnDragKind m_hoveredStageAnnPart = StageAnnDragKind::None;
    bool m_draggingZoomFocus = false;
    bool m_hoveredZoomFocusReticle = false;
    int m_pressedPresetCardIdx = -1;
    bool m_draggingPresetToStage = false;
    float m_presetDragStartLx = 0.0f;
    float m_presetDragStartLy = 0.0f;
    float m_lastMouseLx = 0.0f;
    float m_lastMouseLy = 0.0f;
    float m_annDragStartNormX = 0.0f;
    float m_annDragStartNormY = 0.0f;
    float m_annOrigX0 = 0.0f, m_annOrigY0 = 0.0f, m_annOrigX1 = 0.0f, m_annOrigY1 = 0.0f;

    // 播放、悬停实时掠览 (Live Skimming) 与三轨交互状态
    bool m_playing = true;
    double m_playheadSec = 0.0;      // 磁性成片时间轴主指针 [0 .. keptDurSec]
    bool m_hoverSkimming = false;    // 鼠标在轨道区滑动实时预览态
    double m_hoverSkimSec = 0.0;     // 悬停掠览指针对应的磁性成片时间 [0 .. keptDurSec]
    bool m_trackingMouseLeave = false;
    DWORD m_lastTickMs = 0;
    bool m_scrubbingTimeline = false;
    bool m_scrubOnLaneArea = false;
    bool m_draggingWindow = false;
    POINT m_dragStartScreen{};
    POINT m_windowStartPos{};
    int m_hoveredHitId = 0;
    int m_hoveredZoomIdx = -1;
    int m_selectedClipIdx = -1;
    int m_selectedZoomIdx = -1;
    int m_selectedAnnIdx = -1;
    TrackDragKind m_trackDragKind = TrackDragKind::None;
    int m_trackDragIndex = -1;
    float m_trackDragStartX = 0.0f;
    int64_t m_trackDragOrigKeptStartMs = 0;
    int64_t m_trackDragOrigKeptEndMs = 0;
    int64_t m_trackDragOrigPlayheadKeptMs = -1;
    int64_t m_trackDragCurrentKeptStartMs = 0;
    int64_t m_trackDragCurrentKeptEndMs = 0;
    bool m_timelineSnapActive = false;
    int64_t m_timelineSnapLineKeptMs = 0;
    TimelineSnapTargetKind m_timelineSnapTargetKind = TimelineSnapTargetKind::None;
    D2D1_RECT_F m_timelineTrackRect{};
    D2D1_RECT_F m_timelineCardRect{};
    D2D1_RECT_F m_cutLaneRect{};
    D2D1_RECT_F m_zoomLaneRect{};
    D2D1_RECT_F m_annLaneRect{};
    float m_timelineZoom = 1.0f;          // 时间轴横向视野缩放倍率 [1.0x 全览 .. 24.0x 精修]
    double m_timelineViewStartSec = 0.0;  // 放大态可见窗口起始成片秒数
    float m_waveformReferencePeak = 1.0f;
    float m_waveformNoiseFloor = 0.008f;
    bool m_hasActiveAudioWaveform = false;
    StudioWaveformEnvelopeConfig m_waveformProfile{};
    std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap>> m_filmstripBitmaps;
    bool m_filmstripDirty = true;
    bool m_preparingSession = false;
    ULONGLONG m_prepareStartTick = 0;
    double m_prepareRecordedDurSec = 0.0;
    ULONGLONG m_clickDemoStartTick = 0; // 演播室左侧切换点击动效时，在舞台光标处触发一次实时 60FPS 动态预览脉冲
    ULONGLONG m_annDemoStartTick = 0;   // 演播室新插入或切换视频标注时，在暂停舞台触发一次 1.45s 实时 60FPS 动态标注预览

    // 无级滑块与 28 宫格背景缩略图状态
    SliderDragKind m_activeSlider = SliderDragKind::None;
    std::array<D2D1_RECT_F, 15> m_sliderTrackRects{};
    int m_wallpaperCategory = 0;
    int m_wallpaperPage = 0;
    std::vector<WallpaperEntry> m_allWallpapers;
    std::vector<int> m_filteredWallpaperIndices;
    bool m_wallpapersDiscovered = false;
    bool m_wallpaperThumbsDirty = true;

    // 演播室企业/品牌水印抽屉与舞台拖拽交互状态
    bool m_editingWatermarkText = false;
    std::vector<std::string> m_recentLogoPaths;
    std::array<Microsoft::WRL::ComPtr<ID2D1Bitmap>, 6> m_recentLogoThumbBitmaps;
    bool m_recentLogoThumbsDirty = true;
    D2D1_RECT_F m_lastWatermarkStageRect{};
    int m_watermarkStageDragMode = 0; // 0: None, 1: Move, 2: ResizeCorner
    bool m_hoveredWatermarkOnStage = false;
    bool m_hoveredWatermarkCorner = false;
    float m_wmDragStartLx = 0.0f;
    float m_wmDragStartLy = 0.0f;
    float m_wmOrigNormX = 0.88f;
    float m_wmOrigNormY = 0.90f;
    float m_wmOrigScale = 1.0f;

    std::vector<HitZone> m_hitZones;

    struct CoachHoverZone {
        int id = 0;
        D2D1_RECT_F rect{};
        bool enabled = true;
    };
    std::vector<CoachHoverZone> m_coachHoverZones;
    int m_hoveredCoachId = 0;
    ULONGLONG m_coachHoverEnterTick = 0;
    ULONGLONG m_coachLastActiveTick = 0;

    // 独立异步 STA 文件选择线程状态
    std::atomic<bool> m_fileDialogInFlight{false};
    std::mutex m_fileDialogMutex;
    std::thread m_fileDialogWorker;
    std::wstring m_pendingCustomBgWide;
    std::wstring m_pendingWatermarkImgWide;
    std::wstring m_pendingExportDirWide;

    // 原位导出模态窗、未导出防丢关闭确认窗与导出完成交付结果卡片状态
    bool m_exportModalOpen = false;
    bool m_exportCompletedCard = false;
    bool m_closeConfirmModalOpen = false;
    bool m_closeAfterSaveExport = false;
    bool m_confirmSkipStudioNextTime = false;
    bool m_editingExportName = false;
    bool m_userEditedExportStem = false;
    ExportDropdownKind m_openDropdown = ExportDropdownKind::None;
    std::wstring m_initialFileNameStem;
    std::wstring m_exportFileNameStem;
    std::wstring m_exportDirectoryWide;
    StudioExportOptions m_exportOpts{};
    bool m_saveExportPrefs = true;
    bool m_autoCloseAfterExport = false;
    std::vector<std::wstring> m_sessionExportedPathsLower;
    std::string m_lastExportedFilePath;
    std::uint64_t m_lastExportedFileBytes = 0;
    StudioExportFormat m_lastExportedFormat = StudioExportFormat::MP4_H264;
    std::wstring m_lastExportedSpecLine;
    ULONGLONG m_copyFeedbackUntilTick = 0;

    // 后台硬件加速转码导出状态
    std::atomic<bool> m_exporting{false};
    std::atomic<bool> m_cancelExport{false};
    std::atomic<double> m_exportProgress{0.0};
    std::thread m_exportWorker;
    std::mutex m_exportMutex;

    // Direct2D / DirectWrite 渲染资源与当前页 28 槽正方形背景缩略图缓存
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_frameBitmap;
    std::array<Microsoft::WRL::ComPtr<ID2D1Bitmap>, kWallpaperSlotsPerPage> m_themeThumbBitmaps;
    UINT32 m_bitmapW = 0;
    UINT32 m_bitmapH = 0;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_sectionFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_bodyFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_captionFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_captionWrapFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_monoFmt;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_STUDIOPREVIEWWINDOW_H

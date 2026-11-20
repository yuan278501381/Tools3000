#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioPreviewWindowInternal.h — 演播室预览窗口私有跨编译单元协作声明
//
// 职责:
//   仅供 StudioPreviewWindow*.cpp 内部拆分编译单元共享：
//   - 窗口类名、定时器、内部消息 ID、交互热区枚举 HitId 及调色板常量
//   - 共享无状态时间码/格式化/矢量图标/配置持久化辅助函数声明（唯一定义位于 StudioPreviewWindow.cpp）
//   - 广播级时间轴标尺/走带中枢/波形胶片条渲染辅助声明（唯一定义位于 StudioPreviewWindowTimeline.cpp）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOPREVIEWWINDOWINTERNAL_H
#define TOOLS3000_CAPTURE_STUDIOPREVIEWWINDOWINTERNAL_H

#include "capture/StudioPreviewWindow.h"
#include <d2d1_1.h>
#include <shlobj.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::capture::studio_detail {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kStudioWndClass = L"Tools3000_StudioPreviewWindow";
constexpr UINT_PTR kPlaybackTimerId = 4101;
constexpr UINT kTimerIntervalMs = 16; // 原生 ~60 FPS 超流畅实时预览与尾帧补齐
constexpr UINT WM_STUDIO_CUSTOM_BG_PICKED  = WM_APP + 61;
constexpr UINT WM_STUDIO_EXPORT_DIR_PICKED = WM_APP + 62;
constexpr UINT WM_STUDIO_WATERMARK_IMG_PICKED = WM_APP + 63;

enum HitId : int {
    Hit_None = 0,
    Hit_CloseBtn = 1,
    Hit_PlayPauseBtn = 2,
    Hit_TimelineTopBarScrub = 3,
    Hit_TimelineRulerScrub = 4,
    Hit_StageCanvas = 5,
    Hit_MinimizeBtn = 6,
    Hit_MaxRestoreBtn = 7,

    // 左侧竖向图标导航栏 (0..3)
    Hit_LeftRailTabBase = 10, // 10..13

    // 时间轴直观拆分切片工具条动作
    Hit_QuickOpenAnnotations = 18,
    Hit_SplitAtPlayhead = 19,
    Hit_DeleteSelectedItem = 20,
    Hit_CycleClipSpeed = 21,
    Hit_UndoBtn = 22,
    Hit_RedoBtn = 23,
    Hit_AddZoomAtPlayhead = 24,
    Hit_ZoomLaneEmpty = 25,

    // 镜头与光标页控制
    Hit_ZoomMode_None = 30,
    Hit_ZoomMode_2D = 31,
    Hit_ZoomMode_3D = 32,
    Hit_ToggleMotionBlur = 33,
    Hit_ZoomPreset_Gentle = 34,
    Hit_ZoomPreset_Balanced = 35,
    Hit_ZoomPreset_Dynamic = 36,
    Hit_ToggleSmoothCursor = 37,
    Hit_CursorScale_100 = 38,
    Hit_CursorScale_135 = 39,
    Hit_CursorScale_170 = 40,
    Hit_ToggleCursorHalo = 41,
    Hit_ClickStyleBase = 42, // 42..45 对应 StudioClickEffectStyle 0..3 (微晶单波 / 柔雾触点 / 错峰涟漪 / 仅按压)
    Hit_ToggleLockCamera = 46,

    // 画布比例与五维精调控制
    Hit_ToggleAutoFrame = 50,
    Hit_AspectBase = 51,       // 51..55 对应 StudioAspectRatio 0..4
    Hit_SliderPadding = 56,
    Hit_SliderInsetBorder = 57,
    Hit_CycleInsetBorderColor = 58,
    Hit_SliderRadius = 59,
    Hit_SliderShadow = 60,
    Hit_SliderBgBlur = 61,
    Hit_SliderZoomFactor = 62,
    Hit_SliderZoomPacing = 63,
    Hit_SliderZoomSensitivity = 64,
    Hit_SliderFocusSpotlight = 65,

    // 背景分类、翻页、选图与 28 宫格正方形缩略图
    Hit_WallCatBase = 70,      // 70..75 (全部 / 沉稳 / 轻盈 / 风景 / 渐变 / 自定义)
    Hit_WallPickCustom = 80,
    Hit_WallPrevPage = 81,
    Hit_WallNextPage = 82,
    Hit_ThemeSlotBase = 100,   // 100..127 当前页 28 槽正方形缩略图

    // 离线字幕页与顶栏导出按钮
    Hit_ToggleCaptions = 140,
    Hit_OpenExportModalBtn = 145,
    Hit_KeepOriginalBtn = 146,
    Hit_ToggleRawModeBtn = 147,

    // 视频标注抽屉：19 张可视化样式卡片 + 8 色圆点 + 文本编辑框
    Hit_AnnCardBase = 150,     // 150..168 (19 款标注卡片)
    Hit_AnnColorBase = 175,    // 175..182 (8 款高饱和颜色圆点)
    Hit_AnnTextInputBox = 185,

    // 企业与品牌水印抽屉控制项（专属无碰撞 ID 段 400..430）
    Hit_WatermarkToggle = 400,
    Hit_WatermarkModeBase = 401,   // 401..403 (ImageLogo / CornerText / TiledTraceability)
    Hit_WatermarkPickImage = 404,
    Hit_WatermarkAnchorBase = 405, // 405..409 (TopLeft / TopRight / BottomLeft / BottomRight / Center)
    Hit_WatermarkTextInput = 410,
    Hit_WatermarkSliderScale = 411,
    Hit_WatermarkSliderOpacity = 412,
    Hit_WatermarkNewLine = 413,
    Hit_WatermarkPresetDualLine = 414,
    Hit_WatermarkTokenBase = 415,  // 415..418 (+用户名 / +设备名 / +日期时间 / +严禁外传)
    Hit_WatermarkRecentBase = 420, // 420..425 (6 槽历史常用 Logo 缩略卡片)

    // 一体化走带中枢与时间轴视野横向缩放舱 (431..440)
    Hit_TransportJumpStart = 431,
    Hit_TransportPrevFrame = 432,
    Hit_TransportNextFrame = 433,
    Hit_TimelineZoomFit    = 434,
    Hit_TimelineZoomOut    = 435,
    Hit_TimelineZoomIn     = 436,
    Hit_TimelineZoomSlider = 437,
    Hit_TimelinePanBar     = 438,

    // 导出模态窗与原位交付结果卡交互热区
    Hit_ModalBackdrop = 200,
    Hit_ModalCardBody = 201,
    Hit_ModalCloseBtn = 202,
    Hit_ModalNameInput = 203,
    Hit_ModalBrowseDirBtn = 204,
    Hit_ModalFormatSelect = 205,
    Hit_ModalResSelect = 206,
    Hit_ModalFpsSelect = 207,
    Hit_ModalQualitySelect = 208,
    Hit_ModalSavePrefsCheck = 209,
    Hit_ModalConfirmExportBtn = 210,
    Hit_ModalAutoCloseCheck = 211,
    Hit_ModalResultCopyFileBtn = 212,
    Hit_ModalResultOpenPlayBtn = 213,
    Hit_ModalResultRevealBtn = 214,
    Hit_ModalResultExportMoreBtn = 215,
    Hit_ModalResultFinishCloseBtn = 216,
    Hit_ModalDropdownItemBase = 300, // 300 + optionIndex

    // 未导出防丢关闭确认模态框热区 (450..459)
    Hit_CloseConfirmBackdrop = 450,
    Hit_CloseConfirmCard = 451,
    Hit_CloseConfirmCancelBtn = 452,
    Hit_CloseConfirmDiscardBtn = 453,
    Hit_CloseConfirmSaveBtn = 454,
    Hit_CloseConfirmSkipStudioCheck = 455,

    // 三轨时间轴可选中/拖拽片段热区
    Hit_ZoomSegmentBase = 1000, // 1000 + idx
    Hit_VideoClipBase   = 2000, // 2000 + clipIdx
    Hit_AnnSegmentBase  = 3000, // 3000 + idx
};

static constexpr std::array<const char*, 8> kAnnotationPaletteHex = {{
    "#EC4899", // 醒目玫粉
    "#EF4444", // 珊瑚红
    "#F59E0B", // 琥珀金
    "#10B981", // 翡翠绿
    "#3B82F6", // 晴空蓝
    "#8B5CF6", // 电光紫
    "#FFFFFF", // 纯白
    "#0F172A", // 深曜黑
}};

static constexpr std::array<const char*, 6> kInsetBorderColors = {{
    "#FFFFFF", "#F8FAFC", "#F59E0B", "#38BDF8", "#EC4899", "#0F172A"
}};


int64_t queryQpcNow() noexcept;
int64_t queryQpcFreq() noexcept;
D2D1_COLOR_F hexToD2DColor(const std::string& hex, float alpha = 1.0f) noexcept;
const wchar_t* formatLabel(StudioExportFormat f, bool isZh) noexcept;
const wchar_t* resolutionLabel(StudioExportResolution r, bool isZh) noexcept;
const wchar_t* qualityLabel(StudioExportQuality q, bool isZh) noexcept;
std::wstring formatEstimatedSize(uint64_t bytes);
std::wstring formatTimecodeMmSs(double sec);
std::wstring formatCompactDurationBadge(double sec, bool isZh);
bool pointInRectF(const D2D1_RECT_F& r, float x, float y) noexcept;
bool isValidFileNameChar(wchar_t ch) noexcept;
void persistStudioConfigToSettings(const StudioRenderConfig& cfg);
void drawChevronDown(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy);
void drawFolderIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy);
ComPtr<ID2D1StrokeStyle> createRoundStrokeStyle(ID2D1Factory* factory);
void fillLinearVGradRounded(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius,
                            const D2D1_COLOR_F& topCol, const D2D1_COLOR_F& botCol);
void drawScissorsIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt = nullptr);
void drawCrispTrashIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt);
void drawCrispSpeedIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt);
void drawCrispUndoRedoIcon(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1Brush* brush,
                           float cx, float cy, bool isUndo, ID2D1StrokeStyle* roundSt);
void drawCrispPenIcon(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1Brush* brush,
                      float cx, float cy, ID2D1StrokeStyle* roundSt);
void drawCrispCameraFocusIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt);
void drawCrispExportIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt);
std::wstring queryKnownFolderPath(REFKNOWNFOLDERID rfid);
bool isTextLikeAnnotation(StudioAnnotationKind k) noexcept;
bool isPointDirectedAnnotation(StudioAnnotationKind k) noexcept;
const wchar_t* annotationCardDisplayName(int cardIndex, bool isZh) noexcept;

struct TransportConsoleLayout {
    D2D1_RECT_F podRect{};
    D2D1_RECT_F jumpStartBtn{};
    D2D1_RECT_F prevFrameBtn{};
    D2D1_RECT_F playPauseBtn{};
    D2D1_RECT_F nextFrameBtn{};
    bool hasFrameStepButtons = true;
};

void renderBroadcastVernierTimeRuler(
    ID2D1RenderTarget* rt,
    ID2D1SolidColorBrush* brush,
    IDWriteTextFormat* monoFmt,
    float laneLeft,
    float laneRight,
    float rulerTopY,
    float rulerBottomY,
    double totalKeptSec,
    double viewStartSec,
    double viewSpanSec,
    bool dark,
    const D2D1_COLOR_F& colSecondary);

TransportConsoleLayout renderIntegratedTransportConsole(
    ID2D1Factory* d2dFactory,
    ID2D1RenderTarget* rt,
    ID2D1SolidColorBrush* brush,
    IDWriteTextFormat* monoFmt,
    float centerTargetX,
    float minLeftX,
    float maxRightX,
    float topY,
    double shownKeptSec,
    double totalKeptSec,
    bool playing,
    int hoveredHitId,
    bool dark,
    const D2D1_COLOR_F& colAccent,
    const D2D1_COLOR_F& colPrimary,
    const D2D1_COLOR_F& colSecondary,
    const D2D1_COLOR_F& colBorder,
    ID2D1StrokeStyle* roundStroke);

void renderCinemaFilmstripAndMountainWaveformClip(
    ID2D1Factory* d2dFactory,
    ID2D1RenderTarget* rt,
    ID2D1SolidColorBrush* brush,
    IDWriteTextFormat* captionFmt,
    IDWriteTextFormat* monoFmt,
    const D2D1_RECT_F& clipRc,
    float visLaneLeft,
    float visLaneRight,
    size_t clipIndex,
    size_t totalClips,
    const StudioVideoClip& clip,
    int64_t rawTotalWaveMs,
    float lanePlayX,
    bool selected,
    bool dark,
    bool isZh,
    const std::vector<ComPtr<ID2D1Bitmap>>& filmstripBitmaps,
    const std::vector<float>& audioPeaks,
    const StudioWaveformEnvelopeConfig& waveProfile,
    ID2D1StrokeStyle* roundStroke);

} // namespace tools3000::capture::studio_detail

#endif // TOOLS3000_CAPTURE_STUDIOPREVIEWWINDOWINTERNAL_H

#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTURE_STATE_H
#define TOOLS3000_CAPTURE_CAPTURE_STATE_H

#include "capture/ScreenCapture.h"
#include "capture/MarkupEngine.h"
#include "capture/BeautyShell.h"
#include "capture/ScreenRecorder.h"

#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <opencv2/core.hpp>

namespace tools3000::capture {

enum class OverlayMode { Screenshot, RecordRegion };
enum class OverlayState { Idle, Selecting, Selected, Marking };
enum class CaptureCompletionAction { Default, Copy, SaveAs, Pin };
enum class ToolbarCommand {
    SelectTool,
    SelectColor,
    ChooseCustomColor,
    ToggleCornerRadius,
    Undo,
    Redo,
    Clear,
    ExtractText,
    PinWindow,
    ScrollCapture,
    StartRecord,
    ToggleBeautyShell,       // 一键切换美化外壳导出模式 (CleanShot X / PixPin 风格)
    Copy,                    // 复制到剪贴板 (Ctrl+C)
    Confirm,
    Cancel,
    // 二级属性栏专属命令
    ToggleFill,              // 切换填充模式
    CycleStrokeWidth,        // 循环切换线宽 (2, 4, 6, 8, 12, 16)
    CycleElementCornerRadius,// 循环切换标注圆角 (0, 8, 14, 20)
    ToggleLineStyleDropdown,  // 展开/收起线条样式下拉菜单
    SelectLineStyle,         // 选择线条样式 (实线/虚线/点线/点划线)
    ToggleArrowStyleDropdown,// 展开/收起箭头样式下拉菜单
    SelectArrowStyle,        // 选择箭头样式 (标准/细线/双向)
    ToggleShapeDropdown,     // 展开/收起形状下拉菜单
    TogglePenDropdown,       // 展开/收起画笔下拉菜单
    ToggleArrowDropdown,     // 展开/收起箭头主下拉菜单
    ToggleMosaicDropdown,    // 展开/收起马赛克下拉菜单
    SelectMosaicType,        // 选择马赛克类型 (像素/高斯模糊)
    CycleBeautyBg,           // 循环切换美化外壳背景预设
    CycleBeautyPadding,      // 循环切换美化外壳衬底内边距
    CycleBeautyRadius,       // 循环切换美化外壳圆角 (0, 10, 16, 24, 32)
    ToggleNumberShape,       // 切换序号形状 (圆形/圆角微晶胶囊)
    ToggleNumberFormatDropdown, // 展开/收起序号编号制式下拉菜单 (1,2,3 / ABC / abc / I,II,III / 一,二,三)
    ResetNumberCounter,      // 重置序号计数器为 1 (Snipaste / PixPin 标杆)
    ToggleSpotlightShape,    // 切换聚光灯形状 (矩形/圆角矩形 vs 椭圆)
    CycleSpotlightDim,       // 调节聚光灯背景弱化暗度 (15% ~ 85%)
    CycleMagnifierScale,     // 循环切换局部放大镜倍率 (1.5x / 2.0x / 3.0x / 4.0x)
    ToggleMagnifierCallout,  // 切换局部放大镜引出详图模式 (零遮挡旁路引出 / 原位同心透镜)
    ToggleTextOutline,       // 切换文字描边开关 (开启/关闭)
    SelectTextOutlineColor,  // 选择文字描边颜色 (自适应/曜石黑/纯白)
    // 录屏准备栏专属命令 (世界级 Pre-Recording Setup Bar)
    RecordToggleFormat,      // 切换录制格式 (MP4 / GIF)
    RecordCycleFps,          // 循环切换帧率 (15 / 30 / 60 fps)
    RecordCycleQuality,      // 循环切换画质 (标准 / 高清 / 原画)
    RecordCycleClickEffect,  // 循环切换鼠标点击特效 (无 / 水波纹 / macOS缩放 / 水波+缩放)
    RecordToggleKeycast,     // 开关按键回显 (Keycast)
    RecordToggleSystemAudio, // 开关录制系统声音
    RecordToggleMicrophone,  // 开关录制麦克风
    RecordStartConfirm,      // 正式确认开始录制 (● 开始录制)
    // 选区侧边浮动菜单专属命令 (PixPin 标杆交互)
    SideToggleCornerRadius,  // 侧边展开/收起选区圆角滑块面板
    SideInvertSelection,     // 侧边反选/选区翻转
    SideResetSelection,      // 侧边重置选区直角
    SideCycleAspectRatio,    // 侧边快速锁定/切换选区固定比例 (自由/16:9/4:3/1:1/黄金比例)
};

enum class AspectRatioPreset {
    Free = 0,     // 自由比例
    Ratio_16_9,   // 16:9
    Ratio_4_3,    // 4:3
    Ratio_1_1,    // 1:1
    Ratio_Golden, // 黄金比例 (1.618:1)
    COUNT
};

enum class SubmenuType {
    None = 0,
    LineStyle,
    ArrowStyle,
    ShapeType,
    PenType,
    ArrowType,
    MosaicType,
};

enum class SliderPopupType {
    None = 0,
    StrokeWidth,            // 线宽无级滑块 (1~30px)
    CornerRadius,           // 标注圆角无级滑块 (0~60px)
    SelectionCornerRadius,  // 选区圆角无级滑块 (0~120px) (PixPin 标杆)
    TextFontSize,           // 字号无级滑块 (12~72px)
    MosaicBlockSize,        // 马赛克颗粒度无级滑块 (2~40px)
    SpotlightDim,           // 聚光灯暗化浓度无级滑块 (15~85%)
};

struct SliderPopupState {
    SliderPopupType type = SliderPopupType::None;
    D2D1_RECT_F popupRect{};
    D2D1_RECT_F trackRect{};
    D2D1_RECT_F thumbRect{};
    std::vector<std::pair<int, D2D1_RECT_F>> presetButtons;
    int minValue = 1;
    int maxValue = 30;
    int currentValue = 4;
    bool isDragging = false;
    float dragAnchorX = 0.0f;
};

enum class DropdownType {
    None = 0,
    RecordFormat,        // MP4 / GIF
    RecordFps,           // 15 / 30 / 60 fps
    RecordQuality,       // 标准 / 高清 / 原画
    RecordClickEffect,   // 无特效 / 柔光水波 / macOS缩放 / 水波+缩放
    BeautyShellBg,       // 冷灰 / 纸白 / 晨雾 / 曜石 / 纯影 (带渐变色块实时预览)
    BeautyShellPadding,  // 16px / 24px / 32px / 48px / 64px
    BeautyShellRadius,   // R0 / R8 / R16 / R24 / R32
    ShapeMode,           // 几何图形 (Shift+1) / 聚光灯 (Shift+0)
    NumberFormat,        // 1,2,3 / A,B,C / a,b,c / I,II,III / 一,二,三
};

struct DropdownItem {
    int id = 0;
    std::wstring label;
    std::wstring hint;
    bool isSelected = false;
    bool isHovered = false;
    D2D1_RECT_F rect{};
    bool hasSwatch = false;
    D2D1_COLOR_F swatchColor1{};
    D2D1_COLOR_F swatchColor2{};
};

struct DropdownMenuState {
    DropdownType type = DropdownType::None;
    D2D1_RECT_F anchorButtonRect{};
    D2D1_RECT_F menuRect{};
    std::vector<DropdownItem> items;
    int hoveredIndex = -1;
    int previewOriginalValue = -1; // 实时预览回退备份
    BeautyBackgroundType originalBgType = BeautyBackgroundType::StudioSlate;
};

enum class ColorFormatType {
    HEX = 0,     // #3A86FF
    RGB,         // rgb(58, 134, 255)
    RGBA,        // rgba(58, 134, 255, 1.0)
    HEX_0x,      // 0x3A86FF
    HSL,         // hsl(217, 100%, 61%)
    HSV,         // hsv(217, 77%, 100%)
    CMYK,        // cmyk(77%, 47%, 0%, 0%)
    DEC,         // 3835647
    COUNT
};

struct CaptureCompletion {
    CaptureCompletionAction action = CaptureCompletionAction::Default;
    std::string filePath;
    ImageFormat format = ImageFormat::PNG;
};

/// 截图完成交付决策单一事实源（供 ScreenCapture 与防回退单测复用）
struct CaptureDeliveryDecision {
    bool shouldSaveAutomatically = false;
    bool shouldSaveExplicitly = false;
    bool shouldCopyToClipboard = false;
    bool shouldPromptSaveAsFallback = false;
    bool shouldRecordHistory = true;
    bool shouldInvokeMemoryCallback = false;
};

inline CaptureDeliveryDecision resolveCaptureDeliveryDecision(
    const CaptureOptions& options,
    const CaptureCompletion& completion) noexcept {
    CaptureDeliveryDecision d;
    d.shouldSaveExplicitly =
        (completion.action == CaptureCompletionAction::SaveAs && !completion.filePath.empty());
    d.shouldSaveAutomatically = (!d.shouldSaveExplicitly && options.saveToFile);
    d.shouldInvokeMemoryCallback = static_cast<bool>(options.onImageCaptured);

    if (completion.action == CaptureCompletionAction::Copy) {
        // 用户显式按 Ctrl+C 或点击「复制」按钮：强制写入剪贴板
        d.shouldCopyToClipboard = true;
    } else if (completion.action == CaptureCompletionAction::Default) {
        // 默认完成动作（Enter、双击选区、点击「完成」勾号）：严格尊重用户 copyToClipboard 开关
        d.shouldCopyToClipboard = options.copyToClipboard;
        if (!d.shouldCopyToClipboard &&
            !d.shouldSaveAutomatically &&
            !d.shouldSaveExplicitly &&
            !d.shouldInvokeMemoryCallback) {
            // 用户同时关闭了自动复制与自动保存且无内存消费方：弹出系统另存为对话框兜底防丢图
            d.shouldPromptSaveAsFallback = true;
        }
    } else {
        // 显式另存为动作：尊重用户是否开启同时复制到剪贴板
        d.shouldCopyToClipboard = options.copyToClipboard;
    }

    d.shouldRecordHistory = options.recordHistory;
    return d;
}

inline int recordBitrateToQualityLevel(int bitrateMbps) noexcept {
    if (bitrateMbps <= 4) return 0;
    if (bitrateMbps <= 8) return 1;
    if (bitrateMbps <= 16) return 2;
    return 3;
}

inline int recordQualityLevelToBitrateMbps(int qualityLevel) noexcept {
    switch (qualityLevel) {
        case 0: return 4;
        case 1: return 8;
        case 2: return 16;
        default: return 32;
    }
}

struct ToolbarButton {
    D2D1_RECT_F rect{};
    ToolbarCommand command = ToolbarCommand::SelectTool;
    MarkupTool tool = MarkupTool::Rectangle;
    MarkupColor color = MarkupColor::Red();
    std::wstring label;
    int intParam = 0;
    LineStyle lineStyleParam = LineStyle::Solid;
    ArrowStyle arrowStyleParam = ArrowStyle::Standard;
    bool boolParam = false;
    bool hasDropdown = false;
    bool isSecondary = false;
    bool isSeparatorBefore = false;
};

struct RecordSetupOptions {
    RecordFormat format = RecordFormat::MP4_H264;
    RecordFormat preferredVideoFormat = RecordFormat::MP4_H264; // 记忆原视频编码器（切 GIF 后再切回视频不丢失 H.265/WebM）
    int fps = 30;         // 15, 30, 60
    int qualityLevel = 1; // 0: 流畅 (4Mbps), 1: 高清 (8Mbps), 2: 超清 (16Mbps), 3: 原画 (32Mbps)
    int bitrateMbps = 8;  // 真实目标码率 (Mbps)，单一事实源直通编码器，绝不被三档硬编码覆盖
    bool showClickEffects = false;
    bool showClickZoom = true;
    bool includeKeycast = true;
    bool captureSystemAudio = false;
    bool captureMicrophone = false;
};

using SelectionCallback = std::function<void(
    const CaptureRegion& region, const cv::Mat& markedImage, const CaptureCompletion& completion)>;
using RecordSelectionCallback = std::function<void(const CaptureRegion& region, const RecordSetupOptions& options)>;

class CaptureState {
public:
    std::atomic<OverlayState> state{OverlayState::Idle};
    OverlayMode mode = OverlayMode::Screenshot;
    CaptureOptions options;

    POINT dragStart{};
    POINT dragEnd{};
    POINT currentCursor{};
    bool dragging = false;
    RECT detectedWindow{};
    std::vector<RECT> detectedWindowHierarchy;
    std::vector<std::wstring> detectedWindowHierarchyDescriptions;
    int detectedWindowHierarchyIndex = 0;
    POINT lastMousePos{};

    // 智能选区物理阻尼弹簧微动效 (Spring Damping Lerp) 与微晶提示
    D2D1_RECT_F animDetectedWindowRect{};
    D2D1_RECT_F targetDetectedWindowRect{};
    D2D1_RECT_F animVelocity{};
    bool isHierarchyAnimating = false;
    DWORD hierarchyBadgeFadeOutUntil = 0;
    std::wstring hierarchyBadgeText;

    cv::Mat frozenScreen;

    MarkupEngine markup;
    MarkupTool currentTool = MarkupTool::Rectangle;
    bool isMarkupToolActive = false;
    MarkupColor currentColor = MarkupColor::Red();
    MarkupColor customColor{139, 92, 246, 255}; // 默认典雅紫罗兰 #8B5CF6
    bool hasCustomColor = false;
    LineStyle currentLineStyle = LineStyle::Solid;
    int currentStrokeWidth = 4;
    bool currentFillMode = false;
    float currentElementCornerRadius = 0.0f;
    ArrowStyle currentArrowStyle = ArrowStyle::Standard;
    int currentMosaicType = 0; // 0: 像素马赛克, 1: 高斯模糊
    bool currentTextOutline = true; // 文字是否描边 (默认开启高反差描边)
    MarkupColor currentTextOutlineColor = MarkupColor::Auto(); // 描边颜色: Auto / Black / White

    bool markupBaseReady = false;
    cv::Rect markupBaseRect{0, 0, 0, 0};
    bool isMarking = false;
    POINT markupStart{};
    POINT markupEnd{};
    std::vector<cv::Point> penPoints;
    std::vector<ToolbarButton> toolbarButtons;
    std::vector<ToolbarButton> secondaryToolbarButtons;
    std::vector<ToolbarButton> selectionSideButtons;
    SubmenuType openSubmenu = SubmenuType::None;
    D2D1_RECT_F openSubmenuRect{};
    std::vector<ToolbarButton> submenuButtons;
    SliderPopupState sliderPopup{};
    D2D1_RECT_F primaryToolbarRect{};
    D2D1_RECT_F secondaryToolbarRect{};
    D2D1_RECT_F selectionSideRect{};
    D2D1_RECT_F toolbarLayoutSelection{};
    D2D1_SIZE_F toolbarLayoutSurface{};
    float toolbarLayoutScale = 0.0f;
    OverlayMode toolbarLayoutMode = OverlayMode::Screenshot;
    bool toolbarLayoutChinese = true;
    bool toolbarLayoutLongImage = false;
    bool toolbarLayoutValid = false;
    MarkupElement* activeElement = nullptr;
    HitArea dragHandle = HitArea::None;
    bool isManipulating = false;
    bool isSelectingText = false;      // 正在编辑态文本框内按住左键拖拽划词选中文字
    float textPreferredCaretX = -1.0f; // 多行文本上下移动光标时保持的期望水平像素 X 坐标
    POINT textManipulateStart{};       // 文本标注按下起点（用于区分拖拽平移文本框 vs 单击进入字符级编辑）

    bool isAdjustingSelection = false;
    HitArea selAdjustHandle = HitArea::None;
    POINT selAdjustLast{};

    bool isAdjustingCornerRadius = false;
    float cornerDragStartRadius = 0.0f;
    POINT cornerDragStartPos{};
    int cornerDragIndex = 0; // 0=LT, 1=RT, 2=RB, 3=LB

    int dynamicMagnifierRadius = 64;
    float dynamicMagnifierScale = 2.0f;
    bool magnifierCalloutMode = true; // 默认启用引出式旁路详图放大，避免遮挡原位周围上下文
    bool magnifierShowBadge = true;
    NumberBadgeShape currentMagnifierShape = NumberBadgeShape::Circle;
    MarkupColor currentMagnifierColor = MarkupColor::White();

    DWORD loupeToastUntil = 0;
    std::wstring loupeToastMessage;
    bool isFadingOut = false;
    float dpiScale = 1.0f;
    ColorFormatType colorFormat = ColorFormatType::HEX;
    bool colorFormatHex = true;
    bool shiftTapCandidate = false; // 纯单按释放 Shift (Tap-Only) 切换颜色格式状态位
    bool showTimestamp = false;
    float cornerRadius = 0.0f; // 选区圆角半径 (0, 8, 12, 16, 24)
    float detectedWindowCornerRadius = 10.0f; // 现代 Windows 11 窗口圆角高亮半径
    DWORD fadeOutStart = 0;

    // 截图美化外壳导出配置 (CleanShot X / PixPin 风格)
    BeautyShellOptions beautyShell;

    // 选区固定比例预设 (自由/16:9/4:3/1:1/黄金比例)
    AspectRatioPreset aspectRatio = AspectRatioPreset::Free;
    RECT savedSelectionBeforeInvert{0, 0, 0, 0};
    bool hasSavedSelectionBeforeInvert = false;

    // 序号标号形状与编号制式 (圆形/圆角微晶胶囊 + 1,2,3 / ABC / abc / I,II,III / 一,二,三)
    NumberBadgeShape currentNumberShape = NumberBadgeShape::Circle;
    NumberFormat currentNumberFormat = NumberFormat::Arabic;

    // 聚光灯 (Spotlight) 多区域联合弱化属性
    bool currentSpotlightEllipse = false;
    float currentSpotlightDimAlpha = 0.55f;
    bool currentSpotlightBorder = false;

    // 主工具栏左侧 2x3 灰色 6 点自由拖拽手柄状态
    D2D1_RECT_F toolbarGripRect{};
    bool isToolbarGripHovered = false;
    bool hasCustomToolbarPos = false;
    D2D1_POINT_2F customToolbarOrigin{};
    bool isDraggingToolbar = false;
    D2D1_POINT_2F toolbarDragOffset{};
    bool toolbarLayoutCustomPos = false;
    D2D1_POINT_2F toolbarLayoutCustomOrigin{};

    enum class SizeUnit { Pixel = 0, DeviceIndependentPixel = 1 };
    SizeUnit sizeUnit = SizeUnit::Pixel;
    bool showPositionInHud = true;
    bool showUnitInHud = true;
    bool isSizeHudHovered = false;
    bool isSizeMenuOpen = false;
    D2D1_RECT_F sizeHudRect{};
    D2D1_RECT_F sizeMenuRect{};
    D2D1_RECT_F loupeHudRect{};
    D2D1_RECT_F hierarchyBadgeRect{};
    int lastSizeHudEdge = -1; ///< 用于 HUD 自动躲避防抖动迟滞跟踪的上一次布局边缘

    // 多态微晶下拉菜单状态
    DropdownMenuState dropdownMenu{};

    /// 获取当前生效的有效圆角半径（美化外壳模式下返回外壳圆角，否则返回选区圆角）
    float effectiveCornerRadius() const {
        return beautyShell.enabled ? beautyShell.cornerRadius : cornerRadius;
    }

    // 智能二维码探测与快速动作胶囊
    std::string detectedQrText;
    D2D1_RECT_F qrChipRect{};
    bool isQrChipHovered = false;

    SelectionCallback callback;
    RecordSelectionCallback recordCallback;
    RecordSetupOptions recordSetup;
    std::function<void(const CaptureRegion& region, const cv::Mat& cropped)> ocrCallback;

    bool historyMode = false;
    int historyIndex = 0;

    // ── 长截图全功能画布视口与 Smartisan 无损裁切控制器 (SSOT Long-Image Studio) ──
    enum class LongCanvasDragMode {
        None = 0,
        PanCanvas,
        MinimapViewport,
        MinimapCropTop,
        MinimapCropBottom,
    };

    struct LongCanvasViewState {
        bool active = false;
        int frameCount = 0;
        int docWidth = 0;
        int docHeight = 0;
        float zoom = 1.0f;
        float fitZoom = 1.0f;
        float scrollX = 0.0f; // 屏幕缩放像素偏移 (0 .. maxScrollX)
        float scrollY = 0.0f; // 屏幕缩放像素偏移 (0 .. maxScrollY)

        D2D1_RECT_F stageRect{};
        D2D1_RECT_F topHeaderRect{};
        D2D1_RECT_F minimapPanelRect{};
        D2D1_RECT_F minimapImageRect{};

        // 顶部快捷导航胶囊热区
        D2D1_RECT_F btnJumpTopRect{};
        D2D1_RECT_F btnJumpBottomRect{};
        D2D1_RECT_F btnResetCropRect{};
        D2D1_RECT_F btnZoomToggleRect{};
        int hoveredHeaderBtn = -1; // -1=无, 0=头部, 1=尾部, 2=重置裁切, 3=缩放切换

        // 右侧 Minimap 与画布拖拽状态
        LongCanvasDragMode dragMode = LongCanvasDragMode::None;
        POINT panLastCursor{0, 0};
        float panStartScrollX = 0.0f;
        float panStartScrollY = 0.0f;
        bool middleButtonPanning = false;

        // 亚像素精准图元操控锚点（避免非 100% 缩放下图元平移/拉伸取整漂移）
        D2D1_POINT_2F lastManipulateDocPt{0.0f, 0.0f};
        D2D1_POINT_2F lastSelAdjustDocPt{0.0f, 0.0f};

        void reset() noexcept {
            *this = LongCanvasViewState{};
        }
    };

    LongCanvasViewState longCanvas{};

    bool isLongImageMode() const noexcept {
        return longCanvas.active && longCanvas.docWidth > 0 && longCanvas.docHeight > 0;
    }

    D2D1_POINT_2F longCanvasOrigin() const noexcept {
        if (!isLongImageMode()) {
            return D2D1::Point2F(0.0f, 0.0f);
        }
        const float z = (std::max)(0.05f, longCanvas.zoom);
        const float stageW = (std::max)(1.0f, longCanvas.stageRect.right - longCanvas.stageRect.left);
        const float stageH = (std::max)(1.0f, longCanvas.stageRect.bottom - longCanvas.stageRect.top);
        const float scaledW = static_cast<float>(longCanvas.docWidth) * z;
        const float scaledH = static_cast<float>(longCanvas.docHeight) * z;
        const float pad = 20.0f * (dpiScale > 0.0f ? dpiScale : 1.0f);

        float ox = 0.0f;
        if (scaledW + pad * 2.0f <= stageW) {
            ox = longCanvas.stageRect.left + (stageW - scaledW) * 0.5f;
        } else {
            ox = longCanvas.stageRect.left + pad - longCanvas.scrollX;
        }

        float oy = 0.0f;
        if (scaledH + pad * 2.0f <= stageH) {
            oy = longCanvas.stageRect.top + (stageH - scaledH) * 0.5f;
        } else {
            oy = longCanvas.stageRect.top + pad - longCanvas.scrollY;
        }
        return D2D1::Point2F(ox, oy);
    }

    void clampLongCanvasScroll() noexcept {
        if (!isLongImageMode()) return;
        const float z = (std::max)(0.05f, longCanvas.zoom);
        const float stageW = (std::max)(1.0f, longCanvas.stageRect.right - longCanvas.stageRect.left);
        const float stageH = (std::max)(1.0f, longCanvas.stageRect.bottom - longCanvas.stageRect.top);
        const float pad = 20.0f * (dpiScale > 0.0f ? dpiScale : 1.0f);
        const float maxScrollX = (std::max)(0.0f, static_cast<float>(longCanvas.docWidth) * z + pad * 2.0f - stageW);
        const float maxScrollY = (std::max)(0.0f, static_cast<float>(longCanvas.docHeight) * z + pad * 2.0f - stageH);
        longCanvas.scrollX = std::clamp(longCanvas.scrollX, 0.0f, maxScrollX);
        longCanvas.scrollY = std::clamp(longCanvas.scrollY, 0.0f, maxScrollY);
    }

    void scrollLongCanvasToDocRow(float docRow, float viewportAnchorRatio = 0.25f) noexcept {
        if (!isLongImageMode()) return;
        const float z = (std::max)(0.05f, longCanvas.zoom);
        const float stageH = (std::max)(1.0f, longCanvas.stageRect.bottom - longCanvas.stageRect.top);
        const float pad = 20.0f * (dpiScale > 0.0f ? dpiScale : 1.0f);
        longCanvas.scrollY = pad + docRow * z - stageH * std::clamp(viewportAnchorRatio, 0.05f, 0.95f);
        clampLongCanvasScroll();
    }

    D2D1_POINT_2F docToScreen(float docX, float docY) const noexcept {
        if (!isLongImageMode()) {
            return D2D1::Point2F(docX, docY);
        }
        const auto orig = longCanvasOrigin();
        const float z = (std::max)(0.05f, longCanvas.zoom);
        return D2D1::Point2F(orig.x + docX * z, orig.y + docY * z);
    }

    D2D1_POINT_2F screenToDoc(float screenX, float screenY) const noexcept {
        if (!isLongImageMode()) {
            return D2D1::Point2F(screenX, screenY);
        }
        const auto orig = longCanvasOrigin();
        const float z = (std::max)(0.05f, longCanvas.zoom);
        return D2D1::Point2F((screenX - orig.x) / z, (screenY - orig.y) / z);
    }

    POINT screenToDocPoint(POINT screenPt) const noexcept {
        if (!isLongImageMode()) {
            return screenPt;
        }
        const auto d = screenToDoc(static_cast<float>(screenPt.x), static_cast<float>(screenPt.y));
        return POINT{
            static_cast<LONG>(std::lround(d.x)),
            static_cast<LONG>(std::lround(d.y))
        };
    }

    D2D1_RECT_F docRectToScreen(const D2D1_RECT_F& docRect) const noexcept {
        if (!isLongImageMode()) {
            return docRect;
        }
        const auto lt = docToScreen(docRect.left, docRect.top);
        const auto rb = docToScreen(docRect.right, docRect.bottom);
        return D2D1::RectF(lt.x, lt.y, rb.x, rb.y);
    }

    /// 计算主/副工具栏与侧边栏在屏幕空间的统一停靠锚点矩形：
    /// 普通截图直接返回选区屏幕矩形；长图模式下返回主画布视口内可见框，使全套工具栏稳固停靠于底部中央且不遮挡右侧 Minimap
    D2D1_RECT_F toolbarAnchorRect() const noexcept {
        const float x1 = static_cast<float>((std::min)(dragStart.x, dragEnd.x));
        const float y1 = static_cast<float>((std::min)(dragStart.y, dragEnd.y));
        const float x2 = static_cast<float>((std::max)(dragStart.x, dragEnd.x));
        const float y2 = static_cast<float>((std::max)(dragStart.y, dragEnd.y));
        return toolbarAnchorRect(D2D1::RectF(x1, y1, x2, y2));
    }

    D2D1_RECT_F toolbarAnchorRect(const D2D1_RECT_F& docSelectionRect) const noexcept {
        if (!isLongImageMode()) {
            return docSelectionRect;
        }
        const auto sr = docRectToScreen(docSelectionRect);
        const auto& st = longCanvas.stageRect;
        if (st.right <= st.left || st.bottom <= st.top) {
            return sr;
        }
        const float left = std::clamp(sr.left, st.left + 8.0f, (std::max)(st.left + 8.0f, st.right - 24.0f));
        const float right = std::clamp(sr.right, left + 16.0f, st.right - 8.0f);
        const float top = std::clamp(sr.top, st.top, (std::max)(st.top, st.bottom - 16.0f));
        const float bottom = std::clamp(sr.bottom, top + 16.0f, st.bottom);
        return D2D1::RectF(left, top, right, bottom);
    }
};

} // namespace tools3000::capture
#endif // TOOLS3000_CAPTURE_CAPTURE_STATE_H

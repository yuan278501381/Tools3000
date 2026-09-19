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
enum class CaptureCompletionAction { Default, Copy, SaveAs };
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
    ToggleNumberShape,       // 切换序号形状 (圆形/方角微晶胶囊)
    ResetNumberCounter,      // 重置序号计数器为 1 (Snipaste / PixPin 标杆)
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
    int fps = 30;         // 15, 30, 60
    int qualityLevel = 1; // 0: 标准 (5Mbps), 1: 高清 (10Mbps), 2: 原画 (20Mbps)
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
    bool toolbarLayoutValid = false;
    MarkupElement* activeElement = nullptr;
    HitArea dragHandle = HitArea::None;
    bool isManipulating = false;

    bool isAdjustingSelection = false;
    HitArea selAdjustHandle = HitArea::None;
    POINT selAdjustLast{};

    bool isAdjustingCornerRadius = false;
    float cornerDragStartRadius = 0.0f;
    POINT cornerDragStartPos{};
    int cornerDragIndex = 0; // 0=LT, 1=RT, 2=RB, 3=LB

    int dynamicMagnifierRadius = 60;
    float dynamicMagnifierScale = 2.0f;

    DWORD loupeToastUntil = 0;
    std::wstring loupeToastMessage;
    bool isFadingOut = false;
    float dpiScale = 1.0f;
    ColorFormatType colorFormat = ColorFormatType::HEX;
    bool colorFormatHex = true;
    bool showTimestamp = false;
    float cornerRadius = 0.0f; // 选区圆角半径 (0, 8, 12, 16, 24)
    float detectedWindowCornerRadius = 10.0f; // 现代 Windows 11 窗口圆角高亮半径
    DWORD fadeOutStart = 0;

    // 截图美化外壳导出配置 (CleanShot X / PixPin 风格)
    BeautyShellOptions beautyShell;

    // 选区固定比例预设 (自由/16:9/4:3/1:1/黄金比例)
    AspectRatioPreset aspectRatio = AspectRatioPreset::Free;

    // 序号标号形状 (圆形/方角微晶胶囊)
    NumberBadgeShape currentNumberShape = NumberBadgeShape::Circle;

    enum class SizeUnit { Pixel = 0, DeviceIndependentPixel = 1 };
    SizeUnit sizeUnit = SizeUnit::Pixel;
    bool showPositionInHud = true;
    bool showUnitInHud = true;
    bool isSizeHudHovered = false;
    bool isSizeMenuOpen = false;
    D2D1_RECT_F sizeHudRect{};
    D2D1_RECT_F sizeMenuRect{};
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
};

} // namespace tools3000::capture
#endif // TOOLS3000_CAPTURE_CAPTURE_STATE_H

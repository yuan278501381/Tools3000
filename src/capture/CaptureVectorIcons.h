#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTUREVECTORICONS_H
#define TOOLS3000_CAPTURE_CAPTUREVECTORICONS_H

#include <d2d1.h>
#include <wrl/client.h>

namespace tools3000::capture {

enum class CaptureIconId {
    None = 0,
    // 标注工具
    ToolRectangle,
    ToolLine,
    ToolEllipse,
    ToolPen,
    ToolHighlight,
    ToolArrow,
    ToolArrowStandard,
    ToolArrowThin,
    ToolArrowWing,
    ToolArrowDouble,
    ToolArrowCampaign,
    ToolArrowDoubleTrack,
    ToolText,
    ToolNumber,
    ToolMagnifier,
    ToolMosaic,
    ToolBlur,
    ToolInpaint,
    ToolSpotlight,
    
    // 动作与控制
    ActionUndo,
    ActionRedo,
    ActionClear,
    ActionExtractText,
    ActionTranslate,
    ActionPinWindow,
    ActionPinGroup,
    ActionScrollCapture,
    ActionRecordVideo,
    ActionRecordStart,
    ActionRecordPause,
    ActionRecordStop,
    ActionToggleMic,
    ActionToggleMicMuted,
    ActionToggleSpeaker,
    ActionToggleSpeakerMuted,
    ActionCopy,
    ActionSave,
    ActionQuickSave,
    ActionCancel,
    ActionConfirm,
    ActionInvert,
    ActionGrayscale,
    ActionResetScale,
    ActionFold,
    ActionRotate,
    ActionResetNumber,
    ActionResetSelection,
    ActionSnapshot,
    ActionOpenFolder,
    
    // 二级属性与样式
    PropSolidLine,
    PropDashedLine,
    PropDottedLine,
    PropDashDotLine,
    PropWavyLine,
    TextMarkupHighlight,
    TextMarkupWavy,
    TextMarkupUnderline,
    TextReplaceEdit,
    PropStrokeWidth,
    PropCornerRadius,
    PropPadding,
    PropFillOutline,
    PropFillSolid,
    PropSpotlightBorder,
    PropPipette,
    PropPalette,
    PropRainbowWheel,
    PropQrCode,
    ToolBeautyShell,
    PropAspectRatio,
    PropNumberSquare,
    PropMagnifierSquare,
    PropMagnifierBadge,
    PropMagnifierCallout,
    PropTextOutline,
    PropFontSize,
    PropTextBackground,
    PropPolyline,
    PropSmoothCurve,
    ActionInvertSelection,
    ActionClickThrough,
    ActionRotateLeft,
    ActionFlipHorizontal,
    ActionFlipVertical,
    ActionArrangeGrid,
    UiChevronRight,
};

struct MarkupColor;

class CaptureVectorIcons {
public:
    /// 在指定的矩形区域内居中渲染矢量微图标
    static void renderIcon(ID2D1RenderTarget* rt, ID2D1Factory* factory,
                           CaptureIconId iconId, const D2D1_RECT_F& rect,
                           ID2D1Brush* brush, float scale);

    /// 渲染支持双色装饰的矢量图标（如文字荧光笔/波浪线/直线图标：字形使用 primaryBrush，底部装饰使用 accentBrush）
    static void renderIconWithAccent(ID2D1RenderTarget* rt, ID2D1Factory* factory,
                                     CaptureIconId iconId, const D2D1_RECT_F& rect,
                                     ID2D1Brush* primaryBrush, ID2D1Brush* accentBrush,
                                     float scale);

    /// 绘制 PixPin 风格调色板第 1 位「连续彩虹圆角方框 + 1.4x 白环 + 当前生效色圆角内芯」指示器
    static void drawCustomColorIndicator(ID2D1RenderTarget* rt,
                                         const D2D1_RECT_F& rect,
                                         const MarkupColor& refColor,
                                         float scale,
                                         bool isDark);

    /// 绘制 PixPin 风格预设颜色圆角方块色块（Squircle，17.5x，r=4.8x）
    static void drawPresetColorSwatch(ID2D1RenderTarget* rt,
                                      const D2D1_RECT_F& rect,
                                      const MarkupColor& color,
                                      float scale,
                                      bool isHovered,
                                      ID2D1Brush* hoverBrush);
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_CAPTUREVECTORICONS_H

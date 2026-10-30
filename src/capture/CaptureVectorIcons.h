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
    ToolText,
    ToolNumber,
    ToolMosaic,
    ToolBlur,
    ToolInpaint,
    
    // 动作与控制
    ActionUndo,
    ActionRedo,
    ActionClear,
    ActionExtractText,
    ActionPinWindow,
    ActionScrollCapture,
    ActionRecordVideo,
    ActionRecordStart,
    ActionRecordPause,
    ActionRecordStop,
    ActionToggleMic,
    ActionToggleSpeaker,
    ActionCopy,
    ActionSave,
    ActionCancel,
    ActionConfirm,
    ActionInvert,
    ActionGrayscale,
    ActionResetScale,
    ActionFold,
    ActionRotate,
    ActionResetNumber,
    ActionSnapshot,
    ActionOpenFolder,
    
    // 二级属性与样式
    PropSolidLine,
    PropDashedLine,
    PropDottedLine,
    PropDashDotLine,
    PropStrokeWidth,
    PropCornerRadius,
    PropFillOutline,
    PropFillSolid,
    PropPipette,
    PropPalette,
    PropRainbowWheel,
    PropQrCode,
    ToolBeautyShell,
    PropAspectRatio,
    PropNumberSquare,
    PropTextOutline,
    PropFontSize,
    PropTextBackground,
    ActionInvertSelection,
};

class CaptureVectorIcons {
public:
    /// 在指定的矩形区域内居中渲染矢量微图标
    static void renderIcon(ID2D1RenderTarget* rt, ID2D1Factory* factory,
                           CaptureIconId iconId, const D2D1_RECT_F& rect,
                           ID2D1Brush* brush, float scale);
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_CAPTUREVECTORICONS_H

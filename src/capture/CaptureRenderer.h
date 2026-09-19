#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTURERENDERER_H
#define TOOLS3000_CAPTURE_CAPTURERENDERER_H

#include "capture/CaptureState.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

namespace tools3000::capture {

class CaptureRenderer {
public:
    bool initialize(HWND hwnd, CaptureState& state);
    void shutdown();
    /// Release HWND/render-target resources while retaining thread-safe,
    /// device-independent factories for the next capture.
    void releaseWindowResources();
    bool updateDpiScale(float scale);
    void applyThemeColors();
    
    void render(CaptureState& state);
    void invalidate() {
        m_needsRender = true;
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    bool needsRender() const { return m_needsRender; }
    void clearNeedsRender() { m_needsRender = false; }
    bool updateScreenBitmap(const cv::Mat& image);
    
    void markMarkupDirty() { m_markupCacheDirty = true; m_needsRender = true; }
    void updateHistoryBitmap(CaptureState& state);

public:
    ID2D1HwndRenderTarget* getRenderTarget() const { return m_renderTarget.Get(); }
private:
    bool createRenderResources(CaptureState& state);

    void drawDimOverlay(const D2D1_RECT_F& selectionRect, CaptureState& state);
    void drawSelection(const D2D1_RECT_F& rect, CaptureState& state);
    void drawSizeInfo(const D2D1_RECT_F& rect, CaptureState& state);
    void drawSizeMenu(const D2D1_RECT_F& hudRect, CaptureState& state);
    void drawToolbar(const D2D1_RECT_F& selectionRect, CaptureState& state);
    void drawSubmenu(CaptureState& state);
    void drawSliderPopup(CaptureState& state);
    void drawDropdownMenu(CaptureState& state);
    void drawBeautyShellPreview(const D2D1_RECT_F& rect, CaptureState& state);
    void drawGlassPanel(const D2D1_RECT_F& rect, float radius, bool seeThrough);
    void drawMarkupPreview(const D2D1_RECT_F& selectionRect, CaptureState& state);
    void drawActiveMarkupPreview(const D2D1_RECT_F& selectionRect, CaptureState& state);
    void drawDynamicMagnifier(CaptureState& state);
    void drawSelectionLoupe(float cx, float cy, CaptureState& state);
    void drawSmartAlignmentGuides(const D2D1_RECT_F& selRect, CaptureState& state);
    void drawQrChip(const D2D1_RECT_F& selRect, CaptureState& state);
    void drawCrosshair(float x, float y, CaptureState* pState = nullptr);
    void drawVectorButtonIcon(const ToolbarButton& button, const D2D1_RECT_F& rect, ID2D1Brush* brush, float scale);
    void drawFloatingToast(const CaptureState& state);
    void drawCornerRadiusHandleVisual(const D2D1_RECT_F& boxRect, float cornerRadius, float scale, bool isDragging, POINT dragStartPos, POINT currentCursor, float minDimension, int cornerIndex = -1);

    public:
    /// 高精度 DirectWrite 文本度量助手 (基于 IDWriteTextLayout 严格度量，消除排版跑动与重叠)
    D2D1_SIZE_F measureText(const std::wstring& text, IDWriteTextFormat* format = nullptr, float maxWidth = 2048.0f, float maxHeight = 256.0f) const;
    bool sampleScreenColor(int x, int y, int& r, int& g, int& b, CaptureState& state) const;
    /// 高精度 Direct2D 文字描边渲染助手 (先绘制外扩描边轮廓，再绘制内芯文字，确保极端背景下字字清晰分明)
    static void drawOutlinedText(ID2D1RenderTarget* rt,
                                 const std::wstring& text,
                                 IDWriteTextFormat* format,
                                 const D2D1_RECT_F& layoutRect,
                                 ID2D1Brush* textBrush,
                                 ID2D1Brush* outlineBrush = nullptr,
                                 float strokeWidth = 1.0f);

    HWND m_hwnd = nullptr;
    bool m_needsRender = true;
    bool m_markupCacheDirty = true;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_screenBitmap;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_markupCacheBitmap;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_historyBitmap;
    Microsoft::WRL::ComPtr<ID2D1Layer> m_markupClipLayer;
    
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dimBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_infoBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_infoTextBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_crosshairBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_windowHighlightBrush;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_infoTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textInputFormat;
    float m_textScale = 0.0f;
};

} // namespace tools3000::capture
#endif // TOOLS3000_CAPTURE_CAPTURERENDERER_H

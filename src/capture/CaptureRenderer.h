#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTURERENDERER_H
#define TOOLS3000_CAPTURE_CAPTURERENDERER_H

#include "capture/CaptureState.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <vector>

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
    bool createTextFormatsForScale(float scale);

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
    void updateShortcutHintAvoidance(const CaptureState& state);
    void drawCornerRadiusHandleVisual(const D2D1_RECT_F& boxRect, float cornerRadius, float scale, bool isDragging, POINT dragStartPos, POINT currentCursor, float minDimension, int cornerIndex = -1);

    struct BitmapTile {
        int xOffset = 0;
        int yOffset = 0;
        int width = 0;
        int height = 0;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    };

    bool hasScreenContent() const noexcept { return m_screenBitmap != nullptr || !m_screenTiles.empty(); }
    bool hasHistoryContent() const noexcept { return m_historyBitmap != nullptr || !m_historyTiles.empty(); }
    void resolveMonitorBoundsInCanvas(float canvasX, float canvasY, D2D1_SIZE_F surfaceSize,
                                      D2D1_RECT_F& outMonitorRect, D2D1_RECT_F& outWorkRect) const;
    bool rebuildBitmapTiles(const cv::Mat& bgra, D2D1_ALPHA_MODE alphaMode,
                            std::vector<BitmapTile>& tiles, int& outW, int& outH);
    void drawTiledRegion(const std::vector<BitmapTile>& tiles, int totalW, int totalH,
                         const D2D1_RECT_F& dstRect, const D2D1_RECT_F& srcRect,
                         D2D1_BITMAP_INTERPOLATION_MODE interp, bool highQualityCubic = false);
    void updateLongCanvasLayout(CaptureState& state);
    void renderLongImageStudio(CaptureState& state);
    void drawLongCanvasTopHeader(const D2D1_RECT_F& docSelRect, CaptureState& state);
    void drawLongCanvasMinimap(const D2D1_RECT_F& docSelRect, CaptureState& state);

    public:
    /// 计算像素取色放大镜（Selection Loupe）在当前画布上的精确外接矩形（供 HUD 联动避让与单元测试复用）
    static D2D1_RECT_F computeSelectionLoupeRect(float cx, float cy, D2D1_SIZE_F surfaceSize, float scale);
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
    /// PixPin 标杆彩虹当前色指示器绘制 (20x20 连续彩虹圆角方环 + 1.4px 隔离白环 + 14.4x14.4 当前生效色实心圆角内芯)
    static void drawCustomColorIndicator(ID2D1RenderTarget* rt,
                                         const D2D1_RECT_F& rect,
                                         const MarkupColor& refColor,
                                         float scale,
                                         bool isDark = false);
    /// PixPin 标杆预设色块 Squircle 绘制 (17.5x17.5 圆角正方形 r=4.8，无选中同心圆环，仅悬停微扩与柔光)
    static void drawPresetColorSwatch(ID2D1RenderTarget* rt,
                                      const D2D1_RECT_F& rect,
                                      const MarkupColor& color,
                                      float scale,
                                      bool isHovered,
                                      ID2D1Brush* hoverBrush = nullptr);

    HWND m_hwnd = nullptr;
    bool m_needsRender = true;
    bool m_markupCacheDirty = true;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_screenBitmap;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_markupCacheBitmap;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_historyBitmap;
    std::vector<BitmapTile> m_historyTiles;
    int m_historyTilesW = 0;
    int m_historyTilesH = 0;
    Microsoft::WRL::ComPtr<ID2D1Layer> m_markupClipLayer;
    std::vector<BitmapTile> m_screenTiles;
    int m_screenTilesW = 0;
    int m_screenTilesH = 0;
    std::vector<BitmapTile> m_markupCacheTiles;
    int m_markupTilesW = 0;
    int m_markupTilesH = 0;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_minimapBitmap;
    int m_minimapSrcW = 0;
    int m_minimapSrcH = 0;
    
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dimBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_infoBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_infoTextBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_crosshairBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_windowHighlightBrush;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_infoTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_hudPrimaryFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_hudBadgeFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textInputFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_numberBadgeLargeFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_numberBadgeMediumFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_numberBadgeSmallFormat;
    float m_textScale = 0.0f;
};

} // namespace tools3000::capture
#endif // TOOLS3000_CAPTURE_CAPTURERENDERER_H

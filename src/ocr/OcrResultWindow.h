#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace tools3000::ocr {

class OcrResultWindow {
public:
    struct TextPosition {
        int line = -1;
        UINT32 offset = 0;

        bool isValid() const noexcept { return line >= 0; }
        bool operator==(const TextPosition& other) const noexcept {
            return line == other.line && offset == other.offset;
        }
        bool operator!=(const TextPosition& other) const noexcept {
            return !(*this == other);
        }
        bool operator<(const TextPosition& other) const noexcept {
            return line != other.line ? line < other.line : offset < other.offset;
        }
    };

    static OcrResultWindow& instance() {
        static OcrResultWindow inst;
        return inst;
    }

    bool initialize();
    void cleanup();

    void showResult(const std::string& text);

    // 文本选区与测试内省接口
    HWND hwnd() const noexcept { return m_hwnd; }
    float currentScale() const noexcept { return m_scale; }
    float scrollY() const noexcept { return m_scrollY; }
    float maxScroll() const noexcept { return m_maxScroll; }
    size_t lineCount() const noexcept { return m_lines.size(); }
    bool hasSelection() const noexcept;
    std::string selectedTextUtf8() const;
    void setSelectionRange(TextPosition anchor, TextPosition caret);
    void selectAll();
    void clearSelection();
    void copySelectionOrAll();
    bool isFooterHintSingleLine(float clientWidth) const;

private:
    OcrResultWindow() = default;
    ~OcrResultWindow() { cleanup(); }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool createResources();
    bool createTextResources(float scale);
    bool ensureSurface(int width, int height);
    void releaseSurface();
    void discardDeviceResources();
    void updatePlacement();
    void rebuildTextLayout();
    void updateHover(POINT point);
    void copyAll();
    void copyLine(int lineIndex);
    void hide();
    void render(int retryDepth = 0);

    bool computeScrollbarMetrics(int clientWidth, int clientHeight,
                                 D2D1_RECT_F& outTrackRect,
                                 D2D1_RECT_F& outThumbRect) const;
    D2D1_RECT_F computeLineCopyButtonRect(int lineIndex, int clientWidth) const;
    TextPosition hitTestTextPosition(POINT clientPt) const;

    struct LineItem {
        std::wstring text;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        float topY = 0.0f;
        float height = 0.0f;
    };

    HWND m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    std::string m_text;
    std::wstring m_wideText;
    std::vector<LineItem> m_lines;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_buttonFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_hintFormat;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> m_textLayout;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushText;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushMuted;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBtn;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBtnHover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushSuccess;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushError;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushSelection;

    HDC m_screenDc = nullptr;
    HDC m_memoryDc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_previousBitmap = nullptr;
    void* m_pixels = nullptr;
    int m_surfaceWidth = 0;
    int m_surfaceHeight = 0;

    float m_scrollY = 0.0f;
    float m_maxScroll = 0.0f;
    float m_totalContentHeight = 0.0f;
    float m_scale = 1.0f;

    // UI Layout
    D2D1_RECT_F m_btnCopyRect{};
    D2D1_RECT_F m_btnCloseRect{};

    // Selection & Interaction State
    TextPosition m_selAnchor{};
    TextPosition m_selCaret{};
    bool m_isSelectingText = false;
    bool m_hadSelectionOnMouseDown = false;
    POINT m_mouseDownPt{};

    bool m_draggingScrollbar = false;
    float m_scrollbarDragOffset = 0.0f;
    bool m_hoverScrollbar = false;

    int m_hoverLineCopyBtn = -1;
    int m_pressedLineCopyBtn = -1;
    bool m_pressedCopyBtn = false;
    bool m_pressedCloseBtn = false;

    bool m_hoverCopy = false;
    bool m_hoverClose = false;
    int m_hoverLineIndex = -1;
    int m_copiedLineIndex = -1;
    bool m_copiedSelection = false;
    bool m_trackingMouse = false;
    bool m_updatingPlacement = false;
    float m_currentAlpha = 0.0f;
    uint64_t m_showTime = 0;

    uint64_t m_copiedTime = 0;
    bool m_copySucceeded = false;
};

} // namespace tools3000::ocr

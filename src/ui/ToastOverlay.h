#ifndef TOOLS3000_UI_ToastOverlay_H
#define TOOLS3000_UI_ToastOverlay_H

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <mutex>
#include <vector>

namespace tools3000::ui {

class ToastOverlay {
public:
    static ToastOverlay& instance();

    bool initialize(HINSTANCE hInstance);
    void shutdown();

    // 显示通知胶囊
    void showToast(const std::string& text);
    void hideNow();
    
    // 是否启用
    void setEnabled(bool enabled);
    bool isEnabled() const;

private:
    ToastOverlay() = default;
    ~ToastOverlay() = default;

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void render();
    bool createResources();
    void discardResources();
    bool ensureSurface(int width, int height);
    void releaseSurface();
    void updatePlacement();
    SIZE measureWindowSizeForText(const std::wstring& wText, int workAreaWidth);

    HWND m_hwnd = nullptr;
    HWND m_helperOwnerHwnd = nullptr;
    bool m_enabled = true;
    float m_dpiScale = 1.0f;
    bool m_updatingPlacement = false;
    
    // UI 数据
    std::string m_displayText;
    std::mutex m_mutex;
    UINT_PTR m_timerId = 0;
    float m_opacity = 0.0f;
    float m_animScale = 1.0f; // For subtle pop-up animation
    bool m_fadingIn = false;

    // D2D 资源
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_strokeBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;

    HDC m_memoryDC = nullptr;
    HBITMAP m_memoryBitmap = nullptr;
    HBITMAP m_oldBitmap = nullptr;
    int m_surfaceWidth = 0;
    int m_surfaceHeight = 0;
    int m_capsuleWidth = 0;
    int m_capsuleHeight = 0;
};

} // namespace tools3000::ui

#endif // TOOLS3000_UI_ToastOverlay_H

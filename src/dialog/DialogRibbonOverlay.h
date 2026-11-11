/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#pragma once

#include "core/utils/ThemeUtils.h"

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace tools3000::dialog {

struct RibbonButtonRect {
    enum class Type { ExplorerSync, Recent, Favorites, Close };
    Type type;
    D2D1_RECT_F rect;
    std::string text;
    std::string extraData;
    int badgeCount{0};
    bool hovered{false};
    float textWidth{0.0f};
    float badgeWidth{0.0f};
};

class DialogRibbonOverlay {
public:
    static DialogRibbonOverlay& instance();

    bool init();
    void cleanup();

    // 绑定并展示在指定文件对话框顶部（支持从任意后台线程安全调用，零主线程跨进程阻塞）
    void attachToDialog(HWND dialogHwnd, const std::string& processName);
    void setCachedDialogFolder(HWND dialogHwnd, const std::string& folderPath);
    void updatePosition();
    void onThemeChanged();
    void hide();

    static bool computeSafeHorizontalPlacement(const RECT& dlgRect, float dpiScale,
                                               int ribbonWidth, const std::string& posConfig,
                                               int& outPosX, int& outMaxAvailableWidth) noexcept;

    HWND getHwnd() const { std::lock_guard lock(m_mutex); return m_hwnd; }
    HWND getTargetDialog() const { std::lock_guard lock(m_mutex); return m_targetDialog; }

private:
    DialogRibbonOverlay();
    ~DialogRibbonOverlay();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void attachValidatedOnUiThread(HWND dialogHwnd, DWORD processId, const std::string& processName);
    bool doUpdatePosition();
    void doAttachToDialog();
    void doHide();
    void resetStateLocked();
    void discardDeviceResources();
    bool ensureDeviceBrushes(bool isDark, const tools3000::core::AccentColorRGB& accent);
    void render();
    void updateButtonsLayout(float dpiScale, int maxAvailableWidth = 0);
    bool hitTestButton(const RibbonButtonRect& btn, int x, int y, float dpiScale) const noexcept;
    void onLButtonDown(int x, int y);
    void onMouseMove(int x, int y);
    void onMouseLeave();
    void showRecentMenu(int screenX, int screenY, bool rightAlign = false);
    void showFavoritesMenu(int screenX, int screenY, bool rightAlign = false);
    void ensureTextFormats(float dpiScale);
    void drawFolderIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth);
    void drawClockIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth);
    void drawBookmarkIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth);
    void drawChevron(ID2D1RenderTarget* rt, ID2D1Brush* brush, float centerX, float centerY, float size, float strokeWidth);
    void drawCloseIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float centerX, float centerY, float size, float strokeWidth);
    void drawBadge(ID2D1RenderTarget* rt, ID2D1Brush* bgBrush, ID2D1Brush* textBrush, float x, float y, float w, float h, int count);

    HWND m_hwnd{nullptr};
    HWND m_targetDialog{nullptr};
    HWND m_dismissedDialog{nullptr};
    DWORD m_targetProcessId{0};
    std::string m_targetProcess;
    std::string m_cachedDialogFolder;
    std::string m_activeExplorerPath;

    int m_width{360};
    int m_height{34};
    int m_posX{0};
    int m_posY{0};
    int m_lastMaxAvailableWidth{0};
    float m_dpiScale{1.0f};
    float m_currentFontDpi{0.0f};
    bool m_isEnglishLocale{false};

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textMutedBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_highlightBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_highlightBorderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_syncBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_syncBorderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_chevronBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_badgeBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_badgeTextBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dividerBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_whiteBrush;
    bool m_brushesDarkTheme{false};
    tools3000::core::AccentColorRGB m_brushesAccent{0.23f, 0.51f, 0.96f};
    bool m_brushesValid{false};

    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_boldFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_badgeFormat;

    HDC m_memDC{nullptr};
    HBITMAP m_memBitmap{nullptr};
    HBITMAP m_oldBitmap{nullptr};
    int m_renderedWidth{0};
    int m_renderedHeight{0};
    bool m_menuOpen{false};

    std::vector<RibbonButtonRect> m_buttons;
    bool m_trackingMouse{false};
    mutable std::mutex m_mutex;
};

} // namespace tools3000::dialog

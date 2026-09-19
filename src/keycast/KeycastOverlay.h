#ifndef TOOLS3000_KEYCAST_KEYCASTOVERLAY_H
#define TOOLS3000_KEYCAST_KEYCASTOVERLAY_H

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include <atomic>
#include "core/hotkey/KeyTranslator.h"

namespace tools3000::keycast {

struct KeycastSettings {
    bool enabled = true;
    bool autoBypassFullscreen = true;
    bool showKeyboard = true;
    std::string filterMode = "smart_shortcuts";
    bool includeFunctionKeys = false; // "smart_shortcuts", "with_single_modifiers", "all_keys"
    std::string position = "top_left";         // "top_left", "top_center", "top_right", "bottom_left", "bottom_center", "bottom_right"
    bool mergeRecentKeys = true;
    int mergeTimeoutMs = 1200; // 同排连击合并间隔 (ms)
    int displayDurationMs = 2500;
    int fontSize = 36;
    int opacity = 80;                          // 20~100 胶囊背景不透明度 (默认 80% 高雅通透)
    std::string textColor = "#ffffff";
    std::string backgroundColor = "#1c1c22";
    std::string modifierKeycapColor = "auto"; // "auto" (跟随主题色) 或 HEX 颜色
    int modifierKeycapOpacity = 50;            // 0~100 修饰键底色不透明度 (默认 50% 高雅微晶)
    std::string modifierTextColor = "#000000"; // 默认纯黑实体文字与徽标，呈现顶级键帽反差质感

    // ── 物理微动效自由配置体系 (World-Class Motion Presets & Custom Combos) ──
    std::string firstKeyAnim = "slide";      // "slide" (阻尼滑入), "pop" (气泡冒出), "fade" (渐现), "none" (无动效)
    std::string subsequentKeyAnim = "fade";  // "fade" (渐现默认), "pop" (气泡冒出), "slide" (阻尼滑入), "none" (无动效)
    bool rowCascadeAnim = true;              // 换行时旧行物理级联上推
    bool exitDriftAnim = true;               // 寿命结束时轻盈飘升消融
};

struct KeycastItem {
    std::string rawKey;
    std::vector<std::string> tokens; // e.g. ["Ctrl", "C"]
    int repeatCount = 1;
    uint64_t pushTime = 0;
    float offsetX = 0.0f;     // 横向推入偏移 (首项)
    float offsetY = 0.0f;     // 微垂直位移 (冒出项)
    float scale = 1.0f;       // 弹性气泡缩放比例
    float scaleVel = 0.0f;    // 弹性速度
    float opacity = 0.0f;     // 阻尼渐现
    bool isFirstInRow = true; // 首项推入 vs 后续项

    // ── 连击微动画物理连续性状态 (Continuous Fluid Dynamics) ──
    float badgeScale = 0.0f;          // 角标微弹簧缩放
    float badgeScaleVel = 0.0f;       // 角标弹簧速度
    float badgeOpacity = 0.0f;        // 角标平滑透明度
    float badgeWidthProgress = 0.0f;  // 胶囊向右延伸因子 (0.0f -> 1.0f)
};

struct KeycastRow {
    std::vector<KeycastItem> items;
    uint64_t lastActiveTime = 0;
    float offsetY = 0.0f;       // 当前垂直位移插值
    float targetOffsetY = 0.0f; // 目标垂直位移
    float opacity = 1.0f;
    bool isExiting = false;     // 是否正在消融退出
};

class KeycastOverlay {
public:
    static KeycastOverlay& instance();

    bool init();
    void cleanup();
    
    // push a new keystroke combination to display
    void pushKey(const std::string& keyStr);
    void pushKey(const tools3000::core::KeycastKeyInfo& keyInfo);
    void pushKey(const std::vector<std::string>& tokens, const std::string& rawKey);

    /// 获取配置
    KeycastSettings getSettings() const;

    /// 更新配置
    void updateSettings(const KeycastSettings& settings);

    /// 恢复默认配置
    void resetDefaults();

    /// 全屏免打扰
    void setAutoBypassFullscreen(bool enable);
    bool autoBypassFullscreen() const;

    /// 颜色解析（支持 auto 与十六进制 HEX）
    D2D1_COLOR_F parseColor(const std::string& hex, float alpha = 1.0f) const;

    /// 响应全局主题与主题色实时变更
    void onThemeChanged();

    /// 胶囊项宽度测量（基于 DirectWrite 真实字形度量）
    float calculateItemWidth(const KeycastItem& item, float dpiScale) const;

    /// 设置录屏选区停靠
    void setDockingRegion(const RECT& region, bool active);

private:
    KeycastOverlay() = default;
    ~KeycastOverlay() = default;

    bool createResources();
    void discardResources();
    bool updatePlacement();
    void render();
    void tickAnimation();
    void onAnimationCompleteAndHide();
    void drawKeycapCapsule(const KeycastItem& item, float x, float y, float alpha, float dpiScale);
    void drawWindowsLogo(const D2D1_RECT_F& rect, float alpha);

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    HWND m_helperOwnerHwnd = nullptr;
    KeycastSettings m_settings;
    mutable std::mutex m_settingsMutex;
    std::string m_lastAccentColor;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    mutable Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_keycapTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_repeatTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_plusTextFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushText;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushModifierText;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushKeycapBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushKeycapBorder;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushFuncKeyBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushFuncKeyBorder;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBadgeBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBadgeBorder;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBadgeText;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushPlusText;

    HDC m_memoryDC = nullptr;
    HBITMAP m_memoryBitmap = nullptr;
    HBITMAP m_oldBitmap = nullptr;
    int m_width = 720;
    int m_height = 200;
    int m_posX = 0;
    int m_posY = 0;
    float m_dpiScale = 1.0f;
    bool m_updatingPlacement = false;
    bool m_timerRunning = false;

    std::vector<KeycastRow> m_rows;
    std::string m_lastRawKey;
    uint64_t m_lastGlobalPushTime = 0;
    bool m_hasDockingRegion = false;
    RECT m_dockingRegion{};

    std::mutex m_mutex;
};

} // namespace tools3000::keycast

#endif // TOOLS3000_KEYCAST_KEYCASTOVERLAY_H

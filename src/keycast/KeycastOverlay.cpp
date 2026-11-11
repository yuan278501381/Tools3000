#include "KeycastOverlay.h"
#include "KeycastStyle.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/hotkey/KeyboardHook.h"
#include <algorithm>
#include <vector>
#include <sstream>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "msimg32.lib")

namespace tools3000::keycast {

namespace {
constexpr UINT_PTR ANIMATION_TIMER_ID = 1;
constexpr UINT START_ANIMATION_MESSAGE = WM_APP + 1;
constexpr UINT REFRESH_RESOURCES_MESSAGE = WM_APP + 2;

std::vector<std::string> splitTokens(const std::string& str) {
    std::vector<std::string> tokens;
    if (str.empty()) return tokens;

    size_t start = 0;
    const std::string delim = " + ";
    size_t pos = 0;
    while ((pos = str.find(delim, start)) != std::string::npos) {
        std::string token = str.substr(start, pos - start);
        size_t first = token.find_first_not_of(" \t\r\n");
        size_t last = token.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && last != std::string::npos) {
            tokens.push_back(token.substr(first, last - first + 1));
        }
        start = pos + delim.length();
    }
    std::string lastToken = str.substr(start);
    size_t first = lastToken.find_first_not_of(" \t\r\n");
    size_t last = lastToken.find_last_not_of(" \t\r\n");
    if (first != std::string::npos && last != std::string::npos) {
        tokens.push_back(lastToken.substr(first, last - first + 1));
    }
    if (tokens.empty() && !str.empty()) {
        tokens.push_back(str);
    }
    return tokens;
}

inline bool isModifierKey(const std::string& token) {
    return token == "Ctrl" || token == "Alt" || token == "Shift" || token == "Win";
}

struct KeycastDynamicMetrics {
    float fontSize;       // 主键基准字号 (baseFontSize 20 -> 15.0px @ 1x DPI)
    float keycapFontSize; // 修饰键字号 (fontSize * 0.88f -> 13.2px, 绝不溢出)
    float plusFontSize;   // 加号字号 (fontSize * 0.72f -> 10.8px, 清晰挺拔)
    float charWidth;      // 单字符兜底估算宽度（仅在极端无 DirectWrite 环境降级使用）
    float capHeight;      // 键帽底座高度 = fontSize * 1.50f (~22.5px)
    float capRadius;      // 键帽圆角 = 5.5f * dpiScale (~5.5px)
    float capsuleHeight;  // 外层胶囊高度 = capHeight + 10.0f * dpiScale (~32.5px)
    float capsuleRadius;  // 外层胶囊圆角 = 8.5f * dpiScale (~8.5px)
    float paddingX;       // 胶囊左右内边距 = fontSize * 0.50f (~7.5px)
    float winWidth;       // Windows 键宽度 = fontSize * 2.25f (~33.8px, 长宽比 1.5:1)
    float logoSize;       // Windows 徽标尺寸 = capHeight * 0.44f (~10px, 充裕呼吸感)
    float plusWidth;      // 加号占位宽度
    float rowStep;        // 多行垂直步进 = capsuleHeight + 8.0f * dpiScale
    float borderWidth;    // 动态微边框 = 1.0f * dpiScale
};

inline KeycastDynamicMetrics computeKeycastMetrics(int baseFontSize, float dpiScale) {
    KeycastDynamicMetrics m;
    m.fontSize = (std::max)(12.0f, static_cast<float>(baseFontSize) * 0.75f) * dpiScale;
    m.keycapFontSize = m.fontSize * 0.82f;
    m.plusFontSize = m.fontSize * 0.85f;
    m.charWidth = m.fontSize * 0.58f;
    m.capHeight = m.fontSize * 1.25f; // 键帽高度黄金比例，消除臃肿充塞感
    const float outerInset = std::clamp(m.fontSize * 0.36f, 5.5f * dpiScale, 14.0f * dpiScale);
    m.capRadius = std::clamp(m.capHeight * 0.24f, 4.5f * dpiScale, 16.0f * dpiScale);
    m.capsuleHeight = m.capHeight + outerInset * 2.0f; // 外托盘上下留白随字号联动
    m.capsuleRadius = m.capRadius + outerInset;        // 严格满足外胶囊同心圆角几何契约
    m.paddingX = (std::max)(outerInset, m.fontSize * 0.55f);
    m.winWidth = m.fontSize * 1.95f;
    m.logoSize = m.capHeight * 0.58f; // Windows 徽标适度调高，饱满挺拔清晰
    m.plusWidth = m.plusFontSize * 0.78f;
    m.rowStep = m.capsuleHeight + 8.0f * dpiScale;
    m.borderWidth = (std::max)(1.0f * dpiScale, 1.0f);
    return m;
}

Microsoft::WRL::ComPtr<IDWriteTextFormat> createKeycastFormat(
    IDWriteFactory* factory,
    float fontSize,
    DWRITE_FONT_WEIGHT weight) {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    if (!factory || fontSize <= 0.0f) return fmt;
    const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(factory);
    const wchar_t* localeName = tools3000::core::isUiLanguageChinese() ? L"zh-CN" : L"en-US";
    factory->CreateTextFormat(
        fontFamily,
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSize,
        localeName,
        &fmt);
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return fmt;
}

float measureTextLayoutWidth(IDWriteFactory* factory,
                             IDWriteTextFormat* format,
                             const std::wstring& wtext,
                             float fallbackCharWidth) {
    if (wtext.empty()) return 0.0f;
    if (factory && format) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(factory->CreateTextLayout(
                wtext.c_str(),
                static_cast<UINT32>(wtext.length()),
                format,
                2000.0f,
                1000.0f,
                &layout)) && layout) {
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(layout->GetMetrics(&m))) {
                return (std::max)(m.width, m.widthIncludingTrailingWhitespace);
            }
        }
    }
    return static_cast<float>(wtext.length()) * fallbackCharWidth;
}

bool isCaptureOverlayVisible() noexcept {
    if (tools3000::core::KeyboardHook::instance().isAnyCaptureOverlayVisible()) {
        return true;
    }
    HWND capHwnd = FindWindowW(L"Tools3000_CaptureOverlay", nullptr);
    if (capHwnd && IsWindowVisible(capHwnd)) return true;
    HWND scrollHwnd = FindWindowW(L"Tools3000_ScrollCaptureOverlay", nullptr);
    return scrollHwnd && IsWindowVisible(scrollHwnd);
}

bool isValidFilterMode(const std::string& v) noexcept {
    return v == "smart_shortcuts" || v == "with_single_modifiers" ||
           v == "shortcuts_only" || v == "all_keys";
}

bool isValidPosition(const std::string& v) noexcept {
    return v == "top_left" || v == "top_center" || v == "top_right" ||
           v == "bottom_left" || v == "bottom_center" || v == "bottom_right";
}

bool isValidKeyAnim(const std::string& v) noexcept {
    return v == "slide" || v == "pop" || v == "fade" || v == "none";
}
}

KeycastOverlay& KeycastOverlay::instance() {
    static KeycastOverlay inst;
    return inst;
}

bool KeycastOverlay::init() {
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = wndProc;
    wcex.hInstance = GetModuleHandleW(nullptr);
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcex.lpszClassName = L"Tools3000_KeycastOverlay";

    RegisterClassExW(&wcex);

    m_helperOwnerHwnd = tools3000::core::WinUtils::createOverlayHelperOwner(wcex.hInstance, L"Tools3000_KeycastHelperOwner");

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wcex.lpszClassName,
        L"Tools3000 Keycast",
        WS_POPUP,
        0, 0, 1, 1,
        m_helperOwnerHwnd, nullptr, wcex.hInstance, nullptr
    );

    if (!m_hwnd) {
        LOG_ERROR("Failed to create KeycastOverlay window");
        if (m_helperOwnerHwnd) {
            DestroyWindow(m_helperOwnerHwnd);
            m_helperOwnerHwnd = nullptr;
        }
        return false;
    }

    tools3000::core::WinUtils::applyTaskbarSafeOverlayStyle(m_hwnd, false);
    SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
    m_uiThreadId = GetCurrentThreadId();

    auto& cfg = tools3000::core::ConfigManager::instance();
    KeycastSettings loaded;
    loaded.enabled = cfg.get<bool>("/keycast/enabled", true);
    loaded.autoBypassFullscreen = cfg.get<bool>("/keycast/autoBypassFullscreen", true);
    loaded.showKeyboard = cfg.get<bool>("/keycast/showKeyboard", true);
    loaded.filterMode = cfg.get<std::string>("/keycast/filterMode", "smart_shortcuts");
    loaded.includeFunctionKeys = cfg.get<bool>("/keycast/includeFunctionKeys", false);
    loaded.position = cfg.get<std::string>("/keycast/position", "top_left");
    loaded.mergeRecentKeys = cfg.get<bool>("/keycast/mergeRecentKeys", true);
    loaded.mergeTimeoutMs = cfg.get<int>("/keycast/mergeTimeoutMs", 1200);
    loaded.displayDurationMs = cfg.get<int>("/keycast/displayDurationMs", 2500);
    loaded.fontSize = cfg.get<int>("/keycast/fontSize", 36);
    loaded.opacity = cfg.get<int>("/keycast/opacity", 80);
    loaded.textColor = cfg.get<std::string>("/keycast/textColor", "#ffffff");
    loaded.backgroundColor = cfg.get<std::string>("/keycast/backgroundColor", "#1c1c22");
    loaded.modifierKeycapColor = cfg.get<std::string>("/keycast/modifierKeycapColor", "auto");
    loaded.modifierKeycapOpacity = cfg.get<int>("/keycast/modifierKeycapOpacity", 50);
    loaded.modifierTextColor = cfg.get<std::string>("/keycast/modifierTextColor", "#000000");
    loaded.firstKeyAnim = cfg.get<std::string>("/keycast/firstKeyAnim", "slide");
    loaded.subsequentKeyAnim = cfg.get<std::string>("/keycast/subsequentKeyAnim", "fade");
    loaded.rowCascadeAnim = cfg.get<bool>("/keycast/rowCascadeAnim", true);
    loaded.exitDriftAnim = cfg.get<bool>("/keycast/exitDriftAnim", true);
    loaded = sanitizeSettings(loaded);
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        m_settings = loaded;
    }

    m_atomicDpiScale.store(m_dpiScale, std::memory_order_release);
    m_atomicOverlayWidth.store(m_width, std::memory_order_release);

    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    return true;
}

void KeycastOverlay::cleanup() {
    if (m_hwnd) {
        KillTimer(m_hwnd, ANIMATION_TIMER_ID);
        m_timerRunning = false;
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_helperOwnerHwnd) {
        DestroyWindow(m_helperOwnerHwnd);
        m_helperOwnerHwnd = nullptr;
    }

    {
        std::lock_guard<std::mutex> renderLock(m_renderMutex);
        discardResourcesLocked();
        m_dwriteFactory.Reset();
        m_d2dFactory.Reset();
    }
}

KeycastSettings KeycastOverlay::sanitizeSettings(const KeycastSettings& raw) {
    KeycastSettings s = raw;
    s.mergeTimeoutMs = std::clamp(s.mergeTimeoutMs, 100, 10000);
    s.displayDurationMs = std::clamp(s.displayDurationMs, 200, 30000);
    s.fontSize = std::clamp(s.fontSize, 12, 120);
    s.opacity = std::clamp(s.opacity, 20, 100);
    s.modifierKeycapOpacity = std::clamp(s.modifierKeycapOpacity, 0, 100);
    if (!isValidFilterMode(s.filterMode)) {
        s.filterMode = "smart_shortcuts";
    }
    if (!isValidPosition(s.position)) {
        s.position = "top_left";
    }
    if (!isValidKeyAnim(s.firstKeyAnim)) {
        s.firstKeyAnim = "slide";
    }
    if (!isValidKeyAnim(s.subsequentKeyAnim)) {
        s.subsequentKeyAnim = "fade";
    }
    if (s.textColor.empty()) s.textColor = "#ffffff";
    if (s.backgroundColor.empty()) s.backgroundColor = "#1c1c22";
    if (s.modifierKeycapColor.empty()) s.modifierKeycapColor = "auto";
    if (s.modifierTextColor.empty()) s.modifierTextColor = "#000000";
    return s;
}

KeycastSettings KeycastOverlay::getSettings() const {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_settings;
}

void KeycastOverlay::applySettings(const KeycastSettings& /*settings*/) {
    m_needRecreateResources.store(true, std::memory_order_release);
    if (!m_hwnd) return;
    if (GetCurrentThreadId() == m_uiThreadId) {
        updatePlacement();
        if (IsWindowVisible(m_hwnd)) {
            render();
        }
    } else {
        PostMessageW(m_hwnd, REFRESH_RESOURCES_MESSAGE, 0, 0);
    }
}

void KeycastOverlay::updateSettings(const KeycastSettings& settings) {
    const KeycastSettings sanitized = sanitizeSettings(settings);
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        m_settings = sanitized;
    }

    auto& cfg = tools3000::core::ConfigManager::instance();
    cfg.set("/keycast/enabled", sanitized.enabled);
    cfg.set("/keycast/autoBypassFullscreen", sanitized.autoBypassFullscreen);
    cfg.set("/keycast/showKeyboard", sanitized.showKeyboard);
    cfg.set("/keycast/filterMode", sanitized.filterMode);
    cfg.set("/keycast/includeFunctionKeys", sanitized.includeFunctionKeys);
    cfg.set("/keycast/position", sanitized.position);
    cfg.set("/keycast/mergeRecentKeys", sanitized.mergeRecentKeys);
    cfg.set("/keycast/mergeTimeoutMs", sanitized.mergeTimeoutMs);
    cfg.set("/keycast/displayDurationMs", sanitized.displayDurationMs);
    cfg.set("/keycast/fontSize", sanitized.fontSize);
    cfg.set("/keycast/opacity", sanitized.opacity);
    cfg.set("/keycast/textColor", sanitized.textColor);
    cfg.set("/keycast/backgroundColor", sanitized.backgroundColor);
    cfg.set("/keycast/modifierKeycapColor", sanitized.modifierKeycapColor);
    cfg.set("/keycast/modifierKeycapOpacity", sanitized.modifierKeycapOpacity);
    cfg.set("/keycast/modifierTextColor", sanitized.modifierTextColor);
    cfg.set("/keycast/firstKeyAnim", sanitized.firstKeyAnim);
    cfg.set("/keycast/subsequentKeyAnim", sanitized.subsequentKeyAnim);
    cfg.set("/keycast/rowCascadeAnim", sanitized.rowCascadeAnim);
    cfg.set("/keycast/exitDriftAnim", sanitized.exitDriftAnim);

    applySettings(sanitized);
}

void KeycastOverlay::resetDefaults() {
    KeycastSettings def;
    updateSettings(def);
}

void KeycastOverlay::setAutoBypassFullscreen(bool enable) {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.autoBypassFullscreen = enable;
}

bool KeycastOverlay::autoBypassFullscreen() const {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_settings.autoBypassFullscreen;
}

D2D1_COLOR_F KeycastOverlay::parseColor(const std::string& hex, float alpha) const {
    if (hex == "auto" || hex.empty()) {
        std::string accent = tools3000::core::ConfigManager::instance().get<std::string>("/general/accentColor", "blue");
        tools3000::core::AccentColorRGB rgb = tools3000::core::getAccentColorRGB(accent);
        return D2D1::ColorF(rgb.r, rgb.g, rgb.b, alpha);
    }
    tools3000::core::AccentColorRGB rgb = tools3000::core::parseHexColor(hex);
    return D2D1::ColorF(rgb.r, rgb.g, rgb.b, alpha);
}

void KeycastOverlay::discardResourcesLocked() {
    m_brushText.Reset();
    m_brushModifierText.Reset();
    m_brushBg.Reset();
    m_brushBorder.Reset();
    m_brushKeycapBg.Reset();
    m_brushKeycapBorder.Reset();
    m_brushFuncKeyBg.Reset();
    m_brushFuncKeyBorder.Reset();
    m_brushBadgeBg.Reset();
    m_brushBadgeBorder.Reset();
    m_brushBadgeText.Reset();
    m_brushPlusText.Reset();
    m_renderTarget.Reset();
    m_textFormat.Reset();
    m_keycapTextFormat.Reset();
    m_repeatTextFormat.Reset();
    m_plusTextFormat.Reset();
    if (m_memoryDC && m_oldBitmap) SelectObject(m_memoryDC, m_oldBitmap);
    m_oldBitmap = nullptr;
    if (m_memoryBitmap) DeleteObject(m_memoryBitmap);
    if (m_memoryDC) DeleteDC(m_memoryDC);
    m_memoryBitmap = nullptr;
    m_memoryDC = nullptr;
}

void KeycastOverlay::discardResources() {
    m_needRecreateResources.store(true, std::memory_order_release);
}

void KeycastOverlay::onThemeChanged() {
    KeycastSettings copy;
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        copy = m_settings;
    }
    applySettings(copy);
}

bool KeycastOverlay::createResources() {
    const KeycastSettings settings = getSettings();
    std::lock_guard<std::mutex> renderLock(m_renderMutex);
    if (m_needRecreateResources.exchange(false, std::memory_order_acq_rel)) {
        m_lastAccentColor.clear();
        discardResourcesLocked();
    }
    return createResourcesLocked(settings);
}

bool KeycastOverlay::createResourcesLocked(const KeycastSettings& settings) {
    if (!m_hwnd) return false;
    if (m_renderTarget && m_brushText && m_brushBg && m_brushKeycapBg && m_brushBadgeText) return true;

    if (!m_d2dFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, m_d2dFactory.GetAddressOf()))) {
            return false;
        }
    }
    if (!m_dwriteFactory) {
        if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())
        ))) {
            return false;
        }
    }

    if (!m_memoryDC) {
        HDC hdcScreen = GetDC(nullptr);
        m_memoryDC = CreateCompatibleDC(hdcScreen);
        ReleaseDC(nullptr, hdcScreen);
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m_width;
    bmi.bmiHeader.biHeight = -m_height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    if (m_memoryBitmap) {
        m_renderTarget.Reset();
        if (m_memoryDC && m_oldBitmap) {
            SelectObject(m_memoryDC, m_oldBitmap);
            m_oldBitmap = nullptr;
        }
        DeleteObject(m_memoryBitmap);
        m_memoryBitmap = nullptr;
    }
    void* pBits = nullptr;
    m_memoryBitmap = CreateDIBSection(m_memoryDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!m_memoryBitmap) return false;
    m_oldBitmap = static_cast<HBITMAP>(SelectObject(m_memoryDC, m_memoryBitmap));

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f,
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT
    );

    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcTarget;
    if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &dcTarget))) return false;

    RECT rc = { 0, 0, m_width, m_height };
    if (FAILED(dcTarget->BindDC(m_memoryDC, &rc))) return false;
    m_renderTarget = dcTarget;

    const KeycastDynamicMetrics dyn = computeKeycastMetrics(settings.fontSize, m_dpiScale);

    m_textFormat = createKeycastFormat(m_dwriteFactory.Get(), dyn.fontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    m_keycapTextFormat = createKeycastFormat(m_dwriteFactory.Get(), dyn.keycapFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    m_repeatTextFormat = createKeycastFormat(m_dwriteFactory.Get(), dyn.fontSize * 0.75f, DWRITE_FONT_WEIGHT_BOLD);
    m_plusTextFormat = createKeycastFormat(m_dwriteFactory.Get(), dyn.plusFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    // 动态画刷体系 (100% 深度支持所有自定义配置与智能自适应)
    auto bgColor = (settings.backgroundColor == "auto") ? 
        D2D1::ColorF(0.15f, 0.15f, 0.18f, 0.90f) : 
        parseColor(settings.backgroundColor, 0.90f);

    float luminance = 0.299f * bgColor.r + 0.587f * bgColor.g + 0.114f * bgColor.b;
    bool isDarkBg = (luminance < 0.55f);

    D2D1_COLOR_F textColor;
    D2D1_COLOR_F modifierTextColor;
    D2D1_COLOR_F plusText;
    D2D1_COLOR_F keycapBg;
    D2D1_COLOR_F keycapBorder;
    D2D1_COLOR_F borderColor;
    D2D1_COLOR_F funcKeyBg;
    D2D1_COLOR_F funcKeyBorder;
    D2D1_COLOR_F badgeBg;
    D2D1_COLOR_F badgeBorder;
    D2D1_COLOR_F badgeTextColor;

    // 1. 修饰键底座颜色
    D2D1_COLOR_F baseKeycapColor = parseColor(settings.modifierKeycapColor, 1.0f);
    keycapBg = D2D1::ColorF(baseKeycapColor.r, baseKeycapColor.g, baseKeycapColor.b, 1.0f);

    // 2. 主按键文字颜色 (优先使用用户自定义配置，auto 时自适应黑白)
    if (settings.textColor == "auto") {
        textColor = isDarkBg ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f) : D2D1::ColorF(0.08f, 0.09f, 0.12f, 0.98f);
    } else {
        textColor = parseColor(settings.textColor, 0.98f);
    }

    // 3. 修饰键文字/图标颜色 (优先使用用户自定义配置，auto 时根据底座复合明暗智能自适应)
    if (settings.modifierTextColor == "auto") {
        float keycapAlpha = (std::max)(0.0f, (std::min)(1.0f, static_cast<float>(settings.modifierKeycapOpacity) / 100.0f));
        float effR = keycapAlpha * baseKeycapColor.r + (1.0f - keycapAlpha) * bgColor.r;
        float effG = keycapAlpha * baseKeycapColor.g + (1.0f - keycapAlpha) * bgColor.g;
        float effB = keycapAlpha * baseKeycapColor.b + (1.0f - keycapAlpha) * bgColor.b;
        float effLum = 0.2126f * effR + 0.7152f * effG + 0.0722f * effB;
        if (effLum >= 0.36f) {
            modifierTextColor = D2D1::ColorF(0.08f, 0.08f, 0.12f, 0.95f);
        } else {
            modifierTextColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f);
        }
    } else {
        modifierTextColor = parseColor(settings.modifierTextColor, 0.98f);
    }

    // 4. 连接符 '+'、功能键、连击角标高对比文字与边框
    if (isDarkBg) {
        plusText = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.40f);
        keycapBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f);
        funcKeyBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
        funcKeyBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f);
        badgeBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);
        badgeBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f);
        badgeTextColor = D2D1::ColorF(
            (std::min)(1.0f, baseKeycapColor.r * 0.45f + 0.55f),
            (std::min)(1.0f, baseKeycapColor.g * 0.45f + 0.55f),
            (std::min)(1.0f, baseKeycapColor.b * 0.45f + 0.55f),
            1.0f);
        borderColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
    } else {
        plusText = D2D1::ColorF(0.08f, 0.09f, 0.12f, 0.45f);
        keycapBorder = D2D1::ColorF(baseKeycapColor.r, baseKeycapColor.g, baseKeycapColor.b, 0.35f);
        funcKeyBg = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f);
        funcKeyBorder = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f);
        badgeBg = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f);
        badgeBorder = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f);
        badgeTextColor = D2D1::ColorF(
            baseKeycapColor.r * 0.65f,
            baseKeycapColor.g * 0.65f,
            baseKeycapColor.b * 0.65f,
            1.0f);
        borderColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f);
    }

    m_renderTarget->CreateSolidColorBrush(textColor, &m_brushText);
    m_renderTarget->CreateSolidColorBrush(modifierTextColor, &m_brushModifierText);
    m_renderTarget->CreateSolidColorBrush(bgColor, &m_brushBg);
    m_renderTarget->CreateSolidColorBrush(borderColor, &m_brushBorder);
    m_renderTarget->CreateSolidColorBrush(keycapBg, &m_brushKeycapBg);
    m_renderTarget->CreateSolidColorBrush(keycapBorder, &m_brushKeycapBorder);
    m_renderTarget->CreateSolidColorBrush(funcKeyBg, &m_brushFuncKeyBg);
    m_renderTarget->CreateSolidColorBrush(funcKeyBorder, &m_brushFuncKeyBorder);
    m_renderTarget->CreateSolidColorBrush(badgeBg, &m_brushBadgeBg);
    m_renderTarget->CreateSolidColorBrush(badgeBorder, &m_brushBadgeBorder);
    m_renderTarget->CreateSolidColorBrush(badgeTextColor, &m_brushBadgeText);
    m_renderTarget->CreateSolidColorBrush(plusText, &m_brushPlusText);

    if (!m_brushText || !m_brushModifierText || !m_brushBg || !m_brushBorder ||
        !m_brushKeycapBg || !m_brushKeycapBorder || !m_brushFuncKeyBg ||
        !m_brushFuncKeyBorder || !m_brushBadgeBg || !m_brushBadgeBorder ||
        !m_brushBadgeText || !m_brushPlusText) {
        discardResourcesLocked();
        return false;
    }
    m_lastAccentColor = tools3000::core::ConfigManager::instance().get<std::string>("/general/accentColor", "blue");
    return true;
}

float KeycastOverlay::calculateItemWidth(const KeycastItem& item, float dpiScale) const {
    const KeycastSettings settings = getSettings();
    std::lock_guard<std::mutex> renderLock(m_renderMutex);
    return calculateItemWidthLocked(item, dpiScale, settings);
}

float KeycastOverlay::calculateItemWidthLocked(
    const KeycastItem& item,
    float dpiScale,
    const KeycastSettings& settings) const {
    auto isModifier = [](const std::string& tok) -> bool {
        return tok == "Win" || tok == "Ctrl" || tok == "Alt" || tok == "Shift" ||
               tok == "Cmd" || tok == "Meta" || tok == "Option" || tok == "Super" ||
               tok == "Control" || tok == "Command";
    };

    auto isSpecialKey = [&isModifier](const std::string& tok) -> bool {
        if (isModifier(tok)) return true;
        return tok == "Space" || tok == "Tab" || tok == "Enter" || tok == "Return" ||
               tok == "Backspace" || tok == "Delete" || tok == "Del" || tok == "Esc" ||
               tok == "Escape" || tok == "CapsLock" || tok == "Caps" || tok == "Insert" ||
               tok == "Home" || tok == "End" || tok == "PageUp" || tok == "PageDown" ||
               tok == "PgUp" || tok == "PgDn" || tok == "Up" || tok == "Down" ||
               tok == "Left" || tok == "Right" || tok == "PrtScn" || tok == "PrintScreen" ||
               tok == "ScrollLock" || tok == "Pause" || tok == "NumLock" ||
               tok == "Menu" || tok == "Sleep" || tok == "PlayPause" ||
               tok.starts_with("Volume") || tok.starts_with("Media") ||
               tok.starts_with("Browser") || tok.starts_with("Launch") ||
               (tok.size() >= 2 && tok[0] == 'F' && isdigit(static_cast<unsigned char>(tok[1])));
    };

    const KeycastDynamicMetrics dyn = computeKeycastMetrics(settings.fontSize, dpiScale);
    float keycapAlpha = (std::max)(0.0f, (std::min)(1.0f, static_cast<float>(settings.modifierKeycapOpacity) / 100.0f));
    bool hasModifierBg = (keycapAlpha > 0.01f);

    if (!m_dwriteFactory) {
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }

    const bool canReuseFormats = !m_needRecreateResources.load(std::memory_order_acquire) &&
                                 (std::abs(dpiScale - m_dpiScale) < 0.01f) &&
                                 m_textFormat && m_keycapTextFormat && m_repeatTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFmt = canReuseFormats
        ? m_textFormat
        : createKeycastFormat(m_dwriteFactory.Get(), dyn.fontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Microsoft::WRL::ComPtr<IDWriteTextFormat> keycapFmt = canReuseFormats
        ? m_keycapTextFormat
        : createKeycastFormat(m_dwriteFactory.Get(), dyn.keycapFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Microsoft::WRL::ComPtr<IDWriteTextFormat> repeatFmt = canReuseFormats
        ? m_repeatTextFormat
        : createKeycastFormat(m_dwriteFactory.Get(), dyn.fontSize * 0.75f, DWRITE_FONT_WEIGHT_BOLD);

    auto hasKeycapBox = [&](const std::string& tok) -> bool {
        if (tok == "Win") return hasModifierBg;
        if (isModifier(tok)) return hasModifierBg;
        if (isSpecialKey(tok)) return true;
        return false;
    };

    float totalW = dyn.paddingX;
    for (size_t i = 0; i < item.tokens.size(); ++i) {
        const std::string& token = item.tokens[i];
        bool curHasBox = hasKeycapBox(token);

        if (token == "Win") {
            totalW += curHasBox ? dyn.winWidth : (dyn.fontSize * 1.20f);
        } else if (isModifier(token)) {
            std::wstring wtoken = tools3000::core::WinUtils::utf8ToWstring(token);
            float textW = measureTextLayoutWidth(
                m_dwriteFactory.Get(), keycapFmt.Get(), wtoken, dyn.keycapFontSize * 0.58f);
            float btnW = curHasBox
                ? (std::max)(dyn.winWidth, textW + dyn.fontSize * 0.85f)
                : (textW + dyn.fontSize * 0.25f);
            totalW += btnW;
        } else if (isSpecialKey(token)) {
            std::wstring wtoken = tools3000::core::WinUtils::utf8ToWstring(token);
            float textW = measureTextLayoutWidth(
                m_dwriteFactory.Get(), keycapFmt.Get(), wtoken, dyn.charWidth);
            float minCapW = (token == "Space") ? (dyn.fontSize * 2.2f) : dyn.winWidth;
            float btnW = (std::max)(minCapW, textW + dyn.fontSize * 0.70f);
            totalW += btnW;
        } else {
            std::wstring wtoken = tools3000::core::WinUtils::utf8ToWstring(token);
            float textW = measureTextLayoutWidth(
                m_dwriteFactory.Get(), textFmt.Get(), wtoken, dyn.charWidth);
            float btnW = (std::max)(dyn.fontSize * 0.85f, textW + dyn.fontSize * 0.40f);
            totalW += btnW;
        }

        // 键间 '+' 连接间距：与 drawKeycapCapsule 100% 对齐
        if (i + 1 < item.tokens.size()) {
            bool nextHasBox = hasKeycapBox(item.tokens[i + 1]);
            float leftGap = curHasBox ? (dyn.fontSize * 0.22f) : (dyn.fontSize * 0.16f);
            float plusW = dyn.plusWidth;
            float rightGap = nextHasBox ? (dyn.fontSize * 0.22f) : (dyn.fontSize * 0.18f);
            totalW += leftGap + plusW + rightGap;
        }
    }
    if (item.repeatCount > 1) {
        std::wstring repStr = L"×" + std::to_wstring(item.repeatCount);
        float repTextW = measureTextLayoutWidth(
            m_dwriteFactory.Get(), repeatFmt.Get(), repStr, dyn.fontSize * 0.75f * 0.58f);
        float fullBadgeW = dyn.fontSize * 0.18f + repTextW;
        float progress = (item.badgeWidthProgress > 0.0f) ? item.badgeWidthProgress : 1.0f;
        totalW += fullBadgeW * (std::max)(0.0f, (std::min)(1.0f, progress));
    }
    totalW += dyn.paddingX;
    return std::ceil(totalW);
}

void KeycastOverlay::setDockingRegion(const RECT& region, bool active) {
    {
        std::lock_guard lock(m_mutex);
        m_hasDockingRegion = active;
        m_dockingRegion = region;
        if (!active) {
            m_rows.clear();
            m_lastRawKey.clear();
        }
    }
    tools3000::core::KeyboardHook::instance().setRecordingMode(active);
    if (active) {
        LOG_INFO("KeycastOverlay: 激活录屏选区停靠回显 [{}, {}, {}, {}]", region.left, region.top, region.right, region.bottom);
    } else {
        LOG_INFO("KeycastOverlay: 退出录屏选区停靠，恢复桌面悬浮回显");
    }
    tools3000::core::MainThreadDispatcher::instance().post([this, active]() {
        if (!active) {
            onAnimationCompleteAndHide();
        }
        updatePlacement();
        if (active) {
            render();
        }
    });
}

bool KeycastOverlay::updatePlacement() {
    if (!m_hwnd) return false;
    if (m_updatingPlacement) return true;
    m_updatingPlacement = true;
    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
    const float newScale = tools3000::core::dpi::scaleForMonitor(monitor);
    const RECT work = tools3000::core::dpi::workArea(monitor);

    RECT bounds = work;
    bool docked = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_hasDockingRegion && (m_dockingRegion.right - m_dockingRegion.left >= 100) &&
            (m_dockingRegion.bottom - m_dockingRegion.top >= 80)) {
            bounds = m_dockingRegion;
            docked = true;
        }
    }

    const float targetW = static_cast<float>(bounds.right - bounds.left);
    const float targetH = static_cast<float>(bounds.bottom - bounds.top);

    KeycastSettings settings = getSettings();
    const KeycastDynamicMetrics dyn = computeKeycastMetrics(settings.fontSize, newScale);

    int newWidth = 0;
    int newHeight = 0;
    if (docked) {
        newWidth = static_cast<int>((std::max)(88.0f, targetW - 16.0f * newScale));
        newHeight = static_cast<int>((std::max)(dyn.capsuleHeight + 12.0f * newScale,
                                               (std::min)(targetH * 0.48f, dyn.rowStep * 2.5f + 20.0f * newScale)));
    } else {
        newWidth = static_cast<int>((std::min)(targetW * 0.70f, (std::max)(1200.0f * newScale, targetW * 0.50f + 64.0f * newScale)));
        newHeight = static_cast<int>(dyn.rowStep * 3.6f + 32.0f * newScale);
    }

    const int marginX = tools3000::core::dpi::scaleMetric(docked ? 8 : 32, newScale);
    const int marginY = tools3000::core::dpi::scaleMetric(docked ? 12 : 42, newScale);

    int x = bounds.left + marginX;
    int y = bounds.top + marginY;

    if (docked) {
        // 录屏选区停靠时居中置底
        x = bounds.left + ((bounds.right - bounds.left) - newWidth) / 2;
        y = bounds.bottom - newHeight - marginY;
    } else {
        if (settings.position == "top_left") {
            x = bounds.left + marginX;
            y = bounds.top + marginY;
        } else if (settings.position == "bottom_left") {
            x = bounds.left + marginX;
            y = bounds.bottom - newHeight - marginY;
        } else if (settings.position == "top_center") {
            x = bounds.left + ((bounds.right - bounds.left) - newWidth) / 2;
            y = bounds.top + marginY;
        } else if (settings.position == "bottom_center") {
            x = bounds.left + ((bounds.right - bounds.left) - newWidth) / 2;
            y = bounds.bottom - newHeight - marginY;
        } else if (settings.position == "top_right") {
            x = bounds.right - newWidth - marginX;
            y = bounds.top + marginY;
        } else if (settings.position == "bottom_right") {
            x = bounds.right - newWidth - marginX;
            y = bounds.bottom - newHeight - marginY;
        }
    }

    {
        std::lock_guard<std::mutex> renderLock(m_renderMutex);
        if (std::abs(newScale - m_dpiScale) >= 0.01f ||
            newWidth != m_width || newHeight != m_height) {
            m_dpiScale = newScale;
            m_width = newWidth;
            m_height = newHeight;
            m_needRecreateResources.store(true, std::memory_order_release);
        }
        m_posX = x;
        m_posY = y;
    }
    m_atomicDpiScale.store(newScale, std::memory_order_release);
    m_atomicOverlayWidth.store(newWidth, std::memory_order_release);

    const bool positioned = SetWindowPos(
        m_hwnd, HWND_TOPMOST, x, y, newWidth, newHeight,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
    m_updatingPlacement = false;
    return positioned;
}

void KeycastOverlay::pushKey(const std::string& keyStr) {
    if (keyStr.empty()) return;
    pushKey(splitTokens(keyStr), keyStr);
}

void KeycastOverlay::pushKey(const tools3000::core::KeycastKeyInfo& keyInfo) {
    if (keyInfo.tokens.empty() && keyInfo.rawKey.empty()) return;
    std::string rawKey = keyInfo.rawKey.empty() ? tools3000::core::KeyTranslator::formatCombo(keyInfo.tokens) : keyInfo.rawKey;
    pushKey(keyInfo.tokens, rawKey);
}

void KeycastOverlay::pushKey(const std::vector<std::string>& tokens, const std::string& rawKey) {
    if (tokens.empty() && rawKey.empty()) return;

    bool isDockedRecording = false;
    std::vector<KeycastItem> lastRowSnap;
    {
        std::lock_guard lock(m_mutex);
        isDockedRecording = m_hasDockingRegion;
        if (!m_rows.empty()) {
            lastRowSnap = m_rows.back().items;
        }
    }

    KeycastSettings settings = getSettings();
    // 录屏选区停靠模式拥有最高优先级（录屏工具栏显式开启回显），无视全局停用开关
    if (!isDockedRecording && (!settings.enabled || !settings.showKeyboard)) return;

    // 截图与长截图浮层激活期间（非录屏态），自动清空并抑制新按键胶囊弹出，防止乱入截图画面
    if (!isDockedRecording && isCaptureOverlayVisible()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_rows.clear();
            m_lastRawKey.clear();
        }
        if (m_hwnd && IsWindowVisible(m_hwnd)) {
            tools3000::core::MainThreadDispatcher::instance().post([this]() {
                onAnimationCompleteAndHide();
            });
        }
        return;
    }

    // 录屏停靠模式下严禁触发全屏免打扰避让（录制全屏场景下正需要回显按键）
    if (!isDockedRecording && settings.autoBypassFullscreen) {
        HWND fg = GetForegroundWindow();
        if (fg) {
            const uint64_t nowTick = GetTickCount64();
            bool cachedHit = false;
            bool shouldBypass = false;
            {
                std::lock_guard<std::mutex> cacheLock(m_fgBypassCacheMutex);
                if (fg == m_cachedFgHwnd && (nowTick - m_cachedFgTick) <= 500) {
                    cachedHit = true;
                    shouldBypass = m_cachedFgBypass;
                }
            }
            if (!cachedHit) {
                wchar_t classWide[256] = {0};
                GetClassNameW(fg, classWide, static_cast<int>(std::size(classWide)));
                std::wstring cls = classWide;
                const bool isAllowedOverlay =
                    (cls.rfind(L"Tools3000_", 0) == 0) ||
                    (cls.find(L"ScreenClip") != std::wstring::npos) ||
                    (cls.find(L"Snipping") != std::wstring::npos) ||
                    (cls.find(L"ScreenSketch") != std::wstring::npos) ||
                    (cls.find(L"Qt") != std::wstring::npos) ||
                    (cls == L"Windows.UI.Core.CoreWindow") ||
                    (cls == L"Snipaste") ||
                    (cls == L"WeChatMainWndForPC") ||
                    (cls == L"TXGuiFoundation");

                shouldBypass = !isAllowedOverlay && tools3000::core::WinUtils::shouldBypassOverlaysForWindow(fg);
                std::lock_guard<std::mutex> cacheLock(m_fgBypassCacheMutex);
                m_cachedFgHwnd = fg;
                m_cachedFgTick = nowTick;
                m_cachedFgBypass = shouldBypass;
            }
            if (shouldBypass) {
                return;
            }
        }
    }

    LOG_DEBUG("KeycastOverlay pushKey: rawKey={}, tokensCount={}", rawKey, tokens.size());

    const float curDpiScale = m_atomicDpiScale.load(std::memory_order_acquire);
    const int curOverlayWidth = m_atomicOverlayWidth.load(std::memory_order_acquire);
    const KeycastDynamicMetrics dyn = computeKeycastMetrics(settings.fontSize, curDpiScale);

    KeycastItem item;
    item.rawKey = rawKey;
    item.tokens = tokens.empty() ? splitTokens(rawKey) : tokens;
    item.opacity = 0.0f;

    // 在 m_mutex 锁外使用 m_renderMutex 预测量排版宽度，杜绝 m_mutex -> m_renderMutex 交叉嵌套
    float precalcRowWidth = 0.0f;
    float newItemWidth = 0.0f;
    {
        std::lock_guard<std::mutex> renderLock(m_renderMutex);
        for (const auto& it : lastRowSnap) {
            precalcRowWidth += calculateItemWidthLocked(it, curDpiScale, settings) + dyn.fontSize * 0.45f;
        }
        newItemWidth = calculateItemWidthLocked(item, curDpiScale, settings);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t now = GetTickCount64();
        const uint64_t mergeTimeout = static_cast<uint64_t>(std::clamp(settings.mergeTimeoutMs, 100, 10000));
        const bool isRecent = (now - m_lastGlobalPushTime <= mergeTimeout);
        item.pushTime = now;

        // 按当前目标可用宽度动态计算单行上限（彻底移除 520px 硬保底导致的窄录屏选区键帽腰斩）
        float effectiveWidth = static_cast<float>(curOverlayWidth);
        if (m_hasDockingRegion && (m_dockingRegion.right - m_dockingRegion.left >= 100)) {
            effectiveWidth = (std::max)(88.0f, static_cast<float>(m_dockingRegion.right - m_dockingRegion.left) - 16.0f * curDpiScale);
        }
        const float maxRowWidth = (std::max)(80.0f * curDpiScale, effectiveWidth - 24.0f * curDpiScale);

        float currentRowWidth = m_rows.empty() ? 0.0f : precalcRowWidth;
        bool isSameKeyRepeat = !m_rows.empty() && !m_rows.back().items.empty() &&
                               (m_rows.back().items.back().rawKey == rawKey);
        bool wouldExceedMidline = !isSameKeyRepeat && (currentRowWidth + newItemWidth > maxRowWidth);

        if (settings.mergeRecentKeys && isRecent && !m_rows.empty() && !wouldExceedMidline) {
            // 同一操作时间段且未超可用行宽：追加到当前排末尾
            auto& currentRow = m_rows.back();
            if (!currentRow.items.empty() && currentRow.items.back().rawKey == rawKey) {
                auto& targetItem = currentRow.items.back();
                targetItem.repeatCount++;
                targetItem.pushTime = now;
                
                // 连续二阶物理微脉冲速度
                targetItem.scaleVel += 0.08f;

                // 连击角标流体气泡激发
                if (targetItem.repeatCount == 2) {
                    targetItem.badgeScale = 0.5f;
                    targetItem.badgeScaleVel = 0.15f;
                    targetItem.badgeOpacity = 0.1f;
                } else {
                    targetItem.badgeScaleVel += 0.18f;
                }
            } else {
                // 同一行后续按键入场动效组合
                item.isFirstInRow = false;
                if (settings.subsequentKeyAnim == "pop") {
                    item.offsetX = 0.0f;
                    item.offsetY = 3.0f;
                    item.scale = 0.58f;
                    item.scaleVel = 0.09f;
                    item.opacity = 0.10f;
                } else if (settings.subsequentKeyAnim == "slide") {
                    item.offsetX = 60.0f;
                    item.offsetY = 0.0f;
                    item.scale = 1.0f;
                    item.scaleVel = 0.0f;
                    item.opacity = 0.08f;
                } else if (settings.subsequentKeyAnim == "fade") {
                    item.offsetX = 0.0f;
                    item.offsetY = 0.0f;
                    item.scale = 1.0f;
                    item.scaleVel = 0.0f;
                    item.opacity = 0.10f;
                } else { // "none"
                    item.offsetX = 0.0f;
                    item.offsetY = 0.0f;
                    item.scale = 1.0f;
                    item.scaleVel = 0.0f;
                    item.opacity = 1.0f;
                }
                currentRow.items.push_back(item);
                currentRow.lastActiveTime = now;
            }
        } else {
            // 超过可用宽度限制 或 超时停顿：智能换到新的一行，推移方向严格匹配停靠方位，杜绝顶部停靠累积负偏移冲出 DIB 顶边
            const bool isBottomPlacement = m_hasDockingRegion ||
                                           settings.position == "bottom_left" ||
                                           settings.position == "bottom_center" ||
                                           settings.position == "bottom_right";
            if (!m_rows.empty() && settings.rowCascadeAnim && isBottomPlacement) {
                for (auto& row : m_rows) {
                    row.offsetY += 12.0f;
                    row.targetOffsetY = 0.0f;
                }
            }

            // 每行第 1 个按键入场动效组合
            item.isFirstInRow = true;
            if (settings.firstKeyAnim == "slide") {
                item.offsetX = m_hasDockingRegion ? 24.0f : 100.0f;
                item.offsetY = 0.0f;
                item.scale = 1.0f;
                item.scaleVel = 0.0f;
                item.opacity = 0.05f;
            } else if (settings.firstKeyAnim == "pop") {
                item.offsetX = 0.0f;
                item.offsetY = 3.0f;
                item.scale = 0.58f;
                item.scaleVel = 0.09f;
                item.opacity = 0.10f;
            } else if (settings.firstKeyAnim == "fade") {
                item.offsetX = 0.0f;
                item.offsetY = 0.0f;
                item.scale = 1.0f;
                item.scaleVel = 0.0f;
                item.opacity = 0.10f;
            } else { // "none"
                item.offsetX = 0.0f;
                item.offsetY = 0.0f;
                item.scale = 1.0f;
                item.scaleVel = 0.0f;
                item.opacity = 1.0f;
            }

            KeycastRow newRow;
            newRow.items.push_back(item);
            newRow.lastActiveTime = now;
            if (settings.rowCascadeAnim) {
                newRow.offsetY = isBottomPlacement ? 10.0f : (m_rows.empty() ? 6.0f : -6.0f);
            } else {
                newRow.offsetY = 0.0f;
            }
            newRow.targetOffsetY = 0.0f;
            newRow.opacity = 1.0f;
            m_rows.push_back(newRow);

            // 限制最多保留 3 排活跃行，超出排平滑消融
            if (m_rows.size() > 3) {
                m_rows.front().isExiting = true;
                if (m_rows.size() > 4) {
                    m_rows.erase(m_rows.begin());
                    if (!isBottomPlacement && settings.rowCascadeAnim) {
                        for (auto& row : m_rows) {
                            row.offsetY += 12.0f;
                            row.targetOffsetY = 0.0f;
                        }
                    }
                }
            }
        }

        m_lastRawKey = rawKey;
        m_lastGlobalPushTime = now;
    }

    if (m_hwnd) PostMessageW(m_hwnd, START_ANIMATION_MESSAGE, 0, 0);
}

void KeycastOverlay::drawWindowsLogo(const D2D1_RECT_F& rect, float alpha) {
    if (!m_renderTarget || !m_brushModifierText || !m_d2dFactory) return;

    m_brushModifierText->SetOpacity(0.95f * alpha);
    float w = rect.right - rect.left;
    float h = rect.bottom - rect.top;

    auto mapX = [&](float x) -> float { return rect.left + (x / 640.0f) * w; };
    auto mapY = [&](float y) -> float { return rect.top + (y / 640.0f) * h; };

    Microsoft::WRL::ComPtr<ID2D1PathGeometry> pathGeo;
    if (FAILED(m_d2dFactory->CreatePathGeometry(&pathGeo))) return;

    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(pathGeo->Open(&sink))) return;

    sink->SetFillMode(D2D1_FILL_MODE_WINDING);

    // 窗格 1 [左上]
    sink->BeginFigure(D2D1::Point2F(mapX(0.0f), mapY(290.0f)), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(mapX(0.0f), mapY(95.0f)));
    sink->AddLine(D2D1::Point2F(mapX(265.0f), mapY(58.0f)));
    sink->AddLine(D2D1::Point2F(mapX(265.0f), mapY(290.0f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    // 窗格 2 [右上]
    sink->BeginFigure(D2D1::Point2F(mapX(305.0f), mapY(52.0f)), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(mapX(640.0f), mapY(0.0f)));
    sink->AddLine(D2D1::Point2F(mapX(640.0f), mapY(290.0f)));
    sink->AddLine(D2D1::Point2F(mapX(305.0f), mapY(290.0f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    // 窗格 3 [右下]
    sink->BeginFigure(D2D1::Point2F(mapX(640.0f), mapY(350.0f)), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(mapX(640.0f), mapY(640.0f)));
    sink->AddLine(D2D1::Point2F(mapX(305.0f), mapY(588.0f)));
    sink->AddLine(D2D1::Point2F(mapX(305.0f), mapY(350.0f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    // 窗格 4 [左下]
    sink->BeginFigure(D2D1::Point2F(mapX(265.0f), mapY(350.0f)), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(mapX(265.0f), mapY(582.0f)));
    sink->AddLine(D2D1::Point2F(mapX(0.0f), mapY(545.0f)));
    sink->AddLine(D2D1::Point2F(mapX(0.0f), mapY(350.0f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    sink->Close();
    m_renderTarget->FillGeometry(pathGeo.Get(), m_brushModifierText.Get());
}

void KeycastOverlay::drawKeycapCapsule(
    const KeycastItem& item,
    float startX,
    float startY,
    float alpha,
    float dpiScale,
    const KeycastSettings& settings) {
    if (!m_renderTarget || !m_brushBg || !m_brushText) return;

    auto isModifier = [](const std::string& tok) -> bool {
        return tok == "Win" || tok == "Ctrl" || tok == "Alt" || tok == "Shift" ||
               tok == "Cmd" || tok == "Meta" || tok == "Option" || tok == "Super" ||
               tok == "Control" || tok == "Command";
    };

    auto isSpecialKey = [&isModifier](const std::string& tok) -> bool {
        if (isModifier(tok)) return true;
        return tok == "Space" || tok == "Tab" || tok == "Enter" || tok == "Return" ||
               tok == "Backspace" || tok == "Delete" || tok == "Del" || tok == "Esc" ||
               tok == "Escape" || tok == "CapsLock" || tok == "Caps" || tok == "Insert" ||
               tok == "Home" || tok == "End" || tok == "PageUp" || tok == "PageDown" ||
               tok == "PgUp" || tok == "PgDn" || tok == "Up" || tok == "Down" ||
               tok == "Left" || tok == "Right" || tok == "PrtScn" || tok == "PrintScreen" ||
               tok == "ScrollLock" || tok == "Pause" || tok == "NumLock" ||
               tok == "Menu" || tok == "Sleep" || tok == "PlayPause" ||
               tok.starts_with("Volume") || tok.starts_with("Media") ||
               tok.starts_with("Browser") || tok.starts_with("Launch") ||
               (tok.size() >= 2 && tok[0] == 'F' && isdigit(static_cast<unsigned char>(tok[1])));
    };

    const KeycastDynamicMetrics dyn = computeKeycastMetrics(settings.fontSize, dpiScale);

    float itemH = dyn.capsuleHeight;
    float capHeight = dyn.capHeight;
    float capCenterY = startY + itemH / 2.0f;

    float curX = startX + dyn.paddingX;
    float keycapAlpha = (std::max)(0.0f, (std::min)(1.0f, static_cast<float>(settings.modifierKeycapOpacity) / 100.0f));
    bool hasModifierBg = (keycapAlpha > 0.01f);

    auto hasKeycapBox = [&](const std::string& tok) -> bool {
        if (tok == "Win") return hasModifierBg;
        if (isModifier(tok)) return hasModifierBg;
        if (isSpecialKey(tok)) return true;
        return false;
    };

    for (size_t i = 0; i < item.tokens.size(); ++i) {
        const std::string& token = item.tokens[i];
        bool curHasBox = hasKeycapBox(token);

        if (token == "Win") {
            float winW = curHasBox ? dyn.winWidth : (dyn.fontSize * 1.20f);
            float winH = capHeight;
            float topY = capCenterY - winH / 2.0f;

            if (curHasBox) {
                D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(
                    D2D1::RectF(curX, topY, curX + winW, topY + winH),
                    dyn.capRadius, dyn.capRadius
                );
                m_brushKeycapBg->SetOpacity(keycapAlpha * alpha);
                m_renderTarget->FillRoundedRectangle(&rrect, m_brushKeycapBg.Get());
                m_brushKeycapBorder->SetOpacity((std::min)(1.0f, keycapAlpha * 1.5f + 0.08f) * alpha);
                m_renderTarget->DrawRoundedRectangle(&rrect, m_brushKeycapBorder.Get(), dyn.borderWidth);
            }

            float logoSize = dyn.logoSize;
            D2D1_RECT_F logoRect = D2D1::RectF(
                curX + (winW - logoSize) / 2.0f,
                capCenterY - logoSize / 2.0f,
                curX + (winW + logoSize) / 2.0f,
                capCenterY + logoSize / 2.0f
            );
            drawWindowsLogo(logoRect, alpha);
            curX += winW;
        } else if (isModifier(token)) {
            std::wstring wtoken = tools3000::core::WinUtils::utf8ToWstring(token);
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            m_dwriteFactory->CreateTextLayout(
                wtoken.c_str(), static_cast<UINT32>(wtoken.length()),
                m_keycapTextFormat.Get(), 2000.0f, 1000.0f, &layout);

            DWRITE_TEXT_METRICS m{};
            if (layout) layout->GetMetrics(&m);
            const float textW = (std::max)(m.width, m.widthIncludingTrailingWhitespace);

            float btnW = curHasBox ? 
                (std::max)(dyn.winWidth, textW + dyn.fontSize * 0.85f) : 
                (textW + dyn.fontSize * 0.25f);
            float btnH = capHeight;
            float topY = capCenterY - btnH / 2.0f;

            if (curHasBox) {
                D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(
                    D2D1::RectF(curX, topY, curX + btnW, topY + btnH),
                    dyn.capRadius, dyn.capRadius
                );
                m_brushKeycapBg->SetOpacity(keycapAlpha * alpha);
                m_renderTarget->FillRoundedRectangle(&rrect, m_brushKeycapBg.Get());
                m_brushKeycapBorder->SetOpacity((std::min)(1.0f, keycapAlpha * 1.5f + 0.08f) * alpha);
                m_renderTarget->DrawRoundedRectangle(&rrect, m_brushKeycapBorder.Get(), dyn.borderWidth);
            }

            m_brushModifierText->SetOpacity(0.95f * alpha);
            if (layout) {
                float textDrawX = curX + (btnW - m.width) / 2.0f - m.left;
                float textDrawY = capCenterY - m.height / 2.0f - m.top;
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(textDrawX, textDrawY),
                    layout.Get(), m_brushModifierText.Get());
            }
            curX += btnW;
        } else if (isSpecialKey(token)) {
            std::wstring wtoken = tools3000::core::WinUtils::utf8ToWstring(token);
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            m_dwriteFactory->CreateTextLayout(
                wtoken.c_str(), static_cast<UINT32>(wtoken.length()),
                m_keycapTextFormat.Get(), 2000.0f, 1000.0f, &layout);

            DWRITE_TEXT_METRICS m{};
            if (layout) layout->GetMetrics(&m);
            const float textW = (std::max)(m.width, m.widthIncludingTrailingWhitespace);

            float minCapW = (token == "Space") ? (dyn.fontSize * 2.2f) : dyn.winWidth;
            float btnW = (std::max)(minCapW, textW + dyn.fontSize * 0.70f);
            float btnH = capHeight;
            float topY = capCenterY - btnH / 2.0f;

            D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(
                D2D1::RectF(curX, topY, curX + btnW, topY + btnH),
                dyn.capRadius, dyn.capRadius
            );

            m_brushFuncKeyBg->SetOpacity(alpha);
            m_renderTarget->FillRoundedRectangle(&rrect, m_brushFuncKeyBg.Get());
            m_brushFuncKeyBorder->SetOpacity(alpha);
            m_renderTarget->DrawRoundedRectangle(&rrect, m_brushFuncKeyBorder.Get(), dyn.borderWidth);

            m_brushText->SetOpacity(alpha);
            if (layout) {
                float textDrawX = curX + (btnW - m.width) / 2.0f - m.left;
                float textDrawY = capCenterY - m.height / 2.0f - m.top;
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(textDrawX, textDrawY),
                    layout.Get(), m_brushText.Get());
            }
            curX += btnW;
        } else {
            // 普通单字符键（如 C, V, E 等）: 使用 m_textFormat (SemiBold 清秀字重，无底座纯白高亮悬浮)
            std::wstring wtoken = tools3000::core::WinUtils::utf8ToWstring(token);
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            m_dwriteFactory->CreateTextLayout(
                wtoken.c_str(), static_cast<UINT32>(wtoken.length()),
                m_textFormat.Get(), 2000.0f, 1000.0f, &layout);

            DWRITE_TEXT_METRICS m{};
            if (layout) layout->GetMetrics(&m);
            const float textW = (std::max)(m.width, m.widthIncludingTrailingWhitespace);

            float btnW = (std::max)(dyn.fontSize * 0.85f, textW + dyn.fontSize * 0.40f);

            m_brushText->SetOpacity(alpha);
            if (layout) {
                float textDrawX = curX + (btnW - m.width) / 2.0f - m.left;
                float textDrawY = capCenterY - m.height / 2.0f - m.top;
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(textDrawX, textDrawY),
                    layout.Get(), m_brushText.Get());
            }
            curX += btnW;
        }

        // 绘制键间连接符号 '+' (根据前后是否有底色框进行微米级对称排版)
        if (i + 1 < item.tokens.size()) {
            bool nextHasBox = hasKeycapBox(item.tokens[i + 1]);
            float leftGap = curHasBox ? (dyn.fontSize * 0.22f) : (dyn.fontSize * 0.16f);
            float rightGap = nextHasBox ? (dyn.fontSize * 0.22f) : (dyn.fontSize * 0.18f);

            curX += leftGap;
            std::wstring plusStr = L"+";
            Microsoft::WRL::ComPtr<IDWriteTextLayout> plusLayout;
            m_dwriteFactory->CreateTextLayout(
                plusStr.c_str(), 1,
                m_plusTextFormat ? m_plusTextFormat.Get() : m_textFormat.Get(),
                2000.0f, 1000.0f, &plusLayout);
            DWRITE_TEXT_METRICS pm{};
            if (plusLayout) plusLayout->GetMetrics(&pm);

            m_brushPlusText->SetOpacity(0.80f * alpha);
            if (plusLayout) {
                float plusX = curX + (dyn.plusWidth - pm.width) / 2.0f - pm.left;
                float plusY = capCenterY - pm.height / 2.0f - pm.top;
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(plusX, plusY),
                    plusLayout.Get(), m_brushPlusText.Get());
            }
            curX += dyn.plusWidth + rightGap;
        }
    }

    // 重复按键指示器 ×2 / ×4（使用不透明高对比 m_brushBadgeText 绘制文字，彻底根治误用 12% 底色画刷隐形缺陷）
    if (item.repeatCount > 1 && item.badgeWidthProgress > 0.05f) {
        curX += dyn.fontSize * 0.18f * item.badgeWidthProgress;
        std::wstring repStr = L"×" + std::to_wstring(item.repeatCount);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> repLayout;
        m_dwriteFactory->CreateTextLayout(
            repStr.c_str(), static_cast<UINT32>(repStr.length()),
            m_repeatTextFormat ? m_repeatTextFormat.Get() : m_keycapTextFormat.Get(),
            2000.0f, 1000.0f, &repLayout);
        DWRITE_TEXT_METRICS rm{};
        if (repLayout) repLayout->GetMetrics(&rm);
        const float repTextW = (std::max)(rm.width, rm.widthIncludingTrailingWhitespace);

        float badgeW = repTextW * item.badgeWidthProgress;
        float finalAlpha = alpha * item.badgeOpacity;

        if (finalAlpha > 0.01f && badgeW > 1.0f) {
            D2D1_MATRIX_3X2_F oldTr;
            m_renderTarget->GetTransform(&oldTr);
            const bool hasBadgeScale = (std::abs(item.badgeScale - 1.0f) > 0.01f);
            if (hasBadgeScale) {
                float bcX = curX + badgeW / 2.0f;
                float bcY = capCenterY;
                D2D1_MATRIX_3X2_F sMat = D2D1::Matrix3x2F::Scale(item.badgeScale, item.badgeScale, D2D1::Point2F(bcX, bcY));
                m_renderTarget->SetTransform(sMat * oldTr);
            }

            ID2D1SolidColorBrush* badgeTextBrush = m_brushBadgeText ? m_brushBadgeText.Get() : m_brushText.Get();
            badgeTextBrush->SetOpacity(finalAlpha * 0.98f);
            if (repLayout) {
                float badgeDrawX = curX + (badgeW - rm.width) / 2.0f - rm.left;
                float badgeDrawY = capCenterY - rm.height / 2.0f - rm.top;
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(badgeDrawX, badgeDrawY),
                    repLayout.Get(), badgeTextBrush);
            }

            if (hasBadgeScale) {
                m_renderTarget->SetTransform(oldTr);
            }
        }
    }
}

void KeycastOverlay::render() {
    if (!m_hwnd) return;

    const KeycastSettings settings = getSettings();

    std::vector<KeycastRow> localRows;
    bool dockedNow = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        localRows = m_rows;
        dockedNow = m_hasDockingRegion;
    }

    std::lock_guard<std::mutex> renderLock(m_renderMutex);
    if (!m_hwnd) return;

    if (m_needRecreateResources.exchange(false, std::memory_order_acq_rel)) {
        m_lastAccentColor.clear();
        discardResourcesLocked();
    }

    // 动态检测全局主题色/强调色实时变更 (0ms 极速响应)
    std::string curAccent = tools3000::core::ConfigManager::instance().get<std::string>("/general/accentColor", "blue");
    if (m_lastAccentColor != curAccent) {
        m_lastAccentColor = curAccent;
        discardResourcesLocked();
    }

    if (!createResourcesLocked(settings) || !m_renderTarget || !m_memoryDC) {
        return;
    }

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0)); // 透明背景

    const KeycastDynamicMetrics dyn = computeKeycastMetrics(settings.fontSize, m_dpiScale);

    const bool isBottom = dockedNow ||
                          (settings.position == "bottom_left" ||
                           settings.position == "bottom_center" ||
                           settings.position == "bottom_right");

    const float topSafePad = (dockedNow ? 8.0f : 14.0f) * m_dpiScale;
    float currentY = topSafePad;
    const float edgePad = (dockedNow ? 6.0f : 12.0f) * m_dpiScale;
    const float maxAvailRowWidth = (std::max)(48.0f, static_cast<float>(m_width) - edgePad * 2.0f);

    for (size_t r = 0; r < localRows.size(); ++r) {
        const auto& row = localRows[r];
        if (row.opacity < 0.02f) continue;

        // 1. 精确测量当前行所有胶囊并排的总宽度
        float rowWidth = 0.0f;
        for (size_t c = 0; c < row.items.size(); ++c) {
            rowWidth += calculateItemWidthLocked(row.items[c], m_dpiScale, settings);
            if (c + 1 < row.items.size()) {
                rowWidth += dyn.fontSize * 0.45f; // 胶囊水平间距
            }
        }

        // 2. 窄录屏选区防裁剪保护：若单行组合键超出目标选区可用宽度，按对齐锚点等比自适应缩放
        const float rowFitScale = (rowWidth > maxAvailRowWidth && rowWidth > 1.0f)
            ? (maxAvailRowWidth / rowWidth)
            : 1.0f;

        float startX = edgePad;
        float pivotX = edgePad;
        if (dockedNow || settings.position == "bottom_center" || settings.position == "top_center") {
            startX = (m_width - rowWidth) / 2.0f;
            pivotX = m_width / 2.0f;
        } else if (settings.position == "top_right" || settings.position == "bottom_right") {
            startX = m_width - edgePad - rowWidth;
            pivotX = m_width - edgePad;
        } else {
            startX = edgePad;
            pivotX = edgePad;
        }

        // 3. 垂直排版计算：底部位置时最新行位于最底行，顶部位置时确保不越过 DIB 顶边安全阈值
        float actualY = 0.0f;
        if (isBottom) {
            float baseY = m_height - dyn.capsuleHeight - dyn.fontSize * 0.35f;
            actualY = baseY - static_cast<float>(localRows.size() - 1 - r) * dyn.rowStep + row.offsetY * m_dpiScale;
        } else {
            actualY = (std::max)(4.0f * m_dpiScale, currentY + row.offsetY * m_dpiScale);
        }

        D2D1_MATRIX_3X2_F rowBaseTransform;
        m_renderTarget->GetTransform(&rowBaseTransform);
        if (rowFitScale < 0.999f) {
            const float pivotY = actualY + dyn.capsuleHeight / 2.0f;
            m_renderTarget->SetTransform(
                D2D1::Matrix3x2F::Scale(rowFitScale, rowFitScale, D2D1::Point2F(pivotX, pivotY)) * rowBaseTransform);
        }

        float currentX = startX;
        for (size_t c = 0; c < row.items.size(); ++c) {
            const auto& item = row.items[c];
            float itemAlpha = row.opacity * item.opacity;
            if (itemAlpha < 0.01f) continue;

            float drawX = currentX + item.offsetX * m_dpiScale;
            float drawY = actualY + item.offsetY * m_dpiScale;
            if (!isBottom) {
                drawY = (std::max)(3.0f * m_dpiScale, drawY);
            }

            float totalCapsuleW = calculateItemWidthLocked(item, m_dpiScale, settings);

            // 整个胶囊原子化同心同轴矩阵变换 (Scale Pop Matrix)
            D2D1_MATRIX_3X2_F oldTransform;
            m_renderTarget->GetTransform(&oldTransform);
            const bool hasScaleTransform = (std::abs(item.scale - 1.0f) > 0.005f);

            if (hasScaleTransform) {
                float centerX = drawX + totalCapsuleW / 2.0f;
                float centerY = drawY + dyn.capsuleHeight / 2.0f;
                D2D1_MATRIX_3X2_F scaleMatrix = D2D1::Matrix3x2F::Scale(
                    item.scale, item.scale, D2D1::Point2F(centerX, centerY));
                m_renderTarget->SetTransform(scaleMatrix * oldTransform);
            }

            // 1. 绘制胶囊托盘背景
            D2D1_ROUNDED_RECT capsuleRect = D2D1::RoundedRect(
                D2D1::RectF(drawX, drawY, drawX + totalCapsuleW, drawY + dyn.capsuleHeight),
                dyn.capsuleRadius, dyn.capsuleRadius
            );

            const float bgUserAlpha = static_cast<float>((std::clamp)(settings.opacity, 20, 100)) / 100.0f;
            m_brushBg->SetOpacity(bgUserAlpha * itemAlpha);
            m_renderTarget->FillRoundedRectangle(&capsuleRect, m_brushBg.Get());

            m_brushBorder->SetOpacity((std::min)(0.35f, bgUserAlpha * 0.40f) * itemAlpha);
            m_renderTarget->DrawRoundedRectangle(&capsuleRect, m_brushBorder.Get(), dyn.borderWidth);

            // 2. 绘制键帽微凸槽与内容
            drawKeycapCapsule(item, drawX, drawY, itemAlpha, m_dpiScale, settings);

            if (hasScaleTransform) {
                m_renderTarget->SetTransform(oldTransform);
            }

            currentX += totalCapsuleW + dyn.fontSize * 0.45f;
        }

        if (rowFitScale < 0.999f) {
            m_renderTarget->SetTransform(rowBaseTransform);
        }

        if (!isBottom) {
            currentY += dyn.rowStep;
        }
    }

    const HRESULT endHr = m_renderTarget->EndDraw();
    if (endHr == D2DERR_RECREATE_TARGET || FAILED(endHr)) {
        discardResourcesLocked();
        return;
    }

    HDC hdcScreen = GetDC(nullptr);
    POINT ptDst = {m_posX, m_posY};
    POINT ptSrc = {0, 0};
    SIZE size = {m_width, m_height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m_hwnd, hdcScreen, &ptDst, &size, m_memoryDC, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, hdcScreen);
}

void KeycastOverlay::tickAnimation() {
    const KeycastSettings settings = getSettings();
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t now = GetTickCount64();
    const uint64_t holdTime = static_cast<uint64_t>(std::clamp(settings.displayDurationMs, 200, 30000));

    for (auto it = m_rows.begin(); it != m_rows.end();) {
        auto& row = *it;
        const uint64_t elapsed = now - row.lastActiveTime;

        // 1. 行级物理平滑推移动画 (新行推旧行向上平滑推移)
        row.offsetY += (row.targetOffsetY - row.offsetY) * 0.28f;

        // 2. 项级物理动力学 (连击连续流体演进 + 长距离阻尼滑行 + 二阶弹簧气泡)
        for (auto& item : row.items) {
            // 连击角标与胶囊宽度物理连续延展 (Continuous Badge Fluid Expansion)
            if (item.repeatCount > 1) {
                item.badgeWidthProgress += (1.0f - item.badgeWidthProgress) * 0.28f;
                if (std::abs(1.0f - item.badgeWidthProgress) < 0.005f) item.badgeWidthProgress = 1.0f;

                float bSpringK = 0.40f;
                float bDamping = 0.62f;
                float bForce = bSpringK * (1.0f - item.badgeScale);
                item.badgeScaleVel = (item.badgeScaleVel + bForce) * bDamping;
                item.badgeScale += item.badgeScaleVel;

                item.badgeOpacity += (1.0f - item.badgeOpacity) * 0.35f;
                if (item.badgeOpacity > 0.99f) item.badgeOpacity = 1.0f;
            }

            // 主按键弹性微颤阻尼逼近
            if (std::abs(item.scale - 1.0f) > 0.005f || std::abs(item.scaleVel) > 0.005f) {
                float springK = 0.36f;
                float damping = 0.65f;
                float force = springK * (1.0f - item.scale);
                item.scaleVel = (item.scaleVel + force) * damping;
                item.scale += item.scaleVel;
            } else {
                item.scale = 1.0f;
                item.scaleVel = 0.0f;
            }

            if (item.isFirstInRow) {
                item.offsetX += (0.0f - item.offsetX) * 0.18f;
                if (std::abs(item.offsetX) < 0.25f) item.offsetX = 0.0f;
                item.opacity += (1.0f - item.opacity) * 0.22f;
            } else {
                if (settings.subsequentKeyAnim == "slide") {
                    item.offsetX += (0.0f - item.offsetX) * 0.20f;
                    if (std::abs(item.offsetX) < 0.25f) item.offsetX = 0.0f;
                    item.opacity += (1.0f - item.opacity) * 0.25f;
                } else if (settings.subsequentKeyAnim == "pop") {
                    item.offsetY += (0.0f - item.offsetY) * 0.32f;
                    item.opacity += (1.0f - item.opacity) * 0.42f;
                } else {
                    item.opacity += (1.0f - item.opacity) * 0.35f;
                }
            }
        }

        // 3. 寿命到期 或 新行挤出时的平滑消融微漂移动效（顶部停靠严禁向上越界撞切顶边）
        if (elapsed > holdTime || row.isExiting) {
            float fadeSpeed = row.isExiting ? 0.08f : 0.045f;
            row.opacity = (std::max)(0.0f, row.opacity - fadeSpeed);
            if (settings.exitDriftAnim) {
                const bool isBottomPlacement = m_hasDockingRegion ||
                                               settings.position == "bottom_left" ||
                                               settings.position == "bottom_center" ||
                                               settings.position == "bottom_right";
                if (isBottomPlacement) {
                    row.targetOffsetY = (std::max)(row.targetOffsetY - 0.6f, -8.0f);
                } else {
                    row.targetOffsetY = (std::min)(row.targetOffsetY + 0.45f, 6.0f);
                }
            }
        }

        if (row.opacity <= 0.01f) {
            it = m_rows.erase(it);
        } else {
            ++it;
        }
    }
}

void KeycastOverlay::onAnimationCompleteAndHide() {
    if (m_hwnd) {
        KillTimer(m_hwnd, ANIMATION_TIMER_ID);
        m_timerRunning = false;
        ShowWindow(m_hwnd, SW_HIDE);
    }
}

LRESULT CALLBACK KeycastOverlay::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == START_ANIMATION_MESSAGE) {
        auto& self = KeycastOverlay::instance();
        if (!self.updatePlacement() || !self.createResources()) {
            LOG_ERROR("按键回显初始化失败: 无法创建渲染表面");
            return 0;
        }
        self.render();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (!self.m_timerRunning) {
            SetTimer(hwnd, ANIMATION_TIMER_ID, 16, nullptr); // 60fps 物理帧循环
            self.m_timerRunning = true;
        }
        return 0;
    }

    if (msg == REFRESH_RESOURCES_MESSAGE) {
        auto& self = KeycastOverlay::instance();
        self.updatePlacement();
        if (IsWindowVisible(hwnd)) {
            self.render();
        }
        return 0;
    }

    if (msg == WM_TIMER && wParam == ANIMATION_TIMER_ID) {
        auto& self = KeycastOverlay::instance();
        self.tickAnimation();

        bool hasActiveRows = false;
        {
            std::lock_guard<std::mutex> lock(self.m_mutex);
            hasActiveRows = !self.m_rows.empty();
        }

        if (hasActiveRows) {
            self.render();
        } else {
            self.onAnimationCompleteAndHide();
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::keycast

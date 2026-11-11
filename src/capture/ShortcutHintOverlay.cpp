#include "capture/ShortcutHintOverlay.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/accessibility/OverlayUiaProvider.h"
#include "capture/ShortcutHintStyle.h"
#include "capture/CaptureHudPalette.h"
#include "capture/FloatingGlassBar.h"

#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace tools3000::capture {

namespace {
constexpr const wchar_t* WindowClass = L"Tools3000_ShortcutHintOverlay";
constexpr UINT_PTR kWatchdogTimerId = 1;
constexpr UINT_PTR kAnimTimerId = 2;
constexpr UINT kAnimIntervalMs = 16;

bool useChineseLabels() {
    return tools3000::core::isUiLanguageChinese();
}

std::wstring configuredShortcut(const char* name, const wchar_t* fallback) {
    for (const auto& entry : tools3000::core::HotkeyManager::instance().getAllHotkeys()) {
        if (entry.name == name) {
            const auto text = entry.def.toString();
            return text.empty() ? std::wstring{} : tools3000::core::WinUtils::utf8ToWstring(text);
        }
    }
    return fallback;
}

bool highContrastEnabled() {
    HIGHCONTRASTW highContrast{sizeof(highContrast)};
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
           (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

D2D1_COLOR_F systemColor(int colorIndex, float alpha = 1.0f) {
    const COLORREF color = GetSysColor(colorIndex);
    return D2D1::ColorF(
        GetRValue(color) / 255.0f,
        GetGValue(color) / 255.0f,
        GetBValue(color) / 255.0f,
        alpha);
}
}  // namespace

ShortcutHintOverlay& ShortcutHintOverlay::instance() {
    static ShortcutHintOverlay overlay;
    return overlay;
}

bool ShortcutHintOverlay::checkOwnerActive(ShortcutHintContext context) const {
    if (m_ownerActiveChecker) {
        return m_ownerActiveChecker(context);
    }
    switch (context) {
        case ShortcutHintContext::CaptureSelecting:
        case ShortcutHintContext::CaptureDragging:
        case ShortcutHintContext::CaptureSelected:
        case ShortcutHintContext::CaptureMarking:
        case ShortcutHintContext::CaptureMarkingNumber:
        case ShortcutHintContext::CaptureEditingText:
        case ShortcutHintContext::RecordSelecting: {
            HWND captureHwnd = FindWindowW(L"Tools3000_CaptureOverlay", nullptr);
            return captureHwnd != nullptr && IsWindowVisible(captureHwnd) != FALSE;
        }
        case ShortcutHintContext::ScrollCapture: {
            HWND scrollHwnd = FindWindowW(L"Tools3000_ScrollCaptureOverlay", nullptr);
            return scrollHwnd != nullptr && IsWindowVisible(scrollHwnd) != FALSE;
        }
        case ShortcutHintContext::Recording:
        case ShortcutHintContext::RecordingPaused: {
            HWND recHwnd = FindWindowW(L"Tools3000_RecordingIndicator", nullptr);
            return recHwnd != nullptr && IsWindowVisible(recHwnd) != FALSE;
        }
        default:
            return false;
    }
}

bool ShortcutHintOverlay::isVisible() const {
    return m_hasContext && m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd);
}

RECT ShortcutHintOverlay::getBounds() const {
    if (!isVisible()) return {};
    if (m_width > 0 && m_height > 0) {
        return RECT{m_targetPos.x, m_targetPos.y, m_targetPos.x + m_width, m_targetPos.y + m_height};
    }
    RECT r{};
    GetWindowRect(m_hwnd, &r);
    return r;
}

RECT ShortcutHintOverlay::getAnimatedWindowBounds() const {
    if (!isVisible()) return {};
    RECT r{};
    GetWindowRect(m_hwnd, &r);
    return r;
}

void ShortcutHintOverlay::stepAnimationForTest(float dtSeconds) {
    if (!isVisible() || !m_animating) return;
    advanceAnimation(dtSeconds);
}

std::wstring ShortcutHintOverlay::headerTitleFor(ShortcutHintContext context) const {
    const bool zh = useChineseLabels();
    switch (context) {
        case ShortcutHintContext::CaptureSelecting:
            return zh ? L"智能寻窗与像素取色" : L"Window Snap & Pixel Color";
        case ShortcutHintContext::CaptureDragging:
            return zh ? L"拖拽框选区域" : L"Drag Region Selection";
        case ShortcutHintContext::CaptureSelected:
            return zh ? L"选区微调与快捷标注" : L"Selection & Quick Markup";
        case ShortcutHintContext::CaptureMarking:
            return zh ? L"图形绘制与几何约束" : L"Drawing & Snap Modifiers";
        case ShortcutHintContext::CaptureMarkingNumber:
            return zh ? L"序号标记与引线模式" : L"Step Badge & Leader Arrow";
        case ShortcutHintContext::CaptureEditingText:
            return zh ? L"富文本原地编辑" : L"Rich Text In-Place Editing";
        case ShortcutHintContext::RecordSelecting:
            return zh ? L"区域录屏取景" : L"Screen Recording Region";
        case ShortcutHintContext::ScrollCapture:
            return zh ? L"智能滚动长截图" : L"Scrolling Long Capture";
        case ShortcutHintContext::Recording:
            return zh ? L"屏幕录制进行中" : L"Recording Active";
        case ShortcutHintContext::RecordingPaused:
            return zh ? L"屏幕录制已暂停" : L"Recording Paused";
    }
    return zh ? L"快捷键指引" : L"Shortcuts";
}

std::vector<ShortcutHintItem>
ShortcutHintOverlay::itemsFor(ShortcutHintContext context) const {
    const bool zh = useChineseLabels();
    switch (context) {
        case ShortcutHintContext::CaptureSelecting:
            return zh
                ? std::vector<ShortcutHintItem>{
                      {L"拖拽", L"框选区域"},
                      {L"单击", L"智能选窗"},
                      {L"Tab / 滚轮", L"切换窗口层级"},
                      {L"WASD", L"1px 像素微调"},
                      {L"C", L"复制色值 (C / K)"},
                      {L"Shift", L"单按切换色制"},
                      {L"Space", L"按住平移"},
                      {L"Ctrl+A / Esc", L"全屏 / 退出"}}
                : std::vector<ShortcutHintItem>{
                      {L"Drag", L"Select area"},
                      {L"Click", L"Auto window"},
                      {L"Tab / Wheel", L"Window hierarchy"},
                      {L"WASD", L"1px cursor nudge"},
                      {L"C", L"Copy color (C / K)"},
                      {L"Shift", L"Tap to cycle format"},
                      {L"Space", L"Hold to pan"},
                      {L"Ctrl+A / Esc", L"Full screen / Exit"}};
        case ShortcutHintContext::CaptureDragging:
            return zh
                ? std::vector<ShortcutHintItem>{{L"W/A/S/D", L"移动鼠标光标"}, {L"Shift / Tab", L"切换吸附 / 辅助微调"},
                                                {L"Space", L"按住平移选区"}, {L"Esc", L"取消"}}
                : std::vector<ShortcutHintItem>{{L"W/A/S/D", L"Move cursor"}, {L"Shift / Tab", L"Toggle snap / nudge"},
                                                {L"Space", L"Hold to pan"}, {L"Esc", L"Cancel"}};
        case ShortcutHintContext::CaptureSelected:
            return zh
                ? std::vector<ShortcutHintItem>{
                      {L"Enter", L"完成并复制"},
                      {L"Ctrl+T", L"贴图置顶"},
                      {L"Ctrl+S", L"另存图片"},
                      {L"Ctrl+C", L"复制选区"},
                      {L"WASD", L"1px 微调移动"},
                      {L"Shift+WASD", L"尺寸微调"},
                      {L"[ / ]", L"圆角调节"},
                      {L"C / K", L"吸取像素色"},
                      {L"R / Q / T / N", L"矩形/箭头/字/号"},
                      {L"Esc", L"分级返回"}}
                : std::vector<ShortcutHintItem>{
                      {L"Enter", L"Copy & finish"},
                      {L"Ctrl+T", L"Pin to screen"},
                      {L"Ctrl+S", L"Save image"},
                      {L"Ctrl+C", L"Copy region"},
                      {L"WASD", L"1px nudge"},
                      {L"Shift+WASD", L"Resize"},
                      {L"[ / ]", L"Corner radius"},
                      {L"C / K", L"Pick pixel color"},
                      {L"R / Q / T / N", L"Rect/Arrow/Text/Step"},
                      {L"Esc", L"Step back"}};
        case ShortcutHintContext::CaptureMarking:
            return zh
                ? std::vector<ShortcutHintItem>{
                      {L"滚轮 / + -", L"调节笔触粗细"},
                      {L"Shift", L"约束正圆 / 45°"},
                      {L"1 ~ 7", L"切换预设画笔色"},
                      {L"Ctrl+Z / Y", L"撤销 / 重做"},
                      {L"Delete", L"删除选中标注"},
                      {L"Esc", L"退出当前工具"}}
                : std::vector<ShortcutHintItem>{
                      {L"Wheel / + -", L"Stroke width"},
                      {L"Shift", L"Snap 0°/45°/90°"},
                      {L"1 ~ 7", L"Preset colors"},
                      {L"Ctrl+Z / Y", L"Undo / Redo"},
                      {L"Delete", L"Delete element"},
                      {L"Esc", L"Exit current tool"}};
        case ShortcutHintContext::CaptureMarkingNumber:
            return zh
                ? std::vector<ShortcutHintItem>{
                      {L"单击", L"放置序号徽章"},
                      {L"按住拖拽", L"引出指示箭头"},
                      {L"[ / ]", L"递减 / 递增序号"},
                      {L"+ / -", L"缩放序号尺寸"},
                      {L"Ctrl+Z", L"撤销上一步"},
                      {L"Esc", L"退出序号模式"}}
                : std::vector<ShortcutHintItem>{
                      {L"Click", L"Place badge"},
                      {L"Drag", L"Leader arrow"},
                      {L"[ / ]", L"Number - / +"},
                      {L"+ / -", L"Scale badge"},
                      {L"Ctrl+Z", L"Undo"},
                      {L"Esc", L"Exit step mode"}};
        case ShortcutHintContext::CaptureEditingText:
            return zh
                ? std::vector<ShortcutHintItem>{
                      {L"Enter", L"插入换行"},
                      {L"Ctrl+Enter", L"完成文本编辑"},
                      {L"Ctrl+A / 拖拽", L"选中文本片段"},
                      {L"Ctrl+C / V", L"复制 / 粘贴"},
                      {L"Esc", L"退出编辑"}}
                : std::vector<ShortcutHintItem>{
                      {L"Enter", L"Insert newline"},
                      {L"Ctrl+Enter", L"Finish editing"},
                      {L"Ctrl+A / Drag", L"Select text"},
                      {L"Ctrl+C / V", L"Copy / Paste"},
                      {L"Esc", L"Exit editing"}};
        case ShortcutHintContext::RecordSelecting:
            return zh
                ? std::vector<ShortcutHintItem>{
                      {L"拖拽", L"框选录制区域"},
                      {L"Tab / 滚轮", L"切换窗口层级"},
                      {L"WASD", L"1px 像素微调"},
                      {L"Space", L"按住平移"},
                      {L"Ctrl+A", L"全屏录制区域"},
                      {L"Enter", L"确认录制区域"},
                      {L"Shift+1~5", L"锁定宽高比例"},
                      {L"Esc", L"取消"}}
                : std::vector<ShortcutHintItem>{
                      {L"Drag", L"Select area"},
                      {L"Tab / Wheel", L"Window hierarchy"},
                      {L"WASD", L"1px nudge"},
                      {L"Space", L"Hold to pan"},
                      {L"Ctrl+A", L"Full screen"},
                      {L"Enter", L"Confirm area"},
                      {L"Shift+1~5", L"Aspect ratio"},
                      {L"Esc", L"Cancel"}};
        case ShortcutHintContext::ScrollCapture:
            return zh
                ? std::vector<ShortcutHintItem>{{L"Space", L"暂停自动滚动"}, {L"Ctrl+T", L"完成并贴图"}, {L"Ctrl+S", L"完成并另存为"}, {L"Ctrl+C", L"完成并复制"}, {L"Enter", L"完成长截图"}, {L"Esc", L"取消"}}
                : std::vector<ShortcutHintItem>{{L"Space", L"Pause auto-scroll"}, {L"Ctrl+T", L"Finish and pin"}, {L"Ctrl+S", L"Finish and save as"}, {L"Ctrl+C", L"Finish and copy"}, {L"Enter", L"Finish capture"}, {L"Esc", L"Cancel"}};
        case ShortcutHintContext::Recording:
        case ShortcutHintContext::RecordingPaused: {
            std::vector<ShortcutHintItem> items;
            if (const auto pause = configuredShortcut("Record Pause", L"Ctrl+Shift+P"); !pause.empty()) {
                items.push_back({pause, zh
                    ? (context == ShortcutHintContext::RecordingPaused ? L"继续录制" : L"暂停录制")
                    : (context == ShortcutHintContext::RecordingPaused ? L"Resume" : L"Pause")});
            }
            if (const auto stop = configuredShortcut("Record", L"Ctrl+Shift+R"); !stop.empty()) {
                items.push_back({stop, zh ? L"停止并保存" : L"Stop and save"});
            }
            if (const auto mute = configuredShortcut("Mute Microphone", L""); !mute.empty()) {
                items.push_back({mute, zh ? L"麦克风静音" : L"Mute microphone"});
            }
            return items;
        }
    }
    return {};
}

bool ShortcutHintOverlay::createWindow() {
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ShortcutHintOverlay::wndProc), &m_module)) {
        return false;
    }
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = wndProc;
    windowClass.hInstance = m_module;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = WindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        m_module = nullptr;
        return false;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WindowClass, L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, windowClass.hInstance, this);
    if (!m_hwnd) return false;
    // 零隐私防截策略 (Zero WDA_EXCLUDEFROMCAPTURE)：保证快捷键指引 HUD 能够被 Windows 截图管线正常截取
    SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
    return true;
}

bool ShortcutHintOverlay::createResources(float scale) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 m_d2dFactory.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.ReleaseAndGetAddressOf())))) return false;

    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (FAILED(m_d2dFactory->CreateDCRenderTarget(
            &properties, m_renderTarget.ReleaseAndGetAddressOf()))) return false;
    m_renderTarget->SetDpi(96.0f, 96.0f);

    const float keySize = ShortcutHintStyle::BaseKeyFont * scale;
    const float labelSize = ShortcutHintStyle::BaseLabelFont * scale;
    const float headerSize = ShortcutHintStyle::BaseHeaderFont * scale;
    const float badgeSize = ShortcutHintStyle::BaseBadgeFont * scale;
    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
    const wchar_t* localeName = useChineseLabels() ? L"zh-CN" : L"en-US";

    if (FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, keySize, localeName, m_keyFormat.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, labelSize, localeName, m_labelFormat.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, headerSize, localeName, m_headerFormat.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, badgeSize, localeName, m_badgeFormat.ReleaseAndGetAddressOf()))) return false;

    m_keyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_keyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_keyFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    m_labelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_labelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_labelFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    m_headerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_headerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_headerFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    m_badgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_badgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_badgeFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (highContrastEnabled()) {
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOW), &m_panelBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_borderBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_HIGHLIGHT), &m_keyBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_keyBevelBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_keyBorderBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_HIGHLIGHTTEXT), &m_keyTextBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_labelBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_headerBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_HIGHLIGHT), &m_accentBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_sheenBrush);
    } else {
        const auto pal = FloatingGlassBar::resolveThemePalette();
        if (pal.isDark) {
            m_renderTarget->CreateSolidColorBrush(pal.panelBg, &m_panelBrush);
            m_renderTarget->CreateSolidColorBrush(pal.panelOuterBorder, &m_borderBrush);
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.94f), &m_keyBrush);
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.22f), &m_keyBevelBrush);
            m_renderTarget->CreateSolidColorBrush(pal.accentBorder, &m_keyBorderBrush);
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 1.0f), &m_keyTextBrush);
            m_renderTarget->CreateSolidColorBrush(pal.textPrimary, &m_labelBrush);
            m_renderTarget->CreateSolidColorBrush(pal.textPrimary, &m_headerBrush);
            m_renderTarget->CreateSolidColorBrush(pal.accent, &m_accentBrush);
            m_renderTarget->CreateSolidColorBrush(pal.panelInnerHighlight, &m_sheenBrush);
        } else {
            // 雪瓷白微晶玻璃底舱 + 动态强调色点缀立体浅瓷键帽 + 深石板高对比文字
            m_renderTarget->CreateSolidColorBrush(pal.panelBg, &m_panelBrush);
            m_renderTarget->CreateSolidColorBrush(pal.panelOuterBorder, &m_borderBrush);
            m_renderTarget->CreateSolidColorBrush(pal.accentSubtle, &m_keyBrush);
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.06f), &m_keyBevelBrush);
            m_renderTarget->CreateSolidColorBrush(pal.accentBorder, &m_keyBorderBrush);
            m_renderTarget->CreateSolidColorBrush(pal.textPrimary, &m_keyTextBrush);
            m_renderTarget->CreateSolidColorBrush(pal.textPrimary, &m_labelBrush);
            m_renderTarget->CreateSolidColorBrush(pal.textPrimary, &m_headerBrush);
            m_renderTarget->CreateSolidColorBrush(pal.accent, &m_accentBrush);
            m_renderTarget->CreateSolidColorBrush(pal.panelInnerHighlight, &m_sheenBrush);
        }
    }
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &m_scratchBrush);
    return m_panelBrush && m_keyBrush && m_keyBorderBrush && m_keyTextBrush && m_labelBrush && m_headerBrush;
}

float ShortcutHintOverlay::measureText(const std::wstring& text, IDWriteTextFormat* format) const {
    if (!m_dwriteFactory || !format || text.empty()) return 0.0f;
    const float safeScale = (std::max)(1.0f, m_scale);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
    if (FAILED(m_dwriteFactory->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format,
            4096.0f * safeScale, 256.0f * safeScale, &textLayout))) return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    return SUCCEEDED(textLayout->GetMetrics(&metrics)) ? std::ceil(metrics.widthIncludingTrailingWhitespace) : 0.0f;
}

bool ShortcutHintOverlay::layout(ShortcutHintContext context,
                                 const std::vector<ShortcutHintItem>& items,
                                 int workWidth, float scale, int& width, int& height) {
    m_items.clear();
    if (items.empty()) return false;

    const float padX = ShortcutHintStyle::BaseHorizontalPadding * scale;
    const float padY = ShortcutHintStyle::BaseVerticalPadding * scale;
    const float keyPad = ShortcutHintStyle::BaseKeyHorizontalPadding * scale;
    const float keyMinW = ShortcutHintStyle::BaseKeyMinWidth * scale;
    const float keyHeight = ShortcutHintStyle::BaseKeyHeight * scale;
    const float labelGap = ShortcutHintStyle::BaseLabelGap * scale;
    const float rowGap = ShortcutHintStyle::BaseRowGap * scale;
    const float colGap = ShortcutHintStyle::BaseColumnGap * scale;
    const float shadowPad = ShortcutHintStyle::BaseShadowPadding * scale;

    // 保留语义标题供无障碍 UIA 播报使用，视觉上采用无冗余抬头的紧凑明亮矩阵（对标 PixPin）
    m_headerTitle = headerTitleFor(context);
    m_headerBadge.clear();

    struct ItemMetric {
        ShortcutHintItem item;
        float keyW;
        float labelW;
    };
    std::vector<ItemMetric> metrics;
    metrics.reserve(items.size());

    for (const auto& it : items) {
        const float kw = (std::max)(keyMinW,
            measureText(it.key, m_keyFormat.Get()) + keyPad * 2.0f);
        const float lw = measureText(it.label, m_labelFormat.Get());
        metrics.push_back({it, kw, lw});
    }

    // 自适应双栏 Bento 网格判定：条目数 >= 5 且显示器工作区足以舒展容纳双栏
    const bool wantTwoCols = (items.size() >= ShortcutHintStyle::TwoColumnMinItems) &&
                             (workWidth <= 0 || static_cast<float>(workWidth) >= 680.0f * scale);
    m_twoColumnLayout = wantTwoCols;
    m_columnDividerX = 0.0f;

    float bodyContentW = 0.0f;
    std::size_t rowCount = items.size();

    if (m_twoColumnLayout) {
        const std::size_t leftCount = (metrics.size() + 1) / 2;
        rowCount = leftCount;

        float col0MaxKeyW = 0.0f;
        float col0MaxLabelW = 0.0f;
        float col1MaxKeyW = 0.0f;
        float col1MaxLabelW = 0.0f;

        for (std::size_t i = 0; i < metrics.size(); ++i) {
            if (i < leftCount) {
                col0MaxKeyW = (std::max)(col0MaxKeyW, metrics[i].keyW);
                col0MaxLabelW = (std::max)(col0MaxLabelW, metrics[i].labelW);
            } else {
                col1MaxKeyW = (std::max)(col1MaxKeyW, metrics[i].keyW);
                col1MaxLabelW = (std::max)(col1MaxLabelW, metrics[i].labelW);
            }
        }

        const float col0W = col0MaxKeyW + labelGap + col0MaxLabelW;
        const float col1W = col1MaxKeyW + labelGap + col1MaxLabelW;
        bodyContentW = col0W + colGap + col1W + 4.0f * scale;

        const float cardW = padX * 2.0f + bodyContentW;
        const float bodyH = static_cast<float>(rowCount) * keyHeight +
                            static_cast<float>(rowCount > 0 ? rowCount - 1 : 0) * rowGap;
        const float cardH = padY * 2.0f + bodyH;

        m_cardRect = D2D1::RectF(shadowPad, shadowPad, shadowPad + cardW, shadowPad + cardH);
        m_headerDividerY = m_cardRect.top + padY;

        const float extraW = (std::max)(0.0f, bodyContentW - (col0W + colGap + col1W));
        const float col0StartX = m_cardRect.left + padX;
        const float col0LabelX = col0StartX + col0MaxKeyW + labelGap;
        const float effectiveColGap = colGap + extraW * 0.5f;
        const float col1StartX = col0StartX + col0W + effectiveColGap;
        const float col1LabelX = col1StartX + col1MaxKeyW + labelGap;
        m_columnDividerX = col0StartX + col0W + effectiveColGap * 0.5f;

        const float bodyStartY = m_cardRect.top + padY;
        for (std::size_t i = 0; i < metrics.size(); ++i) {
            const bool isRightCol = (i >= leftCount);
            const std::size_t rowIdx = isRightCol ? (i - leftCount) : i;
            const float itemX = isRightCol ? col1StartX : col0StartX;
            const float itemLabelX = isRightCol ? col1LabelX : col0LabelX;
            const float itemY = bodyStartY + static_cast<float>(rowIdx) * (keyHeight + rowGap);
            m_items.push_back({metrics[i].item, itemX, itemY, metrics[i].keyW, metrics[i].labelW, itemLabelX});
        }
    } else {
        float maxKeyWidth = 0.0f;
        float maxLabelWidth = 0.0f;
        for (const auto& m : metrics) {
            maxKeyWidth = (std::max)(maxKeyWidth, m.keyW);
            maxLabelWidth = (std::max)(maxLabelWidth, m.labelW);
        }

        bodyContentW = maxKeyWidth + labelGap + maxLabelWidth + 4.0f * scale;
        const float cardW = padX * 2.0f + bodyContentW;
        const float bodyH = static_cast<float>(metrics.size()) * keyHeight +
                            static_cast<float>(metrics.size() > 0 ? metrics.size() - 1 : 0) * rowGap;
        const float cardH = padY * 2.0f + bodyH;

        m_cardRect = D2D1::RectF(shadowPad, shadowPad, shadowPad + cardW, shadowPad + cardH);
        m_headerDividerY = m_cardRect.top + padY;

        float curY = m_cardRect.top + padY;
        const float labelStartX = m_cardRect.left + padX + maxKeyWidth + labelGap;
        for (const auto& m : metrics) {
            m_items.push_back({m.item, m_cardRect.left + padX, curY, m.keyW, m.labelW, labelStartX});
            curY += keyHeight + rowGap;
        }
    }

    const float totalW = (m_cardRect.right - m_cardRect.left) + shadowPad * 2.0f;
    const float totalH = (m_cardRect.bottom - m_cardRect.top) + shadowPad * 2.0f;

    width = static_cast<int>(std::ceil(totalW));
    height = static_cast<int>(std::ceil(totalH));
    return width > 0 && height > 0;
}

POINT ShortcutHintOverlay::computeOptimalPosition(int width, int height, const RECT& workArea, float scale,
                                                 const std::vector<RECT>& avoidRects) {
    const float safeScale = (scale > 0.0f) ? scale : 1.0f;
    const int margin = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * safeScale);
    const int workLeft = static_cast<int>(workArea.left);
    const int workTop = static_cast<int>(workArea.top);
    const int workRight = static_cast<int>(workArea.right);
    const int workBottom = static_cast<int>(workArea.bottom);
    const int workWidth = workRight - workLeft;
    const int workHeight = workBottom - workTop;
    const int minX = workLeft + margin;
    const int maxX = (std::max)(minX, workRight - width - margin);
    const int minY = workTop + margin;
    const int maxY = (std::max)(minY, workBottom - height - margin);

    // 默认首选左下角 (PixPin / CleanShot 桌面级交互规范)
    POINT defaultPos{
        std::clamp(workLeft + margin, minX, maxX),
        std::clamp(workBottom - height - margin, minY, maxY)
    };
    if (avoidRects.empty()) {
        return defaultPos;
    }

    const int centerX = std::clamp(workLeft + std::max(margin, (workWidth - width) / 2), minX, maxX);
    const int centerY = std::clamp(workTop + std::max(margin, (workHeight - height) / 2), minY, maxY);
    std::vector<POINT> candidates = {
        {minX, maxY},       // 1. 左下角 (默认最优)
        {minX, minY},       // 2. 左上角
        {maxX, maxY},       // 3. 右下角
        {maxX, minY},       // 4. 右上角
        {centerX, maxY},    // 5. 底部居中
        {centerX, minY},    // 6. 顶部居中
        {minX, centerY},    // 7. 左侧居中
        {maxX, centerY},    // 8. 右侧居中
    };

    auto calcIntersectionArea = [](const RECT& r1, const RECT& r2) -> long long {
        int left = std::max(r1.left, r2.left);
        int top = std::max(r1.top, r2.top);
        int right = std::min(r1.right, r2.right);
        int bottom = std::min(r1.bottom, r2.bottom);
        if (left < right && top < bottom) {
            return static_cast<long long>(right - left) * (bottom - top);
        }
        return 0;
    };

    const int pad = std::max(8, static_cast<int>(std::lround(10.0f * safeScale)));
    const RECT cornerBL{minX, maxY, minX + width, maxY + height};
    const RECT cornerTL{minX, minY, minX + width, minY + height};
    const RECT cornerBR{maxX, maxY, maxX + width, maxY + height};
    const RECT cornerTR{maxX, minY, maxX + width, minY + height};

    std::vector<RECT> localObstacles;
    std::vector<RECT> globalObstacles;
    localObstacles.reserve(avoidRects.size());
    for (const auto& avoid : avoidRects) {
        if (avoid.right <= avoid.left || avoid.bottom <= avoid.top) continue;
        RECT paddedAvoid{avoid.left - pad, avoid.top - pad, avoid.right + pad, avoid.bottom + pad};
        const long long aBL = calcIntersectionArea(cornerBL, paddedAvoid);
        const long long aTL = calcIntersectionArea(cornerTL, paddedAvoid);
        const long long aBR = calcIntersectionArea(cornerBR, paddedAvoid);
        const long long aTR = calcIntersectionArea(cornerTR, paddedAvoid);
        const long long minCorner = std::min({aBL, aTL, aBR, aTR});
        const long long maxCorner = std::max({aBL, aTL, aBR, aTR});
        if (minCorner > 0 && minCorner * 2 >= maxCorner) {
            globalObstacles.push_back(paddedAvoid);
        } else {
            localObstacles.push_back(paddedAvoid);
        }
    }

    if (localObstacles.empty()) {
        return defaultPos;
    }

    auto computeLocalCollision = [&](const RECT& candRect) -> long long {
        long long total = 0;
        for (const auto& obs : localObstacles) {
            total += calcIntersectionArea(candRect, obs);
        }
        return total;
    };

    auto computeGlobalCollision = [&](const RECT& candRect) -> long long {
        long long total = 0;
        for (const auto& obs : globalObstacles) {
            total += calcIntersectionArea(candRect, obs);
        }
        return total;
    };

    const RECT defaultRect{defaultPos.x, defaultPos.y, defaultPos.x + width, defaultPos.y + height};
    const long long defaultLocalCollision = computeLocalCollision(defaultRect);
    if (defaultLocalCollision == 0) {
        return defaultPos;
    }

    POINT bestPos = defaultPos;
    long long bestLocalCollision = defaultLocalCollision;
    long long bestGlobalCollision = computeGlobalCollision(defaultRect);
    int bestGroup = 0;
    long long bestMetric = 0;
    bool hasAlternative = false;

    auto considerCandidate = [&](const POINT& cand, int group, long long metric) {
        const RECT candRect{cand.x, cand.y, cand.x + width, cand.y + height};
        const long long localCol = computeLocalCollision(candRect);
        const long long globalCol = computeGlobalCollision(candRect);
        if (!hasAlternative ||
            std::tie(localCol, globalCol, group, metric) <
                std::tie(bestLocalCollision, bestGlobalCollision, bestGroup, bestMetric)) {
            hasAlternative = true;
            bestLocalCollision = localCol;
            bestGlobalCollision = globalCol;
            bestGroup = group;
            bestMetric = metric;
            bestPos = cand;
        }
    };

    for (size_t i = 1; i < candidates.size(); ++i) {
        considerCandidate(candidates[i], 0, static_cast<long long>(i));
    }

    if (bestLocalCollision > 0) {
        std::vector<POINT> dynamicCandidates;
        dynamicCandidates.reserve(localObstacles.size() * 8);
        for (const auto& obs : localObstacles) {
            int rx = std::clamp(static_cast<int>(obs.right + margin), minX, maxX);
            dynamicCandidates.push_back({rx, maxY});
            dynamicCandidates.push_back({rx, minY});

            int lx = std::clamp(static_cast<int>(obs.left - width - margin), minX, maxX);
            dynamicCandidates.push_back({lx, maxY});
            dynamicCandidates.push_back({lx, minY});

            int ty = std::clamp(static_cast<int>(obs.top - height - margin), minY, maxY);
            dynamicCandidates.push_back({minX, ty});
            dynamicCandidates.push_back({maxX, ty});

            int by = std::clamp(static_cast<int>(obs.bottom + margin), minY, maxY);
            dynamicCandidates.push_back({minX, by});
            dynamicCandidates.push_back({maxX, by});
        }

        for (const auto& cand : dynamicCandidates) {
            const long long manhattanDist =
                static_cast<long long>(std::abs(cand.x - defaultPos.x)) +
                static_cast<long long>(std::abs(cand.y - defaultPos.y));
            considerCandidate(cand, 1, manhattanDist);
        }
    }

    if (bestLocalCollision >= defaultLocalCollision) {
        return defaultPos;
    }

    return bestPos;
}

std::vector<RECT> ShortcutHintOverlay::effectiveAvoidRects() const {
    std::vector<RECT> combined;
    {
        std::lock_guard<std::mutex> lock(m_avoidMutex);
        combined = m_avoidRects;
    }
    if (m_hasTrackedCursor && m_trackedCursorPos.x != LONG_MIN && m_trackedCursorPos.y != LONG_MIN) {
        const int pad = std::max(44, static_cast<int>(std::lround(60.0f * m_scale)));
        combined.push_back(RECT{
            m_trackedCursorPos.x - pad,
            m_trackedCursorPos.y - pad,
            m_trackedCursorPos.x + pad,
            m_trackedCursorPos.y + pad
        });
    }
    return combined;
}

void ShortcutHintOverlay::startOrSnapToTarget(POINT targetPos, bool allowAnimate) {
    m_targetPos = targetPos;
    if (!allowAnimate || !m_hwnd || !IsWindowVisible(m_hwnd)) {
        m_animX = static_cast<float>(targetPos.x);
        m_animY = static_cast<float>(targetPos.y);
        if (m_animating.load(std::memory_order_relaxed) && m_hwnd) {
            KillTimer(m_hwnd, kAnimTimerId);
            m_animating.store(false, std::memory_order_release);
        }
        if (m_hwnd && IsWindowVisible(m_hwnd)) {
            SetWindowPos(m_hwnd, nullptr, targetPos.x, targetPos.y, 0, 0,
                         SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
        }
        return;
    }

    const float dx = static_cast<float>(targetPos.x) - m_animX;
    const float dy = static_cast<float>(targetPos.y) - m_animY;
    if (dx * dx + dy * dy <= 0.64f) {
        m_animX = static_cast<float>(targetPos.x);
        m_animY = static_cast<float>(targetPos.y);
        if (m_animating.load(std::memory_order_relaxed)) {
            KillTimer(m_hwnd, kAnimTimerId);
            m_animating.store(false, std::memory_order_release);
        }
        SetWindowPos(m_hwnd, nullptr, targetPos.x, targetPos.y, 0, 0,
                     SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
        return;
    }

    m_lastAnimTick = GetTickCount64();
    if (!m_animating.load(std::memory_order_relaxed)) {
        m_animating.store(true, std::memory_order_release);
        SetTimer(m_hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
    }
}

void ShortcutHintOverlay::advanceAnimation(float dtSeconds) {
    if (!m_animating.load(std::memory_order_relaxed)) return;
    const float clampedDt = std::clamp(dtSeconds, 0.001f, 0.08f);
    // 60fps 指数阻尼平滑位移 (Ease-Out)：16ms 步进约推进 25%，约 170ms 灵动停稳，到位即停表零 CPU 开销
    const float factor = 1.0f - std::exp(-18.0f * clampedDt);
    const float tx = static_cast<float>(m_targetPos.x);
    const float ty = static_cast<float>(m_targetPos.y);
    m_animX += (tx - m_animX) * factor;
    m_animY += (ty - m_animY) * factor;

    const float remX = tx - m_animX;
    const float remY = ty - m_animY;
    if (remX * remX + remY * remY <= 0.64f) {
        m_animX = tx;
        m_animY = ty;
        m_animating.store(false, std::memory_order_release);
        if (m_hwnd) {
            KillTimer(m_hwnd, kAnimTimerId);
        }
    }

    if (m_hwnd && IsWindowVisible(m_hwnd)) {
        SetWindowPos(m_hwnd, nullptr,
                     static_cast<int>(std::lround(m_animX)),
                     static_cast<int>(std::lround(m_animY)),
                     0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    }
}

void ShortcutHintOverlay::syncPosition() {
    if (!m_hwnd || !IsWindowVisible(m_hwnd) || m_width <= 0 || m_height <= 0) return;
    const auto combined = effectiveAvoidRects();
    POINT pos = computeOptimalPosition(m_width, m_height, m_workArea, m_scale, combined);
    const RECT curRect{m_targetPos.x, m_targetPos.y, m_targetPos.x + m_width, m_targetPos.y + m_height};

    // 迟滞防抖 (Hysteresis Anti-Jitter)
    const float safeScale = (m_scale > 0.0f) ? m_scale : 1.0f;
    const int margin = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * safeScale);
    const int minX = static_cast<int>(m_workArea.left) + margin;
    const int maxX = (std::max)(minX, static_cast<int>(m_workArea.right) - m_width - margin);
    const int minY = static_cast<int>(m_workArea.top) + margin;
    const int maxY = (std::max)(minY, static_cast<int>(m_workArea.bottom) - m_height - margin);
    const POINT defaultPos{
        std::clamp(static_cast<int>(m_workArea.left) + margin, minX, maxX),
        std::clamp(static_cast<int>(m_workArea.bottom) - m_height - margin, minY, maxY)
    };
    const bool currentlyDisplaced = (curRect.left != defaultPos.x || curRect.top != defaultPos.y);
    if (currentlyDisplaced && pos.x == defaultPos.x && pos.y == defaultPos.y && !combined.empty()) {
        const int hystPad = std::max(20, static_cast<int>(std::lround(28.0f * safeScale)));
        const RECT hystDefaultRect{
            defaultPos.x - hystPad,
            defaultPos.y - hystPad,
            defaultPos.x + m_width + hystPad,
            defaultPos.y + m_height + hystPad
        };
        const RECT cornerBL{minX, maxY, minX + m_width, maxY + m_height};
        const RECT cornerTL{minX, minY, minX + m_width, minY + m_height};
        const RECT cornerBR{maxX, maxY, maxX + m_width, maxY + m_height};
        const RECT cornerTR{maxX, minY, maxX + m_width, minY + m_height};
        auto areaOf = [](const RECT& a, const RECT& b) -> long long {
            int l = (std::max)(a.left, b.left);
            int t = (std::max)(a.top, b.top);
            int r = (std::min)(a.right, b.right);
            int bt = (std::min)(a.bottom, b.bottom);
            return (l < r && t < bt) ? static_cast<long long>(r - l) * (bt - t) : 0;
        };
        bool stillNearLocalObstacle = false;
        for (const auto& avoid : combined) {
            if (avoid.right <= avoid.left || avoid.bottom <= avoid.top) continue;
            const long long aBL = areaOf(cornerBL, avoid);
            const long long aTL = areaOf(cornerTL, avoid);
            const long long aBR = areaOf(cornerBR, avoid);
            const long long aTR = areaOf(cornerTR, avoid);
            const long long minC = std::min({aBL, aTL, aBR, aTR});
            const long long maxC = std::max({aBL, aTL, aBR, aTR});
            if (minC > 0 && minC * 2 >= maxC) continue;
            if (areaOf(hystDefaultRect, avoid) > 0) {
                stillNearLocalObstacle = true;
                break;
            }
        }
        if (stillNearLocalObstacle) {
            RECT stayRect{curRect.left, curRect.top, curRect.left + m_width, curRect.top + m_height};
            bool stayClear = true;
            for (const auto& avoid : combined) {
                if (avoid.right <= avoid.left || avoid.bottom <= avoid.top) continue;
                const long long aBL = areaOf(cornerBL, avoid);
                const long long aTL = areaOf(cornerTL, avoid);
                const long long aBR = areaOf(cornerBR, avoid);
                const long long aTR = areaOf(cornerTR, avoid);
                const long long minC = std::min({aBL, aTL, aBR, aTR});
                const long long maxC = std::max({aBL, aTL, aBR, aTR});
                if (minC > 0 && minC * 2 >= maxC) continue;
                if (areaOf(stayRect, avoid) > 0) {
                    stayClear = false;
                    break;
                }
            }
            if (stayClear) {
                return;
            }
        }
    }

    if (curRect.left != pos.x || curRect.top != pos.y) {
        startOrSnapToTarget(pos, true);
    }
}

void ShortcutHintOverlay::updateAvoidance(const std::vector<RECT>& avoidRects) {
    if (!m_hwnd || !IsWindowVisible(m_hwnd)) return;
    if (GetWindowThreadProcessId(m_hwnd, nullptr) != GetCurrentThreadId()) {
        if (tools3000::core::MainThreadDispatcher::instance().isInitialized()) {
            tools3000::core::MainThreadDispatcher::instance().post([avoidRects]() {
                ShortcutHintOverlay::instance().updateAvoidance(avoidRects);
            });
        }
        return;
    }
    const bool ownerActiveNow = checkOwnerActive(m_context);
    if (!m_observedOwnerActive && ownerActiveNow) {
        m_observedOwnerActive = true;
    } else if ((m_ownerActiveChecker != nullptr || m_observedOwnerActive) && !ownerActiveNow) {
        hide();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_avoidMutex);
        m_avoidRects = avoidRects;
    }
    m_hasTrackedCursor = false;
    GetCursorPos(&m_initialCursorPos);
    syncPosition();
}

void ShortcutHintOverlay::show(ShortcutHintContext context, POINT anchor,
                              const std::vector<RECT>& avoidRects) {
    if ((m_hwnd && IsWindow(m_hwnd) && GetWindowThreadProcessId(m_hwnd, nullptr) != GetCurrentThreadId()) ||
        (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
         !tools3000::core::MainThreadDispatcher::instance().isOwnerThread())) {
        tools3000::core::MainThreadDispatcher::instance().post([context, anchor, avoidRects]() {
            ShortcutHintOverlay::instance().show(context, anchor, avoidRects);
        });
        return;
    }
    if (!tools3000::core::ConfigManager::instance().get<bool>(
            "/capture/showShortcutHints", true)) {
        hide();
        return;
    }
    auto items = itemsFor(context);
    if (items.empty()) {
        hide();
        return;
    }
    const bool hasCtx = m_hasContext.load(std::memory_order_acquire);
    const bool ownerActiveNow = checkOwnerActive(context);
    if ((m_ownerActiveChecker != nullptr || (hasCtx && m_context == context && m_observedOwnerActive)) &&
        !ownerActiveNow) {
        hide();
        return;
    }
    const bool wasAlreadyVisible = isVisible();
    const bool isContextSwitchOnly =
        (anchor.x == LONG_MIN && anchor.y == LONG_MIN && avoidRects.empty() && hasCtx && wasAlreadyVisible);
    if (anchor.x == LONG_MIN || anchor.y == LONG_MIN) {
        if (!GetCursorPos(&anchor)) anchor = {0, 0};
    }
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const auto monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return;
    m_workArea = monitorInfo.rcWork;

    const UINT dpiX = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = ShortcutHintStyle::scaleForDpi(dpiX);
    if (!isContextSwitchOnly) {
        {
            std::lock_guard<std::mutex> lock(m_avoidMutex);
            m_avoidRects = avoidRects;
        }
        m_hasTrackedCursor = false;
        if (!GetCursorPos(&m_initialCursorPos)) {
            m_initialCursorPos = {LONG_MIN, LONG_MIN};
        }
        m_trackedCursorPos = m_initialCursorPos;
    }
    if (hasCtx && m_context == context && wasAlreadyVisible &&
        std::abs(scale - m_scale) <= 0.01f) {
        if (ownerActiveNow) m_observedOwnerActive = true;
        if (!isContextSwitchOnly) {
            updateAvoidance(avoidRects);
        } else {
            syncPosition();
        }
        return;
    }
    if (m_hwnd && !IsWindow(m_hwnd)) {
        m_hwnd = nullptr;
    }
    if (!m_hwnd && !createWindow()) return;
    if (!m_renderTarget || std::abs(scale - m_scale) > 0.01f) {
        discardResources();
        m_scale = scale;
        if (!createResources(scale)) {
            hide();
            return;
        }
    }
    if (!layout(context, items, m_workArea.right - m_workArea.left, scale, m_width, m_height)) {
        hide();
        return;
    }

    m_context = context;
    m_hasContext.store(true, std::memory_order_release);
    m_observedOwnerActive = ownerActiveNow;

    POINT pos = computeOptimalPosition(m_width, m_height, m_workArea, scale, effectiveAvoidRects());
    if (!wasAlreadyVisible) {
        m_targetPos = pos;
        m_animX = static_cast<float>(pos.x);
        m_animY = static_cast<float>(pos.y);
        m_animating.store(false, std::memory_order_release);
        SetWindowPos(m_hwnd, HWND_TOPMOST, pos.x, pos.y, m_width, m_height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        render();
    } else {
        const int curX = static_cast<int>(std::lround(m_animX));
        const int curY = static_cast<int>(std::lround(m_animY));
        SetWindowPos(m_hwnd, HWND_TOPMOST, curX, curY, m_width, m_height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        render();
        startOrSnapToTarget(pos, true);
    }

    const bool isZh = tools3000::core::isUiLanguageChinese();
    std::wstring accessibleName = (isZh ? L"快捷键提示：" : L"Shortcut Hints: ") + m_headerTitle + (isZh ? L"；" : L"; ");
    for (const auto& item : items) {
        if (accessibleName.size() > 1024) break;
        accessibleName += item.key;
        accessibleName += L" ";
        accessibleName += item.label;
        accessibleName += isZh ? L"；" : L"; ";
    }
    tools3000::core::accessibility::announceOverlay(m_hwnd, accessibleName);
    SetTimer(m_hwnd, kWatchdogTimerId, 45, nullptr);
}

void ShortcutHintOverlay::render(int retryDepth) {
    if (!m_hwnd || !m_renderTarget || m_width <= 0 || m_height <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = m_width;
    bitmapInfo.bmiHeader.biHeight = -m_height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    const auto previous = SelectObject(memory, bitmap);
    RECT bounds{0, 0, m_width, m_height};
    m_renderTarget->BindDC(memory, &bounds);
    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float cornerR = ShortcutHintStyle::BaseCornerRadius * m_scale;
    const float padY = ShortcutHintStyle::BaseVerticalPadding * m_scale;
    const bool isHC = highContrastEnabled();

    // 1. 双层物理分层高斯微阴影 (Dual-Layer Ambient & Key Shadows)
    if (!isHC) {
        struct ShadowStep {
            float dx;
            float dy;
            float expand;
            float alpha;
        };

        static const ShadowStep ambientSteps[] = {
            { 0.0f, 2.5f, 3.5f, 0.055f },
            { 0.0f, 4.0f, 6.5f, 0.036f },
            { 0.0f, 5.5f, 9.5f, 0.022f },
        };
        static const ShadowStep keySteps[] = {
            { 0.0f, 1.0f, 0.6f, 0.085f },
            { 0.0f, 1.8f, 1.6f, 0.055f },
            { 0.0f, 2.5f, 2.6f, 0.032f },
        };

        if (!m_scratchBrush) {
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &m_scratchBrush);
        }
        if (m_scratchBrush) {
            for (const auto& step : ambientSteps) {
                m_scratchBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, step.alpha));
                const float exp = step.expand * m_scale;
                const auto sRect = D2D1::RectF(
                    m_cardRect.left - exp + step.dx * m_scale,
                    m_cardRect.top - exp + step.dy * m_scale,
                    m_cardRect.right + exp + step.dx * m_scale,
                    m_cardRect.bottom + exp + step.dy * m_scale);
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sRect, cornerR + exp, cornerR + exp), m_scratchBrush.Get());
            }
            for (const auto& step : keySteps) {
                m_scratchBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, step.alpha));
                const float exp = step.expand * m_scale;
                const auto sRect = D2D1::RectF(
                    m_cardRect.left - exp + step.dx * m_scale,
                    m_cardRect.top - exp + step.dy * m_scale,
                    m_cardRect.right + exp + step.dx * m_scale,
                    m_cardRect.bottom + exp + step.dy * m_scale);
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sRect, cornerR + exp, cornerR + exp), m_scratchBrush.Get());
            }
        }
    }

    // 2. 高通透曜石纯黑底舱 + 极细微晶冷白边界 (对标 PixPin 图 1 明亮清晰效果)
    const auto panel = D2D1::RoundedRect(m_cardRect, cornerR, cornerR);
    if (m_panelBrush) {
        m_renderTarget->FillRoundedRectangle(panel, m_panelBrush.Get());
    }
    if (m_borderBrush) {
        m_renderTarget->DrawRoundedRectangle(panel, m_borderBrush.Get(), isHC ? 1.5f : (1.0f * m_scale));
    }

    // 双栏模式下极柔和中轴视觉分隔丝
    if (m_twoColumnLayout && m_columnDividerX > m_cardRect.left) {
        ID2D1SolidColorBrush* divBrush = nullptr;
        if (isHC) {
            divBrush = m_borderBrush.Get();
        } else if (m_scratchBrush) {
            const auto pal = FloatingGlassBar::resolveThemePalette();
            m_scratchBrush->SetColor(pal.divider);
            divBrush = m_scratchBrush.Get();
        }
        if (divBrush) {
            const float colDivTop = m_cardRect.top + padY + 2.0f * m_scale;
            const float colDivBottom = m_cardRect.bottom - padY - 2.0f * m_scale;
            if (colDivBottom > colDivTop) {
                m_renderTarget->DrawLine(
                    D2D1::Point2F(m_columnDividerX, colDivTop),
                    D2D1::Point2F(m_columnDividerX, colDivBottom),
                    divBrush, 1.0f * m_scale);
            }
        }
    }

    // 3. 渲染 100% 纯白高亮实心圆角键帽（深曜石黑粗体键名）与明亮纯白说明文字
    const float keyHeight = ShortcutHintStyle::BaseKeyHeight * m_scale;
    const float keyCornerR = ShortcutHintStyle::BaseKeyCornerRadius * m_scale;
    const float labelGap = ShortcutHintStyle::BaseLabelGap * m_scale;

    for (const auto& positioned : m_items) {
        const auto keyFaceRect = D2D1::RectF(
            positioned.x,
            positioned.y,
            positioned.x + positioned.keyWidth,
            positioned.y + keyHeight);

        const auto keyRR = D2D1::RoundedRect(keyFaceRect, keyCornerR, keyCornerR);
        m_renderTarget->FillRoundedRectangle(keyRR, m_keyBrush.Get());
        if (m_keyBorderBrush) {
            m_renderTarget->DrawRoundedRectangle(keyRR, m_keyBorderBrush.Get(), 1.0f * m_scale);
        }
        m_renderTarget->DrawTextW(
            positioned.item.key.c_str(), static_cast<UINT32>(positioned.item.key.size()),
            m_keyFormat.Get(), keyFaceRect, m_keyTextBrush.Get());

        const float labelStartX = positioned.labelX > 0.0f
            ? positioned.labelX
            : (positioned.x + positioned.keyWidth + labelGap);
        const auto labelRect = D2D1::RectF(
            labelStartX,
            positioned.y,
            labelStartX + positioned.labelWidth + 6.0f * m_scale,
            positioned.y + keyHeight);
        m_renderTarget->DrawTextW(
            positioned.item.label.c_str(), static_cast<UINT32>(positioned.item.label.size()),
            m_labelFormat.Get(), labelRect, m_labelBrush.Get());
    }

    const HRESULT drawResult = m_renderTarget->EndDraw();
    if (SUCCEEDED(drawResult)) {
        POINT source{0, 0};
        SIZE size{m_width, m_height};
        POINT destination{
            static_cast<LONG>(std::lround(m_animX)),
            static_cast<LONG>(std::lround(m_animY))
        };
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(m_hwnd, screen, &destination, &size, memory,
                            &source, 0, &blend, ULW_ALPHA);
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    if (drawResult == D2DERR_RECREATE_TARGET) {
        discardResources();
        if (retryDepth < 1 && createResources(m_scale)) {
            render(retryDepth + 1);
        }
    }
}

void ShortcutHintOverlay::discardResources() {
    m_scratchBrush.Reset();
    m_sheenBrush.Reset();
    m_accentBrush.Reset();
    m_headerBrush.Reset();
    m_labelBrush.Reset();
    m_keyTextBrush.Reset();
    m_keyBorderBrush.Reset();
    m_keyBevelBrush.Reset();
    m_keyBrush.Reset();
    m_borderBrush.Reset();
    m_panelBrush.Reset();
    m_badgeFormat.Reset();
    m_headerFormat.Reset();
    m_labelFormat.Reset();
    m_keyFormat.Reset();
    m_renderTarget.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
}

void ShortcutHintOverlay::reloadTheme() {
    if (!isVisible()) return;
    discardResources();
    if (createResources(m_scale)) {
        render();
    }
}

void ShortcutHintOverlay::hide() {
    m_hasContext.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_avoidMutex);
        m_avoidRects.clear();
    }
    HWND currentHwnd = m_hwnd;
    if (currentHwnd && IsWindow(currentHwnd) &&
        GetWindowThreadProcessId(currentHwnd, nullptr) != GetCurrentThreadId()) {
        ShowWindowAsync(currentHwnd, SW_HIDE);
        PostMessageW(currentHwnd, WM_APP + 1, 0, 0);
        if (tools3000::core::MainThreadDispatcher::instance().isInitialized()) {
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                if (!ShortcutHintOverlay::instance().isVisible()) {
                    ShortcutHintOverlay::instance().hide();
                }
            });
        }
        return;
    }
    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([]() {
            ShortcutHintOverlay::instance().hide();
        });
        return;
    }
    m_observedOwnerActive = false;
    m_hasTrackedCursor = false;
    m_animating.store(false, std::memory_order_release);
    if (m_hwnd && !IsWindow(m_hwnd)) {
        m_hwnd = nullptr;
    }
    m_items.clear();
    discardResources();
    if (m_hwnd) {
        HWND hwndToDestroy = m_hwnd;
        m_hwnd = nullptr;
        KillTimer(hwndToDestroy, kWatchdogTimerId);
        KillTimer(hwndToDestroy, kAnimTimerId);
        tools3000::core::accessibility::hideOverlay(hwndToDestroy);
        ShowWindow(hwndToDestroy, SW_HIDE);
        DestroyWindow(hwndToDestroy);
    }
    if (m_module) {
        UnregisterClassW(WindowClass, m_module);
        m_module = nullptr;
    }
}

std::vector<tools3000::core::accessibility::OverlayUiaAction>
ShortcutHintOverlay::accessibilityActions() const {
    std::vector<tools3000::core::accessibility::OverlayUiaAction> actions;
    if (!m_hwnd || m_items.empty()) return actions;
    const RECT window = getBounds();
    const float labelGap = ShortcutHintStyle::BaseLabelGap * m_scale;
    const float itemHeight = ShortcutHintStyle::BaseKeyHeight * m_scale;
    actions.reserve(m_items.size());
    for (std::size_t index = 0; index < m_items.size(); ++index) {
        const auto& item = m_items[index];
        const LONG left = window.left + static_cast<LONG>(std::lround(item.x));
        const LONG top = window.top + static_cast<LONG>(std::lround(item.y));
        const LONG right = left + static_cast<LONG>(std::lround(item.keyWidth + labelGap + item.labelWidth));
        const LONG bottom = top + static_cast<LONG>(std::lround(itemHeight));
        actions.push_back({
            L"Tools3000.ShortcutHint." + std::to_wstring(index),
            item.item.key + L"：" + item.item.label,
            L"Contextual keyboard shortcut", item.item.key,
            {left, top, right, bottom}, true, false,
            tools3000::core::accessibility::OverlayUiaActionRole::Text, 0, 0,
        });
    }
    return actions;
}

LRESULT CALLBACK ShortcutHintOverlay::wndProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* overlay = reinterpret_cast<ShortcutHintOverlay*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        overlay = static_cast<ShortcutHintOverlay*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    }
    if (message == WM_APP + 1) {
        if (overlay && !overlay->m_hasContext.load(std::memory_order_acquire)) {
            overlay->hide();
        }
        return 0;
    }
    if (message == WM_TIMER && wParam == kAnimTimerId) {
        if (!overlay || !overlay->m_hasContext.load(std::memory_order_acquire) ||
            !overlay->m_animating.load(std::memory_order_acquire)) {
            KillTimer(hwnd, kAnimTimerId);
            if (overlay) overlay->m_animating.store(false, std::memory_order_release);
            return 0;
        }
        const ULONGLONG now = GetTickCount64();
        const float dtSec = (overlay->m_lastAnimTick > 0 && now >= overlay->m_lastAnimTick)
            ? static_cast<float>(now - overlay->m_lastAnimTick) * 0.001f
            : 0.016f;
        overlay->m_lastAnimTick = now;
        overlay->advanceAnimation(dtSec);
        return 0;
    }
    if (message == WM_TIMER && wParam == kWatchdogTimerId) {
        if (!overlay || !overlay->m_hasContext.load(std::memory_order_acquire)) {
            KillTimer(hwnd, kWatchdogTimerId);
            KillTimer(hwnd, kAnimTimerId);
            if (overlay) overlay->hide();
            return 0;
        }
        const bool ownerActiveNow = overlay->checkOwnerActive(overlay->m_context);
        if (!overlay->m_observedOwnerActive && ownerActiveNow) {
            overlay->m_observedOwnerActive = true;
        } else if ((overlay->m_ownerActiveChecker != nullptr || overlay->m_observedOwnerActive) && !ownerActiveNow) {
            overlay->hide();
            return 0;
        }
        POINT curPt{};
        if (GetCursorPos(&curPt)) {
            const bool movedSinceInit = (overlay->m_initialCursorPos.x == LONG_MIN) ||
                (std::abs(curPt.x - overlay->m_initialCursorPos.x) > 4) ||
                (std::abs(curPt.y - overlay->m_initialCursorPos.y) > 4);
            if (movedSinceInit || overlay->m_hasTrackedCursor) {
                if (!overlay->m_hasTrackedCursor ||
                    std::abs(curPt.x - overlay->m_trackedCursorPos.x) > 2 ||
                    std::abs(curPt.y - overlay->m_trackedCursorPos.y) > 2) {
                    overlay->m_hasTrackedCursor = true;
                    overlay->m_trackedCursorPos = curPt;
                    overlay->syncPosition();
                }
            }
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        if (overlay && overlay->m_hwnd == hwnd) {
            overlay->hide();
        } else {
            KillTimer(hwnd, kWatchdogTimerId);
            KillTimer(hwnd, kAnimTimerId);
            tools3000::core::accessibility::hideOverlay(hwnd);
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
        }
        return 0;
    }
    if (message == WM_GETOBJECT) {
        return tools3000::core::accessibility::respondToOverlayUiaGetObject(
            hwnd, wParam, lParam,
            {L"Tools3000.ShortcutHints", L"Contextual Tools3000 keyboard shortcut guide",
             tools3000::core::accessibility::OverlayUiaRole::Text, true},
            overlay ? overlay->accessibilityActions()
                    : std::vector<tools3000::core::accessibility::OverlayUiaAction>{});
    }
    if (message == WM_NCDESTROY) {
        if (overlay && overlay->m_hwnd == hwnd) {
            overlay->m_hwnd = nullptr;
            overlay->m_hasContext.store(false, std::memory_order_release);
            overlay->m_observedOwnerActive = false;
            overlay->m_animating.store(false, std::memory_order_release);
        }
        tools3000::core::accessibility::disconnectOverlayUiaProvider(hwnd);
    }
    if (overlay && (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED ||
                    message == WM_SYSCOLORCHANGE)) {
        overlay->discardResources();
        if (overlay->createResources(overlay->m_scale)) overlay->render();
        return 0;
    }
    if (overlay && overlay->m_hasContext.load(std::memory_order_acquire) &&
        (message == WM_DPICHANGED || message == WM_DISPLAYCHANGE)) {
        if ((overlay->m_ownerActiveChecker != nullptr || overlay->m_observedOwnerActive) &&
            !overlay->checkOwnerActive(overlay->m_context)) {
            overlay->hide();
            return 0;
        }
        POINT anchor{
            (overlay->m_workArea.left + overlay->m_workArea.right) / 2,
            (overlay->m_workArea.top + overlay->m_workArea.bottom) / 2};
        if (message == WM_DPICHANGED && lParam) {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            anchor = {(suggested->left + suggested->right) / 2,
                      (suggested->top + suggested->bottom) / 2};
        }
        const auto context = overlay->m_context;
        overlay->m_hasContext.store(false, std::memory_order_release);
        overlay->show(context, anchor);
        return 0;
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace tools3000::capture

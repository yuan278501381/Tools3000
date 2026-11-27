#include "OcrResultWindow.h"
#include "core/i18n/UiI18n.h"

#include "OcrEngine.h"
#include "OcrResultStyle.h"
#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <iostream>
#include <utility>

#include <windowsx.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "msimg32.lib")

namespace tools3000::ocr {
namespace {

constexpr wchar_t WindowClass[] = L"Tools3000_OcrResult";
constexpr UINT FadeTimerId = 1;
constexpr UINT CopyStateTimerId = 2;
constexpr UINT LoadingAnimTimerId = 3;
constexpr UINT FadeIntervalMs = 16;
constexpr UINT FadeDurationMs = 150;
constexpr UINT LoadingAnimIntervalMs = 33;
constexpr UINT CopyStateDurationMs = 2000;
constexpr float BaseShadowInset = 6.0f;
constexpr float BaseIndexColWidth = 30.0f;
constexpr float BaseRowPadX = 10.0f;
constexpr float BaseRowPadY = 5.0f;
constexpr float BaseRowGap = 4.0f;
constexpr float BaseCopyIconAreaWidth = 26.0f;
constexpr float BaseScrollbarReserveWidth = 12.0f;
constexpr size_t MaxEditUndoDepth = 40;

struct OcrThemePalette {
    bool isDark = true;
    D2D1_COLOR_F bg{};
    D2D1_COLOR_F innerHighlight{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F text{};
    D2D1_COLOR_F muted{};
    D2D1_COLOR_F btn{};
    D2D1_COLOR_F btnHover{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F accentSubtle{};
    D2D1_COLOR_F selection{};
};

OcrThemePalette resolveOcrThemePalette() {
    auto& cfg = tools3000::core::ConfigManager::instance();
    const std::string themePref = cfg.get<std::string>("/general/theme", "system");
    bool isDark = true;
    if (themePref == "light") {
        isDark = false;
    } else if (themePref == "dark") {
        isDark = true;
    } else {
        isDark = tools3000::core::WinUtils::isSystemDarkMode();
    }

    const std::string accentStr = cfg.get<std::string>("/general/accentColor", "blue");
    const tools3000::core::AccentColorRGB rgb =
        (!accentStr.empty() && accentStr.front() == '#')
            ? tools3000::core::parseHexColor(accentStr)
            : tools3000::core::getAccentColorRGB(accentStr == "emerald" ? "mint" : accentStr);
    const float ar = rgb.r;
    const float ag = rgb.g;
    const float ab = rgb.b;

    OcrThemePalette pal{};
    pal.isDark = isDark;
    pal.accent = D2D1::ColorF(ar, ag, ab, 1.0f);
    if (isDark) {
        pal.bg = D2D1::ColorF(15.0f / 255.0f, 18.0f / 255.0f, 25.0f / 255.0f, 0.96f);
        pal.innerHighlight = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f);
        pal.border = D2D1::ColorF(ar, ag, ab, 0.52f);
        pal.text = D2D1::ColorF(248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 1.0f);
        pal.muted = D2D1::ColorF(148.0f / 255.0f, 163.0f / 255.0f, 184.0f / 255.0f, 0.92f);
        pal.btn = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
        pal.btnHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f);
        pal.accentSubtle = D2D1::ColorF(ar, ag, ab, 0.22f);
        pal.selection = D2D1::ColorF(ar, ag, ab, 0.38f);
    } else {
        pal.bg = D2D1::ColorF(250.0f / 255.0f, 251.0f / 255.0f, 253.0f / 255.0f, 0.97f);
        pal.innerHighlight = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f);
        pal.border = D2D1::ColorF(ar * 0.65f + 0.06f, ag * 0.65f + 0.09f, ab * 0.65f + 0.16f, 0.28f);
        pal.text = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 1.0f);
        pal.muted = D2D1::ColorF(100.0f / 255.0f, 116.0f / 255.0f, 139.0f / 255.0f, 0.95f);
        pal.btn = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.055f);
        pal.btnHover = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.115f);
        pal.accentSubtle = D2D1::ColorF(ar, ag, ab, 0.14f);
        pal.selection = D2D1::ColorF(ar, ag, ab, 0.24f);
    }
    return pal;
}

int classifyWordChar(wchar_t ch) noexcept {
    if (std::iswspace(ch)) return 0;
    if ((ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF) ||
        (ch >= 0x3040 && ch <= 0x30FF) || (ch >= 0xAC00 && ch <= 0xD7AF)) {
        return 2; // CJK 字符流
    }
    if (std::iswalnum(ch) || ch == L'_' || ch == L'-' || ch == L'.' || ch == L'/') {
        return 1; // 西文标识符/数字/路径词块
    }
    return 3; // 符号标点
}

std::wstring readClipboardWideText(HWND owner) {
    if (!OpenClipboard(owner)) return {};
    std::wstring out;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        if (const auto* ptr = static_cast<const wchar_t*>(GlobalLock(hData))) {
            out.assign(ptr);
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    // 归一化 CRLF -> LF
    std::wstring clean;
    clean.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == L'\r') {
            if (i + 1 < out.size() && out[i + 1] == L'\n') continue;
            clean.push_back(L'\n');
        } else {
            clean.push_back(out[i]);
        }
    }
    return clean;
}

bool contains(const D2D1_RECT_F& rect, POINT point) noexcept {
    return point.x >= rect.left && point.x <= rect.right &&
           point.y >= rect.top && point.y <= rect.bottom;
}

bool containsExpanded(const D2D1_RECT_F& rect, POINT point, float pad) noexcept {
    return rect.right > rect.left && rect.bottom > rect.top &&
           point.x >= rect.left - pad && point.x <= rect.right + pad &&
           point.y >= rect.top - pad && point.y <= rect.bottom + pad;
}

const wchar_t* footerHintText(bool chinese, bool compact) noexcept {
    if (chinese) {
        return compact
            ? L"双击选词复制 · 可直接编辑 · Esc"
            : L"拖选/双击复制词 · 点击可微调 · Esc";
    }
    return compact
        ? L"Dbl-click copy · Click edit · Esc"
        : L"Drag/Dbl-click copy · Click to edit · Esc";
}

}  // namespace

bool OcrResultWindow::initialize() {
    if (m_hwnd && IsWindow(m_hwnd)) return true;

    HMODULE moduleInstance = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&OcrResultWindow::wndProc),
            &moduleInstance) || !moduleInstance) {
        moduleInstance = GetModuleHandleW(nullptr);
    }
    m_instance = moduleInstance;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = wndProc;
    windowClass.hInstance = m_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = WindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LOG_ERROR("OCR result window class registration failed, error={}", GetLastError());
        return false;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        WindowClass, L"OCR Result", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, m_instance, this);
    if (!m_hwnd) {
        LOG_ERROR("OCR result window creation failed, error={}", GetLastError());
        return false;
    }

    // 零隐私防截策略 (Zero WDA_EXCLUDEFROMCAPTURE)：全库所有界面均支持被截图正常抓取
    SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
    if (!createResources()) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        return false;
    }
    return true;
}

void OcrResultWindow::cleanup() {
    if (m_hwnd) {
        KillTimer(m_hwnd, FadeTimerId);
        KillTimer(m_hwnd, CopyStateTimerId);
        if (GetCapture() == m_hwnd) ReleaseCapture();
        ShowWindow(m_hwnd, SW_HIDE);
        SetFocus(nullptr);
    }
    discardDeviceResources();
    if (m_hwnd) {
        const HWND window = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(window);
    }
    if (m_instance) {
        UnregisterClassW(WindowClass, m_instance);
        m_instance = nullptr;
    }
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
    m_rawUnmergedText.clear();
    m_rawUnmergedWide.clear();
    m_text.clear();
    m_wideText.clear();
    m_editUndoStack.clear();
    clearSelection();
}

bool OcrResultWindow::createResources() {
    if (!m_d2dFactory && FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) {
        return false;
    }
    if (!m_dwriteFactory && FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &m_dwriteFactory))) {
        return false;
    }
    if (!m_renderTarget) {
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE,
            D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(
                &properties, &m_renderTarget))) {
            return false;
        }
    }
    return createTextResources(m_scale);
}

bool OcrResultWindow::createTextResources(float scale) {
    if (!m_dwriteFactory) return false;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> buttonFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> hintFormat;
    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
    const wchar_t* localeName = tools3000::core::trWCstr(L"zh-CN", L"en-US");
    const auto createFormat = [&](float size, DWRITE_FONT_WEIGHT weight,
                                  IDWriteTextFormat** output) {
        return m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size * scale,
            localeName,
            output);
    };
    if (FAILED(createFormat(OcrResultStyle::BaseBodyFont,
                            DWRITE_FONT_WEIGHT_MEDIUM, &textFormat)) ||
        FAILED(createFormat(OcrResultStyle::BaseTitleFont,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD, &titleFormat)) ||
        FAILED(createFormat(OcrResultStyle::BaseButtonFont,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD, &buttonFormat)) ||
        FAILED(createFormat(OcrResultStyle::BaseHintFont,
                            DWRITE_FONT_WEIGHT_MEDIUM, &hintFormat))) {
        return false;
    }
    textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    textFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                               OcrResultStyle::BaseBodyFont * scale * 1.50f,
                               OcrResultStyle::BaseBodyFont * scale * 1.20f);

    // 标题栏严格单行垂直居中，配合省略号裁剪防止窄窗口下复制反馈长文案折行腰斩
    titleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Microsoft::WRL::ComPtr<IDWriteInlineObject> titleEllipsisSign;
    if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(titleFormat.Get(), &titleEllipsisSign))) {
        DWRITE_TRIMMING titleTrimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        titleFormat->SetTrimming(&titleTrimming, titleEllipsisSign.Get());
    }

    buttonFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    buttonFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    buttonFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 底部提示栏严格禁止多行折行，配合省略裁剪双保险确保 100% 单行显示
    hintFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    hintFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsisSign;
    if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(hintFormat.Get(), &ellipsisSign))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        hintFormat->SetTrimming(&trimming, ellipsisSign.Get());
    }

    m_textLayout.Reset();
    m_textFormat = std::move(textFormat);
    m_titleFormat = std::move(titleFormat);
    m_buttonFormat = std::move(buttonFormat);
    m_hintFormat = std::move(hintFormat);
    return true;
}

void OcrResultWindow::resetColorBrushes() {
    m_brushBg.Reset();
    m_brushInnerHighlight.Reset();
    m_brushText.Reset();
    m_brushMuted.Reset();
    m_brushBorder.Reset();
    m_brushBtn.Reset();
    m_brushBtnHover.Reset();
    m_brushAccent.Reset();
    m_brushAccentSubtle.Reset();
    m_brushSuccess.Reset();
    m_brushError.Reset();
    m_brushSelection.Reset();
}

void OcrResultWindow::reloadTheme() {
    resetColorBrushes();
    if (m_hwnd && IsWindowVisible(m_hwnd)) {
        render();
    }
}

void OcrResultWindow::discardDeviceResources() {
    resetColorBrushes();
    for (auto& item : m_lines) item.layout.Reset();
    m_textLayout.Reset();
    m_textFormat.Reset();
    m_titleFormat.Reset();
    m_buttonFormat.Reset();
    m_hintFormat.Reset();
    m_renderTarget.Reset();
    releaseSurface();
}

void OcrResultWindow::releaseSurface() {
    if (m_renderTarget) {
        RECT emptyRect{0, 0, 0, 0};
        m_renderTarget->BindDC(nullptr, &emptyRect);
    }
    if (m_memoryDc && m_previousBitmap) {
        SelectObject(m_memoryDc, m_previousBitmap);
    }
    m_previousBitmap = nullptr;
    if (m_bitmap) DeleteObject(m_bitmap);
    if (m_memoryDc) DeleteDC(m_memoryDc);
    if (m_screenDc) ReleaseDC(nullptr, m_screenDc);
    m_bitmap = nullptr;
    m_memoryDc = nullptr;
    m_screenDc = nullptr;
    m_pixels = nullptr;
    m_surfaceWidth = 0;
    m_surfaceHeight = 0;
}

bool OcrResultWindow::ensureSurface(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (m_bitmap && m_surfaceWidth == width && m_surfaceHeight == height) return true;

    releaseSurface();
    m_screenDc = GetDC(nullptr);
    m_memoryDc = m_screenDc ? CreateCompatibleDC(m_screenDc) : nullptr;
    if (!m_screenDc || !m_memoryDc) {
        releaseSurface();
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    m_bitmap = CreateDIBSection(m_screenDc, &bitmapInfo, DIB_RGB_COLORS,
                                &m_pixels, nullptr, 0);
    if (!m_bitmap || !m_pixels) {
        releaseSurface();
        return false;
    }
    m_previousBitmap = SelectObject(m_memoryDc, m_bitmap);
    if (!m_previousBitmap || m_previousBitmap == HGDI_ERROR) {
        m_previousBitmap = nullptr;
        releaseSurface();
        return false;
    }
    m_surfaceWidth = width;
    m_surfaceHeight = height;
    return true;
}

void OcrResultWindow::updatePlacement() {
    if (!m_hwnd || m_updatingPlacement) return;
    m_updatingPlacement = true;

    HMONITOR monitor = nullptr;
    if (m_hasAnchorRect && m_anchorRect.right > m_anchorRect.left && m_anchorRect.bottom > m_anchorRect.top) {
        monitor = MonitorFromRect(&m_anchorRect, MONITOR_DEFAULTTONEAREST);
    }
    if (!monitor) {
        monitor = tools3000::core::dpi::activeMonitor();
    }
    const RECT workArea = tools3000::core::dpi::workArea(monitor);
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = tools3000::core::dpi::scaleForDpi(dpi);
    SIZE size = OcrResultStyle::windowSizeForDpi(dpi);
    const int margin = tools3000::core::dpi::scaleMetric(
        OcrResultStyle::BaseScreenMargin, scale);
    const int availableWidth = (std::max)(
        1, static_cast<int>(workArea.right - workArea.left) - margin * 2);
    const int availableHeight = (std::max)(
        1, static_cast<int>(workArea.bottom - workArea.top) - margin * 2);
    size.cx = (std::min)(size.cx, static_cast<LONG>(availableWidth));
    size.cy = (std::min)(size.cy, static_cast<LONG>(availableHeight));

    int x = workArea.left + (workArea.right - workArea.left - size.cx) / 2;
    int y = workArea.top + (workArea.bottom - workArea.top - size.cy) / 2;

    if (m_hasAnchorRect && m_anchorRect.right > m_anchorRect.left && m_anchorRect.bottom > m_anchorRect.top) {
        const int gap = tools3000::core::dpi::scaleMetric(14, scale);
        const int rightSpace = workArea.right - margin - m_anchorRect.right;
        const int leftSpace = m_anchorRect.left - (workArea.left + margin);
        if (rightSpace >= size.cx + gap) {
            x = m_anchorRect.right + gap;
            y = std::clamp(static_cast<int>(m_anchorRect.top),
                           static_cast<int>(workArea.top + margin),
                           static_cast<int>((std::max)(workArea.top + margin, workArea.bottom - margin - size.cy)));
        } else if (leftSpace >= size.cx + gap) {
            x = m_anchorRect.left - gap - size.cx;
            y = std::clamp(static_cast<int>(m_anchorRect.top),
                           static_cast<int>(workArea.top + margin),
                           static_cast<int>((std::max)(workArea.top + margin, workArea.bottom - margin - size.cy)));
        } else {
            // 原图较宽时停靠在右侧工作区边缘，最大限度露出左侧原图区域便于双栏对照
            x = (std::max)(static_cast<int>(workArea.left + margin),
                           static_cast<int>(workArea.right - margin - size.cx));
            y = std::clamp(static_cast<int>(m_anchorRect.top + gap),
                           static_cast<int>(workArea.top + margin),
                           static_cast<int>((std::max)(workArea.top + margin, workArea.bottom - margin - size.cy)));
        }
    }

    const bool dpiChanged = std::abs(m_scale - scale) > 0.001f;
    m_scale = scale;
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, size.cx, size.cy,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (dpiChanged) createTextResources(scale);
    rebuildTextLayout();
    m_updatingPlacement = false;
}

void OcrResultWindow::rebuildTextLayout() {
    m_textLayout.Reset();
    m_lines.clear();
    m_maxScroll = 0.0f;
    m_totalContentHeight = 0.0f;
    if (!m_textFormat || !m_dwriteFactory || m_wideText.empty() || !m_hwnd) return;

    RECT client{};
    GetClientRect(m_hwnd, &client);
    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    const float padding = OcrResultStyle::BasePadding * m_scale;
    const float header = OcrResultStyle::BaseHeaderHeight * m_scale;
    const float footer = OcrResultStyle::BaseFooterHeight * m_scale;
    const float indexColWidth = BaseIndexColWidth * m_scale;
    const float rowPadX = BaseRowPadX * m_scale;
    const float rowPadY = BaseRowPadY * m_scale;
    const float rowGap = BaseRowGap * m_scale;
    const float rightReserved = (BaseCopyIconAreaWidth + BaseScrollbarReserveWidth) * m_scale;
    const float contentWidth = (std::max)(
        1.0f, width - padding * 2.0f - indexColWidth - rowPadX - rightReserved);
    const float viewportHeight = (std::max)(1.0f, height - header - footer);

    // 按行切分并为每一行创建独立 IDWriteTextLayout，同时支持字符级拖选与原地微调编辑
    size_t start = 0;
    float curY = 0.0f;
    while (start <= m_wideText.size()) {
        size_t end = m_wideText.find(L'\n', start);
        std::wstring lineStr = (end == std::wstring::npos)
            ? m_wideText.substr(start)
            : m_wideText.substr(start, end - start);
        if (!lineStr.empty() && lineStr.back() == L'\r') lineStr.pop_back();

        if (!lineStr.empty() || m_editedSinceModeSwitch) {
            LineItem item;
            item.text = lineStr;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    item.text.c_str(), static_cast<UINT32>(item.text.size()),
                    m_textFormat.Get(), contentWidth, 10000.0f, &item.layout)) && item.layout) {
                DWRITE_TEXT_METRICS metrics{};
                float textH = OcrResultStyle::BaseBodyFont * m_scale * 1.5f;
                if (SUCCEEDED(item.layout->GetMetrics(&metrics))) {
                    textH = (std::max)(textH, metrics.height);
                }
                item.topY = curY;
                item.height = textH + rowPadY * 2.0f;
                curY += item.height + rowGap;
                m_lines.push_back(std::move(item));
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    m_totalContentHeight = (std::max)(0.0f, curY - rowGap);
    m_maxScroll = (std::max)(0.0f, m_totalContentHeight - viewportHeight);
    m_scrollY = std::clamp(m_scrollY, 0.0f, m_maxScroll);

    // 校验既有选区索引范围不越界
    auto clampPos = [&](TextPosition& pos) {
        if (!pos.isValid() || m_lines.empty()) {
            pos = {};
            return;
        }
        pos.line = std::clamp(pos.line, 0, static_cast<int>(m_lines.size()) - 1);
        pos.offset = (std::min)(pos.offset, static_cast<UINT32>(m_lines[pos.line].text.size()));
    };
    clampPos(m_selAnchor);
    clampPos(m_selCaret);
}

void OcrResultWindow::syncFullTextFromLines() {
    std::wstring joined;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (i > 0) joined.push_back(L'\n');
        joined += m_lines[i].text;
    }
    m_wideText = std::move(joined);
    m_text = tools3000::core::WinUtils::wstringToUtf8(m_wideText);
    if (!m_mergeParagraphs) {
        m_rawUnmergedWide = m_wideText;
        m_rawUnmergedText = m_text;
    }
}

void OcrResultWindow::pushEditUndo() {
    if (!m_editUndoStack.empty() && m_editUndoStack.back() == m_wideText) {
        return;
    }
    if (m_editUndoStack.size() >= MaxEditUndoDepth) {
        m_editUndoStack.erase(m_editUndoStack.begin());
    }
    m_editUndoStack.push_back(m_wideText);
}

bool OcrResultWindow::undoEdit() {
    if (m_editUndoStack.empty()) return false;
    m_wideText = std::move(m_editUndoStack.back());
    m_editUndoStack.pop_back();
    m_text = tools3000::core::WinUtils::wstringToUtf8(m_wideText);
    if (!m_mergeParagraphs) {
        m_rawUnmergedWide = m_wideText;
        m_rawUnmergedText = m_text;
    }
    m_editedSinceModeSwitch = true;
    rebuildTextLayout();
    if (!m_lines.empty() && m_selCaret.isValid()) {
        m_selCaret.line = std::clamp(m_selCaret.line, 0, static_cast<int>(m_lines.size()) - 1);
        m_selCaret.offset = (std::min)(m_selCaret.offset, static_cast<UINT32>(m_lines[m_selCaret.line].text.size()));
        m_selAnchor = m_selCaret;
    }
    if (m_hwnd && IsWindowVisible(m_hwnd)) render();
    return true;
}

bool OcrResultWindow::deleteSelectionInternal() {
    if (!hasSelection() || m_lines.empty()) return false;
    TextPosition start = (m_selAnchor < m_selCaret) ? m_selAnchor : m_selCaret;
    TextPosition end = (m_selAnchor < m_selCaret) ? m_selCaret : m_selAnchor;
    const int maxLine = static_cast<int>(m_lines.size()) - 1;
    start.line = std::clamp(start.line, 0, maxLine);
    end.line = std::clamp(end.line, 0, maxLine);
    start.offset = (std::min)(start.offset, static_cast<UINT32>(m_lines[start.line].text.size()));
    end.offset = (std::min)(end.offset, static_cast<UINT32>(m_lines[end.line].text.size()));

    if (start.line == end.line) {
        m_lines[start.line].text.erase(start.offset, end.offset - start.offset);
    } else {
        const std::wstring tail = m_lines[end.line].text.substr(end.offset);
        m_lines[start.line].text = m_lines[start.line].text.substr(0, start.offset) + tail;
        m_lines.erase(m_lines.begin() + start.line + 1, m_lines.begin() + end.line + 1);
    }
    m_selAnchor = start;
    m_selCaret = start;
    m_hasCaret = true;
    return true;
}

void OcrResultWindow::setMergeParagraphs(bool enabled) {
    if (m_mergeParagraphs == enabled) return;
    m_mergeParagraphs = enabled;
    m_editedSinceModeSwitch = false;
    m_editUndoStack.clear();
    if (m_mergeParagraphs) {
        m_wideText = OcrEngine::smartMergeParagraphs(m_rawUnmergedWide);
        m_text = tools3000::core::WinUtils::wstringToUtf8(m_wideText);
    } else {
        m_wideText = m_rawUnmergedWide;
        m_text = m_rawUnmergedText;
    }
    m_scrollY = 0.0f;
    clearSelection();
    rebuildTextLayout();
    if (m_hwnd && IsWindowVisible(m_hwnd)) {
        render();
    }
}

std::string OcrResultWindow::selectAndCopyWordAt(TextPosition pos) {
    if (m_lines.empty() || !pos.isValid()) return {};
    const int lineIdx = std::clamp(pos.line, 0, static_cast<int>(m_lines.size()) - 1);
    const std::wstring& lineStr = m_lines[lineIdx].text;
    if (lineStr.empty()) return {};

    size_t pivot = (std::min)(static_cast<size_t>(pos.offset), lineStr.size() - 1);
    if (classifyWordChar(lineStr[pivot]) == 0 && pivot > 0 && classifyWordChar(lineStr[pivot - 1]) != 0) {
        --pivot;
    }
    const int cls = classifyWordChar(lineStr[pivot]);
    if (cls == 0) return {};

    size_t wStart = pivot;
    while (wStart > 0 && classifyWordChar(lineStr[wStart - 1]) == cls) {
        --wStart;
    }
    size_t wEnd = pivot + 1;
    while (wEnd < lineStr.size() && classifyWordChar(lineStr[wEnd]) == cls) {
        ++wEnd;
    }
    // 剔除西文词块首尾多余句点
    if (cls == 1) {
        while (wStart < wEnd && lineStr[wStart] == L'.') ++wStart;
        while (wEnd > wStart && lineStr[wEnd - 1] == L'.') --wEnd;
        if (wStart >= wEnd) {
            wStart = pivot;
            wEnd = pivot + 1;
        }
    }

    m_selAnchor = {lineIdx, static_cast<UINT32>(wStart)};
    m_selCaret = {lineIdx, static_cast<UINT32>(wEnd)};
    m_hasCaret = true;
    m_isSelectingText = false;

    const std::string wordUtf8 = selectedTextUtf8();
    if (!wordUtf8.empty()) {
        m_copySucceeded = tools3000::core::WinUtils::copyToClipboard(wordUtf8);
        m_copiedLineIndex = -1;
        m_copiedSelection = false;
        m_copiedWord = true;
        m_copiedTime = GetTickCount64();
        if (m_hwnd) SetTimer(m_hwnd, CopyStateTimerId, CopyStateDurationMs, nullptr);
    }
    if (m_hwnd && IsWindowVisible(m_hwnd)) {
        render();
    }
    return wordUtf8;
}

void OcrResultWindow::insertTextAtCaret(const std::wstring& text) {
    if (text.empty()) return;
    if (m_lines.empty()) {
        m_lines.push_back(LineItem{L"", nullptr, 0.0f, 0.0f});
        m_selAnchor = {0, 0};
        m_selCaret = {0, 0};
    }
    if (!m_selCaret.isValid()) {
        const int lastIdx = static_cast<int>(m_lines.size()) - 1;
        m_selCaret = {lastIdx, static_cast<UINT32>(m_lines[lastIdx].text.size())};
        m_selAnchor = m_selCaret;
    }

    pushEditUndo();
    m_editedSinceModeSwitch = true;
    deleteSelectionInternal();

    int curLine = std::clamp(m_selCaret.line, 0, static_cast<int>(m_lines.size()) - 1);
    UINT32 curOff = (std::min)(m_selCaret.offset, static_cast<UINT32>(m_lines[curLine].text.size()));

    size_t segStart = 0;
    while (segStart <= text.size()) {
        const size_t nl = text.find(L'\n', segStart);
        const std::wstring seg = (nl == std::wstring::npos)
            ? text.substr(segStart)
            : text.substr(segStart, nl - segStart);

        if (!seg.empty()) {
            m_lines[curLine].text.insert(curOff, seg);
            curOff += static_cast<UINT32>(seg.size());
        }
        if (nl == std::wstring::npos) break;

        const std::wstring remainder = m_lines[curLine].text.substr(curOff);
        m_lines[curLine].text.erase(curOff);
        LineItem nextItem;
        nextItem.text = remainder;
        m_lines.insert(m_lines.begin() + curLine + 1, std::move(nextItem));
        ++curLine;
        curOff = 0;
        segStart = nl + 1;
    }

    m_selCaret = {curLine, curOff};
    m_selAnchor = m_selCaret;
    m_hasCaret = true;
    syncFullTextFromLines();
    rebuildTextLayout();
    if (m_hwnd && IsWindowVisible(m_hwnd)) {
        render();
    }
}

void OcrResultWindow::deleteBackwardAtCaret() {
    if (m_lines.empty() || !m_selCaret.isValid()) return;
    if (hasSelection()) {
        pushEditUndo();
        m_editedSinceModeSwitch = true;
        deleteSelectionInternal();
        syncFullTextFromLines();
        rebuildTextLayout();
        if (m_hwnd && IsWindowVisible(m_hwnd)) render();
        return;
    }
    int curLine = std::clamp(m_selCaret.line, 0, static_cast<int>(m_lines.size()) - 1);
    UINT32 curOff = (std::min)(m_selCaret.offset, static_cast<UINT32>(m_lines[curLine].text.size()));
    if (curOff > 0) {
        pushEditUndo();
        m_editedSinceModeSwitch = true;
        UINT32 eraseCount = 1;
        if (curOff >= 2) {
            const wchar_t low = m_lines[curLine].text[curOff - 1];
            const wchar_t high = m_lines[curLine].text[curOff - 2];
            if (low >= 0xDC00 && low <= 0xDFFF && high >= 0xD800 && high <= 0xDBFF) {
                eraseCount = 2;
            }
        }
        m_lines[curLine].text.erase(curOff - eraseCount, eraseCount);
        curOff -= eraseCount;
    } else if (curLine > 0) {
        pushEditUndo();
        m_editedSinceModeSwitch = true;
        const UINT32 prevLen = static_cast<UINT32>(m_lines[curLine - 1].text.size());
        m_lines[curLine - 1].text += m_lines[curLine].text;
        m_lines.erase(m_lines.begin() + curLine);
        --curLine;
        curOff = prevLen;
    } else {
        return;
    }
    m_selCaret = {curLine, curOff};
    m_selAnchor = m_selCaret;
    m_hasCaret = true;
    syncFullTextFromLines();
    rebuildTextLayout();
    if (m_hwnd && IsWindowVisible(m_hwnd)) render();
}

void OcrResultWindow::deleteForwardAtCaret() {
    if (m_lines.empty() || !m_selCaret.isValid()) return;
    if (hasSelection()) {
        pushEditUndo();
        m_editedSinceModeSwitch = true;
        deleteSelectionInternal();
        syncFullTextFromLines();
        rebuildTextLayout();
        if (m_hwnd && IsWindowVisible(m_hwnd)) render();
        return;
    }
    int curLine = std::clamp(m_selCaret.line, 0, static_cast<int>(m_lines.size()) - 1);
    UINT32 curOff = (std::min)(m_selCaret.offset, static_cast<UINT32>(m_lines[curLine].text.size()));
    if (curOff < m_lines[curLine].text.size()) {
        pushEditUndo();
        m_editedSinceModeSwitch = true;
        UINT32 eraseCount = 1;
        if (curOff + 1 < m_lines[curLine].text.size()) {
            const wchar_t high = m_lines[curLine].text[curOff];
            const wchar_t low = m_lines[curLine].text[curOff + 1];
            if (high >= 0xD800 && high <= 0xDBFF && low >= 0xDC00 && low <= 0xDFFF) {
                eraseCount = 2;
            }
        }
        m_lines[curLine].text.erase(curOff, eraseCount);
    } else if (curLine + 1 < static_cast<int>(m_lines.size())) {
        pushEditUndo();
        m_editedSinceModeSwitch = true;
        m_lines[curLine].text += m_lines[curLine + 1].text;
        m_lines.erase(m_lines.begin() + curLine + 1);
    } else {
        return;
    }
    m_selCaret = {curLine, curOff};
    m_selAnchor = m_selCaret;
    m_hasCaret = true;
    syncFullTextFromLines();
    rebuildTextLayout();
    if (m_hwnd && IsWindowVisible(m_hwnd)) render();
}

bool OcrResultWindow::hasSelection() const noexcept {
    return m_selAnchor.isValid() && m_selCaret.isValid() && m_selAnchor != m_selCaret && !m_lines.empty();
}

std::string OcrResultWindow::selectedTextUtf8() const {
    if (!hasSelection()) return {};
    TextPosition start = (m_selAnchor < m_selCaret) ? m_selAnchor : m_selCaret;
    TextPosition end = (m_selAnchor < m_selCaret) ? m_selCaret : m_selAnchor;

    const int maxLine = static_cast<int>(m_lines.size()) - 1;
    start.line = std::clamp(start.line, 0, maxLine);
    end.line = std::clamp(end.line, 0, maxLine);
    start.offset = (std::min)(start.offset, static_cast<UINT32>(m_lines[start.line].text.size()));
    end.offset = (std::min)(end.offset, static_cast<UINT32>(m_lines[end.line].text.size()));

    std::wstring selectedWide;
    if (start.line == end.line) {
        if (end.offset > start.offset) {
            selectedWide = m_lines[start.line].text.substr(start.offset, end.offset - start.offset);
        }
    } else {
        for (int i = start.line; i <= end.line; ++i) {
            if (i > start.line) selectedWide += L"\r\n";
            const auto& lineText = m_lines[i].text;
            if (i == start.line) {
                selectedWide += lineText.substr(start.offset);
            } else if (i == end.line) {
                selectedWide += lineText.substr(0, end.offset);
            } else {
                selectedWide += lineText;
            }
        }
    }
    return tools3000::core::WinUtils::wstringToUtf8(selectedWide);
}

void OcrResultWindow::setSelectionRange(TextPosition anchor, TextPosition caret) {
    if (m_lines.empty()) {
        clearSelection();
        return;
    }
    const int maxLine = static_cast<int>(m_lines.size()) - 1;
    anchor.line = std::clamp(anchor.line, 0, maxLine);
    caret.line = std::clamp(caret.line, 0, maxLine);
    anchor.offset = (std::min)(anchor.offset, static_cast<UINT32>(m_lines[anchor.line].text.size()));
    caret.offset = (std::min)(caret.offset, static_cast<UINT32>(m_lines[caret.line].text.size()));
    m_selAnchor = anchor;
    m_selCaret = caret;
    if (m_hwnd && IsWindowVisible(m_hwnd)) render();
}

void OcrResultWindow::selectAll() {
    if (m_lines.empty()) return;
    m_selAnchor = {0, 0};
    const int lastLine = static_cast<int>(m_lines.size()) - 1;
    m_selCaret = {lastLine, static_cast<UINT32>(m_lines[lastLine].text.size())};
    if (m_hwnd && IsWindowVisible(m_hwnd)) render();
}

void OcrResultWindow::clearSelection() {
    m_selAnchor = {};
    m_selCaret = {};
    m_hasCaret = false;
    m_isSelectingText = false;
}

void OcrResultWindow::copySelectionOrAll() {
    if (hasSelection()) {
        const std::string selUtf8 = selectedTextUtf8();
        if (selUtf8.empty()) return;
        m_copySucceeded = tools3000::core::WinUtils::copyToClipboard(selUtf8);
        m_copiedLineIndex = -1;
        m_copiedSelection = true;
        m_copiedWord = false;
        m_copiedTime = GetTickCount64();
        if (m_hwnd) SetTimer(m_hwnd, CopyStateTimerId, CopyStateDurationMs, nullptr);
        render();
        return;
    }
    copyAll();
}

bool OcrResultWindow::isFooterHintSingleLine(float clientWidth) const {
    if (!m_dwriteFactory || !m_hintFormat) return false;
    const float padding = OcrResultStyle::BasePadding * m_scale;
    const float copyWidth = OcrResultStyle::BaseButtonWidth * m_scale;
    const float closeWidth = OcrResultStyle::BaseCloseButtonWidth * m_scale;
    const float buttonGap = OcrResultStyle::BaseButtonGap * m_scale;
    const float buttonHeight = OcrResultStyle::BaseButtonHeight * m_scale;
    const float availWidth = (std::max)(
        1.0f, clientWidth - padding * 2.0f - copyWidth - closeWidth - buttonGap * 2.0f);

    for (bool chinese : {true, false}) {
        const wchar_t* text = footerHintText(chinese, false);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(m_dwriteFactory->CreateTextLayout(
                text, static_cast<UINT32>(wcslen(text)),
                m_hintFormat.Get(), availWidth, buttonHeight, &layout)) || !layout) {
            return false;
        }
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics)) || metrics.lineCount > 1) {
            return false;
        }
    }
    return true;
}

bool OcrResultWindow::computeScrollbarMetrics(
    int clientWidth, int clientHeight,
    D2D1_RECT_F& outTrackRect, D2D1_RECT_F& outThumbRect) const {
    if (m_maxScroll <= 0.5f || m_totalContentHeight <= 1.0f) return false;
    const float padding = OcrResultStyle::BasePadding * m_scale;
    const float header = OcrResultStyle::BaseHeaderHeight * m_scale;
    const float footer = OcrResultStyle::BaseFooterHeight * m_scale;
    const float vpTop = header;
    const float vpBottom = static_cast<float>(clientHeight) - footer;
    const float vpH = vpBottom - vpTop;
    if (vpH <= 8.0f) return false;

    const float rightEdge = static_cast<float>(clientWidth) - padding;
    outTrackRect = D2D1::RectF(rightEdge - 10.0f * m_scale, vpTop, rightEdge + 4.0f * m_scale, vpBottom);
    const float thumbH = std::clamp(vpH * (vpH / m_totalContentHeight), 24.0f * m_scale, vpH);
    const float ratio = std::clamp(m_scrollY / m_maxScroll, 0.0f, 1.0f);
    const float thumbY = vpTop + (vpH - thumbH) * ratio;
    const float visualW = (m_hoverScrollbar || m_draggingScrollbar) ? 6.0f * m_scale : 4.5f * m_scale;
    outThumbRect = D2D1::RectF(rightEdge - visualW, thumbY, rightEdge, thumbY + thumbH);
    return true;
}

D2D1_RECT_F OcrResultWindow::computeLineCopyButtonRect(int lineIndex, int clientWidth) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(m_lines.size())) return {};
    const float padding = OcrResultStyle::BasePadding * m_scale;
    const float header = OcrResultStyle::BaseHeaderHeight * m_scale;
    const float rowRight = static_cast<float>(clientWidth) - padding -
                           (m_maxScroll > 0.5f ? BaseScrollbarReserveWidth * m_scale : 0.0f);
    const float btnSize = 22.0f * m_scale;
    const float rowTop = header + m_lines[lineIndex].topY - m_scrollY;
    const float btnRight = rowRight - 4.0f * m_scale;
    const float btnTop = rowTop + (std::max)(2.0f * m_scale, (m_lines[lineIndex].height - btnSize) * 0.5f);
    return D2D1::RectF(btnRight - btnSize, btnTop, btnRight, btnTop + btnSize);
}

OcrResultWindow::TextPosition OcrResultWindow::hitTestTextPosition(POINT clientPt) const {
    if (m_lines.empty()) return {};

    const float padding = OcrResultStyle::BasePadding * m_scale;
    const float header = OcrResultStyle::BaseHeaderHeight * m_scale;
    const float indexColWidth = BaseIndexColWidth * m_scale;
    const float rowPadX = BaseRowPadX * m_scale;
    const float rowPadY = BaseRowPadY * m_scale;
    const float textOriginX = padding + indexColWidth + rowPadX * 0.5f;
    const float relY = static_cast<float>(clientPt.y) - header + m_scrollY;

    int lineIdx = 0;
    if (relY <= m_lines.front().topY) {
        lineIdx = 0;
    } else if (relY >= m_lines.back().topY + m_lines.back().height) {
        lineIdx = static_cast<int>(m_lines.size()) - 1;
    } else {
        for (int i = 0; i < static_cast<int>(m_lines.size()); ++i) {
            const float nextTop = (i + 1 < static_cast<int>(m_lines.size()))
                ? m_lines[i + 1].topY
                : (m_lines[i].topY + m_lines[i].height);
            if (relY < nextTop) {
                lineIdx = i;
                break;
            }
        }
    }

    const auto& item = m_lines[lineIdx];
    if (!item.layout) {
        return {lineIdx, 0};
    }

    const float localX = static_cast<float>(clientPt.x) - textOriginX;
    const float localY = relY - (item.topY + rowPadY);
    BOOL isTrailingHit = FALSE;
    BOOL isInside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (SUCCEEDED(item.layout->HitTestPoint(localX, localY, &isTrailingHit, &isInside, &metrics))) {
        UINT32 offset = metrics.textPosition + (isTrailingHit ? metrics.length : 0u);
        offset = (std::min)(offset, static_cast<UINT32>(item.text.size()));
        return {lineIdx, offset};
    }
    return {lineIdx, 0};
}

void OcrResultWindow::showResult(const std::string& text, const RECT* anchorRect,
                                 const std::wstring& statusBadge) {
    const bool hasAnchor = (anchorRect != nullptr && anchorRect->right > anchorRect->left && anchorRect->bottom > anchorRect->top);
    const RECT capturedAnchor = hasAnchor ? *anchorRect : RECT{};
    auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
    if (m_hwnd && GetWindowThreadProcessId(m_hwnd, nullptr) != GetCurrentThreadId()) {
        dispatcher.post([this, text, hasAnchor, capturedAnchor, statusBadge]() {
            showResult(text, hasAnchor ? &capturedAnchor : nullptr, statusBadge);
        });
        return;
    }
    if (!m_hwnd && dispatcher.isInitialized() && !dispatcher.isOwnerThread()) {
        dispatcher.post([this, text, hasAnchor, capturedAnchor, statusBadge]() {
            showResult(text, hasAnchor ? &capturedAnchor : nullptr, statusBadge);
        });
        return;
    }

    if ((!m_hwnd || !IsWindow(m_hwnd)) && !initialize()) return;

    const bool alreadyVisible = IsWindowVisible(m_hwnd) != FALSE;
    if (hasAnchor) {
        m_hasAnchorRect = true;
        m_anchorRect = capturedAnchor;
    } else if (!alreadyVisible) {
        m_hasAnchorRect = false;
        m_anchorRect = {};
    }

    m_rawUnmergedText = text;
    m_rawUnmergedWide = tools3000::core::WinUtils::utf8ToWstring(text);
    m_statusBadge = statusBadge;
    const bool isZh = tools3000::core::isUiLanguageChinese();
    const std::string loadingLabel = tools3000::core::trUtf8CstrFor(isZh, "正在识别...", "Recognizing...");
    m_isLoadingState = (text == loadingLabel);
    if (m_isLoadingState) {
        m_loadingSpinAngle = 0.0f;
        m_loadingStartTick = GetTickCount64();
        SetTimer(m_hwnd, LoadingAnimTimerId, LoadingAnimIntervalMs, nullptr);
    } else {
        KillTimer(m_hwnd, LoadingAnimTimerId);
        m_loadingStartTick = 0;
    }
    m_mergeParagraphs = false;
    m_editedSinceModeSwitch = false;
    m_editUndoStack.clear();
    m_text = m_rawUnmergedText;
    m_wideText = m_rawUnmergedWide;
    m_scrollY = 0.0f;
    m_maxScroll = 0.0f;
    m_totalContentHeight = 0.0f;
    m_copiedTime = 0;
    m_copySucceeded = false;
    m_copiedSelection = false;
    m_copiedWord = false;
    m_hoverTopClose = false;
    m_hoverMerge = false;
    m_hoverCopy = false;
    m_hoverClose = false;
    m_hoverScrollbar = false;
    m_draggingScrollbar = false;
    m_hoverLineCopyBtn = -1;
    m_pressedLineCopyBtn = -1;
    m_pressedTopCloseBtn = false;
    m_pressedMergeBtn = false;
    m_pressedCopyBtn = false;
    m_pressedCloseBtn = false;
    m_hoverLineIndex = -1;
    m_copiedLineIndex = -1;
    resetColorBrushes();
    clearSelection();
    KillTimer(m_hwnd, CopyStateTimerId);

    if (!alreadyVisible || hasAnchor) {
        updatePlacement();
    } else {
        rebuildTextLayout();
    }
    if (!alreadyVisible) {
        m_showTime = GetTickCount64();
        m_currentAlpha = 0.0f;
        ShowWindow(m_hwnd, SW_SHOW);
        SetTimer(m_hwnd, FadeTimerId, FadeIntervalMs, nullptr);
    } else {
        // 窗口已可见时直接稳态满不透明 (255.0f)，杜绝旧版误设为 1.0f 导致窗口变 1/255 透明幽灵窗
        KillTimer(m_hwnd, FadeTimerId);
        m_currentAlpha = 255.0f;
    }
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    render();
}

void OcrResultWindow::updateLoadingStatus(const std::string& statusText, const std::wstring& statusBadge) {
    auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
    if (m_hwnd && GetWindowThreadProcessId(m_hwnd, nullptr) != GetCurrentThreadId()) {
        dispatcher.post([this, statusText, statusBadge]() {
            updateLoadingStatus(statusText, statusBadge);
        });
        return;
    }
    if (!m_hwnd || !IsWindow(m_hwnd) || !IsWindowVisible(m_hwnd) || !m_isLoadingState) {
        return;
    }
    m_rawUnmergedText = statusText;
    m_rawUnmergedWide = tools3000::core::WinUtils::utf8ToWstring(statusText);
    m_text = m_rawUnmergedText;
    m_wideText = m_rawUnmergedWide;
    m_statusBadge = statusBadge;
    rebuildTextLayout();
    render();
}

void OcrResultWindow::copyAll() {
    if (m_text.empty()) return;
    m_copySucceeded = tools3000::core::WinUtils::copyToClipboard(m_text);
    m_copiedLineIndex = -1;
    m_copiedSelection = false;
    m_copiedWord = false;
    m_copiedTime = GetTickCount64();
    if (m_hwnd) SetTimer(m_hwnd, CopyStateTimerId, CopyStateDurationMs, nullptr);
    render();
}

void OcrResultWindow::copyLine(int lineIndex) {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(m_lines.size())) return;
    const std::string utf8Line = tools3000::core::WinUtils::wstringToUtf8(m_lines[lineIndex].text);
    if (utf8Line.empty()) return;
    m_copySucceeded = tools3000::core::WinUtils::copyToClipboard(utf8Line);
    m_copiedLineIndex = lineIndex;
    m_copiedSelection = false;
    m_copiedWord = false;
    m_copiedTime = GetTickCount64();
    if (m_hwnd) SetTimer(m_hwnd, CopyStateTimerId, CopyStateDurationMs, nullptr);
    render();
}

void OcrResultWindow::hide() {
    if (!m_hwnd) return;
    if (GetCapture() == m_hwnd) ReleaseCapture();
    KillTimer(m_hwnd, FadeTimerId);
    KillTimer(m_hwnd, CopyStateTimerId);
    KillTimer(m_hwnd, LoadingAnimTimerId);
    m_isLoadingState = false;
    m_loadingStartTick = 0;
    ShowWindow(m_hwnd, SW_HIDE);
    releaseSurface();
    m_lines.clear();
    m_lines.shrink_to_fit();
    m_textLayout.Reset();
    m_rawUnmergedText.clear();
    m_rawUnmergedText.shrink_to_fit();
    m_rawUnmergedWide.clear();
    m_rawUnmergedWide.shrink_to_fit();
    m_text.clear();
    m_text.shrink_to_fit();
    m_wideText.clear();
    m_wideText.shrink_to_fit();
    m_editUndoStack.clear();
    m_editUndoStack.shrink_to_fit();
    clearSelection();
    m_draggingScrollbar = false;
    tools3000::core::WinUtils::trimWorkingSet();
}

void OcrResultWindow::updateHover(POINT point) {
    const bool hoverTopClose = contains(m_btnTopCloseRect, point);
    const bool hoverMerge = contains(m_btnMergeRect, point);
    const bool hoverCopy = contains(m_btnCopyRect, point);
    const bool hoverClose = contains(m_btnCloseRect, point);

    RECT client{};
    GetClientRect(m_hwnd, &client);
    const int clientW = client.right - client.left;
    const int clientH = client.bottom - client.top;
    const float width = static_cast<float>(clientW);
    const float height = static_cast<float>(clientH);
    const float padding = OcrResultStyle::BasePadding * m_scale;
    const float header = OcrResultStyle::BaseHeaderHeight * m_scale;
    const float footer = OcrResultStyle::BaseFooterHeight * m_scale;

    D2D1_RECT_F trackRect{};
    D2D1_RECT_F thumbRect{};
    const bool hoverScrollbar =
        computeScrollbarMetrics(clientW, clientH, trackRect, thumbRect) &&
        contains(trackRect, point);

    int hoverLine = -1;
    int hoverCopyBtn = -1;
    bool inTextViewport = false;
    bool overLineIndex = false;
    if (!hoverScrollbar &&
        point.x >= padding && point.x <= width - padding &&
        point.y >= header && point.y <= height - footer) {
        inTextViewport = true;
        overLineIndex = (point.x < padding + BaseIndexColWidth * m_scale);
        const float relY = static_cast<float>(point.y) - header + m_scrollY;
        for (int i = 0; i < static_cast<int>(m_lines.size()); ++i) {
            if (relY >= m_lines[i].topY && relY <= m_lines[i].topY + m_lines[i].height) {
                hoverLine = i;
                if (contains(computeLineCopyButtonRect(i, clientW), point)) {
                    hoverCopyBtn = i;
                }
                break;
            }
        }
    }

    const bool inDragHeader = (point.y >= 0 && point.y < header && !hoverMerge && !hoverTopClose);
    const LPCWSTR cursorId =
        (hoverTopClose || hoverMerge || hoverCopy || hoverClose || hoverScrollbar || hoverCopyBtn >= 0 ||
         (overLineIndex && hoverLine >= 0))
            ? IDC_HAND
            : (inTextViewport && !m_lines.empty() ? IDC_IBEAM : (inDragHeader ? IDC_SIZEALL : IDC_ARROW));
    SetCursor(LoadCursorW(nullptr, cursorId));

    if (hoverTopClose == m_hoverTopClose &&
        hoverMerge == m_hoverMerge &&
        hoverCopy == m_hoverCopy &&
        hoverClose == m_hoverClose &&
        hoverScrollbar == m_hoverScrollbar &&
        hoverLine == m_hoverLineIndex &&
        hoverCopyBtn == m_hoverLineCopyBtn) {
        return;
    }
    m_hoverTopClose = hoverTopClose;
    m_hoverMerge = hoverMerge;
    m_hoverCopy = hoverCopy;
    m_hoverClose = hoverClose;
    m_hoverScrollbar = hoverScrollbar;
    m_hoverLineIndex = hoverLine;
    m_hoverLineCopyBtn = hoverCopyBtn;
    render();
}

void OcrResultWindow::render(int retryDepth) {
    if (!m_hwnd || !IsWindowVisible(m_hwnd)) return;
    if (!m_renderTarget && !createResources()) return;

    RECT client{};
    GetClientRect(m_hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (!ensureSurface(width, height)) return;
    std::memset(m_pixels, 0, static_cast<size_t>(width) * height * 4);

    RECT bindRect{0, 0, width, height};
    if (FAILED(m_renderTarget->BindDC(m_memoryDc, &bindRect))) return;
    if (!m_brushBg) {
        const auto pal = resolveOcrThemePalette();
        m_isDarkTheme = pal.isDark;
        const auto brush = [&](D2D1_COLOR_F color,
                               Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& output) {
            return SUCCEEDED(m_renderTarget->CreateSolidColorBrush(color, &output));
        };
        if (!brush(pal.bg, m_brushBg) ||
            !brush(pal.innerHighlight, m_brushInnerHighlight) ||
            !brush(pal.text, m_brushText) ||
            !brush(pal.muted, m_brushMuted) ||
            !brush(pal.border, m_brushBorder) ||
            !brush(pal.btn, m_brushBtn) ||
            !brush(pal.btnHover, m_brushBtnHover) ||
            !brush(pal.accent, m_brushAccent) ||
            !brush(pal.accentSubtle, m_brushAccentSubtle) ||
            !brush(D2D1::ColorF(0x10B981, 0.88f), m_brushSuccess) ||
            !brush(D2D1::ColorF(0xEF4444, 0.88f), m_brushError) ||
            !brush(pal.selection, m_brushSelection)) {
            resetColorBrushes();
            return;
        }
    }

    const float scale = m_scale;
    const float shadowInset = BaseShadowInset * scale;
    const float padding = OcrResultStyle::BasePadding * scale;
    const float header = OcrResultStyle::BaseHeaderHeight * scale;
    const float footer = OcrResultStyle::BaseFooterHeight * scale;
    const float radius = OcrResultStyle::BaseCornerRadius * scale;
    const float border = OcrResultStyle::BaseBorderWidth * scale;
    const float buttonHeight = OcrResultStyle::BaseButtonHeight * scale;
    const float copyWidth = OcrResultStyle::BaseButtonWidth * scale;
    const float closeWidth = OcrResultStyle::BaseCloseButtonWidth * scale;
    const float buttonGap = OcrResultStyle::BaseButtonGap * scale;
    const bool chinese = tools3000::core::isUiLanguageChinese();

    const float topCloseSz = 26.0f * scale;
    const float mergeBtnW = (chinese ? 94.0f : 104.0f) * scale;
    const float mergeBtnH = 26.0f * scale;
    const float mergeBtnTop = shadowInset + (std::max)(4.0f * scale, (header - shadowInset - mergeBtnH) * 0.5f);
    m_btnTopCloseRect = D2D1::RectF(
        width - padding - topCloseSz, mergeBtnTop,
        width - padding, mergeBtnTop + topCloseSz);
    const float topGap = 8.0f * scale;
    m_btnMergeRect = D2D1::RectF(
        m_btnTopCloseRect.left - topGap - mergeBtnW, mergeBtnTop,
        m_btnTopCloseRect.left - topGap, mergeBtnTop + mergeBtnH);

    m_btnCloseRect = D2D1::RectF(
        width - padding - closeWidth, height - padding - buttonHeight,
        width - padding, height - padding);
    m_btnCopyRect = D2D1::RectF(
        m_btnCloseRect.left - buttonGap - copyWidth,
        m_btnCloseRect.top, m_btnCloseRect.left - buttonGap,
        m_btnCloseRect.bottom);

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0.0f));

    // 1. 分层柔和投影（Dual-Layer Ambient & Key Soft Drop Shadow）
    const D2D1_RECT_F cardRect = D2D1::RectF(
        shadowInset, shadowInset,
        (std::max)(shadowInset + 2.0f, static_cast<float>(width) - shadowInset),
        (std::max)(shadowInset + 2.0f, static_cast<float>(height) - shadowInset));
    {
        struct ShadowLayer {
            float dy;
            float expand;
            float alpha;
        };
        const float shadowScale = m_isDarkTheme ? 1.35f : 1.0f;
        static constexpr ShadowLayer kLayers[] = {
            {2.0f, 5.2f, 0.032f},
            {1.6f, 3.6f, 0.052f},
            {1.0f, 2.1f, 0.075f},
            {0.5f, 0.9f, 0.095f},
        };
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush;
        if (SUCCEEDED(m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &shadowBrush)) && shadowBrush) {
            for (const auto& sl : kLayers) {
                const float exp = sl.expand * scale;
                const float dy = sl.dy * scale;
                const D2D1_RECT_F sr = D2D1::RectF(
                    cardRect.left - exp,
                    cardRect.top - exp + dy,
                    cardRect.right + exp,
                    cardRect.bottom + exp + dy);
                shadowBrush->SetOpacity(std::clamp(sl.alpha * shadowScale, 0.0f, 0.35f));
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(sr, radius + exp, radius + exp), shadowBrush.Get());
            }
        }
    }

    // 2. 微晶玻璃卡片主体 + 内高光环 + 动态强调色细边
    const auto card = D2D1::RoundedRect(cardRect, radius, radius);
    m_renderTarget->FillRoundedRectangle(&card, m_brushBg.Get());
    if (m_brushInnerHighlight) {
        const float inOff = 1.0f * scale;
        const auto innerCard = D2D1::RoundedRect(
            D2D1::RectF(cardRect.left + inOff, cardRect.top + inOff,
                        cardRect.right - inOff, cardRect.bottom - inOff),
            (std::max)(1.0f, radius - inOff), (std::max)(1.0f, radius - inOff));
        m_renderTarget->DrawRoundedRectangle(&innerCard, m_brushInnerHighlight.Get(), 0.9f * scale);
    }
    m_renderTarget->DrawRoundedRectangle(&card, m_brushBorder.Get(), border);

    // 3. 顶栏标题与“合并段落”切换胶囊按钮
    std::wstring title;
    if (m_isLoadingState) {
        title = chinese ? L"正在文字识别" : L"Recognizing Text";
        if (!m_statusBadge.empty()) {
            title += L"  ·  " + m_statusBadge;
        }
    } else {
        title = chinese
            ? (L"文字识别结果 (" + std::to_wstring(m_lines.size()) + L" 行)")
            : (L"Recognized Text (" + std::to_wstring(m_lines.size()) + L" lines)");
        if (m_copiedTime && m_copySucceeded) {
            if (m_copiedWord) {
                title += chinese ? L"  ·  已复制词块" : L"  ·  Copied word";
            } else if (m_copiedLineIndex >= 0) {
                title += chinese
                    ? (L"  ·  已复制第 " + std::to_wstring(m_copiedLineIndex + 1) + L" 行")
                    : (L"  ·  Copied line " + std::to_wstring(m_copiedLineIndex + 1));
            } else if (m_copiedSelection) {
                title += chinese ? L"  ·  已复制选中文字" : L"  ·  Copied selection";
            }
        } else if (!m_statusBadge.empty()) {
            title += L"  ·  " + m_statusBadge;
        }
    }
    const float titleRight = (std::max)(padding, m_btnMergeRect.left - 8.0f * scale);
    m_renderTarget->DrawTextW(
        title.c_str(), static_cast<UINT32>(title.size()), m_titleFormat.Get(),
        D2D1::RectF(padding, shadowInset, titleRight, header),
        m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    {
        const float pillR = mergeBtnH * 0.5f;
        const auto mergePill = D2D1::RoundedRect(m_btnMergeRect, pillR, pillR);
        ID2D1SolidColorBrush* pillBg = m_mergeParagraphs
            ? m_brushAccentSubtle.Get()
            : (m_hoverMerge ? m_brushBtnHover.Get() : m_brushBtn.Get());
        if (pillBg) m_renderTarget->FillRoundedRectangle(&mergePill, pillBg);
        if (m_mergeParagraphs && m_brushAccent) {
            m_renderTarget->DrawRoundedRectangle(&mergePill, m_brushAccent.Get(), 1.1f * scale);
        }
        float labelLeft = m_btnMergeRect.left;
        if (m_mergeParagraphs && m_brushAccent) {
            // 纯矢量勾选标记
            const float ckX = m_btnMergeRect.left + 11.5f * scale;
            const float ckY = (m_btnMergeRect.top + m_btnMergeRect.bottom) * 0.5f;
            m_renderTarget->DrawLine(
                D2D1::Point2F(ckX - 3.2f * scale, ckY + 0.2f * scale),
                D2D1::Point2F(ckX - 0.8f * scale, ckY + 2.8f * scale),
                m_brushAccent.Get(), 1.6f * scale);
            m_renderTarget->DrawLine(
                D2D1::Point2F(ckX - 0.8f * scale, ckY + 2.8f * scale),
                D2D1::Point2F(ckX + 4.0f * scale, ckY - 2.6f * scale),
                m_brushAccent.Get(), 1.6f * scale);
            labelLeft += 14.0f * scale;
        }
        const std::wstring mergeLabel = chinese ? L"合并段落" : L"Merge Lines";
        ID2D1SolidColorBrush* mergeFg = m_mergeParagraphs
            ? (m_isDarkTheme ? m_brushText.Get() : m_brushAccent.Get())
            : (m_hoverMerge ? m_brushText.Get() : m_brushMuted.Get());
        m_renderTarget->DrawTextW(
            mergeLabel.c_str(), static_cast<UINT32>(mergeLabel.size()),
            m_hintFormat.Get(),
            D2D1::RectF(labelLeft + (m_mergeParagraphs ? 4.0f * scale : 10.0f * scale),
                        m_btnMergeRect.top, m_btnMergeRect.right - 6.0f * scale, m_btnMergeRect.bottom),
            mergeFg, D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // 右上角标准微晶矢量关闭按钮 (×)：悬停呈现柔和珊瑚绯红微光底与锐利高反差交叉线
        const float closeR = 7.0f * scale;
        const auto closePill = D2D1::RoundedRect(m_btnTopCloseRect, closeR, closeR);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> closeHoverBg, closeHoverFg;
        if (m_hoverTopClose) {
            m_renderTarget->CreateSolidColorBrush(
                m_isDarkTheme ? D2D1::ColorF(239.0f / 255.0f, 68.0f / 255.0f, 68.0f / 255.0f, 0.26f)
                              : D2D1::ColorF(239.0f / 255.0f, 68.0f / 255.0f, 68.0f / 255.0f, 0.14f),
                &closeHoverBg);
            m_renderTarget->CreateSolidColorBrush(
                m_isDarkTheme ? D2D1::ColorF(254.0f / 255.0f, 202.0f / 255.0f, 202.0f / 255.0f, 0.98f)
                              : D2D1::ColorF(220.0f / 255.0f, 38.0f / 255.0f, 38.0f / 255.0f, 0.98f),
                &closeHoverFg);
            if (closeHoverBg) {
                m_renderTarget->FillRoundedRectangle(&closePill, closeHoverBg.Get());
            }
        } else if (m_brushBtn) {
            m_renderTarget->FillRoundedRectangle(&closePill, m_brushBtn.Get());
        }
        ID2D1SolidColorBrush* xStroke = (m_hoverTopClose && closeHoverFg) ? closeHoverFg.Get() : m_brushMuted.Get();
        if (xStroke) {
            const float cx = (m_btnTopCloseRect.left + m_btnTopCloseRect.right) * 0.5f;
            const float cy = (m_btnTopCloseRect.top + m_btnTopCloseRect.bottom) * 0.5f;
            const float half = 4.6f * scale;
            const float sw = 1.65f * scale;
            m_renderTarget->DrawLine(D2D1::Point2F(cx - half, cy - half), D2D1::Point2F(cx + half, cy + half), xStroke, sw);
            m_renderTarget->DrawLine(D2D1::Point2F(cx + half, cy - half), D2D1::Point2F(cx - half, cy + half), xStroke, sw);
        }
    }

    if (m_isLoadingState) {
        // 深度识别/长耗时加载态：纯矢量双环旋转指示器 + 实时阶段主文案 + 温和副提示（0 Emoji，全 DPI 清晰）
        const D2D1_RECT_F viewport = D2D1::RectF(
            padding, header, width - padding, height - footer);
        m_renderTarget->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const float centerX = (viewport.left + viewport.right) * 0.5f;
        const float centerY = (viewport.top + viewport.bottom) * 0.5f - 10.0f * scale;
        const float spinR = 13.0f * scale;
        const float spinStroke = 2.4f * scale;

        if (m_brushBtn) {
            m_renderTarget->DrawEllipse(
                D2D1::Ellipse(D2D1::Point2F(centerX, centerY - 18.0f * scale), spinR, spinR),
                m_brushBtn.Get(), spinStroke);
        }
        if (m_d2dFactory && m_brushAccent) {
            constexpr float kPi = 3.14159265f;
            const float a0 = m_loadingSpinAngle;
            const float a1 = a0 + 1.45f * kPi;
            const float scx = centerX;
            const float scy = centerY - 18.0f * scale;
            const D2D1_POINT_2F pStart = D2D1::Point2F(scx + spinR * std::cos(a0), scy + spinR * std::sin(a0));
            const D2D1_POINT_2F pEnd = D2D1::Point2F(scx + spinR * std::cos(a1), scy + spinR * std::sin(a1));
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> arcGeo;
            if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(&arcGeo)) && arcGeo) {
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(arcGeo->Open(&sink)) && sink) {
                    sink->BeginFigure(pStart, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(
                        pEnd,
                        D2D1::SizeF(spinR, spinR),
                        0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE,
                        D2D1_ARC_SIZE_LARGE));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if (SUCCEEDED(sink->Close())) {
                        m_renderTarget->DrawGeometry(arcGeo.Get(), m_brushAccent.Get(), spinStroke);
                    }
                }
            }
        }

        const std::wstring stageMain = m_wideText.empty()
            ? (chinese ? L"正在识别..." : L"Recognizing...")
            : m_wideText;
        if (m_dwriteFactory && m_textFormat && m_brushText) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> stageLayout;
            const float maxTextW = (std::max)(120.0f * scale, (viewport.right - viewport.left) - 24.0f * scale);
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    stageMain.c_str(), static_cast<UINT32>(stageMain.size()),
                    m_textFormat.Get(), maxTextW, 64.0f * scale, &stageLayout)) && stageLayout) {
                stageLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(viewport.left + 12.0f * scale, centerY + 6.0f * scale),
                    stageLayout.Get(), m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        const uint64_t elapsedMs = (m_loadingStartTick > 0) ? (GetTickCount64() - m_loadingStartTick) : 0ULL;
        if (elapsedMs >= 1200ULL && m_hintFormat && m_brushMuted) {
            const std::wstring subHint = chinese
                ? L"深度模型正在精细计算，请稍候（按 Esc 可关闭）"
                : L"Deep model processing, please wait (Esc to close)";
            Microsoft::WRL::ComPtr<IDWriteTextLayout> subLayout;
            const float maxTextW = (std::max)(120.0f * scale, (viewport.right - viewport.left) - 24.0f * scale);
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    subHint.c_str(), static_cast<UINT32>(subHint.size()),
                    m_hintFormat.Get(), maxTextW, 32.0f * scale, &subLayout)) && subLayout) {
                subLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(viewport.left + 12.0f * scale, centerY + 34.0f * scale),
                    subLayout.Get(), m_brushMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        m_renderTarget->PopAxisAlignedClip();
    } else if (!m_lines.empty()) {
        const D2D1_RECT_F viewport = D2D1::RectF(
            padding, header, width - padding, height - footer);
        m_renderTarget->PushAxisAlignedClip(
            viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const float indexColWidth = BaseIndexColWidth * scale;
        const float rowPadX = BaseRowPadX * scale;
        const float rowPadY = BaseRowPadY * scale;
        const float rowRight = width - padding - (m_maxScroll > 0.5f ? BaseScrollbarReserveWidth * scale : 0.0f);
        const float textOriginX = padding + indexColWidth + rowPadX * 0.5f;

        const bool activeSelection = hasSelection();
        const TextPosition selStart = (m_selAnchor < m_selCaret) ? m_selAnchor : m_selCaret;
        const TextPosition selEnd = (m_selAnchor < m_selCaret) ? m_selCaret : m_selAnchor;
        const float baseBtnAlpha = m_isDarkTheme ? 0.08f : 0.055f;

        for (int i = 0; i < static_cast<int>(m_lines.size()); ++i) {
            const auto& item = m_lines[i];
            const float rowTop = header + item.topY - m_scrollY;
            const float rowBottom = rowTop + item.height;
            if (rowBottom < viewport.top || rowTop > viewport.bottom) continue;

            const D2D1_RECT_F rowRect = D2D1::RectF(padding, rowTop, rowRight, rowBottom);
            const auto roundedRow = D2D1::RoundedRect(rowRect, 6.0f * scale, 6.0f * scale);

            if (m_copiedTime && m_copiedLineIndex == i && m_copySucceeded) {
                m_brushSuccess->SetOpacity(0.24f);
                m_renderTarget->FillRoundedRectangle(&roundedRow, m_brushSuccess.Get());
                m_brushSuccess->SetOpacity(0.88f);
            } else if (m_hoverLineIndex == i && !m_isSelectingText) {
                m_renderTarget->FillRoundedRectangle(&roundedRow, m_brushBtn.Get());
            } else if (i % 2 == 1) {
                m_brushBtn->SetOpacity(0.42f);
                m_renderTarget->FillRoundedRectangle(&roundedRow, m_brushBtn.Get());
                m_brushBtn->SetOpacity(1.0f);
                (void)baseBtnAlpha;
            }

            // 行号标签（点击可快捷复制本行）
            wchar_t idxBuf[16]{};
            swprintf_s(idxBuf, L"%02d", i + 1);
            m_renderTarget->DrawTextW(
                idxBuf, static_cast<UINT32>(wcslen(idxBuf)), m_hintFormat.Get(),
                D2D1::RectF(padding + 6.0f * scale, rowTop,
                            padding + indexColWidth, rowTop + rowPadY * 2.0f + OcrResultStyle::BaseBodyFont * scale * 1.5f),
                m_brushMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            const float textOriginY = rowTop + rowPadY;

            // 字符级强调色半透明选区高亮
            if (activeSelection && item.layout && i >= selStart.line && i <= selEnd.line) {
                const UINT32 subStart = (i == selStart.line) ? selStart.offset : 0u;
                const UINT32 subEnd = (i == selEnd.line)
                    ? (std::min)(selEnd.offset, static_cast<UINT32>(item.text.size()))
                    : static_cast<UINT32>(item.text.size());
                if (subEnd > subStart) {
                    UINT32 actualCount = 0;
                    item.layout->HitTestTextRange(
                        subStart, subEnd - subStart, textOriginX, textOriginY,
                        nullptr, 0, &actualCount);
                    if (actualCount > 0) {
                        std::vector<DWRITE_HIT_TEST_METRICS> hitRanges(actualCount);
                        if (SUCCEEDED(item.layout->HitTestTextRange(
                                subStart, subEnd - subStart, textOriginX, textOriginY,
                                hitRanges.data(), actualCount, &actualCount))) {
                            for (UINT32 rIdx = 0; rIdx < actualCount; ++rIdx) {
                                const auto& hm = hitRanges[rIdx];
                                if (hm.width <= 0.0f || hm.height <= 0.0f) continue;
                                const auto selBox = D2D1::RoundedRect(
                                    D2D1::RectF(hm.left, hm.top, hm.left + hm.width, hm.top + hm.height),
                                    3.0f * scale, 3.0f * scale);
                                m_renderTarget->FillRoundedRectangle(&selBox, m_brushSelection.Get());
                            }
                        }
                    }
                }
            }

            if (item.layout) {
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(textOriginX, textOriginY),
                    item.layout.Get(), m_brushText.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }

            // 文本微调编辑插入符（Caret）
            if (m_hasCaret && !activeSelection && m_selCaret.line == i && item.layout && m_brushAccent) {
                FLOAT cx = 0.0f;
                FLOAT cy = 0.0f;
                DWRITE_HIT_TEST_METRICS cm{};
                const UINT32 safeOff = (std::min)(m_selCaret.offset, static_cast<UINT32>(item.text.size()));
                if (SUCCEEDED(item.layout->HitTestTextPosition(safeOff, FALSE, &cx, &cy, &cm))) {
                    const float lineX = textOriginX + cx;
                    const float lineTop = textOriginY + cy + 1.0f * scale;
                    const float lineH = (cm.height > 2.0f) ? cm.height : (OcrResultStyle::BaseBodyFont * scale * 1.35f);
                    m_renderTarget->DrawLine(
                        D2D1::Point2F(lineX, lineTop),
                        D2D1::Point2F(lineX, lineTop + lineH - 2.0f * scale),
                        m_brushAccent.Get(), 1.8f * scale);
                }
            }

            // 行右侧精致矢量复制小图标按钮（悬停该行时清晰呈现）
            if (m_hoverLineIndex == i || m_hoverLineCopyBtn == i) {
                const D2D1_RECT_F iconBtn = computeLineCopyButtonRect(i, width);
                if (m_hoverLineCopyBtn == i) {
                    const auto btnBg = D2D1::RoundedRect(iconBtn, 4.5f * scale, 4.5f * scale);
                    m_renderTarget->FillRoundedRectangle(&btnBg, m_brushBtnHover.Get());
                }
                const float cx = (iconBtn.left + iconBtn.right) * 0.5f;
                const float cy = (iconBtn.top + iconBtn.bottom) * 0.5f;
                const float boxW = 8.0f * scale;
                const float boxH = 9.5f * scale;
                ID2D1SolidColorBrush* iconStroke =
                    (m_hoverLineCopyBtn == i) ? m_brushText.Get() : m_brushMuted.Get();
                const auto backPage = D2D1::RoundedRect(
                    D2D1::RectF(cx - boxW * 0.55f, cy - boxH * 0.55f,
                                cx + boxW * 0.35f, cy + boxH * 0.35f),
                    1.5f * scale, 1.5f * scale);
                const auto frontPage = D2D1::RoundedRect(
                    D2D1::RectF(cx - boxW * 0.25f, cy - boxH * 0.25f,
                                cx + boxW * 0.65f, cy + boxH * 0.65f),
                    1.5f * scale, 1.5f * scale);
                m_renderTarget->DrawRoundedRectangle(&backPage, iconStroke, 1.2f * scale);
                m_renderTarget->FillRoundedRectangle(&frontPage, m_brushBg.Get());
                m_renderTarget->DrawRoundedRectangle(&frontPage, iconStroke, 1.2f * scale);
            }
        }

        // 右侧独立滚动条轨道与滑块
        D2D1_RECT_F trackRect{};
        D2D1_RECT_F thumbRect{};
        if (computeScrollbarMetrics(width, height, trackRect, thumbRect)) {
            const float thumbRadius = (thumbRect.right - thumbRect.left) * 0.5f;
            const auto thumbRounded = D2D1::RoundedRect(thumbRect, thumbRadius, thumbRadius);
            if (m_hoverScrollbar || m_draggingScrollbar) {
                m_brushText->SetOpacity(0.45f);
                m_renderTarget->FillRoundedRectangle(&thumbRounded, m_brushText.Get());
                m_brushText->SetOpacity(1.0f);
            } else {
                m_renderTarget->FillRoundedRectangle(&thumbRounded, m_brushBtnHover.Get());
            }
        }

        m_renderTarget->PopAxisAlignedClip();
    }

    const float buttonRadius = 7.0f * scale;
    const bool hasSel = hasSelection();
    ID2D1SolidColorBrush* copyBrush = m_hoverCopy ? m_brushBtnHover.Get() : m_brushAccentSubtle.Get();
    std::wstring copyLabel = hasSel
        ? (chinese ? L"复制选区" : L"Copy Selected")
        : (chinese ? L"复制全文" : L"Copy All");
    bool isCopyFeedback = false;
    if (m_copiedTime && m_copiedLineIndex < 0 && !m_copiedWord) {
        isCopyFeedback = true;
        if (m_copySucceeded) {
            copyBrush = m_brushSuccess.Get();
            copyLabel = m_copiedSelection
                ? (chinese ? L"已复制选区" : L"Copied")
                : (chinese ? L"已复制全文" : L"Copied All");
        } else {
            copyBrush = m_brushError.Get();
            copyLabel = chinese ? L"复制失败" : L"Copy Failed";
        }
    }
    const auto copyButton = D2D1::RoundedRect(
        m_btnCopyRect, buttonRadius, buttonRadius);
    const auto closeButton = D2D1::RoundedRect(
        m_btnCloseRect, buttonRadius, buttonRadius);
    m_renderTarget->FillRoundedRectangle(&copyButton, copyBrush);
    if (!isCopyFeedback && m_brushAccent) {
        m_brushAccent->SetOpacity(m_hoverCopy ? 0.85f : 0.48f);
        m_renderTarget->DrawRoundedRectangle(&copyButton, m_brushAccent.Get(), 1.0f * scale);
        m_brushAccent->SetOpacity(1.0f);
    }
    m_renderTarget->FillRoundedRectangle(
        &closeButton, m_hoverClose ? m_brushBtnHover.Get() : m_brushBtn.Get());

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> whiteTextBrush;
    if (isCopyFeedback) {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &whiteTextBrush);
    }
    m_renderTarget->DrawTextW(
        copyLabel.c_str(), static_cast<UINT32>(copyLabel.size()),
        m_buttonFormat.Get(), m_btnCopyRect,
        (isCopyFeedback && whiteTextBrush) ? whiteTextBrush.Get() : m_brushText.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const std::wstring closeLabel = chinese ? L"关闭" : L"Close";
    m_renderTarget->DrawTextW(
        closeLabel.c_str(), static_cast<UINT32>(closeLabel.size()),
        m_buttonFormat.Get(), m_btnCloseRect, m_brushText.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);

    const float hintLeft = padding;
    const float hintRight = (std::max)(hintLeft + 10.0f, m_btnCopyRect.left - buttonGap);
    const float hintAvailW = hintRight - hintLeft;
    const wchar_t* hint = footerHintText(chinese, false);
    if (m_dwriteFactory && m_hintFormat) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> hintLayout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                hint, static_cast<UINT32>(wcslen(hint)),
                m_hintFormat.Get(), 4000.0f, buttonHeight, &hintLayout)) && hintLayout) {
            DWRITE_TEXT_METRICS hm{};
            if (SUCCEEDED(hintLayout->GetMetrics(&hm)) && hm.width > hintAvailW) {
                hint = footerHintText(chinese, true);
            }
        }
    }
    m_renderTarget->DrawTextW(
        hint, static_cast<UINT32>(wcslen(hint)), m_hintFormat.Get(),
        D2D1::RectF(hintLeft, height - padding - buttonHeight,
                    hintRight, height - padding),
        m_brushMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    const HRESULT endResult = m_renderTarget->EndDraw();
    if (endResult == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
        if (retryDepth < 1 && createResources()) {
            rebuildTextLayout();
            render(retryDepth + 1);
        }
        return;
    }
    if (FAILED(endResult)) return;

    POINT source{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0,
                        static_cast<BYTE>(std::clamp(m_currentAlpha, 0.0f, 255.0f)),
                        AC_SRC_ALPHA};
    UpdateLayeredWindow(m_hwnd, m_screenDc, nullptr, &size, m_memoryDc,
                        &source, 0, &blend, ULW_ALPHA);
}

LRESULT CALLBACK OcrResultWindow::wndProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    OcrResultWindow* self = reinterpret_cast<OcrResultWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OcrResultWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

    if (message == WM_SYSCOMMAND) {
        const WPARAM cmd = wParam & 0xFFF0;
        if (cmd == SC_KEYMENU || cmd == SC_CONTEXTHELP) return 0;
    } else if (message == WM_HELP) {
        return 0;
    }

    switch (message) {
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (mmi) {
                const float safeScale = (self->m_scale > 0.0f) ? self->m_scale : 1.0f;
                mmi->ptMinTrackSize.x = tools3000::core::dpi::scaleMetric(OcrResultStyle::BaseMinWidth, safeScale);
                mmi->ptMinTrackSize.y = tools3000::core::dpi::scaleMetric(OcrResultStyle::BaseMinHeight, safeScale);
            }
            return 0;
        }
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);

            // 交互按钮最高优先级放行，绝不被窗口边缘缩放热区或标题栏拖拽拦截
            if (contains(self->m_btnTopCloseRect, point) ||
                contains(self->m_btnMergeRect, point) ||
                contains(self->m_btnCopyRect, point) ||
                contains(self->m_btnCloseRect, point)) {
                return HTCLIENT;
            }

            RECT client{};
            GetClientRect(hwnd, &client);
            const int cw = client.right - client.left;
            const int ch = client.bottom - client.top;
            const int grip = (std::max)(10, static_cast<int>(std::lround((BaseShadowInset + 6.0f) * self->m_scale)));

            const bool onLeft = (point.x < grip);
            const bool onRight = (point.x >= cw - grip);
            const bool onTop = (point.y < grip);
            const bool onBottom = (point.y >= ch - grip);
            if (onTop && onLeft) return HTTOPLEFT;
            if (onTop && onRight) return HTTOPRIGHT;
            if (onBottom && onLeft) return HTBOTTOMLEFT;
            if (onBottom && onRight) return HTBOTTOMRIGHT;
            if (onLeft) return HTLEFT;
            if (onRight) return HTRIGHT;
            if (onTop) return HTTOP;
            if (onBottom) return HTBOTTOM;

            const float dragBottom = OcrResultStyle::BaseHeaderHeight * self->m_scale;
            return point.y < dragBottom ? HTCAPTION : HTCLIENT;
        }
        case WM_CLOSE: {
            self->hide();
            return 0;
        }
        case WM_SYSKEYDOWN: {
            if (wParam == VK_F4) {
                self->hide();
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        case WM_SIZE: {
            if (!self->m_updatingPlacement && wParam != SIZE_MINIMIZED) {
                self->rebuildTextLayout();
                if (IsWindowVisible(hwnd)) {
                    self->render();
                }
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        case WM_LBUTTONDBLCLK: {
            SetFocus(hwnd);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(hwnd, &client);
            const int clientW = client.right - client.left;
            const int clientH = client.bottom - client.top;
            const float padding = OcrResultStyle::BasePadding * self->m_scale;
            const float header = OcrResultStyle::BaseHeaderHeight * self->m_scale;
            const float footer = OcrResultStyle::BaseFooterHeight * self->m_scale;
            if (point.x >= padding + BaseIndexColWidth * self->m_scale &&
                point.x <= clientW - padding &&
                point.y >= header && point.y <= clientH - footer &&
                !self->m_lines.empty()) {
                const TextPosition hitPos = self->hitTestTextPosition(point);
                self->selectAndCopyWordAt(hitPos);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(hwnd, &client);
            const int clientW = client.right - client.left;
            const int clientH = client.bottom - client.top;

            // 0. 顶栏关闭按钮 (×) 与“合并段落”切换胶囊按钮
            if (contains(self->m_btnTopCloseRect, point)) {
                self->m_pressedTopCloseBtn = true;
                SetCapture(hwnd);
                return 0;
            }
            if (contains(self->m_btnMergeRect, point)) {
                self->m_pressedMergeBtn = true;
                SetCapture(hwnd);
                return 0;
            }

            // 1. 优先命中右侧滚动条轨道/滑块：彻底隔离滚动条交互，防止误触行复制
            D2D1_RECT_F trackRect{};
            D2D1_RECT_F thumbRect{};
            if (self->computeScrollbarMetrics(clientW, clientH, trackRect, thumbRect) &&
                contains(trackRect, point)) {
                self->m_draggingScrollbar = true;
                SetCapture(hwnd);
                const float vpH = trackRect.bottom - trackRect.top;
                const float thumbH = thumbRect.bottom - thumbRect.top;
                if (point.y >= thumbRect.top && point.y <= thumbRect.bottom) {
                    self->m_scrollbarDragOffset = static_cast<float>(point.y) - thumbRect.top;
                } else {
                    self->m_scrollbarDragOffset = thumbH * 0.5f;
                    const float ratio = std::clamp(
                        (static_cast<float>(point.y) - trackRect.top - self->m_scrollbarDragOffset) /
                            (std::max)(1.0f, vpH - thumbH),
                        0.0f, 1.0f);
                    self->m_scrollY = ratio * self->m_maxScroll;
                }
                self->render();
                return 0;
            }

            // 2. 底部复制与关闭按钮
            if (contains(self->m_btnCopyRect, point)) {
                self->m_pressedCopyBtn = true;
                SetCapture(hwnd);
                return 0;
            }
            if (contains(self->m_btnCloseRect, point)) {
                self->m_pressedCloseBtn = true;
                SetCapture(hwnd);
                return 0;
            }

            // 3. 行右侧单行复制小图标按钮或左侧行号快捷复制
            const float padding = OcrResultStyle::BasePadding * self->m_scale;
            const float header = OcrResultStyle::BaseHeaderHeight * self->m_scale;
            const float footer = OcrResultStyle::BaseFooterHeight * self->m_scale;
            if (point.x >= padding && point.x <= clientW - padding &&
                point.y >= header && point.y <= clientH - footer &&
                !self->m_lines.empty()) {
                const float relY = static_cast<float>(point.y) - header + self->m_scrollY;
                for (int i = 0; i < static_cast<int>(self->m_lines.size()); ++i) {
                    if (relY >= self->m_lines[i].topY &&
                        relY <= self->m_lines[i].topY + self->m_lines[i].height) {
                        if (contains(self->computeLineCopyButtonRect(i, clientW), point) ||
                            point.x < padding + BaseIndexColWidth * self->m_scale) {
                            self->m_pressedLineCopyBtn = i;
                            SetCapture(hwnd);
                            return 0;
                        }
                        break;
                    }
                }

                // 4. 文本正文区：定位微调编辑光标或进入字符级拖选
                self->m_mouseDownPt = point;
                self->m_hadSelectionOnMouseDown = self->hasSelection();
                const TextPosition hitPos = self->hitTestTextPosition(point);
                if ((wParam & MK_SHIFT) != 0 && self->m_selAnchor.isValid()) {
                    self->m_selCaret = hitPos;
                } else {
                    self->m_selAnchor = hitPos;
                    self->m_selCaret = hitPos;
                }
                self->m_hasCaret = true;
                self->m_isSelectingText = true;
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                self->render();
                return 0;
            }

            if (self->hasSelection() || self->m_hasCaret) {
                self->clearSelection();
                self->render();
            }
            if (point.y >= 0 && point.y < header) {
                self->m_draggingWindow = true;
                GetCursorPos(&self->m_dragStartScreenPt);
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                self->m_windowStartPos = {wr.left, wr.top};
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!self->m_trackingMouse) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
                self->m_trackingMouse = TrackMouseEvent(&tracking) != FALSE;
            }
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if (self->m_draggingWindow) {
                POINT curScreen{};
                GetCursorPos(&curScreen);
                SetWindowPos(
                    hwnd, nullptr,
                    self->m_windowStartPos.x + (curScreen.x - self->m_dragStartScreenPt.x),
                    self->m_windowStartPos.y + (curScreen.y - self->m_dragStartScreenPt.y),
                    0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return 0;
            }

            if (self->m_draggingScrollbar) {
                RECT client{};
                GetClientRect(hwnd, &client);
                D2D1_RECT_F trackRect{};
                D2D1_RECT_F thumbRect{};
                if (self->computeScrollbarMetrics(
                        client.right - client.left, client.bottom - client.top,
                        trackRect, thumbRect)) {
                    const float vpH = trackRect.bottom - trackRect.top;
                    const float thumbH = thumbRect.bottom - thumbRect.top;
                    const float ratio = std::clamp(
                        (static_cast<float>(point.y) - trackRect.top - self->m_scrollbarDragOffset) /
                            (std::max)(1.0f, vpH - thumbH),
                        0.0f, 1.0f);
                    self->m_scrollY = ratio * self->m_maxScroll;
                    self->render();
                }
                return 0;
            }

            if (self->m_isSelectingText) {
                RECT client{};
                GetClientRect(hwnd, &client);
                const float header = OcrResultStyle::BaseHeaderHeight * self->m_scale;
                const float footer = OcrResultStyle::BaseFooterHeight * self->m_scale;
                const float vpBottom = static_cast<float>(client.bottom - client.top) - footer;
                if (self->m_maxScroll > 0.5f) {
                    if (point.y < header) {
                        self->m_scrollY = std::clamp(
                            self->m_scrollY - (header - point.y) * 0.35f, 0.0f, self->m_maxScroll);
                    } else if (point.y > vpBottom) {
                        self->m_scrollY = std::clamp(
                            self->m_scrollY + (point.y - vpBottom) * 0.35f, 0.0f, self->m_maxScroll);
                    }
                }
                self->m_selCaret = self->hitTestTextPosition(point);
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                self->render();
                return 0;
            }

            self->updateHover(point);
            return 0;
        }
        case WM_MOUSELEAVE:
            self->m_trackingMouse = false;
            self->m_hoverTopClose = false;
            self->m_hoverMerge = false;
            self->m_hoverCopy = false;
            self->m_hoverClose = false;
            self->m_hoverScrollbar = false;
            self->m_hoverLineCopyBtn = -1;
            self->m_hoverLineIndex = -1;
            self->render();
            return 0;
        case WM_LBUTTONUP: {
            // 关键：ReleaseCapture() 会同步派发 WM_CAPTURECHANGED 清空全部按压态，
            // 因此必须在释放捕获之前先行快照按压状态，否则按钮点击 100% 被吞噬失效！
            const bool wasDraggingWindow = self->m_draggingWindow;
            const bool wasDraggingScrollbar = self->m_draggingScrollbar;
            const bool wasPressedTopClose = self->m_pressedTopCloseBtn;
            const bool wasPressedMerge = self->m_pressedMergeBtn;
            const bool wasPressedCopy = self->m_pressedCopyBtn;
            const bool wasPressedClose = self->m_pressedCloseBtn;
            const int wasPressedLineCopy = self->m_pressedLineCopyBtn;
            const bool wasSelectingText = self->m_isSelectingText;

            self->m_draggingWindow = false;
            self->m_draggingScrollbar = false;
            self->m_pressedTopCloseBtn = false;
            self->m_pressedMergeBtn = false;
            self->m_pressedCopyBtn = false;
            self->m_pressedCloseBtn = false;
            self->m_pressedLineCopyBtn = -1;
            self->m_isSelectingText = false;

            if (GetCapture() == hwnd) ReleaseCapture();
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const float clickSlop = 8.0f * self->m_scale;

            if (wasDraggingWindow) {
                self->updateHover(point);
                return 0;
            }

            if (wasDraggingScrollbar) {
                self->updateHover(point);
                self->render();
                return 0;
            }

            if (wasPressedTopClose) {
                if (containsExpanded(self->m_btnTopCloseRect, point, clickSlop)) {
                    self->hide();
                }
                return 0;
            }
            if (wasPressedMerge) {
                if (containsExpanded(self->m_btnMergeRect, point, clickSlop)) {
                    self->setMergeParagraphs(!self->isMergeParagraphsEnabled());
                }
                return 0;
            }
            if (wasPressedCopy) {
                if (containsExpanded(self->m_btnCopyRect, point, clickSlop)) self->copySelectionOrAll();
                return 0;
            }
            if (wasPressedClose) {
                if (containsExpanded(self->m_btnCloseRect, point, clickSlop)) self->hide();
                return 0;
            }
            if (wasPressedLineCopy >= 0) {
                RECT client{};
                GetClientRect(hwnd, &client);
                const float padding = OcrResultStyle::BasePadding * self->m_scale;
                if (contains(self->computeLineCopyButtonRect(wasPressedLineCopy, client.right - client.left), point) ||
                    point.x < padding + BaseIndexColWidth * self->m_scale) {
                    self->copyLine(wasPressedLineCopy);
                }
                return 0;
            }

            if (wasSelectingText) {
                self->m_selCaret = self->hitTestTextPosition(point);
                self->m_hasCaret = true;
                self->render();
                return 0;
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            self->m_draggingWindow = false;
            self->m_draggingScrollbar = false;
            self->m_isSelectingText = false;
            self->m_pressedTopCloseBtn = false;
            self->m_pressedMergeBtn = false;
            self->m_pressedCopyBtn = false;
            self->m_pressedCloseBtn = false;
            self->m_pressedLineCopyBtn = -1;
            return 0;
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const float amount = OcrResultStyle::BaseLineScroll * self->m_scale;
            self->m_scrollY = std::clamp(
                self->m_scrollY - (static_cast<float>(delta) / WHEEL_DELTA) * amount,
                0.0f, self->m_maxScroll);
            self->render();
            return 0;
        }
        case WM_CHAR: {
            const wchar_t ch = static_cast<wchar_t>(wParam);
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (!ctrl && ch >= 0x20 && ch != 0x7F && (self->m_hasCaret || self->hasSelection())) {
                self->insertTextAtCaret(std::wstring(1, ch));
                return 0;
            }
            return 0;
        }
        case WM_KEYDOWN: {
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wParam == VK_ESCAPE) {
                // 若存在显式拖选高亮，首次 Esc 一步清除选区与光标；无选区时（即便曾点过正文）单次 Esc 立即关闭窗口
                if (self->hasSelection()) {
                    self->clearSelection();
                    self->render();
                } else {
                    self->hide();
                }
            } else if (control && wParam == 'W') {
                self->hide();
            } else if (control && wParam == 'C') {
                self->copySelectionOrAll();
            } else if (control && wParam == 'X') {
                if (self->hasSelection()) {
                    self->copySelectionOrAll();
                    self->deleteBackwardAtCaret();
                }
            } else if (control && wParam == 'V') {
                const std::wstring clip = readClipboardWideText(hwnd);
                if (!clip.empty()) {
                    self->insertTextAtCaret(clip);
                }
            } else if (control && wParam == 'Z') {
                self->undoEdit();
            } else if (control && wParam == 'M') {
                self->setMergeParagraphs(!self->isMergeParagraphsEnabled());
            } else if (control && wParam == 'A') {
                self->selectAll();
            } else if (!control && wParam == VK_BACK && (self->m_hasCaret || self->hasSelection())) {
                self->deleteBackwardAtCaret();
            } else if (!control && wParam == VK_DELETE && (self->m_hasCaret || self->hasSelection())) {
                self->deleteForwardAtCaret();
            } else if (!control && wParam == VK_RETURN && (self->m_hasCaret || self->hasSelection())) {
                self->insertTextAtCaret(L"\n");
            } else if (!control && (wParam == VK_LEFT || wParam == VK_RIGHT) &&
                       self->m_selCaret.isValid() && !self->m_lines.empty()) {
                TextPosition nextPos = self->m_selCaret;
                nextPos.line = std::clamp(nextPos.line, 0, static_cast<int>(self->m_lines.size()) - 1);
                nextPos.offset = (std::min)(nextPos.offset, static_cast<UINT32>(self->m_lines[nextPos.line].text.size()));
                if (wParam == VK_LEFT) {
                    if (!shift && self->hasSelection()) {
                        nextPos = (self->m_selAnchor < self->m_selCaret) ? self->m_selAnchor : self->m_selCaret;
                    } else if (nextPos.offset > 0) {
                        --nextPos.offset;
                    } else if (nextPos.line > 0) {
                        --nextPos.line;
                        nextPos.offset = static_cast<UINT32>(self->m_lines[nextPos.line].text.size());
                    }
                } else {
                    if (!shift && self->hasSelection()) {
                        nextPos = (self->m_selAnchor < self->m_selCaret) ? self->m_selCaret : self->m_selAnchor;
                    } else if (nextPos.offset < self->m_lines[nextPos.line].text.size()) {
                        ++nextPos.offset;
                    } else if (nextPos.line + 1 < static_cast<int>(self->m_lines.size())) {
                        ++nextPos.line;
                        nextPos.offset = 0;
                    }
                }
                self->m_selCaret = nextPos;
                if (!shift) self->m_selAnchor = nextPos;
                self->m_hasCaret = true;
                self->render();
            } else {
                const float viewport = (std::max)(
                    1.0f, static_cast<float>(self->m_surfaceHeight) -
                                  (OcrResultStyle::BaseHeaderHeight +
                                   OcrResultStyle::BaseFooterHeight) * self->m_scale);
                const float lineStep = OcrResultStyle::BaseLineScroll * self->m_scale;
                float next = self->m_scrollY;
                if (wParam == VK_UP) next -= lineStep;
                else if (wParam == VK_DOWN) next += lineStep;
                else if (wParam == VK_PRIOR) next -= viewport * 0.9f;
                else if (wParam == VK_NEXT) next += viewport * 0.9f;
                else if (wParam == VK_HOME) next = 0.0f;
                else if (wParam == VK_END) next = self->m_maxScroll;
                else return 0;
                self->m_scrollY = std::clamp(next, 0.0f, self->m_maxScroll);
                self->render();
            }
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            self->reloadTheme();
            return 0;
        case WM_TIMER:
            if (wParam == FadeTimerId) {
                const float elapsed = static_cast<float>(
                    GetTickCount64() - self->m_showTime);
                const float progress = std::clamp(
                    elapsed / static_cast<float>(FadeDurationMs), 0.0f, 1.0f);
                self->m_currentAlpha = 255.0f *
                    (1.0f - std::pow(1.0f - progress, 3.0f));
                self->render();
                if (progress >= 1.0f) KillTimer(hwnd, FadeTimerId);
            } else if (wParam == CopyStateTimerId) {
                KillTimer(hwnd, CopyStateTimerId);
                self->m_copiedTime = 0;
                self->render();
            } else if (wParam == LoadingAnimTimerId) {
                if (self->m_isLoadingState && IsWindowVisible(hwnd)) {
                    self->m_loadingSpinAngle += 0.24f;
                    if (self->m_loadingSpinAngle > 6.2831853f) {
                        self->m_loadingSpinAngle -= 6.2831853f;
                    }
                    self->render();
                } else {
                    KillTimer(hwnd, LoadingAnimTimerId);
                }
            }
            return 0;
        case WM_DPICHANGED: {
            // 跨不同缩放率显示器拖拽时严格遵循系统 lParam 建议矩形，绝不重调 updatePlacement() 飞回屏幕中央
            const unsigned newDpi = HIWORD(wParam);
            const float newScale = tools3000::core::dpi::scaleForDpi(newDpi);
            const bool scaleChanged = std::abs(self->m_scale - newScale) > 0.001f;
            self->m_scale = newScale;
            if (scaleChanged) {
                self->createTextResources(newScale);
            }
            if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
                self->m_updatingPlacement = true;
                SetWindowPos(
                    hwnd, nullptr,
                    suggested->left, suggested->top,
                    (std::max)(1L, suggested->right - suggested->left),
                    (std::max)(1L, suggested->bottom - suggested->top),
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                self->m_updatingPlacement = false;
            }
            self->rebuildTextLayout();
            if (IsWindowVisible(hwnd)) {
                self->render();
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            if (!self->m_updatingPlacement && IsWindowVisible(hwnd)) {
                self->updatePlacement();
                self->render();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, FadeTimerId);
            KillTimer(hwnd, CopyStateTimerId);
            KillTimer(hwnd, LoadingAnimTimerId);
            self->releaseSurface();
            if (self->m_hwnd == hwnd) self->m_hwnd = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

}  // namespace tools3000::ocr

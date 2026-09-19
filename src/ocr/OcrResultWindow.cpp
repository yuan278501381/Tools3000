#include "OcrResultWindow.h"

#include "OcrResultStyle.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
constexpr UINT FadeIntervalMs = 16;
constexpr UINT FadeDurationMs = 150;
constexpr UINT CopyStateDurationMs = 2000;
constexpr float BaseIndexColWidth = 30.0f;
constexpr float BaseRowPadX = 10.0f;
constexpr float BaseRowPadY = 5.0f;
constexpr float BaseRowGap = 4.0f;
constexpr float BaseCopyIconAreaWidth = 26.0f;
constexpr float BaseScrollbarReserveWidth = 12.0f;

bool contains(const D2D1_RECT_F& rect, POINT point) noexcept {
    return point.x >= rect.left && point.x <= rect.right &&
           point.y >= rect.top && point.y <= rect.bottom;
}

const wchar_t* footerHintText(bool chinese, bool compact) noexcept {
    if (chinese) {
        return compact
            ? L"拖选文字 · 轻点复制行 · Esc 关闭"
            : L"拖选文字 · 轻点复制行 · Ctrl+C/A · Esc";
    }
    return compact
        ? L"Drag select · Tap copy · Esc"
        : L"Drag to select · Tap row to copy · Ctrl+C/A · Esc";
}

}  // namespace

bool OcrResultWindow::initialize() {
    if (m_hwnd && IsWindow(m_hwnd)) return true;

    m_instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
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
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
    m_text.clear();
    m_wideText.clear();
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
    const wchar_t* localeName = tools3000::core::isUiLanguageChinese() ? L"zh-CN" : L"en-US";
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

void OcrResultWindow::discardDeviceResources() {
    m_brushBg.Reset();
    m_brushText.Reset();
    m_brushMuted.Reset();
    m_brushBorder.Reset();
    m_brushBtn.Reset();
    m_brushBtnHover.Reset();
    m_brushSuccess.Reset();
    m_brushError.Reset();
    m_brushSelection.Reset();
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

    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
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
    const int x = workArea.left + (workArea.right - workArea.left - size.cx) / 2;
    const int y = workArea.top + (workArea.bottom - workArea.top - size.cy) / 2;

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

    // 按行切分并为每一行创建独立 IDWriteTextLayout，同时支持字符级拖选与轻点/图标复制单行
    size_t start = 0;
    float curY = 0.0f;
    while (start <= m_wideText.size()) {
        size_t end = m_wideText.find(L'\n', start);
        std::wstring lineStr = (end == std::wstring::npos)
            ? m_wideText.substr(start)
            : m_wideText.substr(start, end - start);
        if (!lineStr.empty() && lineStr.back() == L'\r') lineStr.pop_back();

        if (!lineStr.empty()) {
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
    m_isSelectingText = false;
}

void OcrResultWindow::copySelectionOrAll() {
    if (hasSelection()) {
        const std::string selUtf8 = selectedTextUtf8();
        if (selUtf8.empty()) return;
        m_copySucceeded = tools3000::core::WinUtils::copyToClipboard(selUtf8);
        m_copiedLineIndex = -1;
        m_copiedSelection = true;
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

void OcrResultWindow::showResult(const std::string& text) {
    auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
    if (m_hwnd && GetWindowThreadProcessId(m_hwnd, nullptr) != GetCurrentThreadId()) {
        dispatcher.post([this, text]() { showResult(text); });
        return;
    }
    if (!m_hwnd && dispatcher.isInitialized() && !dispatcher.isOwnerThread()) {
        dispatcher.post([this, text]() { showResult(text); });
        return;
    }

    if ((!m_hwnd || !IsWindow(m_hwnd)) && !initialize()) return;

    m_text = text;
    m_wideText = tools3000::core::WinUtils::utf8ToWstring(text);
    m_scrollY = 0.0f;
    m_maxScroll = 0.0f;
    m_totalContentHeight = 0.0f;
    m_copiedTime = 0;
    m_copySucceeded = false;
    m_copiedSelection = false;
    m_hoverCopy = false;
    m_hoverClose = false;
    m_hoverScrollbar = false;
    m_draggingScrollbar = false;
    m_hoverLineCopyBtn = -1;
    m_pressedLineCopyBtn = -1;
    m_pressedCopyBtn = false;
    m_pressedCloseBtn = false;
    m_hoverLineIndex = -1;
    m_copiedLineIndex = -1;
    clearSelection();
    m_showTime = GetTickCount64();
    m_currentAlpha = 0.0f;
    KillTimer(m_hwnd, CopyStateTimerId);

    updatePlacement();
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    SetTimer(m_hwnd, FadeTimerId, FadeIntervalMs, nullptr);
    render();
}

void OcrResultWindow::copyAll() {
    if (m_text.empty()) return;
    m_copySucceeded = tools3000::core::WinUtils::copyToClipboard(m_text);
    m_copiedLineIndex = -1;
    m_copiedSelection = false;
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
    m_copiedTime = GetTickCount64();
    if (m_hwnd) SetTimer(m_hwnd, CopyStateTimerId, CopyStateDurationMs, nullptr);
    render();
}

void OcrResultWindow::hide() {
    if (!m_hwnd) return;
    if (GetCapture() == m_hwnd) ReleaseCapture();
    KillTimer(m_hwnd, FadeTimerId);
    KillTimer(m_hwnd, CopyStateTimerId);
    ShowWindow(m_hwnd, SW_HIDE);
    releaseSurface();
    m_lines.clear();
    m_textLayout.Reset();
    m_text.clear();
    m_wideText.clear();
    clearSelection();
    m_draggingScrollbar = false;
    tools3000::core::WinUtils::trimWorkingSet();
}

void OcrResultWindow::updateHover(POINT point) {
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
    if (!hoverScrollbar &&
        point.x >= padding && point.x <= width - padding &&
        point.y >= header && point.y <= height - footer) {
        inTextViewport = true;
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

    const LPCWSTR cursorId =
        (hoverCopy || hoverClose || hoverScrollbar || hoverCopyBtn >= 0)
            ? IDC_HAND
            : (inTextViewport && !m_lines.empty() ? IDC_IBEAM : IDC_ARROW);
    SetCursor(LoadCursorW(nullptr, cursorId));

    if (hoverCopy == m_hoverCopy &&
        hoverClose == m_hoverClose &&
        hoverScrollbar == m_hoverScrollbar &&
        hoverLine == m_hoverLineIndex &&
        hoverCopyBtn == m_hoverLineCopyBtn) {
        return;
    }
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
        const auto brush = [&](D2D1_COLOR_F color,
                               Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& output) {
            return SUCCEEDED(m_renderTarget->CreateSolidColorBrush(color, &output));
        };
        if (!brush(D2D1::ColorF(0x0F1219, 0.98f), m_brushBg) ||
            !brush(D2D1::ColorF(0xF8FAFC, 1.0f), m_brushText) ||
            !brush(D2D1::ColorF(0x94A3B8, 0.92f), m_brushMuted) ||
            !brush(D2D1::ColorF(0x6366F1, 0.65f), m_brushBorder) ||
            !brush(D2D1::ColorF(0xFFFFFF, 0.08f), m_brushBtn) ||
            !brush(D2D1::ColorF(0xFFFFFF, 0.16f), m_brushBtnHover) ||
            !brush(D2D1::ColorF(0x10B981, 0.88f), m_brushSuccess) ||
            !brush(D2D1::ColorF(0xEF4444, 0.88f), m_brushError) ||
            !brush(D2D1::ColorF(0x3B82F6, 0.42f), m_brushSelection)) {
            m_brushBg.Reset();
            m_brushText.Reset();
            m_brushMuted.Reset();
            m_brushBorder.Reset();
            m_brushBtn.Reset();
            m_brushBtnHover.Reset();
            m_brushSuccess.Reset();
            m_brushError.Reset();
            m_brushSelection.Reset();
            return;
        }
    }

    const float scale = m_scale;
    const float padding = OcrResultStyle::BasePadding * scale;
    const float header = OcrResultStyle::BaseHeaderHeight * scale;
    const float footer = OcrResultStyle::BaseFooterHeight * scale;
    const float radius = OcrResultStyle::BaseCornerRadius * scale;
    const float border = OcrResultStyle::BaseBorderWidth * scale;
    const float buttonHeight = OcrResultStyle::BaseButtonHeight * scale;
    const float copyWidth = OcrResultStyle::BaseButtonWidth * scale;
    const float closeWidth = OcrResultStyle::BaseCloseButtonWidth * scale;
    const float buttonGap = OcrResultStyle::BaseButtonGap * scale;

    m_btnCloseRect = D2D1::RectF(
        width - padding - closeWidth, height - padding - buttonHeight,
        width - padding, height - padding);
    m_btnCopyRect = D2D1::RectF(
        m_btnCloseRect.left - buttonGap - copyWidth,
        m_btnCloseRect.top, m_btnCloseRect.left - buttonGap,
        m_btnCloseRect.bottom);

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0.0f));
    const auto card = D2D1::RoundedRect(
        D2D1::RectF(border / 2.0f, border / 2.0f,
                    width - border / 2.0f, height - border / 2.0f),
        radius, radius);
    m_renderTarget->FillRoundedRectangle(&card, m_brushBg.Get());
    m_renderTarget->DrawRoundedRectangle(&card, m_brushBorder.Get(), border);

    const bool chinese = tools3000::core::isUiLanguageChinese();
    std::wstring title = chinese
        ? (L"文字识别结果 (" + std::to_wstring(m_lines.size()) + L" 行)")
        : (L"Recognized Text (" + std::to_wstring(m_lines.size()) + L" lines)");
    if (m_copiedTime && m_copySucceeded) {
        if (m_copiedLineIndex >= 0) {
            title += chinese
                ? (L"  ·  已复制第 " + std::to_wstring(m_copiedLineIndex + 1) + L" 行")
                : (L"  ·  Copied line " + std::to_wstring(m_copiedLineIndex + 1));
        } else if (m_copiedSelection) {
            title += chinese ? L"  ·  已复制选中文字" : L"  ·  Copied selection";
        }
    }
    m_renderTarget->DrawTextW(
        title.c_str(), static_cast<UINT32>(title.size()), m_titleFormat.Get(),
        D2D1::RectF(padding, 0.0f, (std::max)(padding, width - padding), header),
        m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    if (!m_lines.empty()) {
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
                m_brushBtn->SetOpacity(0.035f);
                m_renderTarget->FillRoundedRectangle(&roundedRow, m_brushBtn.Get());
                m_brushBtn->SetOpacity(0.08f);
            }

            // 行号标签
            wchar_t idxBuf[16]{};
            swprintf_s(idxBuf, L"%02d", i + 1);
            m_renderTarget->DrawTextW(
                idxBuf, static_cast<UINT32>(wcslen(idxBuf)), m_hintFormat.Get(),
                D2D1::RectF(padding + 6.0f * scale, rowTop,
                            padding + indexColWidth, rowTop + rowPadY * 2.0f + OcrResultStyle::BaseBodyFont * scale * 1.5f),
                m_brushMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            const float textOriginY = rowTop + rowPadY;

            // 字符级蓝色半透明选区高亮
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
    ID2D1SolidColorBrush* copyBrush = m_hoverCopy ? m_brushBtnHover.Get() : m_brushBtn.Get();
    std::wstring copyLabel = hasSel
        ? (chinese ? L"复制选区" : L"Copy Selected")
        : (chinese ? L"复制全文" : L"Copy All");
    if (m_copiedTime && m_copiedLineIndex < 0) {
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
    m_renderTarget->FillRoundedRectangle(
        &closeButton, m_hoverClose ? m_brushBtnHover.Get() : m_brushBtn.Get());
    m_renderTarget->DrawTextW(
        copyLabel.c_str(), static_cast<UINT32>(copyLabel.size()),
        m_buttonFormat.Get(), m_btnCopyRect, m_brushText.Get(),
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
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            const float dragBottom = OcrResultStyle::BaseHeaderHeight * self->m_scale;
            return point.y < dragBottom ? HTCAPTION : HTCLIENT;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(hwnd, &client);
            const int clientW = client.right - client.left;
            const int clientH = client.bottom - client.top;

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

            // 3. 行右侧单行复制小图标按钮
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
                        if (contains(self->computeLineCopyButtonRect(i, clientW), point)) {
                            self->m_pressedLineCopyBtn = i;
                            SetCapture(hwnd);
                            return 0;
                        }
                        break;
                    }
                }

                // 4. 文本正文区：进入字符级划词选字（或轻点复制单行）
                self->m_mouseDownPt = point;
                self->m_hadSelectionOnMouseDown = self->hasSelection();
                const TextPosition hitPos = self->hitTestTextPosition(point);
                if ((wParam & MK_SHIFT) != 0 && self->m_selAnchor.isValid()) {
                    self->m_selCaret = hitPos;
                } else {
                    self->m_selAnchor = hitPos;
                    self->m_selCaret = hitPos;
                }
                self->m_isSelectingText = true;
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                self->render();
                return 0;
            }

            if (self->hasSelection()) {
                self->clearSelection();
                self->render();
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!self->m_trackingMouse) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
                self->m_trackingMouse = TrackMouseEvent(&tracking) != FALSE;
            }
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

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
            self->m_hoverCopy = false;
            self->m_hoverClose = false;
            self->m_hoverScrollbar = false;
            self->m_hoverLineCopyBtn = -1;
            self->m_hoverLineIndex = -1;
            self->render();
            return 0;
        case WM_LBUTTONUP: {
            if (GetCapture() == hwnd) ReleaseCapture();
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if (self->m_draggingScrollbar) {
                self->m_draggingScrollbar = false;
                self->updateHover(point);
                self->render();
                return 0;
            }

            if (self->m_pressedCopyBtn) {
                self->m_pressedCopyBtn = false;
                if (contains(self->m_btnCopyRect, point)) self->copySelectionOrAll();
                return 0;
            }
            if (self->m_pressedCloseBtn) {
                self->m_pressedCloseBtn = false;
                if (contains(self->m_btnCloseRect, point)) self->hide();
                return 0;
            }
            if (self->m_pressedLineCopyBtn >= 0) {
                const int targetLine = self->m_pressedLineCopyBtn;
                self->m_pressedLineCopyBtn = -1;
                RECT client{};
                GetClientRect(hwnd, &client);
                if (contains(self->computeLineCopyButtonRect(targetLine, client.right - client.left), point)) {
                    self->copyLine(targetLine);
                }
                return 0;
            }

            if (self->m_isSelectingText) {
                self->m_selCaret = self->hitTestTextPosition(point);
                self->m_isSelectingText = false;
                const float dx = static_cast<float>(point.x - self->m_mouseDownPt.x);
                const float dy = static_cast<float>(point.y - self->m_mouseDownPt.y);
                const float dragDist = std::hypot(dx, dy);

                // 拖选文字（产生字符选区或拖拽距离 >= 4px）时保留选区高亮，绝不覆盖成整行复制
                if (self->hasSelection() || dragDist >= 4.0f * self->m_scale) {
                    self->render();
                    return 0;
                }

                // 轻点（< 4px 且无字符跨度）：若按下前已有选区则清除选区，否则快捷复制当前行
                if (self->m_hadSelectionOnMouseDown) {
                    self->clearSelection();
                    self->render();
                    return 0;
                }

                RECT client{};
                GetClientRect(hwnd, &client);
                const float padding = OcrResultStyle::BasePadding * self->m_scale;
                const float header = OcrResultStyle::BaseHeaderHeight * self->m_scale;
                const float footer = OcrResultStyle::BaseFooterHeight * self->m_scale;
                if (point.x >= padding && point.x <= (client.right - client.left) - padding &&
                    point.y >= header && point.y <= (client.bottom - client.top) - footer) {
                    const float relY = static_cast<float>(point.y) - header + self->m_scrollY;
                    for (int i = 0; i < static_cast<int>(self->m_lines.size()); ++i) {
                        if (relY >= self->m_lines[i].topY &&
                            relY <= self->m_lines[i].topY + self->m_lines[i].height) {
                            self->copyLine(i);
                            break;
                        }
                    }
                }
                return 0;
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            self->m_draggingScrollbar = false;
            self->m_isSelectingText = false;
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
        case WM_KEYDOWN: {
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wParam == VK_ESCAPE) {
                if (self->hasSelection()) {
                    self->clearSelection();
                    self->render();
                } else {
                    self->hide();
                }
            } else if (control && wParam == 'C') {
                self->copySelectionOrAll();
            } else if (control && wParam == 'A') {
                self->selectAll();
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
            self->releaseSurface();
            if (self->m_hwnd == hwnd) self->m_hwnd = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

}  // namespace tools3000::ocr

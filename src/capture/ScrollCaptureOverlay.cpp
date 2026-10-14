#include "ScrollCaptureOverlay.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")

namespace tools3000::capture {

namespace {
float scrollOverlayDpiScale(const RECT& rect) {
    POINT center{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    return tools3000::core::dpi::scaleAtPoint(center);
}
}  // namespace

bool ScrollCaptureOverlay::initialize() {
    if (m_hwnd) return true;

    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = wndProc;
    wcex.hInstance = GetModuleHandleW(nullptr);
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcex.lpszClassName = L"Tools3000_ScrollCaptureOverlay";

    RegisterClassExW(&wcex);

    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screenW = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    int screenH = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wcex.lpszClassName,
        L"Scroll Capture Overlay",
        WS_POPUP,
        screenX, screenY, screenW, screenH,
        nullptr, nullptr, wcex.hInstance, nullptr
    );

    if (!m_hwnd) return false;

    SetPropW(m_hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));

    // Exclude the preview and progress chrome from the scroll-capture source.
    if (!tools3000::core::WinUtils::excludeWindowFromCapture(m_hwnd)) {
        LOG_WARN("当前 Windows 版本无法从捕获中排除长截图预览: error={}", GetLastError());
    }

    if (!createResources()) return false;
    
    return true;
}

void ScrollCaptureOverlay::hide() {
    if (m_hwnd) {
        KillTimer(m_hwnd, 1);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_brushFlash.Reset();
    m_brushBorder.Reset();
    m_brushBg.Reset();
    m_d2dBitmap.Reset();
    m_renderTarget.Reset();
    m_d2dFactory.Reset();
}

bool ScrollCaptureOverlay::createResources() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) return false;

    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE, D2D1_FEATURE_LEVEL_DEFAULT
    );

    if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &m_renderTarget))) return false;

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &m_brushFlash);
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.8f, 0.2f, 1.0f), &m_brushBorder);
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f, 0.8f), &m_brushBg);

    return true;
}

void ScrollCaptureOverlay::show(RECT captureRect) {
    m_captureRect = captureRect;
    m_dpiScale = scrollOverlayDpiScale(captureRect);
    if (!initialize()) return;
    
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_preview.release();
        m_previewDirty = false;
        m_d2dBitmap.Reset();
        m_frameCount = 0;
        m_lastFlashTime = GetTickCount64();
    }

    UpdateLayeredWindow(m_hwnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, ULW_ALPHA);
    ShowWindow(m_hwnd, SW_SHOWNA);
    
    SetTimer(m_hwnd, 1, 16, nullptr);
    render();
}

void ScrollCaptureOverlay::updatePreview(const cv::Mat& stitched, int frameCount) {
    if (!m_hwnd) return;
    cv::Mat previewBgra;
    if (!stitched.empty()) {
        const int targetWidth = std::max(1, static_cast<int>(std::lround(200.0f * m_dpiScale)));
        const int maxHeight = std::max(
            static_cast<int>(std::lround(120.0f * m_dpiScale)), std::min(
            static_cast<int>(std::lround(480.0f * m_dpiScale)),
            GetSystemMetrics(SM_CYVIRTUALSCREEN) -
                static_cast<int>(std::lround(80.0f * m_dpiScale))));
        const int sourceRows = std::max(1, maxHeight * stitched.cols / targetWidth);
        const int rows = std::min(stitched.rows, sourceRows);
        const cv::Mat recent = stitched(cv::Rect(0, stitched.rows - rows, stitched.cols, rows));
        const double scale = std::min(
            static_cast<double>(targetWidth) / recent.cols,
            static_cast<double>(maxHeight) / recent.rows);
        cv::Mat resized;
        cv::resize(recent, resized, cv::Size(
            std::max(1, static_cast<int>(recent.cols * scale)),
            std::max(1, static_cast<int>(recent.rows * scale))), 0, 0, cv::INTER_AREA);
        cv::cvtColor(resized, previewBgra, cv::COLOR_BGR2BGRA);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_preview = std::move(previewBgra);
        m_frameCount = frameCount;
        m_lastFlashTime = GetTickCount64();
        // D2D resources belong to the window thread. render() consumes this flag
        // and recreates the bitmap there instead of releasing COM state here.
        m_previewDirty = true;
    }
}

void ScrollCaptureOverlay::render() {
    if (!m_hwnd || !m_renderTarget) return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0) return;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMemory = CreateCompatibleDC(hdcScreen);
    
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; 
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    HBITMAP hbmpOld = (HBITMAP)SelectObject(hdcMemory, hbmp);

    RECT bindRect = {0, 0, width, height};
    m_renderTarget->BindDC(hdcMemory, &bindRect);

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    uint64_t elapsed = GetTickCount64() - m_lastFlashTime;
    float flashOpacity = 0.0f;
    if (elapsed < 300) {
        flashOpacity = 1.0f - (elapsed / 300.0f);
    }

    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);

    D2D1_RECT_F cRect = D2D1::RectF(
        (float)(m_captureRect.left - screenX),
        (float)(m_captureRect.top - screenY),
        (float)(m_captureRect.right - screenX),
        (float)(m_captureRect.bottom - screenY)
    );

    // Draw border
    m_renderTarget->DrawRectangle(&cRect, m_brushBorder.Get(), 3.0f * m_dpiScale);

    // Draw Flash
    if (flashOpacity > 0.01f) {
        m_brushFlash->SetOpacity(flashOpacity * 0.4f);
        m_renderTarget->FillRectangle(&cRect, m_brushFlash.Get());
    }

    // Draw thumbnail preview on the right side of the capture rect
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_preview.empty()) {
        if (m_previewDirty) {
            m_d2dBitmap.Reset();
            m_previewDirty = false;
        }
        if (!m_d2dBitmap) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );
            m_renderTarget->CreateBitmap(
                D2D1::SizeU(m_preview.cols, m_preview.rows),
                m_preview.data,
                static_cast<UINT32>(m_preview.step),
                &props,
                &m_d2dBitmap
            );
        }

        if (m_d2dBitmap) {
            float thumbWidth = 200.0f * m_dpiScale;
            float scale = thumbWidth / m_preview.cols;
            float thumbHeight = m_preview.rows * scale;
            
            float thumbX = cRect.right + 20.0f * m_dpiScale;
            if (thumbX + thumbWidth > width) {
                thumbX = cRect.left - thumbWidth - 20.0f * m_dpiScale; // try left side
            }
            float thumbY = cRect.top;

            D2D1_RECT_F tRect = D2D1::RectF(thumbX, thumbY, thumbX + thumbWidth, thumbY + thumbHeight);
            
            // Draw background for thumbnail
            D2D1_ROUNDED_RECT rRect = D2D1::RoundedRect(
                D2D1::RectF(tRect.left - 5.0f * m_dpiScale,
                            tRect.top - 5.0f * m_dpiScale,
                            tRect.right + 5.0f * m_dpiScale,
                            tRect.bottom + 5.0f * m_dpiScale),
                8.0f * m_dpiScale, 8.0f * m_dpiScale
            );
            m_renderTarget->FillRoundedRectangle(&rRect, m_brushBg.Get());
            
            // Draw the stitched image
            m_renderTarget->DrawBitmap(m_d2dBitmap.Get(), &tRect);
            
            // Draw thumbnail border
            m_renderTarget->DrawRoundedRectangle(
                &rRect, m_brushBorder.Get(), 2.0f * m_dpiScale);
        }
    }

    m_renderTarget->EndDraw();

    POINT ptSrc = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m_hwnd, hdcScreen, nullptr, &size, hdcMemory, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMemory, hbmpOld);
    DeleteObject(hbmp);
    DeleteDC(hdcMemory);
    ReleaseDC(nullptr, hdcScreen);
}

LRESULT CALLBACK ScrollCaptureOverlay::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& self = instance();
    if (msg == WM_TIMER && wParam == 1) {
        self.render();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture

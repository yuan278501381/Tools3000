// ─────────────────────────────────────────────────────────────────────────────
// MouseFeedbackOverlayBase.cpp — 硬件加速鼠标反馈可视化覆盖层基类实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/mouse/MouseFeedbackOverlayBase.h"
#include "core/utils/WinUtils.h"
#include "core/utils/UiThreadJoin.h"
#include "core/logger/Logger.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")

namespace tools3000::core {

MouseFeedbackOverlayBase::MouseFeedbackOverlayBase() = default;

MouseFeedbackOverlayBase::~MouseFeedbackOverlayBase() {
    shutdownBase();
}

bool MouseFeedbackOverlayBase::isCompositorReady() const noexcept {
    return m_compositorReady;
}

bool MouseFeedbackOverlayBase::isVisible() const noexcept {
    return m_visible.load(std::memory_order_relaxed);
}

HWND MouseFeedbackOverlayBase::hwnd() const noexcept {
    return m_hwnd;
}

void MouseFeedbackOverlayBase::wakeRender() noexcept {
    m_wakeRender.store(true, std::memory_order_release);
    if (m_wakeEvent) {
        SetEvent(m_wakeEvent);
    }
}

void MouseFeedbackOverlayBase::requestViewport(const MouseViewportRect& viewport) noexcept {
    {
        std::lock_guard lock(m_baseMutex);
        m_pendingViewport = viewport;
        m_viewportDirty.store(true, std::memory_order_release);
    }
    wakeRender();
}

void MouseFeedbackOverlayBase::requestHide() noexcept {
    m_hideRequested.store(true, std::memory_order_release);
    if (m_hwnd && GetCurrentThreadId() == GetWindowThreadProcessId(m_hwnd, nullptr)) {
        std::lock_guard lock(m_baseMutex);
        applyHideOverlayStateLocked();
        m_hideRequested.store(false, std::memory_order_release);
    } else {
        wakeRender();
    }
}

void MouseFeedbackOverlayBase::yieldZOrderForInput() noexcept {
    m_zOrderYieldRequested.store(true, std::memory_order_release);
    wakeRender();
}

void MouseFeedbackOverlayBase::raiseZOrderForDraw() noexcept {
    m_zOrderRaiseRequested.store(true, std::memory_order_release);
    wakeRender();
}

bool MouseFeedbackOverlayBase::initializeBase(HINSTANCE hInstance, const wchar_t* windowClassName, const wchar_t* helperOwnerName) {
    m_hInstance = hInstance ? hInstance : GetModuleHandleW(nullptr);
    m_windowClassName = windowClassName ? windowClassName : L"Tools3000_MouseFeedbackOverlay";
    m_helperOwnerName = helperOwnerName ? helperOwnerName : L"Tools3000_MouseHelperOwner";

    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_wakeEvent) {
        LOG_ERROR("MouseFeedbackOverlayBase: Failed to create wake event, error={}", GetLastError());
        return false;
    }

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        LOG_ERROR("MouseFeedbackOverlayBase: Failed to create ready event, error={}", GetLastError());
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
        return false;
    }

    m_renderThread = std::jthread([this, readyEvent](std::stop_token st) {
        baseRenderLoop(st, readyEvent);
    });

    if (m_renderThread.native_handle()) {
        SetThreadPriority(m_renderThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
    }

    WaitForSingleObject(readyEvent, 5000);
    CloseHandle(readyEvent);

    if (!m_hwnd || !m_compositorReady) {
        LOG_ERROR("MouseFeedbackOverlayBase: Render thread window or compositor initialization failed");
        shutdownBase();
        return false;
    }

    LOG_INFO("MouseFeedbackOverlayBase: Pipeline initialized successfully");
    return true;
}

void MouseFeedbackOverlayBase::shutdownBase() {
    if (m_renderThread.joinable()) {
        m_renderThread.request_stop();
        wakeRender();
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_renderThread, 3000);
    }

    if (m_wakeEvent) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }

    m_visible.store(false, std::memory_order_release);
    LOG_INFO("MouseFeedbackOverlayBase: Pipeline shutdown complete");
}

LRESULT CALLBACK MouseFeedbackOverlayBase::baseWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MouseFeedbackOverlayBase::baseRenderLoop(std::stop_token st, HANDLE readyEvent) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = baseWindowProc;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = m_windowClassName.c_str();
    RegisterClassExW(&wc);

    m_helperOwnerHwnd = tools3000::core::WinUtils::createOverlayHelperOwner(wc.hInstance, m_helperOwnerName.c_str());

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        m_windowClassName.c_str(), L"",
        WS_POPUP,
        0, 0, 1, 1,
        m_helperOwnerHwnd, nullptr, wc.hInstance, nullptr
    );

    if (m_hwnd) {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        tools3000::core::WinUtils::applyTaskbarSafeOverlayStyle(m_hwnd, false);
        SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
        std::lock_guard lock(m_baseMutex);
        initCompositorLocked();
    }

    SetEvent(readyEvent);

    if (!m_hwnd || !m_compositorReady) {
        LOG_ERROR("MouseFeedbackOverlayBase: Render thread failed to create window or initialize compositor");
        return;
    }

    MSG msg;
    while (!st.stop_requested()) {
        DWORD waitMs = INFINITE;
        if (!isOverlayIdle()) {
            waitMs = 16; // 动效活跃时保持 60FPS 渲染采样循环
        }

        DWORD waitResult = MsgWaitForMultipleObjectsEx(
            m_wakeEvent ? 1 : 0,
            m_wakeEvent ? &m_wakeEvent : nullptr,
            waitMs,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE
        );
        (void)waitResult;
        m_wakeRender.store(false, std::memory_order_release);

        if (st.stop_requested()) break;

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (m_zOrderYieldRequested.exchange(false, std::memory_order_acq_rel)) {
            if (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) {
                SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }

        if (m_zOrderRaiseRequested.exchange(false, std::memory_order_acq_rel)) {
            if (m_hwnd && IsWindow(m_hwnd)) {
                SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }

        if (m_hideRequested.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard lock(m_baseMutex);
            applyHideOverlayStateLocked();
            continue;
        }

        if (m_viewportDirty.exchange(false, std::memory_order_acq_rel)) {
            MouseViewportRect vp;
            {
                std::lock_guard lock(m_baseMutex);
                vp = m_pendingViewport;
            }
            applyViewportLocked(vp);
        }

        onRenderTick();
    }

    std::lock_guard lock(m_baseMutex);
    releaseCompositorLocked();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_helperOwnerHwnd) {
        DestroyWindow(m_helperOwnerHwnd);
        m_helperOwnerHwnd = nullptr;
    }
}

bool MouseFeedbackOverlayBase::initCompositorLocked() {
    if (m_compositorReady) return true;
    if (!m_hwnd) return false;

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d.GetAddressOf(), &featureLevel, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d.GetAddressOf(), &featureLevel, nullptr);
        if (FAILED(hr)) {
            LOG_ERROR("MouseFeedbackOverlayBase: D3D11CreateDevice failed: hr=0x{:X}", hr);
            return false;
        }
    }
    m_d3dDevice = d3d;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: As(IDXGIDevice) failed: hr=0x{:X}", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp;
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(dcomp.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: DCompositionCreateDevice failed: hr=0x{:X}", hr);
        return false;
    }
    m_dcompDevice = dcomp;

    hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, m_dcompTarget.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateTargetForHwnd failed: hr=0x{:X}", hr);
        return false;
    }

    hr = m_dcompDevice->CreateVisual(m_dcompVisual.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateVisual failed: hr=0x{:X}", hr);
        return false;
    }

    hr = m_dcompTarget->SetRoot(m_dcompVisual.Get());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: SetRoot failed: hr=0x{:X}", hr);
        return false;
    }

    D2D1_FACTORY_OPTIONS opt{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, opt, m_d2dFactory1.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: D2D1CreateFactory failed: hr=0x{:X}", hr);
        return false;
    }

    hr = m_d2dFactory1->CreateDevice(dxgiDevice.Get(), m_d2dDevice.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateDevice failed: hr=0x{:X}", hr);
        return false;
    }

    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateDeviceContext failed: hr=0x{:X}", hr);
        return false;
    }

    m_compositorReady = true;
    onRecreateDeviceResources();
    return true;
}

void MouseFeedbackOverlayBase::releaseCompositorLocked() {
    onReleaseDeviceResources();
    releaseDcompSurfaceLocked();
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory1.Reset();
    m_dcompVisual.Reset();
    m_dcompTarget.Reset();
    m_dcompDevice.Reset();
    m_d3dDevice.Reset();
    m_compositorReady = false;
}

bool MouseFeedbackOverlayBase::ensureDcompSurfaceLocked(int width, int height) {
    if (!m_compositorReady) return false;
    if (width <= 0 || height <= 0) return false;

    if (m_dcompSurface && m_surfaceWidth == width && m_surfaceHeight == height) {
        return true;
    }

    Microsoft::WRL::ComPtr<IDCompositionSurface> newSurface;
    HRESULT hr = m_dcompDevice->CreateSurface(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        newSurface.GetAddressOf()
    );

    if (FAILED(hr) || !newSurface) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateSurface failed: w={}, h={}, hr=0x{:X}", width, height, hr);
        return false;
    }

    // 预清零新分配的硬件表面，杜绝任何未初始化显存瓦片闪烁
    if (m_d2dContext) {
        POINT offset{};
        Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurf;
        if (SUCCEEDED(newSurface->BeginDraw(nullptr, IID_PPV_ARGS(dxgiSurf.GetAddressOf()), &offset)) && dxgiSurf) {
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> clearBmp;
            if (SUCCEEDED(m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurf.Get(), &bp, clearBmp.GetAddressOf())) && clearBmp) {
                m_d2dContext->SetTarget(clearBmp.Get());
                m_d2dContext->BeginDraw();
                m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0.0f));
                m_d2dContext->EndDraw();
                m_d2dContext->SetTarget(nullptr);
            }
            dxgiSurf.Reset();
            newSurface->EndDraw();
        }
    }

    m_dcompSurface = std::move(newSurface);
    m_surfaceWidth = width;
    m_surfaceHeight = height;

    hr = m_dcompVisual->SetContent(m_dcompSurface.Get());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: SetContent failed: hr=0x{:X}", hr);
        return false;
    }

    return true;
}

void MouseFeedbackOverlayBase::releaseDcompSurfaceLocked() {
    if (m_dcompVisual) {
        m_dcompVisual->SetContent(nullptr);
    }
    if (m_dcompDevice) {
        m_dcompDevice->Commit();
    }
    m_dcompSurface.Reset();
    m_surfaceWidth = 0;
    m_surfaceHeight = 0;
}

void MouseFeedbackOverlayBase::applyViewportLocked(const MouseViewportRect& viewport) {
    if (!m_hwnd) return;
    if (m_currentViewport == viewport && m_visible.load(std::memory_order_relaxed)) {
        return;
    }
    m_currentViewport = viewport;

    SetWindowPos(
        m_hwnd,
        HWND_TOPMOST,
        viewport.x,
        viewport.y,
        viewport.width,
        viewport.height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW
    );
    m_visible.store(true, std::memory_order_release);
}

bool MouseFeedbackOverlayBase::beginDrawLocked(ID2D1DeviceContext** outContext, POINT* surfaceOffset) {
    if (!m_compositorReady || !m_dcompSurface || !m_d2dContext) return false;

    POINT offset{};
    Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurface;
    HRESULT hr = m_dcompSurface->BeginDraw(
        nullptr,
        __uuidof(IDXGISurface),
        reinterpret_cast<void**>(dxgiSurface.GetAddressOf()),
        &offset
    );

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LOG_WARN("MouseFeedbackOverlayBase: GPU device lost, resetting compositor");
        releaseCompositorLocked();
        if (initCompositorLocked() && ensureDcompSurfaceLocked(m_surfaceWidth, m_surfaceHeight)) {
            hr = m_dcompSurface->BeginDraw(
                nullptr,
                __uuidof(IDXGISurface),
                reinterpret_cast<void**>(dxgiSurface.GetAddressOf()),
                &offset
            );
        }
    }

    if (FAILED(hr)) {
        LOG_WARN("MouseFeedbackOverlayBase: BeginDraw failed: hr=0x{:X}", hr);
        return false;
    }

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap;
    hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bp, targetBitmap.GetAddressOf());
    if (FAILED(hr)) {
        m_dcompSurface->EndDraw();
        LOG_ERROR("MouseFeedbackOverlayBase: CreateBitmapFromDxgiSurface failed: hr=0x{:X}", hr);
        return false;
    }

    m_d2dContext->SetTarget(targetBitmap.Get());
    m_d2dContext->BeginDraw();

    if (surfaceOffset) {
        *surfaceOffset = offset;
    }
    if (outContext) {
        *outContext = m_d2dContext.Get();
    }
    return true;
}

bool MouseFeedbackOverlayBase::endDrawAndCommitLocked() {
    if (!m_compositorReady || !m_dcompSurface || !m_d2dContext) return false;

    HRESULT hr = m_d2dContext->EndDraw();
    m_d2dContext->SetTarget(nullptr);
    m_dcompSurface->EndDraw();

    if (SUCCEEDED(hr)) {
        m_dcompDevice->Commit();
        return true;
    }

    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LOG_WARN("MouseFeedbackOverlayBase: EndDraw device lost, releasing compositor");
        releaseCompositorLocked();
    }
    return false;
}

void MouseFeedbackOverlayBase::applyHideOverlayStateLocked() {
    bool wasActiveOrVisible = m_visible.load(std::memory_order_relaxed) ||
                              (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) ||
                              (m_dcompSurface != nullptr);

    if (m_hwnd && IsWindow(m_hwnd)) {
        ShowWindow(m_hwnd, SW_HIDE);
    }
    releaseDcompSurfaceLocked();
    m_visible.store(false, std::memory_order_release);
    m_currentViewport = MouseViewportRect{};

    if (wasActiveOrVisible) {
        tools3000::core::WinUtils::trimWorkingSet();
    }
}

} // namespace tools3000::core

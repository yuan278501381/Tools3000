#include "ui/WebViewSuspend.h"

#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <wrl/client.h>
#include <wrl/event.h>

// WebView2's MIDL-generated C++ declarations require the COM interface
// macros supplied by WRL/unknwn before the SDK header is parsed.
#include <WebView2.h>

#include <atomic>
#include <string>

namespace tools3000::ui {

struct WebViewSuspendController::State {
    std::atomic<bool> suspendDesired{false};
    std::atomic<bool> abandoned{false};
    std::atomic<uint64_t> generation{0};
};

WebViewSuspendController::WebViewSuspendController()
    : m_state(std::make_shared<State>()) {}

WebViewSuspendController::~WebViewSuspendController() {
    abandon();
}

HRESULT WebViewSuspendController::requestSuspend(ICoreWebView2* webView,
                                                  const char* surface) noexcept {
    if (!webView) return E_POINTER;
    if (!m_state) return E_UNEXPECTED;
    const uint64_t requestGen = m_state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_state->suspendDesired.store(true, std::memory_order_release);
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webView3;
    const HRESULT query = webView->QueryInterface(IID_PPV_ARGS(&webView3));
    if (FAILED(query) || !webView3) return query;
    const std::string surfaceName = surface ? surface : "unknown";
    const std::shared_ptr<State> state = m_state;
    const HRESULT result = webView3->TrySuspend(
        Microsoft::WRL::Callback<ICoreWebView2TrySuspendCompletedHandler>(
            [state, webView3, surfaceName, requestGen](HRESULT error, BOOL successful) -> HRESULT {
                const auto outcome = classifyWebViewSuspendCompletion(error, successful);
                const bool stillDesired = state->suspendDesired.load(std::memory_order_acquire);
                const bool isAbandoned = state->abandoned.load(std::memory_order_acquire);
                const bool sameGeneration = (state->generation.load(std::memory_order_acquire) == requestGen);
                switch (outcome) {
                    case WebViewSuspendOutcome::Suspended:
                        LOG_DEBUG("WebView suspended: {}", surfaceName);
                        // 仅当当前挂起世代仍有效且窗口未重新显示/废弃时触发物理内存修剪
                        if (stillDesired && !isAbandoned && sameGeneration) {
                            tools3000::core::WinUtils::trimWorkingSet();
                        }
                        break;
                    case WebViewSuspendOutcome::Refused:
                        LOG_DEBUG("WebView suspend was declined by runtime: {}", surfaceName);
                        break;
                    case WebViewSuspendOutcome::Failed:
                        LOG_WARN("WebView suspend failed: {}, hr=0x{:08X}", surfaceName,
                                 static_cast<unsigned long>(error));
                        break;
                }
                // The WebView runtime may complete TrySuspend after the owner
                // has already shown the surface again. Resume in that exact
                // case so visible content cannot remain frozen. `webView3` is
                // retained only until this callback returns; no HWND/window is
                // captured, so destruction remains safe.
                if (shouldResumeAfterSuspendCompletion(outcome, stillDesired, isAbandoned)) {
                    const HRESULT resumeResult = webView3->Resume();
                    if (FAILED(resumeResult)) {
                        LOG_WARN("WebView late Resume failed: {}, hr=0x{:08X}", surfaceName,
                                 static_cast<unsigned long>(resumeResult));
                    }
                }
                return S_OK;
            }).Get());
    if (FAILED(result)) {
        LOG_WARN("WebView TrySuspend request failed: {}, hr=0x{:08X}", surfaceName,
                 static_cast<unsigned long>(result));
    }
    return result;
}

HRESULT WebViewSuspendController::requestSuspend(ICoreWebView2Controller* controller,
                                                ICoreWebView2* webView) noexcept {
    if (webView) {
        return requestSuspend(webView, "tray");
    }
    if (!controller) return E_POINTER;
    Microsoft::WRL::ComPtr<ICoreWebView2> wv;
    const HRESULT hr = controller->get_CoreWebView2(&wv);
    if (FAILED(hr) || !wv) return hr;
    return requestSuspend(wv.Get(), "tray");
}

HRESULT WebViewSuspendController::resume(ICoreWebView2* webView, const char* surface) noexcept {
    if (!webView) return E_POINTER;
    if (!m_state) return E_UNEXPECTED;
    m_state->generation.fetch_add(1, std::memory_order_acq_rel);
    m_state->suspendDesired.store(false, std::memory_order_release);
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webView3;
    const HRESULT query = webView->QueryInterface(IID_PPV_ARGS(&webView3));
    if (FAILED(query) || !webView3) return query;
    const HRESULT result = webView3->Resume();
    if (FAILED(result)) {
        LOG_WARN("WebView Resume failed: {}, hr=0x{:08X}", surface ? surface : "unknown",
                 static_cast<unsigned long>(result));
    }
    return result;
}

HRESULT WebViewSuspendController::resume(ICoreWebView2Controller* controller) noexcept {
    if (!controller) return E_POINTER;
    Microsoft::WRL::ComPtr<ICoreWebView2> wv;
    const HRESULT hr = controller->get_CoreWebView2(&wv);
    if (FAILED(hr) || !wv) return hr;
    return resume(wv.Get(), "tray");
}

void WebViewSuspendController::reset() noexcept {
    try {
        m_state = std::make_shared<State>();
    } catch (...) {
        // Keep the old state abandoned if allocating a replacement fails. The
        // WebView remains usable; only best-effort deep suspension is skipped.
    }
}

void WebViewSuspendController::abandon() noexcept {
    if (!m_state) return;
    m_state->abandoned.store(true, std::memory_order_release);
    m_state->suspendDesired.store(false, std::memory_order_release);
}

}  // namespace tools3000::ui

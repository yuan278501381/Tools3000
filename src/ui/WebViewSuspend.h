#pragma once

#include <windows.h>

#include <memory>

struct ICoreWebView2;
struct ICoreWebView2Controller;

namespace tools3000::ui {

enum class WebViewSuspendOutcome {
    Suspended,
    Refused,
    Failed,
};

// `TrySuspend` uses S_OK plus FALSE for best-effort refusal, so HRESULT alone
// is not enough for diagnostics or lifecycle tests.
inline WebViewSuspendOutcome classifyWebViewSuspendCompletion(HRESULT error, BOOL successful) noexcept {
    if (FAILED(error)) return WebViewSuspendOutcome::Failed;
    return successful ? WebViewSuspendOutcome::Suspended : WebViewSuspendOutcome::Refused;
}

// A hide/show cycle can complete asynchronously in either order. If a suspend
// completion arrives after a later show, it must issue one final Resume; an
// abandoned window must never receive that late COM call.
inline bool shouldResumeAfterSuspendCompletion(
    WebViewSuspendOutcome outcome, bool suspendStillDesired, bool abandoned) noexcept {
    return outcome == WebViewSuspendOutcome::Suspended && !suspendStillDesired && !abandoned;
}

/// Per-window owner for best-effort WebView2 suspension. Completion handlers
/// retain no HWND or window object, and `abandon()` makes late completions
/// inert before the controller/WebView is released during destruction.
class WebViewSuspendController final {
public:
    WebViewSuspendController();
    ~WebViewSuspendController();
    WebViewSuspendController(const WebViewSuspendController&) = delete;
    WebViewSuspendController& operator=(const WebViewSuspendController&) = delete;

    HRESULT requestSuspend(ICoreWebView2* webView, const char* surface) noexcept;
    HRESULT requestSuspend(ICoreWebView2Controller* controller, ICoreWebView2* webView) noexcept;
    HRESULT resume(ICoreWebView2* webView, const char* surface) noexcept;
    HRESULT resume(ICoreWebView2Controller* controller) noexcept;
    /// Starts a fresh generation after a window and its WebView controller were
    /// recreated. Late callbacks keep the previous state and remain inert.
    void reset() noexcept;
    void abandon() noexcept;

private:
    struct State;
    std::shared_ptr<State> m_state;
};

}  // namespace tools3000::ui

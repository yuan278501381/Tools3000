#pragma once

#include "core/utils/SessionIsolation.h"

#include <windows.h>
#include <wrl/client.h>
#include <WebView2.h>

#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>
#include <vector>

namespace tools3000::ui {

/// Owns the single WebView2 environment shared by every application surface.
/// Controllers remain window-specific, while browser processes and profile
/// state are reused to keep idle memory and startup latency bounded.
class WebViewEnvironmentManager final {
public:
    using ReadyCallback = std::function<void(HRESULT, ICoreWebView2Environment*)>;

    static WebViewEnvironmentManager& instance();
    void acquire(ReadyCallback callback);
    void shutdown();
    bool isInitializing() const noexcept;

private:
    enum class State { Idle, Initializing, Ready };

    WebViewEnvironmentManager() = default;
    ~WebViewEnvironmentManager() = default;
    WebViewEnvironmentManager(const WebViewEnvironmentManager&) = delete;
    WebViewEnvironmentManager& operator=(const WebViewEnvironmentManager&) = delete;

    void completeInitialization(uint64_t generation, HRESULT result,
                                ICoreWebView2Environment* environment);

    std::mutex m_mutex;
    State m_state = State::Idle;
    std::atomic<bool> m_initializing{false};
    uint64_t m_generation = 0;
    tools3000::core::WebViewProfileSlotLease m_profileSlotLease;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    std::vector<ReadyCallback> m_callbacks;
};

}  // namespace tools3000::ui

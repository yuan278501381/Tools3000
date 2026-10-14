#include "ui/WebViewEnvironmentManager.h"

#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <wrl.h>
#include <WebView2EnvironmentOptions.h>
#include <vector>

using namespace Microsoft::WRL;

namespace tools3000::ui {

WebViewEnvironmentManager& WebViewEnvironmentManager::instance() {
    static WebViewEnvironmentManager s_instance;
    return s_instance;
}

void WebViewEnvironmentManager::acquire(ReadyCallback callback) {
    if (!callback) return;

    ComPtr<ICoreWebView2Environment> readyEnvironment;
    bool shouldInitialize = false;
    uint64_t generation = 0;

    {
        std::lock_guard lock(m_mutex);
        if (m_state == State::Ready && m_environment) {
            readyEnvironment = m_environment;
        } else {
            m_callbacks.push_back(std::move(callback));
            if (m_state == State::Idle) {
                m_state = State::Initializing;
                shouldInitialize = true;
                generation = ++m_generation;
            }
        }
    }

    if (readyEnvironment) {
        try {
            callback(S_OK, readyEnvironment.Get());
        } catch (const std::exception& e) {
            LOG_ERROR("WebView2 环境获取回调异常: {}", e.what());
        } catch (...) {
            LOG_ERROR("WebView2 环境获取回调未知异常");
        }
        return;
    }
    if (!shouldInitialize) return;

    const auto userDataPath =
        (tools3000::core::WinUtils::getAppDataDirectory() / L"webview2_data").wstring();
    auto options = Make<CoreWebView2EnvironmentOptions>();
    if (options) {
        options->put_AdditionalBrowserArguments(
            L"--no-sandbox "
            L"--js-flags=--optimize-for-size "
            L"--enable-features=OverlayScrollbar,ResourcePriorityPolicy "
            L"--disable-features=RendererCodeIntegrity,Translate,InterestFeedContentSuggestions "
            L"--disable-background-networking "
            L"--disable-component-update "
            L"--allow-no-sandbox-job");
    }

    const HRESULT startResult = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataPath.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, generation](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                completeInitialization(generation, result, environment);
                return S_OK;
            }).Get());
    if (FAILED(startResult)) completeInitialization(generation, startResult, nullptr);
}

void WebViewEnvironmentManager::shutdown() {
    std::vector<ReadyCallback> pending;
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        m_state = State::Idle;
        m_environment.Reset();
        pending.swap(m_callbacks);
    }
    for (auto& cb : pending) {
        try {
            cb(E_ABORT, nullptr);
        } catch (...) {
        }
    }
}

void WebViewEnvironmentManager::completeInitialization(
    uint64_t generation, HRESULT result, ICoreWebView2Environment* environment) {
    std::vector<ReadyCallback> callbacks;
    {
        std::lock_guard lock(m_mutex);
        if (generation != m_generation || m_state != State::Initializing) return;
        if (SUCCEEDED(result) && environment) {
            m_environment = environment;
            m_state = State::Ready;
        } else {
            m_environment.Reset();
            m_state = State::Idle;
        }
        callbacks.swap(m_callbacks);
    }

    if (FAILED(result) || !environment) {
        LOG_ERROR("共享 WebView2 环境创建失败, hr=0x{:08X}", static_cast<unsigned>(result));
    } else {
        LOG_INFO("共享 WebView2 环境已就绪");
    }
    for (auto& callback : callbacks) {
        try {
            callback(result, environment);
        } catch (const std::exception& e) {
            LOG_ERROR("WebView2 环境就绪回调异常: {}", e.what());
        } catch (...) {
            LOG_ERROR("WebView2 环境就绪回调未知异常");
        }
    }
}

}  // namespace tools3000::ui

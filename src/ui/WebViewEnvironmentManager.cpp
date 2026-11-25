#include "ui/WebViewEnvironmentManager.h"

#include "core/i18n/UiI18n.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <wrl.h>
#include <WebView2EnvironmentOptions.h>
#include <atomic>
#include <thread>
#include <vector>

using namespace Microsoft::WRL;

namespace tools3000::ui {

namespace {

constexpr const wchar_t* kWebView2EvergreenDownloadUrl =
    L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";

bool isWebView2RuntimeInstalled() noexcept {
    LPWSTR versionInfo = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &versionInfo);
    if (versionInfo) {
        CoTaskMemFree(versionInfo);
    }
    return SUCCEEDED(hr);
}

bool isAutomatedHeadlessProcess() noexcept {
    const wchar_t* cmdLine = GetCommandLineW();
    if (!cmdLine) return false;
    return wcsstr(cmdLine, L"--lifecycle-test-instance") != nullptr ||
           wcsstr(cmdLine, L"--headless") != nullptr;
}

void promptInstallWebView2RuntimeOnce() {
    if (isAutomatedHeadlessProcess()) return;
    static std::atomic<bool> s_prompted{false};
    if (s_prompted.exchange(true, std::memory_order_acq_rel)) return;

    std::thread([]() {
        const std::wstring title = tools3000::core::UiI18n::trW(
            L"缺少 Microsoft Edge WebView2 运行时",
            L"Microsoft Edge WebView2 Runtime Required");
        const std::wstring body = tools3000::core::UiI18n::trW(
            L"当前 Windows 系统未安装 Microsoft Edge WebView2 运行时，因此设置、搜索与剪贴板界面暂时无法显示。\n\n点击“是”立即打开微软官方页面下载安装，完成后重启 Tools3000 即可正常使用。",
            L"Microsoft Edge WebView2 Runtime is not installed on this Windows system, so Settings, Search, and Clipboard UI cannot be displayed yet.\n\nClick Yes to open the official Microsoft download page, then restart Tools3000 after installation.");
        const int choice = MessageBoxW(
            nullptr, body.c_str(), title.c_str(), MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST);
        if (choice == IDYES) {
            tools3000::core::WinUtils::openUrl(kWebView2EvergreenDownloadUrl);
        }
    }).detach();
}

}  // namespace

WebViewEnvironmentManager& WebViewEnvironmentManager::instance() {
    static WebViewEnvironmentManager s_instance;
    return s_instance;
}

bool WebViewEnvironmentManager::isRuntimeInstalled() noexcept {
    return isWebView2RuntimeInstalled();
}

const wchar_t* WebViewEnvironmentManager::evergreenDownloadUrl() noexcept {
    return kWebView2EvergreenDownloadUrl;
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
                m_initializing.store(true, std::memory_order_release);
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

    std::wstring userDataPath;
    int acquiredSlot = -1;
    {
        auto lease = tools3000::core::SessionIsolation::acquireWebViewProfileSlot(
            tools3000::core::WinUtils::getAppDataDirectory());
        userDataPath = lease.dataDirectory().wstring();
        acquiredSlot = lease.slotIndex();
        std::lock_guard lock(m_mutex);
        if (generation != m_generation || m_state != State::Initializing) {
            return;
        }
        m_profileSlotLease = std::move(lease);
    }
    if (acquiredSlot > 0) {
        LOG_INFO("WebView2 multi-session isolation active: sessionId={}, slot={}, dataDir={}",
                 tools3000::core::SessionIsolation::currentSessionId(),
                 acquiredSlot,
                 tools3000::core::WinUtils::wstringToUtf8(userDataPath));
    }
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

bool WebViewEnvironmentManager::isInitializing() const noexcept {
    return m_initializing.load(std::memory_order_acquire);
}

void WebViewEnvironmentManager::shutdown() {
    std::vector<ReadyCallback> pending;
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        m_state = State::Idle;
        m_initializing.store(false, std::memory_order_release);
        m_environment.Reset();
        m_profileSlotLease.release();
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
            m_profileSlotLease.release();
            m_state = State::Idle;
        }
        m_initializing.store(false, std::memory_order_release);
        callbacks.swap(m_callbacks);
    }

    if (FAILED(result) || !environment) {
        LOG_ERROR("共享 WebView2 环境创建失败, hr=0x{:08X}", static_cast<unsigned>(result));
        if (!isWebView2RuntimeInstalled()) {
            LOG_WARN("WebView2 Runtime is missing on this Windows machine; prompting official Evergreen installer");
            promptInstallWebView2RuntimeOnce();
        }
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

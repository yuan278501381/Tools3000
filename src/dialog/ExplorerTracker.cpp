/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#include "ExplorerTracker.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <shlobj.h>
#include <exdisp.h>
#include <shlwapi.h>
#include <wrl/client.h>

#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

namespace tools3000::dialog {

namespace {

constexpr uint64_t kMinRefreshIntervalMs = 180;
constexpr uint64_t kActivePollingIntervalMs = 1500;
constexpr uint64_t kStaleReadThresholdMs = 350;

struct ScopedComApartment {
    HRESULT hr = E_FAIL;
    ScopedComApartment() noexcept
        : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ScopedComApartment() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;
};

int hexDigitValue(wchar_t ch) noexcept {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return 10 + (ch - L'a');
    if (ch >= L'A' && ch <= L'F') return 10 + (ch - L'A');
    return -1;
}

bool startsWithFileScheme(const std::wstring& s) noexcept {
    if (s.size() < 7) return false;
    const wchar_t prefix[7] = {L'f', L'i', L'l', L'e', L':', L'/', L'/'};
    for (size_t i = 0; i < 7; ++i) {
        if (std::towlower(s[i]) != prefix[i]) return false;
    }
    return true;
}

std::wstring decodePercentUtf8Segment(const std::wstring& encoded) {
    std::wstring out;
    out.reserve(encoded.size());
    std::string byteRun;

    auto flushBytes = [&]() {
        if (byteRun.empty()) return;
        int wlen = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, byteRun.data(), static_cast<int>(byteRun.size()), nullptr, 0);
        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (wlen <= 0) {
            codePage = CP_ACP;
            flags = 0;
            wlen = MultiByteToWideChar(
                codePage, flags, byteRun.data(), static_cast<int>(byteRun.size()), nullptr, 0);
        }
        if (wlen > 0) {
            std::wstring decoded(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(
                codePage, flags, byteRun.data(), static_cast<int>(byteRun.size()), decoded.data(), wlen);
            out += decoded;
        }
        byteRun.clear();
    };

    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == L'%' && i + 2 < encoded.size()) {
            const int hi = hexDigitValue(encoded[i + 1]);
            const int lo = hexDigitValue(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                byteRun.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        flushBytes();
        out.push_back(encoded[i]);
    }
    flushBytes();
    return out;
}

std::wstring decodeFileUriFallback(const std::wstring& uri) {
    if (!startsWithFileScheme(uri)) return {};

    size_t endPos = uri.find_first_of(L"?#");
    std::wstring body = uri.substr(7, endPos == std::wstring::npos ? std::wstring::npos : (endPos - 7));

    bool isUnc = false;
    if (!body.empty() && (body[0] == L'/' || body[0] == L'\\')) {
        size_t slashCount = 0;
        while (slashCount < body.size() && (body[slashCount] == L'/' || body[slashCount] == L'\\')) {
            ++slashCount;
        }
        if (slashCount >= 2) {
            isUnc = true;
        }
        body.erase(0, slashCount);
    } else if (body.size() >= 10) {
        std::wstring hostLower = body.substr(0, 10);
        for (auto& ch : hostLower) ch = static_cast<wchar_t>(std::towlower(ch));
        if (hostLower == L"localhost/" || hostLower == L"localhost\\") {
            body.erase(0, 10);
        } else {
            isUnc = true;
        }
    } else if (!body.empty()) {
        if (!(body.size() >= 2 && std::iswalpha(body[0]) && (body[1] == L':' || body[1] == L'|'))) {
            isUnc = true;
        }
    }

    std::wstring decoded = decodePercentUtf8Segment(body);
    if (decoded.empty()) return {};

    if (!isUnc && decoded.size() >= 2 && std::iswalpha(decoded[0]) && decoded[1] == L'|') {
        decoded[1] = L':';
    }

    for (auto& ch : decoded) {
        if (ch == L'/') ch = L'\\';
    }

    if (isUnc) {
        decoded = L"\\\\" + decoded;
    }

    while (decoded.size() > 3 && decoded.back() == L'\\') {
        decoded.pop_back();
    }
    return decoded;
}

bool isExplorerTopWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    wchar_t className[64] = {0};
    if (GetClassNameW(hwnd, className, 64) <= 0) return false;
    return wcscmp(className, L"CabinetWClass") == 0 || wcscmp(className, L"ExploreWClass") == 0;
}

HWND findCurrentActiveExplorerHwnd() noexcept {
    HWND fgHwnd = GetForegroundWindow();
    if (isExplorerTopWindow(fgHwnd)) {
        return fgHwnd;
    }
    for (HWND w = GetTopWindow(nullptr); w != nullptr; w = GetWindow(w, GW_HWNDNEXT)) {
        if (isExplorerTopWindow(w)) {
            return w;
        }
    }
    return nullptr;
}

} // namespace

ExplorerTracker& ExplorerTracker::instance() {
    static ExplorerTracker s_instance;
    return s_instance;
}

ExplorerTracker::~ExplorerTracker() {
    shutdown();
}

void ExplorerTracker::start() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_running.load(std::memory_order_acquire)) {
        return;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_running.store(true, std::memory_order_release);
    // 惰性启动：初始化时不主动轮询，等待首次活动对话框会话或显式 requestRefresh() 再唤醒，保障 0% 空闲 CPU
    m_refreshRequested = false;
    m_worker = std::thread(&ExplorerTracker::workerLoop, this);
}

void ExplorerTracker::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_running.exchange(false, std::memory_order_acq_rel) && !m_worker.joinable()) {
            return;
        }
        m_activeMonitoring = false;
        m_refreshRequested = false;
    }
    m_wakeCv.notify_all();
    m_doneCv.notify_all();
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) {
        m_worker.join();
    }
}

void ExplorerTracker::ensureStarted() {
    if (!m_running.load(std::memory_order_acquire)) {
        start();
    }
}

void ExplorerTracker::setActiveMonitoring(bool active) {
    if (active) {
        ensureStarted();
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_activeMonitoring == active) {
            return;
        }
        m_activeMonitoring = active;
        if (active) {
            m_refreshRequested = true;
        }
    }
    m_wakeCv.notify_one();
}

void ExplorerTracker::requestRefresh() {
    ensureStarted();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        const uint64_t now = GetTickCount64();
        if (m_snapshotEpoch > 0 && now >= m_lastRefreshTick &&
            (now - m_lastRefreshTick) < kMinRefreshIntervalMs) {
            return;
        }
        m_refreshRequested = true;
    }
    m_wakeCv.notify_one();
}

void ExplorerTracker::setPathChangedCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_onPathChanged = std::move(callback);
}

void ExplorerTracker::workerLoop() {
    ScopedComApartment comGuard;
    while (m_running.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(m_stateMutex);
            if (!m_activeMonitoring && !m_refreshRequested) {
                // 无活动对话框时无限期休眠：彻底消除 24/7 空闲唤醒与 COM 轮询开销
                m_wakeCv.wait(lock, [this]() {
                    return !m_running.load(std::memory_order_acquire) ||
                           m_activeMonitoring ||
                           m_refreshRequested;
                });
            } else if (!m_refreshRequested) {
                m_wakeCv.wait_for(lock, std::chrono::milliseconds(kActivePollingIntervalMs), [this]() {
                    return !m_running.load(std::memory_order_acquire) ||
                           !m_activeMonitoring ||
                           m_refreshRequested;
                });
            }
            if (!m_running.load(std::memory_order_acquire)) {
                break;
            }
            if (!m_activeMonitoring && !m_refreshRequested) {
                continue;
            }
            m_refreshRequested = false;
        }

        refreshSnapshotOnStaThread();
    }
}

void ExplorerTracker::refreshSnapshotOnStaThread() {
    std::vector<std::pair<HWND, std::string>> nextPerWindow;
    std::vector<std::string> nextAllPaths;

    ComPtr<IShellWindows> shellWindows;
    const HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows));
    if (SUCCEEDED(hr) && shellWindows) {
        long count = 0;
        shellWindows->get_Count(&count);

        std::vector<std::pair<HWND, std::string>> windowFallbacks;

        for (long i = 0; i < count && m_running.load(std::memory_order_acquire); ++i) {
            VARIANT vIndex;
            VariantInit(&vIndex);
            vIndex.vt = VT_I4;
            vIndex.lVal = i;

            ComPtr<IDispatch> disp;
            if (SUCCEEDED(shellWindows->Item(vIndex, &disp)) && disp) {
                ComPtr<IWebBrowserApp> app;
                if (SUCCEEDED(disp.As(&app)) && app) {
                    HWND topHwnd = nullptr;
                    app->get_HWND(reinterpret_cast<SHANDLE_PTR*>(&topHwnd));

                    BSTR bstrUrl = nullptr;
                    if (SUCCEEDED(app->get_LocationURL(&bstrUrl)) && bstrUrl) {
                        std::wstring url(bstrUrl);
                        SysFreeString(bstrUrl);

                        std::string path = normalizeFolderPath(url);
                        if (!path.empty()) {
                            if (std::find(nextAllPaths.begin(), nextAllPaths.end(), path) == nextAllPaths.end()) {
                                nextAllPaths.push_back(path);
                            }

                            if (topHwnd) {
                                HWND activeTabHwnd = FindWindowExW(topHwnd, nullptr, L"ShellTabWindowClass", nullptr);
                                bool isExactActiveTab = false;
                                if (activeTabHwnd) {
                                    ComPtr<IServiceProvider> sp;
                                    if (SUCCEEDED(app.As(&sp)) && sp) {
                                        ComPtr<IShellBrowser> shellBrowser;
                                        if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser))) && shellBrowser) {
                                            HWND browserHwnd = nullptr;
                                            if (SUCCEEDED(shellBrowser->GetWindow(&browserHwnd)) && browserHwnd) {
                                                isExactActiveTab = (browserHwnd == activeTabHwnd);
                                            }
                                        }
                                    }
                                } else {
                                    isExactActiveTab = true;
                                }

                                if (isExactActiveTab) {
                                    auto it = std::find_if(
                                        nextPerWindow.begin(), nextPerWindow.end(),
                                        [topHwnd](const auto& kv) { return kv.first == topHwnd; });
                                    if (it == nextPerWindow.end()) {
                                        nextPerWindow.emplace_back(topHwnd, path);
                                    } else {
                                        it->second = path;
                                    }
                                } else {
                                    auto fbIt = std::find_if(
                                        windowFallbacks.begin(), windowFallbacks.end(),
                                        [topHwnd](const auto& kv) { return kv.first == topHwnd; });
                                    if (fbIt == windowFallbacks.end()) {
                                        windowFallbacks.emplace_back(topHwnd, path);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        for (const auto& fb : windowFallbacks) {
            auto it = std::find_if(
                nextPerWindow.begin(), nextPerWindow.end(),
                [hwnd = fb.first](const auto& kv) { return kv.first == hwnd; });
            if (it == nextPerWindow.end()) {
                nextPerWindow.push_back(fb);
            }
        }
    }

    std::function<void()> notifyCb;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        const bool changed = (m_perWindowActivePaths != nextPerWindow) || (m_allOpenPaths != nextAllPaths);
        m_perWindowActivePaths = std::move(nextPerWindow);
        m_allOpenPaths = std::move(nextAllPaths);
        m_lastRefreshTick = GetTickCount64();
        ++m_snapshotEpoch;
        if (changed) {
            notifyCb = m_onPathChanged;
        }
    }
    m_doneCv.notify_all();
    if (notifyCb) {
        notifyCb();
    }
}

std::string ExplorerTracker::normalizeFolderPath(const std::wstring& rawPath) {
    if (rawPath.empty()) return "";

    if (startsWithFileScheme(rawPath)) {
        DWORD cap = static_cast<DWORD>((std::max<size_t>)(MAX_PATH + 1, rawPath.size() + 32));
        std::wstring localBuf(cap, L'\0');
        DWORD pathLen = cap;
        HRESULT hr = PathCreateFromUrlW(rawPath.c_str(), localBuf.data(), &pathLen, 0);
        if (hr == E_POINTER && pathLen > cap && pathLen < 65536) {
            localBuf.assign(static_cast<size_t>(pathLen) + 4, L'\0');
            DWORD retryLen = pathLen + 4;
            hr = PathCreateFromUrlW(rawPath.c_str(), localBuf.data(), &retryLen, 0);
            if (SUCCEEDED(hr)) {
                pathLen = retryLen;
            }
        }
        if (SUCCEEDED(hr)) {
            localBuf.resize(wcslen(localBuf.c_str()));
            if (!localBuf.empty() && !startsWithFileScheme(localBuf)) {
                return tools3000::core::WinUtils::wstringToUtf8(localBuf);
            }
        }

        std::wstring fallbackDecoded = decodeFileUriFallback(rawPath);
        if (!fallbackDecoded.empty()) {
            return tools3000::core::WinUtils::wstringToUtf8(fallbackDecoded);
        }
        return "";
    }

    std::wstring normalized = rawPath;
    for (auto& ch : normalized) {
        if (ch == L'/') ch = L'\\';
    }
    return tools3000::core::WinUtils::wstringToUtf8(normalized);
}

std::string ExplorerTracker::getActiveExplorerPath() {
    ensureStarted();

    const auto& disp = tools3000::core::MainThreadDispatcher::instance();
    const bool onUiThread = disp.isInitialized() && disp.isOwnerThread();
    const uint64_t now = GetTickCount64();

    {
        std::unique_lock<std::mutex> lock(m_stateMutex);
        if (m_snapshotEpoch == 0 || (now - m_lastRefreshTick) > kStaleReadThresholdMs) {
            m_refreshRequested = true;
            m_wakeCv.notify_one();
            if (!onUiThread && m_snapshotEpoch == 0) {
                const uint64_t targetEpoch = m_snapshotEpoch + 1;
                m_doneCv.wait_for(lock, std::chrono::milliseconds(80), [this, targetEpoch]() {
                    return !m_running.load(std::memory_order_acquire) || m_snapshotEpoch >= targetEpoch;
                });
            }
        }

        const HWND activeExplorer = findCurrentActiveExplorerHwnd();
        if (activeExplorer) {
            for (const auto& kv : m_perWindowActivePaths) {
                if (kv.first == activeExplorer && !kv.second.empty()) {
                    return kv.second;
                }
            }
        }
        if (!m_perWindowActivePaths.empty()) {
            return m_perWindowActivePaths.front().second;
        }
        if (!m_allOpenPaths.empty()) {
            return m_allOpenPaths.front();
        }
    }
    return "";
}

std::vector<std::string> ExplorerTracker::getAllOpenExplorerPaths() {
    ensureStarted();

    const auto& disp = tools3000::core::MainThreadDispatcher::instance();
    const bool onUiThread = disp.isInitialized() && disp.isOwnerThread();
    const uint64_t now = GetTickCount64();

    std::unique_lock<std::mutex> lock(m_stateMutex);
    if (m_snapshotEpoch == 0 || (now - m_lastRefreshTick) > kStaleReadThresholdMs) {
        m_refreshRequested = true;
        m_wakeCv.notify_one();
        if (!onUiThread && m_snapshotEpoch == 0) {
            const uint64_t targetEpoch = m_snapshotEpoch + 1;
            m_doneCv.wait_for(lock, std::chrono::milliseconds(80), [this, targetEpoch]() {
                return !m_running.load(std::memory_order_acquire) || m_snapshotEpoch >= targetEpoch;
            });
        }
    }
    return m_allOpenPaths;
}

} // namespace tools3000::dialog

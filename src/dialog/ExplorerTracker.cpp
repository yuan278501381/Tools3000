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
#include <uiautomation.h>
#include <wrl/client.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "UIAutomationCore.lib")

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

bool isDirectoryPathValidFast(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\') {
        return true;  // UNC 共享路径不做同步阻塞网络探测
    }
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring readWindowTextBounded(HWND hwnd, size_t maxChars = MAX_PATH * 2) {
    if (!hwnd || !IsWindow(hwnd) || maxChars < 2) return {};
    std::wstring buf(maxChars, L'\0');
    DWORD_PTR copied = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, static_cast<WPARAM>(maxChars),
                             reinterpret_cast<LPARAM>(buf.data()),
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &copied) ||
        copied == 0) {
        return {};
    }
    buf.resize((std::min)(static_cast<size_t>(copied), maxChars - 1));
    return buf;
}

/// Win11 多标签资源管理器精准查找当前处于激活显示态的 ShellTabWindowClass（优先 IsWindowVisible）
HWND findActiveShellTabHwnd(HWND explorerTopHwnd) noexcept {
    if (!explorerTopHwnd || !IsWindow(explorerTopHwnd)) return nullptr;
    HWND firstFallback = nullptr;
    for (HWND tab = FindWindowExW(explorerTopHwnd, nullptr, L"ShellTabWindowClass", nullptr);
         tab != nullptr;
         tab = FindWindowExW(explorerTopHwnd, tab, L"ShellTabWindowClass", nullptr)) {
        if (!firstFallback) firstFallback = tab;
        if (IsWindowVisible(tab)) {
            return tab;
        }
    }
    return firstFallback;
}

/// 通过标准 Win32 子控件文本提取第三方传统文件管理器（Total Commander / Directory Opus / XYplorer / Q-Dir）的当前目录
std::string extractWin32ThirdPartyManagerFolder(HWND topHwnd, FileManagerKind kind) {
    if (!topHwnd || !IsWindow(topHwnd)) return "";

    struct ScanContext {
        FileManagerKind kind;
        std::wstring preferredPath;
        std::wstring fallbackPath;
    };
    ScanContext ctx{kind, {}, {}};

    EnumChildWindows(topHwnd, [](HWND child, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<ScanContext*>(lParam);
        if (!IsWindowVisible(child)) return TRUE;
        wchar_t cls[96]{};
        if (GetClassNameW(child, cls, static_cast<int>(std::size(cls))) <= 0) return TRUE;
        const std::wstring_view clsView(cls);

        bool isPreferredClass = false;
        bool isCandidateClass = false;
        switch (c->kind) {
            case FileManagerKind::TotalCommander:
                isPreferredClass = (clsView == L"TPathPanel");
                isCandidateClass = isPreferredClass || clsView == L"Window" || clsView == L"TMyPanel";
                break;
            case FileManagerKind::DirectoryOpus:
                isPreferredClass = (clsView == L"dopus.lister.pathfield" || clsView == L"dopus.breadcrumb");
                isCandidateClass = isPreferredClass || clsView == L"Edit";
                break;
            case FileManagerKind::Xyplorer:
                isPreferredClass = (clsView == L"ThunderRT6TextBox");
                isCandidateClass = isPreferredClass || clsView == L"Edit";
                break;
            case FileManagerKind::QDir:
                isPreferredClass = (clsView == L"Edit");
                isCandidateClass = isPreferredClass || clsView == L"ComboBox";
                break;
            default:
                isCandidateClass = (clsView == L"Edit");
                break;
        }
        if (!isCandidateClass) return TRUE;

        const std::wstring raw = readWindowTextBounded(child);
        const std::wstring extracted = extractDirectoryFromFileManagerText(c->kind, raw);
        if (!extracted.empty() && isDirectoryPathValidFast(extracted)) {
            if (isPreferredClass) {
                if (c->preferredPath.empty()) c->preferredPath = extracted;
            } else if (c->fallbackPath.empty()) {
                c->fallbackPath = extracted;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.preferredPath.empty()) {
        return tools3000::core::WinUtils::wstringToUtf8(ctx.preferredPath);
    }
    if (!ctx.fallbackPath.empty()) {
        return tools3000::core::WinUtils::wstringToUtf8(ctx.fallbackPath);
    }

    // 兜底检查顶层窗口标题（如 XYplorer / Directory Opus 配置了标题栏显示完整路径）
    const std::wstring title = readWindowTextBounded(topHwnd);
    const std::wstring fromTitle = extractDirectoryFromFileManagerText(kind, title);
    if (!fromTitle.empty() && isDirectoryPathValidFast(fromTitle)) {
        return tools3000::core::WinUtils::wstringToUtf8(fromTitle);
    }
    return "";
}

/// 仅在按下跳转快捷键且最近激活窗口是现代第三方管理器（Files / OneCommander / DoubleCmd / FreeCommander）时按需跑一次有界 UIA 读取
std::string extractModernThirdPartyManagerFolderViaUia(HWND topHwnd, FileManagerKind kind) {
    if (!topHwnd || !IsWindow(topHwnd)) return "";
    // 先尝试 0 开销标题栏匹配
    const std::wstring title = readWindowTextBounded(topHwnd);
    const std::wstring fromTitle = extractDirectoryFromFileManagerText(kind, title);
    if (!fromTitle.empty() && isDirectoryPathValidFast(fromTitle)) {
        return tools3000::core::WinUtils::wstringToUtf8(fromTitle);
    }

    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))) || !uia) {
        return "";
    }
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(topHwnd, &root)) || !root) {
        return "";
    }
    VARIANT varEdit;
    VariantInit(&varEdit);
    varEdit.vt = VT_I4;
    varEdit.lVal = UIA_EditControlTypeId;
    ComPtr<IUIAutomationCondition> editCond;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, varEdit, &editCond)) || !editCond) {
        return "";
    }
    ComPtr<IUIAutomationElementArray> edits;
    if (FAILED(root->FindAll(TreeScope_Descendants, editCond.Get(), &edits)) || !edits) {
        return "";
    }
    int count = 0;
    edits->get_Length(&count);
    const int capped = (std::min)(count, 24);
    for (int i = 0; i < capped; ++i) {
        ComPtr<IUIAutomationElement> elem;
        if (FAILED(edits->GetElement(i, &elem)) || !elem) continue;

        ComPtr<IUIAutomationValuePattern> valPat;
        if (SUCCEEDED(elem->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&valPat))) && valPat) {
            BSTR bstr = nullptr;
            if (SUCCEEDED(valPat->get_CurrentValue(&bstr)) && bstr) {
                std::wstring val(bstr);
                SysFreeString(bstr);
                const std::wstring dir = extractDirectoryFromFileManagerText(kind, val);
                if (!dir.empty() && isDirectoryPathValidFast(dir)) {
                    return tools3000::core::WinUtils::wstringToUtf8(dir);
                }
            }
        }
        BSTR nameBstr = nullptr;
        if (SUCCEEDED(elem->get_CurrentName(&nameBstr)) && nameBstr) {
            std::wstring name(nameBstr);
            SysFreeString(nameBstr);
            const std::wstring dir = extractDirectoryFromFileManagerText(kind, name);
            if (!dir.empty() && isDirectoryPathValidFast(dir)) {
                return tools3000::core::WinUtils::wstringToUtf8(dir);
            }
        }
    }
    return "";
}

std::vector<std::pair<HWND, FileManagerKind>> enumerateThirdPartyManagerWindows() {
    std::vector<std::pair<HWND, FileManagerKind>> out;
    for (HWND w = GetTopWindow(nullptr); w != nullptr; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w)) continue;
        const FileManagerKind kind = ExplorerTracker::detectFileManagerKind(w);
        if (kind != FileManagerKind::None && kind != FileManagerKind::WindowsExplorer) {
            out.emplace_back(w, kind);
            if (out.size() >= 12) break;
        }
    }
    return out;
}

bool isFileManagerTopWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    return ExplorerTracker::detectFileManagerKind(hwnd) != FileManagerKind::None;
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

FileManagerKind ExplorerTracker::detectFileManagerKind(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return FileManagerKind::None;
    wchar_t className[96]{};
    const int clsLen = GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const std::wstring_view clsView = (clsLen > 0) ? std::wstring_view(className, static_cast<size_t>(clsLen)) : std::wstring_view{};
    if (clsView == L"CabinetWClass" || clsView == L"ExploreWClass") {
        return FileManagerKind::WindowsExplorer;
    }
    if (clsView == L"TTOTAL_CMD") {
        return FileManagerKind::TotalCommander;
    }
    if (clsView == L"dopus.lister") {
        return FileManagerKind::DirectoryOpus;
    }
    const std::string procUtf8 = tools3000::core::WinUtils::getProcessNameFromWindow(hwnd);
    if (procUtf8.empty()) return FileManagerKind::None;
    const std::wstring procW = tools3000::core::WinUtils::utf8ToWstring(procUtf8);
    return classifyFileManagerWindow(clsView, procW);
}

void ExplorerTracker::notifyFileManagerActivated(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (detectFileManagerKind(hwnd) == FileManagerKind::None) return;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_recentManagerHwnds.erase(
        std::remove_if(m_recentManagerHwnds.begin(), m_recentManagerHwnds.end(),
                       [hwnd](HWND existing) { return existing == hwnd || !IsWindow(existing); }),
        m_recentManagerHwnds.end());
    m_recentManagerHwnds.insert(m_recentManagerHwnds.begin(), hwnd);
    if (m_recentManagerHwnds.size() > 16) {
        m_recentManagerHwnds.resize(16);
    }
}

void ExplorerTracker::setPathChangedCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_onPathChanged = std::move(callback);
}

void ExplorerTracker::workerLoop() {
    ScopedComApartment comGuard;
    while (m_running.load(std::memory_order_acquire)) {
        bool wantUia = false;
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
            wantUia = m_freshUiaRequested;
            m_freshUiaRequested = false;
        }

        refreshSnapshotOnStaThread(wantUia);
    }
}

void ExplorerTracker::refreshSnapshotOnStaThread(bool includeOnDemandUiaManagers) {
    std::vector<std::pair<HWND, std::string>> nextPerWindow;
    std::vector<std::string> nextAllPaths;

    auto recordPath = [&](HWND topHwnd, const std::string& path) {
        if (path.empty()) return;
        if (std::find(nextAllPaths.begin(), nextAllPaths.end(), path) == nextAllPaths.end()) {
            nextAllPaths.push_back(path);
        }
        if (topHwnd) {
            auto it = std::find_if(
                nextPerWindow.begin(), nextPerWindow.end(),
                [topHwnd](const auto& kv) { return kv.first == topHwnd; });
            if (it == nextPerWindow.end()) {
                nextPerWindow.emplace_back(topHwnd, path);
            } else {
                it->second = path;
            }
        }
    };

    ComPtr<IShellWindows> shellWindows;
    const HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows));
    if (SUCCEEDED(hr) && shellWindows) {
        long count = 0;
        shellWindows->get_Count(&count);

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
                                HWND activeTabHwnd = findActiveShellTabHwnd(topHwnd);
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

                                // 严格仅将当前可见激活 Tab 记录为该窗口的活动目录；
                                // 若当前激活 Tab 为「主页/此电脑」虚拟页，则不拿后台旧 Tab 顶替，留给 LRU 自动顺延次近有效窗口。
                                if (isExactActiveTab) {
                                    recordPath(topHwnd, path);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 扫描已打开的主流第三方文件管理器（Total Commander / Directory Opus / XYplorer / Q-Dir / Files / OneCommander）
    const auto thirdPartyWindows = enumerateThirdPartyManagerWindows();
    for (const auto& [topHwnd, kind] : thirdPartyWindows) {
        if (!m_running.load(std::memory_order_acquire)) break;
        std::string path;
        if (kind == FileManagerKind::ModernThirdParty) {
            if (includeOnDemandUiaManagers) {
                path = extractModernThirdPartyManagerFolderViaUia(topHwnd, kind);
            } else {
                const std::wstring title = readWindowTextBounded(topHwnd);
                const std::wstring fromTitle = extractDirectoryFromFileManagerText(kind, title);
                if (!fromTitle.empty() && isDirectoryPathValidFast(fromTitle)) {
                    path = tools3000::core::WinUtils::wstringToUtf8(fromTitle);
                }
            }
        } else {
            path = extractWin32ThirdPartyManagerFolder(topHwnd, kind);
        }
        if (!path.empty()) {
            recordPath(topHwnd, path);
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

std::string ExplorerTracker::selectBestActivePathLocked() const {
    auto lookupWindowPath = [&](HWND targetHwnd) -> std::string {
        if (!targetHwnd || !IsWindow(targetHwnd)) return "";
        for (const auto& kv : m_perWindowActivePaths) {
            if (kv.first == targetHwnd && !kv.second.empty()) {
                return kv.second;
            }
        }
        return "";
    };

    // 1. 优先按最近激活文件管理器 LRU 顺序（即使刚被顺手最小化也能精确命中，遇到主页/此电脑自动顺延）
    for (HWND lruHwnd : m_recentManagerHwnds) {
        const std::string p = lookupWindowPath(lruHwnd);
        if (!p.empty()) return p;
    }

    // 2. Z-order 兜底（覆盖启动 Tools3000 前已打开但尚未切过焦点的文件管理器）
    const HWND fgHwnd = GetForegroundWindow();
    if (isFileManagerTopWindow(fgHwnd)) {
        const std::string p = lookupWindowPath(fgHwnd);
        if (!p.empty()) return p;
    }
    for (HWND w = GetTopWindow(nullptr); w != nullptr; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!isFileManagerTopWindow(w)) continue;
        const std::string p = lookupWindowPath(w);
        if (!p.empty()) return p;
    }

    if (!m_perWindowActivePaths.empty()) {
        return m_perWindowActivePaths.front().second;
    }
    if (!m_allOpenPaths.empty()) {
        return m_allOpenPaths.front();
    }
    return "";
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
        return selectBestActivePathLocked();
    }
}

std::string ExplorerTracker::getActiveExplorerPathFresh(uint32_t maxWaitMs) {
    ensureStarted();

    const auto& disp = tools3000::core::MainThreadDispatcher::instance();
    const bool onUiThread = disp.isInitialized() && disp.isOwnerThread();

    std::unique_lock<std::mutex> lock(m_stateMutex);
    const uint64_t targetEpoch = m_snapshotEpoch + 1;
    m_refreshRequested = true;
    m_freshUiaRequested = true;
    m_wakeCv.notify_one();
    if (!onUiThread) {
        m_doneCv.wait_for(lock, std::chrono::milliseconds(maxWaitMs), [this, targetEpoch]() {
            return !m_running.load(std::memory_order_acquire) || m_snapshotEpoch >= targetEpoch;
        });
    }
    return selectBestActivePathLocked();
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

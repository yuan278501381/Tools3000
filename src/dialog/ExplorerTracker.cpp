/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#include "ExplorerTracker.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include <shlobj.h>
#include <exdisp.h>
#include <shlwapi.h>
#include <wrl/client.h>

#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

namespace tools3000::dialog {

namespace {

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

} // namespace

ExplorerTracker& ExplorerTracker::instance() {
    static ExplorerTracker s_instance;
    return s_instance;
}

std::string ExplorerTracker::normalizeFolderPath(const std::wstring& rawPath) {
    if (rawPath.empty()) return "";

    wchar_t localPath[MAX_PATH] = {0};
    DWORD pathLen = MAX_PATH;

    if (rawPath.rfind(L"file://", 0) == 0) {
        if (SUCCEEDED(PathCreateFromUrlW(rawPath.c_str(), localPath, &pathLen, 0))) {
            return tools3000::core::WinUtils::wstringToUtf8(localPath);
        }
    }

    // 已经是本地路径
    if (GetFileAttributesW(rawPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return tools3000::core::WinUtils::wstringToUtf8(rawPath);
    }

    return tools3000::core::WinUtils::wstringToUtf8(rawPath);
}

std::string ExplorerTracker::getActiveExplorerPath() {
    ScopedComApartment comGuard;
    HWND fgHwnd = GetForegroundWindow();
    HWND activeExplorer = nullptr;

    // 1. 优先检查前台窗口是否为资源管理器；若前台是文件对话框，则沿 Z-Order 向下寻找最近激活的资源管理器主窗口
    auto isExplorerWindow = [](HWND hwnd) -> bool {
        if (!hwnd || !IsWindowVisible(hwnd)) return false;
        wchar_t className[64] = {0};
        if (GetClassNameW(hwnd, className, 64) <= 0) return false;
        return wcscmp(className, L"CabinetWClass") == 0 || wcscmp(className, L"ExploreWClass") == 0;
    };

    if (isExplorerWindow(fgHwnd)) {
        activeExplorer = fgHwnd;
    } else {
        for (HWND w = GetTopWindow(nullptr); w != nullptr; w = GetWindow(w, GW_HWNDNEXT)) {
            if (isExplorerWindow(w)) {
                activeExplorer = w;
                break;
            }
        }
    }

    // 2. 通过 IShellWindows 遍历，兼容 Windows 11 多标签页资源管理器 (ShellTabWindowClass)
    ComPtr<IShellWindows> shellWindows;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr) || !shellWindows) {
        return "";
    }

    long count = 0;
    shellWindows->get_Count(&count);
    if (count <= 0) return "";

    HWND activeTabHwnd = activeExplorer
        ? FindWindowExW(activeExplorer, nullptr, L"ShellTabWindowClass", nullptr)
        : nullptr;

    std::string fallbackPath;
    std::string activeWindowFallbackPath;

    for (long i = 0; i < count; ++i) {
        VARIANT vIndex;
        VariantInit(&vIndex);
        vIndex.vt = VT_I4;
        vIndex.lVal = i;

        ComPtr<IDispatch> disp;
        if (SUCCEEDED(shellWindows->Item(vIndex, &disp)) && disp) {
            ComPtr<IWebBrowserApp> app;
            if (SUCCEEDED(disp.As(&app)) && app) {
                HWND hwnd = nullptr;
                app->get_HWND(reinterpret_cast<SHANDLE_PTR*>(&hwnd));

                BSTR bstrUrl = nullptr;
                if (SUCCEEDED(app->get_LocationURL(&bstrUrl)) && bstrUrl) {
                    std::wstring url(bstrUrl);
                    SysFreeString(bstrUrl);

                    std::string path = normalizeFolderPath(url);
                    if (!path.empty()) {
                        if (activeExplorer && hwnd == activeExplorer) {
                            // Windows 11 多标签校验：通过 IServiceProvider -> IShellBrowser 获取具体 Tab 的 HWND
                            ComPtr<IServiceProvider> sp;
                            if (activeTabHwnd && SUCCEEDED(app.As(&sp)) && sp) {
                                ComPtr<IShellBrowser> shellBrowser;
                                if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser))) && shellBrowser) {
                                    HWND browserHwnd = nullptr;
                                    if (SUCCEEDED(shellBrowser->GetWindow(&browserHwnd)) && browserHwnd) {
                                        if (browserHwnd == activeTabHwnd || IsWindowVisible(browserHwnd)) {
                                            return path;
                                        }
                                    }
                                }
                            } else {
                                return path;
                            }
                            if (activeWindowFallbackPath.empty()) {
                                activeWindowFallbackPath = path;
                            }
                        }
                        if (fallbackPath.empty()) {
                            fallbackPath = path;
                        }
                    }
                }
            }
        }
    }

    return !activeWindowFallbackPath.empty() ? activeWindowFallbackPath : fallbackPath;
}

std::vector<std::string> ExplorerTracker::getAllOpenExplorerPaths() {
    ScopedComApartment comGuard;
    std::vector<std::string> paths;

    ComPtr<IShellWindows> shellWindows;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr) || !shellWindows) {
        return paths;
    }

    long count = 0;
    shellWindows->get_Count(&count);

    for (long i = 0; i < count; ++i) {
        VARIANT vIndex;
        VariantInit(&vIndex);
        vIndex.vt = VT_I4;
        vIndex.lVal = i;

        ComPtr<IDispatch> disp;
        if (SUCCEEDED(shellWindows->Item(vIndex, &disp)) && disp) {
            ComPtr<IWebBrowserApp> app;
            if (SUCCEEDED(disp.As(&app)) && app) {
                BSTR bstrUrl = nullptr;
                if (SUCCEEDED(app->get_LocationURL(&bstrUrl)) && bstrUrl) {
                    std::wstring url(bstrUrl);
                    SysFreeString(bstrUrl);

                    std::string path = normalizeFolderPath(url);
                    if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end()) {
                        paths.push_back(path);
                    }
                }
            }
        }
    }

    return paths;
}

} // namespace tools3000::dialog

// ─────────────────────────────────────────────────────────────────────────────
// WinUtils.cpp — Windows API 常用操作封装实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/utils/WinUtils.h"
#include "core/logger/Logger.h"

#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <vector>
#include <unordered_set>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <system_error>
#include <atomic>
#include <string_view>

#pragma comment(lib, "gdiplus.lib")

namespace tools3000::core {

std::filesystem::path WinUtils::getExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

bool WinUtils::isPortableMode() {
    const auto exeDir = getExeDirectory();
    std::error_code ec;
    if (std::filesystem::is_directory(exeDir / L".tools3000", ec)) return true;
    if (std::filesystem::is_directory(exeDir / L"data", ec)) return true;
    if (std::filesystem::is_directory(exeDir / L"portable_data", ec)) return true;
    return false;
}

std::filesystem::path WinUtils::getAppDataDirectory() {
    const auto exeDir = getExeDirectory();
    std::error_code ec;

    // CI/生命周期门禁可为待测进程和它派生的索引进程提供完全隔离的数据根。
    // 只接受绝对路径，避免主进程与服务因工作目录不同而落到两个位置。
    const DWORD overrideLength = GetEnvironmentVariableW(L"TOOLS3000_DATA_ROOT", nullptr, 0);
    if (overrideLength > 1) {
        std::vector<wchar_t> overrideBuffer(overrideLength);
        const DWORD copied = GetEnvironmentVariableW(
            L"TOOLS3000_DATA_ROOT", overrideBuffer.data(), overrideLength);
        if (copied > 0 && copied < overrideLength) {
            std::filesystem::path overridePath(overrideBuffer.data());
            if (overridePath.is_absolute()) {
                std::filesystem::create_directories(overridePath, ec);
                if (!ec) return overridePath;
                ec.clear();
            }
        }
    }
    
    // 1. 便携模式检测：若主程序同级目录下存在 .tools3000 或 data 目录，优先作为数据根目录
    if (std::filesystem::is_directory(exeDir / L".tools3000", ec)) {
        return exeDir / L".tools3000";
    }
    if (std::filesystem::is_directory(exeDir / L"data", ec)) {
        return exeDir / L"data";
    }
    if (std::filesystem::is_directory(exeDir / L"portable_data", ec)) {
        return exeDir / L"portable_data";
    }

    // 2. 标准系统模式：回退到 %LOCALAPPDATA%\Tools3000
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        auto dir = std::filesystem::path(path) / L"Tools3000";
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    return exeDir / L"data";
}

std::filesystem::path WinUtils::getLogDirectory() {
    auto dir = getAppDataDirectory() / L"logs";
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path WinUtils::getConfigDirectory() {
    auto dir = getAppDataDirectory() / L"config";
    std::filesystem::create_directories(dir);
    return dir;
}

namespace {
std::atomic<ULONGLONG> g_lastTrimTick{0};
constexpr ULONGLONG kTrimThrottleIntervalMs = 2000;

inline bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() && _wcsnicmp(a.data(), b.data(), a.size()) == 0;
}
} // namespace

bool WinUtils::shouldTrimProcess(std::wstring_view exeName) {
    if (exeName.empty()) return false;
    size_t lastSlash = exeName.find_last_of(L"\\/");
    if (lastSlash != std::wstring_view::npos) {
        exeName = exeName.substr(lastSlash + 1);
    }

    // 严格排除全盘搜索常驻服务，保障 286MB 核心索引与 SQLite 内存页安全 (零堆内存分配比对)
    if (equalsIgnoreCase(exeName, L"Tools3000_Service.exe")) {
        return false;
    }

    // 白名单仅修剪 WebView2 渲染管线等子进程
    if (equalsIgnoreCase(exeName, L"msedgewebview2.exe") ||
        equalsIgnoreCase(exeName, L"msedge.exe")) {
        return true;
    }

    return false;
}

void WinUtils::resetTrimThrottleForTesting() {
    g_lastTrimTick.store(0, std::memory_order_relaxed);
}

bool WinUtils::trimWorkingSet(bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force) {
        ULONGLONG last = g_lastTrimTick.load(std::memory_order_relaxed);
        do {
            if (last != 0 && (now >= last && (now - last) < kTrimThrottleIntervalMs)) {
                return false;
            }
        } while (!g_lastTrimTick.compare_exchange_weak(
            last, now, std::memory_order_acq_rel, std::memory_order_relaxed));
    } else {
        g_lastTrimTick.store(now, std::memory_order_release);
    }

    // 1. 修剪 Tools3000 宿主主进程自身物理工作集
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));

    // 2. 级联深度修剪所有子进程与孙子进程 (仅限 WebView2 浏览器进程及其派生的 GPU/Network/Renderer 渲染管线)
    const DWORD currentPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        std::vector<PROCESSENTRY32W> entries;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snapshot, &pe)) {
            do {
                entries.push_back(pe);
            } while (Process32NextW(snapshot, &pe));
        }
        CloseHandle(snapshot);

        // BFS 遍历收集以 currentPid 为根的所有符合白名单后代进程 PID
        std::unordered_set<DWORD> descendantPids;
        std::vector<DWORD> queue;
        queue.push_back(currentPid);

        size_t head = 0;
        while (head < queue.size()) {
            DWORD parentPid = queue[head++];
            for (const auto& entry : entries) {
                if (entry.th32ParentProcessID == parentPid &&
                    entry.th32ProcessID != 0 &&
                    entry.th32ProcessID != currentPid) {
                    if (!shouldTrimProcess(entry.szExeFile)) {
                        continue;
                    }
                    if (descendantPids.insert(entry.th32ProcessID).second) {
                        queue.push_back(entry.th32ProcessID);
                    }
                }
            }
        }

        // 对符合白名单的后代进程调用 SetProcessWorkingSetSize(-1, -1)
        for (DWORD pid : descendantPids) {
            HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                SetProcessWorkingSetSize(hProc, static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
                CloseHandle(hProc);
            }
        }
    }
    return true;
}

std::wstring WinUtils::processNameFromPid(DWORD pid) {
    if (pid == 0) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        std::wstring full(path, size);
        size_t slash = full.find_last_of(L"\\/");
        result = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    }
    CloseHandle(proc);
    return result;
}

std::wstring WinUtils::processPathFromPid(DWORD pid) {
    if (pid == 0) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    wchar_t path[2048] = {};
    DWORD size = static_cast<DWORD>(std::size(path));
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        result.assign(path, size);
    }
    CloseHandle(proc);
    return result;
}

std::optional<std::wstring> WinUtils::getForegroundProcessName() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return std::nullopt;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring name = processNameFromPid(pid);
    if (name.empty()) return std::nullopt;
    return name;
}

std::wstring WinUtils::getWindowClassName(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};
    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, 256);
    return className;
}

std::wstring WinUtils::getWindowTitle(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};
    int len = GetWindowTextLengthW(hwnd);
    if (len == 0) return L"";
    std::wstring title(len + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(len);
    return title;
}

std::string WinUtils::wstringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring WinUtils::utf8ToWstring(const std::string& str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), size);
    return result;
}

std::string WinUtils::getProcessNameFromWindow(HWND hwnd) {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return wstringToUtf8(processNameFromPid(pid));
}

std::wstring WinUtils::getProcessPathFromWindow(HWND hwnd) {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return processPathFromPid(pid);
}

std::string WinUtils::base64Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return "";
    DWORD len = 0;
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    if (len == 0) return "";
    std::string result(len, '\0');
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &len);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

std::string WinUtils::toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return str;
}

std::wstring WinUtils::toLower(std::wstring str) {
    std::transform(str.begin(), str.end(), str.begin(),
        [](wchar_t c){ return static_cast<wchar_t>(std::tolower(c)); });
    return str;
}

std::string WinUtils::getFileTypeIconBase64(const std::wstring& extension, bool isDirectory) {
    static std::unordered_map<std::wstring, std::string> s_iconCache;
    static std::mutex s_iconCacheMutex;

    std::wstring key = isDirectory ? L"::dir::" : toLower(extension);
    if (key.rfind(L".", 0) == 0) {
        key = key.substr(1);
    }

    {
        std::lock_guard<std::mutex> lock(s_iconCacheMutex);
        auto it = s_iconCache.find(key);
        if (it != s_iconCache.end()) return it->second;
    }

    std::wstring fakePath = isDirectory ? L"folder" : (L"dummy." + key);
    SHFILEINFOW sfi{};
    DWORD_PTR hr = SHGetFileInfoW(
        fakePath.c_str(),
        isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
        &sfi,
        sizeof(sfi),
        SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON
    );

    if (!hr || !sfi.hIcon) {
        SHGetFileInfoW(
            L"dummy",
            FILE_ATTRIBUTE_NORMAL,
            &sfi,
            sizeof(sfi),
            SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON
        );
    }

    std::string base64;
    if (sfi.hIcon) {
        ICONINFO iconInfo{};
        if (GetIconInfo(sfi.hIcon, &iconInfo)) {
            BITMAP bm{};
            if (GetObjectW(iconInfo.hbmColor ? iconInfo.hbmColor : iconInfo.hbmMask, sizeof(bm), &bm)) {
                int width = bm.bmWidth;
                int height = iconInfo.hbmColor ? bm.bmHeight : (bm.bmHeight / 2);
                if (width <= 0 || height <= 0) {
                    width = GetSystemMetrics(SM_CXSMICON);
                    height = GetSystemMetrics(SM_CYSMICON);
                }

                HDC hdcScreen = GetDC(nullptr);
                HDC hdcMem = CreateCompatibleDC(hdcScreen);

                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = width;
                bi.bmiHeader.biHeight = -height; // Top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;

                void* pBits = nullptr;
                HBITMAP hDIB = CreateDIBSection(hdcMem, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
                if (hDIB && pBits) {
                    HGDIOBJ hOldBmp = SelectObject(hdcMem, hDIB);
                    memset(pBits, 0, width * height * 4);

                    // 使用 Windows 底层权威 DrawIconEx 渲染合成
                    DrawIconEx(hdcMem, 0, 0, sfi.hIcon, width, height, 0, nullptr, DI_NORMAL);

                    uint32_t* pixels = static_cast<uint32_t*>(pBits);
                    bool hasValidAlpha = false;

                    for (int i = 0; i < width * height; ++i) {
                        if ((pixels[i] & 0xFF000000) != 0) {
                            hasValidAlpha = true;
                            break;
                        }
                    }

                    // 如果原图标是传统的 1-bit / 24-bit 掩码图标（Alpha 全为 0），利用 hbmMask 智能重建真实 Alpha 通道，彻底消灭黑边
                    if (!hasValidAlpha && iconInfo.hbmMask) {
                        HDC hdcMask = CreateCompatibleDC(hdcScreen);
                        HGDIOBJ hOldMask = SelectObject(hdcMask, iconInfo.hbmMask);

                        for (int y = 0; y < height; ++y) {
                            for (int x = 0; x < width; ++x) {
                                COLORREF maskColor = GetPixel(hdcMask, x, y);
                                int idx = y * width + x;
                                if (maskColor == 0x00FFFFFF) {
                                    pixels[idx] = 0x00000000; // 掩码白色 = 完全透明
                                } else {
                                    pixels[idx] |= 0xFF000000; // 掩码黑色 = 完全不透明
                                }
                            }
                        }

                        SelectObject(hdcMask, hOldMask);
                        DeleteDC(hdcMask);
                    }

                    // 通过 GDI+ 导出为 32 位透明 PNG
                    ULONG_PTR gdiplusToken = 0;
                    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
                    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) == Gdiplus::Ok) {
                        {
                            Gdiplus::Bitmap gdiBmp(width, height, width * 4, PixelFormat32bppARGB, static_cast<BYTE*>(pBits));
                            if (gdiBmp.GetLastStatus() == Gdiplus::Ok) {
                                CLSID pngClsid{};
                                UINT num = 0, size = 0;
                                Gdiplus::GetImageEncodersSize(&num, &size);
                                if (size > 0) {
                                    auto pCodecsBuf = std::make_unique<uint8_t[]>(size);
                                    auto* pCodecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(pCodecsBuf.get());
                                    if (Gdiplus::GetImageEncoders(num, size, pCodecs) == Gdiplus::Ok) {
                                        for (UINT j = 0; j < num; ++j) {
                                            if (wcscmp(pCodecs[j].MimeType, L"image/png") == 0) {
                                                pngClsid = pCodecs[j].Clsid;
                                                break;
                                            }
                                        }
                                    }
                                }

                                if (pngClsid != CLSID{}) {
                                    IStream* pStream = nullptr;
                                    if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)) && pStream) {
                                        if (gdiBmp.Save(pStream, &pngClsid, nullptr) == Gdiplus::Ok) {
                                            HGLOBAL hMem = nullptr;
                                            if (SUCCEEDED(GetHGlobalFromStream(pStream, &hMem)) && hMem) {
                                                const SIZE_T memSize = GlobalSize(hMem);
                                                const void* pData = GlobalLock(hMem);
                                                if (pData && memSize > 0) {
                                                    std::vector<uint8_t> buffer(static_cast<const uint8_t*>(pData),
                                                                                static_cast<const uint8_t*>(pData) + memSize);
                                                    GlobalUnlock(hMem);
                                                    base64 = "data:image/png;base64," + base64Encode(buffer);
                                                }
                                            }
                                        }
                                        pStream->Release();
                                    }
                                }
                            }
                        }
                        Gdiplus::GdiplusShutdown(gdiplusToken);
                    }

                    SelectObject(hdcMem, hOldBmp);
                    DeleteObject(hDIB);
                }
                DeleteDC(hdcMem);
                ReleaseDC(nullptr, hdcScreen);
            }
            if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
            if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
        }
        DestroyIcon(sfi.hIcon);
    }

    {
        std::lock_guard<std::mutex> lock(s_iconCacheMutex);
        s_iconCache[key] = base64;
    }
    return base64;
}

bool WinUtils::copyToClipboard(const std::wstring& wtext, HWND owner) {
    if (wtext.empty()) return false;

    // EmptyClipboard assigns ownership to the HWND passed to OpenClipboard.
    // Passing nullptr leaves the clipboard without an owner and makes the
    // following SetClipboardData call fail, so provide a short-lived
    // message-only window when the caller has no suitable UI window.
    HWND temporaryOwner = nullptr;
    if (!owner || !IsWindow(owner)) {
        temporaryOwner = CreateWindowExW(
            0, L"STATIC", L"Tools3000 Clipboard Owner", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        owner = temporaryOwner;
    }
    if (!owner) return false;
    struct TemporaryOwnerGuard {
        HWND hwnd;
        ~TemporaryOwnerGuard() { if (hwnd) DestroyWindow(hwnd); }
    } temporaryOwnerGuard{temporaryOwner};

    // 剪贴板可能被其他进程瞬时独占（如剪贴板管理器、Office），进行有界重试
    bool opened = false;
    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(owner)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) return false;
    struct ClipboardGuard {
        ~ClipboardGuard() { CloseClipboard(); }
    } clipboardGuard;

    if (!EmptyClipboard()) return false;
    const size_t byteCount = (wtext.length() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!hMem) return false;

    void* dest = GlobalLock(hMem);
    if (!dest) {
        GlobalFree(hMem);
        return false;
    }
    memcpy(dest, wtext.c_str(), byteCount);
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        return false;
    }
    return true;
}

bool WinUtils::copyToClipboard(const std::string& text, HWND owner) {
    return copyToClipboard(utf8ToWstring(text), owner);
}

bool WinUtils::copyFileToClipboard(const std::wstring& filePath, HWND owner) {
    if (filePath.empty()) return false;
    DWORD attrs = GetFileAttributesW(filePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;

    size_t charCount = filePath.size() + 2;
    size_t totalBytes = sizeof(DROPFILES) + charCount * sizeof(wchar_t);
    HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, totalBytes);
    if (!hGlobal) return false;

    auto* drop = static_cast<DROPFILES*>(GlobalLock(hGlobal));
    if (!drop) {
        GlobalFree(hGlobal);
        return false;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t* dest = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(drop) + sizeof(DROPFILES));
    memcpy(dest, filePath.c_str(), filePath.size() * sizeof(wchar_t));
    dest[filePath.size()] = L'\0';
    dest[filePath.size() + 1] = L'\0';
    GlobalUnlock(hGlobal);

    bool opened = false;
    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(owner)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) {
        GlobalFree(hGlobal);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_HDROP, hGlobal)) {
        GlobalFree(hGlobal);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

bool WinUtils::copyGifToClipboard(const std::wstring& filePath, HWND owner) {
    if (filePath.empty()) return false;
    DWORD attrs = GetFileAttributesW(filePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;

    // 1. 准备标准 CF_HDROP 拖拽文件路径
    size_t charCount = filePath.size() + 2;
    size_t dropBytes = sizeof(DROPFILES) + charCount * sizeof(wchar_t);
    HGLOBAL hDropGlobal = GlobalAlloc(GHND | GMEM_SHARE, dropBytes);
    if (!hDropGlobal) return false;

    auto* drop = static_cast<DROPFILES*>(GlobalLock(hDropGlobal));
    if (!drop) {
        GlobalFree(hDropGlobal);
        return false;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t* dest = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(drop) + sizeof(DROPFILES));
    memcpy(dest, filePath.c_str(), filePath.size() * sizeof(wchar_t));
    dest[filePath.size()] = L'\0';
    dest[filePath.size() + 1] = L'\0';
    GlobalUnlock(hDropGlobal);

    // 2. 准备二进制 GIF 格式数据 (供高级 IM / 浏览器等直接读取原生动图流)
    HGLOBAL hGifGlobal = nullptr;
    UINT formatGif = RegisterClipboardFormatW(L"GIF");
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD fileSize = GetFileSize(hFile, nullptr);
        if (fileSize > 0 && fileSize < 50 * 1024 * 1024) {
            hGifGlobal = GlobalAlloc(GMEM_MOVEABLE, fileSize);
            if (hGifGlobal) {
                void* gifBuf = GlobalLock(hGifGlobal);
                if (gifBuf) {
                    DWORD bytesRead = 0;
                    ReadFile(hFile, gifBuf, fileSize, &bytesRead, nullptr);
                    GlobalUnlock(hGifGlobal);
                } else {
                    GlobalFree(hGifGlobal);
                    hGifGlobal = nullptr;
                }
            }
        }
        CloseHandle(hFile);
    }

    // 3. 准备首帧标准底向上 (Bottom-Up) CF_DIB
    // 微信在 Windows 上粘贴时：检测到包含图像 DIB 且附带指向 .gif 文件的 CF_HDROP，
    // 会直接作为动态 GIF 动图在聊天会话中以可循环播放的富媒体发送，而不是静态图或普通文件。
    HGLOBAL hDibGlobal = nullptr;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) == Gdiplus::Ok) {
        {
            Gdiplus::Bitmap gifBmp(filePath.c_str());
            if (gifBmp.GetLastStatus() == Gdiplus::Ok && gifBmp.GetWidth() > 0 && gifBmp.GetHeight() > 0) {
                int width = static_cast<int>(gifBmp.GetWidth());
                int height = static_cast<int>(gifBmp.GetHeight());
                Gdiplus::Rect gdiRect(0, 0, width, height);
                Gdiplus::BitmapData bmpData{};
                if (gifBmp.LockBits(&gdiRect, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &bmpData) == Gdiplus::Ok) {
                    BITMAPINFOHEADER bi{};
                    bi.biSize = sizeof(bi);
                    bi.biWidth = width;
                    bi.biHeight = height; // 正数 = 规范底向上 (Bottom-Up DIB)
                    bi.biPlanes = 1;
                    bi.biBitCount = 24;
                    bi.biCompression = BI_RGB;
                    int rowBytes = width * 3;
                    int stride = (rowBytes + 3) & ~3;
                    bi.biSizeImage = stride * height;

                    size_t totalBytes = sizeof(BITMAPINFOHEADER) + bi.biSizeImage;
                    hDibGlobal = GlobalAlloc(GMEM_MOVEABLE, totalBytes);
                    if (hDibGlobal) {
                        auto* pMem = static_cast<uint8_t*>(GlobalLock(hDibGlobal));
                        if (pMem) {
                            memcpy(pMem, &bi, sizeof(bi));
                            uint8_t* pPixels = pMem + sizeof(bi);
                            const auto* srcPtr = static_cast<const uint8_t*>(bmpData.Scan0);
                            for (int y = 0; y < height; ++y) {
                                int srcY = height - 1 - y;
                                memcpy(pPixels + y * stride, srcPtr + srcY * bmpData.Stride, rowBytes);
                            }
                            GlobalUnlock(hDibGlobal);
                        } else {
                            GlobalFree(hDibGlobal);
                            hDibGlobal = nullptr;
                        }
                    }
                    gifBmp.UnlockBits(&bmpData);
                }
            }
        }
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    // 4. 打开剪贴板并同时写入所有复合格式
    bool opened = false;
    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(owner)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) {
        if (hDropGlobal) GlobalFree(hDropGlobal);
        if (hGifGlobal) GlobalFree(hGifGlobal);
        if (hDibGlobal) GlobalFree(hDibGlobal);
        return false;
    }

    EmptyClipboard();

    if (hDropGlobal) {
        if (!SetClipboardData(CF_HDROP, hDropGlobal)) {
            GlobalFree(hDropGlobal);
        }
    }
    if (formatGif != 0 && hGifGlobal) {
        if (!SetClipboardData(formatGif, hGifGlobal)) {
            GlobalFree(hGifGlobal);
        }
    }
    if (hDibGlobal) {
        if (!SetClipboardData(CF_DIB, hDibGlobal)) {
            GlobalFree(hDibGlobal);
        }
    }

    CloseClipboard();
    return true;
}

bool WinUtils::showInExplorer(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    std::wstring params = L"/select,\"" + filePath + L"\"";
    HINSTANCE hInst = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(hInst) > 32;
}

void WinUtils::enableHighDpiSupport() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

RECT WinUtils::getVirtualScreenPhysicalBounds() {
    RECT virtualBounds = { 0, 0, 0, 0 };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT, LPARAM dwData) -> BOOL {
        MONITORINFO info;
        info.cbSize = sizeof(MONITORINFO);
        if (GetMonitorInfoW(MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY), &info)) {
            RECT* bounds = reinterpret_cast<RECT*>(dwData);
            bounds->left = std::min(bounds->left, info.rcMonitor.left);
            bounds->top = std::min(bounds->top, info.rcMonitor.top);
            bounds->right = std::max(bounds->right, info.rcMonitor.right);
            bounds->bottom = std::max(bounds->bottom, info.rcMonitor.bottom);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&virtualBounds));
    
    // 如果没有获取到，回退到 GetSystemMetrics
    if (virtualBounds.left == 0 && virtualBounds.right == 0 && 
        virtualBounds.top == 0 && virtualBounds.bottom == 0) {
        virtualBounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        virtualBounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        virtualBounds.right = virtualBounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        virtualBounds.bottom = virtualBounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    return virtualBounds;
}

float WinUtils::getDpiScale(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return dpi / 96.0f;
}

bool WinUtils::allowWindowCapture(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    SetLastError(ERROR_SUCCESS);
    return SetWindowDisplayAffinity(hwnd, WDA_NONE) != FALSE;
}

bool WinUtils::excludeWindowFromCapture(HWND hwnd) {
    // 零隐私防截底层策略 (Zero WDA_EXCLUDEFROMCAPTURE)：
    // 用户明确要求所有界面都支持被截图，绝不开启隐私防截，统一重定向至 allowWindowCapture (WDA_NONE)
    return allowWindowCapture(hwnd);
}

bool WinUtils::isDesktopWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    std::wstring cls = getWindowClassName(hwnd);
    if (cls == L"Progman" || cls == L"WorkerW") return true;
    HWND parent = GetParent(hwnd);
    if (parent) {
        std::wstring pCls = getWindowClassName(parent);
        if (pCls == L"Progman" || pCls == L"WorkerW") return true;
    }
    return false;
}

bool WinUtils::isTaskbarWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    std::wstring cls = getWindowClassName(root);
    return (cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd");
}

bool WinUtils::isSystemLanguageChinese() {
    LANGID langId = GetUserDefaultUILanguage();
    return PRIMARYLANGID(langId) == LANG_CHINESE;
}

bool WinUtils::isWindowFullscreen(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;

    // 排除桌面、Shell 窗口、隐藏窗口、最小化窗口与鼠标穿透层
    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow() ||
        !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }
    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((exStyle & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) == (WS_EX_LAYERED | WS_EX_TRANSPARENT)) {
        return false;
    }

    std::wstring className = getWindowClassName(hwnd);
    if (className == L"Progman" || className == L"WorkerW" ||
        className == L"Shell_TrayWnd" || className == L"Shell_SecondaryTrayWnd") {
        return false;
    }

    // 获取窗口所在的显示器
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!hMon) return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(hMon, &mi)) return false;

    // 允许 2px 容差，兼容 DWM Focus Assist 避让缩进 (vw-1, vh-1) 与高分屏 DPI 舍入误差
    constexpr LONG kFullscreenTolerancePx = 2;

    // 若具有标准标题栏（WS_CAPTION），常规最大化窗口 (IsZoomed) 非全屏；
    // 但部分 DXGI / SDL2 全屏游戏保留了 WS_CAPTION 样式位且未处于最大化状态，其客户区覆盖整个物理显示器
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) {
        if (IsZoomed(hwnd)) {
            return false;
        }
        RECT rcClient{};
        if (!GetClientRect(hwnd, &rcClient)) return false;
        POINT ptTopLeft{rcClient.left, rcClient.top};
        POINT ptBottomRight{rcClient.right, rcClient.bottom};
        if (!ClientToScreen(hwnd, &ptTopLeft) || !ClientToScreen(hwnd, &ptBottomRight)) {
            return false;
        }
        return (ptTopLeft.x <= mi.rcMonitor.left + kFullscreenTolerancePx &&
                ptTopLeft.y <= mi.rcMonitor.top + kFullscreenTolerancePx &&
                ptBottomRight.x >= mi.rcMonitor.right - kFullscreenTolerancePx &&
                ptBottomRight.y >= mi.rcMonitor.bottom - kFullscreenTolerancePx);
    }

    RECT rcWindow{};
    if (!GetWindowRect(hwnd, &rcWindow)) return false;

    // 判定无边框窗口是否覆盖整个物理显示器区域（含 2px 容差）
    return (rcWindow.left <= mi.rcMonitor.left + kFullscreenTolerancePx &&
            rcWindow.top <= mi.rcMonitor.top + kFullscreenTolerancePx &&
            rcWindow.right >= mi.rcMonitor.right - kFullscreenTolerancePx &&
            rcWindow.bottom >= mi.rcMonitor.bottom - kFullscreenTolerancePx);
}

namespace {

std::atomic<int> g_fullscreenPolicyMode{static_cast<int>(FullscreenPolicyMode::Smart)};

inline bool classNameStartsWith(std::wstring_view cls, std::wstring_view prefix) noexcept {
    return cls.size() >= prefix.size() && cls.substr(0, prefix.size()) == prefix;
}

inline bool isTabbedBrowserClassName(std::wstring_view cls) noexcept {
    return classNameStartsWith(cls, L"Chrome_WidgetWin") ||
           cls == L"MozillaWindowClass" ||
           cls == L"CabinetWClass";
}

inline bool isWebBrowserClassName(std::wstring_view cls) noexcept {
    return classNameStartsWith(cls, L"Chrome_WidgetWin") ||
           cls == L"MozillaWindowClass";
}

inline bool isTools3000UiClassName(std::wstring_view cls) noexcept {
    constexpr std::wstring_view kPrefix = L"Tools3000_";
    return cls.size() >= kPrefix.size() && cls.substr(0, kPrefix.size()) == kPrefix;
}

inline wchar_t asciiLowerWide(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch - L'A' + L'a');
    return ch;
}

inline char asciiLowerChar(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch - 'A' + 'a');
    return ch;
}

inline bool equalsIgnoreCaseAscii(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLowerWide(a[i]) != asciiLowerWide(b[i])) return false;
    }
    return true;
}

inline bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLowerChar(a[i]) != asciiLowerChar(b[i])) return false;
    }
    return true;
}

inline bool endsWithIgnoreCaseAscii(std::wstring_view str, std::wstring_view suffix) noexcept {
    if (str.size() < suffix.size()) return false;
    return equalsIgnoreCaseAscii(str.substr(str.size() - suffix.size()), suffix);
}

inline bool containsIgnoreCaseNormalizedPath(std::wstring_view path, std::wstring_view needle) noexcept {
    if (needle.empty() || path.size() < needle.size()) return false;
    const size_t limit = path.size() - needle.size();
    for (size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            wchar_t pc = asciiLowerWide(path[i + j]);
            if (pc == L'/') pc = L'\\';
            wchar_t nc = asciiLowerWide(needle[j]);
            if (nc == L'/') nc = L'\\';
            if (pc != nc) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

inline std::wstring_view extractFileNameView(std::wstring_view pathOrName) noexcept {
    size_t pos = pathOrName.find_last_of(L"\\/");
    return (pos == std::wstring_view::npos) ? pathOrName : pathOrName.substr(pos + 1);
}

inline bool isWebBrowserProcessName(std::wstring_view procFile) noexcept {
    static constexpr std::wstring_view kBrowsers[] = {
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe",
        L"opera.exe", L"vivaldi.exe", L"arc.exe", L"waterfox.exe",
        L"librewolf.exe", L"zen.exe", L"thorium.exe", L"chromium.exe",
        L"qqbrowser.exe", L"360chrome.exe", L"360se.exe", L"sogouexplorer.exe",
        L"2345explorer.exe", L"liebao.exe", L"maxthon.exe", L"centbrowser.exe",
        L"whale.exe", L"yandex.exe"
    };
    for (const auto& b : kBrowsers) {
        if (equalsIgnoreCaseAscii(procFile, b)) return true;
    }
    return false;
}

inline bool isElectronOrIdeProductivityProcessName(std::wstring_view procFile) noexcept {
    static constexpr std::wstring_view kProductivityApps[] = {
        L"code.exe", L"cursor.exe", L"windsurf.exe", L"devenv.exe",
        L"idea64.exe", L"clion64.exe", L"pycharm64.exe", L"webstorm64.exe",
        L"goland64.exe", L"rider64.exe", L"rustrover64.exe", L"phpstorm64.exe",
        L"datagrip64.exe", L"studio64.exe", L"sublime_text.exe", L"notepad++.exe",
        L"obsidian.exe", L"notion.exe", L"logseq.exe", L"typora.exe",
        L"feishu.exe", L"lark.exe", L"dingtalk.exe", L"wxwork.exe",
        L"wechat.exe", L"weixin.exe", L"qq.exe", L"tim.exe",
        L"slack.exe", L"teams.exe", L"zoom.exe", L"wemeetapp.exe",
        L"winword.exe", L"excel.exe", L"onenote.exe", L"outlook.exe",
        L"wps.exe", L"et.exe", L"photoshop.exe", L"illustrator.exe",
        L"acad.exe", L"blender.exe", L"unity.exe", L"unrealeditor.exe",
        L"ue4editor.exe", L"godot.exe", L"scrcpy.exe", L"explorer.exe", L"tools3000.exe"
    };
    for (const auto& app : kProductivityApps) {
        if (equalsIgnoreCaseAscii(procFile, app)) return true;
    }
    return false;
}

inline bool isRemoteDesktopOrVmProcessOrClass(std::wstring_view processName,
                                              std::wstring_view className) noexcept {
    const std::wstring_view procFile = extractFileNameView(processName);
    if (!procFile.empty()) {
        static constexpr std::wstring_view kRemoteAndVmProcesses[] = {
            L"mstsc.exe", L"msrdc.exe", L"todesk.exe", L"sunloginclient.exe",
            L"anydesk.exe", L"rustdesk.exe", L"teamviewer.exe", L"vncviewer.exe",
            L"tv_w32.exe", L"parsec.exe", L"parsecd.exe", L"moonlight.exe",
            L"splashtop.exe", L"ultraviewer_desktop.exe",
            L"vmware.exe", L"vmware-vmx.exe", L"vmware-remotemks.exe",
            L"virtualboxvm.exe", L"vmconnect.exe"
        };
        for (const auto& rp : kRemoteAndVmProcesses) {
            if (equalsIgnoreCaseAscii(procFile, rp)) return true;
        }
    }
    if (!className.empty()) {
        static constexpr std::wstring_view kRemoteAndVmClasses[] = {
            L"TscShellContainerClass", L"TSSHELLWND", L"IHWindowClass", L"OPContainerClass",
            L"SunloginMain", L"SunloginRemote", L"ToDesk_Main", L"ToDesk_Remote",
            L"anydesk", L"RustDesk", L"TeamViewer"
        };
        for (const auto& rc : kRemoteAndVmClasses) {
            if (equalsIgnoreCaseAscii(className, rc)) return true;
        }
    }
    return false;
}

inline bool isNonGameLauncherOrUtilityProcessName(std::wstring_view procFile) noexcept {
    if (isWebBrowserProcessName(procFile) ||
        isElectronOrIdeProductivityProcessName(procFile) ||
        isRemoteDesktopOrVmProcessOrClass(procFile, L"")) {
        return true;
    }
    static constexpr std::wstring_view kLaunchersAndUtilities[] = {
        L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe", L"gameoverlayui.exe",
        L"epicgameslauncher.exe", L"epicwebhelper.exe", L"unrealcefsubp.exe",
        L"wegame.exe", L"tgp_daemon.exe", L"tgp_browser.exe",
        L"battle.net.exe", L"agent.exe", L"galaxyclient.exe", L"galaxyclienthelper.exe",
        L"eaapp.exe", L"eadesktop.exe", L"eabackgroundservice.exe",
        L"upc.exe", L"ubisoftconnect.exe", L"uplaywebcore.exe",
        L"riotclientservices.exe", L"riotclientux.exe", L"riotclientuxrender.exe",
        L"leagueclient.exe", L"leagueclientux.exe", L"leagueclientuxrender.exe",
        L"wallpaper32.exe", L"wallpaper64.exe", L"ui32.exe", L"webwallpaper32.exe",
        L"losslessscaling.exe", L"obs64.exe", L"obs32.exe",
        L"discord.exe", L"kook.exe", L"yy.exe", L"teamspeak3.exe",
        L"msi afterburner.exe", L"rtss.exe", L"playnite.desktopapp.exe", L"playnite.fullscreenapp.exe"
    };
    for (const auto& util : kLaunchersAndUtilities) {
        if (equalsIgnoreCaseAscii(procFile, util)) return true;
    }
    return false;
}

inline bool isExplicitCompetitiveOr3DGameProcessName(std::wstring_view procFile) noexcept {
    static constexpr std::wstring_view kExplicitGames[] = {
        L"dota2.exe", L"cs2.exe", L"csgo.exe", L"hl2.exe", L"deadlock.exe", L"project8.exe",
        L"league of legends.exe", L"valorant.exe", L"valorant-win64-shipping.exe",
        L"overwatch.exe", L"wow.exe", L"wowclassic.exe", L"diablo iv.exe", L"diablo iii64.exe",
        L"hearthstone.exe", L"sc2_x64.exe", L"sc2.exe", L"war3.exe",
        L"r5apex.exe", L"tslgame.exe", L"narakabladepoint.exe",
        L"genshinimpact.exe", L"yuanshen.exe", L"starrail.exe", L"zenlesszonezero.exe", L"bh3.exe",
        L"b1-win64-shipping.exe", L"eldenring.exe", L"sekiro.exe", L"darksoulsiii.exe", L"armoredcore6.exe",
        L"cyberpunk2077.exe", L"witcher3.exe", L"bg3.exe", L"bg3_dx11.exe",
        L"rdr2.exe", L"gtav.exe", L"gta5.exe", L"playgtav.exe",
        L"monsterhunterworld.exe", L"monsterhunterrise.exe", L"monsterhunterwilds.exe",
        L"re4.exe", L"re8.exe", L"streetfighter6.exe",
        L"pathofexile_x64.exe", L"pathofexile.exe", L"pathofexilesteam.exe", L"pathofexile2.exe",
        L"destiny2.exe", L"warframe.x64.exe", L"helldivers2.exe",
        L"palworld-win64-shipping.exe", L"client-win64-shipping.exe", L"deltaforceclient-win64-shipping.exe",
        L"fc24.exe", L"fc25.exe", L"nba2k24.exe", L"nba2k25.exe",
        L"forzahorizon5.exe", L"forzahorizon4.exe", L"flightsimulator.exe",
        L"minecraft.windows.exe", L"terraria.exe", L"stardew valley.exe",
        L"factorio.exe", L"rimworldwin64.exe", L"dontstarve_steam_x64.exe",
        L"hollow_knight.exe", L"hades.exe", L"hades2.exe", L"celeste.exe",
        L"balatro.exe", L"slaythespire.exe", L"deadbydaylight-win64-shipping.exe",
        L"rainbowsix.exe", L"rainbowsix_vulkan.exe", L"bf2042.exe", L"bfv.exe", L"bf1.exe",
        L"cod.exe", L"ModernWarfare.exe", L"BlackOpsColdWar.exe"
    };
    for (const auto& game : kExplicitGames) {
        if (equalsIgnoreCaseAscii(procFile, game)) return true;
    }
    if (endsWithIgnoreCaseAscii(procFile, L"-win64-shipping.exe") ||
        endsWithIgnoreCaseAscii(procFile, L"-win32-shipping.exe")) {
        return true;
    }
    return false;
}

inline bool isKnownGameEngineWindowClass(std::wstring_view cls) noexcept {
    if (cls.empty()) return false;
    static constexpr std::wstring_view kGameClasses[] = {
        L"Valve001", L"Source2WndClass", L"SDL_app",
        L"UnityWndClass", L"UnrealWindow", L"RiotWindowClass",
        L"CryENGINE", L"GLFW30", L"LWJGL",
        L"D3DWindow", L"Direct3DWindowClass", L"D3D11_GameWindow", L"D3D12_GameWindow",
        L"grcWindow", L"CreationEngine", L"REEngine", L"AnvilNext", L"Prism3D"
    };
    for (const auto& gc : kGameClasses) {
        if (equalsIgnoreCaseAscii(cls, gc)) return true;
    }
    return false;
}

inline bool isGameLibraryExecutablePath(std::wstring_view exePath) noexcept {
    if (exePath.empty()) return false;
    static constexpr std::wstring_view kGameLibraryMarkers[] = {
        L"\\steamapps\\common\\",
        L"\\epic games\\",
        L"\\wegameapps\\",
        L"\\riot games\\",
        L"\\gog galaxy\\games\\",
        L"\\xboxgames\\",
        L"\\ea games\\",
        L"\\origin games\\",
        L"\\ubisoft game launcher\\games\\",
        L"\\battle.net\\games\\"
    };
    for (const auto& marker : kGameLibraryMarkers) {
        if (containsIgnoreCaseNormalizedPath(exePath, marker)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool WinUtils::isProductivityToolkitClassName(std::wstring_view cls) noexcept {
    if (isTabbedBrowserClassName(cls) || isTools3000UiClassName(cls)) return true;
    if (cls == L"OrpheusBrowserHost" || cls == L"CefBrowserWindow" ||
        cls == L"ApplicationFrameWindow") {
        return true;
    }
    return classNameStartsWith(cls, L"Qt");
}

bool WinUtils::shouldBypassFullscreenInteractions(bool isFullscreen, const wchar_t* cls) noexcept {
    return isFullscreen && !isProductivityToolkitClassName(cls ? cls : L"");
}

bool WinUtils::shouldBypassFullscreenInteractions(bool isFullscreen, std::wstring_view cls) noexcept {
    return isFullscreen && !isProductivityToolkitClassName(cls);
}

bool WinUtils::shouldBypassFullscreenInteractions(bool isFullscreen, const std::wstring& cls) noexcept {
    return isFullscreen && !isProductivityToolkitClassName(cls);
}

bool WinUtils::shouldBypassFullscreenInteractions(bool isFullscreen, bool isProductivityClass) noexcept {
    return isFullscreen && !isProductivityClass;
}

bool WinUtils::shouldBypassFullscreenInteractions(HWND hwnd) {
    if (!hwnd || !isWindowFullscreen(hwnd)) {
        return false;
    }
    const std::wstring classWide = getWindowClassName(hwnd);
    return shouldBypassFullscreenInteractions(true, classWide);
}

FullscreenPolicyMode WinUtils::parseFullscreenPolicyMode(std::string_view modeStr) noexcept {
    if (equalsIgnoreCaseAscii(modeStr, "strict_all") ||
        equalsIgnoreCaseAscii(modeStr, "strict") ||
        equalsIgnoreCaseAscii(modeStr, "all")) {
        return FullscreenPolicyMode::StrictAll;
    }
    if (equalsIgnoreCaseAscii(modeStr, "gesture_only") ||
        equalsIgnoreCaseAscii(modeStr, "gesture")) {
        return FullscreenPolicyMode::GestureOnly;
    }
    if (equalsIgnoreCaseAscii(modeStr, "off") ||
        equalsIgnoreCaseAscii(modeStr, "disabled") ||
        equalsIgnoreCaseAscii(modeStr, "none") ||
        equalsIgnoreCaseAscii(modeStr, "false")) {
        return FullscreenPolicyMode::Off;
    }
    return FullscreenPolicyMode::Smart;
}

const char* WinUtils::fullscreenPolicyModeToString(FullscreenPolicyMode mode) noexcept {
    switch (mode) {
        case FullscreenPolicyMode::StrictAll:   return "strict_all";
        case FullscreenPolicyMode::GestureOnly: return "gesture_only";
        case FullscreenPolicyMode::Off:         return "off";
        case FullscreenPolicyMode::Smart:
        default:                                return "smart";
    }
}

void WinUtils::setFullscreenPolicyMode(FullscreenPolicyMode mode) noexcept {
    g_fullscreenPolicyMode.store(static_cast<int>(mode), std::memory_order_release);
}

FullscreenPolicyMode WinUtils::getFullscreenPolicyMode() noexcept {
    return static_cast<FullscreenPolicyMode>(g_fullscreenPolicyMode.load(std::memory_order_acquire));
}

bool WinUtils::isMediaPlayerProcessOrClass(std::wstring_view processName,
                                           std::wstring_view className) noexcept {
    const std::wstring_view procFile = extractFileNameView(processName);
    if (!procFile.empty()) {
        static constexpr std::wstring_view kMediaProcesses[] = {
            L"potplayermini64.exe", L"potplayermini.exe", L"potplayer64.exe", L"potplayer.exe",
            L"vlc.exe", L"mpv.exe", L"mpvnet.exe", L"ffplay.exe",
            L"mpc-hc.exe", L"mpc-hc64.exe", L"mpc-be.exe", L"mpc-be64.exe",
            L"kmplayer.exe", L"kmplayer64.exe", L"smplayer.exe", L"gom.exe", L"gom64.exe",
            L"wmplayer.exe", L"microsoft.media.player.exe", L"video.ui.exe",
            L"bilibili.exe", L"qiyiclient.exe", L"qqlive.exe", L"youkudesktop.exe",
            L"douyin.exe", L"baofeng.exe", L"xmp.exe", L"qqplayer.exe",
            L"kodi.exe", L"plex.exe", L"plexhtpc.exe", L"jellyfinmediaplayer.exe", L"embytheater.exe",
            L"cloudmusic.exe", L"qqmusic.exe", L"kugou.exe", L"kuwomusic.exe",
            L"spotify.exe", L"foobar2000.exe", L"aimp.exe", L"musicbee.exe",
            L"powerpnt.exe", L"wpp.exe"
        };
        for (const auto& mp : kMediaProcesses) {
            if (equalsIgnoreCaseAscii(procFile, mp)) return true;
        }
    }

    if (!className.empty()) {
        static constexpr std::wstring_view kMediaClasses[] = {
            L"PotPlayer", L"PotPlayer64", L"MediaPlayerClassicW",
            L"mpv", L"VLC DirectX", L"WMPlayerApp", L"screenClass"
        };
        for (const auto& mc : kMediaClasses) {
            if (equalsIgnoreCaseAscii(className, mc)) return true;
        }
    }
    return false;
}

bool WinUtils::isKnownGameProcessOrClass(std::wstring_view processName,
                                         std::wstring_view className,
                                         std::wstring_view exePath) noexcept {
    if (isTools3000UiClassName(className) ||
        className == L"Progman" || className == L"WorkerW" ||
        className == L"Shell_TrayWnd" || className == L"Shell_SecondaryTrayWnd" ||
        className == L"CabinetWClass" || className == L"ExploreWClass") {
        return false;
    }

    if (isMediaPlayerProcessOrClass(processName, className) ||
        isRemoteDesktopOrVmProcessOrClass(processName, className)) {
        return false;
    }

    const std::wstring_view procFile = extractFileNameView(
        !processName.empty() ? processName : exePath);

    if (!procFile.empty()) {
        if (isExplicitCompetitiveOr3DGameProcessName(procFile)) {
            return true;
        }
        if (isNonGameLauncherOrUtilityProcessName(procFile)) {
            return false;
        }
    }

    if (isKnownGameEngineWindowClass(className)) {
        return true;
    }

    if (isGameLibraryExecutablePath(exePath)) {
        return true;
    }

    return false;
}

FullscreenScene WinUtils::classifyFullscreenScene(bool isFullscreen,
                                                  std::wstring_view processName,
                                                  std::wstring_view className,
                                                  std::wstring_view exePath) noexcept {
    if (!isFullscreen) {
        return FullscreenScene::None;
    }

    // 1. Tools3000 自身界面、桌面背景、任务栏与资源管理器全屏：归为生产力场景，绝不屏蔽热键与截图
    if (isTools3000UiClassName(className) ||
        className == L"Progman" || className == L"WorkerW" ||
        className == L"Shell_TrayWnd" || className == L"Shell_SecondaryTrayWnd" ||
        className == L"CabinetWClass" || className == L"ExploreWClass") {
        return FullscreenScene::Productivity;
    }

    const std::wstring_view procFile = extractFileNameView(
        !processName.empty() ? processName : exePath);

    // 2. 专用影音播放器、幻灯片放映与远程桌面/虚拟机全屏会话
    //    (PotPlayer / VLC / MPV / Bilibili / PowerPoint / MSTSC / ToDesk / 向日葵 / VMware 等)
    if (isMediaPlayerProcessOrClass(procFile, className) ||
        isRemoteDesktopOrVmProcessOrClass(procFile, className)) {
        return FullscreenScene::MediaOrBrowser;
    }

    // 3. 已知游戏进程、游戏引擎窗口类名 (SDL_app / Valve001 / UnityWndClass / UnrealWindow) 或 Steam/Epic 游戏库目录
    if (isKnownGameProcessOrClass(procFile, className, exePath)) {
        return FullscreenScene::Game;
    }

    // 4. 生产力 IDE / 办公协作客户端 (VS Code / Cursor / Visual Studio / JetBrains / 微信 / 飞书 / scrcpy 等)
    if (!procFile.empty() && isElectronOrIdeProductivityProcessName(procFile)) {
        return FullscreenScene::Productivity;
    }

    // 5. 网页浏览器全屏 (Chrome / Edge / Firefox / Brave 等观看网页全屏电影或 F11 全屏浏览)
    if ((!procFile.empty() && isWebBrowserProcessName(procFile)) ||
        (procFile.empty() && isWebBrowserClassName(className))) {
        return FullscreenScene::MediaOrBrowser;
    }

    // 6. 其它生产力工具类框架窗口 (Qt / CEF / UWP ApplicationFrameWindow 等)
    if (isProductivityToolkitClassName(className)) {
        return FullscreenScene::Productivity;
    }

    // 7. 其余未知的全屏独占非生产力窗口（如自研引擎全屏 3D 游戏）默认按游戏场景保护
    return FullscreenScene::Game;
}

FullscreenScene WinUtils::classifyWindowScene(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return FullscreenScene::None;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;

    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow() ||
        !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return FullscreenScene::None;
    }

    const std::wstring className = getWindowClassName(hwnd);
    if (className == L"Progman" || className == L"WorkerW" ||
        className == L"Shell_TrayWnd" || className == L"Shell_SecondaryTrayWnd" ||
        isTools3000UiClassName(className)) {
        return FullscreenScene::None;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const std::wstring procName = processNameFromPid(pid);
    const std::wstring procPath = processPathFromPid(pid);

    if (isWindowFullscreen(hwnd)) {
        return classifyFullscreenScene(true, procName, className, procPath);
    }

    // 针对明确已知的大型电竞/3D 游戏进程或安装于游戏库目录下的游戏，
    // 即便玩家采用带边框窗口化运行，只要该游戏窗口处于前台活动态且客户区尺寸 >= 640x480，
    // 同样识别为 Game 场景，自动让出 F1 等游戏高频按键并停用鼠标手势；
    // 注意：非全屏状态下不单凭通用引擎类名（如 SDL_app / GLFW30）判定，避免误伤 scrcpy 或桌面开发工具。
    const std::wstring_view procFile = extractFileNameView(
        !procName.empty() ? std::wstring_view(procName) : std::wstring_view(procPath));
    if (!isMediaPlayerProcessOrClass(procFile, className) &&
        !isRemoteDesktopOrVmProcessOrClass(procFile, className) &&
        !isNonGameLauncherOrUtilityProcessName(procFile) &&
        (isExplicitCompetitiveOr3DGameProcessName(procFile) ||
         isGameLibraryExecutablePath(procPath))) {
        RECT rcClient{};
        if (GetClientRect(hwnd, &rcClient)) {
            const int w = rcClient.right - rcClient.left;
            const int h = rcClient.bottom - rcClient.top;
            if (w >= 640 && h >= 480) {
                return FullscreenScene::Game;
            }
        }
    }

    return FullscreenScene::None;
}

bool WinUtils::shouldSuspendHotkeysForScene(FullscreenScene scene, FullscreenPolicyMode mode) noexcept {
    switch (mode) {
        case FullscreenPolicyMode::Off:
        case FullscreenPolicyMode::GestureOnly:
            return false;
        case FullscreenPolicyMode::StrictAll:
            return scene == FullscreenScene::Game || scene == FullscreenScene::MediaOrBrowser;
        case FullscreenPolicyMode::Smart:
        default:
            // 智能模式：游戏内卸下 F1 等全局快捷键；全屏看电影/视频时保留截图、录屏、OCR 等快捷键可用
            return scene == FullscreenScene::Game;
    }
}

bool WinUtils::shouldSuspendGesturesForScene(FullscreenScene scene,
                                             FullscreenPolicyMode mode,
                                             std::wstring_view className) noexcept {
    if (mode == FullscreenPolicyMode::Off) {
        return false;
    }
    if (scene == FullscreenScene::Game) {
        return true;
    }
    if (scene == FullscreenScene::MediaOrBrowser) {
        if (mode == FullscreenPolicyMode::StrictAll) {
            return true;
        }
        // Smart / GestureOnly 模式下：独立影音播放器全屏停用手势（保留原生右键菜单），浏览器全屏保留网页手势
        return !isTabbedBrowserClassName(className);
    }
    return false;
}

bool WinUtils::shouldBypassCaptureForScene(FullscreenScene scene, FullscreenPolicyMode mode) noexcept {
    switch (mode) {
        case FullscreenPolicyMode::Off:
        case FullscreenPolicyMode::GestureOnly:
            return false;
        case FullscreenPolicyMode::StrictAll:
            return scene == FullscreenScene::Game || scene == FullscreenScene::MediaOrBrowser;
        case FullscreenPolicyMode::Smart:
        default:
            // 智能模式：游戏全屏禁用截图防误触（F1 归还游戏），全屏看电影/视频时允许正常截图与录屏
            return scene == FullscreenScene::Game;
    }
}

bool WinUtils::shouldBypassOverlaysForScene(FullscreenScene scene, FullscreenPolicyMode mode) noexcept {
    if (mode == FullscreenPolicyMode::Off) {
        return false;
    }
    // 无论是打游戏还是全屏看电影，均自动静默屏幕四角触发角、按键回显与鼠标聚光灯水波纹
    return scene == FullscreenScene::Game || scene == FullscreenScene::MediaOrBrowser;
}

bool WinUtils::shouldSuspendHotkeysForWindow(HWND hwnd, FullscreenPolicyMode mode) {
    if (mode == FullscreenPolicyMode::Off) return false;
    return shouldSuspendHotkeysForScene(classifyWindowScene(hwnd), mode);
}

bool WinUtils::shouldSuspendGesturesForWindow(HWND hwnd, FullscreenPolicyMode mode) {
    if (mode == FullscreenPolicyMode::Off || !hwnd || !IsWindow(hwnd)) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;
    const FullscreenScene scene = classifyWindowScene(hwnd);
    const std::wstring className = getWindowClassName(hwnd);
    return shouldSuspendGesturesForScene(scene, mode, className);
}

bool WinUtils::shouldBypassCaptureForWindow(HWND hwnd, FullscreenPolicyMode mode) {
    if (mode == FullscreenPolicyMode::Off) return false;
    return shouldBypassCaptureForScene(classifyWindowScene(hwnd), mode);
}

bool WinUtils::shouldBypassOverlaysForWindow(HWND hwnd, FullscreenPolicyMode mode) {
    if (mode == FullscreenPolicyMode::Off) return false;
    return shouldBypassOverlaysForScene(classifyWindowScene(hwnd), mode);
}

bool WinUtils::queryProcessElevated(HANDLE process) {
    if (!process) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD ret = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &ret) != FALSE;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

bool WinUtils::isCurrentProcessElevated() {
    return queryProcessElevated(GetCurrentProcess());
}

WinUtils::WindowProcessQuery WinUtils::queryWindowProcessAccess(HWND hwnd) {
    WindowProcessQuery q;
    if (!hwnd || !IsWindow(hwnd)) return q;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return q;

    HANDLE limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    q.queryLimitedOk = limited != nullptr;

    HANDLE full = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    q.queryInformationOk = full != nullptr;

    HANDLE tokenSource = full ? full : limited;
    if (tokenSource) {
        HANDLE token = nullptr;
        if (OpenProcessToken(tokenSource, TOKEN_QUERY, &token)) {
            q.tokenQueryOk = true;
            TOKEN_ELEVATION elev{};
            DWORD ret = 0;
            if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &ret)) {
                q.tokenElevated = elev.TokenIsElevated != 0;
            }
            CloseHandle(token);
        }
    }
    if (full) CloseHandle(full);
    if (limited) CloseHandle(limited);
    return q;
}

bool WinUtils::isWindowProcessElevated(HWND hwnd) {
    return queryWindowProcessAccess(hwnd).tokenElevated;
}

bool WinUtils::isWindowHigherIntegrity(HWND hwnd) {
    const auto q = queryWindowProcessAccess(hwnd);
    if (!q.queryLimitedOk) return false;
    if (!q.queryInformationOk || !q.tokenQueryOk) {
        return !isCurrentProcessElevated();
    }
    return q.tokenElevated && !isCurrentProcessElevated();
}

std::optional<std::wstring> WinUtils::getSelectedExplorerFile() {
    HWND foregroundWnd = GetForegroundWindow();
    if (!foregroundWnd) return std::nullopt;

    wchar_t className[256] = {0};
    GetClassNameW(foregroundWnd, className, 256);
    std::wstring cls = className;

    bool isExplorer = (cls == L"CabinetWClass" || cls == L"ExploreWClass");
    bool isDesktop = (cls == L"Progman" || cls == L"WorkerW");

    if (!isExplorer && !isDesktop) {
        return std::nullopt;
    }

    // 检查焦点控件是否处于重命名编辑状态 (Edit 控件)，防止打字误触发
    DWORD threadId = GetWindowThreadProcessId(foregroundWnd, nullptr);
    GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
    if (GetGUIThreadInfo(threadId, &gti)) {
        if (gti.hwndFocus) {
            wchar_t focusClass[256] = {0};
            GetClassNameW(gti.hwndFocus, focusClass, 256);
            if (_wcsicmp(focusClass, L"Edit") == 0) {
                return std::nullopt;
            }
        }
    }

    // COM 遍历 IShellWindows 取得选中项
    Microsoft::WRL::ComPtr<IShellWindows> shellWindows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr) || !shellWindows) return std::nullopt;

    long count = 0;
    shellWindows->get_Count(&count);

    for (long i = 0; i < count; ++i) {
        VARIANT vi;
        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;
        Microsoft::WRL::ComPtr<IDispatch> disp;
        HRESULT itemHr = shellWindows->Item(vi, &disp);
        VariantClear(&vi);
        if (FAILED(itemHr) || !disp) continue;

        Microsoft::WRL::ComPtr<IWebBrowserApp> app;
        if (FAILED(disp.As(&app)) || !app) continue;

        HWND hwnd = nullptr;
        app->get_HWND(reinterpret_cast<SHANDLE_PTR*>(&hwnd));
        if (!hwnd) continue;

        // 匹配顶层窗口或其父/子窗口句柄（兼容 Win11 多标签页 Explorer）
        if (hwnd != foregroundWnd && !isDesktop) {
            if (!IsChild(hwnd, foregroundWnd) && !IsChild(foregroundWnd, hwnd)) {
                continue;
            }
        }

        Microsoft::WRL::ComPtr<IServiceProvider> sp;
        if (FAILED(disp.As(&sp)) || !sp) continue;

        Microsoft::WRL::ComPtr<IShellBrowser> sb;
        if (FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) || !sb) continue;

        Microsoft::WRL::ComPtr<IShellView> sv;
        if (FAILED(sb->QueryActiveShellView(&sv)) || !sv) continue;

        Microsoft::WRL::ComPtr<IFolderView> fv;
        if (FAILED(sv.As(&fv)) || !fv) continue;

        int focusIndex = -1;
        if (FAILED(fv->GetFocusedItem(&focusIndex)) || focusIndex < 0) {
            if (FAILED(fv->GetSelectionMarkedItem(&focusIndex)) || focusIndex < 0) {
                continue;
            }
        }

        Microsoft::WRL::ComPtr<IPersistFolder2> pf;
        if (FAILED(fv->GetFolder(IID_PPV_ARGS(&pf))) || !pf) continue;

        PIDLIST_ABSOLUTE pidlFolder = nullptr;
        if (FAILED(pf->GetCurFolder(&pidlFolder)) || !pidlFolder) continue;

        PITEMID_CHILD pidlItem = nullptr;
        if (SUCCEEDED(fv->Item(focusIndex, &pidlItem)) && pidlItem) {
            PIDLIST_ABSOLUTE pidlFull = ILCombine(pidlFolder, pidlItem);
            CoTaskMemFree(pidlItem);
            CoTaskMemFree(pidlFolder);

            if (pidlFull) {
                wchar_t path[MAX_PATH] = {0};
                const bool hasPath = SHGetPathFromIDListW(pidlFull, path) != FALSE;
                CoTaskMemFree(pidlFull);
                if (hasPath) {
                    std::error_code ec;
                    if (std::filesystem::exists(path, ec)) {
                        return std::wstring(path);
                    }
                }
            }
        } else {
            CoTaskMemFree(pidlFolder);
        }
    }

    return std::nullopt;
}

std::string WinUtils::captureSelectedText() {
    // 先尝试获取当前剪贴板
    std::string initialText;
    if (OpenClipboard(nullptr)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            auto* pszText = static_cast<wchar_t*>(GlobalLock(hData));
            if (pszText) {
                initialText = wstringToUtf8(pszText);
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
    }

    // 模拟 Ctrl+C 复制选中文本
    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD; inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD; inputs[1].ki.wVk = 'C';
    inputs[2].type = INPUT_KEYBOARD; inputs[2].ki.wVk = 'C'; inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD; inputs[3].ki.wVk = VK_CONTROL; inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));

    Sleep(50); // 给系统一点时间完成复制

    // 再次读取剪贴板
    if (OpenClipboard(nullptr)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            auto* pszText = static_cast<wchar_t*>(GlobalLock(hData));
            if (pszText) {
                std::string newText = wstringToUtf8(pszText);
                GlobalUnlock(hData);
                CloseClipboard();
                return newText;
            }
        }
        CloseClipboard();
    }

    return initialText;
}

std::vector<WinUtils::SystemDriveInfo> WinUtils::getSystemDrives() {
    std::vector<SystemDriveInfo> drives;
    DWORD driveMask = GetLogicalDrives();
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        if (!(driveMask & (1u << (letter - 'A')))) continue;

        SystemDriveInfo info;
        info.letter = letter;
        info.path = std::wstring{static_cast<wchar_t>(letter), L':', L'\\'};

        UINT driveType = GetDriveTypeW(info.path.c_str());
        switch (driveType) {
            case DRIVE_FIXED:     info.typeStr = L"fixed"; break;
            case DRIVE_REMOTE:    info.typeStr = L"remote"; break;
            case DRIVE_REMOVABLE: info.typeStr = L"removable"; break;
            case DRIVE_CDROM:     info.typeStr = L"cdrom"; break;
            case DRIVE_RAMDISK:   info.typeStr = L"ramdisk"; break;
            default:              info.typeStr = L"unknown"; break;
        }

        wchar_t volumeName[MAX_PATH + 1] = {};
        wchar_t fileSystemName[MAX_PATH + 1] = {};
        DWORD serialNumber = 0, maxComponentLen = 0, flags = 0;
        if (GetVolumeInformationW(info.path.c_str(), volumeName, MAX_PATH + 1,
                                 &serialNumber, &maxComponentLen, &flags,
                                 fileSystemName, MAX_PATH + 1)) {
            info.volumeLabel = volumeName;
            info.fileSystem = fileSystemName;
        }

        ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;
        if (GetDiskFreeSpaceExW(info.path.c_str(), &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
            info.totalBytes = totalNumberOfBytes.QuadPart;
            info.freeBytes = totalNumberOfFreeBytes.QuadPart;
        }

        drives.push_back(std::move(info));
    }
    return drives;
}

bool WinUtils::openFolderAndSelectItem(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    const DWORD attributes = GetFileAttributesW(filePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        LOG_WARN("WinUtils: openFolder target no longer exists: {}", wstringToUtf8(filePath));
        return false;
    }

    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result > 32) return true;
        LOG_WARN("WinUtils: failed to open directory, shellCode={}, path={}",
                 result, wstringToUtf8(filePath));
        return false;
    }

    // 采用独立 STA 线程执行 COM Shell API，彻底避免调用方线程处于 MTA 导致的 RPC_E_WRONG_THREAD 失败
    bool shellSuccess = false;
    HRESULT parseResult = E_FAIL;
    HRESULT selectResult = E_FAIL;
    std::thread staThread([&]() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        PIDLIST_ABSOLUTE itemId = nullptr;
        parseResult = SHParseDisplayName(filePath.c_str(), nullptr, &itemId, 0, nullptr);
        if (SUCCEEDED(parseResult) && itemId) {
            selectResult = SHOpenFolderAndSelectItems(itemId, 0, nullptr, 0);
            CoTaskMemFree(itemId);
            if (SUCCEEDED(selectResult)) {
                shellSuccess = true;
            }
        }
        if (SUCCEEDED(comResult)) CoUninitialize();
    });
    if (staThread.joinable()) staThread.join();
    if (shellSuccess) return true;

    // 二级降级容灾：通过 explorer.exe /select 启动并定位目标项
    const std::wstring arguments = L"/select,\"" + filePath + L"\"";
    const auto explorerResult = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL));
    if (explorerResult > 32) return true;

    // 三级降级容灾：打开父文件夹
    const std::filesystem::path parent = std::filesystem::path(filePath).parent_path();
    if (!parent.empty()) {
        const auto parentResult = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", parent.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (parentResult > 32) return true;
    }
    LOG_WARN("WinUtils: failed to locate item, parseHr=0x{:08X}, selectHr=0x{:08X}, shellCode={}, path={}",
             static_cast<unsigned>(parseResult), static_cast<unsigned>(selectResult), explorerResult,
             wstringToUtf8(filePath));
    return false;
}

bool WinUtils::openFile(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    const auto first = filePath.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const auto last = filePath.find_last_not_of(L" \t\r\n");
    const std::wstring trimmed = filePath.substr(first, last - first + 1);

    const auto startsWithCaseInsensitive = [](std::wstring_view s, std::wstring_view prefix) {
        if (s.size() < prefix.size()) return false;
        return _wcsnicmp(s.data(), prefix.data(), prefix.size()) == 0;
    };
    if (startsWithCaseInsensitive(trimmed, L"http://") ||
        startsWithCaseInsensitive(trimmed, L"https://")) {
        return openUrl(trimmed);
    }
    if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_WARN("WinUtils: openFile target no longer exists: {}", wstringToUtf8(filePath));
        return false;
    }
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result > 32) return true;
    LOG_WARN("WinUtils: openFile failed, shellCode={}, path={}", result, wstringToUtf8(filePath));
    return false;
}

bool WinUtils::openUrl(const std::wstring& url) {
    if (url.empty()) return false;
    const auto first = url.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const auto last = url.find_last_not_of(L" \t\r\n");
    const std::wstring trimmed = url.substr(first, last - first + 1);

    const auto startsWithCaseInsensitive = [](std::wstring_view s, std::wstring_view prefix) {
        if (s.size() < prefix.size()) return false;
        return _wcsnicmp(s.data(), prefix.data(), prefix.size()) == 0;
    };
    if (!startsWithCaseInsensitive(trimmed, L"http://") &&
        !startsWithCaseInsensitive(trimmed, L"https://")) {
        LOG_WARN("WinUtils: openUrl rejected non-http(s) scheme: {}", wstringToUtf8(trimmed));
        return false;
    }
    if (trimmed == L"http://" || trimmed == L"https://") {
        LOG_WARN("WinUtils: openUrl rejected incomplete scheme: {}", wstringToUtf8(trimmed));
        return false;
    }
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", trimmed.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result > 32) return true;
    LOG_WARN("WinUtils: openUrl failed, shellCode={}, url={}", result, wstringToUtf8(trimmed));
    return false;
}

bool WinUtils::openWithNotepad(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    const std::wstring args = L"\"" + filePath + L"\"";
    return reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

bool WinUtils::openFileAsAdmin(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    return reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"runas", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

bool WinUtils::showFileProperties(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const HRESULT oleResult = OleInitialize(nullptr);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"properties";
    info.lpFile = filePath.c_str();
    info.nShow = SW_SHOWNORMAL;
    const bool success = ShellExecuteExW(&info) != FALSE;
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (oleResult == S_OK || oleResult == S_FALSE) OleUninitialize();
    if (!success) {
        LOG_WARN("WinUtils: show properties failed, error={}, path={}", error, wstringToUtf8(filePath));
    }
    return success;
}

bool WinUtils::isSystemTaskbarDark() {
    DWORD data = 0;
    DWORD dataSize = sizeof(data);
    // 读取 Windows 10 (1903+) / Windows 11 系统任务栏主题设置 (0: Dark, 1: Light)
    LONG res = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &data,
        &dataSize
    );
    if (res == ERROR_SUCCESS) {
        return (data == 0); // 0 为深色任务栏，1 为浅色任务栏
    }

    // 若不存在，尝试读取应用级主题 AppsUseLightTheme 回退
    res = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &data,
        &dataSize
    );
    if (res == ERROR_SUCCESS) {
        return (data == 0);
    }

    // 默认回退深色
    return true;
}

bool WinUtils::isSystemDarkMode() {
    DWORD data = 0;
    DWORD dataSize = sizeof(data);
    LONG res = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &data,
        &dataSize
    );
    if (res == ERROR_SUCCESS) {
        return (data == 0);
    }
    return isSystemTaskbarDark();
}

HANDLE WinUtils::getProcessJobObject() {
    static HANDLE s_job = []() -> HANDLE {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job) return nullptr;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            CloseHandle(job);
            return nullptr;
        }
        // 将当前主进程绑定到 Job Object
        AssignProcessToJobObject(job, GetCurrentProcess());
        return job;
    }();
    return s_job;
}

bool WinUtils::assignProcessToCurrentJob(HANDLE hProcess) {
    if (!hProcess) return false;
    HANDLE job = getProcessJobObject();
    if (!job) return false;
    return AssignProcessToJobObject(job, hProcess) != FALSE;
}

bool WinUtils::flushFileToPhysicalDisk(HANDLE hFile) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE) return false;
    return FlushFileBuffers(hFile) != FALSE;
}

bool WinUtils::atomicWriteFileWithFlush(const std::wstring& targetPath, const std::string& data) {
    return tools3000::common::atomicWriteFileWithFlush(targetPath, data);
}

HANDLE WinUtils::createLowMemoryNotification() {
    return CreateMemoryResourceNotification(LowMemoryResourceNotification);
}

HWND WinUtils::createOverlayHelperOwner(HINSTANCE hInstance, const wchar_t* name) {
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC",
        name ? name : L"Tools3000_OverlayHelperOwner",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr
    );
    if (hwnd) {
        SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    }
    return hwnd;
}

void WinUtils::applyTaskbarSafeOverlayStyle(HWND hwnd, bool excludeFromCapture) {
    if (!hwnd || !IsWindow(hwnd)) return;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    constexpr LONG_PTR required = WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                  WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                                  WS_EX_TOOLWINDOW;
    exStyle = (exStyle | required) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    // 显式标记 NonRudeHWND 属性，阻止 explorer.exe (CRudeAppManager) 将全屏覆盖层误判为全屏独占应用并压低任务栏 Z 序
    SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));

    const DWM_WINDOW_CORNER_PREFERENCE noCorners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &noCorners, sizeof(noCorners));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                          &disableTransitions, sizeof(disableTransitions));
    (void)excludeFromCapture;
    allowWindowCapture(hwnd);
}

void WinUtils::applyUniversalRoundedCorners(HWND hwnd, int width, int height, int radius) {
    if (!hwnd || !IsWindow(hwnd) || width <= 0 || height <= 0 || radius <= 0) return;

    // 1. Windows 11 DWM 硬件级超平滑圆角首选
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // 2. Win32 硬件级 RGN 裁剪兜底 (确保在 Win10、Windows Server 2022/2025、虚拟机与 RDP 下 100% 绝对圆角)
    const HRGN hRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
    if (hRgn) {
        SetWindowRgn(hwnd, hRgn, TRUE);
        // SetWindowRgn 成功后系统接管 hRgn 句柄的所有权，无需手动 DeleteObject
    }
}

bool WinUtils::setWindowAppUserModelId(HWND hwnd, const std::wstring& aumid) {
    if (!hwnd || !IsWindow(hwnd) || aumid.empty()) return false;
    IPropertyStore* pps = nullptr;
    HRESULT hr = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&pps));
    if (SUCCEEDED(hr) && pps) {
        PROPVARIANT pv;
        InitPropVariantFromString(aumid.c_str(), &pv);
        hr = pps->SetValue(PKEY_AppUserModel_ID, pv);
        if (SUCCEEDED(hr)) {
            pps->Commit();
        }
        PropVariantClear(&pv);
        pps->Release();
        return SUCCEEDED(hr);
    }
    return false;
}

bool WinUtils::setShortcutAppUserModelId(const std::wstring& shortcutPath, const std::wstring& aumid) {
    if (shortcutPath.empty() || aumid.empty()) return false;
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
    if (FAILED(hr) || !psl) return false;

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_PPV_ARGS(&ppf));
    if (SUCCEEDED(hr) && ppf) {
        hr = ppf->Load(shortcutPath.c_str(), STGM_READWRITE);
        if (SUCCEEDED(hr)) {
            IPropertyStore* pps = nullptr;
            hr = psl->QueryInterface(IID_PPV_ARGS(&pps));
            if (SUCCEEDED(hr) && pps) {
                PROPVARIANT pv;
                InitPropVariantFromString(aumid.c_str(), &pv);
                hr = pps->SetValue(PKEY_AppUserModel_ID, pv);
                if (SUCCEEDED(hr)) {
                    pps->Commit();
                }
                PropVariantClear(&pv);
                pps->Release();
                if (SUCCEEDED(hr)) {
                    hr = ppf->Save(shortcutPath.c_str(), TRUE);
                }
            }
        }
        ppf->Release();
    }
    psl->Release();
    return SUCCEEDED(hr);
}

void WinUtils::syncApplicationShortcuts() {
    const std::wstring aumid = L"Yy1.Tools3000";

    // 1. 获取用户及公共目录下的开始菜单与桌面路径
    const int csidls[] = {
        CSIDL_PROGRAMS,
        CSIDL_COMMON_PROGRAMS,
        CSIDL_DESKTOPDIRECTORY,
        CSIDL_COMMON_DESKTOPDIRECTORY
    };

    for (int csidl : csidls) {
        wchar_t folderPath[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, folderPath))) {
            const std::filesystem::path baseDir(folderPath);

            // 针对当前 Tools3000 快捷方式注入 AUMID
            std::filesystem::path toolsLnk = (csidl == CSIDL_PROGRAMS || csidl == CSIDL_COMMON_PROGRAMS)
                ? (baseDir / L"Tools3000" / L"Tools3000.lnk")
                : (baseDir / L"Tools3000.lnk");

            std::error_code ec;
            if (std::filesystem::exists(toolsLnk, ec)) {
                setShortcutAppUserModelId(toolsLnk.wstring(), aumid);
            }
        }
    }

    // 广播 SHChangeNotify 通知 Windows Shell 刷新图标与属性存储
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void WinUtils::emergencyFlushInputState() noexcept {
    // 1. 如果当前进程任意窗口持有鼠标捕获，立即强制释放
    if (GetCapture() != nullptr) {
        ReleaseCapture();
    }
    // 2. 解除可能存在的光标区域限制
    ClipCursor(nullptr);

    // 3. 释放可能被卡住的修饰键 (Win, Ctrl, Shift, Alt)
    std::vector<INPUT> inputs;
    inputs.reserve(10);

    auto addKeyUp = [&](WORD vk, bool extended = false) {
        INPUT inp{};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = vk;
        inp.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        inp.ki.dwFlags = KEYEVENTF_KEYUP | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs.push_back(inp);
    };

    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) addKeyUp(VK_LWIN, true);
    if ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) addKeyUp(VK_RWIN, true);
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) addKeyUp(VK_CONTROL, false);
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) addKeyUp(VK_SHIFT, false);

    // 针对 Alt 键 (VK_MENU)：若处于逻辑按下态，绝不能单独发裸 Alt KeyUp，
    // 否则 Windows 会立即判定为单按 Alt 并触发前台窗口 (尤其是 Explorer) 激活系统菜单栏 SC_KEYMENU，
    // 从而锁死文件拖拽和 OLE DragDrop。
    // 正确方案：先行注入中立无害按键脉冲 (VK_F24 Down + Up)，彻底消除 SC_KEYMENU 判定，再安全释放 Alt。
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        INPUT neutralDown{};
        neutralDown.type = INPUT_KEYBOARD;
        neutralDown.ki.wVk = VK_F24;
        inputs.push_back(neutralDown);

        INPUT neutralUp{};
        neutralUp.type = INPUT_KEYBOARD;
        neutralUp.ki.wVk = VK_F24;
        neutralUp.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(neutralUp);

        addKeyUp(VK_MENU, false);
    }

    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

}  // namespace tools3000::core

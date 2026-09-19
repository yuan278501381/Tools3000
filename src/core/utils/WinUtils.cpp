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
    if (equalsIgnoreCase(exeName, L"Tools3000_Service.exe") ||
        equalsIgnoreCase(exeName, L"Tools3000_Service.exe")) {
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
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return wstringToUtf8(processNameFromPid(pid));
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

    // 排除桌面和 Shell 窗口
    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow()) return false;
    std::wstring className = getWindowClassName(hwnd);
    if (className == L"Progman" || className == L"WorkerW" || className == L"Shell_TrayWnd") {
        return false;
    }

    // 若具有标准标题栏（WS_CAPTION），说明是普通窗口或常规最大化窗口，非全屏独占模式
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) {
        return false;
    }

    // 获取窗口所在的显示器
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!hMon) return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(hMon, &mi)) return false;

    RECT rcWindow{};
    if (!GetWindowRect(hwnd, &rcWindow)) return false;

    // 判定无边框窗口是否覆盖整个物理显示器区域
    return (rcWindow.left <= mi.rcMonitor.left &&
            rcWindow.top <= mi.rcMonitor.top &&
            rcWindow.right >= mi.rcMonitor.right &&
            rcWindow.bottom >= mi.rcMonitor.bottom);
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
    return CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC",
        name ? name : L"Tools3000_OverlayHelperOwner",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr
    );
}

void WinUtils::applyTaskbarSafeOverlayStyle(HWND hwnd, bool excludeFromCapture) {
    if (!hwnd || !IsWindow(hwnd)) return;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    constexpr LONG_PTR required = WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                  WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                                  WS_EX_TOOLWINDOW;
    exStyle = (exStyle | required) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    const DWM_WINDOW_CORNER_PREFERENCE noCorners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &noCorners, sizeof(noCorners));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                          &disableTransitions, sizeof(disableTransitions));
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

            // 清理历史遗留的旧版 Tools3000 快捷方式
            std::filesystem::path legacyLnk = (csidl == CSIDL_PROGRAMS || csidl == CSIDL_COMMON_PROGRAMS)
                ? (baseDir / L"Tools3000" / L"Tools3000.lnk")
                : (baseDir / L"Tools3000.lnk");

            if (std::filesystem::exists(legacyLnk, ec)) {
                std::filesystem::remove(legacyLnk, ec);
            }

            // 如果旧版开始菜单目录为空，则一并移除
            if (csidl == CSIDL_PROGRAMS || csidl == CSIDL_COMMON_PROGRAMS) {
                std::filesystem::path legacyDir = baseDir / L"Tools3000";
                if (std::filesystem::exists(legacyDir, ec) && std::filesystem::is_empty(legacyDir, ec)) {
                    std::filesystem::remove(legacyDir, ec);
                }
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

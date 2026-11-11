#include "ui/QuickLookWindow.h"
#include "ui/SearchWindow.h"
#include "ui/KeyboardPipeline.h"
#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"
#include "core/utils/TextFilePreviewDecoder.h"
#include "ui/WebViewEnvironmentManager.h"
#include "ui/WebViewDpi.h"
#include "ui/WebViewWindowStyle.h"
#include "ui/WebViewSecurity.h"
#include "ui/QuickLookNavigation.h"
#include <WebView2.h>
#include <wrl/event.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include "../resource.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <vector>

using namespace Microsoft::WRL;
using json = nlohmann::json;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMSBT_MAINWINDOW 2
#define DWMSBT_TRANSIENTWINDOW 3
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWCP_ROUND 2
#endif

namespace tools3000::ui {

static constexpr const wchar_t* QUICKLOOK_WINDOW_CLASS = L"Tools3000_QuickLookWindow";
static constexpr const wchar_t* QUICKLOOK_MEDIA_HOST = L"quicklook-media.tools3000.local";
static constexpr UINT WM_QUICKLOOK_VERIFY_DEACTIVATED = WM_APP + 19;
static constexpr size_t kMaxTextPreviewBytes = 256 * 1024; // 256 KB 极速预览上限，防止超大日志阻塞 WebView2 DOM

namespace {

bool isQuickLookEnglishLocale() {
    const std::string lang = tools3000::core::ConfigManager::instance().get<std::string>("/general/language", "auto");
    if (lang == "en" || lang == "en-US") return true;
    if (lang == "auto") {
        const LANGID langId = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(langId) == LANG_ENGLISH) return true;
    }
    return false;
}

bool isQuickLookDarkTheme() {
    const std::string theme = tools3000::core::ConfigManager::instance().get<std::string>("/general/theme", "dark");
    if (theme == "light") return false;
    if (theme == "dark") return true;
    return tools3000::core::WinUtils::isSystemDarkMode();
}

void syncQuickLookWindowTheme(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    const BOOL useDarkMode = isQuickLookDarkTheme() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
}

std::string formatFileSize(uintmax_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double kb = bytes / 1024.0;
    if (kb < 1024.0) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << kb << " KB";
        return ss.str();
    }
    double mb = kb / 1024.0;
    if (mb < 1024.0) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << mb << " MB";
        return ss.str();
    }
    double gb = mb / 1024.0;
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << gb << " GB";
    return ss.str();
}

std::string formatTimePoint(std::filesystem::file_time_type ftime) {
    try {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
        std::tm tm{};
        localtime_s(&tm, &cftime);
        char buf[64] = {0};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    } catch (...) {
        return "-";
    }
}

std::string detectFileTypeByExtension(const std::filesystem::path& p, bool isDir) {
    if (isDir) return "folder";
    std::string ext = tools3000::core::WinUtils::toLower(p.extension().string());
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);

    if (ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mkd") return "markdown";
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "gif" || ext == "bmp" || ext == "svg" || ext == "ico") return "image";
    if (ext == "mp4" || ext == "webm" || ext == "mkv" || ext == "mov" || ext == "avi") return "video";
    if (ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "flac" || ext == "aac" || ext == "m4a") return "audio";
    if (ext == "pdf") return "pdf";
    if (ext == "json" || ext == "jsonc" || ext == "js" || ext == "mjs" || ext == "cjs" ||
        ext == "ts" || ext == "tsx" || ext == "jsx" || ext == "vue" || ext == "svelte" ||
        ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c" || ext == "h" || ext == "hpp" || ext == "inl" || ext == "cs" ||
        ext == "py" || ext == "rs" || ext == "go" || ext == "java" || ext == "kt" || ext == "lua" ||
        ext == "html" || ext == "htm" || ext == "css" || ext == "scss" || ext == "less" ||
        ext == "xml" || ext == "xaml" || ext == "yaml" || ext == "yml" ||
        ext == "sql" || ext == "prc" || ext == "fnc" || ext == "trg" || ext == "ch" || ext == "pld" ||
        ext == "sh" || ext == "bash" || ext == "ps1" || ext == "psm1" || ext == "bat" || ext == "cmd" || ext == "vbs" ||
        ext == "toml" || ext == "ini" || ext == "cfg" || ext == "conf" || ext == "config" || ext == "reg" ||
        ext == "env" || ext == "log" || ext == "txt" || ext == "csv" || ext == "tsv" ||
        ext == "cmake" || ext == "dockerfile" || ext == "gitignore" || ext == "diff" || ext == "patch") {
        return "code";
    }
    return "binary";
}

std::string formatWin32FileTime(const FILETIME& ft) {
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return "-";
    }
    char buf[64] = {0};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
                  local.wYear, local.wMonth, local.wDay,
                  local.wHour, local.wMinute, local.wSecond);
    return buf;
}

json generateFastInitialPreviewPayload(const std::wstring& filePath, bool& outNeedsAsyncDeepLoad) {
    outNeedsAsyncDeepLoad = false;
    WIN32_FILE_ATTRIBUTE_DATA attrData{};
    if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &attrData)) {
        return {
            {"exists", false},
            {"path", tools3000::core::WinUtils::wstringToUtf8(filePath)},
            {"error", "file not found"}
        };
    }

    const std::filesystem::path p(filePath);
    const bool isDir = (attrData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::string ext = isDir ? "" : tools3000::core::WinUtils::toLower(p.extension().string());
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    const std::string type = detectFileTypeByExtension(p, isDir);

    uintmax_t sizeBytes = 0;
    if (!isDir) {
        sizeBytes = (static_cast<uintmax_t>(attrData.nFileSizeHigh) << 32) |
                    static_cast<uintmax_t>(attrData.nFileSizeLow);
    }

    json payload = {
        {"exists", true},
        {"path", tools3000::core::WinUtils::wstringToUtf8(filePath)},
        {"name", tools3000::core::WinUtils::wstringToUtf8(p.filename().wstring())},
        {"extension", ext},
        {"type", type},
        {"isDirectory", isDir},
        {"size", sizeBytes},
        {"formattedSize", isDir ? (isQuickLookEnglishLocale() ? "Folder" : "文件夹") : formatFileSize(sizeBytes)},
        {"modified", formatWin32FileTime(attrData.ftLastWriteTime)}
    };

    if (isDir) {
        payload["folderChildren"] = json::array();
        outNeedsAsyncDeepLoad = true;
    } else if (type == "markdown" || type == "code" || type == "binary") {
        payload["content"] = "";
        outNeedsAsyncDeepLoad = true;
    }

    return payload;
}

json generateFilePreviewPayload(const std::wstring& filePath) {
    std::error_code ec;
    std::filesystem::path p(filePath);
    if (!std::filesystem::exists(p, ec)) {
        return {
            {"exists", false},
            {"path", tools3000::core::WinUtils::wstringToUtf8(filePath)},
            {"error", "file not found"}
        };
    }

    bool isDir = std::filesystem::is_directory(p, ec);
    std::string ext = isDir ? "" : tools3000::core::WinUtils::toLower(p.extension().string());
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    std::string type = detectFileTypeByExtension(p, isDir);

    uintmax_t sizeBytes = 0;
    if (!isDir) {
        sizeBytes = std::filesystem::file_size(p, ec);
    }

    std::string modifiedTime = formatTimePoint(std::filesystem::last_write_time(p, ec));

    json payload = {
        {"exists", true},
        {"path", tools3000::core::WinUtils::wstringToUtf8(filePath)},
        {"name", tools3000::core::WinUtils::wstringToUtf8(p.filename().wstring())},
        {"extension", ext},
        {"type", type},
        {"isDirectory", isDir},
        {"size", sizeBytes},
        {"formattedSize", isDir ? (isQuickLookEnglishLocale() ? "Folder" : "文件夹") : formatFileSize(sizeBytes)},
        {"modified", modifiedTime}
    };

    if (isDir) {
        json children = json::array();
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(p, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (count >= 50) break;
            bool childIsDir = entry.is_directory(ec);
            uintmax_t childSize = childIsDir ? 0 : entry.file_size(ec);
            children.push_back({
                {"name", tools3000::core::WinUtils::wstringToUtf8(entry.path().filename().wstring())},
                {"isDirectory", childIsDir},
                {"size", childSize},
                {"formattedSize", childIsDir ? "-" : formatFileSize(childSize)}
            });
            count++;
        }
        payload["folderChildren"] = children;
    } else if (type == "markdown" || type == "code" || type == "binary") {
        auto decoded = tools3000::core::TextFilePreviewDecoder::readAndDecodeFile(filePath, sizeBytes);
        if (decoded.fileOpened) {
            if (decoded.isReadableText) {
                if (type == "binary") {
                    payload["type"] = "code";
                }
                payload["encoding"] = decoded.encoding;
                payload["truncated"] = decoded.truncated;
                payload["content"] = std::move(decoded.utf8Text);
            } else {
                payload["type"] = "binary";
                payload["hexDump"] = std::move(decoded.hexDump);
            }
        }
    } else if (type == "image") {
        if (sizeBytes < 30 * 1024 * 1024) {
            HANDLE hImg = CreateFileW(
                filePath.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (hImg != INVALID_HANDLE_VALUE) {
                std::vector<uint8_t> buffer(static_cast<size_t>(sizeBytes));
                DWORD bytesRead = 0;
                if (sizeBytes > 0 && ReadFile(hImg, buffer.data(), static_cast<DWORD>(sizeBytes), &bytesRead, nullptr) && bytesRead > 0) {
                    buffer.resize(bytesRead);
                    std::string mime = "image/" + (ext == "svg" ? "svg+xml" : (ext == "jpg" ? "jpeg" : ext));
                    payload["dataUri"] = "data:" + mime + ";base64," + tools3000::core::WinUtils::base64Encode(buffer);
                }
                CloseHandle(hImg);
            }
        }
    }

    return payload;
}

} // namespace

QuickLookWindow& QuickLookWindow::instance() {
    static QuickLookWindow inst;
    return inst;
}

QuickLookWindow::~QuickLookWindow() {
    shutdownAsyncWorker();
}

void QuickLookWindow::ensureAsyncWorkerStarted() {
    if (m_asyncWorker.joinable()) {
        return;
    }
    m_asyncWorker = std::jthread([this](std::stop_token st) {
        asyncWorkerLoop(st);
    });
}

void QuickLookWindow::shutdownAsyncWorker() {
    {
        std::lock_guard lock(m_asyncMutex);
        m_hasAsyncPreviewTask = false;
        m_hasAsyncStepTask = false;
    }
    if (m_asyncWorker.joinable()) {
        m_asyncWorker.request_stop();
        m_asyncCv.notify_all();
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_asyncWorker, 3000);
    }
}

void QuickLookWindow::asyncWorkerLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        std::wstring previewPath;
        uint64_t previewReqId = 0;
        uint64_t previewGen = 0;
        bool deepDecode = false;
        bool runPreview = false;

        std::wstring stepBase;
        int stepDelta = 0;
        uint64_t stepGen = 0;
        bool runStep = false;

        {
            std::unique_lock lock(m_asyncMutex);
            m_asyncCv.wait(lock, st, [this]() {
                return m_hasAsyncStepTask || m_hasAsyncPreviewTask;
            });
            if (st.stop_requested()) {
                break;
            }
            if (m_hasAsyncStepTask) {
                stepBase = std::move(m_asyncStepBase);
                stepDelta = m_asyncStepDelta;
                stepGen = m_asyncStepGen;
                m_hasAsyncStepTask = false;
                runStep = true;
            } else if (m_hasAsyncPreviewTask) {
                previewPath = std::move(m_asyncPreviewPath);
                previewReqId = m_asyncPreviewReqId;
                previewGen = m_asyncPreviewGen;
                deepDecode = m_asyncDeepDecode;
                m_hasAsyncPreviewTask = false;
                runPreview = true;
            }
        }

        if (runStep) {
            const std::wstring nextPath = resolveSiblingPreviewPath(stepBase, stepDelta);
            if (!st.stop_requested() && !nextPath.empty() &&
                _wcsicmp(nextPath.c_str(), stepBase.c_str()) != 0) {
                tools3000::core::MainThreadDispatcher::instance().postDeferred(
                    [this, stepBase, nextPath, stepGen]() {
                        if (stepGen != m_generation.load() || !isVisible()) {
                            return;
                        }
                        if (_wcsicmp(m_currentFilePath.c_str(), stepBase.c_str()) == 0) {
                            previewFile(nextPath);
                        }
                    });
            }
            continue;
        }

        if (runPreview) {
            if (deepDecode) {
                json fullPayload = generateFilePreviewPayload(previewPath);
                if (st.stop_requested() || previewGen != m_generation.load() ||
                    previewReqId != m_previewRequestId.load()) {
                    continue;
                }
                const std::string dumped =
                    fullPayload.dump(-1, ' ', false, json::error_handler_t::replace);
                const std::string fileType = fullPayload.value("type", "");
                const bool exists = fullPayload.value("exists", false);
                const bool hasDataUri = fullPayload.contains("dataUri");
                const uintmax_t sz = fullPayload.value("size", static_cast<uintmax_t>(0));

                tools3000::core::MainThreadDispatcher::instance().postDeferred(
                    [this, previewPath, previewReqId, previewGen, dumped, fileType, exists, hasDataUri, sz]() {
                        if (previewGen != m_generation.load() ||
                            previewReqId != m_previewRequestId.load() ||
                            !m_visible.load() || !m_webView || !m_webViewReady) {
                            return;
                        }
                        json payload = json::parse(dumped, nullptr, false);
                        if (payload.is_discarded()) {
                            return;
                        }
                        m_currentIsPdfPreview.store(exists && fileType == "pdf", std::memory_order_release);
                        const bool needsMediaMapping =
                            exists &&
                            (fileType == "video" || fileType == "audio" || fileType == "pdf" ||
                             (fileType == "image" && !hasDataUri));
                        if (needsMediaMapping) {
                            std::error_code ec;
                            const std::filesystem::path p(previewPath);
                            const std::wstring parentDir = p.parent_path().wstring();
                            if (!parentDir.empty() && std::filesystem::exists(parentDir, ec)) {
                                updateMediaHostMapping(parentDir);
                                const std::string encodedName = encodeUriPathSegment(
                                    tools3000::core::WinUtils::wstringToUtf8(p.filename().wstring()));
                                const uint64_t rev = ++m_mediaUrlRevision;
                                payload["mediaUrl"] =
                                    std::string("https://quicklook-media.tools3000.local/") +
                                    encodedName + "?s=" + std::to_string(sz) +
                                    "&v=" + std::to_string(rev);
                            }
                        } else {
                            clearMediaHostMapping();
                        }
                        std::string envelope =
                            R"({"type":"event","event":"quicklook.fileChanged","data":)" +
                            payload.dump(-1, ' ', false, json::error_handler_t::replace) + "}";
                        std::wstring wMsg = tools3000::core::WinUtils::utf8ToWstring(envelope);
                        m_webView->PostWebMessageAsString(wMsg.c_str());
                    });
            }

            if (!st.stop_requested() && previewGen == m_generation.load() &&
                previewReqId == m_previewRequestId.load()) {
                warmDirectorySiblingsCache(previewPath);
            }
        }
    }
}

void QuickLookWindow::stepSiblingAsync(int delta) {
    if (delta == 0) return;
    const std::wstring cur = m_currentFilePath;
    if (cur.empty()) return;

    ensureAsyncWorkerStarted();
    {
        std::lock_guard lock(m_asyncMutex);
        m_asyncStepBase = cur;
        m_asyncStepDelta = delta;
        m_asyncStepGen = m_generation.load();
        m_hasAsyncStepTask = true;
    }
    m_asyncCv.notify_one();
}

void QuickLookWindow::show(const std::wstring& filePath, HINSTANCE hInstance) {
    const std::wstring normalizedPath = normalizePreviewFilePath(filePath);
    const bool wasAlreadyVisible = isVisible();
    m_currentFilePath = normalizedPath;
    m_pendingFilePath = normalizedPath;
    m_showTimeTick = GetTickCount64();
    m_visible = true;

    if (m_hwnd && IsWindow(m_hwnd)) {
        syncQuickLookWindowTheme(m_hwnd);
        if (!wasAlreadyVisible) {
            updatePlacement();
        }
        ShowWindow(m_hwnd, SW_SHOW);
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        BringWindowToTop(m_hwnd);
        SetActiveWindow(m_hwnd);
        SetForegroundWindow(m_hwnd);
        SetFocus(m_hwnd);
        m_visible = true;

        if (m_controller) {
            m_controller->put_IsVisible(TRUE);
            m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        previewFile(normalizedPath);
        return;
    }

    if (!createWindow(hInstance ? hInstance : GetModuleHandleW(nullptr))) {
        m_visible = false;
        LOG_ERROR("QuickLookWindow: createWindow failed");
        return;
    }

    initializeWebView2();
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    m_visible = true;
}

void QuickLookWindow::updateMediaHostMapping(const std::wstring& folderPath) {
    const std::wstring cleanFolder = stripExtendedLengthPathPrefix(normalizePreviewFilePath(folderPath));
    if (!m_webView || cleanFolder.empty()) return;
    if (_wcsicmp(m_mappedMediaFolder.c_str(), cleanFolder.c_str()) == 0) {
        return;
    }
    ComPtr<ICoreWebView2_3> webView3;
    if (SUCCEEDED(m_webView.As(&webView3)) && webView3) {
        if (SUCCEEDED(webView3->SetVirtualHostNameToFolderMapping(
                QUICKLOOK_MEDIA_HOST, cleanFolder.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
            m_mappedMediaFolder = cleanFolder;
        }
    }
}

void QuickLookWindow::clearMediaHostMapping() {
    if (!m_webView || m_mappedMediaFolder.empty()) {
        m_mappedMediaFolder.clear();
        return;
    }
    ComPtr<ICoreWebView2_3> webView3;
    if (SUCCEEDED(m_webView.As(&webView3)) && webView3) {
        webView3->ClearVirtualHostNameToFolderMapping(QUICKLOOK_MEDIA_HOST);
    }
    m_mappedMediaFolder.clear();
}

void QuickLookWindow::previewFile(const std::wstring& filePath) {
    const std::wstring normalizedPath = normalizePreviewFilePath(filePath);
    m_currentFilePath = normalizedPath;
    m_pendingFilePath = normalizedPath;
    m_showTimeTick = GetTickCount64();
    m_visible = true;
    const uint64_t requestId = ++m_previewRequestId;
    const uint64_t generation = m_generation.load();

    if (m_webView && m_webViewReady) {
        bool needsAsyncDeepLoad = false;
        json payload = generateFastInitialPreviewPayload(normalizedPath, needsAsyncDeepLoad);
        const std::string fileType = payload.value("type", "");
        const bool exists = payload.value("exists", false);
        m_currentIsPdfPreview.store(exists && fileType == "pdf", std::memory_order_release);
        const bool needsMediaMapping =
            exists &&
            (fileType == "video" || fileType == "audio" || fileType == "pdf" ||
             (fileType == "image" && !payload.contains("dataUri")));
        if (needsMediaMapping) {
            const std::filesystem::path p(normalizedPath);
            const std::wstring parentDir = p.parent_path().wstring();
            if (!parentDir.empty()) {
                updateMediaHostMapping(parentDir);
                const std::string encodedName = encodeUriPathSegment(
                    tools3000::core::WinUtils::wstringToUtf8(p.filename().wstring()));
                const uintmax_t sz = payload.value("size", static_cast<uintmax_t>(0));
                const uint64_t rev = ++m_mediaUrlRevision;
                payload["mediaUrl"] = std::string("https://quicklook-media.tools3000.local/") +
                                      encodedName + "?s=" + std::to_string(sz) +
                                      "&v=" + std::to_string(rev);
            }
        } else {
            clearMediaHostMapping();
        }
        // 首帧 0ms 立即推送当前选中文件元数据或流媒体地址，杜绝磁盘 I/O 阻塞主 UI 线程
        std::string envelope = R"({"type":"event","event":"quicklook.fileChanged","data":)" +
                               payload.dump(-1, ' ', false, json::error_handler_t::replace) + "}";
        std::wstring wMsg = tools3000::core::WinUtils::utf8ToWstring(envelope);
        m_webView->PostWebMessageAsString(wMsg.c_str());

        if (exists) {
            ensureAsyncWorkerStarted();
            {
                std::lock_guard lock(m_asyncMutex);
                m_asyncPreviewPath = normalizedPath;
                m_asyncPreviewReqId = requestId;
                m_asyncPreviewGen = generation;
                m_asyncDeepDecode = needsAsyncDeepLoad;
                m_hasAsyncPreviewTask = true;
            }
            m_asyncCv.notify_one();
        }
    }
}

void QuickLookWindow::hide() {
    ++m_previewRequestId;
    m_currentFilePath.clear();
    m_pendingFilePath.clear();
    m_currentIsPdfPreview.store(false, std::memory_order_release);
    if (m_webView && m_webViewReady) {
        m_webView->PostWebMessageAsJson(
            LR"({"type":"event","event":"quicklook.fileChanged","data":null})");
    }
    if (m_hwnd) {
        clearMediaHostMapping();
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
        if (m_controller) m_controller->put_IsVisible(FALSE);
        tools3000::core::WinUtils::trimWorkingSet();

        // 若搜索窗口仍处于显示状态（用户从 Alt+Space 搜索窗口唤起预览），关闭预览后自动将焦点归还给搜索窗口
        auto& searchWin = SearchWindow::instance();
        if (searchWin.isVisible() && searchWin.getHwnd() && IsWindow(searchWin.getHwnd())) {
            SetForegroundWindow(searchWin.getHwnd());
            searchWin.focusSearchIfVisible();
        }
    }
}

bool QuickLookWindow::isVisible() const {
    return m_visible.load() && m_hwnd && IsWindowVisible(m_hwnd);
}

void QuickLookWindow::destroy() {
    ++m_generation;
    ++m_previewRequestId;
    shutdownAsyncWorker();
    m_currentFilePath.clear();
    m_pendingFilePath.clear();
    m_currentIsPdfPreview.store(false, std::memory_order_release);
    clearMediaHostMapping();
    if (m_controller) {
        m_controller->Close();
        m_controller = nullptr;
    }
    m_webView = nullptr;
    m_environment = nullptr;

    const HWND hwnd = std::exchange(m_hwnd, nullptr);
    if (hwnd && IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    m_visible = false;
    m_webViewReady = false;
}

void QuickLookWindow::updatePlacement() {
    if (!m_hwnd) return;
    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
    const RECT work = tools3000::core::dpi::workArea(monitor);
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = tools3000::core::dpi::scaleForDpi(dpi);

    int targetW = tools3000::core::dpi::scaleMetric(1060, scale);
    int targetH = tools3000::core::dpi::scaleMetric(740, scale);

    const int maxW = static_cast<int>(work.right - work.left) - 40;
    const int maxH = static_cast<int>(work.bottom - work.top) - 40;
    targetW = (std::min)(targetW, maxW);
    targetH = (std::min)(targetH, maxH);

    int x = work.left + (work.right - work.left - targetW) / 2;
    int y = work.top + (work.bottom - work.top - targetH) / 2;

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, targetW, targetH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const int radius = tools3000::core::dpi::scaleMetric(10, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, targetW, targetH, radius);
}

bool QuickLookWindow::createWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
    const RECT work = tools3000::core::dpi::workArea(monitor);
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = tools3000::core::dpi::scaleForDpi(dpi);

    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = QUICKLOOK_WINDOW_CLASS;
    wc.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_DEFAULTCOLOR | LR_SHARED);
    if (!wc.hIcon) wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR | LR_SHARED);
    RegisterClassExW(&wc);

    int targetW = tools3000::core::dpi::scaleMetric(1060, scale);
    int targetH = tools3000::core::dpi::scaleMetric(740, scale);
    int x = work.left + (work.right - work.left - targetW) / 2;
    int y = work.top + (work.bottom - work.top - targetH) / 2;

    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_TOPMOST,
        QUICKLOOK_WINDOW_CLASS,
        L"Tools3000 QuickLook",
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x, y, targetW, targetH,
        nullptr, nullptr, hInstance, this
    );

    if (!m_hwnd) {
        LOG_ERROR("QuickLookWindow: CreateWindowExW failed, err={}", GetLastError());
        return false;
    }

    tools3000::core::WinUtils::setWindowAppUserModelId(m_hwnd, L"Yy1.Tools3000");

    if (wc.hIcon) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    }
    if (wc.hIconSm) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    }

    syncQuickLookWindowTheme(m_hwnd);
    DWORD backdropType = DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    const MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    const int radius = tools3000::core::dpi::scaleMetric(10, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, targetW, targetH, radius);

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    tools3000::core::WinUtils::setWindowAppUserModelId(m_hwnd, L"Yy1.Tools3000");
    return true;
}

void QuickLookWindow::initializeWebView2() {
    const uint64_t generation = ++m_generation;
    WebViewEnvironmentManager::instance().acquire(
        [this, generation](HRESULT result, ICoreWebView2Environment* environment) {
            if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) return;
            if (FAILED(result) || !environment) {
                LOG_ERROR("QuickLookWindow: acquire environment failed");
                return;
            }

            m_environment = environment;
            environment->CreateCoreWebView2Controller(
                m_hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, generation](HRESULT readyResult, ICoreWebView2Controller* controller) -> HRESULT {
                        if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) {
                            if (controller) controller->Close();
                            return S_OK;
                        }
                        if (FAILED(readyResult) || !controller) {
                            LOG_ERROR("QuickLookWindow: CreateCoreWebView2Controller failed");
                            return readyResult;
                        }
                        m_controller = controller;
                        controller->get_CoreWebView2(&m_webView);

                        if (m_visible.load()) {
                            m_controller->put_IsVisible(TRUE);
                        } else {
                            m_controller->put_IsVisible(FALSE);
                        }

                        RECT bounds;
                        GetClientRect(m_hwnd, &bounds);
                        m_controller->put_Bounds(bounds);
                        syncWebViewDpi(m_controller.Get(), m_hwnd);

                        ComPtr<ICoreWebView2Controller2> controller2;
                        if (SUCCEEDED(m_controller.As(&controller2)) && controller2) {
                            COREWEBVIEW2_COLOR transparentColor = { 0, 0, 0, 0 };
                            controller2->put_DefaultBackgroundColor(transparentColor);
                        }

                        ComPtr<ICoreWebView2Settings> settings;
                        m_webView->get_Settings(&settings);
                        if (settings) {
                            settings->put_IsScriptEnabled(TRUE);
                            settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                            settings->put_IsWebMessageEnabled(TRUE);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(FALSE);

                            ComPtr<ICoreWebView2Settings3> settings3;
                            if (SUCCEEDED(settings.As(&settings3)) && settings3) {
                                settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                            }
                        }

                        const auto uiFolder = (tools3000::core::WinUtils::getExeDirectory() / L"ui").wstring();
                        ComPtr<ICoreWebView2_3> webView3;
                        if (SUCCEEDED(m_webView.As(&webView3)) && webView3) {
                            webView3->SetVirtualHostNameToFolderMapping(
                                L"tools3000.local", uiFolder.c_str(),
                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                        }

                        web_security::applyNavigationPolicy(m_webView.Get());
                        KeyboardPipeline::applyWebKeyboardPolicy(m_controller.Get());

                        // JS -> C++ IPC Message Bridge
                        m_webView->add_WebMessageReceived(
                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [this, generation](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    if (!web_security::isTrustedMessageSource(args)) return S_OK;
                                    try {
                                        LPWSTR msgRaw = nullptr;
                                        args->TryGetWebMessageAsString(&msgRaw);
                                        if (msgRaw) {
                                            std::string message = tools3000::core::WinUtils::wstringToUtf8(msgRaw);
                                            CoTaskMemFree(msgRaw);
                                            if (!web_security::isBridgeMethodAllowed(
                                                    message, web_security::Surface::QuickLook)) return S_OK;
                                            auto parsed = json::parse(message, nullptr, false);
                                            if (parsed.is_object() && parsed.value("method", "") == "quicklook.startDrag") {
                                                if (generation == m_generation.load() && m_hwnd && IsWindow(m_hwnd) && !IsZoomed(m_hwnd)) {
                                                    ReleaseCapture();
                                                    SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                                                }
                                                json reply = {
                                                    {"id", parsed.contains("id") ? parsed["id"] : json(nullptr)},
                                                    {"result", {{"success", true}}}
                                                };
                                                std::wstring wReply = tools3000::core::WinUtils::utf8ToWstring(reply.dump());
                                                sender->PostWebMessageAsString(wReply.c_str());
                                                return S_OK;
                                            }
                                            if (parsed.is_object() && parsed.value("method", "") == "quicklook.startResize") {
                                                const auto paramsObj = parsed.contains("params") && parsed["params"].is_object()
                                                    ? parsed["params"] : json::object();
                                                const std::string edge = paramsObj.value("edge", paramsObj.value("direction", "bottom_right"));
                                                WPARAM scSizeParam = 0;
                                                if (edge == "left" || edge == "w") scSizeParam = 0xF001;
                                                else if (edge == "right" || edge == "e") scSizeParam = 0xF002;
                                                else if (edge == "top" || edge == "n") scSizeParam = 0xF003;
                                                else if (edge == "top_left" || edge == "nw") scSizeParam = 0xF004;
                                                else if (edge == "top_right" || edge == "ne") scSizeParam = 0xF005;
                                                else if (edge == "bottom" || edge == "s") scSizeParam = 0xF006;
                                                else if (edge == "bottom_left" || edge == "sw") scSizeParam = 0xF007;
                                                else if (edge == "bottom_right" || edge == "se") scSizeParam = 0xF008;
                                                if (scSizeParam != 0 && generation == m_generation.load() && m_hwnd && IsWindow(m_hwnd) && !IsZoomed(m_hwnd)) {
                                                    SetForegroundWindow(m_hwnd);
                                                    ReleaseCapture();
                                                    POINT pt{};
                                                    LPARAM lParam = 0;
                                                    if (GetCursorPos(&pt)) {
                                                        lParam = MAKELPARAM(pt.x, pt.y);
                                                    }
                                                    PostMessageW(m_hwnd, WM_SYSCOMMAND, scSizeParam, lParam);
                                                }
                                                json reply = {
                                                    {"id", parsed.contains("id") ? parsed["id"] : json(nullptr)},
                                                    {"result", {{"success", scSizeParam != 0}}}
                                                };
                                                std::wstring wReply = tools3000::core::WinUtils::utf8ToWstring(reply.dump());
                                                sender->PostWebMessageAsString(wReply.c_str());
                                                return S_OK;
                                            }
                                            std::string response = tools3000::core::MessageBridge::instance().handleMessage(message);
                                            std::wstring wResponse = tools3000::core::WinUtils::utf8ToWstring(response);
                                            sender->PostWebMessageAsString(wResponse.c_str());
                                        }
                                    } catch (const std::exception& error) {
                                        LOG_WARN("QuickLook WebMessage 处理异常: {}", error.what());
                                    } catch (...) {
                                        LOG_WARN("QuickLook WebMessage 处理发生未知异常");
                                    }
                                    return S_OK;
                                }
                            ).Get(), nullptr
                        );

                        EventRegistrationToken token;
                        m_webView->add_NavigationCompleted(
                            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                    BOOL success;
                                    args->get_IsSuccess(&success);
                                    if (success) {
                                        m_webViewReady = true;
                                        if (!m_pendingFilePath.empty()) {
                                            previewFile(m_pendingFilePath);
                                        }
                                    }
                                    return S_OK;
                                }
                            ).Get(), &token);

                        std::string targetUrl = "https://tools3000.local/index.html?quicklook=1";
#ifdef _DEBUG
                        auto devUrlPath = tools3000::core::WinUtils::getExeDirectory().parent_path().parent_path().parent_path() / L"ui" / L".dev-server-url";
                        std::error_code ec;
                        if (std::filesystem::exists(devUrlPath, ec)) {
                            std::ifstream f(devUrlPath);
                            std::string u;
                            if (std::getline(f, u) && !u.empty()) {
                                targetUrl = u + "?quicklook=1";
                            }
                        }
#endif
                        std::wstring wUrl = tools3000::core::WinUtils::utf8ToWstring(targetUrl);
                        m_webView->Navigate(wUrl.c_str());
                        return S_OK;
                    }
                ).Get()
            );
        }
    );
}

LRESULT CALLBACK QuickLookWindow::windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<QuickLookWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            const COLORREF bgColor = isQuickLookDarkTheme() ? RGB(22, 24, 30) : RGB(255, 255, 255);
            HBRUSH hBrush = CreateSolidBrush(bgColor);
            if (hBrush) {
                FillRect(hdc, &ps.rcPaint, hBrush);
                DeleteObject(hBrush);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCCALCSIZE: {
            if (wParam == TRUE) {
                return 0;
            }
            break;
        }
        case WM_NCHITTEST: {
            if (!IsZoomed(hwnd)) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT rc{};
                if (GetWindowRect(hwnd, &rc)) {
                    const float scale = tools3000::core::dpi::scaleForWindow(hwnd);
                    const int border = tools3000::core::dpi::scaleMetric(8, scale);
                    const bool left = pt.x >= rc.left && pt.x < rc.left + border;
                    const bool right = pt.x <= rc.right && pt.x > rc.right - border;
                    const bool top = pt.y >= rc.top && pt.y < rc.top + border;
                    const bool bottom = pt.y <= rc.bottom && pt.y > rc.bottom - border;
                    if (top && left) return HTTOPLEFT;
                    if (top && right) return HTTOPRIGHT;
                    if (bottom && left) return HTBOTTOMLEFT;
                    if (bottom && right) return HTBOTTOMRIGHT;
                    if (left) return HTLEFT;
                    if (right) return HTRIGHT;
                    if (top) return HTTOP;
                    if (bottom) return HTBOTTOM;
                }
            }
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            if (auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam)) {
                const float scale = tools3000::core::dpi::scaleForWindow(hwnd);
                mmi->ptMinTrackSize.x = tools3000::core::dpi::scaleMetric(480, scale);
                mmi->ptMinTrackSize.y = tools3000::core::dpi::scaleMetric(320, scale);
                return 0;
            }
            break;
        }
        case WM_SIZE: {
            const int newW = LOWORD(lParam);
            const int newH = HIWORD(lParam);
            if (newW > 0 && newH > 0) {
                if (IsZoomed(hwnd)) {
                    SetWindowRgn(hwnd, nullptr, TRUE);
                } else {
                    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
                    const int radius = tools3000::core::dpi::scaleMetric(10, scale);
                    tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, newW, newH, radius);
                }
            }
            if (self && self->m_controller) {
                RECT bounds{};
                GetClientRect(hwnd, &bounds);
                self->m_controller->put_Bounds(bounds);
                syncWebViewDpi(self->m_controller.Get(), hwnd);
                if (IsWindowVisible(hwnd)) {
                    self->m_controller->put_IsVisible(TRUE);
                }
            }
            return 0;
        }
        case WM_DPICHANGED: {
            if (const auto* suggested = reinterpret_cast<RECT*>(lParam)) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (self && self->m_controller) {
                syncWebViewDpi(self->m_controller.Get(), hwnd);
            }
            if (!IsZoomed(hwnd)) {
                RECT winRc{};
                if (GetWindowRect(hwnd, &winRc)) {
                    const float scale = tools3000::core::dpi::scaleForWindow(hwnd);
                    const int radius = tools3000::core::dpi::scaleMetric(10, scale);
                    tools3000::core::WinUtils::applyUniversalRoundedCorners(
                        hwnd, winRc.right - winRc.left, winRc.bottom - winRc.top, radius);
                }
            }
            return 0;
        }
        case WM_ACTIVATE: {
        if (LOWORD(wParam) == WA_INACTIVE) {
                PostMessageW(hwnd, WM_QUICKLOOK_VERIFY_DEACTIVATED, 0, 0);
            } else if (self && self->m_controller) {
                self->m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
            break;
        }
        case WM_QUICKLOOK_VERIFY_DEACTIVATED: {
            if (!self || !self->isVisible()) return 0;
            if (!self->m_webViewReady) return 0;
            const uint64_t elapsed = GetTickCount64() - self->m_showTimeTick;
            if (elapsed < 400) {
                return 0;
            }
            const HWND foreground = GetForegroundWindow();
            const HWND searchHwnd = SearchWindow::instance().getHwnd();
            const bool isSearchForeground = searchHwnd && SearchWindow::instance().isVisible() &&
                                            (foreground == searchHwnd || IsChild(searchHwnd, foreground));
            if (foreground != hwnd && (!foreground || !IsChild(hwnd, foreground)) && !isSearchForeground) {
                self->hide();
            }
            return 0;
        }
        case WM_SYSCOMMAND:
        case WM_HELP: {
            if (KeyboardPipeline::filterWindowMessage(hwnd, uMsg, wParam, lParam)) {
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            if (self) self->hide();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

} // namespace tools3000::ui

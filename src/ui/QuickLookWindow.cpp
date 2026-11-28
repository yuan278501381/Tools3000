#include "ui/QuickLookWindow.h"
#include "ui/SearchWindow.h"
#include "ui/KeyboardPipeline.h"
#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"
#include "core/utils/TextFilePreviewDecoder.h"
#include "core/utils/OfficeAndPdfPreview.h"
#include "core/utils/DesignAndCadPreview.h"
#include "core/utils/ArchivePreview.h"
#include "core/utils/FontPreview.h"
#include "ui/WebViewEnvironmentManager.h"
#include "ui/WebViewDpi.h"
#include "ui/WebViewWindowStyle.h"
#include "ui/WebViewSecurity.h"
#include "ui/QuickLookNavigation.h"
#include <WebView2.h>
#include <wrl/event.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <windowsx.h>
#include "../resource.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
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
    return !tools3000::core::isUiLanguageChinese();
}

bool isQuickLookDarkTheme() {
    return tools3000::core::NativeThemePalette::isEffectiveDarkMode();
}

void syncQuickLookWindowTheme(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    tools3000::core::WinUtils::applyImmersiveDarkMode(hwnd, isQuickLookDarkTheme());
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

std::string extractLowerExtUtf8(const std::filesystem::path& p, bool isDir) {
    if (isDir) return {};
    std::string ext = tools3000::core::WinUtils::toLower(
        tools3000::core::WinUtils::wstringToUtf8(p.extension().wstring()));
    if (!ext.empty() && ext[0] == '.') {
        ext.erase(0, 1);
    }
    return ext;
}

std::mutex g_cachedPinMutex;
std::wstring g_cachedPinSourcePath;
std::string g_cachedPinDataUri;

void updateCachedPinDataUri(const std::wstring& filePath, const std::string& dataUri) {
    std::lock_guard lock(g_cachedPinMutex);
    if (dataUri.empty()) {
        g_cachedPinSourcePath.clear();
        g_cachedPinDataUri.clear();
    } else {
        g_cachedPinSourcePath = filePath;
        g_cachedPinDataUri = dataUri;
    }
}

void clearCachedPinDataUri() {
    std::lock_guard lock(g_cachedPinMutex);
    g_cachedPinSourcePath.clear();
    g_cachedPinDataUri.clear();
    g_cachedPinDataUri.shrink_to_fit();
}

bool writeDataUriToTempImageFile(const std::string& dataUri, std::wstring& outTempPath) {
    outTempPath.clear();
    const size_t comma = dataUri.find(";base64,");
    if (dataUri.rfind("data:image/", 0) != 0 || comma == std::string::npos) {
        return false;
    }
    const std::string_view fmt(dataUri.data() + 11, comma - 11);
    const wchar_t* ext = L".png";
    if (fmt == "jpeg" || fmt == "jpg") ext = L".jpg";
    else if (fmt == "bmp") ext = L".bmp";
    else if (fmt == "webp") ext = L".webp";

    const char* b64Start = dataUri.data() + comma + 8;
    const DWORD b64Len = static_cast<DWORD>(dataUri.size() - (comma + 8));
    DWORD binLen = 0;
    if (!CryptStringToBinaryA(b64Start, b64Len, CRYPT_STRING_BASE64, nullptr, &binLen, nullptr, nullptr) ||
        binLen == 0 || binLen > 64ULL * 1024ULL * 1024ULL) {
        return false;
    }
    std::vector<uint8_t> decoded(binLen);
    if (!CryptStringToBinaryA(b64Start, b64Len, CRYPT_STRING_BASE64, decoded.data(), &binLen, nullptr, nullptr)) {
        return false;
    }
    decoded.resize(binLen);

    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) {
        return false;
    }
    wchar_t fileName[96] = {};
    std::swprintf(fileName, std::size(fileName), L"Tools3000_QLPin_%lu_%llu%ls",
                  GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()), ext);
    const std::filesystem::path fullTemp = std::filesystem::path(tempDir) / fileName;

    HANDLE hOut = CreateFileW(
        fullTemp.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(hOut, decoded.data(), static_cast<DWORD>(decoded.size()), &written, nullptr);
    CloseHandle(hOut);
    if (!ok || written != decoded.size()) {
        DeleteFileW(fullTemp.c_str());
        return false;
    }
    outTempPath = fullTemp.wstring();
    return true;
}

std::string encodeFloat32BufferBase64(const std::vector<float>& floats) {
    if (floats.empty()) return {};
    const auto* rawBytes = reinterpret_cast<const uint8_t*>(floats.data());
    const size_t byteLen = floats.size() * sizeof(float);
    DWORD b64Chars = 0;
    if (!CryptBinaryToStringA(
            rawBytes,
            static_cast<DWORD>(byteLen),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &b64Chars) || b64Chars == 0) {
        return {};
    }
    std::string out(b64Chars, '\0');
    DWORD written = b64Chars;
    if (!CryptBinaryToStringA(
            rawBytes,
            static_cast<DWORD>(byteLen),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            out.data(),
            &written)) {
        return {};
    }
    out.resize(written);
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

double roundCoordForJson(double v) noexcept {
    if (!std::isfinite(v)) return 0.0;
    return std::round(v * 1000.0) * 0.001;
}

bool isExtendedWicImageExt(const std::string& ext) {
    return ext == "heic" || ext == "heif" || ext == "tif" || ext == "tiff" ||
           ext == "tga" || ext == "dds" || ext == "jxl" ||
           ext == "dng" || ext == "cr2" || ext == "cr3" || ext == "nef" ||
           ext == "arw" || ext == "orf" || ext == "rw2" || ext == "raf";
}

bool isVideoNeedsProbeFallbackImmediately(const std::string& ext) {
    return ext == "avi" || ext == "wmv" || ext == "flv" || ext == "rmvb" ||
           ext == "vob" || ext == "asf" || ext == "ts" || ext == "m2ts" ||
           ext == "mts" || ext == "mpg" || ext == "mpeg" || ext == "3gp";
}

std::wstring mimeTypeForPreviewExtension(const std::wstring& extLower) {
    if (extLower == L".png" || extLower == L".apng") return L"image/png";
    if (extLower == L".jpg" || extLower == L".jpeg" || extLower == L".jfif") return L"image/jpeg";
    if (extLower == L".webp") return L"image/webp";
    if (extLower == L".gif") return L"image/gif";
    if (extLower == L".bmp") return L"image/bmp";
    if (extLower == L".svg") return L"image/svg+xml";
    if (extLower == L".ico" || extLower == L".cur") return L"image/x-icon";
    if (extLower == L".avif") return L"image/avif";
    if (extLower == L".mp4" || extLower == L".m4v") return L"video/mp4";
    if (extLower == L".webm") return L"video/webm";
    if (extLower == L".ogv") return L"video/ogg";
    if (extLower == L".mov") return L"video/quicktime";
    if (extLower == L".mkv") return L"video/x-matroska";
    if (extLower == L".avi") return L"video/x-msvideo";
    if (extLower == L".mp3") return L"audio/mpeg";
    if (extLower == L".wav") return L"audio/wav";
    if (extLower == L".ogg") return L"audio/ogg";
    if (extLower == L".flac") return L"audio/flac";
    if (extLower == L".aac") return L"audio/aac";
    if (extLower == L".m4a") return L"audio/mp4";
    if (extLower == L".pdf") return L"application/pdf";
    if (extLower == L".ttf" || extLower == L".ttc") return L"font/ttf";
    if (extLower == L".otf") return L"font/otf";
    if (extLower == L".woff") return L"font/woff";
    if (extLower == L".woff2") return L"font/woff2";
    return L"application/octet-stream";
}

// 零锁有界文件 COM IStream：直接流式传输任意体积媒体文件（支持 HTTP Range/206 拖动），
// 内存恒为 ~0 拷贝，不会将 GB 级视频加载进 RAM，且以 FILE_SHARE_DELETE 绝不锁定文件。
class BoundedFileIStream final : public IStream {
public:
    BoundedFileIStream(HANDLE hFile, uint64_t startOffset, uint64_t length) noexcept
        : m_file(hFile), m_start(startOffset), m_length(length) {}

    ~BoundedFileIStream() {
        if (m_file != INVALID_HANDLE_VALUE && m_file != nullptr) {
            CloseHandle(m_file);
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refCount.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const long next = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (next == 0) {
            delete this;
        }
        return static_cast<ULONG>(next);
    }

    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (pcbRead) *pcbRead = 0;
        if (!pv) return STG_E_INVALIDPOINTER;
        if (m_pos >= m_length || cb == 0) return S_FALSE;

        const uint64_t remaining = m_length - m_pos;
        const DWORD toRead = static_cast<DWORD>((std::min)(static_cast<uint64_t>(cb), remaining));
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(m_start + m_pos);
        if (!SetFilePointerEx(m_file, offset, nullptr, FILE_BEGIN)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        DWORD got = 0;
        if (!ReadFile(m_file, pv, toRead, &got, nullptr)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        m_pos += got;
        if (pcbRead) *pcbRead = got;
        return (got < cb) ? S_FALSE : S_OK;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override {
        int64_t base = 0;
        if (dwOrigin == STREAM_SEEK_SET) base = 0;
        else if (dwOrigin == STREAM_SEEK_CUR) base = static_cast<int64_t>(m_pos);
        else if (dwOrigin == STREAM_SEEK_END) base = static_cast<int64_t>(m_length);
        else return STG_E_INVALIDFUNCTION;

        const int64_t next = base + dlibMove.QuadPart;
        if (next < 0) return STG_E_INVALIDFUNCTION;
        m_pos = static_cast<uint64_t>(next);
        if (plibNewPosition) plibNewPosition->QuadPart = m_pos;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD) override {
        if (!pstatstg) return STG_E_INVALIDPOINTER;
        std::memset(pstatstg, 0, sizeof(STATSTG));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = m_length;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    std::atomic<long> m_refCount{1};
    HANDLE m_file = INVALID_HANDLE_VALUE;
    uint64_t m_start = 0;
    uint64_t m_length = 0;
    uint64_t m_pos = 0;
};

class MemoryBufferIStream final : public IStream {
public:
    explicit MemoryBufferIStream(std::vector<uint8_t> data) noexcept : m_data(std::move(data)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refCount.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const long next = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (next == 0) {
            delete this;
        }
        return static_cast<ULONG>(next);
    }

    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (pcbRead) *pcbRead = 0;
        if (!pv) return STG_E_INVALIDPOINTER;
        if (m_pos >= m_data.size() || cb == 0) return S_FALSE;
        const size_t avail = m_data.size() - m_pos;
        const size_t n = (std::min)(static_cast<size_t>(cb), avail);
        std::memcpy(pv, m_data.data() + m_pos, n);
        m_pos += n;
        if (pcbRead) *pcbRead = static_cast<ULONG>(n);
        return (n < cb) ? S_FALSE : S_OK;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override {
        int64_t base = 0;
        if (dwOrigin == STREAM_SEEK_SET) base = 0;
        else if (dwOrigin == STREAM_SEEK_CUR) base = static_cast<int64_t>(m_pos);
        else if (dwOrigin == STREAM_SEEK_END) base = static_cast<int64_t>(m_data.size());
        else return STG_E_INVALIDFUNCTION;
        const int64_t next = base + dlibMove.QuadPart;
        if (next < 0) return STG_E_INVALIDFUNCTION;
        m_pos = static_cast<size_t>(next);
        if (plibNewPosition) plibNewPosition->QuadPart = m_pos;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD) override {
        if (!pstatstg) return STG_E_INVALIDPOINTER;
        std::memset(pstatstg, 0, sizeof(STATSTG));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = m_data.size();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    std::atomic<long> m_refCount{1};
    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
};

bool parseHttpByteRangeHeader(const std::wstring& rangeHeader,
                              uint64_t totalSize,
                              uint64_t& outStart,
                              uint64_t& outEnd) noexcept {
    if (totalSize == 0) return false;
    const size_t eq = rangeHeader.find(L"bytes=");
    if (eq == std::wstring::npos) return false;
    std::wstring spec = rangeHeader.substr(eq + 6);
    const size_t comma = spec.find(L',');
    if (comma != std::wstring::npos) spec = spec.substr(0, comma);
    const size_t dash = spec.find(L'-');
    if (dash == std::wstring::npos) return false;

    const std::wstring startStr = spec.substr(0, dash);
    const std::wstring endStr = spec.substr(dash + 1);
    try {
        if (startStr.empty()) {
            // suffix-byte-range: bytes=-N
            if (endStr.empty()) return false;
            const uint64_t suffix = std::stoull(endStr);
            if (suffix == 0) return false;
            outEnd = totalSize - 1;
            outStart = (suffix >= totalSize) ? 0 : (totalSize - suffix);
            return true;
        }
        outStart = std::stoull(startStr);
        if (outStart >= totalSize) return false;
        if (endStr.empty()) {
            outEnd = totalSize - 1;
        } else {
            outEnd = std::stoull(endStr);
            if (outEnd >= totalSize) outEnd = totalSize - 1;
        }
        return outStart <= outEnd;
    } catch (...) {
        return false;
    }
}

std::string queryParamValueFromUri(const std::wstring& uri, const wchar_t* keyWithEq) {
    const size_t qPos = uri.find(L'?');
    if (qPos == std::wstring::npos) return {};
    const std::wstring query = uri.substr(qPos + 1);
    const std::wstring key(keyWithEq);
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find(L'&', pos);
        if (amp == std::wstring::npos) amp = query.size();
        const std::wstring part = query.substr(pos, amp - pos);
        if (part.rfind(key, 0) == 0) {
            return tools3000::core::WinUtils::wstringToUtf8(part.substr(key.size()));
        }
        pos = amp + 1;
    }
    return {};
}

json runIsolatedMediaProbeWorker(const std::wstring& filePath, std::stop_token st) {
    const std::filesystem::path exePath = tools3000::core::WinUtils::getExePath();
    if (exePath.empty()) return json::object();

    std::error_code ec;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec) / L"Tools3000_MediaProbe";
    std::filesystem::create_directories(tempDir, ec);
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    const std::filesystem::path jobPath = tempDir / (L"job_" + suffix + L".json");
    const std::filesystem::path outPath = tempDir / (L"out_" + suffix + L".json");

    struct CleanupFiles {
        std::filesystem::path a;
        std::filesystem::path b;
        ~CleanupFiles() {
            std::error_code e;
            std::filesystem::remove(a, e);
            std::filesystem::remove(b, e);
        }
    } cleanup{jobPath, outPath};

    const json jobJson = {
        {"inputPath", tools3000::core::WinUtils::pathToUtf8(std::filesystem::path(filePath))},
        {"outputPath", tools3000::core::WinUtils::pathToUtf8(outPath)},
        {"maxFrames", 5},
        {"maxThumbWidth", 420}
    };
    if (!tools3000::core::WinUtils::atomicWriteFileWithFlush(jobPath.wstring(), jobJson.dump())) {
        return json::object();
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = L"\"" + exePath.wstring() + L"\" --worker=media-probe --job=\"" + jobPath.wstring() + L"\"";
    if (!CreateProcessW(
            exePath.wstring().c_str(),
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
            nullptr,
            exePath.parent_path().wstring().c_str(),
            &si,
            &pi)) {
        return json::object();
    }
    tools3000::core::WinUtils::assignProcessToCurrentJob(pi.hProcess);

    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        if (st.stop_requested()) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 40) == WAIT_OBJECT_0) {
            break;
        }
    }
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (st.stop_requested() || !std::filesystem::is_regular_file(outPath, ec)) {
        return json::object();
    }
    std::ifstream ifs(outPath, std::ios::binary);
    if (!ifs) return json::object();
    json parsed = json::parse(ifs, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

std::string detectFileTypeByExtension(const std::filesystem::path& p, bool isDir) {
    if (isDir) return "folder";
    const std::string ext = extractLowerExtUtf8(p, false);

    if (ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mkd") return "markdown";
    if (ext == "csv" || ext == "tsv") return "csv";
    if (ext == "xlsx") return "spreadsheet";
    if (ext == "docx" || ext == "pptx") return "document";
    if (ext == "doc" || ext == "xls" || ext == "ppt" || ext == "rtf") return "office_legacy";
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "jfif" || ext == "webp" ||
        ext == "gif" || ext == "bmp" || ext == "svg" || ext == "ico" || ext == "cur" ||
        ext == "avif" || ext == "apng" || isExtendedWicImageExt(ext)) {
        return "image";
    }
    if (ext == "psd" || ext == "psb") return "psd";
    if (ext == "cdr") return "cdr";
    if (ext == "ai" || ext == "eps") return "vector_design";
    if (ext == "dwg" || ext == "sldprt" || ext == "sldasm" || ext == "skp") return "cad_snapshot";
    if (ext == "dxf") return "cad_dxf";
    if (ext == "stl" || ext == "obj") return "model_3d";
    if (ext == "mp4" || ext == "m4v" || ext == "webm" || ext == "mkv" || ext == "mov" ||
        ext == "avi" || ext == "wmv" || ext == "flv" || ext == "ts" || ext == "m2ts" ||
        ext == "mts" || ext == "mpg" || ext == "mpeg" || ext == "3gp" || ext == "ogv" ||
        ext == "rmvb" || ext == "vob" || ext == "asf") {
        return "video";
    }
    if (ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "flac" || ext == "aac" || ext == "m4a") return "audio";
    if (ext == "pdf") return "pdf";
    if (ext == "zip" || ext == "jar" || ext == "apk" || ext == "whl" || ext == "nupkg" ||
        ext == "vsix" || ext == "xpi" || ext == "epub" || ext == "tar" || ext == "tgz" ||
        ext == "gz" || ext == "7z" || ext == "rar" || ext == "xz" || ext == "txz" ||
        ext == "bz2" || ext == "tbz2" || ext == "zst" || ext == "tzst") {
        return "archive";
    }
    if (ext == "ttf" || ext == "otf" || ext == "woff" || ext == "woff2" || ext == "ttc") {
        return "font";
    }
    if (ext == "json" || ext == "jsonc" || ext == "js" || ext == "mjs" || ext == "cjs" ||
        ext == "ts" || ext == "tsx" || ext == "jsx" || ext == "vue" || ext == "svelte" ||
        ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c" || ext == "h" || ext == "hpp" || ext == "inl" || ext == "cs" ||
        ext == "py" || ext == "rs" || ext == "go" || ext == "java" || ext == "kt" || ext == "lua" ||
        ext == "html" || ext == "htm" || ext == "css" || ext == "scss" || ext == "less" ||
        ext == "xml" || ext == "xaml" || ext == "yaml" || ext == "yml" ||
        ext == "sql" || ext == "prc" || ext == "fnc" || ext == "trg" || ext == "ch" || ext == "pld" ||
        ext == "sh" || ext == "bash" || ext == "ps1" || ext == "psm1" || ext == "bat" || ext == "cmd" || ext == "vbs" ||
        ext == "toml" || ext == "ini" || ext == "cfg" || ext == "conf" || ext == "config" || ext == "reg" ||
        ext == "env" || ext == "log" || ext == "txt" ||
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
    const std::string ext = extractLowerExtUtf8(p, isDir);
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
        payload["loading"] = true;
        outNeedsAsyncDeepLoad = true;
    } else if (type == "markdown" || type == "code" || type == "csv" ||
               type == "spreadsheet" || type == "document" || type == "office_legacy" ||
               type == "pdf" || type == "psd" || type == "cdr" || type == "vector_design" ||
               type == "cad_snapshot" || type == "cad_dxf" || type == "model_3d" ||
               type == "archive" || type == "font" || type == "binary" ||
               (type == "image" && isExtendedWicImageExt(ext)) ||
               (type == "video" && isVideoNeedsProbeFallbackImmediately(ext))) {
        payload["content"] = "";
        payload["loading"] = true;
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
    std::string ext = extractLowerExtUtf8(p, isDir);
    std::string type = detectFileTypeByExtension(p, isDir);

    uintmax_t sizeBytes = 0;
    if (!isDir) {
        sizeBytes = std::filesystem::file_size(p, ec);
    }

    std::string modifiedTime = formatTimePoint(std::filesystem::last_write_time(p, ec));

    json payload = {
        {"exists", true},
        {"loading", false},
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
    } else if (type == "csv") {
        auto decoded = tools3000::core::TextFilePreviewDecoder::readAndDecodeFile(filePath, sizeBytes);
        if (decoded.fileOpened) {
            if (decoded.isReadableText) {
                payload["encoding"] = decoded.encoding;
                payload["truncated"] = decoded.truncated;
                payload["csvDelimiter"] = (ext == "tsv") ? "\t" : ",";
                payload["content"] = std::move(decoded.utf8Text);
            } else {
                payload["type"] = "binary";
                payload["hexDump"] = std::move(decoded.hexDump);
            }
        }
    } else if (type == "spreadsheet") {
        auto xls = tools3000::core::OfficeAndPdfPreview::extractXlsxPreview(filePath);
        if (xls.ok) {
            json sheetsJson = json::array();
            bool anySheetTruncated = xls.truncatedSheets;
            for (const auto& sh : xls.sheets) {
                if (sh.truncatedRows || sh.truncatedCols) {
                    anySheetTruncated = true;
                }
                sheetsJson.push_back({
                    {"name", sh.name},
                    {"rows", sh.rows},
                    {"truncatedRows", sh.truncatedRows},
                    {"truncatedCols", sh.truncatedCols}
                });
            }
            payload["sheets"] = std::move(sheetsJson);
            payload["totalSheetCount"] = xls.totalSheetCount;
            payload["truncated"] = anySheetTruncated;
        } else if (!xls.encryptedOrUnsupported) {
            auto legacy = tools3000::core::OfficeAndPdfPreview::extractLegacyOfficePreview(filePath);
            if (legacy.ok) {
                payload["type"] = "document";
                payload["content"] = std::move(legacy.markdownText);
                payload["truncated"] = legacy.truncated;
            } else {
                auto decoded = tools3000::core::TextFilePreviewDecoder::readAndDecodeFile(filePath, sizeBytes);
                if (decoded.fileOpened && decoded.isReadableText && !decoded.utf8Text.empty()) {
                    const bool hasTab = decoded.utf8Text.find('\t') != std::string::npos;
                    payload["type"] = hasTab ? "csv" : "code";
                    if (hasTab) payload["csvDelimiter"] = "\t";
                    payload["encoding"] = decoded.encoding;
                    payload["truncated"] = decoded.truncated;
                    payload["content"] = std::move(decoded.utf8Text);
                } else {
                    payload["officeFallback"] = true;
                }
            }
        } else {
            payload["encrypted"] = true;
            payload["officeFallback"] = true;
        }
    } else if (type == "document") {
        auto doc = tools3000::core::OfficeAndPdfPreview::extractDocxOrPptxPreview(filePath, ext);
        if (doc.ok) {
            payload["content"] = std::move(doc.markdownText);
            payload["truncated"] = doc.truncated;
            payload["emptyDocument"] = doc.emptyDocument;
            if (!doc.embeddedImages.empty()) {
                payload["embeddedImages"] = std::move(doc.embeddedImages);
            }
        } else if (!doc.encryptedOrUnsupported) {
            auto legacy = tools3000::core::OfficeAndPdfPreview::extractLegacyOfficePreview(filePath);
            if (legacy.ok) {
                payload["content"] = std::move(legacy.markdownText);
                payload["truncated"] = legacy.truncated;
            } else {
                auto decoded = tools3000::core::TextFilePreviewDecoder::readAndDecodeFile(filePath, sizeBytes);
                if (decoded.fileOpened && decoded.isReadableText && !decoded.utf8Text.empty()) {
                    payload["type"] = "code";
                    payload["encoding"] = decoded.encoding;
                    payload["truncated"] = decoded.truncated;
                    payload["content"] = std::move(decoded.utf8Text);
                } else {
                    payload["officeFallback"] = true;
                }
            }
        } else {
            payload["encrypted"] = true;
            payload["officeFallback"] = true;
        }
    } else if (type == "office_legacy") {
        auto legacy = tools3000::core::OfficeAndPdfPreview::extractLegacyOfficePreview(filePath);
        if (legacy.ok) {
            payload["type"] = "document";
            payload["content"] = std::move(legacy.markdownText);
            payload["truncated"] = legacy.truncated;
        } else {
            payload["officeFallback"] = true;
        }
    } else if (type == "pdf") {
        auto pdf = tools3000::core::OfficeAndPdfPreview::renderPdfPreviewPages(filePath, 6, 1240);
        if (pdf.passwordProtected) {
            payload["pdfPasswordProtected"] = true;
        } else if (pdf.ok) {
            payload["pdfPageCount"] = pdf.totalPageCount;
            payload["pdfPages"] = std::move(pdf.pageDataUris);
        }
    } else if (type == "vector_design") {
        auto pdf = tools3000::core::OfficeAndPdfPreview::renderAiOrEpsPreviewPages(filePath, 6, 1240);
        if (pdf.passwordProtected) {
            payload["type"] = "pdf";
            payload["pdfPasswordProtected"] = true;
        } else if (pdf.ok) {
            payload["type"] = "pdf";
            payload["pdfPageCount"] = pdf.totalPageCount;
            payload["pdfPages"] = std::move(pdf.pageDataUris);
        } else {
            auto snap = tools3000::core::DesignAndCadPreview::extractCadOr3dSnapshotPreview(filePath, ext, true);
            if (!snap.metaSummary.empty()) {
                payload["designMeta"] = snap.metaSummary;
            }
            if (snap.ok) {
                payload["type"] = "image";
                payload["dataUri"] = std::move(snap.dataUri);
            } else {
                payload["designFallback"] = true;
            }
        }
    } else if (type == "psd") {
        auto psd = tools3000::core::DesignAndCadPreview::extractPsdPreview(filePath, 2560, true);
        if (!psd.metaSummary.empty()) {
            payload["designMeta"] = psd.metaSummary;
        }
        if (psd.ok) {
            payload["type"] = "image";
            payload["dataUri"] = std::move(psd.dataUri);
        } else {
            payload["designFallback"] = true;
        }
    } else if (type == "cdr") {
        auto cdr = tools3000::core::DesignAndCadPreview::extractCdrPreview(filePath, true);
        if (!cdr.metaSummary.empty()) {
            payload["designMeta"] = cdr.metaSummary;
        }
        if (cdr.ok) {
            payload["type"] = "image";
            payload["dataUri"] = std::move(cdr.dataUri);
            if (cdr.pageDataUris.size() > 1) {
                payload["imagePages"] = std::move(cdr.pageDataUris);
            }
        } else {
            payload["designFallback"] = true;
        }
    } else if (type == "cad_snapshot") {
        auto snap = tools3000::core::DesignAndCadPreview::extractCadOr3dSnapshotPreview(filePath, ext, true);
        if (!snap.metaSummary.empty()) {
            payload["designMeta"] = snap.metaSummary;
        }
        if (snap.ok) {
            payload["type"] = "image";
            payload["dataUri"] = std::move(snap.dataUri);
            payload["previewNoteKind"] = "cad_snapshot";
        } else {
            payload["designFallback"] = true;
        }
    } else if (type == "cad_dxf") {
        auto dxf = tools3000::core::DesignAndCadPreview::extractDxfVectorPreview(filePath, 30000);
        if (dxf.ok) {
            payload["encoding"] = dxf.encoding;
            payload["truncated"] = dxf.truncated;

            json layersJson = json::array();
            for (const auto& ly : dxf.layers) {
                layersJson.push_back({
                    {"name", ly.name},
                    {"colorHex", ly.colorHex},
                    {"adaptive", ly.adaptiveDefaultColor},
                    {"count", ly.entityCount}
                });
            }

            json entsJson = json::array();
            for (const auto& ent : dxf.entities) {
                json coordsJson = json::array();
                for (double c : ent.coords) {
                    coordsJson.push_back(roundCoordForJson(c));
                }
                json item = {
                    {"k", static_cast<int>(ent.kind)},
                    {"l", ent.layer},
                    {"c", ent.colorHex},
                    {"a", ent.adaptiveDefaultColor},
                    {"pts", std::move(coordsJson)}
                };
                if (ent.closed) item["cl"] = true;
                if (ent.kind == tools3000::core::DxfEntityKind::Circle ||
                    ent.kind == tools3000::core::DxfEntityKind::Arc) {
                    item["r"] = roundCoordForJson(ent.radius);
                }
                if (ent.kind == tools3000::core::DxfEntityKind::Arc) {
                    item["sa"] = roundCoordForJson(ent.startAngleDeg);
                    item["ea"] = roundCoordForJson(ent.endAngleDeg);
                }
                if (ent.kind == tools3000::core::DxfEntityKind::Text) {
                    item["th"] = roundCoordForJson(ent.textHeight);
                    if (std::abs(ent.rotationDeg) > 1e-3) {
                        item["rot"] = roundCoordForJson(ent.rotationDeg);
                    }
                    item["t"] = ent.text;
                }
                entsJson.push_back(std::move(item));
            }

            payload["dxfData"] = {
                {"totalEntities", dxf.totalEntities},
                {"bounds", {
                    roundCoordForJson(dxf.bounds[0]),
                    roundCoordForJson(dxf.bounds[1]),
                    roundCoordForJson(dxf.bounds[2]),
                    roundCoordForJson(dxf.bounds[3])
                }},
                {"layers", std::move(layersJson)},
                {"entities", std::move(entsJson)}
            };
        } else {
            payload["designFallback"] = true;
        }
    } else if (type == "model_3d") {
        auto mesh = tools3000::core::DesignAndCadPreview::extract3dMeshPreview(filePath, ext, 50000);
        if (mesh.ok) {
            payload["truncated"] = mesh.truncated;
            payload["designMeta"] = mesh.formatLabel;
            payload["meshData"] = {
                {"formatLabel", mesh.formatLabel},
                {"totalTriangles", mesh.totalTriangles},
                {"sampledTriangles", mesh.sampledTriangles},
                {"dimensions", {
                    roundCoordForJson(mesh.dimensions[0]),
                    roundCoordForJson(mesh.dimensions[1]),
                    roundCoordForJson(mesh.dimensions[2])
                }},
                {"positionsBase64", encodeFloat32BufferBase64(mesh.positions)}
            };
        } else {
            payload["designFallback"] = true;
        }
    } else if (type == "image" && isExtendedWicImageExt(ext)) {
        auto snap = tools3000::core::DesignAndCadPreview::extractCadOr3dSnapshotPreview(filePath, ext, true);
        if (!snap.metaSummary.empty()) {
            payload["designMeta"] = snap.metaSummary;
        }
        if (snap.ok) {
            payload["dataUri"] = std::move(snap.dataUri);
        } else {
            payload["imageFallbackFailed"] = true;
        }
    } else if (type == "video" && isVideoNeedsProbeFallbackImmediately(ext)) {
        payload["videoNeedsProbe"] = true;
    } else if (type == "archive") {
        auto arch = tools3000::core::ArchivePreview::inspectArchive(filePath, 2000);
        json entriesJson = json::array();
        for (const auto& item : arch.entries) {
            entriesJson.push_back({
                {"path", item.path},
                {"isDirectory", item.isDirectory},
                {"size", item.uncompressedSize},
                {"formattedSize", item.isDirectory ? "-" : formatFileSize(item.uncompressedSize)},
                {"compressedSize", item.compressedSize},
                {"encrypted", item.encrypted},
                {"modified", item.modified}
            });
        }
        payload["archiveData"] = {
            {"ok", arch.ok},
            {"format", arch.format},
            {"detailNote", arch.detailNote},
            {"encrypted", arch.encrypted},
            {"truncated", arch.truncated},
            {"totalEntries", arch.totalEntries},
            {"totalFiles", arch.totalFiles},
            {"totalFolders", arch.totalFolders},
            {"totalUncompressedBytes", arch.totalUncompressedBytes},
            {"formattedUncompressedSize", formatFileSize(arch.totalUncompressedBytes)},
            {"entries", std::move(entriesJson)}
        };
    } else if (type == "font") {
        auto font = tools3000::core::FontPreview::inspectFont(filePath);
        json axesJson = json::array();
        for (const auto& ax : font.axes) {
            axesJson.push_back({
                {"tag", ax.tag},
                {"min", ax.minValue},
                {"default", ax.defaultValue},
                {"max", ax.maxValue}
            });
        }
        payload["fontData"] = {
            {"ok", font.ok},
            {"format", font.format},
            {"familyName", font.familyName},
            {"subfamilyName", font.subfamilyName},
            {"fullName", font.fullName},
            {"version", font.version},
            {"copyright", font.copyright},
            {"designer", font.designer},
            {"glyphCount", font.glyphCount},
            {"weightClass", font.weightClass},
            {"isItalic", font.isItalic},
            {"isVariable", font.isVariable},
            {"hasCjkSupport", font.hasCjkSupport},
            {"faceCount", font.faceCount},
            {"faceNames", font.faceNames},
            {"axes", std::move(axesJson)}
        };
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
        m_hasAsyncFallbackTask = false;
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

        std::wstring fallbackPath;
        std::string fallbackKind;
        uint64_t fallbackReqId = 0;
        uint64_t fallbackGen = 0;
        bool runFallback = false;

        std::wstring stepBase;
        int stepDelta = 0;
        uint64_t stepGen = 0;
        bool runStep = false;

        {
            std::unique_lock lock(m_asyncMutex);
            m_asyncCv.wait(lock, st, [this]() {
                return m_hasAsyncStepTask || m_hasAsyncPreviewTask || m_hasAsyncFallbackTask;
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
            } else if (m_hasAsyncFallbackTask) {
                fallbackPath = std::move(m_asyncFallbackPath);
                fallbackKind = std::move(m_asyncFallbackKind);
                fallbackReqId = m_asyncFallbackReqId;
                fallbackGen = m_asyncFallbackGen;
                m_hasAsyncFallbackTask = false;
                runFallback = true;
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

        if (runFallback) {
            json patch = {
                {"path", tools3000::core::WinUtils::wstringToUtf8(fallbackPath)},
                {"kind", fallbackKind}
            };
            if (fallbackKind == "image") {
                const std::string ext = extractLowerExtUtf8(std::filesystem::path(fallbackPath), false);
                auto snap = tools3000::core::DesignAndCadPreview::extractCadOr3dSnapshotPreview(fallbackPath, ext, true);
                if (snap.ok && !snap.dataUri.empty()) {
                    patch["ok"] = true;
                    patch["dataUri"] = std::move(snap.dataUri);
                    if (!snap.metaSummary.empty()) patch["designMeta"] = std::move(snap.metaSummary);
                } else {
                    patch["ok"] = false;
                }
            } else if (fallbackKind == "video") {
                json probe = runIsolatedMediaProbeWorker(fallbackPath, st);
                if (!st.stop_requested() && probe.value("ok", false)) {
                    patch["ok"] = true;
                    patch["videoProbe"] = std::move(probe);
                } else {
                    patch["ok"] = false;
                }
            }
            if (!st.stop_requested() && fallbackGen == m_generation.load() &&
                fallbackReqId == m_previewRequestId.load()) {
                const std::string dumped = patch.dump(-1, ' ', false, json::error_handler_t::replace);
                tools3000::core::MainThreadDispatcher::instance().postDeferred(
                    [this, fallbackPath, fallbackReqId, fallbackGen, dumped]() {
                        if (fallbackGen != m_generation.load() ||
                            fallbackReqId != m_previewRequestId.load() ||
                            !m_visible.load() || !m_webView || !m_webViewReady) {
                            return;
                        }
                        auto parsed = json::parse(dumped, nullptr, false);
                        if (parsed.is_object() && parsed.contains("dataUri") && parsed["dataUri"].is_string()) {
                            updateCachedPinDataUri(fallbackPath, parsed["dataUri"].get<std::string>());
                        }
                        const std::string env =
                            R"({"type":"event","event":"quicklook.mediaFallbackReady","data":)" + dumped + "}";
                        const std::wstring wMsg = tools3000::core::WinUtils::utf8ToWstring(env);
                        m_webView->PostWebMessageAsString(wMsg.c_str());
                    });
            }
            continue;
        }

        if (runPreview) {
            if (deepDecode) {
                json fullPayload = generateFilePreviewPayload(previewPath);
                if (!st.stop_requested() && fullPayload.value("videoNeedsProbe", false)) {
                    json probe = runIsolatedMediaProbeWorker(previewPath, st);
                    if (probe.value("ok", false)) {
                        fullPayload["videoProbe"] = std::move(probe);
                    }
                    fullPayload["videoFallbackOnly"] = true;
                }
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
                        if (hasDataUri && payload["dataUri"].is_string()) {
                            updateCachedPinDataUri(previewPath, payload["dataUri"].get<std::string>());
                        } else {
                            clearCachedPinDataUri();
                        }
                        m_currentIsPdfPreview.store(exists && fileType == "pdf", std::memory_order_release);
                        const bool pdfNeedsIframeFallback =
                            (fileType == "pdf") &&
                            !payload.contains("pdfPages") &&
                            !payload.value("pdfPasswordProtected", false);
                        const bool needsMediaStream =
                            exists &&
                            (fileType == "video" || fileType == "audio" || fileType == "font" ||
                             pdfNeedsIframeFallback ||
                             (fileType == "image" && !hasDataUri));
                        if (needsMediaStream) {
                            payload["mediaUrl"] = bindActiveMediaUrl(previewPath, sz, fileType == "font" ? 0 : -1);
                        } else {
                            clearActiveMediaBinding();
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
        KillTimer(m_hwnd, IDT_IDLE_DESTROY);
        KillTimer(m_hwnd, IDT_DEFERRED_SUSPEND);
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

        if (m_webView) {
            m_suspendController.resume(m_webView.Get(), "quicklook");
        }
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

std::string QuickLookWindow::bindActiveMediaUrl(const std::wstring& filePath, uintmax_t sizeBytes, int faceIndex) {
    const uint64_t rev = ++m_mediaUrlRevision;
    const std::string token = std::to_string(m_generation.load()) + "_" + std::to_string(rev);
    {
        std::lock_guard lock(m_mediaBindingMutex);
        m_activeMediaPath = filePath;
        m_activeMediaToken = token;
    }
    std::string url = "https://quicklook-media.tools3000.local/item?t=" + token +
                      "&s=" + std::to_string(sizeBytes);
    if (faceIndex >= 0) {
        url += "&f=" + std::to_string(faceIndex);
    }
    return url;
}

void QuickLookWindow::clearActiveMediaBinding() {
    std::lock_guard lock(m_mediaBindingMutex);
    m_activeMediaPath.clear();
    m_activeMediaToken.clear();
}

void QuickLookWindow::handleMediaResourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (!args || !m_environment) return;

    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || !request) return;

    LPWSTR uriRaw = nullptr;
    if (FAILED(request->get_Uri(&uriRaw)) || !uriRaw) return;
    const std::wstring uri(uriRaw);
    CoTaskMemFree(uriRaw);

    LPWSTR methodRaw = nullptr;
    std::wstring method = L"GET";
    if (SUCCEEDED(request->get_Method(&methodRaw)) && methodRaw) {
        method = methodRaw;
        CoTaskMemFree(methodRaw);
    }

    auto setSimpleResponse = [&](int status, const wchar_t* reason, const std::wstring& headers, IStream* body) {
        ComPtr<ICoreWebView2WebResourceResponse> resp;
        if (SUCCEEDED(m_environment->CreateWebResourceResponse(body, status, reason, headers.c_str(), &resp)) && resp) {
            args->put_Response(resp.Get());
        }
    };

    const std::wstring corsHeaders =
        L"Access-Control-Allow-Origin: https://tools3000.local\r\n"
        L"Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
        L"Access-Control-Allow-Headers: Range\r\n"
        L"Access-Control-Expose-Headers: Content-Length, Content-Range, Accept-Ranges\r\n"
        L"Cache-Control: no-store\r\n";

    if (_wcsicmp(method.c_str(), L"OPTIONS") == 0) {
        setSimpleResponse(204, L"No Content", corsHeaders, nullptr);
        return;
    }

    const std::string reqToken = queryParamValueFromUri(uri, L"t=");
    std::wstring boundPath;
    {
        std::lock_guard lock(m_mediaBindingMutex);
        if (!reqToken.empty() && reqToken == m_activeMediaToken) {
            boundPath = m_activeMediaPath;
        }
    }
    if (boundPath.empty()) {
        setSimpleResponse(403, L"Forbidden", corsHeaders, nullptr);
        return;
    }

    const std::filesystem::path fsPath(boundPath);
    const std::wstring extLower = tools3000::core::WinUtils::toLower(fsPath.extension().wstring());

    // .ttc 字体集合：内存中无损抽离指定子字面为独立 .ttf 流供 FontFace 渲染
    if (extLower == L".ttc") {
        uint32_t faceIdx = 0;
        const std::string fVal = queryParamValueFromUri(uri, L"f=");
        if (!fVal.empty()) {
            try {
                faceIdx = static_cast<uint32_t>(std::stoul(fVal));
            } catch (...) {
                faceIdx = 0;
            }
        }
        std::vector<uint8_t> ttfBytes;
        if (tools3000::core::FontPreview::extractStandaloneTtfFromTtc(boundPath, faceIdx, ttfBytes) && !ttfBytes.empty()) {
            const size_t sz = ttfBytes.size();
            ComPtr<IStream> stream;
            stream.Attach(new MemoryBufferIStream(std::move(ttfBytes)));
            const std::wstring headers = corsHeaders +
                L"Content-Type: font/ttf\r\n"
                L"Content-Length: " + std::to_wstring(sz) + L"\r\n";
            setSimpleResponse(200, L"OK", headers, stream.Get());
            return;
        }
        setSimpleResponse(404, L"Not Found", corsHeaders, nullptr);
        return;
    }

    HANDLE hFile = CreateFileW(
        boundPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        setSimpleResponse(404, L"Not Found", corsHeaders, nullptr);
        return;
    }

    LARGE_INTEGER fileSizeLi{};
    if (!GetFileSizeEx(hFile, &fileSizeLi) || fileSizeLi.QuadPart < 0) {
        CloseHandle(hFile);
        setSimpleResponse(500, L"Internal Server Error", corsHeaders, nullptr);
        return;
    }
    const uint64_t totalSize = static_cast<uint64_t>(fileSizeLi.QuadPart);
    const std::wstring mime = mimeTypeForPreviewExtension(extLower);

    std::wstring rangeHeader;
    ComPtr<ICoreWebView2HttpRequestHeaders> reqHeaders;
    if (SUCCEEDED(request->get_Headers(&reqHeaders)) && reqHeaders) {
        LPWSTR rangeRaw = nullptr;
        if (SUCCEEDED(reqHeaders->GetHeader(L"Range", &rangeRaw)) && rangeRaw) {
            rangeHeader = rangeRaw;
            CoTaskMemFree(rangeRaw);
        }
    }

    uint64_t rangeStart = 0;
    uint64_t rangeEnd = (totalSize > 0) ? (totalSize - 1) : 0;
    if (!rangeHeader.empty() && totalSize > 0) {
        if (!parseHttpByteRangeHeader(rangeHeader, totalSize, rangeStart, rangeEnd)) {
            CloseHandle(hFile);
            const std::wstring hdrs = corsHeaders +
                L"Content-Range: bytes */" + std::to_wstring(totalSize) + L"\r\n";
            setSimpleResponse(416, L"Range Not Satisfiable", hdrs, nullptr);
            return;
        }
        const uint64_t sliceLen = (rangeEnd - rangeStart) + 1;
        ComPtr<IStream> stream;
        if (_wcsicmp(method.c_str(), L"HEAD") == 0) {
            CloseHandle(hFile);
        } else {
            stream.Attach(new BoundedFileIStream(hFile, rangeStart, sliceLen));
        }
        const std::wstring hdrs = corsHeaders +
            L"Content-Type: " + mime + L"\r\n"
            L"Accept-Ranges: bytes\r\n"
            L"Content-Range: bytes " + std::to_wstring(rangeStart) + L"-" +
                std::to_wstring(rangeEnd) + L"/" + std::to_wstring(totalSize) + L"\r\n"
            L"Content-Length: " + std::to_wstring(sliceLen) + L"\r\n";
        setSimpleResponse(206, L"Partial Content", hdrs, stream.Get());
        return;
    }

    ComPtr<IStream> stream;
    if (_wcsicmp(method.c_str(), L"HEAD") == 0) {
        CloseHandle(hFile);
    } else {
        stream.Attach(new BoundedFileIStream(hFile, 0, totalSize));
    }
    const std::wstring hdrs = corsHeaders +
        L"Content-Type: " + mime + L"\r\n"
        L"Accept-Ranges: bytes\r\n"
        L"Content-Length: " + std::to_wstring(totalSize) + L"\r\n";
    setSimpleResponse(200, L"OK", hdrs, stream.Get());
}

void QuickLookWindow::notifyFrontendReady() {
    m_frontendReady = true;
    if (m_pendingFilePath.empty() || !m_visible.load()) return;
    // 防冷启动双重中断：若同一 generation 下同一路径已推送，跳过重复推送以免打断视频播放
    if (m_lastDispatchedGen == m_generation.load() &&
        _wcsicmp(m_lastDispatchedPath.c_str(), m_pendingFilePath.c_str()) == 0) {
        return;
    }
    previewFile(m_pendingFilePath);
}

void QuickLookWindow::requestFallbackMediaAsync(const std::wstring& filePath, const std::string& mediaKind) {
    const std::wstring normalizedPath = normalizePreviewFilePath(filePath);
    if (normalizedPath.empty() || !m_visible.load()) return;
    if (_wcsicmp(normalizedPath.c_str(), m_currentFilePath.c_str()) != 0) return;

    ensureAsyncWorkerStarted();
    {
        std::lock_guard lock(m_asyncMutex);
        m_asyncFallbackPath = normalizedPath;
        m_asyncFallbackKind = mediaKind;
        m_asyncFallbackReqId = m_previewRequestId.load();
        m_asyncFallbackGen = m_generation.load();
        m_hasAsyncFallbackTask = true;
    }
    m_asyncCv.notify_one();
}

void QuickLookWindow::previewFile(const std::wstring& filePath) {
    const std::wstring normalizedPath = normalizePreviewFilePath(filePath);
    m_currentFilePath = normalizedPath;
    m_pendingFilePath = normalizedPath;
    m_showTimeTick = GetTickCount64();
    m_visible = true;
    const uint64_t requestId = ++m_previewRequestId;
    const uint64_t generation = m_generation.load();

    if (m_webView && m_webViewReady && m_frontendReady) {
        m_lastDispatchedPath = normalizedPath;
        m_lastDispatchedGen = generation;
        bool needsAsyncDeepLoad = false;
        json payload = generateFastInitialPreviewPayload(normalizedPath, needsAsyncDeepLoad);
        const std::string fileType = payload.value("type", "");
        const bool exists = payload.value("exists", false);
        m_currentIsPdfPreview.store(exists && fileType == "pdf", std::memory_order_release);
        const bool needsMediaStream =
            exists &&
            ((fileType == "video" && !needsAsyncDeepLoad) ||
             fileType == "audio" ||
             (fileType == "image" && !needsAsyncDeepLoad && !payload.contains("dataUri")));
        if (needsMediaStream) {
            const uintmax_t sz = payload.value("size", static_cast<uintmax_t>(0));
            payload["mediaUrl"] = bindActiveMediaUrl(normalizedPath, sz, -1);
        } else {
            clearActiveMediaBinding();
        }
        // 首帧 0ms 立即推送当前选中文件元数据或流媒体地址，杜绝磁盘 I/O 阻塞主 UI 线程
        std::string envelope = R"({"type":"event","event":"quicklook.fileChanged","data":)" +
                               payload.dump(-1, ' ', false, json::error_handler_t::replace) + "}";
        std::wstring wMsg = tools3000::core::WinUtils::utf8ToWstring(envelope);
        m_webView->PostWebMessageAsString(wMsg.c_str());

        if (exists && needsAsyncDeepLoad) {
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
    m_lastDispatchedPath.clear();
    clearCachedPinDataUri();
    m_currentIsPdfPreview.store(false, std::memory_order_release);
    if (m_webView && m_webViewReady) {
        m_webView->PostWebMessageAsJson(
            LR"({"type":"event","event":"quicklook.fileChanged","data":null})");
    }
    if (m_hwnd) {
        clearActiveMediaBinding();
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
        // 延迟 15 秒再冻结 WebView2（保护用户连续按空格预览多个文件的热态瞬开，避免同步 trimWorkingSet 引发软缺页微卡顿）；
        // 90 秒彻底销毁 WebView2 与异步工作线程释放全部内存。
        SetTimer(m_hwnd, IDT_DEFERRED_SUSPEND, DEFERRED_SUSPEND_DELAY_MS, nullptr);
        SetTimer(m_hwnd, IDT_IDLE_DESTROY, IDLE_DESTROY_TIMEOUT_MS, nullptr);

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
    m_suspendController.abandon();
    ++m_generation;
    ++m_previewRequestId;
    shutdownAsyncWorker();
    m_currentFilePath.clear();
    m_pendingFilePath.clear();
    m_lastDispatchedPath.clear();
    clearCachedPinDataUri();
    m_currentIsPdfPreview.store(false, std::memory_order_release);
    clearActiveMediaBinding();
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, IDT_IDLE_DESTROY);
        KillTimer(m_hwnd, IDT_DEFERRED_SUSPEND);
    }
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
    m_frontendReady = false;
    tools3000::core::WinUtils::trimWorkingSet();
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
    m_suspendController.reset();
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
    tools3000::core::WinUtils::applyWin11SystemBackdrop(m_hwnd, DWMSBT_MAINWINDOW);
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

                        // 在 Navigate() 之前注册流式媒体/字体拦截器：彻底替代动态文件夹映射，
                        // 原生支持 HTTP Range/206、CORS 字体加载、UNC/超长路径、8 语种特殊符号文件名，且仅限当前单文件访问。
                        m_webView->AddWebResourceRequestedFilter(
                            L"https://quicklook-media.tools3000.local/*",
                            COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                        m_webView->add_WebResourceRequested(
                            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                [this, generation](ICoreWebView2*,
                                                   ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                    if (generation != m_generation.load()) return S_OK;
                                    handleMediaResourceRequested(args);
                                    return S_OK;
                                }).Get(),
                            nullptr);

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
                                            std::wstring tempPinPath;
                                            if (parsed.is_object() && parsed.value("method", "") == "capture.pinImageFile" &&
                                                parsed.contains("params") && parsed["params"].is_object()) {
                                                const std::string reqPathUtf8 = parsed["params"].value("path", "");
                                                const std::wstring reqPathWide = normalizePreviewFilePath(
                                                    tools3000::core::WinUtils::utf8ToWstring(reqPathUtf8));
                                                std::string cachedUri;
                                                {
                                                    std::lock_guard lock(g_cachedPinMutex);
                                                    if (!g_cachedPinDataUri.empty() &&
                                                        _wcsicmp(reqPathWide.c_str(), g_cachedPinSourcePath.c_str()) == 0) {
                                                        cachedUri = g_cachedPinDataUri;
                                                    }
                                                }
                                                if (!cachedUri.empty() && writeDataUriToTempImageFile(cachedUri, tempPinPath)) {
                                                    parsed["params"]["path"] = tools3000::core::WinUtils::wstringToUtf8(tempPinPath);
                                                    message = parsed.dump();
                                                }
                                            }
                                            std::string response = tools3000::core::MessageBridge::instance().handleMessage(message);
                                            if (!tempPinPath.empty()) {
                                                DeleteFileW(tempPinPath.c_str());
                                            }
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
                                [this, generation](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                    if (generation != m_generation.load()) return S_OK;
                                    BOOL success;
                                    args->get_IsSuccess(&success);
                                    if (success) {
                                        m_webViewReady = true;
                                        if (!m_visible.load()) {
                                            if (m_controller) m_controller->put_IsVisible(FALSE);
                                            if (m_webView) m_suspendController.requestSuspend(m_webView.Get(), "quicklook");
                                        } else if (m_frontendReady && !m_pendingFilePath.empty()) {
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
    LRESULT ncResult = 0;
    if (tools3000::core::WinUtils::handleFramelessNonClientMessage(hwnd, uMsg, wParam, lParam, ncResult)) {
        return ncResult;
    }

    switch (uMsg) {
        case WM_TIMER: {
            if (wParam == IDT_DEFERRED_SUSPEND) {
                KillTimer(hwnd, IDT_DEFERRED_SUSPEND);
                if (self && !self->isVisible()) {
                    if (self->m_controller) self->m_controller->put_IsVisible(FALSE);
                    if (self->m_webView) self->m_suspendController.requestSuspend(self->m_webView.Get(), "quicklook");
                    tools3000::core::WinUtils::scheduleQuietIdleTrim(2500);
                }
                return 0;
            }
            if (wParam == IDT_IDLE_DESTROY) {
                KillTimer(hwnd, IDT_IDLE_DESTROY);
                KillTimer(hwnd, IDT_DEFERRED_SUSPEND);
                if (self && !self->isVisible()) {
                    LOG_INFO("QuickLookWindow: idle 90s, destroying WebView2 and releasing memory");
                    self->destroy();
                }
                return 0;
            }
            break;
        }
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
        case WM_NCCALCSIZE:
            return 0;
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
                    const DWM_WINDOW_CORNER_PREFERENCE noCorners = DWMWCP_DONOTROUND;
                    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &noCorners, sizeof(noCorners));
                    tools3000::core::WinUtils::clearWindowRegionIfSet(hwnd, false);
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

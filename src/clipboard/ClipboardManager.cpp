// ─────────────────────────────────────────────────────────────────────────────
// ClipboardManager.cpp — 原生 Win32 事件驱动剪贴板监听与 Rust 持久化内核集成
// ─────────────────────────────────────────────────────────────────────────────

#include "clipboard/ClipboardManager.h"
#include "clipboard/ClipboardRustFfi.h"

#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/ipc/MessageBridge.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace tools3000::clipboard {

namespace {

constexpr const wchar_t* kListenerWndClass = L"Tools3000_ClipboardListenerMsgOnly";
constexpr size_t kMaxPendingQueueItems = 16;

std::string toLowerAscii(std::string_view sv) {
    std::string out(sv);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string trimAscii(std::string_view sv) {
    size_t s = 0;
    while (s < sv.size() && std::isspace(static_cast<unsigned char>(sv[s]))) ++s;
    size_t e = sv.size();
    while (e > s && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(s, e - s));
}

std::string queryProcessExeNameFromHwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t pathBuf[MAX_PATH * 2]{};
    DWORD len = static_cast<DWORD>(std::size(pathBuf));
    std::string exeName;
    if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &len) && len > 0) {
        const std::filesystem::path p(std::wstring(pathBuf, len));
        exeName = tools3000::core::WinUtils::wstringToUtf8(p.filename().wstring());
    }
    CloseHandle(hProc);
    return exeName;
}

std::string consumeRustJsonString(char* raw) {
    if (!raw) return {};
    std::string s(raw);
    t3k_clip_free_string(raw);
    return s;
}

std::filesystem::path resolveDefaultClipboardDataDir() {
    auto dir = tools3000::core::WinUtils::getAppDataDirectory() / L"clipboard";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

/// 将 CF_DIB / CF_DIBV5 字节块通过 WIC 无损编码为 PNG 并返回宽高（运行于后台工作线程）
bool encodeDibToPngBytes(
    const std::vector<uint8_t>& dibBytes,
    std::vector<uint8_t>& outPng,
    uint32_t& outWidth,
    uint32_t& outHeight) {
    outPng.clear();
    outWidth = 0;
    outHeight = 0;
    if (dibBytes.size() < sizeof(BITMAPINFOHEADER)) return false;

    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dibBytes.data());
    if (bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biSize > dibBytes.size()) return false;

    // 组装标准内存 BMP (BITMAPFILEHEADER + DIB) 交给 WIC 解码器，完整兼容 BI_RGB / BI_BITFIELDS / 32bpp Alpha / 24bpp / 8bpp 调色板
    DWORD colorTableBytes = 0;
    if (bih->biBitCount <= 8) {
        const DWORD colors = (bih->biClrUsed != 0) ? bih->biClrUsed : (1u << bih->biBitCount);
        colorTableBytes = colors * sizeof(RGBQUAD);
    } else if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) {
        colorTableBytes = 3 * sizeof(DWORD);
    }

    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42; // "BM"
    bfh.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + bih->biSize + colorTableBytes);
    bfh.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + dibBytes.size());
    if (bfh.bfOffBits > bfh.bfSize) {
        bfh.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + bih->biSize);
    }

    std::vector<uint8_t> bmpBuf;
    bmpBuf.resize(sizeof(BITMAPFILEHEADER) + dibBytes.size());
    std::memcpy(bmpBuf.data(), &bfh, sizeof(BITMAPFILEHEADER));
    std::memcpy(bmpBuf.data() + sizeof(BITMAPFILEHEADER), dibBytes.data(), dibBytes.size());

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf())))) {
        return false;
    }

    ComPtr<IWICStream> inStream;
    if (FAILED(factory->CreateStream(inStream.GetAddressOf()))) return false;
    if (FAILED(inStream->InitializeFromMemory(bmpBuf.data(), static_cast<DWORD>(bmpBuf.size())))) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(
            inStream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;
    if (FAILED(frame->GetSize(&outWidth, &outHeight)) || outWidth == 0 || outHeight == 0) return false;

    ComPtr<IStream> memOut;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, memOut.GetAddressOf()))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()))) return false;
    if (FAILED(encoder->Initialize(memOut.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frameEnc;
    if (FAILED(encoder->CreateNewFrame(frameEnc.GetAddressOf(), nullptr))) return false;
    if (FAILED(frameEnc->Initialize(nullptr))) return false;
    if (FAILED(frameEnc->WriteSource(frame.Get(), nullptr))) return false;
    if (FAILED(frameEnc->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;

    HGLOBAL hMem = nullptr;
    if (FAILED(GetHGlobalFromStream(memOut.Get(), &hMem)) || !hMem) return false;
    const SIZE_T sz = GlobalSize(hMem);
    if (sz == 0) return false;
    const void* locked = GlobalLock(hMem);
    if (!locked) return false;
    outPng.resize(sz);
    std::memcpy(outPng.data(), locked, sz);
    GlobalUnlock(hMem);
    return !outPng.empty();
}

/// 读取 PNG 尺寸（验证 PNG 有效性并提取宽高）
bool probePngDimensions(const std::vector<uint8_t>& pngBytes, uint32_t& outW, uint32_t& outH) noexcept {
    outW = 0;
    outH = 0;
    static constexpr uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (pngBytes.size() < 24 || std::memcmp(pngBytes.data(), kPngSig, 8) != 0) return false;
    if (std::memcmp(pngBytes.data() + 12, "IHDR", 4) != 0) return false;
    auto be32 = [&](size_t off) -> uint32_t {
        return (static_cast<uint32_t>(pngBytes[off]) << 24) |
               (static_cast<uint32_t>(pngBytes[off + 1]) << 16) |
               (static_cast<uint32_t>(pngBytes[off + 2]) << 8) |
               static_cast<uint32_t>(pngBytes[off + 3]);
    };
    outW = be32(16);
    outH = be32(20);
    return outW > 0 && outH > 0;
}

/// 使用 WIC 将本地 PNG 文件解码为 32bpp BGRA DIB 字节块，供无损写回剪贴板 CF_DIB
bool decodePngFileToDibBytes(
    const std::filesystem::path& pngPath,
    std::vector<uint8_t>& outDib,
    std::vector<uint8_t>& outRawPng) {
    outDib.clear();
    outRawPng.clear();

    std::ifstream ifs(pngPath, std::ios::binary);
    if (!ifs) return false;
    outRawPng.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    if (outRawPng.empty()) return false;

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwned = SUCCEEDED(comHr);
    struct ComUninitGuard {
        bool owned = false;
        ~ComUninitGuard() { if (owned) CoUninitialize(); }
    } comGuard{comOwned};

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf())))) {
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.GetAddressOf()))) return false;
    if (FAILED(stream->InitializeFromMemory(outRawPng.data(), static_cast<DWORD>(outRawPng.size())))) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(conv.GetAddressOf()))) return false;
    if (FAILED(conv->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const size_t stride = static_cast<size_t>(w) * 4u;
    const size_t pixelBytes = stride * static_cast<size_t>(h);
    std::vector<uint8_t> topDown(pixelBytes);
    if (FAILED(conv->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(pixelBytes), topDown.data()))) {
        return false;
    }

    // CF_DIB 兼容标准正高度 (bottom-up) 像素排列，通吃所有 Win32/Office/微信应用
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = static_cast<LONG>(w);
    bih.biHeight = static_cast<LONG>(h);
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = static_cast<DWORD>(pixelBytes);

    outDib.resize(sizeof(BITMAPINFOHEADER) + pixelBytes);
    std::memcpy(outDib.data(), &bih, sizeof(BITMAPINFOHEADER));
    uint8_t* dst = outDib.data() + sizeof(BITMAPINFOHEADER);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* srcRow = topDown.data() + static_cast<size_t>(h - 1 - y) * stride;
        std::memcpy(dst + static_cast<size_t>(y) * stride, srcRow, stride);
    }
    return true;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ClipboardManager 实现
// ─────────────────────────────────────────────────────────────────────────────

ClipboardManager& ClipboardManager::instance() {
    static ClipboardManager inst;
    return inst;
}

ClipboardManager::~ClipboardManager() {
    shutdown();
}

bool ClipboardManager::isExcludedProcessName(std::string_view processExeName, std::string_view excludedAppsCsv) {
    const std::string procLow = toLowerAscii(trimAscii(processExeName));
    if (procLow.empty()) return false;

    size_t pos = 0;
    while (pos <= excludedAppsCsv.size()) {
        const size_t sep = excludedAppsCsv.find_first_of(",;\n\r", pos);
        const std::string_view token = (sep == std::string_view::npos)
            ? excludedAppsCsv.substr(pos)
            : excludedAppsCsv.substr(pos, sep - pos);
        const std::string pat = toLowerAscii(trimAscii(token));
        if (!pat.empty()) {
            if (procLow == pat) return true;
            // 支持不写 .exe 后缀（如 "Bitwarden" 匹配 "bitwarden.exe"）
            if (pat.find('.') == std::string::npos && procLow == (pat + ".exe")) return true;
        }
        if (sep == std::string_view::npos) break;
        pos = sep + 1;
    }
    return false;
}

std::string ClipboardManager::fnv1a128Hex(const uint8_t* data, size_t len) {
    // 使用双 64 位独立盐值 FNV-1a 生成 128-bit 高抗碰撞指纹（无依赖、极速且跨 x64/ARM64 一致）
    uint64_t h1 = 0xcbf29ce484222325ULL;
    uint64_t h2 = 0x84222325cbf29ce4ULL ^ static_cast<uint64_t>(len);
    for (size_t i = 0; i < len; ++i) {
        const uint64_t b = data[i];
        h1 ^= b;
        h1 *= 0x100000001b3ULL;
        h2 ^= (b + static_cast<uint64_t>(i & 0xFFu));
        h2 *= 0x100000001b3ULL;
    }
    char buf[33]{};
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(h1),
                  static_cast<unsigned long long>(h2));
    return std::string(buf);
}

bool ClipboardManager::initialize(HINSTANCE hInstance, const std::filesystem::path& customDataDir) {
    if (m_initialized.load(std::memory_order_acquire)) {
        return true;
    }

    // 1. 加载单一事实源配置
    auto& cfg = tools3000::core::ConfigManager::instance();
    ClipboardSettings loaded{};
    loaded.enabled = cfg.has("/plugins/clipboard/enabled")
        ? cfg.get<bool>("/plugins/clipboard/enabled", true)
        : cfg.get<bool>("/clipboard/enabled", true);
    loaded.hotkey = cfg.get<std::string>("/hotkeys/Toggle Clipboard", "Alt+V");
    loaded.maxItems = std::clamp(cfg.get<int>("/clipboard/maxItems", 500), 20, 5000);
    loaded.maxDays = std::clamp(cfg.get<int>("/clipboard/maxDays", 30), 0, 3650);
    loaded.maxImageMb = std::clamp(cfg.get<int>("/clipboard/maxImageMb", 16), 1, 64);
    loaded.autoPaste = cfg.get<bool>("/clipboard/autoPaste", true);
    loaded.plainTextByDefault = cfg.get<bool>("/clipboard/plainTextByDefault", false);
    loaded.captureImages = cfg.get<bool>("/clipboard/captureImages", true);
    loaded.captureFiles = cfg.get<bool>("/clipboard/captureFiles", true);
    loaded.excludedApps = cfg.get<std::string>(
        "/clipboard/excludedApps", loaded.excludedApps);

    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        m_settings = loaded;
        m_dataDir = customDataDir.empty() ? resolveDefaultClipboardDataDir() : customDataDir;
        m_blobsDir = m_dataDir / L"blobs";
    }

    std::error_code ec;
    std::filesystem::create_directories(m_blobsDir, ec);

    // 2. 启动 Rust 持久化存储内核
    const std::string dataDirUtf8 = tools3000::core::WinUtils::wstringToUtf8(m_dataDir.wstring());
    if (t3k_clip_init(dataDirUtf8.c_str()) == 0) {
        LOG_ERROR("ClipboardManager: Rust persistent store init failed for {}", dataDirUtf8);
        return false;
    }

    // 3. 注册 Windows 标准隐私、图片与富文本 (HTML/RTF) 剪贴板格式
    m_fmtExcludeMonitor = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    m_fmtCanIncludeHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    m_fmtViewerIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    m_fmtPng = RegisterClipboardFormatW(L"PNG");
    m_fmtHtml = RegisterClipboardFormatW(L"HTML Format");
    m_fmtRtf = RegisterClipboardFormatW(L"Rich Text Format");
    m_lastProcessedSeq = GetClipboardSequenceNumber();

    // 4. 创建后台图片编码与入库单工作线程
    m_worker = std::jthread([this](std::stop_token st) { workerLoop(st); });

    // 5. 创建 0% CPU 事件驱动 HWND_MESSAGE 窗口并按配置注册监听
    if (hInstance != nullptr) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ClipboardManager::listenerWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = kListenerWndClass;
        RegisterClassExW(&wc);

        m_listenerHwnd = CreateWindowExW(
            0, kListenerWndClass, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, hInstance, this);
    }

    m_enabled.store(loaded.enabled, std::memory_order_release);
    m_initialized.store(true, std::memory_order_release);
    ensureListenerRegistered(loaded.enabled);

    LOG_INFO("ClipboardManager: initialized (enabled={}, dataDir={})", loaded.enabled, dataDirUtf8);
    return true;
}

void ClipboardManager::shutdown() {
    if (!m_initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    m_enabled.store(false, std::memory_order_release);
    ensureListenerRegistered(false);

    if (m_listenerHwnd) {
        KillTimer(m_listenerHwnd, IDT_CLIP_DEBOUNCE);
        DestroyWindow(m_listenerHwnd);
        m_listenerHwnd = nullptr;
    }

    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_queueCv.notify_all();
        m_worker.join();
    }
    {
        std::lock_guard<std::mutex> qlock(m_queueMutex);
        m_queue.clear();
    }

    t3k_clip_shutdown();
    LOG_INFO("ClipboardManager: shutdown complete");
}

void ClipboardManager::ensureListenerRegistered(bool shouldListen) {
    if (!m_listenerHwnd || !IsWindow(m_listenerHwnd)) return;
    if (shouldListen && !m_listenerRegistered) {
        if (AddClipboardFormatListener(m_listenerHwnd)) {
            m_listenerRegistered = true;
            m_lastProcessedSeq = GetClipboardSequenceNumber();
        }
    } else if (!shouldListen && m_listenerRegistered) {
        KillTimer(m_listenerHwnd, IDT_CLIP_DEBOUNCE);
        RemoveClipboardFormatListener(m_listenerHwnd);
        m_listenerRegistered = false;
    }
}

ClipboardSettings ClipboardManager::getSettings() const {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_settings;
}

bool ClipboardManager::updateSettings(const ClipboardSettings& next) {
    ClipboardSettings clamped = next;
    clamped.maxItems = std::clamp(clamped.maxItems, 20, 5000);
    clamped.maxDays = std::clamp(clamped.maxDays, 0, 3650);
    clamped.maxImageMb = std::clamp(clamped.maxImageMb, 1, 64);

    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        m_settings = clamped;
    }
    m_enabled.store(clamped.enabled, std::memory_order_release);

    auto& cfg = tools3000::core::ConfigManager::instance();
    const bool saved = cfg.mergePatch({
        {"clipboard", {
            {"enabled", clamped.enabled},
            {"maxItems", clamped.maxItems},
            {"maxDays", clamped.maxDays},
            {"maxImageMb", clamped.maxImageMb},
            {"autoPaste", clamped.autoPaste},
            {"plainTextByDefault", clamped.plainTextByDefault},
            {"captureImages", clamped.captureImages},
            {"captureFiles", clamped.captureFiles},
            {"excludedApps", clamped.excludedApps}
        }},
        {"plugins", {
            {"clipboard", {{"enabled", clamped.enabled}}}
        }}
    }, "/clipboard");

    tools3000::core::MainThreadDispatcher::instance().post([this, en = clamped.enabled]() {
        ensureListenerRegistered(en);
    });

    if (m_initialized.load(std::memory_order_acquire)) {
        (void)t3k_clip_prune(static_cast<uint32_t>(clamped.maxItems), static_cast<uint32_t>(clamped.maxDays));
    }
    return saved;
}

void ClipboardManager::setEnabled(bool enabled) {
    auto s = getSettings();
    s.enabled = enabled;
    m_enabled.store(enabled, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        m_settings.enabled = enabled;
    }
    tools3000::core::MainThreadDispatcher::instance().post([this, enabled]() {
        ensureListenerRegistered(enabled);
    });
}

LRESULT CALLBACK ClipboardManager::listenerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ClipboardManager* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        self = static_cast<ClipboardManager*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ClipboardManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        if (msg == WM_CLIPBOARDUPDATE) {
            if (self->m_enabled.load(std::memory_order_acquire)) {
                // 45ms 轻量防抖，合并 Office/浏览器单次复制产生的多段写入
                SetTimer(hwnd, IDT_CLIP_DEBOUNCE, kDebounceMs, nullptr);
            }
            return 0;
        }
        if (msg == WM_TIMER && wp == IDT_CLIP_DEBOUNCE) {
            KillTimer(hwnd, IDT_CLIP_DEBOUNCE);
            self->handleClipboardUpdateDebounced();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ClipboardManager::handleClipboardUpdateDebounced() {
    if (!m_initialized.load(std::memory_order_acquire) || !m_enabled.load(std::memory_order_acquire)) {
        return;
    }

    const DWORD seq = GetClipboardSequenceNumber();
    if (seq == 0 || seq == m_lastProcessedSeq) {
        return;
    }
    m_lastProcessedSeq = seq;

    if (seq == m_suppressedSeq.load(std::memory_order_acquire)) {
        m_suppressedSeq.store(0, std::memory_order_release);
        return;
    }

    const ClipboardSettings snap = getSettings();

    // 1. 来源进程隐私黑名单过滤（先查剪贴板拥有者，回退查当前前台窗口）
    std::string sourceApp = queryProcessExeNameFromHwnd(GetClipboardOwner());
    if (sourceApp.empty()) {
        sourceApp = queryProcessExeNameFromHwnd(GetForegroundWindow());
    }
    if (isExcludedProcessName(sourceApp, snap.excludedApps)) {
        return;
    }

    // 2. 极速短持锁打开系统剪贴板（最多尝试 3 次短等，绝不阻塞 Ctrl+C / RDP）
    bool opened = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (OpenClipboard(m_listenerHwnd)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) return;

    struct ClipboardGuard {
        ~ClipboardGuard() { CloseClipboard(); }
    } clipGuard;

    // 3. 严格尊重 Windows 官方隐私协议（密码管理器自动免录）
    if ((m_fmtExcludeMonitor != 0 && IsClipboardFormatAvailable(m_fmtExcludeMonitor)) ||
        (m_fmtViewerIgnore != 0 && IsClipboardFormatAvailable(m_fmtViewerIgnore))) {
        return;
    }
    if (m_fmtCanIncludeHistory != 0 && IsClipboardFormatAvailable(m_fmtCanIncludeHistory)) {
        HANDLE hFlag = GetClipboardData(m_fmtCanIncludeHistory);
        if (hFlag) {
            const auto* pVal = static_cast<const DWORD*>(GlobalLock(hFlag));
            const bool forbidden = (pVal != nullptr && *pVal == 0);
            GlobalUnlock(hFlag);
            if (forbidden) return;
        }
    }

    PendingCaptureTask task{};
    task.sourceApp = std::move(sourceApp);

    // 4. 优先级抓取：图片 (PNG / CF_DIBV5 / CF_DIB) -> 文件列表 (CF_HDROP) -> 文本 (CF_UNICODETEXT)
    // 注意：截图工具（Tools3000 / 微信 / QQ / 浏览器「复制图像」）常同时写入 CF_DIB 与临时 PNG 的 CF_HDROP；
    // 资源管理器复制文件仅写 CF_HDROP 不写位图格式。因此图片必须优先于 CF_HDROP 判定，避免截图误归入「文件」分类！
    if (snap.captureImages) {
        const size_t maxImgBytes = static_cast<size_t>(snap.maxImageMb) * 1024u * 1024u;
        if (m_fmtPng != 0 && IsClipboardFormatAvailable(m_fmtPng)) {
            HANDLE hPng = GetClipboardData(m_fmtPng);
            if (hPng) {
                const SIZE_T sz = GlobalSize(hPng);
                if (sz > 24 && sz <= maxImgBytes) {
                    const void* ptr = GlobalLock(hPng);
                    if (ptr) {
                        task.rawPngBytes.resize(sz);
                        std::memcpy(task.rawPngBytes.data(), ptr, sz);
                        GlobalUnlock(hPng);
                        task.kind = 1; // Image
                        enqueueTask(std::move(task));
                        return;
                    }
                }
            }
        }
        if (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB)) {
            const UINT fmt = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB;
            HANDLE hDib = GetClipboardData(fmt);
            if (hDib) {
                const SIZE_T sz = GlobalSize(hDib);
                // 未压缩 DIB 可能大于 PNG，放宽至 maxImageMb * 4 (上限 96MB) 交由后台压缩后再校验 PNG 上限
                const size_t maxRawDib = (std::min)(maxImgBytes * 4u, static_cast<size_t>(96u * 1024u * 1024u));
                if (sz >= sizeof(BITMAPINFOHEADER) && sz <= maxRawDib) {
                    const void* ptr = GlobalLock(hDib);
                    if (ptr) {
                        task.rawDibBytes.resize(sz);
                        std::memcpy(task.rawDibBytes.data(), ptr, sz);
                        GlobalUnlock(hDib);
                        task.kind = 1; // Image
                        enqueueTask(std::move(task));
                        return;
                    }
                }
            }
        }
    }

    if (snap.captureFiles && IsClipboardFormatAvailable(CF_HDROP)) {
        HANDLE hDrop = GetClipboardData(CF_HDROP);
        if (hDrop) {
            const UINT count = DragQueryFileW(static_cast<HDROP>(hDrop), 0xFFFFFFFFu, nullptr, 0);
            if (count > 0 && count <= 500) {
                std::ostringstream oss;
                for (UINT i = 0; i < count; ++i) {
                    const UINT chars = DragQueryFileW(static_cast<HDROP>(hDrop), i, nullptr, 0);
                    if (chars == 0 || chars > 32768) continue;
                    std::wstring filePath(chars, L'\0');
                    DragQueryFileW(static_cast<HDROP>(hDrop), i, filePath.data(), chars + 1);
                    if (i > 0) oss << '\n';
                    oss << tools3000::core::WinUtils::wstringToUtf8(filePath);
                }
                task.textUtf8 = oss.str();
                if (!task.textUtf8.empty()) {
                    task.kind = 2; // Files
                    enqueueTask(std::move(task));
                    return;
                }
            }
        }
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hText = GetClipboardData(CF_UNICODETEXT);
        if (hText) {
            const auto* wptr = static_cast<const wchar_t*>(GlobalLock(hText));
            if (wptr) {
                const SIZE_T maxChars = GlobalSize(hText) / sizeof(wchar_t);
                const size_t safeLen = wcsnlen(wptr, (std::min<size_t>)(maxChars, 262144u));
                const std::wstring wtext(wptr, safeLen);
                GlobalUnlock(hText);
                task.textUtf8 = tools3000::core::WinUtils::wstringToUtf8(wtext);
                if (!task.textUtf8.empty()) {
                    // 同步捕获 CF_HTML / CF_RTF 富文本样式（严格封顶 512 KB 杜绝内存突刺）
                    auto grabNullTermBytes = [](UINT fmt, std::vector<uint8_t>& out) {
                        if (fmt == 0 || !IsClipboardFormatAvailable(fmt)) return;
                        HANDLE hData = GetClipboardData(fmt);
                        if (!hData) return;
                        const SIZE_T rawSz = GlobalSize(hData);
                        if (rawSz == 0 || rawSz > kMaxRichTextBytes) return;
                        const auto* bytes = static_cast<const uint8_t*>(GlobalLock(hData));
                        if (!bytes) return;
                        const size_t actualLen = strnlen(reinterpret_cast<const char*>(bytes), static_cast<size_t>(rawSz));
                        if (actualLen > 0 && actualLen <= kMaxRichTextBytes) {
                            out.assign(bytes, bytes + actualLen);
                        }
                        GlobalUnlock(hData);
                    };
                    grabNullTermBytes(m_fmtHtml, task.rawHtmlBytes);
                    grabNullTermBytes(m_fmtRtf, task.rawRtfBytes);

                    task.kind = 0; // Text (Rust 内核会自动升级识别 #HEX/rgb() 为 Color)
                    enqueueTask(std::move(task));
                    return;
                }
            }
        }
    }
}

void ClipboardManager::enqueueTask(PendingCaptureTask&& task) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (m_queue.size() >= kMaxPendingQueueItems) {
            m_queue.pop_front();
        }
        m_queue.push_back(std::move(task));
    }
    m_queueCv.notify_one();
}

void ClipboardManager::workerLoop(std::stop_token st) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInit = SUCCEEDED(hr);

    while (!st.stop_requested()) {
        PendingCaptureTask task{};
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, st, [this]() { return !m_queue.empty(); });
            if (st.stop_requested()) break;
            if (m_queue.empty()) continue;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        try {
            processTask(task);
        } catch (const std::exception& e) {
            LOG_WARN("ClipboardManager: worker task failed: {}", e.what());
        } catch (...) {
            LOG_WARN("ClipboardManager: worker task unknown error");
        }
    }

    if (comInit) {
        CoUninitialize();
    }
}

void ClipboardManager::processTask(PendingCaptureTask& task) {
    const ClipboardSettings snap = getSettings();
    uint64_t addedId = 0;

    if (task.kind == 1) {
        std::vector<uint8_t> pngBytes;
        uint32_t imgW = 0;
        uint32_t imgH = 0;
        if (!task.rawPngBytes.empty()) {
            if (!probePngDimensions(task.rawPngBytes, imgW, imgH)) return;
            pngBytes = std::move(task.rawPngBytes);
        } else if (!task.rawDibBytes.empty()) {
            if (!encodeDibToPngBytes(task.rawDibBytes, pngBytes, imgW, imgH)) return;
        } else {
            return;
        }

        const size_t maxImgBytes = static_cast<size_t>(snap.maxImageMb) * 1024u * 1024u;
        if (pngBytes.empty() || pngBytes.size() > maxImgBytes) return;

        const std::string hashHex = fnv1a128Hex(pngBytes.data(), pngBytes.size());
        const std::string fileName = hashHex.substr(0, 20) + ".png";
        const std::filesystem::path blobPath = getBlobsDir() / tools3000::core::WinUtils::utf8ToWstring(fileName);

        std::error_code ec;
        if (!std::filesystem::exists(blobPath, ec)) {
            std::filesystem::create_directories(blobPath.parent_path(), ec);
            const std::filesystem::path tmpPath = blobPath.wstring() + L".tmp";
            {
                std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
                if (!ofs) return;
                ofs.write(reinterpret_cast<const char*>(pngBytes.data()),
                          static_cast<std::streamsize>(pngBytes.size()));
            }
            std::filesystem::rename(tmpPath, blobPath, ec);
            if (ec) {
                std::filesystem::remove(tmpPath, ec);
                if (!std::filesystem::exists(blobPath, ec)) return;
            }
        }

        addedId = t3k_clip_add_item(
            1,
            "",
            fileName.c_str(),
            imgW,
            imgH,
            hashHex.c_str(),
            task.sourceApp.c_str(),
            static_cast<uint64_t>(pngBytes.size()),
            static_cast<uint32_t>(snap.maxItems),
            static_cast<uint32_t>(snap.maxDays));
    } else {
        if (task.textUtf8.empty()) return;
        auto persistRichBlob = [&](const std::vector<uint8_t>& bytes, const char* prefix, const char* ext) -> std::string {
            if (bytes.empty() || bytes.size() > kMaxRichTextBytes) return {};
            const std::string h = fnv1a128Hex(bytes.data(), bytes.size());
            const std::string name = std::string(prefix) + h.substr(0, 20) + ext;
            const std::filesystem::path p = getBlobsDir() / tools3000::core::WinUtils::utf8ToWstring(name);
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                std::filesystem::create_directories(p.parent_path(), ec);
                const std::filesystem::path tmp = p.wstring() + L".tmp";
                {
                    std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
                    if (!ofs) return {};
                    ofs.write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
                }
                std::filesystem::rename(tmp, p, ec);
                if (ec) {
                    std::filesystem::remove(tmp, ec);
                    if (!std::filesystem::exists(p, ec)) return {};
                }
            }
            return name;
        };

        const std::string htmlFile = persistRichBlob(task.rawHtmlBytes, "h_", ".html");
        const std::string rtfFile = persistRichBlob(task.rawRtfBytes, "r_", ".rtf");

        addedId = t3k_clip_add_item_v2(
            task.kind,
            task.textUtf8.c_str(),
            "",
            0,
            0,
            "",
            task.sourceApp.c_str(),
            static_cast<uint64_t>(task.textUtf8.size()),
            static_cast<uint32_t>(snap.maxItems),
            static_cast<uint32_t>(snap.maxDays),
            htmlFile.c_str(),
            rtfFile.c_str());
    }

    if (addedId > 0) {
        notifyChanged({{"id", addedId}, {"kind", task.kind}});
    }
}

void ClipboardManager::setChangeNotifier(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lock(m_notifierMutex);
    m_changeNotifier = std::move(cb);
}

void ClipboardManager::notifyChanged(const nlohmann::json& payload) {
    tools3000::core::MessageBridge::instance().pushEvent("clipboard:updated", payload);
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(m_notifierMutex);
        cb = m_changeNotifier;
    }
    if (cb) {
        const std::string envelope = R"({"type":"event","event":"clipboard:updated","data":)" + payload.dump() + "}";
        tools3000::core::MainThreadDispatcher::instance().post([cb = std::move(cb), envelope]() {
            cb(envelope);
        });
    }
}

nlohmann::json ClipboardManager::queryItems(
    const std::string& query, const std::string& kind, uint32_t limit, uint32_t offset,
    const std::string& groupId) const {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return {{"total", 0}, {"matched", 0}, {"items", nlohmann::json::array()}};
    }
    const std::string raw = consumeRustJsonString(
        t3k_clip_query_json_v2(query.c_str(), kind.c_str(), groupId.c_str(), limit, offset));
    try {
        return nlohmann::json::parse(raw);
    } catch (...) {
        return {{"total", 0}, {"matched", 0}, {"items", nlohmann::json::array()}};
    }
}

nlohmann::json ClipboardManager::getItem(uint64_t id) const {
    if (!m_initialized.load(std::memory_order_acquire)) return nlohmann::json();
    const std::string raw = consumeRustJsonString(t3k_clip_get_item_json(id));
    if (raw.empty()) return nlohmann::json();
    try {
        return nlohmann::json::parse(raw);
    } catch (...) {
        return nlohmann::json();
    }
}

int ClipboardManager::togglePin(uint64_t id) {
    if (!m_initialized.load(std::memory_order_acquire)) return -1;
    const int res = t3k_clip_toggle_pin(id);
    if (res >= 0) {
        notifyChanged({{"id", id}, {"pinned", res == 1}});
    }
    return res;
}

bool ClipboardManager::deleteItem(uint64_t id) {
    if (!m_initialized.load(std::memory_order_acquire)) return false;
    const bool ok = t3k_clip_delete_item(id) != 0;
    if (ok) {
        notifyChanged({{"deletedId", id}});
    }
    return ok;
}

uint32_t ClipboardManager::deleteItems(const std::vector<uint64_t>& ids, bool skipPinned) {
    if (!m_initialized.load(std::memory_order_acquire) || ids.empty()) return 0;
    const uint32_t removed = t3k_clip_delete_items_batch(
        ids.data(), static_cast<uint32_t>(ids.size()), skipPinned ? 1 : 0);
    if (removed > 0) {
        notifyChanged({{"batchDeleted", removed}});
    }
    return removed;
}

bool ClipboardManager::updateItemText(uint64_t id, const std::string& newTextUtf8) {
    if (!m_initialized.load(std::memory_order_acquire) || id == 0) return false;
    const bool ok = t3k_clip_update_item_text(id, newTextUtf8.c_str()) != 0;
    if (ok) {
        notifyChanged({{"editedId", id}});
    }
    return ok;
}

nlohmann::json ClipboardManager::listGroups() const {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return {{"groups", nlohmann::json::array()}};
    }
    const std::string raw = consumeRustJsonString(t3k_clip_list_groups_json());
    try {
        return nlohmann::json::parse(raw);
    } catch (...) {
        return {{"groups", nlohmann::json::array()}};
    }
}

nlohmann::json ClipboardManager::createGroup(const std::string& nameUtf8) {
    if (!m_initialized.load(std::memory_order_acquire) || nameUtf8.empty()) {
        return nlohmann::json();
    }
    const std::string raw = consumeRustJsonString(t3k_clip_create_group(nameUtf8.c_str()));
    if (raw.empty()) return nlohmann::json();
    try {
        auto grp = nlohmann::json::parse(raw);
        notifyChanged({{"groupsChanged", true}});
        return grp;
    } catch (...) {
        return nlohmann::json();
    }
}

bool ClipboardManager::renameGroup(const std::string& groupId, const std::string& newNameUtf8) {
    if (!m_initialized.load(std::memory_order_acquire)) return false;
    const bool ok = t3k_clip_rename_group(groupId.c_str(), newNameUtf8.c_str()) != 0;
    if (ok) {
        notifyChanged({{"groupsChanged", true}});
    }
    return ok;
}

bool ClipboardManager::deleteGroup(const std::string& groupId) {
    if (!m_initialized.load(std::memory_order_acquire)) return false;
    const bool ok = t3k_clip_delete_group(groupId.c_str()) != 0;
    if (ok) {
        notifyChanged({{"groupsChanged", true}});
    }
    return ok;
}

uint32_t ClipboardManager::setItemsGroup(const std::vector<uint64_t>& ids, const std::string& groupId) {
    if (!m_initialized.load(std::memory_order_acquire) || ids.empty()) return 0;
    const uint32_t updated = t3k_clip_set_items_group(
        ids.data(), static_cast<uint32_t>(ids.size()), groupId.c_str());
    if (updated > 0) {
        notifyChanged({{"groupsChanged", true}, {"updated", updated}});
    }
    return updated;
}

uint32_t ClipboardManager::clearHistory(bool keepPinned) {
    if (!m_initialized.load(std::memory_order_acquire)) return 0;
    const uint32_t removed = t3k_clip_clear_history(keepPinned ? 1 : 0);
    notifyChanged({{"cleared", true}, {"removed", removed}});
    return removed;
}

nlohmann::json ClipboardManager::getStats() const {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return {{"totalCount", 0}, {"pinnedCount", 0}, {"diskBytes", 0}, {"dataDir", ""}};
    }
    const std::string raw = consumeRustJsonString(t3k_clip_stats_json());
    try {
        return nlohmann::json::parse(raw);
    } catch (...) {
        return {{"totalCount", 0}, {"pinnedCount", 0}, {"diskBytes", 0}, {"dataDir", ""}};
    }
}

std::filesystem::path ClipboardManager::resolveSafeBlobFile(const std::string& relName) const {
    if (relName.empty() || relName.find("..") != std::string::npos ||
        relName.find('\\') != std::string::npos || relName.find('/') != std::string::npos) {
        return {};
    }
    return getBlobsDir() / tools3000::core::WinUtils::utf8ToWstring(relName);
}

std::filesystem::path ClipboardManager::resolveImageBlobPath(uint64_t id) const {
    const auto item = getItem(id);
    if (!item.is_object()) return {};
    return resolveSafeBlobFile(item.value("imageFile", ""));
}

std::filesystem::path ClipboardManager::getDataDir() const {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_dataDir;
}

std::filesystem::path ClipboardManager::getBlobsDir() const {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_blobsDir;
}

bool ClipboardManager::writeTextToSystemClipboard(
    const std::wstring& wtext,
    const std::vector<uint8_t>& htmlBytes,
    const std::vector<uint8_t>& rtfBytes) {
    if (!OpenClipboard(m_listenerHwnd)) return false;
    struct Guard { ~Guard() { CloseClipboard(); } } guard;
    EmptyClipboard();

    const size_t bytes = (wtext.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) return false;
    void* dst = GlobalLock(hMem);
    if (!dst) {
        GlobalFree(hMem);
        return false;
    }
    std::memcpy(dst, wtext.c_str(), bytes);
    GlobalUnlock(hMem);
    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        return false;
    }

    auto setNullTermFormat = [](UINT fmt, const std::vector<uint8_t>& raw) {
        if (fmt == 0 || raw.empty()) return;
        const bool hasNul = (raw.back() == 0);
        const size_t allocSize = raw.size() + (hasNul ? 0u : 1u);
        HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, allocSize);
        if (!hBuf) return;
        auto* p = static_cast<uint8_t*>(GlobalLock(hBuf));
        if (!p) {
            GlobalFree(hBuf);
            return;
        }
        std::memcpy(p, raw.data(), raw.size());
        if (!hasNul) p[raw.size()] = 0;
        GlobalUnlock(hBuf);
        if (!SetClipboardData(fmt, hBuf)) {
            GlobalFree(hBuf);
        }
    };
    setNullTermFormat(m_fmtHtml, htmlBytes);
    setNullTermFormat(m_fmtRtf, rtfBytes);

    m_suppressedSeq.store(GetClipboardSequenceNumber(), std::memory_order_release);
    return true;
}

bool ClipboardManager::writeFilesToSystemClipboard(const std::vector<std::wstring>& files) {
    if (files.empty()) return false;
    size_t totalChars = 1; // 末尾双 null
    for (const auto& f : files) totalChars += f.size() + 1;

    const size_t dropBytes = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);
    HGLOBAL hDrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, dropBytes);
    if (!hDrop) return false;
    auto* df = static_cast<DROPFILES*>(GlobalLock(hDrop));
    if (!df) {
        GlobalFree(hDrop);
        return false;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
    for (const auto& f : files) {
        std::memcpy(cursor, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
        cursor += f.size() + 1;
    }
    *cursor = L'\0';
    GlobalUnlock(hDrop);

    if (!OpenClipboard(m_listenerHwnd)) {
        GlobalFree(hDrop);
        return false;
    }
    struct Guard { ~Guard() { CloseClipboard(); } } guard;
    EmptyClipboard();
    if (!SetClipboardData(CF_HDROP, hDrop)) {
        GlobalFree(hDrop);
        return false;
    }
    m_suppressedSeq.store(GetClipboardSequenceNumber(), std::memory_order_release);
    return true;
}

bool ClipboardManager::writePngImageToSystemClipboard(const std::filesystem::path& pngPath) {
    std::vector<uint8_t> dibBytes;
    std::vector<uint8_t> rawPng;
    if (!decodePngFileToDibBytes(pngPath, dibBytes, rawPng)) {
        return false;
    }

    if (!OpenClipboard(m_listenerHwnd)) return false;
    struct Guard { ~Guard() { CloseClipboard(); } } guard;
    EmptyClipboard();

    bool anyOk = false;
    if (!dibBytes.empty()) {
        HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, dibBytes.size());
        if (hDib) {
            void* p = GlobalLock(hDib);
            if (p) {
                std::memcpy(p, dibBytes.data(), dibBytes.size());
                GlobalUnlock(hDib);
                if (SetClipboardData(CF_DIB, hDib)) {
                    anyOk = true;
                } else {
                    GlobalFree(hDib);
                }
            } else {
                GlobalFree(hDib);
            }
        }
    }
    if (m_fmtPng != 0 && !rawPng.empty()) {
        HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, rawPng.size());
        if (hPng) {
            void* p = GlobalLock(hPng);
            if (p) {
                std::memcpy(p, rawPng.data(), rawPng.size());
                GlobalUnlock(hPng);
                if (SetClipboardData(m_fmtPng, hPng)) {
                    anyOk = true;
                } else {
                    GlobalFree(hPng);
                }
            } else {
                GlobalFree(hPng);
            }
        }
    }
    if (anyOk) {
        m_suppressedSeq.store(GetClipboardSequenceNumber(), std::memory_order_release);
    }
    return anyOk;
}

void ClipboardManager::sendCtrlVPasteAsync(HWND targetHwnd) {
    std::thread([targetHwnd]() {
        Sleep(55);
        if (targetHwnd && IsWindow(targetHwnd)) {
            SetForegroundWindow(targetHwnd);
            Sleep(25);
        }
        // 确保释放任何残留的修饰键，随后投递标准 Ctrl+V
        std::array<INPUT, 6> inputs{};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_MENU;
        inputs[0].ki.dwFlags = KEYEVENTF_KEYUP;

        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_SHIFT;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wVk = VK_CONTROL;

        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wVk = 'V';

        inputs[4].type = INPUT_KEYBOARD;
        inputs[4].ki.wVk = 'V';
        inputs[4].ki.dwFlags = KEYEVENTF_KEYUP;

        inputs[5].type = INPUT_KEYBOARD;
        inputs[5].ki.wVk = VK_CONTROL;
        inputs[5].ki.dwFlags = KEYEVENTF_KEYUP;

        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }).detach();
}

bool ClipboardManager::activateItem(uint64_t id, bool plainTextOnly, bool autoPaste, HWND targetHwnd) {
    const auto item = getItem(id);
    if (!item.is_object()) return false;

    const std::string kind = item.value("kind", "text");
    bool ok = false;

    if (kind == "image" && !plainTextOnly) {
        const auto blobPath = resolveImageBlobPath(id);
        std::error_code ec;
        if (!blobPath.empty() && std::filesystem::exists(blobPath, ec)) {
            ok = writePngImageToSystemClipboard(blobPath);
        }
    } else if (kind == "files" && !plainTextOnly) {
        const std::string text = item.value("text", "");
        std::vector<std::wstring> files;
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            const std::string t = trimAscii(line);
            if (!t.empty()) {
                files.push_back(tools3000::core::WinUtils::utf8ToWstring(t));
            }
        }
        ok = writeFilesToSystemClipboard(files);
    } else {
        const std::string text = item.value("text", "");
        if (!text.empty()) {
            std::vector<uint8_t> htmlBytes;
            std::vector<uint8_t> rtfBytes;
            if (!plainTextOnly) {
                auto readRichFile = [&](const std::string& relName, std::vector<uint8_t>& out) {
                    const auto path = resolveSafeBlobFile(relName);
                    std::error_code ec;
                    if (path.empty() || !std::filesystem::exists(path, ec)) return;
                    const auto sz = std::filesystem::file_size(path, ec);
                    if (ec || sz == 0 || sz > kMaxRichTextBytes) return;
                    std::ifstream ifs(path, std::ios::binary);
                    if (!ifs) return;
                    out.resize(static_cast<size_t>(sz));
                    ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sz));
                };
                readRichFile(item.value("htmlFile", ""), htmlBytes);
                readRichFile(item.value("rtfFile", ""), rtfBytes);
            }
            ok = writeTextToSystemClipboard(
                tools3000::core::WinUtils::utf8ToWstring(text), htmlBytes, rtfBytes);
        }
    }

    if (ok && autoPaste) {
        sendCtrlVPasteAsync(targetHwnd);
    }
    return ok;
}

bool ClipboardManager::activateItemsBatch(
    const std::vector<uint64_t>& ids,
    const std::string& separator,
    bool autoPaste,
    HWND targetHwnd) {
    if (ids.empty()) return false;
    std::string combined;
    size_t added = 0;
    for (uint64_t id : ids) {
        const auto item = getItem(id);
        if (!item.is_object()) continue;
        const std::string kind = item.value("kind", "text");
        if (kind == "image") continue;
        const std::string text = item.value("text", "");
        if (text.empty()) continue;
        if (added > 0) {
            combined.append(separator);
        }
        combined.append(text);
        ++added;
    }
    if (added == 0 || combined.empty()) return false;

    const bool ok = writeTextToSystemClipboard(tools3000::core::WinUtils::utf8ToWstring(combined));
    if (ok && autoPaste) {
        sendCtrlVPasteAsync(targetHwnd);
    }
    return ok;
}

}  // namespace tools3000::clipboard

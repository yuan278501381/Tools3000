// ─────────────────────────────────────────────────────────────────────────────
// ClipboardNativeDrag.cpp — 混合双引擎 Win32 OLE + 落点智能直贴拖拽引擎实现
// ─────────────────────────────────────────────────────────────────────────────

#include "clipboard/ClipboardNativeDrag.h"
#include "clipboard/ClipboardManager.h"

#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace tools3000::clipboard {

namespace {

constexpr size_t kMaxDragHtmlBytes = 512u * 1024u;

std::string trimAsciiCopy(std::string_view sv) {
    size_t s = 0;
    while (s < sv.size() && std::isspace(static_cast<unsigned char>(sv[s]))) ++s;
    size_t e = sv.size();
    while (e > s && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(s, e - s));
}

bool isSafeRelativeBlobName(const std::string& relName) noexcept {
    return !relName.empty() &&
           relName.find("..") == std::string::npos &&
           relName.find('\\') == std::string::npos &&
           relName.find('/') == std::string::npos;
}

UINT getHtmlClipboardFormat() noexcept {
    static const UINT fmt = RegisterClipboardFormatW(L"HTML Format");
    return fmt;
}

UINT getPngClipboardFormat() noexcept {
    static const UINT fmt = RegisterClipboardFormatW(L"PNG");
    return fmt;
}

HGLOBAL allocGlobalBytes(const void* data, size_t sizeBytes) noexcept {
    if (!data || sizeBytes == 0) return nullptr;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeBytes);
    if (!hMem) return nullptr;
    void* dst = GlobalLock(hMem);
    if (!dst) {
        GlobalFree(hMem);
        return nullptr;
    }
    std::memcpy(dst, data, sizeBytes);
    GlobalUnlock(hMem);
    return hMem;
}

HGLOBAL allocNullTerminatedBytes(const std::vector<uint8_t>& raw) noexcept {
    if (raw.empty()) return nullptr;
    const bool hasNul = (raw.back() == 0);
    const size_t total = raw.size() + (hasNul ? 0u : 1u);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!hMem) return nullptr;
    auto* dst = static_cast<uint8_t*>(GlobalLock(hMem));
    if (!dst) {
        GlobalFree(hMem);
        return nullptr;
    }
    std::memcpy(dst, raw.data(), raw.size());
    if (!hasNul) {
        dst[raw.size()] = 0;
    }
    GlobalUnlock(hMem);
    return hMem;
}

HGLOBAL allocUnicodeTextGlobal(const std::wstring& text) noexcept {
    if (text.empty()) return nullptr;
    const size_t bytes = (text.size() + 1u) * sizeof(wchar_t);
    return allocGlobalBytes(text.c_str(), bytes);
}

HGLOBAL allocAnsiTextGlobal(const std::wstring& text) noexcept {
    if (text.empty()) return nullptr;
    const int needed = WideCharToMultiByte(
        CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return nullptr;
    std::string ansi(static_cast<size_t>(needed) + 1u, '\0');
    const int written = WideCharToMultiByte(
        CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), ansi.data(), needed, nullptr, nullptr);
    if (written <= 0) return nullptr;
    ansi[static_cast<size_t>(written)] = '\0';
    return allocGlobalBytes(ansi.data(), static_cast<size_t>(written) + 1u);
}

HGLOBAL allocDropFilesGlobal(const std::vector<std::wstring>& files) noexcept {
    if (files.empty()) return nullptr;
    size_t totalChars = 1u; // 末尾双 null 终止符
    for (const auto& f : files) {
        if (!f.empty()) {
            totalChars += f.size() + 1u;
        }
    }
    if (totalChars <= 1u) return nullptr;

    const size_t dropBytes = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);
    HGLOBAL hDrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, dropBytes);
    if (!hDrop) return nullptr;
    auto* df = static_cast<DROPFILES*>(GlobalLock(hDrop));
    if (!df) {
        GlobalFree(hDrop);
        return nullptr;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
    for (const auto& f : files) {
        if (f.empty()) continue;
        std::memcpy(cursor, f.c_str(), (f.size() + 1u) * sizeof(wchar_t));
        cursor += f.size() + 1u;
    }
    *cursor = L'\0';
    GlobalUnlock(hDrop);
    return hDrop;
}

bool isDesktopShellWindow(HWND rootHwnd) noexcept {
    if (!rootHwnd || !IsWindow(rootHwnd)) return true;
    wchar_t cls[64]{};
    const int len = GetClassNameW(rootHwnd, cls, static_cast<int>(std::size(cls)));
    if (len <= 0) return false;
    const std::wstring_view sv(cls, static_cast<size_t>(len));
    return sv == L"Progman" || sv == L"WorkerW" || sv == L"Shell_SecondaryTrayWnd" ||
           sv == L"Shell_TrayWnd" || sv == L"NotifyIconOverflowWindow" ||
           sv == L"TopLevelWindowForOverflowXamlIsland";
}

bool equalsIgnoreCaseAsciiW(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const wchar_t ca = (a[i] >= L'A' && a[i] <= L'Z') ? static_cast<wchar_t>(a[i] - L'A' + L'a') : a[i];
        const wchar_t cb = (b[i] >= L'A' && b[i] <= L'Z') ? static_cast<wchar_t>(b[i] - L'A' + L'a') : b[i];
        if (ca != cb) return false;
    }
    return true;
}

bool sourceWindowMatches(HWND fgHwnd, HWND sourceClipboardHwnd) noexcept {
    if (!fgHwnd) return false;
    if (sourceClipboardHwnd && (fgHwnd == sourceClipboardHwnd || IsChild(sourceClipboardHwnd, fgHwnd))) {
        return true;
    }
    wchar_t cls[64]{};
    if (GetClassNameW(fgHwnd, cls, static_cast<int>(std::size(cls))) > 0 &&
        std::wstring_view(cls) == L"Tools3000_ClipboardWindow") {
        return true;
    }
    return false;
}

HWND resolveSafeExternalDropTarget(
    POINT screenPt,
    HWND sourceClipboardHwnd,
    bool& outInClientArea,
    TerminalPasteKind& outTerminalKind) noexcept {
    outInClientArea = false;
    outTerminalKind = TerminalPasteKind::NotTerminal;

    HWND hitHwnd = WindowFromPoint(screenPt);
    if (!hitHwnd || !IsWindow(hitHwnd)) return nullptr;

    HWND rootHwnd = ClipboardManager::normalizeValidTargetHwnd(hitHwnd, sourceClipboardHwnd);
    if (!rootHwnd || isDesktopShellWindow(rootHwnd)) {
        return nullptr;
    }

    wchar_t rootCls[96]{};
    const int rootClsLen = GetClassNameW(rootHwnd, rootCls, static_cast<int>(std::size(rootCls)));
    const std::wstring_view rootClsView(rootCls, rootClsLen > 0 ? static_cast<size_t>(rootClsLen) : 0u);

    const std::wstring procPath = tools3000::core::WinUtils::getProcessPathFromWindow(rootHwnd);
    std::wstring exeLower;
    if (!procPath.empty()) {
        exeLower = tools3000::core::WinUtils::toLower(std::filesystem::path(procPath).filename().wstring());
    }
    outTerminalKind = classifyTerminalPasteTarget(rootClsView, exeLower);

    RECT rc{};
    if (!GetWindowRect(rootHwnd, &rc)) {
        return rootHwnd;
    }

    const float dpi = std::clamp(tools3000::core::dpi::scaleForWindow(rootHwnd), 1.0f, 4.0f);
    const LONG edgeMargin = static_cast<LONG>(std::lround(7.0f * dpi));
    const LONG stdTitleHeight = static_cast<LONG>(std::lround(38.0f * dpi));
    const LONG winBtnsWidth = static_cast<LONG>(std::lround(150.0f * dpi));
    const LONG winBtnsHeight = static_cast<LONG>(std::lround(42.0f * dpi));

    // 硬几何安全红线：严禁在窗口四周拉伸窄边或右上角系统最小化/最大化/关闭按钮区触发落点单击
    const bool insideOuterSafeBounds =
        (screenPt.x > rc.left + edgeMargin && screenPt.x < rc.right - edgeMargin &&
         screenPt.y > rc.top + edgeMargin && screenPt.y < rc.bottom - edgeMargin);
    const bool inTopRightWindowControls =
        (screenPt.x >= rc.right - winBtnsWidth && screenPt.y <= rc.top + winBtnsHeight);

    if (!insideOuterSafeBounds || inTopRightWindowControls) {
        outInClientArea = false;
        return rootHwnd;
    }

    const bool deepInsideBody = (screenPt.y > rc.top + stdTitleHeight);

    // 若 WindowFromPoint 直接命中了真实子内容渲染窗（如 Windows Terminal 的 InputSiteWindowClass
    // 或 Chromium 的 Chrome_RenderWidgetHostHWND），排除标题栏拖拽子窗后即确认为有效客户区
    if (hitHwnd != rootHwnd) {
        wchar_t hitCls[96]{};
        const int hitClsLen = GetClassNameW(hitHwnd, hitCls, static_cast<int>(std::size(hitCls)));
        const std::wstring_view hitClsView(hitCls, hitClsLen > 0 ? static_cast<size_t>(hitClsLen) : 0u);
        if (!equalsIgnoreCaseAsciiW(hitClsView, L"DRAG_BAR_WINDOW_CLASS") &&
            !equalsIgnoreCaseAsciiW(hitClsView, L"NonClientIslandWindow")) {
            DWORD_PTR childHit = HTCLIENT;
            if (SendMessageTimeoutW(
                    hitHwnd,
                    WM_NCHITTEST,
                    0,
                    MAKELPARAM(screenPt.x, screenPt.y),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    45,
                    &childHit) != 0) {
                outInClientArea = isSafeDropHitTestResult(static_cast<LRESULT>(childHit), deepInsideBody);
            } else {
                outInClientArea = true;
            }
            return rootHwnd;
        }
    }

    // 校验顶层窗口非客户区命中：对无边框自绘 Electron/Tauri 窗口（如 Clash Party），
    // 当落点已深入窗口腹地（y > top + 38*dpi）且已排除右上角控制按钮区时，放行外层 drag 容器的 HTCAPTION
    DWORD_PTR hitTest = HTCLIENT;
    if (SendMessageTimeoutW(
            rootHwnd,
            WM_NCHITTEST,
            0,
            MAKELPARAM(screenPt.x, screenPt.y),
            SMTO_ABORTIFHUNG | SMTO_BLOCK,
            60,
            &hitTest) != 0) {
        outInClientArea = isSafeDropHitTestResult(static_cast<LRESULT>(hitTest), deepInsideBody);
    } else {
        outInClientArea = deepInsideBody;
    }
    return rootHwnd;
}

void sendVirtualDesktopClickAt(POINT screenPt) noexcept {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int vh = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    const double normX = static_cast<double>(screenPt.x - vx) * 65535.0 / static_cast<double>((std::max)(1, vw - 1));
    const double normY = static_cast<double>(screenPt.y - vy) * 65535.0 / static_cast<double>((std::max)(1, vh - 1));
    const LONG absX = std::clamp(static_cast<LONG>(std::lround(normX)), 0L, 65535L);
    const LONG absY = std::clamp(static_cast<LONG>(std::lround(normY)), 0L, 65535L);

    INPUT mouseInputs[3]{};
    mouseInputs[0].type = INPUT_MOUSE;
    mouseInputs[0].mi.dx = absX;
    mouseInputs[0].mi.dy = absY;
    mouseInputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    mouseInputs[1].type = INPUT_MOUSE;
    mouseInputs[1].mi.dx = absX;
    mouseInputs[1].mi.dy = absY;
    mouseInputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    mouseInputs[2].type = INPUT_MOUSE;
    mouseInputs[2].mi.dx = absX;
    mouseInputs[2].mi.dy = absY;
    mouseInputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    SendInput(3u, mouseInputs, sizeof(INPUT));
}

void dispatchSmartDropPasteAsync(
    HWND targetRootHwnd,
    POINT dropScreenPt,
    HWND keepVisibleClipboardHwnd,
    TerminalPasteKind terminalKind) {
    // 异步线程内完成：
    // - 普通 GUI 应用：激活目标窗 -> 精准单击落点定位输入框光标（并解除 Sublime 等编辑器旧选区） -> Ctrl+V；
    // - 命令行终端宿主：仅激活终端窗（跳过鼠标左键单击，绝不误触 QuickEdit 选区或打断 pi/vim TUI） -> 终端原生粘贴热键。
    std::thread([targetRootHwnd, dropScreenPt, keepVisibleClipboardHwnd, terminalKind]() {
        constexpr DWORD kPollStepMs = 10;
        constexpr DWORD kMaxFgWaitMs = 190;

        // 1. 等待物理左键完全松开（防止极少数驱动上行抖动）
        for (int i = 0; i < 8 && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; ++i) {
            Sleep(8);
        }

        // 2. 激活目标窗口；仅非终端 GUI 窗口执行落点单击定位输入框光标
        if (targetRootHwnd && IsWindow(targetRootHwnd)) {
            AllowSetForegroundWindow(ASFW_ANY);
            (void)tools3000::core::WinUtils::activateWindowForeground(targetRootHwnd);
        }
        if (terminalKind == TerminalPasteKind::NotTerminal) {
            sendVirtualDesktopClickAt(dropScreenPt);
        }

        // 3. 确认目标窗口真正位于前台，并留出短暂焦点稳定期
        if (targetRootHwnd && IsWindow(targetRootHwnd)) {
            for (DWORD waited = 0; waited < kMaxFgWaitMs; waited += kPollStepMs) {
                if (ClipboardManager::isTargetForegroundActive(targetRootHwnd)) {
                    break;
                }
                Sleep(kPollStepMs);
            }
        }
        Sleep(45);

        // 安全守卫：若前台意外仍停留在剪贴板窗自身，中止按键投递防止误贴回自身搜索框
        const HWND fgNow = GetForegroundWindow();
        if (fgNow && sourceWindowMatches(fgNow, keepVisibleClipboardHwnd)) {
            return;
        }

        // 4. 释放残留修饰键，附带真实硬件扫描码发送对应粘贴组合键
        std::vector<INPUT> inputs;
        inputs.reserve(12);
        auto appendKey = [&inputs](WORD vk, DWORD flags) {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = vk;
            in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            if (vk == VK_INSERT || vk == VK_RCONTROL || vk == VK_RMENU) {
                in.ki.dwFlags = flags | KEYEVENTF_EXTENDEDKEY;
            } else {
                in.ki.dwFlags = flags;
            }
            inputs.push_back(in);
        };

        if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) appendKey(VK_MENU, KEYEVENTF_KEYUP);
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) appendKey(VK_SHIFT, KEYEVENTF_KEYUP);
        if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) appendKey(VK_LWIN, KEYEVENTF_KEYUP);
        if ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) appendKey(VK_RWIN, KEYEVENTF_KEYUP);

        switch (terminalKind) {
            case TerminalPasteKind::ModernCtrlShiftV:
                appendKey(VK_CONTROL, 0);
                appendKey(VK_SHIFT, 0);
                appendKey('V', 0);
                appendKey('V', KEYEVENTF_KEYUP);
                appendKey(VK_SHIFT, KEYEVENTF_KEYUP);
                appendKey(VK_CONTROL, KEYEVENTF_KEYUP);
                break;
            case TerminalPasteKind::ShiftInsert:
                appendKey(VK_SHIFT, 0);
                appendKey(VK_INSERT, 0);
                appendKey(VK_INSERT, KEYEVENTF_KEYUP);
                appendKey(VK_SHIFT, KEYEVENTF_KEYUP);
                break;
            case TerminalPasteKind::LegacyCtrlV:
            case TerminalPasteKind::NotTerminal:
            default:
                appendKey(VK_CONTROL, 0);
                appendKey('V', 0);
                appendKey('V', KEYEVENTF_KEYUP);
                appendKey(VK_CONTROL, KEYEVENTF_KEYUP);
                break;
        }
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));

        // 5. 粘贴完成后确保剪贴板窗口无激活浮于目标窗之上（不抢焦点，也不被盖住）
        if (keepVisibleClipboardHwnd && IsWindow(keepVisibleClipboardHwnd) && IsWindowVisible(keepVisibleClipboardHwnd)) {
            Sleep(45);
            SetWindowPos(
                keepVisibleClipboardHwnd,
                HWND_TOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }).detach();
}

bool decodePngToDibAndRaw(
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
    struct ComGuard {
        bool owned = false;
        ~ComGuard() {
            if (owned) CoUninitialize();
        }
    } guard{comOwned};

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf())))) {
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.GetAddressOf()))) return false;
    if (FAILED(stream->InitializeFromMemory(outRawPng.data(), static_cast<DWORD>(outRawPng.size())))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;

    UINT w = 0;
    UINT h = 0;
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

class FormatEtcEnumerator final : public IEnumFORMATETC {
public:
    FormatEtcEnumerator(std::vector<FORMATETC> formats, size_t index = 0)
        : m_formats(std::move(formats)), m_index(index) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *ppv = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG next = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (next == 0) {
            delete this;
        }
        return next;
    }

    STDMETHODIMP Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override {
        if (!rgelt) return E_INVALIDARG;
        if (celt > 1 && !pceltFetched) return E_INVALIDARG;

        ULONG fetched = 0;
        while (fetched < celt && m_index < m_formats.size()) {
            rgelt[fetched] = m_formats[m_index];
            ++fetched;
            ++m_index;
        }
        if (pceltFetched) {
            *pceltFetched = fetched;
        }
        return (fetched == celt) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Skip(ULONG celt) override {
        if (m_index + celt > m_formats.size()) {
            m_index = m_formats.size();
            return S_FALSE;
        }
        m_index += celt;
        return S_OK;
    }

    STDMETHODIMP Reset() override {
        m_index = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumFORMATETC** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new (std::nothrow) FormatEtcEnumerator(m_formats, m_index);
        return (*ppEnum != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

private:
    ~FormatEtcEnumerator() = default;

    std::atomic<ULONG> m_refCount{1};
    std::vector<FORMATETC> m_formats;
    size_t m_index = 0;
};

class ClipboardDragDataObject final : public IDataObject {
public:
    explicit ClipboardDragDataObject(NativeDragPayload payload)
        : m_payload(std::move(payload)) {
        auto addFormat = [this](CLIPFORMAT cf) {
            if (cf == 0) return;
            FORMATETC fe{};
            fe.cfFormat = cf;
            fe.ptd = nullptr;
            fe.dwAspect = DVASPECT_CONTENT;
            fe.lindex = -1;
            fe.tymed = TYMED_HGLOBAL;
            m_formats.push_back(fe);
        };

        if (!m_payload.filePaths.empty()) {
            addFormat(CF_HDROP);
        }
        if (!m_payload.dibBytes.empty()) {
            addFormat(CF_DIB);
        }
        if (!m_payload.pngBytes.empty()) {
            addFormat(static_cast<CLIPFORMAT>(getPngClipboardFormat()));
        }
        // 纯文本格式优先排在 HTML Format 之前，确保 WinRT DataPackageView / 终端 / 代码编辑器优先消费纯净文本
        if (!m_payload.unicodeText.empty()) {
            addFormat(CF_UNICODETEXT);
            addFormat(CF_TEXT);
        }
        if (!m_payload.htmlBytes.empty()) {
            addFormat(static_cast<CLIPFORMAT>(getHtmlClipboardFormat()));
        }
    }

    bool wasAnyDataRetrieved() const noexcept {
        return m_anyDataRetrieved.load(std::memory_order_acquire);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG next = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (next == 0) {
            delete this;
        }
        return next;
    }

    STDMETHODIMP GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override {
        if (!pformatetcIn || !pmedium) return E_INVALIDARG;
        std::memset(pmedium, 0, sizeof(STGMEDIUM));

        const HRESULT checkHr = validateFormatEtc(pformatetcIn);
        if (FAILED(checkHr)) return checkHr;

        HGLOBAL hMem = nullptr;
        const UINT htmlFmt = getHtmlClipboardFormat();
        const UINT pngFmt = getPngClipboardFormat();

        if (pformatetcIn->cfFormat == CF_UNICODETEXT) {
            hMem = allocUnicodeTextGlobal(m_payload.unicodeText);
        } else if (pformatetcIn->cfFormat == CF_TEXT) {
            hMem = allocAnsiTextGlobal(m_payload.unicodeText);
        } else if (pformatetcIn->cfFormat == CF_HDROP) {
            hMem = allocDropFilesGlobal(m_payload.filePaths);
        } else if (pformatetcIn->cfFormat == CF_DIB) {
            hMem = allocGlobalBytes(m_payload.dibBytes.data(), m_payload.dibBytes.size());
        } else if (htmlFmt != 0 && pformatetcIn->cfFormat == static_cast<CLIPFORMAT>(htmlFmt)) {
            hMem = allocNullTerminatedBytes(m_payload.htmlBytes);
        } else if (pngFmt != 0 && pformatetcIn->cfFormat == static_cast<CLIPFORMAT>(pngFmt)) {
            hMem = allocGlobalBytes(m_payload.pngBytes.data(), m_payload.pngBytes.size());
        }

        if (!hMem) return STG_E_MEDIUMFULL;
        pmedium->tymed = TYMED_HGLOBAL;
        pmedium->hGlobal = hMem;
        pmedium->pUnkForRelease = nullptr;
        m_anyDataRetrieved.store(true, std::memory_order_release);
        return S_OK;
    }

    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }

    STDMETHODIMP QueryGetData(FORMATETC* pformatetc) override {
        if (!pformatetc) return E_INVALIDARG;
        return validateFormatEtc(pformatetc);
    }

    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* pformatectIn, FORMATETC* pformatetcOut) override {
        if (!pformatectIn || !pformatetcOut) return E_INVALIDARG;
        *pformatetcOut = *pformatectIn;
        pformatetcOut->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }

    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override {
        if (!ppenumFormatEtc) return E_POINTER;
        *ppenumFormatEtc = nullptr;
        if (dwDirection != DATADIR_GET) {
            return E_NOTIMPL;
        }
        *ppenumFormatEtc = new (std::nothrow) FormatEtcEnumerator(m_formats, 0);
        return (*ppenumFormatEtc != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    STDMETHODIMP DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~ClipboardDragDataObject() = default;

    HRESULT validateFormatEtc(const FORMATETC* fe) const noexcept {
        if (!fe) return E_INVALIDARG;
        if (fe->dwAspect != DVASPECT_CONTENT && fe->dwAspect != 0) {
            return DV_E_DVASPECT;
        }
        if (fe->lindex != -1 && fe->lindex != 0) {
            return DV_E_LINDEX;
        }
        if ((fe->tymed & TYMED_HGLOBAL) == 0) {
            return DV_E_TYMED;
        }
        for (const auto& supported : m_formats) {
            if (supported.cfFormat == fe->cfFormat) {
                return S_OK;
            }
        }
        return DV_E_FORMATETC;
    }

    std::atomic<ULONG> m_refCount{1};
    std::atomic<bool> m_anyDataRetrieved{false};
    NativeDragPayload m_payload;
    std::vector<FORMATETC> m_formats;
};

class ClipboardDropSource final : public IDropSource {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG next = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (next == 0) {
            delete this;
        }
        return next;
    }

    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed || (grfKeyState & MK_RBUTTON) != 0) {
            m_cancelledByUser.store(true, std::memory_order_release);
            return DRAGDROP_S_CANCEL;
        }
        if ((grfKeyState & MK_LBUTTON) == 0) {
            m_releasedLeftButton.store(true, std::memory_order_release);
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    STDMETHODIMP GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

    bool wasCancelledByUser() const noexcept {
        return m_cancelledByUser.load(std::memory_order_acquire);
    }

    bool wasReleasedLeftButton() const noexcept {
        return m_releasedLeftButton.load(std::memory_order_acquire);
    }

private:
    ~ClipboardDropSource() = default;

    std::atomic<ULONG> m_refCount{1};
    std::atomic<bool> m_cancelledByUser{false};
    std::atomic<bool> m_releasedLeftButton{false};
};

}  // namespace

TerminalPasteKind classifyTerminalPasteTarget(
    std::wstring_view rootClassName,
    std::wstring_view processExeLower) noexcept {
    if (equalsIgnoreCaseAsciiW(rootClassName, L"CASCADIA_HOSTING_WINDOW_CLASS") ||
        equalsIgnoreCaseAsciiW(rootClassName, L"org.wezfurlong.wezterm") ||
        equalsIgnoreCaseAsciiW(rootClassName, L"WarpTerminal")) {
        return TerminalPasteKind::ModernCtrlShiftV;
    }
    if (equalsIgnoreCaseAsciiW(rootClassName, L"ConsoleWindowClass") ||
        equalsIgnoreCaseAsciiW(rootClassName, L"VirtualConsoleClass")) {
        return TerminalPasteKind::LegacyCtrlV;
    }
    if (equalsIgnoreCaseAsciiW(rootClassName, L"mintty") ||
        equalsIgnoreCaseAsciiW(rootClassName, L"PuTTY")) {
        return TerminalPasteKind::ShiftInsert;
    }

    if (equalsIgnoreCaseAsciiW(processExeLower, L"windowsterminal.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"wt.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"openconsole.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"wezterm-gui.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"alacritty.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"kitty.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"warp.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"tabby.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"hyper.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"rio.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"ghostty.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"windterm.exe")) {
        return TerminalPasteKind::ModernCtrlShiftV;
    }
    if (equalsIgnoreCaseAsciiW(processExeLower, L"conhost.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"cmd.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"powershell.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"pwsh.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"conemu64.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"conemu.exe")) {
        return TerminalPasteKind::LegacyCtrlV;
    }
    if (equalsIgnoreCaseAsciiW(processExeLower, L"mintty.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"putty.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"xshell.exe") ||
        equalsIgnoreCaseAsciiW(processExeLower, L"mobaxterm.exe")) {
        return TerminalPasteKind::ShiftInsert;
    }

    return TerminalPasteKind::NotTerminal;
}

bool buildNativeDragPayloadForItem(uint64_t itemId, NativeDragPayload& outPayload) {
    outPayload = NativeDragPayload{};
    if (itemId == 0) return false;

    auto& mgr = ClipboardManager::instance();
    const auto item = mgr.getItem(itemId);
    if (!item.is_object()) return false;

    const std::string kind = item.value("kind", "text");
    if (kind == "image") {
        const auto blobPath = mgr.resolveImageBlobPath(itemId);
        std::error_code ec;
        if (blobPath.empty() || !std::filesystem::exists(blobPath, ec)) {
            return false;
        }
        outPayload.filePaths.push_back(blobPath.wstring());
        (void)decodePngToDibAndRaw(blobPath, outPayload.dibBytes, outPayload.pngBytes);
        return outPayload.hasAnyFormat();
    }

    if (kind == "files") {
        const std::string text = item.value("text", "");
        std::istringstream iss(text);
        std::string line;
        std::wstring joinedText;
        while (std::getline(iss, line)) {
            const std::string trimmed = trimAsciiCopy(line);
            if (trimmed.empty()) continue;
            const std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(trimmed);
            if (!wpath.empty()) {
                outPayload.filePaths.push_back(wpath);
                if (!joinedText.empty()) joinedText.push_back(L'\n');
                joinedText.append(wpath);
            }
        }
        // 同时提供 CF_UNICODETEXT 路径文本，确保拖入文本输入框（如设置页程序路径）同样自动填入
        outPayload.unicodeText = std::move(joinedText);
        return outPayload.hasAnyFormat();
    }

    // text / color
    const std::string text = item.value("text", "");
    if (text.empty()) return false;
    outPayload.unicodeText = tools3000::core::WinUtils::utf8ToWstring(text);

    const std::string htmlFile = item.value("htmlFile", "");
    if (isSafeRelativeBlobName(htmlFile)) {
        const auto htmlPath = mgr.getBlobsDir() / tools3000::core::WinUtils::utf8ToWstring(htmlFile);
        std::error_code ec;
        if (std::filesystem::exists(htmlPath, ec)) {
            const auto sz = std::filesystem::file_size(htmlPath, ec);
            if (!ec && sz > 0 && sz <= kMaxDragHtmlBytes) {
                std::ifstream ifs(htmlPath, std::ios::binary);
                if (ifs) {
                    outPayload.htmlBytes.resize(static_cast<size_t>(sz));
                    ifs.read(reinterpret_cast<char*>(outPayload.htmlBytes.data()),
                             static_cast<std::streamsize>(sz));
                }
            }
        }
    }
    return outPayload.hasAnyFormat();
}

ComPtr<IDataObject> createNativeDragDataObject(const NativeDragPayload& payload) {
    ComPtr<IDataObject> out;
    if (!payload.hasAnyFormat()) return out;
    out.Attach(new (std::nothrow) ClipboardDragDataObject(payload));
    return out;
}

ComPtr<IDropSource> createNativeDropSource() {
    ComPtr<IDropSource> out;
    out.Attach(new (std::nothrow) ClipboardDropSource());
    return out;
}

NativeDragOutcome startNativeDragForItem(uint64_t itemId, HWND sourceClipboardHwnd) {
    NativeDragOutcome outcome{};
    if (itemId == 0) return outcome;
    // 若异步派发到达 UI 线程时左键已松开，直接安全退出避免空挂拖拽状态
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
        LOG_DEBUG("ClipboardNativeDrag: skipped item {} because left mouse button is already released", itemId);
        return outcome;
    }

    NativeDragPayload payload;
    if (!buildNativeDragPayloadForItem(itemId, payload)) {
        LOG_WARN("ClipboardNativeDrag: failed to build payload for item {}", itemId);
        return outcome;
    }

    const HRESULT oleHr = OleInitialize(nullptr);
    const bool oleOwned = SUCCEEDED(oleHr);
    if (!oleOwned) {
        LOG_WARN("ClipboardNativeDrag: OleInitialize failed, hr=0x{:08X}", static_cast<unsigned>(oleHr));
        return outcome;
    }
    struct OleGuard {
        ~OleGuard() { OleUninitialize(); }
    } oleGuard;

    ComPtr<ClipboardDragDataObject> concreteDataObj;
    concreteDataObj.Attach(new (std::nothrow) ClipboardDragDataObject(payload));
    ComPtr<ClipboardDropSource> dropSource;
    dropSource.Attach(new (std::nothrow) ClipboardDropSource());
    if (!concreteDataObj || !dropSource) {
        return outcome;
    }

    ReleaseCapture();

    DWORD dwEffect = DROPEFFECT_NONE;
    const HRESULT dragHr = DoDragDrop(
        concreteDataObj.Get(),
        dropSource.Get(),
        DROPEFFECT_COPY | DROPEFFECT_LINK,
        &dwEffect);

    POINT dropPt{};
    GetCursorPos(&dropPt);
    outcome.dropScreenPt = dropPt;

    bool inClientArea = false;
    TerminalPasteKind terminalKind = TerminalPasteKind::NotTerminal;
    HWND targetRoot = resolveSafeExternalDropTarget(dropPt, sourceClipboardHwnd, inClientArea, terminalKind);
    outcome.targetRootHwnd = targetRoot;

    const bool oleRetrieved = concreteDataObj->wasAnyDataRetrieved();

    LOG_DEBUG("ClipboardNativeDrag: DoDragDrop finished for item {} (hr=0x{:08X}, effect={}, oleRead={}, userCancel={}, relLBtn={}, targetInClient={}, termKind={})",
              itemId,
              static_cast<unsigned>(dragHr),
              static_cast<unsigned>(dwEffect),
              oleRetrieved,
              dropSource->wasCancelledByUser(),
              dropSource->wasReleasedLeftButton(),
              inClientArea,
              static_cast<unsigned>(terminalKind));

    if (!shouldFallbackToSmartDropPaste(
            dragHr,
            dwEffect,
            dropSource->wasCancelledByUser(),
            dropSource->wasReleasedLeftButton(),
            targetRoot != nullptr && inClientArea,
            oleRetrieved)) {
        if (dragHr == DRAGDROP_S_DROP && dwEffect != DROPEFFECT_NONE && oleRetrieved) {
            outcome.status = NativeDragStatus::DroppedByOle;
        } else {
            outcome.status = NativeDragStatus::Cancelled;
        }
        return outcome;
    }

    // 第二引擎：跨 UIPI 权限隔离（如管理员 Tools3000 拖入普通权限 Clash Party / Windows Terminal / Electron）、
    // 假 Drop 空吞或代码编辑器选中态拒收时，自动执行落点智能直贴；对终端强制剥离富文本包装
    const auto settings = ClipboardManager::instance().getSettings();
    const bool forcePlainText =
        settings.plainTextByDefault || (terminalKind != TerminalPasteKind::NotTerminal);
    const bool written = ClipboardManager::instance().activateItem(
        itemId,
        forcePlainText,
        /*autoPaste=*/false,
        nullptr);
    if (written) {
        LOG_INFO("ClipboardNativeDrag: engaging smart drop-paste at ({}, {}) for item {} (termKind={})",
                 dropPt.x, dropPt.y, itemId, static_cast<unsigned>(terminalKind));
        dispatchSmartDropPasteAsync(targetRoot, dropPt, sourceClipboardHwnd, terminalKind);
        outcome.status = NativeDragStatus::DroppedBySmartPaste;
        return outcome;
    }

    outcome.status = NativeDragStatus::FailedOrSkipped;
    return outcome;
}

}  // namespace tools3000::clipboard

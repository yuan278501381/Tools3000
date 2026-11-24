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

LPARAM packScreenPointLParam(POINT pt) noexcept {
    const uint32_t ux = static_cast<uint16_t>(static_cast<int16_t>(std::clamp(pt.x, -32768L, 32767L)));
    const uint32_t uy = static_cast<uint16_t>(static_cast<int16_t>(std::clamp(pt.y, -32768L, 32767L)));
    return static_cast<LPARAM>(ux | (uy << 16));
}

bool isBrowserProcessName(std::wstring_view exeLower) noexcept {
    return equalsIgnoreCaseAsciiW(exeLower, L"chrome.exe") ||
           equalsIgnoreCaseAsciiW(exeLower, L"msedge.exe") ||
           equalsIgnoreCaseAsciiW(exeLower, L"brave.exe") ||
           equalsIgnoreCaseAsciiW(exeLower, L"vivaldi.exe") ||
           equalsIgnoreCaseAsciiW(exeLower, L"arc.exe") ||
           equalsIgnoreCaseAsciiW(exeLower, L"opera.exe") ||
           equalsIgnoreCaseAsciiW(exeLower, L"firefox.exe") ||
           equalsIgnoreCaseAsciiW(exeLower, L"zen.exe") ||
           equalsIgnoreCaseAsciiW(exeLower, L"waterfox.exe");
}

HWND resolveSafeExternalDropTarget(
    POINT screenPt,
    HWND sourceClipboardHwnd,
    bool& outInClientArea,
    TerminalPasteKind& outTerminalKind,
    bool& outIsMultiProcessWebHost) noexcept {
    outInClientArea = false;
    outTerminalKind = TerminalPasteKind::NotTerminal;
    outIsMultiProcessWebHost = false;

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

    const bool isChromiumWidget = equalsIgnoreCaseAsciiW(rootClsView, L"Chrome_WidgetWin_1");
    const bool isMozillaWidget = equalsIgnoreCaseAsciiW(rootClsView, L"MozillaWindowClass");
    outIsMultiProcessWebHost = isChromiumWidget || isMozillaWidget || isBrowserProcessName(exeLower);

    RECT rc{};
    if (!GetWindowRect(rootHwnd, &rc)) {
        return rootHwnd;
    }

    const float dpi = std::clamp(tools3000::core::dpi::scaleForWindow(rootHwnd), 1.0f, 4.0f);
    const LONG edgeMargin = static_cast<LONG>(std::lround(7.0f * dpi));
    const LONG topBarGuard = static_cast<LONG>(std::lround((isBrowserProcessName(exeLower) ? 40.0f : 38.0f) * dpi));
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

    const bool deepInsideBody = (screenPt.y > rc.top + topBarGuard);

    // Chromium / Firefox 顶层单窗口架构：只要已深入标签栏/标题栏下方（含浏览器地址栏与整个网页内容区），
    // 直接确认为有效客户区，彻底免疫跨进程 DComp 合成窗 WM_NCHITTEST 超时或自定义拖拽区误杀
    if ((isChromiumWidget || isMozillaWidget) && deepInsideBody) {
        outInClientArea = true;
        return rootHwnd;
    }

    const LPARAM hitLParam = packScreenPointLParam(screenPt);

    // 若 WindowFromPoint 直接命中了真实子内容渲染窗（如 Windows Terminal 的 InputSiteWindowClass），
    // 排除标题栏拖拽子窗后即确认为有效客户区
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
                    hitLParam,
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

    DWORD_PTR hitTest = HTCLIENT;
    if (SendMessageTimeoutW(
            rootHwnd,
            WM_NCHITTEST,
            0,
            hitLParam,
            SMTO_ABORTIFHUNG | SMTO_BLOCK,
            60,
            &hitTest) != 0) {
        outInClientArea = isSafeDropHitTestResult(static_cast<LRESULT>(hitTest), deepInsideBody);
    } else {
        outInClientArea = deepInsideBody;
    }
    return rootHwnd;
}

void sendSegmentedDesktopClickAt(POINT screenPt) noexcept {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int vh = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    const double normX = static_cast<double>(screenPt.x - vx) * 65535.0 / static_cast<double>((std::max)(1, vw - 1));
    const double normY = static_cast<double>(screenPt.y - vy) * 65535.0 / static_cast<double>((std::max)(1, vh - 1));
    const LONG absX = std::clamp(static_cast<LONG>(std::lround(normX)), 0L, 65535L);
    const LONG absY = std::clamp(static_cast<LONG>(std::lround(normY)), 0L, 65535L);

    auto emitMouseFlag = [absX, absY](DWORD flag) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = absX;
        in.mi.dy = absY;
        in.mi.dwFlags = flag | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        SendInput(1u, &in, sizeof(INPUT));
    };

    // 分步物理点击节奏：先刷新 Chromium InputRouter 跨进程悬停命中测试，再触发完整 pointerdown -> mouseup -> click 状态机，
    // 确保 Chrome 新标签页 Fakebox、「问问 Google」动态展开搜索框及各类富文本编辑器 100% 挂载 DOM 焦点
    emitMouseFlag(MOUSEEVENTF_MOVE);
    Sleep(10);
    emitMouseFlag(MOUSEEVENTF_LEFTDOWN);
    Sleep(14);
    emitMouseFlag(MOUSEEVENTF_LEFTUP);
}

void dispatchSmartDropPasteAsync(
    HWND targetRootHwnd,
    POINT dropScreenPt,
    HWND keepVisibleClipboardHwnd,
    TerminalPasteKind terminalKind,
    bool isMultiProcessWebHost) {
    // 五步确定性聚焦与粘贴管线：
    //   1. 等待物理左键完全抬起；
    //   2. 先激活目标窗口并等待前台交接完成 + 消化 WM_ACTIVATE 默认旧焦点恢复（杜绝激活反吞点击焦点）；
    //   3. 非终端窗口执行分步物理单击，精准唤醒网页/编辑器动态输入框焦点；
    //   4. 多进程 Web 宿主留出 95ms 跨进程 DOM 聚焦窗口（原生控件 45ms）；
    //   5. 发送硬件扫描码粘贴热键并无激活保活剪贴板面板。
    std::thread([targetRootHwnd, dropScreenPt, keepVisibleClipboardHwnd, terminalKind, isMultiProcessWebHost]() {
        constexpr DWORD kPollStepMs = 10;
        constexpr DWORD kMaxFgWaitMs = 190;

        for (int i = 0; i < 8 && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; ++i) {
            Sleep(8);
        }

        const bool wasAlreadyForeground =
            targetRootHwnd && IsWindow(targetRootHwnd) &&
            ClipboardManager::isTargetForegroundActive(targetRootHwnd);

        if (targetRootHwnd && IsWindow(targetRootHwnd) && !wasAlreadyForeground) {
            AllowSetForegroundWindow(ASFW_ANY);
            (void)tools3000::core::WinUtils::activateWindowForeground(targetRootHwnd);
            for (DWORD waited = 0; waited < kMaxFgWaitMs; waited += kPollStepMs) {
                if (ClipboardManager::isTargetForegroundActive(targetRootHwnd)) {
                    break;
                }
                Sleep(kPollStepMs);
            }
            // 让目标窗口（特别是 Chrome / Electron）先完成 WM_ACTIVATE 记住的上次控件焦点恢复，
            // 随后我们的物理点击才能稳稳将最终焦点落到目标输入框上而不被覆盖
            Sleep(28);
        }

        if (terminalKind == TerminalPasteKind::NotTerminal) {
            sendSegmentedDesktopClickAt(dropScreenPt);
        }

        Sleep(isMultiProcessWebHost ? 95 : 45);

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

    void enableDynamicPlainTextHostGuard(HWND sourceClipboardHwnd) noexcept {
        m_sourceClipboardHwnd = sourceClipboardHwnd;
        m_guardNonUrlPlainText =
            m_payload.filePaths.empty() &&
            m_payload.dibBytes.empty() &&
            m_payload.pngBytes.empty() &&
            !m_payload.unicodeText.empty() &&
            !isSingleHttpUrlPayload(m_payload);
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
        if (m_guardNonUrlPlainText && isCursorOverAttachmentDropzoneHost()) {
            return DV_E_FORMATETC;
        }
        for (const auto& supported : m_formats) {
            if (supported.cfFormat == fe->cfFormat) {
                return S_OK;
            }
        }
        return DV_E_FORMATETC;
    }

    bool isCursorOverAttachmentDropzoneHost() const noexcept {
        POINT pt{};
        if (!GetCursorPos(&pt)) return false;
        HWND hit = WindowFromPoint(pt);
        if (!hit || !IsWindow(hit)) return false;
        HWND root = ClipboardManager::normalizeValidTargetHwnd(hit, m_sourceClipboardHwnd);
        if (!root || isDesktopShellWindow(root)) return false;
        const std::wstring procPath = tools3000::core::WinUtils::getProcessPathFromWindow(root);
        if (procPath.empty()) return false;
        const std::wstring exeLower =
            tools3000::core::WinUtils::toLower(std::filesystem::path(procPath).filename().wstring());
        return shouldBypassOlePlainTextDropForHost(exeLower);
    }

    std::atomic<ULONG> m_refCount{1};
    std::atomic<bool> m_anyDataRetrieved{false};
    bool m_guardNonUrlPlainText = false;
    HWND m_sourceClipboardHwnd = nullptr;
    NativeDragPayload m_payload;
    std::vector<FORMATETC> m_formats;
};

class ClipboardDropSource final : public IDropSource {
public:
    explicit ClipboardDropSource(HWND sourceClipboardHwnd = nullptr) noexcept
        : m_sourceClipboardHwnd(sourceClipboardHwnd) {}

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

    STDMETHODIMP GiveFeedback(DWORD dwEffect) override {
        if (dwEffect == DROPEFFECT_NONE && m_sourceClipboardHwnd != nullptr) {
            // 当处于跨 UIPI（如拖向普通权限 Chrome/微信）或主动绕过 AI IDE 附件层时，
            // 若光标已位于合法外部应用客户区，展示直观的手型/十字准星而非刺眼的红色禁止圈
            POINT pt{};
            if (GetCursorPos(&pt)) {
                bool inClient = false;
                TerminalPasteKind termKind = TerminalPasteKind::NotTerminal;
                bool isWeb = false;
                if (resolveSafeExternalDropTarget(pt, m_sourceClipboardHwnd, inClient, termKind, isWeb) != nullptr &&
                    inClient) {
                    static HCURSOR s_copyLikeCursor = LoadCursorW(nullptr, IDC_ARROW);
                    if (s_copyLikeCursor) {
                        SetCursor(s_copyLikeCursor);
                        return S_OK;
                    }
                }
            }
        }
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
    HWND m_sourceClipboardHwnd = nullptr;
};

}  // namespace

bool isSingleHttpUrlPayload(const NativeDragPayload& payload) noexcept {
    if (!payload.filePaths.empty() || !payload.dibBytes.empty() || !payload.pngBytes.empty()) {
        return false;
    }
    std::wstring_view sv(payload.unicodeText);
    while (!sv.empty() && (sv.front() == L' ' || sv.front() == L'\t' || sv.front() == L'\r' || sv.front() == L'\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == L' ' || sv.back() == L'\t' || sv.back() == L'\r' || sv.back() == L'\n')) {
        sv.remove_suffix(1);
    }
    if (sv.size() < 10u || sv.size() > 2048u) {
        return false;
    }
    for (wchar_t ch : sv) {
        if (ch <= 0x20 || ch == L'"' || ch == L'<' || ch == L'>') {
            return false;
        }
    }
    const bool isHttp = sv.size() > 7u && equalsIgnoreCaseAsciiW(sv.substr(0, 7), L"http://");
    const bool isHttps = sv.size() > 8u && equalsIgnoreCaseAsciiW(sv.substr(0, 8), L"https://");
    return isHttp || isHttps;
}

bool shouldBypassOlePlainTextDropForHost(std::wstring_view processExeLower) noexcept {
    return equalsIgnoreCaseAsciiW(processExeLower, L"antigravity.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"cursor.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"windsurf.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"trae.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"zed.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"claude.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"chatgpt.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"doubao.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"kimi.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"yuanbao.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"cherry-studio.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"chatbox.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"clash party.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"clash-verge.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"flclash.exe") ||
           equalsIgnoreCaseAsciiW(processExeLower, L"mihomo-party.exe");
}

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
    dropSource.Attach(new (std::nothrow) ClipboardDropSource(sourceClipboardHwnd));
    if (!concreteDataObj || !dropSource) {
        return outcome;
    }
    concreteDataObj->enableDynamicPlainTextHostGuard(sourceClipboardHwnd);

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
    bool isMultiProcessWebHost = false;
    HWND targetRoot = resolveSafeExternalDropTarget(
        dropPt, sourceClipboardHwnd, inClientArea, terminalKind, isMultiProcessWebHost);
    outcome.targetRootHwnd = targetRoot;

    const bool oleRetrieved = concreteDataObj->wasAnyDataRetrieved();

    LOG_DEBUG("ClipboardNativeDrag: DoDragDrop finished for item {} (hr=0x{:08X}, effect={}, oleRead={}, userCancel={}, relLBtn={}, targetInClient={}, termKind={}, webHost={})",
              itemId,
              static_cast<unsigned>(dragHr),
              static_cast<unsigned>(dwEffect),
              oleRetrieved,
              dropSource->wasCancelledByUser(),
              dropSource->wasReleasedLeftButton(),
              inClientArea,
              static_cast<unsigned>(terminalKind),
              isMultiProcessWebHost);

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

    // 第二引擎：跨 UIPI 权限隔离（如管理员 Tools3000 拖入普通权限 Chrome / 微信 / Clash Party / Windows Terminal）、
    // AI IDE 外层附件层纯文本防空吞、或代码编辑器选中态拒收时，自动执行确定性落点聚焦粘贴；对终端强制剥离富文本包装
    const auto settings = ClipboardManager::instance().getSettings();
    const bool forcePlainText =
        settings.plainTextByDefault || (terminalKind != TerminalPasteKind::NotTerminal);
    const bool written = ClipboardManager::instance().activateItem(
        itemId,
        forcePlainText,
        /*autoPaste=*/false,
        nullptr);
    if (written) {
        LOG_INFO("ClipboardNativeDrag: engaging smart drop-paste at ({}, {}) for item {} (termKind={}, webHost={})",
                 dropPt.x, dropPt.y, itemId, static_cast<unsigned>(terminalKind), isMultiProcessWebHost);
        dispatchSmartDropPasteAsync(
            targetRoot, dropPt, sourceClipboardHwnd, terminalKind, isMultiProcessWebHost);
        outcome.status = NativeDragStatus::DroppedBySmartPaste;
        return outcome;
    }

    outcome.status = NativeDragStatus::FailedOrSkipped;
    return outcome;
}

}  // namespace tools3000::clipboard

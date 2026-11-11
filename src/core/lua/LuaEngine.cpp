// ─────────────────────────────────────────────────────────────────────────────
// LuaEngine.cpp — Lua 脚本运行时实现
//
// 暴露给脚本的 API 命名空间 (对齐 implementation_plan.md §2.4):
//   easy.log       info / warn / error
//   easy.keyboard  sendKeys / keyDown / keyUp
//   easy.mouse     moveTo / click / scroll
//   easy.clipboard getText / setText
//   easy.shell     open / run / runAsync
//   easy.window    getForeground / getTitle / getClass / getProcess /
//                  minimize / maximize / restore / close / setTopmost
//   easy.screen    getPixelColor
//   easy.fs        readFile / writeFile / exists
//   easy.http      get / post
//   easy.ui        toast / notify / confirm / inputBox
//   easy.url       encode / decode
//
// 兼容旧脚本: 同时保留 tools3000.* 别名 (指向 easy 表)。
// ─────────────────────────────────────────────────────────────────────────────

#include "core/lua/LuaEngine.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "Tools3000Version.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace tools3000::core {

namespace {

// ── 键盘: 解析并发送组合键 (如 "Ctrl+Shift+T", "Ctrl+-", "Ctrl+=", "Ctrl++", "+") ──
bool isExtendedVirtualKey(WORD vk) noexcept {
    switch (vk) {
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LWIN:
        case VK_RWIN:
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_APPS:
        case VK_SNAPSHOT:
        case VK_DIVIDE:
        case VK_NUMLOCK:
            return true;
        default:
            return false;
    }
}

WORD virtualKeyFromToken(const std::string& tok) {
    static const std::unordered_map<std::string, WORD> kMap = {
        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5},
        {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10},
        {"F11", VK_F11}, {"F12", VK_F12}, {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15},
        {"F16", VK_F16}, {"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
        {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},
        {"TAB", VK_TAB}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN}, {"SPACE", VK_SPACE},
        {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE}, {"DELETE", VK_DELETE}, {"DEL", VK_DELETE},
        {"BACKSPACE", VK_BACK}, {"BACK", VK_BACK}, {"HOME", VK_HOME}, {"END", VK_END},
        {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
        {"PAGEUP", VK_PRIOR}, {"PGUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"PGDN", VK_NEXT},
        {"INSERT", VK_INSERT}, {"INS", VK_INSERT}, {"PRINTSCREEN", VK_SNAPSHOT},
        {"LWIN", VK_LWIN}, {"RWIN", VK_RWIN},
        {"-", VK_OEM_MINUS}, {"MINUS", VK_OEM_MINUS},
        {"+", VK_OEM_PLUS}, {"=", VK_OEM_PLUS}, {"PLUS", VK_OEM_PLUS}, {"EQUAL", VK_OEM_PLUS},
        {",", VK_OEM_COMMA}, {"COMMA", VK_OEM_COMMA},
        {".", VK_OEM_PERIOD}, {"PERIOD", VK_OEM_PERIOD},
        {"/", VK_OEM_2}, {"SLASH", VK_OEM_2},
        {";", VK_OEM_1}, {"SEMICOLON", VK_OEM_1},
        {"'", VK_OEM_7}, {"QUOTE", VK_OEM_7},
        {"[", VK_OEM_4}, {"LBRACKET", VK_OEM_4},
        {"]", VK_OEM_6}, {"RBRACKET", VK_OEM_6},
        {"\\", VK_OEM_5}, {"BACKSLASH", VK_OEM_5},
        {"`", VK_OEM_3}, {"TILDE", VK_OEM_3}, {"GRAVE", VK_OEM_3},
    };
    std::string up = WinUtils::toLower(tok);
    for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto it = kMap.find(up);
    if (it != kMap.end()) return it->second;
    if (up.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(up[0]);
        if (std::isalnum(ch)) {
            return static_cast<WORD>(ch);
        }
        const SHORT scanned = VkKeyScanW(static_cast<WCHAR>(ch));
        if (scanned != -1) {
            return static_cast<WORD>(LOBYTE(scanned));
        }
    }
    return 0;
}

std::vector<std::string> splitKeyComboTokens(const std::string& combo) {
    auto trimToken = [](const std::string& raw) -> std::string {
        const size_t b = raw.find_first_not_of(" \t");
        if (b == std::string::npos) return {};
        const size_t e = raw.find_last_not_of(" \t");
        return raw.substr(b, e - b + 1);
    };

    const std::string trimmed = trimToken(combo);
    if (trimmed.empty()) return {};
    if (trimmed == "+" || trimmed == "\\+") return {"+"};

    std::vector<std::string> tokens;
    std::string current;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        const char ch = trimmed[i];
        if (ch == '\\' && i + 1 < trimmed.size() && trimmed[i + 1] == '+') {
            current.push_back('+');
            ++i;
            continue;
        }
        if (ch == '+') {
            std::string tok = trimToken(current);
            current.clear();
            if (!tok.empty()) {
                tokens.push_back(std::move(tok));
            } else {
                // 连续 '+'（如 "Ctrl++"）或前导 '+' 表示主键自身为 '+'
                tokens.push_back("+");
            }
            continue;
        }
        current.push_back(ch);
    }
    std::string tail = trimToken(current);
    if (!tail.empty()) {
        tokens.push_back(std::move(tail));
    }
    return tokens;
}

void sendKeys(const std::string& combo) {
    constexpr uint8_t kLuaModCtrl  = 1 << 0;
    constexpr uint8_t kLuaModAlt   = 1 << 1;
    constexpr uint8_t kLuaModShift = 1 << 2;
    constexpr uint8_t kLuaModLWin  = 1 << 3;
    constexpr uint8_t kLuaModRWin  = 1 << 4;

    uint8_t mods = 0;
    WORD vk = 0;
    const auto tokens = splitKeyComboTokens(combo);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& tok = tokens[i];
        const std::string low = WinUtils::toLower(tok);
        const bool isLast = (i + 1 == tokens.size());
        if (!isLast && (low == "ctrl" || low == "control")) mods |= kLuaModCtrl;
        else if (!isLast && low == "alt") mods |= kLuaModAlt;
        else if (!isLast && low == "shift") mods |= kLuaModShift;
        else if (!isLast && (low == "win" || low == "lwin" || low == "super" || low == "meta")) mods |= kLuaModLWin;
        else if (!isLast && low == "rwin") mods |= kLuaModRWin;
        else vk = virtualKeyFromToken(tok);
    }
    if (vk == 0) {
        LOG_WARN("[Lua] keyboard.sendKeys 无法解析主键: {}", combo);
        return;
    }

    std::vector<INPUT> inputs;
    auto push = [&](WORD k, bool up) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = k;
        in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) |
                        (isExtendedVirtualKey(k) ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs.push_back(in);
    };
    static constexpr std::array<std::pair<uint8_t, WORD>, 5> kMods = {{
        {kLuaModCtrl, VK_CONTROL},
        {kLuaModAlt, VK_MENU},
        {kLuaModShift, VK_SHIFT},
        {kLuaModLWin, VK_LWIN},
        {kLuaModRWin, VK_RWIN},
    }};
    for (auto [m, k] : kMods) if (mods & m) push(k, false);
    push(vk, false);
    push(vk, true);
    for (auto it = kMods.rbegin(); it != kMods.rend(); ++it) if (mods & it->first) push(it->second, true);
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

// ── 剪贴板读取 (UTF-8，带 GlobalSize 边界保护) ────────────────────────────────
std::string clipboardGetText() {
    if (!OpenClipboard(nullptr)) return {};
    std::string result;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        const SIZE_T byteSize = GlobalSize(h);
        const size_t maxChars = byteSize / sizeof(wchar_t);
        if (maxChars > 0) {
            if (const auto* wptr = static_cast<const wchar_t*>(GlobalLock(h))) {
                const size_t len = wcsnlen(wptr, maxChars);
                result = WinUtils::wstringToUtf8(std::wstring(wptr, len));
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
    return result;
}

// ── 原生轻量输入对话框 (DialogBoxIndirectParamW 内存模板，零剪贴板触碰) ──────
struct LuaInputBoxContext {
    std::wstring prompt;
    std::wstring result;
    bool accepted = false;
};

constexpr WORD kIdInputPrompt = 1001;
constexpr WORD kIdInputEdit   = 1002;

INT_PTR CALLBACK luaInputBoxDlgProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(hwndDlg, GWLP_USERDATA, lParam);
            auto* ctx = reinterpret_cast<LuaInputBoxContext*>(lParam);
            SetWindowPos(hwndDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            if (ctx) {
                SetDlgItemTextW(hwndDlg, kIdInputPrompt, ctx->prompt.c_str());
            }
            HWND editHwnd = GetDlgItem(hwndDlg, kIdInputEdit);
            if (editHwnd) {
                SendMessageW(editHwnd, EM_SETLIMITTEXT, 4096, 0);
                SetFocus(editHwnd);
                return FALSE;
            }
            return TRUE;
        }
        case WM_COMMAND: {
            const WORD cmdId = LOWORD(wParam);
            if (cmdId == IDOK) {
                auto* ctx = reinterpret_cast<LuaInputBoxContext*>(
                    GetWindowLongPtrW(hwndDlg, GWLP_USERDATA));
                if (ctx) {
                    HWND editHwnd = GetDlgItem(hwndDlg, kIdInputEdit);
                    const int len = editHwnd ? GetWindowTextLengthW(editHwnd) : 0;
                    if (len > 0) {
                        std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
                        const int copied = GetWindowTextW(editHwnd, buf.data(), len + 1);
                        buf.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
                        ctx->result = std::move(buf);
                    }
                    ctx->accepted = true;
                }
                EndDialog(hwndDlg, IDOK);
                return TRUE;
            }
            if (cmdId == IDCANCEL) {
                EndDialog(hwndDlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            EndDialog(hwndDlg, IDCANCEL);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

std::string showWin32InputBoxDialog(const std::string& promptUtf8) {
    struct DialogBuffer {
        std::vector<BYTE> bytes;
        void alignDword() {
            while ((bytes.size() & 3u) != 0u) bytes.push_back(0);
        }
        void appendRaw(const void* data, size_t size) {
            const auto* p = static_cast<const BYTE*>(data);
            bytes.insert(bytes.end(), p, p + size);
        }
        void appendWord(WORD v) { appendRaw(&v, sizeof(v)); }
        void appendWString(const wchar_t* s) {
            const size_t count = (s ? wcslen(s) : 0) + 1;
            appendRaw(s ? s : L"", count * sizeof(wchar_t));
        }
        void addControl(DWORD style, short x, short y, short cx, short cy,
                        WORD id, WORD classAtom, const wchar_t* caption) {
            alignDword();
            DLGITEMTEMPLATE item{};
            item.style = style | WS_CHILD | WS_VISIBLE;
            item.dwExtendedStyle = 0;
            item.x = x;
            item.y = y;
            item.cx = cx;
            item.cy = cy;
            item.id = id;
            appendRaw(&item, sizeof(item));
            appendWord(0xFFFF);
            appendWord(classAtom);
            appendWString(caption);
            appendWord(0);  // 无附加创建数据
        }
    } tpl;

    tpl.bytes.reserve(512);
    DLGTEMPLATE dlg{};
    dlg.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlg.dwExtendedStyle = WS_EX_TOPMOST;
    dlg.cdit = 4;
    dlg.x = 0;
    dlg.y = 0;
    dlg.cx = 236;
    dlg.cy = 92;
    tpl.appendRaw(&dlg, sizeof(dlg));
    tpl.appendWord(0);  // menu
    tpl.appendWord(0);  // class
    tpl.appendWString(L"Tools3000");
    tpl.appendWord(9);  // font size
    tpl.appendWString(L"Segoe UI");

    // 0x0082 = Static, 0x0081 = Edit, 0x0080 = Button
    tpl.addControl(SS_LEFT, 10, 10, 216, 24, kIdInputPrompt, 0x0082, L"");
    tpl.addControl(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 10, 38, 216, 16, kIdInputEdit, 0x0081, L"");
    tpl.addControl(WS_TABSTOP | BS_DEFPUSHBUTTON, 116, 64, 52, 16, IDOK, 0x0080, L"OK");
    tpl.addControl(WS_TABSTOP | BS_PUSHBUTTON, 174, 64, 52, 16, IDCANCEL, 0x0080, L"Cancel");

    LuaInputBoxContext ctx;
    ctx.prompt = promptUtf8.empty() ? L"Please enter a value:" : WinUtils::utf8ToWstring(promptUtf8);
    const INT_PTR ret = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(tpl.bytes.data()),
        nullptr,
        &luaInputBoxDlgProc,
        reinterpret_cast<LPARAM>(&ctx));
    if (ret == IDOK && ctx.accepted) {
        return WinUtils::wstringToUtf8(ctx.result);
    }
    return {};
}

// ── 目标窗口: 显式句柄或默认前台窗口 ──────────────────────────────────────────
// 接受 sol::optional (绑定回调收到的类型); 在该 sol2 配置下它与 std::optional 不同型。
HWND resolveHwnd(sol::optional<int64_t> handle) {
    if (handle && *handle != 0) return reinterpret_cast<HWND>(static_cast<uintptr_t>(*handle));
    HWND fg = GetForegroundWindow();
    return fg ? GetAncestor(fg, GA_ROOT) : nullptr;
}

// ── URL 编码/解码 ─────────────────────────────────────────────────────────────
std::string urlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += (s[i] == '+') ? ' ' : s[i];
    }
    return out;
}

// ── 极简 HTTP 客户端 (WinHTTP)，返回 {status, body} ──────────────────────────
struct HttpResult {
    int status = 0;
    std::string body;
};

HttpResult httpRequest(const std::wstring& method, const std::string& url, const std::string& body) {
    HttpResult result;

    std::wstring wurl = WinUtils::utf8ToWstring(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]   = {};
    wchar_t path[2048]  = {};
    wchar_t extra[2048] = {};
    uc.lpszHostName   = host;   uc.dwHostNameLength   = ARRAYSIZE(host);
    uc.lpszUrlPath    = path;   uc.dwUrlPathLength    = ARRAYSIZE(path);
    uc.lpszExtraInfo  = extra;  uc.dwExtraInfoLength  = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        LOG_WARN("[Lua] http: 无效的 URL: {}", url);
        return result;
    }
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring object = std::wstring(path) + extra;  // 路径 + ?查询串

    const std::wstring userAgent = L"Tools3000/" + WinUtils::utf8ToWstring(tools3000::version::String);
    HINTERNET hSession = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;
    constexpr int NativeCallTimeoutMs = 5000;
    constexpr std::size_t MaxHttpResponseBytes = 4ull * 1024ull * 1024ull;
    WinHttpSetTimeouts(hSession, NativeCallTimeoutMs, NativeCallTimeoutMs,
                       NativeCallTimeoutMs, NativeCallTimeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    HINTERNET hRequest = nullptr;
    if (hConnect) {
        hRequest = WinHttpOpenRequest(hConnect, method.c_str(), object.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      https ? WINHTTP_FLAG_SECURE : 0);
    }

    if (hRequest) {
        BOOL ok;
        if (body.empty()) {
            ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        } else {
            static constexpr wchar_t kCtype[] = L"Content-Type: application/x-www-form-urlencoded\r\n";
            ok = WinHttpSendRequest(hRequest, kCtype, static_cast<DWORD>(-1L),
                                    const_cast<char*>(body.data()),
                                    static_cast<DWORD>(body.size()),
                                    static_cast<DWORD>(body.size()), 0);
        }

        if (ok && WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD statusCode = 0, len = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &len, WINHTTP_NO_HEADER_INDEX);
            result.status = static_cast<int>(statusCode);

            DWORD avail = 0;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
                if (result.body.size() > MaxHttpResponseBytes ||
                    static_cast<std::size_t>(avail) > MaxHttpResponseBytes - result.body.size()) {
                    LOG_WARN("[Lua] http 响应超过 {} 字节上限: {}", MaxHttpResponseBytes, url);
                    result.body.clear();
                    break;
                }
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (WinHttpReadData(hRequest, chunk.data(), avail, &read) && read > 0) {
                    result.body.append(chunk.data(), read);
                }
            } while (avail > 0);
        } else {
            LOG_WARN("[Lua] http 请求失败: {} (err={})", url, GetLastError());
        }
        WinHttpCloseHandle(hRequest);
    }
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

LuaEngine& LuaEngine::instance() {
    static LuaEngine inst;
    return inst;
}

bool LuaEngine::initialize() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_executing) {
        LOG_WARN("Rejected LuaEngine::initialize while a script is actively executing.");
        return false;
    }
    m_shutdownPending = false;
    try {
        m_lua = std::make_unique<sol::state>();
        // 沙箱: 仅开放安全标准库; 不开放 io / debug / package (杜绝 C-DLL 动态载入攻击)
        m_lua->open_libraries(sol::lib::base, sol::lib::string,
                              sol::lib::math, sol::lib::table, sol::lib::coroutine,
                              sol::lib::utf8, sol::lib::os);
        // 收紧 os: 保留 time/date/clock 等只读函数, 移除可改动系统状态的危险函数
        if (sol::optional<sol::table> os = (*m_lua)["os"]) {
            for (const char* fn : {"execute", "remove", "rename", "exit", "tmpname", "setlocale"}) {
                (*os)[fn] = sol::nil;
            }
        }
        bindApi();
        LOG_INFO("Lua 脚本引擎初始化成功 (easy.* API 已注入并启用细粒度沙箱权限防御)");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Lua 引擎初始化失败: {}", e.what());
        m_lua.reset();
        return false;
    }
}

namespace {

struct ExecutionGuard {
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::duration pausedDuration{};
    std::chrono::milliseconds timeout{5000};
    std::atomic<bool>* cancelToken = nullptr;
    LuaPermission permissions = LuaPermission::Standard;
};
static thread_local ExecutionGuard s_currentGuard;

struct ScopedGuardTimerPause {
    std::chrono::steady_clock::time_point pausedAt = std::chrono::steady_clock::now();
    ~ScopedGuardTimerPause() noexcept {
        s_currentGuard.pausedDuration += (std::chrono::steady_clock::now() - pausedAt);
    }
};

static void requirePermission(lua_State* L, LuaPermission perm, const char* name) {
    if (!hasPermission(s_currentGuard.permissions, perm)) {
        luaL_error(L, "权限不足: 该脚本未获得使用 %s 模块的授权", name);
    }
}

static void luaTimeoutHook(lua_State* L, lua_Debug*) {
    auto now = std::chrono::steady_clock::now();
    const auto activeElapsed = (now - s_currentGuard.startTime) - s_currentGuard.pausedDuration;
    if (s_currentGuard.timeout.count() > 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(activeElapsed) > s_currentGuard.timeout) {
        luaL_error(L, "Lua 脚本执行超时 (已触发沙箱安全中断)");
    }
    if (s_currentGuard.cancelToken && s_currentGuard.cancelToken->load(std::memory_order_relaxed)) {
        luaL_error(L, "Lua 脚本已被取消");
    }
}

}  // namespace

void LuaEngine::shutdown() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_executing) {
        m_shutdownPending = true;
        if (!m_execCv.wait_for(lock, std::chrono::milliseconds(500),
                               [this] { return !m_executing; })) {
            LOG_WARN("LuaEngine shutdown deferred VM teardown until modal script unwinds.");
            return;
        }
    }
    m_shutdownPending = false;
    m_lua.reset();
    LOG_INFO("Lua 脚本引擎已关闭");
}

bool LuaEngine::runProtected(const std::function<sol::protected_function_result()>& fn,
                             const char* context,
                             LuaPermission permissions,
                             std::chrono::milliseconds timeout,
                             std::atomic<bool>* cancelToken) {
    if (!m_lua) {
        LOG_ERROR("[Lua] 引擎未初始化, 无法执行 {}", context);
        return false;
    }
    lua_State* L = m_lua->lua_state();
    s_currentGuard.startTime = std::chrono::steady_clock::now();
    s_currentGuard.pausedDuration = {};
    s_currentGuard.timeout = timeout;
    s_currentGuard.cancelToken = cancelToken;
    s_currentGuard.permissions = permissions;

    // 每执行 1000 条 Lua 字节码指令检查一次超时与取消
    lua_sethook(L, luaTimeoutHook, LUA_MASKCOUNT, 1000);

    bool ok = false;
    try {
        sol::protected_function_result r = fn();
        if (!r.valid()) {
            sol::error err = r;
            LOG_ERROR("[Lua] {} 执行错误: {}", context, err.what());
        } else {
            ok = true;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[Lua] {} 运行时异常: {}", context, e.what());
    }

    lua_sethook(L, nullptr, 0, 0);
    s_currentGuard.cancelToken = nullptr;
    s_currentGuard.permissions = LuaPermission::Standard;
    return ok;
}

bool LuaEngine::executeScript(const std::string& script,
                              LuaPermission permissions,
                              std::chrono::milliseconds timeout,
                              std::atomic<bool>* cancelToken) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_shutdownPending) {
        return false;
    }
    if (m_executing) {
        if (m_modalDepth.load(std::memory_order_acquire) > 0) {
            LOG_WARN("[Lua] Another script is waiting on a modal dialog; skipping re-entrant execution.");
            return false;
        }
        const auto waitBudget = timeout.count() > 0 ? timeout : std::chrono::milliseconds(5000);
        if (!m_execCv.wait_for(lock, waitBudget, [this] { return !m_executing || m_shutdownPending; }) ||
            m_shutdownPending) {
            LOG_WARN("[Lua] Timed out or shutting down while waiting for previous script execution.");
            return false;
        }
    }
    if (!m_lua) {
        LOG_ERROR("[Lua] Engine not initialized; cannot run executeScript");
        return false;
    }

    m_executing = true;
    const bool ok = runProtected(
        [&] { return m_lua->safe_script(script, sol::script_pass_on_error); },
        "executeScript", permissions, timeout, cancelToken);
    m_executing = false;
    if (m_shutdownPending) {
        m_shutdownPending = false;
        m_lua.reset();
    }
    lock.unlock();
    m_execCv.notify_all();
    return ok;
}

bool LuaEngine::authorizeAndExecute(const std::string& script,
                                    const ScriptContext& context,
                                    std::chrono::milliseconds timeout,
                                    std::atomic<bool>* cancelToken) {
    LuaPermission effectivePerms = LuaPermission::Safe;

    if (context.requestedPerms == LuaPermission::None ||
        hasPermission(LuaPermission::Safe, context.requestedPerms)) {
        effectivePerms = LuaPermission::Safe;
    } else if (isScriptAuthorized(context.scriptId, context.requestedPerms)) {
        effectivePerms = context.requestedPerms;
    } else if (context.interactive) {
        // 模态弹窗向用户请求显式授权
        std::wstring title = L"Tools3000 脚本安全授权确认";
        std::wstring prompt = L"手势/脚本「" +
            WinUtils::utf8ToWstring(context.scriptName.empty() ? context.scriptId : context.scriptName) +
            L"」申请获取以下敏感操作权限：\n\n";

        std::vector<std::string> perms = formatLuaPermissions(context.requestedPerms);
        for (const auto& p : perms) {
            if (p == "keyboard") prompt += L"  • 模拟键盘输入\n";
            else if (p == "mouse") prompt += L"  • 模拟鼠标点击与移动\n";
            else if (p == "clipboard") prompt += L"  • 读写系统剪贴板\n";
            else if (p == "window") prompt += L"  • 控制其他窗口状态 (最小化/最大化/关闭)\n";
            else if (p == "screen") prompt += L"  • 读取屏幕像素数据\n";
            else if (p == "ui") prompt += L"  • 弹出交互对话框或通知\n";
            else if (p == "shell") prompt += L"  • 启动外部应用程序 (高风险)\n";
            else if (p == "fs") prompt += L"  • 读写本地磁盘文件 (高风险)\n";
            else if (p == "http") prompt += L"  • 发起网络 HTTP 请求 (高风险)\n";
        }
        prompt += L"\n是否信任并授权该脚本执行？(选择「是」将在本次运行期间记住授权)";

        int choice = MessageBoxW(nullptr, prompt.c_str(), title.c_str(),
                                 MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
        if (choice == IDYES) {
            grantPermissions(context.scriptId, context.requestedPerms);
            effectivePerms = context.requestedPerms;
            LOG_INFO("用户已明确授权脚本 {} 权限", context.scriptId);
        } else {
            LOG_WARN("用户拒绝授予脚本 {} 敏感权限，终止执行", context.scriptId);
            return false;
        }
    } else {
        LOG_WARN("脚本 {} 未经用户预授权，已自动限制在安全只读沙箱执行", context.scriptId);
        effectivePerms = LuaPermission::Safe;
    }

    return executeScript(script, effectivePerms, timeout, cancelToken);
}

bool LuaEngine::isScriptAuthorized(const std::string& scriptId, LuaPermission requiredPerms) {
    std::lock_guard<std::mutex> lock(m_authMutex);
    auto it = m_authorizedScripts.find(scriptId);
    if (it == m_authorizedScripts.end()) return false;
    return hasPermission(it->second, requiredPerms);
}

void LuaEngine::grantPermissions(const std::string& scriptId, LuaPermission perms) {
    std::lock_guard<std::mutex> lock(m_authMutex);
    m_authorizedScripts[scriptId] = m_authorizedScripts[scriptId] | perms;
}

void LuaEngine::revokePermissions(const std::string& scriptId) {
    std::lock_guard<std::mutex> lock(m_authMutex);
    m_authorizedScripts.erase(scriptId);
}

void LuaEngine::revokeAllPermissions() {
    std::lock_guard<std::mutex> lock(m_authMutex);
    m_authorizedScripts.clear();
}

bool LuaEngine::executeFile(const std::string& utf8Path,
                            LuaPermission permissions,
                            std::chrono::milliseconds timeout,
                            std::atomic<bool>* cancelToken) {
    std::ifstream f(WinUtils::utf8ToWstring(utf8Path).c_str(), std::ios::binary);
    if (!f) {
        LOG_ERROR("[Lua] 无法打开脚本文件: {}", utf8Path);
        return false;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    return executeScript(buf.str(), permissions, timeout, cancelToken);
}

// ── API 绑定 ──────────────────────────────────────────────────────────────────

void LuaEngine::bindApi() {
    sol::table easy = m_lua->create_named_table("easy");
    bindLog(easy);
    bindKeyboard(easy);
    bindMouse(easy);
    bindClipboard(easy);
    bindShell(easy);
    bindWindow(easy);
    bindScreen(easy);
    bindFs(easy);
    bindHttp(easy);
    bindUi(easy);
    bindUrl(easy);

    // 模块注册: tools3000 作为核心模块暴露
    (*m_lua)["tools3000"] = easy;
}

LuaPermission parseLuaPermissions(const std::vector<std::string>& permStrings) {
    LuaPermission result = LuaPermission::None;
    for (const auto& s : permStrings) {
        if (s == "log") result = result | LuaPermission::Log;
        else if (s == "url") result = result | LuaPermission::Url;
        else if (s == "window") result = result | LuaPermission::Window;
        else if (s == "ui") result = result | LuaPermission::Ui;
        else if (s == "keyboard") result = result | LuaPermission::Keyboard;
        else if (s == "mouse") result = result | LuaPermission::Mouse;
        else if (s == "clipboard") result = result | LuaPermission::Clipboard;
        else if (s == "screen") result = result | LuaPermission::Screen;
        else if (s == "shell") result = result | LuaPermission::Shell;
        else if (s == "fs") result = result | LuaPermission::Fs;
        else if (s == "http") result = result | LuaPermission::Http;
        else if (s == "safe") result = result | LuaPermission::Safe;
        else if (s == "standard") result = result | LuaPermission::Standard;
        else if (s == "all") result = result | LuaPermission::All;
    }
    return result;
}

std::vector<std::string> formatLuaPermissions(LuaPermission perms) {
    std::vector<std::string> res;
    if (hasPermission(perms, LuaPermission::Log)) res.push_back("log");
    if (hasPermission(perms, LuaPermission::Url)) res.push_back("url");
    if (hasPermission(perms, LuaPermission::Window)) res.push_back("window");
    if (hasPermission(perms, LuaPermission::Ui)) res.push_back("ui");
    if (hasPermission(perms, LuaPermission::Keyboard)) res.push_back("keyboard");
    if (hasPermission(perms, LuaPermission::Mouse)) res.push_back("mouse");
    if (hasPermission(perms, LuaPermission::Clipboard)) res.push_back("clipboard");
    if (hasPermission(perms, LuaPermission::Screen)) res.push_back("screen");
    if (hasPermission(perms, LuaPermission::Shell)) res.push_back("shell");
    if (hasPermission(perms, LuaPermission::Fs)) res.push_back("fs");
    if (hasPermission(perms, LuaPermission::Http)) res.push_back("http");
    return res;
}

void LuaEngine::bindLog(sol::table& easy) {
    sol::table t = easy.create_named("log");
    t.set_function("info",  [this](const std::string& m) {
        requirePermission(m_lua->lua_state(), LuaPermission::Log, "easy.log");
        LOG_INFO("[Lua] {}", m);
    });
    t.set_function("warn",  [this](const std::string& m) {
        requirePermission(m_lua->lua_state(), LuaPermission::Log, "easy.log");
        LOG_WARN("[Lua] {}", m);
    });
    t.set_function("error", [this](const std::string& m) {
        requirePermission(m_lua->lua_state(), LuaPermission::Log, "easy.log");
        LOG_ERROR("[Lua] {}", m);
    });
}

void LuaEngine::bindKeyboard(sol::table& easy) {
    sol::table t = easy.create_named("keyboard");
    t.set_function("sendKeys", [this](const std::string& combo) {
        requirePermission(m_lua->lua_state(), LuaPermission::Keyboard, "easy.keyboard");
        sendKeys(combo);
    });
    t.set_function("keyDown", [this](int vk) {
        requirePermission(m_lua->lua_state(), LuaPermission::Keyboard, "easy.keyboard");
        const WORD wvk = static_cast<WORD>(vk);
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = wvk;
        in.ki.dwFlags = isExtendedVirtualKey(wvk) ? KEYEVENTF_EXTENDEDKEY : 0;
        SendInput(1, &in, sizeof(INPUT));
    });
    t.set_function("keyUp", [this](int vk) {
        requirePermission(m_lua->lua_state(), LuaPermission::Keyboard, "easy.keyboard");
        const WORD wvk = static_cast<WORD>(vk);
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = wvk;
        in.ki.dwFlags = KEYEVENTF_KEYUP | (isExtendedVirtualKey(wvk) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof(INPUT));
    });
}

void LuaEngine::bindMouse(sol::table& easy) {
    sol::table t = easy.create_named("mouse");
    t.set_function("moveTo", [this](int x, int y) {
        requirePermission(m_lua->lua_state(), LuaPermission::Mouse, "easy.mouse");
        SetCursorPos(x, y);
    });
    t.set_function("click", [this](int button) {
        requirePermission(m_lua->lua_state(), LuaPermission::Mouse, "easy.mouse");
        DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
        if (button == 2)      { down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP; }
        else if (button == 3) { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
        INPUT in[2] = {};
        in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = down;
        in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = up;
        SendInput(2, in, sizeof(INPUT));
    });
    t.set_function("scroll", [this](int amount) {
        requirePermission(m_lua->lua_state(), LuaPermission::Mouse, "easy.mouse");
        INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = static_cast<DWORD>(amount * WHEEL_DELTA);
        SendInput(1, &in, sizeof(INPUT));
    });
}

void LuaEngine::bindClipboard(sol::table& easy) {
    sol::table t = easy.create_named("clipboard");
    t.set_function("getText", [this]() {
        requirePermission(m_lua->lua_state(), LuaPermission::Clipboard, "easy.clipboard");
        return clipboardGetText();
    });
    t.set_function("setText", [this](const std::string& s) {
        requirePermission(m_lua->lua_state(), LuaPermission::Clipboard, "easy.clipboard");
        return WinUtils::copyToClipboard(s);
    });
    t.set_function("captureSelected", [this]() {
        requirePermission(m_lua->lua_state(), LuaPermission::Clipboard, "easy.clipboard");
        requirePermission(m_lua->lua_state(), LuaPermission::Keyboard, "easy.keyboard");
        ScopedGuardTimerPause pauseGuard;
        return WinUtils::captureSelectedText();
    });
}

void LuaEngine::bindShell(sol::table& easy) {
    sol::table t = easy.create_named("shell");
    t.set_function("open", [this](const std::string& target) {
        requirePermission(m_lua->lua_state(), LuaPermission::Shell, "easy.shell");
        ShellExecuteW(nullptr, L"open", WinUtils::utf8ToWstring(target).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    });
    auto runImpl = [this](const std::string& cmd, bool wait) {
        requirePermission(m_lua->lua_state(), LuaPermission::Shell, "easy.shell");
        std::wstring wcmd = WinUtils::utf8ToWstring(cmd);
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // CreateProcessW 需要可写的命令行缓冲
        std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
        buf.push_back(L'\0');
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            if (wait) {
                ScopedGuardTimerPause pauseGuard;
                WaitForSingleObject(pi.hProcess, 5000);
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
        LOG_WARN("[Lua] shell.run 启动失败: {}", cmd);
        return false;
    };
    t.set_function("run",      [runImpl](const std::string& cmd) { return runImpl(cmd, true); });
    t.set_function("runAsync", [runImpl](const std::string& cmd) { return runImpl(cmd, false); });
}

void LuaEngine::bindWindow(sol::table& easy) {
    sol::table t = easy.create_named("window");
    t.set_function("getForeground", [this]() -> int64_t {
        requirePermission(m_lua->lua_state(), LuaPermission::Window, "easy.window");
        return static_cast<int64_t>(reinterpret_cast<uintptr_t>(resolveHwnd(sol::optional<int64_t>{})));
    });
    t.set_function("getTitle", [this](sol::optional<int64_t> h) {
        requirePermission(m_lua->lua_state(), LuaPermission::Window, "easy.window");
        return WinUtils::wstringToUtf8(WinUtils::getWindowTitle(resolveHwnd(h)));
    });
    t.set_function("getClass", [this](sol::optional<int64_t> h) {
        requirePermission(m_lua->lua_state(), LuaPermission::Window, "easy.window");
        return WinUtils::wstringToUtf8(WinUtils::getWindowClassName(resolveHwnd(h)));
    });
    t.set_function("getProcess", [this](sol::optional<int64_t> h) {
        requirePermission(m_lua->lua_state(), LuaPermission::Window, "easy.window");
        return WinUtils::getProcessNameFromWindow(resolveHwnd(h));
    });
    t.set_function("minimize", [this](sol::optional<int64_t> h) {
        requirePermission(m_lua->lua_state(), LuaPermission::Window, "easy.window");
        ShowWindow(resolveHwnd(h), SW_MINIMIZE);
    });
    t.set_function("maximize", [this](sol::optional<int64_t> h) {
        requirePermission(m_lua->lua_state(), LuaPermission::Window, "easy.window");
        ShowWindow(resolveHwnd(h), SW_MAXIMIZE);
    });
    t.set_function("restore",  [this](sol::optional<int64_t> h) {
        requirePermission(m_lua->lua_state(), LuaPermission::Window, "easy.window");
        ShowWindow(resolveHwnd(h), SW_RESTORE);
    });
    t.set_function("close",    [this](sol::optional<int64_t> h) {
        requirePermission(m_lua->lua_state(), LuaPermission::Window, "easy.window");
        if (HWND w = resolveHwnd(h)) PostMessageW(w, WM_CLOSE, 0, 0);
    });
    t.set_function("setTopmost", [this](bool topmost, sol::optional<int64_t> h) {
        requirePermission(m_lua->lua_state(), LuaPermission::Window, "easy.window");
        if (HWND w = resolveHwnd(h))
            SetWindowPos(w, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    });
}

void LuaEngine::bindScreen(sol::table& easy) {
    sol::table t = easy.create_named("screen");
    // 返回 {r, g, b}
    t.set_function("getPixelColor", [this](int x, int y) {
        requirePermission(m_lua->lua_state(), LuaPermission::Screen, "easy.screen");
        HDC hdc = GetDC(nullptr);
        COLORREF c = GetPixel(hdc, x, y);
        ReleaseDC(nullptr, hdc);
        sol::table rgb = m_lua->create_table();
        rgb["r"] = GetRValue(c);
        rgb["g"] = GetGValue(c);
        rgb["b"] = GetBValue(c);
        return rgb;
    });
}

void LuaEngine::bindFs(sol::table& easy) {
    sol::table t = easy.create_named("fs");
    t.set_function("exists", [this](const std::string& p) {
        requirePermission(m_lua->lua_state(), LuaPermission::Fs, "easy.fs");
        return std::filesystem::exists(WinUtils::utf8ToWstring(p));
    });
    t.set_function("readFile", [this](const std::string& p) -> std::string {
        requirePermission(m_lua->lua_state(), LuaPermission::Fs, "easy.fs");
        std::ifstream f(WinUtils::utf8ToWstring(p).c_str(), std::ios::binary);
        if (!f) return {};
        std::stringstream buf; buf << f.rdbuf();
        return buf.str();
    });
    t.set_function("writeFile", [this](const std::string& p, const std::string& content) {
        requirePermission(m_lua->lua_state(), LuaPermission::Fs, "easy.fs");
        std::ofstream f(WinUtils::utf8ToWstring(p).c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        return static_cast<bool>(f);
    });
}

void LuaEngine::bindHttp(sol::table& easy) {
    sol::table t = easy.create_named("http");
    t.set_function("get", [this](const std::string& url) {
        requirePermission(m_lua->lua_state(), LuaPermission::Http, "easy.http");
        HttpResult r = httpRequest(L"GET", url, {});
        sol::table out = m_lua->create_table();
        out["status"] = r.status;
        out["body"] = r.body;
        return out;
    });
    t.set_function("post", [this](const std::string& url, sol::optional<std::string> body) {
        requirePermission(m_lua->lua_state(), LuaPermission::Http, "easy.http");
        HttpResult r = httpRequest(L"POST", url, body.value_or(""));
        sol::table out = m_lua->create_table();
        out["status"] = r.status;
        out["body"] = r.body;
        return out;
    });
}

void LuaEngine::bindUi(sol::table& easy) {
    sol::table t = easy.create_named("ui");
    auto runModal = [this](auto&& modalFn) {
        ScopedGuardTimerPause pauseGuard;
        struct ScopedModalUnlock {
            std::mutex& mtx;
            std::atomic<uint32_t>& depth;
            ScopedModalUnlock(std::mutex& m, std::atomic<uint32_t>& d) noexcept : mtx(m), depth(d) {
                depth.fetch_add(1, std::memory_order_release);
                mtx.unlock();
            }
            ~ScopedModalUnlock() noexcept {
                mtx.lock();
                depth.fetch_sub(1, std::memory_order_release);
            }
        };
        auto ret = [&]() {
            ScopedModalUnlock unlockGuard(m_mutex, m_modalDepth);
            return modalFn();
        }();
        if (m_shutdownPending && m_lua) {
            luaL_error(m_lua->lua_state(), "Lua engine is shutting down");
        }
        return ret;
    };
    auto box = [runModal](const std::string& msg, const std::string& title, UINT flags) {
        const std::wstring wmsg = WinUtils::utf8ToWstring(msg);
        const std::wstring wtitle = WinUtils::utf8ToWstring(title);
        return runModal([&]() {
            return MessageBoxW(nullptr, wmsg.c_str(), wtitle.c_str(), flags | MB_TOPMOST);
        });
    };
    t.set_function("toast", [this, box](const std::string& msg) {
        requirePermission(m_lua->lua_state(), LuaPermission::Ui, "easy.ui");
        box(msg, "Tools3000", MB_OK | MB_ICONINFORMATION);
    });
    t.set_function("notify", [this, box](const std::string& title, const std::string& msg) {
        requirePermission(m_lua->lua_state(), LuaPermission::Ui, "easy.ui");
        box(msg, title, MB_OK | MB_ICONINFORMATION);
    });
    t.set_function("confirm", [this, box](const std::string& msg) {
        requirePermission(m_lua->lua_state(), LuaPermission::Ui, "easy.ui");
        return box(msg, "Tools3000", MB_YESNO | MB_ICONQUESTION) == IDYES;
    });
    // inputBox: 使用真实轻量 Win32 内存模板输入对话框，仅需 Ui 弹窗权限，绝不暗中读取剪贴板
    t.set_function("inputBox", [this, runModal](sol::optional<std::string> prompt) {
        requirePermission(m_lua->lua_state(), LuaPermission::Ui, "easy.ui");
        const std::string text = prompt.value_or("");
        return runModal([&]() { return showWin32InputBoxDialog(text); });
    });
}

void LuaEngine::bindUrl(sol::table& easy) {
    sol::table t = easy.create_named("url");
    t.set_function("encode", [this](const std::string& s) {
        requirePermission(m_lua->lua_state(), LuaPermission::Url, "easy.url");
        return urlEncode(s);
    });
    t.set_function("decode", [this](const std::string& s) {
        requirePermission(m_lua->lua_state(), LuaPermission::Url, "easy.url");
        return urlDecode(s);
    });
}

}  // namespace tools3000::core

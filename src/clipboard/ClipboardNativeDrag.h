#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClipboardNativeDrag.h — 混合双引擎 Win32 OLE + 落点智能直贴拖拽引擎
//
// 职责 (MECE 架构):
//   1. 第一引擎（原生 COM OLE DoDragDrop）：为文本/颜色/文件/图片条目构建标准
//      IDataObject + IEnumFORMATETC + IDropSource，提供 CF_UNICODETEXT / CF_TEXT /
//      HTML Format / CF_HDROP / CF_DIB / PNG 全格式簇；
//   2. 第二引擎（跨 UIPI / 选区拒收落点智能直贴 Smart Drop-Paste）：当高权限进程向
//      普通权限应用（如 Antigravity/Electron/Chrome/微信）拖拽被 UIPI 拦截、或代码
//      编辑器（如 Sublime Text）因目标文本处于选中态对 OLE 返回 DROPEFFECT_NONE 时，
//      在通过 4 重落点安全过滤（非自身/非任务栏/非桌面/非标题栏按钮）后，自动对落点
//      执行精准单击定位（同步解除旧选区）并无痕 Ctrl+V 投递，实现全桌面 100% 通吃。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CLIPBOARD_CLIPBOARDNATIVEDRAG_H
#define TOOLS3000_CLIPBOARD_CLIPBOARDNATIVEDRAG_H

#include <windows.h>
#include <objidl.h>
#include <oleidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::clipboard {

/// 原生拖拽结束状态（严格区分用户取消、OLE 成功与落点智能直贴）
enum class NativeDragStatus : uint8_t {
    FailedOrSkipped = 0,
    Cancelled = 1,
    DroppedByOle = 2,
    DroppedBySmartPaste = 3,
};

/// 拖拽执行结果上下文（供 ClipboardWindow 维持拖拽驻留态与刷新最新目标窗）
struct NativeDragOutcome {
    NativeDragStatus status = NativeDragStatus::FailedOrSkipped;
    HWND targetRootHwnd = nullptr;
    POINT dropScreenPt{0, 0};

    constexpr bool delivered() const noexcept {
        return status == NativeDragStatus::DroppedByOle ||
               status == NativeDragStatus::DroppedBySmartPaste;
    }
};

/// 原生 OLE 拖拽数据载荷（支持单元测试离线构建与验证）
struct NativeDragPayload {
    std::wstring unicodeText;            ///< CF_UNICODETEXT + CF_TEXT 回退
    std::vector<uint8_t> htmlBytes;      ///< HTML Format（可选富文本 UTF-8 字节块）
    std::vector<std::wstring> filePaths; ///< CF_HDROP（文件列表或图片本地路径）
    std::vector<uint8_t> dibBytes;       ///< CF_DIB（图片 32bpp bottom-up 位图数据）
    std::vector<uint8_t> pngBytes;       ///< 注册格式 "PNG"（图片无损原始字节）

    bool hasAnyFormat() const noexcept {
        return !unicodeText.empty() || !htmlBytes.empty() || !filePaths.empty() ||
               !dibBytes.empty() || !pngBytes.empty();
    }
};

/// 判定目标窗口是否属于命令行终端 / TUI 宿主（如 Windows Terminal / ConHost / WezTerm / Alacritty / Mintty）：
/// 终端宿主光标固定在命令行输入点，落点直贴时严禁发送虚拟鼠标左键单击（防止误入 QuickEdit 选区或打断 TUI），
/// 且强制剥离富文本包装并采用终端原生粘贴热键投递。
enum class TerminalPasteKind : uint8_t {
    NotTerminal = 0,
    ModernCtrlShiftV = 1, ///< Windows Terminal / WezTerm / Alacritty / Kitty / Warp / Tabby
    LegacyCtrlV = 2,      ///< 经典 ConsoleWindowClass (conhost / cmd / powershell)
    ShiftInsert = 3,      ///< Mintty / PuTTY / Git Bash / Xshell
};

TerminalPasteKind classifyTerminalPasteTarget(
    std::wstring_view rootClassName,
    std::wstring_view processExeLower) noexcept;

/// 判定拖拽载荷是否为单条合法 HTTP(S) 链接（不含文件/图片与内部空白）：
/// 对 AI IDE（如 Antigravity / Cursor）单条 HTTP(S) 链接保留原生 OLE 以生成 @Link 上下文卡片，
/// 而普通非 URL 纯文本绕过外层附件上传 DropZone 空吞陷阱，直达落点光标粘贴。
bool isSingleHttpUrlPayload(const NativeDragPayload& payload) noexcept;

/// 判定目标进程是否属于外层全局附件/文件 DropZone 会空吞纯文本的宿主：
/// - 对 AI IDE（Antigravity / Cursor 等）：单条 http(s):// 链接保留原生 OLE 以生成 @Link 胶囊，其余普通文本走落点粘贴；
/// - 对现代浏览器（Chrome / Edge / Firefox 等）：仅图片/文件走原生 OLE（触发网页「将文件拖放到此处」上传），
///   所有文本载荷一律绕过网页文件遮罩层并走确定性落点聚焦粘贴（同时避免拖网址误覆盖当前标签页）。
bool shouldBypassOleTextDropForHost(
    std::wstring_view processExeLower,
    bool payloadIsSingleHttpUrl) noexcept;

inline bool shouldBypassOlePlainTextDropForHost(std::wstring_view processExeLower) noexcept {
    return shouldBypassOleTextDropForHost(processExeLower, /*payloadIsSingleHttpUrl=*/false);
}

/// 判定 WM_NCHITTEST 返回值是否属于安全的客户区落点（严禁误点击标题栏/关闭按钮/菜单栏/边框）；
/// 对无边框自绘顶栏窗口（如 Electron / Clash Party），当落点深入窗口内部腹地且避开右上角窗口控制按钮禁区时，
/// 允许放行 HTCAPTION，消除外层 -webkit-app-region: drag 容器导致的输入框拖放误杀。
constexpr bool isSafeDropHitTestResult(
    LRESULT hitTest,
    bool deepInsideWindowBody = false) noexcept {
    if (hitTest == HTCAPTION) {
        return deepInsideWindowBody;
    }
    switch (hitTest) {
        case HTCLOSE:
        case HTMINBUTTON:
        case HTMAXBUTTON:
        case HTHELP:
        case HTSYSMENU:
        case HTMENU:
        case HTLEFT:
        case HTRIGHT:
        case HTTOP:
        case HTTOPLEFT:
        case HTTOPRIGHT:
        case HTBOTTOM:
        case HTBOTTOMLEFT:
        case HTBOTTOMRIGHT:
        case HTBORDER:
        case HTNOWHERE:
        case HTERROR:
            return false;
        default:
            return true;
    }
}

/// 纯函数判定：当 OLE DoDragDrop 结束后，是否应启用第二引擎（落点智能直贴）兜底。
/// 若目标窗口虽返回了非零 dwEffect 但全程未调用 IDataObject::GetData 读取任何有效格式
///（如 Electron 页面外层仅监听文件拖入导入配置而空吞纯文本 Drop），同样自动启用直贴兜底。
constexpr bool shouldFallbackToSmartDropPaste(
    HRESULT dragHr,
    DWORD dwEffect,
    bool cancelledByUser,
    bool releasedLeftButton,
    bool hasValidExternalClientTarget,
    bool oleDataActuallyRetrieved = true) noexcept {
    if (cancelledByUser || !releasedLeftButton || !hasValidExternalClientTarget) {
        return false;
    }
    // 仅当第一引擎 OLE 返回成功 Drop 且目标确实调用 GetData 取走了有效载荷时，才视为已被消费
    if (dragHr == DRAGDROP_S_DROP && dwEffect != DROPEFFECT_NONE && oleDataActuallyRetrieved) {
        return false;
    }
    return true;
}

/// 根据持久化剪贴板条目 ID 组装原生 OLE 拖拽载荷
bool buildNativeDragPayloadForItem(uint64_t itemId, NativeDragPayload& outPayload);

/// 根据载荷构造标准 COM IDataObject（完整支持 GetData / QueryGetData / EnumFormatEtc）
Microsoft::WRL::ComPtr<IDataObject> createNativeDragDataObject(const NativeDragPayload& payload);

/// 构造标准 COM IDropSource（左键释放即 Drop，按 Esc/右键取消，使用系统原生拖拽光标）
Microsoft::WRL::ComPtr<IDropSource> createNativeDropSource();

/// 在拥有窗口的 STA 主线程中启动混合双引擎拖拽（优先 OLE，跨 UIPI 或选区拒收时自动落点直贴）
NativeDragOutcome startNativeDragForItem(uint64_t itemId, HWND sourceClipboardHwnd = nullptr);

}  // namespace tools3000::clipboard

#endif  // TOOLS3000_CLIPBOARD_CLIPBOARDNATIVEDRAG_H

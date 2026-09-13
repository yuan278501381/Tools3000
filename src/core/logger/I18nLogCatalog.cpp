// ─────────────────────────────────────────────────────────────────────────────
// I18nLogCatalog.cpp — 世界级 0 内存分配日志多语言模板翻译映射中枢实现 (100% 纯英文覆盖)
// ─────────────────────────────────────────────────────────────────────────────

#include "core/logger/I18nLogCatalog.h"
#include "core/logger/Logger.h"

#include <unordered_map>
#include <string>

namespace tools3000::core {

struct LogEntry {
    const char* zhWithTrace;
    const char* enWithTrace;
};

static const std::unordered_map<std::string_view, LogEntry>& getCatalog() {
    static const std::unordered_map<std::string_view, LogEntry> catalog = {
        {"KeyboardHook: 激活录屏专属全量按键回显模式", {
            "[{}] KeyboardHook: 激活录屏专属全量按键回显模式",
            "[{}] KeyboardHook: activated recording exclusive full keycast mode"
        }},
        {"KeyboardHook: 恢复日常按键过滤模式", {
            "[{}] KeyboardHook: 恢复日常按键过滤模式",
            "[{}] KeyboardHook: restored normal keycast filter mode"
        }},
        {"KeycastOverlay: 激活录屏选区停靠回显 [{}, {}, {}, {}]", {
            "[{}] KeycastOverlay: 激活录屏选区停靠回显 [{}, {}, {}, {}]",
            "[{}] KeycastOverlay: activated recording docking keycast [{}, {}, {}, {}]"
        }},
        {"KeycastOverlay: 退出录屏选区停靠，恢复桌面悬浮回显", {
            "[{}] KeycastOverlay: 退出录屏选区停靠，恢复桌面悬浮回显",
            "[{}] KeycastOverlay: deactivated recording docking, restored desktop overlay"
        }},
        {"[RemoteMaster] 无有效远控目标窗口，安全跳过急救冲刷", {
            "[{}] [RemoteMaster] 无有效远控目标窗口，安全跳过急救冲刷",
            "[{}] [RemoteMaster] No valid remote target window found, safely skipping emergency flush"
        }},
        {"手势追踪超过 15 秒超时未完成，自动重置状态机: elapsed={}s", {
            "[{}] 手势追踪超过 15 秒超时未完成，自动重置状态机: elapsed={}s",
            "[{}] Gesture tracking exceeded 15s timeout without finishing, auto-resetting state machine: elapsed={}s"
        }},
        {"手势看门狗触发超时强制熔断，自动重置状态机: elapsed={}ms", {
            "[{}] 手势看门狗触发超时强制熔断，自动重置状态机: elapsed={}ms",
            "[{}] Gesture watchdog triggered timeout circuit breaker, auto-resetting state machine: elapsed={}ms"
        }},
        {"检测到系统进入休眠或睡眠 (0x{:X})，主动挂起渲染并释放物理内存", {
            "[{}] 检测到系统进入休眠或睡眠 (0x{:X})，主动挂起渲染并释放物理内存",
            "[{}] Detected system entering sleep or hibernate (0x{:X}), suspending rendering and trimming working set"
        }},
        {"检测到系统从休眠或睡眠中唤醒 (0x{:X})，防御性重置输入拦截状态", {
            "[{}] 检测到系统从休眠或睡眠中唤醒 (0x{:X})，防御性重置输入拦截状态",
            "[{}] Detected system resuming from sleep or hibernate (0x{:X}), defensively resetting input interception state"
        }},
        {"创建手势渲染唤醒事件失败", {
            "[{}] 创建手势渲染唤醒事件失败",
            "[{}] Failed to create gesture render wake event"
        }},
        {"滚轮切换虚拟桌面节流中 (间隔 {}ms < 120ms)，抑制过频切换", {
            "[{}] 滚轮切换虚拟桌面节流中 (间隔 {}ms < 120ms)，抑制过频切换",
            "[{}] Virtual desktop wheel switch throttled (interval {}ms < 120ms), suppressing excessive switching"
        }},
        {"目标为 Windows 桌面或系统外壳，拦截关窗动作，放弃按键注入: keys={}, target={}", {
            "[{}] 目标为 Windows 桌面或系统外壳，拦截关窗动作，放弃按键注入: keys={}, target={}",
            "[{}] Target is Windows desktop or shell window, intercepting close action and skipping key injection: keys={}, target={}"
        }},
        {"拦截对 Windows Shell / 桌面窗口的关闭请求: hwnd=0x{:X} class={} exe={}", {
            "[{}] 拦截对 Windows Shell / 桌面窗口的关闭请求: hwnd=0x{:X} class={} exe={}",
            "[{}] Intercepting close request for Windows Shell or desktop window: hwnd=0x{:X} class={} exe={}"
        }},
        {"创建聚光灯独立渲染线程唤醒事件失败", {
            "[{}] 创建聚光灯独立渲染线程唤醒事件失败",
            "[{}] Failed to create spotlight independent render thread wake event"
        }},
        {"创建聚光灯独立渲染线程就绪事件失败", {
            "[{}] 创建聚光灯独立渲染线程就绪事件失败",
            "[{}] Failed to create spotlight independent render thread ready event"
        }},
        {"聚光灯独立渲染线程或 DirectComposition 硬件上下文初始化失败", {
            "[{}] 聚光灯独立渲染线程或 DirectComposition 硬件上下文初始化失败",
            "[{}] Failed to initialize spotlight render thread or DirectComposition hardware context"
        }},
        {"鼠标演示与特效 Overlay 初始化完成 (独立专用渲染线程 + DirectComposition 硬件加速)", {
            "[{}] 鼠标演示与特效 Overlay 初始化完成 (独立专用渲染线程 + DirectComposition 硬件加速)",
            "[{}] Spotlight and mouse FX overlay initialized (dedicated render thread + DirectComposition hardware acceleration)"
        }},
        {"SpotlightOverlay: 独立专用渲染管线与硬件上下文已注销", {
            "[{}] SpotlightOverlay: 独立专用渲染管线与硬件上下文已注销",
            "[{}] SpotlightOverlay: Dedicated render pipeline and hardware context uninitialized"
        }},
        {"SpotlightOverlay: 聚光灯已完全隐藏并完成物理内存收缩 (DirectComposition GPU 直通)", {
            "[{}] SpotlightOverlay: 聚光灯已完全隐藏并完成物理内存收缩 (DirectComposition GPU 直通)",
            "[{}] SpotlightOverlay: Spotlight fully hidden and physical memory trimmed (DirectComposition GPU direct)"
        }},
        {"SpotlightOverlay 独立渲染线程未能成功建窗或绑定合成器", {
            "[{}] SpotlightOverlay 独立渲染线程未能成功建窗或绑定合成器",
            "[{}] SpotlightOverlay independent render thread failed to create window or bind compositor"
        }},
        {"SpotlightOverlay: D3D11CreateDevice 失败: hr=0x{:X}", {
            "[{}] SpotlightOverlay: D3D11CreateDevice 失败: hr=0x{:X}",
            "[{}] SpotlightOverlay: D3D11CreateDevice failed: hr=0x{:X}"
        }},
        {"EventBus: 取消订阅超时，可能存在未完成的在途调用, id={}", {
            "[{}] EventBus: 取消订阅超时，可能存在未完成的在途调用, id={}",
            "[{}] EventBus: Retire subscription timeout, pending calls remaining, id={}"
        }},
        {"SpotlightOverlay: D2D1CreateFactory 失败: hr=0x{:X}", {
            "[{}] SpotlightOverlay: D2D1CreateFactory 失败: hr=0x{:X}",
            "[{}] SpotlightOverlay: D2D1CreateFactory failed: hr=0x{:X}"
        }},
        {"SpotlightOverlay: DCompositionCreateDevice 失败: hr=0x{:X}", {
            "[{}] SpotlightOverlay: DCompositionCreateDevice 失败: hr=0x{:X}",
            "[{}] SpotlightOverlay: DCompositionCreateDevice failed: hr=0x{:X}"
        }},
        {"SpotlightOverlay: DirectComposition 挂载视觉树失败", {
            "[{}] SpotlightOverlay: DirectComposition 挂载视觉树失败",
            "[{}] SpotlightOverlay: DirectComposition visual tree attach failed"
        }},
        {"SpotlightOverlay: DirectComposition 硬件加速合成引擎就绪", {
            "[{}] SpotlightOverlay: DirectComposition 硬件加速合成引擎就绪",
            "[{}] SpotlightOverlay: DirectComposition hardware acceleration composition engine ready"
        }},
        {"SpotlightOverlay: DirectComposition CreateSurface 失败: hr=0x{:X}", {
            "[{}] SpotlightOverlay: DirectComposition CreateSurface 失败: hr=0x{:X}",
            "[{}] SpotlightOverlay: DirectComposition CreateSurface failed: hr=0x{:X}"
        }},
        {"SpotlightOverlay: DirectComposition BeginDraw 失败: hr=0x{:X}", {
            "[{}] SpotlightOverlay: DirectComposition BeginDraw 失败: hr=0x{:X}",
            "[{}] SpotlightOverlay: DirectComposition BeginDraw failed: hr=0x{:X}"
        }},
        {"SpotlightOverlay: 覆盖层组件及硬件渲染上下文已注销", {
            "[{}] SpotlightOverlay: 覆盖层组件及硬件渲染上下文已注销",
            "[{}] SpotlightOverlay: Overlay component and hardware render context uninitialized"
        }},
        {"SpotlightOverlay: 聚光灯已完全隐藏并完成物理内存收缩", {
            "[{}] SpotlightOverlay: 聚光灯已完全隐藏并完成物理内存收缩",
            "[{}] SpotlightOverlay: Spotlight fully hidden and physical memory trimmed"
        }},
        {"鼠标演示与特效 Overlay 初始化完成 (基于通用 MouseFeedbackOverlayBase 硬件加速管线)", {
            "[{}] 鼠标演示与特效 Overlay 初始化完成 (基于通用 MouseFeedbackOverlayBase 硬件加速管线)",
            "[{}] Mouse presentation and visual fx overlay initialized (based on MouseFeedbackOverlayBase hardware accelerated pipeline)"
        }},
        {"SpotlightOverlay: 聚光灯已触发, pos=({},{})", {
            "[{}] SpotlightOverlay: 聚光灯已触发, pos=({},{})",
            "[{}] SpotlightOverlay: Spotlight triggered, pos=({},{})"
        }},
        {"SpotlightOverlay: 聚光灯请求退出 (FadeOut)", {
            "[{}] SpotlightOverlay: 聚光灯请求退出 (FadeOut)",
            "[{}] SpotlightOverlay: Spotlight dismiss requested (FadeOut)"
        }},
        {"SpotlightOverlay: 聚光灯下点击鼠标退出 (button={})", {
            "[{}] SpotlightOverlay: 聚光灯下点击鼠标退出 (button={})",
            "[{}] SpotlightOverlay: Mouse clicked to dismiss spotlight (button={})"
        }},
        {"ShellContextMenuService: QueryContextMenu 返回 hr=0x{:08X}, itemsCount={}", {
            "[{}] ShellContextMenuService: QueryContextMenu 返回 hr=0x{:08X}, itemsCount={}",
            "[{}] ShellContextMenuService: QueryContextMenu returned hr=0x{:08X}, itemsCount={}"
        }},
        {"ShellContextMenuService: 检测到第三方 Shell 扩展执行超时 (500ms 熔断)，自动触发原生自愈降级菜单！", {
            "[{}] ShellContextMenuService: 检测到第三方 Shell 扩展执行超时 (500ms 熔断)，自动触发原生自愈降级菜单！",
            "[{}] ShellContextMenuService: Detected third-party shell extension timeout (500ms circuit breaker), activating fallback menu!"
        }},
        {"ShellContextMenuService: 即将弹出原生菜单, path={}, x={}, y={}, isFallback={}", {
            "[{}] ShellContextMenuService: 即将弹出原生菜单, path={}, x={}, y={}, isFallback={}",
            "[{}] ShellContextMenuService: About to pop context menu, path={}, x={}, y={}, isFallback={}"
        }},
        {"ShellContextMenuService: [Build __TIMESTAMP__] 即将调用 QueryContextMenu, flags=0x{:08X}...", {
            "[{}] ShellContextMenuService: [Build __TIMESTAMP__] 即将调用 QueryContextMenu, flags=0x{:08X}...",
            "[{}] ShellContextMenuService: [Build __TIMESTAMP__] About to call QueryContextMenu, flags=0x{:08X}..."
        }},
        {"ShellContextMenuService: 即将调用 QueryContextMenu...", {
            "[{}] ShellContextMenuService: 即将调用 QueryContextMenu...",
            "[{}] ShellContextMenuService: About to call QueryContextMenu..."
        }},
        {"ShellContextMenuService: 尝试直接从 Desktop IShellFolder 绑定绝对 PIDL 上下文菜单...", {
            "[{}] ShellContextMenuService: 尝试直接从 Desktop IShellFolder 绑定绝对 PIDL 上下文菜单...",
            "[{}] ShellContextMenuService: Attempting to bind absolute PIDL context menu directly from Desktop IShellFolder..."
        }},
        {"ShellContextMenuService: 尝试直接从文件夹自身视图绑定上下文菜单...", {
            "[{}] ShellContextMenuService: 尝试直接从文件夹自身视图绑定上下文菜单...",
            "[{}] ShellContextMenuService: Attempting to bind context menu directly from folder view..."
        }},
        {"ShellContextMenuService: 无法获取目标对象右键菜单 (文件或文件夹), path={}", {
            "[{}] ShellContextMenuService: 无法获取目标对象右键菜单 (文件或文件夹), path={}",
            "[{}] ShellContextMenuService: Failed to get context menu for target (file or folder), path={}"
        }},
        {"ShellContextMenuService: 开始解析路径 PIDL, path={}", {
            "[{}] ShellContextMenuService: 开始解析路径 PIDL, path={}",
            "[{}] ShellContextMenuService: Start parsing path PIDL, path={}"
        }},
        {"ShellContextMenuService: 无法将路径解析为 PIDL, path={}", {
            "[{}] ShellContextMenuService: 无法将路径解析为 PIDL, path={}",
            "[{}] ShellContextMenuService: Failed to parse path to PIDL, path={}"
        }},
        {"ShellContextMenuService: 开始解析路径 PIDL, path={}", {
            "[{}] ShellContextMenuService: 开始解析路径 PIDL, path={}",
            "[{}] ShellContextMenuService: Starting PIDL resolution for path, path={}"
        }},
        {"ShellContextMenuService: QueryContextMenu 返回 hr=0x{:08X}, itemsCount={}", {
            "[{}] ShellContextMenuService: QueryContextMenu 返回 hr=0x{:08X}, itemsCount={}",
            "[{}] ShellContextMenuService: QueryContextMenu returned hr=0x{:08X}, itemsCount={}"
        }},
        {"ShellContextMenuService: 即将弹出原生菜单, path={}, x={}, y={}, isFallback={}", {
            "[{}] ShellContextMenuService: 即将弹出原生菜单, path={}, x={}, y={}, isFallback={}",
            "[{}] ShellContextMenuService: About to show native shell menu, path={}, x={}, y={}, isFallback={}"
        }},
        {"ShellContextMenuService: 原生菜单已关闭, command={}", {
            "[{}] ShellContextMenuService: 原生菜单已关闭, command={}",
            "[{}] ShellContextMenuService: Native shell menu closed, command={}"
        }},
        {"ShellContextMenuService: 捕获第三方 Shell 扩展异常，已安全隔离防御", {
            "[{}] ShellContextMenuService: 捕获第三方 Shell 扩展异常，已安全隔离防御",
            "[{}] ShellContextMenuService: Caught 3rd-party shell extension exception, safely isolated"
        }},
        {"ShellContextMenuService: 检测到第三方 Shell 扩展执行超时 (300ms 熔断)，自动触发原生自愈降级菜单！", {
            "[{}] ShellContextMenuService: 检测到第三方 Shell 扩展执行超时 (300ms 熔断)，自动触发原生自愈降级菜单！",
            "[{}] ShellContextMenuService: 3rd-party shell extension timed out (300ms circuit breaker), auto-fallback to self-healing menu!"
        }},
        {"鼠标钩子执行耗时过长(当前耗时 {} ms), 触发安全熔断机制, 将暂停工作 {} ms 以保护系统响应", {
            "[{}] 鼠标钩子执行耗时过长(当前耗时 {} ms), 触发安全熔断机制, 将暂停工作 {} ms 以保护系统响应",
            "[{}] Mouse hook execution took too long ({} ms), triggering circuit breaker, pausing for {} ms to protect system responsiveness"
        }},
        {"SearchPlugin: 收到 showShellContextMenu 请求, filepath={}", {
            "[{}] SearchPlugin: 收到 showShellContextMenu 请求, filepath={}",
            "[{}] SearchPlugin: Received showShellContextMenu request, filepath={}"
        }},
        {"SearchPlugin: showShellContextMenu 调度结果 started={}", {
            "[{}] SearchPlugin: showShellContextMenu 调度结果 started={}",
            "[{}] SearchPlugin: showShellContextMenu dispatch result started={}"
        }},
        {"Tools3000 实时调试控制台已激活 (Debug Mode)", {
            "[{}] Tools3000 实时调试控制台已激活 (Debug Mode)",
            "[{}] Tools3000 Live Debug Console Activated (Debug Mode)"
        }},
        {"前台窗口为 Tools3000 自身界面，放行按键不触发手势", {
            "[{}] 前台窗口为 Tools3000 自身界面，放行按键不触发手势",
            "[{}] Foreground window is Tools3000 internal UI, pass-through button without gesture"
        }},
        {"光标所在窗口为 Tools3000 自身界面，放行按键不触发手势", {
            "[{}] 光标所在窗口为 Tools3000 自身界面，放行按键不触发手势",
            "[{}] Window under cursor is Tools3000 internal UI, pass-through button without gesture"
        }},
        {"DXGI 捕获不可用，回退 GDI: {}", {
            "[{}] DXGI 捕获不可用，回退 GDI: {}",
            "[{}] DXGI operation，operation GDI: {}"
        }},
        {"DXGI 捕获运行时失败，已切换 GDI: {}", {
            "[{}] DXGI 捕获运行时失败，已切换 GDI: {}",
            "[{}] DXGI operationfailed ，operation GDI: {}"
        }},
        {"CaptureHistory: 忽略空图像", {
            "[{}] CaptureHistory: 忽略空图像",
            "[{}] CaptureHistory: operation"
        }},
        {"CaptureHistory: 推入新截图, 尺寸={}x{}, 当前历史数={}", {
            "[{}] CaptureHistory: 推入新截图, 尺寸={}x{}, 当前历史数={}",
            "[{}] CaptureHistory: operation, operation={}x{}, operation={}"
        }},
        {"CaptureHistory: 已清空所有历史记录", {
            "[{}] CaptureHistory: 已清空所有历史记录",
            "[{}] CaptureHistory: operation"
        }},
        {"CaptureHistory: 设置最大历史数={}", {
            "[{}] CaptureHistory: 设置最大历史数={}",
            "[{}] CaptureHistory: operation={}"
        }},
        {"截图覆盖层 DPI 文本资源更新失败: dpi={}", {
            "[{}] 截图覆盖层 DPI 文本资源更新失败: dpi={}",
            "[{}] operation DPI operationfailed : dpi={}"
        }},
        {"CaptureOverlay 窗口过程异常: {}", {
            "[{}] CaptureOverlay 窗口过程异常: {}",
            "[{}] CaptureOverlay window operationexception : {}"
        }},
        {"CaptureOverlay 窗口过程未知异常", {
            "[{}] CaptureOverlay 窗口过程未知异常",
            "[{}] CaptureOverlay window operationexception"
        }},
        {"截图覆盖层窗口创建失败", {
            "[{}] 截图覆盖层窗口创建失败",
            "[{}] operationwindow create failed"
        }},
        {"截图覆盖层 Direct2D 初始化失败", {
            "[{}] 截图覆盖层 Direct2D 初始化失败",
            "[{}] operation Direct2D initialize failed"
        }},
        {"无法启动截图: 覆盖层窗口初始化失败", {
            "[{}] 无法启动截图: 覆盖层窗口初始化失败",
            "[{}] Failed to start screenshot: overlay window initialization failed"
        }},
        {"无法启动截图: 桌面底图捕获或上传失败", {
            "[{}] 无法启动截图: 桌面底图捕获或上传失败",
            "[{}] Failed to start screenshot: backdrop capture or upload failed"
        }},
        {"无法启动截图: 桌面底图上传失败", {
            "[{}] 无法启动截图: 桌面底图上传失败",
            "[{}] Failed to start screenshot: desktop backdrop upload failed"
        }},
        {"无法启动贴图编辑: 覆盖层窗口初始化失败", {
            "[{}] 无法启动贴图编辑: 覆盖层窗口初始化失败",
            "[{}] Failed to start pin edit: overlay window initialization failed"
        }},
        {"无法捕获屏幕底图用于贴图编辑", {
            "[{}] 无法捕获屏幕底图用于贴图编辑",
            "[{}] Failed to capture screen backdrop for pin edit"
        }},
        {"贴图编辑底图上传失败", {
            "[{}] 贴图编辑底图上传失败",
            "[{}] Pin edit backdrop upload failed"
        }},
        {"无效的虚拟屏幕尺寸: {}x{}", {
            "[{}] 无效的虚拟屏幕尺寸: {}x{}",
            "[{}] Invalid virtual screen size: {}x{}"
        }},
        {"GetDC(nullptr) 失败, error={}", {
            "[{}] GetDC(nullptr) 失败, error={}",
            "[{}] GetDC(nullptr) failed , error={}"
        }},
        {"CreateCompatibleDC 失败, error={}", {
            "[{}] CreateCompatibleDC 失败, error={}",
            "[{}] CreateCompatibleDC failed , error={}"
        }},
        {"CreateDIBSection 失败, error={}", {
            "[{}] CreateDIBSection 失败, error={}",
            "[{}] CreateDIBSection failed , error={}"
        }},
        {"截图覆盖层桌面底图捕获成功: {}x{} (坐标=[{}, {}])", {
            "[{}] 截图覆盖层桌面底图捕获成功: {}x{} (坐标=[{}, {}])",
            "[{}] operationsucceeded : {}x{} (operation=[{}, {}])"
        }},
        {"截图覆盖层 BitBlt 失败, error={}", {
            "[{}] 截图覆盖层 BitBlt 失败, error={}",
            "[{}] operation BitBlt failed , error={}"
        }},
        {"截图底图上传失败: 渲染目标或图像为空", {
            "[{}] 截图底图上传失败: 渲染目标或图像为空",
            "[{}] operationfailed : operation"
        }},
        {"截图底图上传失败: 不支持的通道数={}", {
            "[{}] 截图底图上传失败: 不支持的通道数={}",
            "[{}] operationfailed : operation={}"
        }},
        {"截图底图上传 Direct2D 失败, hr=0x{:08X}", {
            "[{}] 截图底图上传 Direct2D 失败, hr=0x{:08X}",
            "[{}] operation Direct2D failed , hr=0x{:08X}"
        }},
        {"GDI+ 文本渲染器初始化失败, status={}", {
            "[{}] GDI+ 文本渲染器初始化失败, status={}",
            "[{}] GDI+ operationinitialize failed , status={}"
        }},
        {"GDI+ 文本渲染器已初始化", {
            "[{}] GDI+ 文本渲染器已初始化",
            "[{}] GDI+ operationinitialize"
        }},
        {"GDI+ 文本渲染器已关闭", {
            "[{}] GDI+ 文本渲染器已关闭",
            "[{}] GDI+ operation"
        }},
        {"标注引擎: 设置底图 {}x{}", {
            "[{}] 标注引擎: 设置底图 {}x{}",
            "[{}] Annotation engine: set backdrop {}x{}"
        }},
        {"标注引擎: 撤销, 剩余元素数={}", {
            "[{}] 标注引擎: 撤销, 剩余元素数={}",
            "[{}] Annotation engine: undo, remaining elements={}"
        }},
        {"标注引擎: 重做, 剩余元素数={}", {
            "[{}] 标注引擎: 重做, 剩余元素数={}",
            "[{}] Annotation engine: redo, remaining elements={}"
        }},
        {"标注引擎: 添加聚光灯 ({},{})→({},{}) dimAlpha={:.2f} ellipse={}", {
            "[{}] 标注引擎: 添加聚光灯 ({},{})→({},{}) dimAlpha={:.2f} ellipse={}",
            "[{}] Annotation engine: add spotlight ({},{})→({},{}) dimAlpha={:.2f} ellipse={}"
        }},
        {"标注引擎: 添加水印 ({},{})→({},{}) text='{}' opacity={:.2f} angle={:.1f}", {
            "[{}] 标注引擎: 添加水印 ({},{})→({},{}) text='{}' opacity={:.2f} angle={:.1f}",
            "[{}] Annotation engine: add watermark ({},{})→({},{}) text='{}' opacity={:.2f} angle={:.1f}"
        }},
        {"标注引擎: 添加智能消除 ({},{})→({},{}) radius={}", {
            "[{}] 标注引擎: 添加智能消除 ({},{})→({},{}) radius={}",
            "[{}] Annotation engine: add smart inpaint ({},{})→({},{}) radius={}"
        }},
        {"标注引擎: 渲染聚光灯 dimAlpha={:.2f} ellipse={}", {
            "[{}] 标注引擎: 渲染聚光灯 dimAlpha={:.2f} ellipse={}",
            "[{}] Annotation engine: render spotlight dimAlpha={:.2f} ellipse={}"
        }},
        {"标注引擎: 渲染水印 text='{}' opacity={:.2f} angle={:.1f} spacing={}", {
            "[{}] 标注引擎: 渲染水印 text='{}' opacity={:.2f} angle={:.1f} spacing={}",
            "[{}] Annotation engine: render watermark text='{}' opacity={:.2f} angle={:.1f} spacing={}"
        }},
        {"标注引擎: 渲染智能消除 radius={}", {
            "[{}] 标注引擎: 渲染智能消除 radius={}",
            "[{}] Annotation engine: render smart inpaint radius={}"
        }},
        {"贴图窗口创建失败", {
            "[{}] 贴图窗口创建失败",
            "[{}] Failed to create pin window"
        }},
        {"贴图窗口渲染资源创建失败", {
            "[{}] 贴图窗口渲染资源创建失败",
            "[{}] Failed to create pin window render resources"
        }},
        {"贴图窗口已创建: {}x{} @ ({},{}), 总数={}", {
            "[{}] 贴图窗口已创建: {}x{} @ ({},{}), 总数={}",
            "[{}] Pin window created: {}x{} @ ({},{}), count={}"
        }},
        {"所有贴图窗口已关闭并释放资源", {
            "[{}] 所有贴图窗口已关闭并释放资源",
            "[{}] All pin windows closed and resources released"
        }},
        {"所有贴图已{}", {
            "[{}] 所有贴图已{}",
            "[{}] All pinned images {}"
        }},
        {"已整理 {} 张贴图", {
            "[{}] 已整理 {} 张贴图",
            "[{}] Arranged {} pinned images"
        }},
        {"贴图鼠标穿透: {}", {
            "[{}] 贴图鼠标穿透: {}",
            "[{}] Pin click-through: {}"
        }},
        {"贴图保存对话框创建失败: hr=0x{:08X}", {
            "[{}] 贴图保存对话框创建失败: hr=0x{:08X}",
            "[{}] Pin save dialog creation failed: hr=0x{:08X}"
        }},
        {"贴图保存对话框失败: hr=0x{:08X}", {
            "[{}] 贴图保存对话框失败: hr=0x{:08X}",
            "[{}] Pin save dialog failed: hr=0x{:08X}"
        }},
        {"贴图已保存: {}", {
            "[{}] 贴图已保存: {}",
            "[{}] Pinned image saved: {}"
        }},
        {"贴图保存失败: {}", {
            "[{}] 贴图保存失败: {}",
            "[{}] Pin save failed: {}"
        }},
        {"剪贴板贴图：剪贴板无可贴内容", {
            "[{}] 剪贴板贴图：剪贴板无可贴内容",
            "[{}] Clipboard pin: No image found in clipboard"
        }},
        {"剪贴板贴图：{}x{} @ ({},{})", {
            "[{}] 剪贴板贴图：{}x{} @ ({},{})",
            "[{}] Clipboard pin: {}x{} @ ({},{})"
        }},
        {"录屏已保存: {}", {
            "[{}] 录屏已保存: {}",
            "[{}] operation: {}"
        }},
        {"OCR 后台线程异常: {}", {
            "[{}] OCR 后台线程异常: {}",
            "[{}] OCR operationexception : {}"
        }},
        {"OCR 后台线程未知异常", {
            "[{}] OCR 后台线程未知异常",
            "[{}] OCR operationexception"
        }},
        {"CapturePlugin: 初始化截图/录屏引擎", {
            "[{}] CapturePlugin: 初始化截图/录屏引擎",
            "[{}] CapturePlugin: Initializing capture & recorder engine"
        }},
        {"贴图文件加载失败: {}", {
            "[{}] 贴图文件加载失败: {}",
            "[{}] Failed to load pin image file: {}"
        }},
        {"CapturePlugin: 卸载截图/录屏引擎", {
            "[{}] CapturePlugin: 卸载截图/录屏引擎",
            "[{}] CapturePlugin: Unloading capture & recorder engine"
        }},
        {"录制指示器按需初始化失败", {
            "[{}] 录制指示器按需初始化失败",
            "[{}] operationinitialize failed"
        }},
        {"录制指示器按需初始化成功", {
            "[{}] 录制指示器按需初始化成功",
            "[{}] operationinitialize succeeded"
        }},
        {"录制指示器已显示", {
            "[{}] 录制指示器已显示",
            "[{}] Recording indicator shown"
        }},
        {"录制指示器已隐藏", {
            "[{}] 录制指示器已隐藏",
            "[{}] operation"
        }},
        {"当前 Windows 版本无法从捕获中排除录制悬浮条: error={}", {
            "[{}] 当前 Windows 版本无法从捕获中排除录制悬浮条: error={}",
            "[{}] operation Windows operation: error={}"
        }},
        {"无法从屏幕录制中排除呼吸发光线框: error={}", {
            "[{}] 无法从屏幕录制中排除呼吸发光线框: error={}",
            "[{}] Failed to exclude breathing border frame from screen recording: error={}"
        }},
        {"截图完成: {}x{}", {
            "[{}] 截图完成: {}x{}",
            "[{}] Screenshot completed: {}x{}"
        }},
        {"OCR 提取完成, 行数={}", {
            "[{}] OCR 提取完成, 行数={}",
            "[{}] OCR text extracted, lines={}"
        }},
        {"OCR 未提取到文字", {
            "[{}] OCR 未提取到文字",
            "[{}] OCR operation"
        }},
        {"OCR(覆盖层) 异常: {}", {
            "[{}] OCR(覆盖层) 异常: {}",
            "[{}] OCR(operation) exception : {}"
        }},
        {"OCR(覆盖层) 未知异常", {
            "[{}] OCR(覆盖层) 未知异常",
            "[{}] OCR(operation) operationexception"
        }},
        {"长截图失败: {}", {
            "[{}] 长截图失败: {}",
            "[{}] Scroll capture failed: {}"
        }},
        {"长截图已保存，共 {} 帧", {
            "[{}] 长截图已保存，共 {} 帧",
            "[{}] Scroll capture saved, total frames={}"
        }},
        {"截图引擎已初始化", {
            "[{}] 截图引擎已初始化",
            "[{}] Screen capture engine initialized"
        }},
        {"截图引擎已关闭", {
            "[{}] 截图引擎已关闭",
            "[{}] Screen capture engine closed"
        }},
        {"前台处于全屏独占应用，自动免打扰跳过截图: hwnd=0x{:X}", {
            "[{}] 前台处于全屏独占应用，自动免打扰跳过截图: hwnd=0x{:X}",
            "[{}] Foreground is exclusive fullscreen, skipping capture (Do Not Disturb): hwnd=0x{:X}"
        }},
        {"截图已在进行中", {
            "[{}] 截图已在进行中",
            "[{}] operation"
        }},
        {"执行全屏截图", {
            "[{}] 执行全屏截图",
            "[{}] Executing fullscreen capture"
        }},
        {"截取窗口: hwnd={}, region=({},{})x{}x{}", {
            "[{}] 截取窗口: hwnd={}, region=({},{})x{}x{}",
            "[{}] Capturing window: hwnd={}, region=({},{})x{}x{}"
        }},
        {"截图失败: {}", {
            "[{}] 截图失败: {}",
            "[{}] operationfailed : {}"
        }},
        {"截图已复制到剪贴板", {
            "[{}] 截图已复制到剪贴板",
            "[{}] operation"
        }},
        {"复制到剪贴板失败", {
            "[{}] 复制到剪贴板失败",
            "[{}] operationfailed"
        }},
        {"截图已保存: path={}, size={}KB", {
            "[{}] 截图已保存: path={}, size={}KB",
            "[{}] Screenshot saved: path={}, size={}KB"
        }},
        {"截图完成: {}x{}, format={}", {
            "[{}] 截图完成: {}x{}, format={}",
            "[{}] Screenshot completed: {}x{}, format={}"
        }},
        {"获取屏幕 DC 失败, error={}", {
            "[{}] 获取屏幕 DC 失败, error={}",
            "[{}] Failed to get screen DC, error={}"
        }},
        {"创建截图内存 DC 失败, error={}", {
            "[{}] 创建截图内存 DC 失败, error={}",
            "[{}] create operation DC failed , error={}"
        }},
        {"创建截图 DIBSection 失败, error={}", {
            "[{}] 创建截图 DIBSection 失败, error={}",
            "[{}] create operation DIBSection failed , error={}"
        }},
        {"选择截图位图失败, error={}", {
            "[{}] 选择截图位图失败, error={}",
            "[{}] Failed to select screenshot bitmap, error={}"
        }},
        {"BitBlt 失败, error={}", {
            "[{}] BitBlt 失败, error={}",
            "[{}] BitBlt failed , error={}"
        }},
        {"无法打开剪贴板, error={}", {
            "[{}] 无法打开剪贴板, error={}",
            "[{}] Failed to open clipboard, error={}"
        }},
        {"无法打开文件: {}", {
            "[{}] 无法打开文件: {}",
            "[{}] Failed to open file: {}"
        }},
        {"保存截图失败: {}", {
            "[{}] 保存截图失败: {}",
            "[{}] operationfailed : {}"
        }},
        {"屏幕录制引擎已初始化", {
            "[{}] 屏幕录制引擎已初始化",
            "[{}] Screen recording engine initialized"
        }},
        {"屏幕录制引擎已关闭", {
            "[{}] 屏幕录制引擎已关闭",
            "[{}] Screen recording engine closed"
        }},
        {"已在录制中, 请先停止当前录制", {
            "[{}] 已在录制中, 请先停止当前录制",
            "[{}] operation, operationstopped operation"
        }},
        {"录屏参数无效: fps={}, bitrate={}, countdown={}", {
            "[{}] 录屏参数无效: fps={}, bitrate={}, countdown={}",
            "[{}] operation: fps={}, bitrate={}, countdown={}"
        }},
        {"录屏区域无效: {}x{}", {
            "[{}] 录屏区域无效: {}x{}",
            "[{}] operation: {}x{}"
        }},
        {"无法创建录屏输出目录: path={}, error={}", {
            "[{}] 无法创建录屏输出目录: path={}, error={}",
            "[{}] Failed to create recording output directory: path={}, error={}"
        }},
        {"FFmpeg 编码器初始化失败", {
            "[{}] FFmpeg 编码器初始化失败",
            "[{}] FFmpeg operationinitialize failed"
        }},
        {"屏幕录制已准备: {}x{} @ {}fps, countdown={}s, format={}, output={}", {
            "[{}] 屏幕录制已准备: {}x{} @ {}fps, countdown={}s, format={}, output={}",
            "[{}] Screen recording prepared: {}x{} @ {}fps, countdown={}s, format={}, output={}"
        }},
        {"屏幕录制已暂停, frames={}", {
            "[{}] 屏幕录制已暂停, frames={}",
            "[{}] Screen recording paused, frames={}"
        }},
        {"屏幕录制已恢复", {
            "[{}] 屏幕录制已恢复",
            "[{}] Screen recording resumed"
        }},
        {"屏幕录制已停止: frames={}, dropped={}, duration={:.1f}s, output={}", {
            "[{}] 屏幕录制已停止: frames={}, dropped={}, duration={:.1f}s, output={}",
            "[{}] Screen recording stopped: frames={}, dropped={}, duration={:.1f}s, output={}"
        }},
        {"录屏捕获资源初始化失败", {
            "[{}] 录屏捕获资源初始化失败",
            "[{}] operationinitialize failed"
        }},
        {"帧捕获/编码失败, 停止录制", {
            "[{}] 帧捕获/编码失败, 停止录制",
            "[{}] operation/operationfailed , stopped operation"
        }},
        {"音频编码失败, 已降级为仅视频录制", {
            "[{}] 音频编码失败, 已降级为仅视频录制",
            "[{}] Audio encoding failed, degraded to video-only recording"
        }},
        {"录屏持续过载，自适应帧步长调整为 {}", {
            "[{}] 录屏持续过载，自适应帧步长调整为 {}",
            "[{}] operation，operation {}"
        }},
        {"录屏负载已恢复，自适应帧步长调整为 {}", {
            "[{}] 录屏负载已恢复，自适应帧步长调整为 {}",
            "[{}] Recording load recovered, adaptive frame step adjusted to {}"
        }},
        {"录屏因磁盘安全余量不足而停止", {
            "[{}] 录屏因磁盘安全余量不足而停止",
            "[{}] operationstopped"
        }},
        {"录制线程异常, 已停止录制: {}", {
            "[{}] 录制线程异常, 已停止录制: {}",
            "[{}] operationexception , stopped operation: {}"
        }},
        {"录制线程未知异常, 已停止录制", {
            "[{}] 录制线程未知异常, 已停止录制",
            "[{}] operationexception , stopped operation"
        }},
        {"录屏临时文件不存在, 无法提交: {}", {
            "[{}] 录屏临时文件不存在, 无法提交: {}",
            "[{}] operationfile operation, operation: {}"
        }},
        {"录屏文件原子提交失败: error={}, temporary={}", {
            "[{}] 录屏文件原子提交失败: error={}, temporary={}",
            "[{}] operationfile operationfailed : error={}, temporary={}"
        }},
        {"录屏输出目录不可写: path={}, error={}", {
            "[{}] 录屏输出目录不可写: path={}, error={}",
            "[{}] operationdirectory operation: path={}, error={}"
        }},
        {"无法查询录屏目录剩余空间: path={}, error={}", {
            "[{}] 无法查询录屏目录剩余空间: path={}, error={}",
            "[{}] Failed to query recording directory free space: path={}, error={}"
        }},
        {"录屏剩余空间不足: available={}, required={}", {
            "[{}] 录屏剩余空间不足: available={}, required={}",
            "[{}] operation: available={}, required={}"
        }},
        {"未找到编码器: format={}", {
            "[{}] 未找到编码器: format={}",
            "[{}] Encoder not found for format: format={}"
        }},
        {"avformat_alloc_output_context2 失败", {
            "[{}] avformat_alloc_output_context2 失败",
            "[{}] avformat_alloc_output_context2 failed "
        }},
        {"编码器不可用: codec={}, pixelFormat={}, error={} ({})", {
            "[{}] 编码器不可用: codec={}, pixelFormat={}, error={} ({})",
            "[{}] Encoder unavailable: codec={}, pixelFormat={}, error={} ({})"
        }},
        {"所有候选编码器均初始化失败: format={}", {
            "[{}] 所有候选编码器均初始化失败: format={}",
            "[{}] operationinitialize failed : format={}"
        }},
        {"请求的编码器在当前系统不可用，已回退兼容编码器: codec={}", {
            "[{}] 请求的编码器在当前系统不可用，已回退兼容编码器: codec={}",
            "[{}] Requested encoder unavailable, fallback to compatible encoder: codec={}"
        }},
        {"avformat_new_stream 失败", {
            "[{}] avformat_new_stream 失败",
            "[{}] avformat_new_stream failed "
        }},
        {"音频编码器不可用, 已降级为仅视频录制", {
            "[{}] 音频编码器不可用, 已降级为仅视频录制",
            "[{}] Audio encoder unavailable, degraded to video-only recording"
        }},
        {"avio_open 失败: {}", {
            "[{}] avio_open 失败: {}",
            "[{}] avio_open failed : {}"
        }},
        {"avformat_write_header 失败", {
            "[{}] avformat_write_header 失败",
            "[{}] avformat_write_header failed "
        }},
        {"av_frame_alloc 失败", {
            "[{}] av_frame_alloc 失败",
            "[{}] av_frame_alloc failed "
        }},
        {"av_frame_get_buffer 失败", {
            "[{}] av_frame_get_buffer 失败",
            "[{}] av_frame_get_buffer failed "
        }},
        {"av_packet_alloc 失败", {
            "[{}] av_packet_alloc 失败",
            "[{}] av_packet_alloc failed "
        }},
        {"FFmpeg 编码器初始化成功: codec={}, {}x{} @ {}fps", {
            "[{}] FFmpeg 编码器初始化成功: codec={}, {}x{} @ {}fps",
            "[{}] FFmpeg operationinitialize succeeded : codec={}, {}x{} @ {}fps"
        }},
        {"音频编码器初始化失败: codec={}, error={}", {
            "[{}] 音频编码器初始化失败: codec={}, error={}",
            "[{}] Audio encoder initialization failed: codec={}, error={}"
        }},
        {"录屏音频已启用: codec={}, rate={}, channels={}", {
            "[{}] 录屏音频已启用: codec={}, rate={}, channels={}",
            "[{}] Screen recording audio enabled: codec={}, rate={}, channels={}"
        }},
        {"录屏帧捕获失败: backend={}, error={}", {
            "[{}] 录屏帧捕获失败: backend={}, error={}",
            "[{}] operationfailed : backend={}, error={}"
        }},
        {"录屏捕获后端已切换: {} -> {}", {
            "[{}] 录屏捕获后端已切换: {} -> {}",
            "[{}] Capture backend switched: {} -> {}"
        }},
        {"sws_getContext 失败: sourceFormat={}", {
            "[{}] sws_getContext 失败: sourceFormat={}",
            "[{}] sws_getContext failed : sourceFormat={}"
        }},
        {"录屏捕获后端初始化失败: error={}", {
            "[{}] 录屏捕获后端初始化失败: error={}",
            "[{}] operationinitialize failed : error={}"
        }},
        {"录屏捕获后端已选择: backend={}, accelerated={}", {
            "[{}] 录屏捕获后端已选择: backend={}, accelerated={}",
            "[{}] Capture backend selected: backend={}, accelerated={}"
        }},
        {"长截图已在运行中", {
            "[{}] 长截图已在运行中",
            "[{}] Scroll capture already in progress"
        }},
        {"长截图捕获后端初始化失败: {}", {
            "[{}] 长截图捕获后端初始化失败: {}",
            "[{}] Scroll capture backend initialization failed: {}"
        }},
        {"长截图已启动: mode={}, scrollDelay={}ms, maxFrames={}", {
            "[{}] 长截图已启动: mode={}, scrollDelay={}ms, maxFrames={}",
            "[{}] Scroll capture started: mode={}, scrollDelay={}ms, maxFrames={}"
        }},
        {"用户按 Esc 结束长截图, frame={}", {
            "[{}] 用户按 Esc 结束长截图, frame={}",
            "[{}] User pressed Esc to finish scroll capture, frame={}"
        }},
        {"检测到滚动到底部, frame={}", {
            "[{}] 检测到滚动到底部, frame={}",
            "[{}] Detected scroll to bottom, frame={}"
        }},
        {"长截图滚轮输入失败: error={}", {
            "[{}] 长截图滚轮输入失败: error={}",
            "[{}] Scroll capture wheel input failed: error={}"
        }},
        {"长截图线程异常, 已停止: {}", {
            "[{}] 长截图线程异常, 已停止: {}",
            "[{}] Scroll capture thread exception, stopped: {}"
        }},
        {"长截图线程未知异常, 已停止", {
            "[{}] 长截图线程未知异常, 已停止",
            "[{}] Scroll capture thread unknown exception, stopped"
        }},
        {"长截图达到字节预算: current={} MiB, next={} MiB, limit={} MiB", {
            "[{}] 长截图达到字节预算: current={} MiB, next={} MiB, limit={} MiB",
            "[{}] Scroll capture reached byte budget: current={} MiB, next={} MiB, limit={} MiB"
        }},
        {"无法删除长截图临时文件: {}", {
            "[{}] 无法删除长截图临时文件: {}",
            "[{}] Failed to delete scroll capture temporary file: {}"
        }},
        {"长截图拼接完成: {}x{}, 帧数={}", {
            "[{}] 长截图拼接完成: {}x{}, 帧数={}",
            "[{}] Scroll capture stitched: {}x{}, frames={}"
        }},
        {"长截图帧捕获失败: backend={}, error={}", {
            "[{}] 长截图帧捕获失败: backend={}, error={}",
            "[{}] Scroll capture frame grab failed: backend={}, error={}"
        }},
        {"拼接帧 {}/{}: offset={}, 结果高度={}", {
            "[{}] 拼接帧 {}/{}: offset={}, 结果高度={}",
            "[{}] operation {}/{}: offset={}, operation={}"
        }},
        {"缩小模板匹配置信度过低: {:.3f}", {
            "[{}] 缩小模板匹配置信度过低: {:.3f}",
            "[{}] Scaled template match confidence too low: {:.3f}"
        }},
        {"模板匹配置信度过低: {:.3f}", {
            "[{}] 模板匹配置信度过低: {:.3f}",
            "[{}] Template match confidence too low: {:.3f}"
        }},
        {"模板匹配: confidence={:.3f}, offset={}", {
            "[{}] 模板匹配: confidence={:.3f}, offset={}",
            "[{}] Template match: confidence={:.3f}, offset={}"
        }},
        {"当前 Windows 版本无法从捕获中排除长截图预览: error={}", {
            "[{}] 当前 Windows 版本无法从捕获中排除长截图预览: error={}",
            "[{}] operation Windows operation: error={}"
        }},
        {"无法从录屏中排除快捷键提示层: error={}", {
            "[{}] 无法从录屏中排除快捷键提示层: error={}",
            "[{}] Failed to exclude shortcut hint overlay from recording: error={}"
        }},
        {"无法创建配置目录: path={}, error={}", {
            "[{}] 无法创建配置目录: path={}, error={}",
            "[{}] Failed to create config directory: path={}, error={}"
        }},
        {"配置管理器初始化, 配置文件路径={}", {
            "[{}] 配置管理器初始化, 配置文件路径={}",
            "[{}] ConfigManager initialized, config path={}"
        }},
        {"无法创建配置监控停止事件: error={}", {
            "[{}] 无法创建配置监控停止事件: error={}",
            "[{}] Failed to create config watch stop event: error={}"
        }},
        {"无法启动配置监控线程: {}", {
            "[{}] 无法启动配置监控线程: {}",
            "[{}] Failed to start config watcher thread: {}"
        }},
        {"配置管理器正在关闭...", {
            "[{}] 配置管理器正在关闭...",
            "[{}] ConfigManager is shutting down..."
        }},
        {"配置管理器关闭时保存失败", {
            "[{}] 配置管理器关闭时保存失败",
            "[{}] Failed to save configuration on ConfigManager shutdown"
        }},
        {"配置文件不存在, 将使用默认配置并创建文件: {}", {
            "[{}] 配置文件不存在, 将使用默认配置并创建文件: {}",
            "[{}] Config file does not exist, creating default config: {}"
        }},
        {"配置文件加载成功, 键数量={}", {
            "[{}] 配置文件加载成功, 键数量={}",
            "[{}] Configuration loaded successfully, key count={}"
        }},
        {"配置文件解析失败: {}, 保留最后一次有效配置", {
            "[{}] 配置文件解析失败: {}, 保留最后一次有效配置",
            "[{}] Config parse failed: {}, retaining last valid config"
        }},
        {"配置文件解析失败: {}, 首次启动使用默认配置", {
            "[{}] 配置文件解析失败: {}, 首次启动使用默认配置",
            "[{}] Config parse failed: {}, fallback to default config"
        }},
        {"损坏配置备份失败: {}", {
            "[{}] 损坏配置备份失败: {}",
            "[{}] Failed to backup corrupted config: {}"
        }},
        {"损坏配置已保留到: {}", {
            "[{}] 损坏配置已保留到: {}",
            "[{}] Corrupted config preserved at: {}"
        }},
        {"配置已通过硬件原子刷盘保存到文件: {}", {
            "[{}] 配置已通过硬件原子刷盘保存到文件: {}",
            "[{}] Configuration saved to file via atomic flush: {}"
        }},
        {"配置原子硬件刷盘失败: {}", {
            "[{}] 配置原子硬件刷盘失败: {}",
            "[{}] Config hardware atomic flush failed: {}"
        }},
        {"配置保存失败: {}", {
            "[{}] 配置保存失败: {}",
            "[{}] Failed to save configuration: {}"
        }},
        {"设置配置项失败: key={}, error={}", {
            "[{}] 设置配置项失败: key={}, error={}",
            "[{}] Failed to set config item: key={}, error={}"
        }},
        {"配置合并失败: patch 根节点不是对象", {
            "[{}] 配置合并失败: patch 根节点不是对象",
            "[{}] Config merge failed: patch root is not an object"
        }},
        {"配置合并失败: {}", {
            "[{}] 配置合并失败: {}",
            "[{}] Config merge failed: {}"
        }},
        {"删除配置项失败: key={}, error={}", {
            "[{}] 删除配置项失败: key={}, error={}",
            "[{}] operationconfig operationfailed : key={}, error={}"
        }},
        {"配置已从 JSON 字符串批量更新", {
            "[{}] 配置已从 JSON 字符串批量更新",
            "[{}] Configuration batch updated from JSON string"
        }},
        {"JSON 字符串解析失败: {}", {
            "[{}] JSON 字符串解析失败: {}",
            "[{}] JSON operationfailed : {}"
        }},
        {"配置已重置", {
            "[{}] 配置已重置",
            "[{}] Configuration reset"
        }},
        {"配置变更回调执行异常: callbackId={}, error={}", {
            "[{}] 配置变更回调执行异常: callbackId={}, error={}",
            "[{}] Config change callback exception: callbackId={}, error={}"
        }},
        {"无法打开配置导出临时文件: {}", {
            "[{}] 无法打开配置导出临时文件: {}",
            "[{}] Failed to open config export temp file: {}"
        }},
        {"配置已导出到: {}", {
            "[{}] 配置已导出到: {}",
            "[{}] Configuration exported to: {}"
        }},
        {"配置导出失败: {}", {
            "[{}] 配置导出失败: {}",
            "[{}] Config export failed: {}"
        }},
        {"配置已从文件导入(合并模式): {}", {
            "[{}] 配置已从文件导入(合并模式): {}",
            "[{}] Config imported from file (merge mode): {}"
        }},
        {"配置导入失败: {}", {
            "[{}] 配置导入失败: {}",
            "[{}] Config import failed: {}"
        }},
        {"无法监控配置文件目录: {}", {
            "[{}] 无法监控配置文件目录: {}",
            "[{}] Failed to watch config directory: {}"
        }},
        {"配置文件监控已启动: {}", {
            "[{}] 配置文件监控已启动: {}",
            "[{}] Config file watcher started: {}"
        }},
        {"无法创建配置文件监控事件: error={}", {
            "[{}] 无法创建配置文件监控事件: error={}",
            "[{}] Failed to create config file watch event: error={}"
        }},
        {"读取配置目录变更失败: error={}", {
            "[{}] 读取配置目录变更失败: error={}",
            "[{}] Failed to read config directory changes: error={}"
        }},
        {"检测到配置文件变更, 正在热加载...", {
            "[{}] 检测到配置文件变更, 正在热加载...",
            "[{}] Config file change detected, hot reloading..."
        }},
        {"配置文件监控已停止", {
            "[{}] 配置文件监控已停止",
            "[{}] Config file watcher stopped"
        }},
        {"EventBus: 取消订阅, subId={}", {
            "[{}] EventBus: 取消订阅, subId={}",
            "[{}] EventBus: operation, subId={}"
        }},
        {"EventBus: 同步取消订阅, subId={}", {
            "[{}] EventBus: 同步取消订阅, subId={}",
            "[{}] EventBus: operation, subId={}"
        }},
        {"EventBus: 清除所有订阅", {
            "[{}] EventBus: 清除所有订阅",
            "[{}] EventBus: operation"
        }},
        {"EventBus: 事件类型转换失败: {}", {
            "[{}] EventBus: 事件类型转换失败: {}",
            "[{}] EventBus: operationfailed : {}"
        }},
        {"EventBus: 事件处理器异常: {}", {
            "[{}] EventBus: 事件处理器异常: {}",
            "[{}] EventBus: operationexception : {}"
        }},
        {"EventBus: 事件处理器未知异常", {
            "[{}] EventBus: 事件处理器未知异常",
            "[{}] EventBus: operationexception"
        }},
        {"EventBus: 订阅事件 [{}], subId={}", {
            "[{}] EventBus: 订阅事件 [{}], subId={}",
            "[{}] EventBus: operation [{}], subId={}"
        }},
        {"EventBus: 发布事件 [{}], 订阅者数={}", {
            "[{}] EventBus: 发布事件 [{}], 订阅者数={}",
            "[{}] EventBus: operation [{}], operation={}"
        }},
        {"快捷键管理器已初始化", {
            "[{}] 快捷键管理器已初始化",
            "[{}] Hotkey manager initialized"
        }},
        {"注销快捷键: name={}, def={}", {
            "[{}] 注销快捷键: name={}, def={}",
            "[{}] Unregister hotkey: name={}, def={}"
        }},
        {"快捷键管理器已关闭, 所有快捷键已注销", {
            "[{}] 快捷键管理器已关闭, 所有快捷键已注销",
            "[{}] operation, operation"
        }},
        {"快捷键名称已存在, 将先注销旧绑定: name={}", {
            "[{}] 快捷键名称已存在, 将先注销旧绑定: name={}",
            "[{}] operation, operation: name={}"
        }},
        {"快捷键已禁用: name={}", {
            "[{}] 快捷键已禁用: name={}",
            "[{}] operation: name={}"
        }},
        {"拒绝注册不安全的全局快捷键: name={}, def={}", {
            "[{}] 拒绝注册不安全的全局快捷键: name={}, def={}",
            "[{}] Refuse to register unsafe global hotkey: name={}, def={}"
        }},
        {"快捷键由底层键盘钩子引擎接管注册成功: name={}, def={}", {
            "[{}] 快捷键由底层键盘钩子引擎接管注册成功: name={}, def={}",
            "[{}] Hotkey registered successfully via low-level keyboard hook engine: name={}, def={}"
        }},
        {"重绑定拒绝不安全的快捷键: name={}, def={}", {
            "[{}] 重绑定拒绝不安全的快捷键: name={}, def={}",
            "[{}] Rebind rejected unsafe hotkey: name={}, def={}"
        }},
        {"快捷键由底层键盘钩子引擎接管重绑定成功: name={}, def={}", {
            "[{}] 快捷键由底层键盘钩子引擎接管重绑定成功: name={}, def={}",
            "[{}] Hotkey rebound successfully via low-level keyboard hook engine: name={}, def={}"
        }},
        {"底层键盘钩子快捷键触发: name={}", {
            "[{}] 底层键盘钩子快捷键触发: name={}",
            "[{}] Low-level keyboard hook hotkey triggered: name={}"
        }},
        {"底层键盘钩子快捷键回调异常: name={}, error={}", {
            "[{}] 底层键盘钩子快捷键回调异常: name={}, error={}",
            "[{}] Low-level keyboard hook hotkey callback error: name={}, error={}"
        }},
        {"底层键盘钩子快捷键未知异常: name={}", {
            "[{}] 底层键盘钩子快捷键未知异常: name={}",
            "[{}] Low-level keyboard hook hotkey unknown error: name={}"
        }},
        {"注册快捷键失败: name={}, def={}, error={}", {
            "[{}] 注册快捷键失败: name={}, def={}, error={}",
            "[{}] Failed to register hotkey: name={}, def={}, error={}"
        }},
        {"快捷键 {} 已被其他程序占用", {
            "[{}] 快捷键 {} 已被其他程序占用",
            "[{}] operation {} operation"
        }},
        {"注册快捷键成功: name={}, def={}, id={}", {
            "[{}] 注册快捷键成功: name={}, def={}, id={}",
            "[{}] Hotkey registered successfully: name={}, def={}, id={}"
        }},
        {"尝试注销不存在的快捷键: name={}", {
            "[{}] 尝试注销不存在的快捷键: name={}",
            "[{}] operation: name={}"
        }},
        {"已注销快捷键: name={}", {
            "[{}] 已注销快捷键: name={}",
            "[{}] operation: name={}"
        }},
        {"尝试重绑定不存在的快捷键: name={}", {
            "[{}] 尝试重绑定不存在的快捷键: name={}",
            "[{}] operation: name={}"
        }},
        {"快捷键重绑定成功（会话外不占用）: name={}, {} → {}", {
            "[{}] 快捷键重绑定成功（会话外不占用）: name={}, {} → {}",
            "[{}] operationsucceeded （operation）: name={}, {} → {}"
        }},
        {"重绑定快捷键失败: name={}, newDef={}", {
            "[{}] 重绑定快捷键失败: name={}, newDef={}",
            "[{}] Failed to rebind hotkey: name={}, newDef={}"
        }},
        {"快捷键重绑定成功: name={}, {} → {}", {
            "[{}] 快捷键重绑定成功: name={}, {} → {}",
            "[{}] operationsucceeded : name={}, {} → {}"
        }},
        {"尝试武装不存在的快捷键: name={}", {
            "[{}] 尝试武装不存在的快捷键: name={}",
            "[{}] operation: name={}"
        }},
        {"快捷键已卸下，交还给前台应用: name={}, def={}", {
            "[{}] 快捷键已卸下，交还给前台应用: name={}, def={}",
            "[{}] operation，operation: name={}, def={}"
        }},
        {"快捷键已重新占用: name={}, def={}", {
            "[{}] 快捷键已重新占用: name={}, def={}",
            "[{}] operation: name={}, def={}"
        }},
        {"重新占用快捷键失败: name={}, def={}, error={}", {
            "[{}] 重新占用快捷键失败: name={}, def={}, error={}",
            "[{}] Failed to reclaim hotkey: name={}, def={}, error={}"
        }},
        {"快捷键已暂停触发 (录制模式中)", {
            "[{}] 快捷键已暂停触发 (录制模式中)",
            "[{}] operationpaused operation (operation)"
        }},
        {"快捷键触发: name={}", {
            "[{}] 快捷键触发: name={}",
            "[{}] operation: name={}"
        }},
        {"快捷键回调异常: name={}, error={}", {
            "[{}] 快捷键回调异常: name={}, error={}",
            "[{}] operationexception : name={}, error={}"
        }},
        {"KeyboardHook 发生未捕获异常 {}", {
            "[{}] KeyboardHook 发生未捕获异常 {}",
            "[{}] KeyboardHook operationexception {}"
        }},
        {"KeyboardHook 发生未知异常", {
            "[{}] KeyboardHook 发生未知异常",
            "[{}] KeyboardHook operationexception"
        }},
        {"安装全局核心鼠标钩子失败, error={}", {
            "[{}] 安装全局核心鼠标钩子失败, error={}",
            "[{}] operationfailed , error={}"
        }},
        {"全局核心鼠标钩子安装成功", {
            "[{}] 全局核心鼠标钩子安装成功",
            "[{}] Global mouse hook installed successfully"
        }},
        {"全局核心鼠标钩子已卸载", {
            "[{}] 全局核心鼠标钩子已卸载",
            "[{}] operation"
        }},
        {"MouseHook 活动事件分发异常: {}", {
            "[{}] MouseHook 活动事件分发异常: {}",
            "[{}] MouseHook operationexception : {}"
        }},
        {"MouseHook 活动事件分发未知异常", {
            "[{}] MouseHook 活动事件分发未知异常",
            "[{}] MouseHook operationexception"
        }},
        {"注册 IPC 处理器: method={}", {
            "[{}] 注册 IPC 处理器: method={}",
            "[{}] Register IPC handler: method={}"
        }},
        {"注销 IPC 命名空间: prefix={}, count={}", {
            "[{}] 注销 IPC 命名空间: prefix={}, count={}",
            "[{}] Unregister IPC namespace: prefix={}, count={}"
        }},
        {"拒绝过大的 IPC 消息: {} bytes", {
            "[{}] 拒绝过大的 IPC 消息: {} bytes",
            "[{}] operation IPC operation: {} bytes"
        }},
        {"收到前端消息: id={}, method={}", {
            "[{}] 收到前端消息: id={}, method={}",
            "[{}] Received frontend IPC message: id={}, method={}"
        }},
        {"未知的 IPC 方法: {}", {
            "[{}] 未知的 IPC 方法: {}",
            "[{}] Unknown IPC method: {}"
        }},
        {"IPC 响应完成: method={}, id={}, 耗时={}us", {
            "[{}] IPC 响应完成: method={}, id={}, 耗时={}us",
            "[{}] IPC response completed: method={}, id={}, duration={}us"
        }},
        {"IPC 处理器异常: id={}, error={}", {
            "[{}] IPC 处理器异常: id={}, error={}",
            "[{}] IPC operationexception : id={}, error={}"
        }},
        {"IPC 处理器未知异常: id={}", {
            "[{}] IPC 处理器未知异常: id={}",
            "[{}] IPC operationexception : id={}"
        }},
        {"异步 IPC 响应回调异常: {}", {
            "[{}] 异步 IPC 响应回调异常: {}",
            "[{}] operation IPC operationexception : {}"
        }},
        {"异步 IPC 响应回调未知异常", {
            "[{}] 异步 IPC 响应回调未知异常",
            "[{}] operation IPC operationexception"
        }},
        {"异步 IPC 线程池已启动: threads={}", {
            "[{}] 异步 IPC 线程池已启动: threads={}",
            "[{}] Async IPC thread pool started: threads={}"
        }},
        {"异步 IPC 线程池已关闭", {
            "[{}] 异步 IPC 线程池已关闭",
            "[{}] operation IPC operation"
        }},
        {"IPC 方法已标记为异步执行: {}", {
            "[{}] IPC 方法已标记为异步执行: {}",
            "[{}] IPC method marked async: {}"
        }},
        {"拒绝过大的异步 IPC 消息: {} bytes", {
            "[{}] 拒绝过大的异步 IPC 消息: {} bytes",
            "[{}] operation IPC operation: {} bytes"
        }},
        {"异步 IPC 队列过载, 丢弃最旧的待处理请求", {
            "[{}] 异步 IPC 队列过载, 丢弃最旧的待处理请求",
            "[{}] operation IPC operation, operation"
        }},
        {"事件推送器已设置", {
            "[{}] 事件推送器已设置",
            "[{}] Event emitter configured"
        }},
        {"IPC 处理器与事件推送器已清空", {
            "[{}] IPC 处理器与事件推送器已清空",
            "[{}] IPC operation"
        }},
        {"推送事件到前端: event={}", {
            "[{}] 推送事件到前端: event={}",
            "[{}] Push event to frontend: event={}"
        }},
        {"配置已重置为默认值", {
            "[{}] 配置已重置为默认值",
            "[{}] Configuration reset to default values"
        }},
        {"快捷键持久化失败且运行时回滚失败: name={}", {
            "[{}] 快捷键持久化失败且运行时回滚失败: name={}",
            "[{}] operationfailed operationfailed : name={}"
        }},
        {"内置核心 IPC 处理器注册完成（含性能监控、配置管理、系统信息、窗口枚举）", {
            "[{}] 内置核心 IPC 处理器注册完成（含性能监控、配置管理、系统信息、窗口枚举）",
            "[{}] Core IPC handlers registered (performance, config, system, window enumeration)"
        }},
        {"日志系统初始化完成, 日志目录={}", {
            "[{}] 日志系统初始化完成, 日志目录={}",
            "[{}] Logging system initialized, log directory={}"
        }},
        {"日志系统正在关闭...", {
            "[{}] 日志系统正在关闭...",
            "[{}] Logging system is shutting down..."
        }},
        {"截图完成, 尺寸={}x{}", {
            "[{}] 截图完成, 尺寸={}x{}",
            "[{}] operationcompleted , operation={}x{}"
        }},
        {"钩子安装失败, errorCode={}", {
            "[{}] 钩子安装失败, errorCode={}",
            "[{}] Hook installation failed, errorCode={}"
        }},
        {"[Lua] keyboard.sendKeys 无法解析主键: {}", {
            "[{}] [Lua] keyboard.sendKeys 无法解析主键: {}",
            "[{}] [Lua] keyboard.sendKeys operation: {}"
        }},
        {"[Lua] http: 无效的 URL: {}", {
            "[{}] [Lua] http: 无效的 URL: {}",
            "[{}] [Lua] http: operation URL: {}"
        }},
        {"[Lua] http 响应超过 {} 字节上限: {}", {
            "[{}] [Lua] http 响应超过 {} 字节上限: {}",
            "[{}] [Lua] http operation {} operation: {}"
        }},
        {"[Lua] http 请求失败: {} (err={})", {
            "[{}] [Lua] http 请求失败: {} (err={})",
            "[{}] [Lua] http operationfailed : {} (err={})"
        }},
        {"Lua 脚本引擎初始化成功 (easy.* API 已注入并启用细粒度沙箱权限防御)", {
            "[{}] Lua 脚本引擎初始化成功 (easy.* API 已注入并启用细粒度沙箱权限防御)",
            "[{}] Lua operationinitialize succeeded (easy.* API operation)"
        }},
        {"Lua 引擎初始化失败: {}", {
            "[{}] Lua 引擎初始化失败: {}",
            "[{}] Lua operationinitialize failed : {}"
        }},
        {"Lua 脚本引擎已关闭", {
            "[{}] Lua 脚本引擎已关闭",
            "[{}] Lua operation"
        }},
        {"[Lua] 引擎未初始化, 无法执行 {}", {
            "[{}] [Lua] 引擎未初始化, 无法执行 {}",
            "[{}] [Lua] operationinitialize , operation {}"
        }},
        {"[Lua] {} 执行错误: {}", {
            "[{}] [Lua] {} 执行错误: {}",
            "[{}] [Lua] {} operationerror : {}"
        }},
        {"[Lua] {} 运行时异常: {}", {
            "[{}] [Lua] {} 运行时异常: {}",
            "[{}] [Lua] {} operationexception : {}"
        }},
        {"用户已明确授权脚本 {} 权限", {
            "[{}] 用户已明确授权脚本 {} 权限",
            "[{}] User explicitly granted permission to script {}"
        }},
        {"用户拒绝授予脚本 {} 敏感权限，终止执行", {
            "[{}] 用户拒绝授予脚本 {} 敏感权限，终止执行",
            "[{}] User denied sensitive permission to script {}, execution aborted"
        }},
        {"脚本 {} 未经用户预授权，已自动限制在安全只读沙箱执行", {
            "[{}] 脚本 {} 未经用户预授权，已自动限制在安全只读沙箱执行",
            "[{}] Script {} not pre-authorized, executing in safe read-only sandbox"
        }},
        {"[Lua] 无法打开脚本文件: {}", {
            "[{}] [Lua] 无法打开脚本文件: {}",
            "[{}] [Lua] operationfile : {}"
        }},
        {"[Lua] shell.run 启动失败: {}", {
            "[{}] [Lua] shell.run 启动失败: {}",
            "[{}] [Lua] shell.run operationfailed : {}"
        }},
        {"插件已加载，拒绝重复扫描", {
            "[{}] 插件已加载，拒绝重复扫描",
            "[{}] Plugins already loaded, skipping duplicate scan"
        }},
        {"插件目录不存在: {}", {
            "[{}] 插件目录不存在: {}",
            "[{}] Plugin directory does not exist: {}"
        }},
        {"开始扫描插件目录: {}", {
            "[{}] 开始扫描插件目录: {}",
            "[{}] Scanning plugin directory: {}"
        }},
        {"扫描插件目录失败: {}, error={}", {
            "[{}] 扫描插件目录失败: {}, error={}",
            "[{}] operationplugin directory failed : {}, error={}"
        }},
        {"发现插件 DLL: {}", {
            "[{}] 发现插件 DLL: {}",
            "[{}] Discovered plugin DLL: {}"
        }},
        {"插件 ID 重复，已拒绝加载: {}", {
            "[{}] 插件 ID 重复，已拒绝加载: {}",
            "[{}] Duplicate plugin ID, load rejected: {}"
        }},
        {"插件清单校验失败: {}, error={}", {
            "[{}] 插件清单校验失败: {}, error={}",
            "[{}] Plugin manifest validation failed: {}, error={}"
        }},
        {"插件名称重复，已拒绝加载: {}", {
            "[{}] 插件名称重复，已拒绝加载: {}",
            "[{}] Duplicate plugin name, load rejected: {}"
        }},
        {"插件已禁用，完全跳过 DLL 加载: {}", {
            "[{}] 插件已禁用，完全跳过 DLL 加载: {}",
            "[{}] Plugin disabled, skipping DLL load: {}"
        }},
        {"无法加载插件 DLL: {}, error={}", {
            "[{}] 无法加载插件 DLL: {}, error={}",
            "[{}] Failed to load plugin DLL: {}, error={}"
        }},
        {"插件 DLL 未导出 ABI 握手函数: {}", {
            "[{}] 插件 DLL 未导出 ABI 握手函数: {}",
            "[{}] Plugin DLL does not export ABI handshake function: {}"
        }},
        {"插件 ABI 校验失败: {}, manifest={}, binary={}", {
            "[{}] 插件 ABI 校验失败: {}, manifest={}, binary={}",
            "[{}] Plugin ABI verification failed: {}, manifest={}, binary={}"
        }},
        {"插件 DLL 未导出 CreatePlugin: {}", {
            "[{}] 插件 DLL 未导出 CreatePlugin: {}",
            "[{}] Plugin DLL does not export CreatePlugin: {}"
        }},
        {"CreatePlugin 异常: {}, error={}", {
            "[{}] CreatePlugin 异常: {}, error={}",
            "[{}] CreatePlugin exception : {}, error={}"
        }},
        {"CreatePlugin 未知异常: {}", {
            "[{}] CreatePlugin 未知异常: {}",
            "[{}] CreatePlugin operationexception : {}"
        }},
        {"CreatePlugin 返回 null: {}", {
            "[{}] CreatePlugin 返回 null: {}",
            "[{}] CreatePlugin operation null: {}"
        }},
        {"插件元数据异常: {}, error={}", {
            "[{}] 插件元数据异常: {}, error={}",
            "[{}] Plugin metadata error: {}, error={}"
        }},
        {"插件元数据未知异常: {}", {
            "[{}] 插件元数据未知异常: {}",
            "[{}] Unknown exception reading plugin metadata: {}"
        }},
        {"插件已成功加载: {} (v{})", {
            "[{}] 插件已成功加载: {} (v{})",
            "[{}] Plugin loaded successfully: {} (v{})"
        }},
        {"初始化插件: {}", {
            "[{}] 初始化插件: {}",
            "[{}] Initializing plugin: {}"
        }},
        {"插件初始化异常: {}, error={}", {
            "[{}] 插件初始化异常: {}, error={}",
            "[{}] Plugin initialization exception: {}, error={}"
        }},
        {"插件初始化未知异常: {}", {
            "[{}] 插件初始化未知异常: {}",
            "[{}] Unknown exception initializing plugin: {}"
        }},
        {"插件初始化失败: {}", {
            "[{}] 插件初始化失败: {}",
            "[{}] Plugin initialization failed: {}"
        }},
        {"停止插件: {}", {
            "[{}] 停止插件: {}",
            "[{}] stopped plugin : {}"
        }},
        {"插件关闭异常: {}, error={}", {
            "[{}] 插件关闭异常: {}, error={}",
            "[{}] Plugin shutdown exception: {}, error={}"
        }},
        {"插件关闭未知异常: {}", {
            "[{}] 插件关闭未知异常: {}",
            "[{}] Unknown exception shutting down plugin: {}"
        }},
        {"卸载插件 DLL: {}", {
            "[{}] 卸载插件 DLL: {}",
            "[{}] operationplugin DLL: {}"
        }},
        {"PerformanceMonitor: 启动, 采样间隔={}ms", {
            "[{}] PerformanceMonitor: 启动, 采样间隔={}ms",
            "[{}] PerformanceMonitor: started, interval={}ms"
        }},
        {"PerformanceMonitor: 已停止", {
            "[{}] PerformanceMonitor: 已停止",
            "[{}] PerformanceMonitor: stopped"
        }},
        {"PerformanceMonitor: 记录延迟 [{}] = {:.2f} ms", {
            "[{}] PerformanceMonitor: 记录延迟 [{}] = {:.2f} ms",
            "[{}] PerformanceMonitor: operation [{}] = {:.2f} ms"
        }},
        {"PerformanceMonitor: 插件初始化 [{}] = {:.2f} ms", {
            "[{}] PerformanceMonitor: 插件初始化 [{}] = {:.2f} ms",
            "[{}] PerformanceMonitor: plugin initialized [{}] = {:.2f} ms"
        }},
        {"按键统计管理器初始化成功, 日期={}", {
            "[{}] 按键统计管理器初始化成功, 日期={}",
            "[{}] Keycast stats manager initialized, date={}"
        }},
        {"按键统计管理器已关闭", {
            "[{}] 按键统计管理器已关闭",
            "[{}] Keycast stats manager closed"
        }},
        {"迁移旧统计文件失败: {}", {
            "[{}] 迁移旧统计文件失败: {}",
            "[{}] Failed to migrate legacy stats file: {}"
        }},
        {"加载统计数据失败: {}", {
            "[{}] 加载统计数据失败: {}",
            "[{}] operationfailed : {}"
        }},
        {"保存统计数据失败: {}", {
            "[{}] 保存统计数据失败: {}",
            "[{}] operationfailed : {}"
        }},
        {"更新响应解析失败: {}", {
            "[{}] 更新响应解析失败: {}",
            "[{}] Failed to parse update response: {}"
        }},
        {"更新检查完成: {}", {
            "[{}] 更新检查完成: {}",
            "[{}] Update check completed: {}"
        }},
        {"ShellContextMenuService: 捕获第三方 Shell 扩展异常，已安全隔离防御", {
            "[{}] ShellContextMenuService: 捕获第三方 Shell 扩展异常，已安全隔离防御",
            "[{}] ShellContextMenuService: operation Shell operationexception ，operation"
        }},
        {"DialogEngine: 生命周期 WinEvent 钩子注册失败, error={}", {
            "[{}] DialogEngine: 生命周期 WinEvent 钩子注册失败, error={}",
            "[{}] DialogEngine: operation WinEvent operationfailed , error={}"
        }},
        {"DialogEngine: 前台窗口钩子注册失败, error={}", {
            "[{}] DialogEngine: 前台窗口钩子注册失败, error={}",
            "[{}] DialogEngine: operationwindow operationfailed , error={}"
        }},
        {"DialogEngine: 位置钩子注册失败, error={}", {
            "[{}] DialogEngine: 位置钩子注册失败, error={}",
            "[{}] DialogEngine: operationfailed , error={}"
        }},
        {"DialogEngine: 按钮调用钩子注册失败, error={}", {
            "[{}] DialogEngine: 按钮调用钩子注册失败, error={}",
            "[{}] DialogEngine: operationfailed , error={}"
        }},
        {"DialogEngine: WinEvent 线程就绪, tid={}", {
            "[{}] DialogEngine: WinEvent 线程就绪, tid={}",
            "[{}] DialogEngine: WinEvent operation, tid={}"
        }},
        {"DialogEngine: WinEvent 线程已退出", {
            "[{}] DialogEngine: WinEvent 线程已退出",
            "[{}] DialogEngine: WinEvent operation"
        }},
        {"启动文件对话框增强引擎 (per-dialog session / per-exe memory)", {
            "[{}] 启动文件对话框增强引擎 (per-dialog session / per-exe memory)",
            "[{}] operationfile operation (per-dialog session / per-exe memory)"
        }},
        {"DialogEngine: 生命周期钩子不可用，文件对话框增强无法可靠工作", {
            "[{}] DialogEngine: 生命周期钩子不可用，文件对话框增强无法可靠工作",
            "[{}] DialogEngine: operation，file operation"
        }},
        {"DialogEngine 启动完成", {
            "[{}] DialogEngine 启动完成",
            "[{}] DialogEngine operationcompleted"
        }},
        {"停止文件对话框增强引擎", {
            "[{}] 停止文件对话框增强引擎",
            "[{}] stopped file operation"
        }},
        {"DialogEngine: 收到候选对话框 SHOW, hwnd=0x{:X}, pid={}", {
            "[{}] DialogEngine: 收到候选对话框 SHOW, hwnd=0x{:X}, pid={}",
            "[{}] DialogEngine: operation SHOW, hwnd=0x{:X}, pid={}"
        }},
        {"DialogEngine: 对话框动作, hwnd=0x{:X}, action={}", {
            "[{}] DialogEngine: 对话框动作, hwnd=0x{:X}, action={}",
            "[{}] DialogEngine: operation, hwnd=0x{:X}, action={}"
        }},
        {"DialogEngine: 候选窗口不是文件对话框, hwnd=0x{:X}", {
            "[{}] DialogEngine: 候选窗口不是文件对话框, hwnd=0x{:X}",
            "[{}] DialogEngine: operationwindow operationfile operation, hwnd=0x{:X}"
        }},
        {"DialogEngine: 文件对话框会话已建立, hwnd=0x{:X}, pid={}, exe={}", {
            "[{}] DialogEngine: 文件对话框会话已建立, hwnd=0x{:X}, pid={}, exe={}",
            "[{}] DialogEngine: File dialog session established, hwnd=0x{:X}, pid={}, exe={}"
        }},
        {"DialogEngine: 文件对话框会话已建立, hwnd=0x{:X}, pid={}, exe={}, initial={}, restore={}", {
            "[{}] DialogEngine: 文件对话框会话已建立, hwnd=0x{:X}, pid={}, exe={}, initial={}, restore={}",
            "[{}] DialogEngine: file operation, hwnd=0x{:X}, pid={}, exe={}, initial={}, restore={}"
        }},
        {"DialogEngine: EXE 目录恢复完成, exe={}, path={}, success={}", {
            "[{}] DialogEngine: EXE 目录恢复完成, exe={}, path={}, success={}",
            "[{}] DialogEngine: EXE directory resumed completed , exe={}, path={}, success={}"
        }},
        {"DialogEngine: 已提交 EXE 目录记忆, exe={}, current={}, selected={}, directory={}, confirmed={}", {
            "[{}] DialogEngine: 已提交 EXE 目录记忆, exe={}, current={}, selected={}, directory={}, confirmed={}",
            "[{}] DialogEngine: operation EXE directory operation, exe={}, current={}, selected={}, directory={}, confirmed={}"
        }},
        {"DialogEngine: 对话框关闭但未提交记忆, exe={}, cancelled={}, confirmed={}, selectionChanged={}", {
            "[{}] DialogEngine: 对话框关闭但未提交记忆, exe={}, cancelled={}, confirmed={}, selectionChanged={}",
            "[{}] DialogEngine: operation, exe={}, cancelled={}, confirmed={}, selectionChanged={}"
        }},
        {"UIA: 地址栏回车后对话框意外关闭", {
            "[{}] UIA: 地址栏回车后对话框意外关闭",
            "[{}] UIA: operation"
        }},
        {"UIA: {} 已向地址栏安全提交导航: {}", {
            "[{}] UIA: {} 已向地址栏安全提交导航: {}",
            "[{}] UIA: {} operation: {}"
        }},
        {"UIA: IUIAutomation 接口不可用", {
            "[{}] UIA: IUIAutomation 接口不可用",
            "[{}] UIA: IUIAutomation operation"
        }},
        {"UIA: {} 候选未通过地址栏硬校验 id={}, belongs={}, upper={}, ", {
            "[{}] UIA: {} 候选未通过地址栏硬校验 id={}, belongs={}, upper={}, ",
            "[{}] UIA: {} operation id={}, belongs={}, upper={},"
        }},
        {"UIA: 地址栏 ValuePattern::SetValue 失败 hr=0x{:X}", {
            "[{}] UIA: 地址栏 ValuePattern::SetValue 失败 hr=0x{:X}",
            "[{}] UIA: operation ValuePattern::SetValue failed hr=0x{:X}"
        }},
        {"UIA: {} 未能发送到目标对话框", {
            "[{}] UIA: {} 未能发送到目标对话框",
            "[{}] UIA: {} operation"
        }},
        {"UIA: 所有地址栏激活方式均未通过安全校验，本次不导航", {
            "[{}] UIA: 所有地址栏激活方式均未通过安全校验，本次不导航",
            "[{}] UIA: operation，operation"
        }},
        {"拒绝导航到不存在或非目录路径: {}", {
            "[{}] 拒绝导航到不存在或非目录路径: {}",
            "[{}] operationdirectory operation: {}"
        }},
        {"执行文件对话框导航: hwnd=0x{:X}, type={}, targetPath={}", {
            "[{}] 执行文件对话框导航: hwnd=0x{:X}, type={}, targetPath={}",
            "[{}] operationfile operation: hwnd=0x{:X}, type={}, targetPath={}"
        }},
        {"UIA: Modern 对话框导航失败，目标路径={}", {
            "[{}] UIA: Modern 对话框导航失败，目标路径={}",
            "[{}] UIA: Modern operationfailed ，operation={}"
        }},
        {"Legacy: 未找到底部输入控件, hwnd=0x{:X}", {
            "[{}] Legacy: 未找到底部输入控件, hwnd=0x{:X}",
            "[{}] Legacy: operation, hwnd=0x{:X}"
        }},
        {"Legacy 导航导致对话框意外关闭，已停止后续操作: {}", {
            "[{}] Legacy 导航导致对话框意外关闭，已停止后续操作: {}",
            "[{}] Legacy operation，stopped operation: {}"
        }},
        {"Legacy: 已提交目录导航并恢复底部输入内容: {}", {
            "[{}] Legacy: 已提交目录导航并恢复底部输入内容: {}",
            "[{}] Legacy: operationdirectory operationresumed operation: {}"
        }},
        {"创建 DialogRibbonOverlay 窗口失败", {
            "[{}] 创建 DialogRibbonOverlay 窗口失败",
            "[{}] create  DialogRibbonOverlay window failed "
        }},
        {"doUpdatePosition 异常: {}", {
            "[{}] doUpdatePosition 异常: {}",
            "[{}] doUpdatePosition exception : {}"
        }},
        {"doUpdatePosition 未知异常", {
            "[{}] doUpdatePosition 未知异常",
            "[{}] doUpdatePosition operationexception"
        }},
        {"初始化文件对话框增强插件 (DialogEnhancerPlugin)", {
            "[{}] 初始化文件对话框增强插件 (DialogEnhancerPlugin)",
            "[{}] initialize file operationplugin (DialogEnhancerPlugin)"
        }},
        {"启动 DialogEngine 失败", {
            "[{}] 启动 DialogEngine 失败",
            "[{}] operation DialogEngine failed"
        }},
        {"关闭文件对话框增强插件 (DialogEnhancerPlugin)", {
            "[{}] 关闭文件对话框增强插件 (DialogEnhancerPlugin)",
            "[{}] operationfile operationplugin (DialogEnhancerPlugin)"
        }},
        {"窗口置顶切换: {} -> {}", {
            "[{}] 窗口置顶切换: {} -> {}",
            "[{}] Window top-most toggled: {} -> {}"
        }},
        {"窗口透明度切换: 开启 (70%)", {
            "[{}] 窗口透明度切换: 开启 (70%)",
            "[{}] Window opacity toggled: On (70%)"
        }},
        {"窗口透明度切换: 关闭 (不透明)", {
            "[{}] 窗口透明度切换: 关闭 (不透明)",
            "[{}] Window opacity toggled: Off (Opaque)"
        }},
        {"应用级内置命令未注册 Handler: cmd={}", {
            "[{}] 应用级内置命令未注册 Handler: cmd={}",
            "[{}] operation Handler: cmd={}"
        }},
        {"关闭窗口已投递: {}", {
            "[{}] 关闭窗口已投递: {}",
            "[{}] operationwindow operation: {}"
        }},
        {"手势目标窗口不可用或属于覆盖层: {}", {
            "[{}] 手势目标窗口不可用或属于覆盖层: {}",
            "[{}] operationwindow operation: {}"
        }},
        {"手势目标窗口未能取得前台: hwnd=0x{:X}, class={}, fg=0x{:X}", {
            "[{}] 手势目标窗口未能取得前台: hwnd=0x{:X}, class={}, fg=0x{:X}",
            "[{}] operationwindow operation: hwnd=0x{:X}, class={}, fg=0x{:X}"
        }},
        {"KeyStroke::send 被调用但 virtualKey 为空, 跳过", {
            "[{}] KeyStroke::send 被调用但 virtualKey 为空, 跳过",
            "[{}] KeyStroke::send operation virtualKey operation, operation"
        }},
        {"拒绝向手势覆盖层注入按键: keys={}, target={}", {
            "[{}] 拒绝向手势覆盖层注入按键: keys={}, target={}",
            "[{}] operation: keys={}, target={}"
        }},
        {"手势按键注入: keys={}, target={}, fg={}", {
            "[{}] 手势按键注入: keys={}, target={}, fg={}",
            "[{}] operation: keys={}, target={}, fg={}"
        }},
        {"无外部目标窗口，放弃按键注入: keys={}, fg={}", {
            "[{}] 无外部目标窗口，放弃按键注入: keys={}, fg={}",
            "[{}] No external target window, aborting key injection: keys={}, fg={}"
        }},
        {"目标窗口完整性更高，关闭可能被 UIPI 拦截: {}", {
            "[{}] 目标窗口完整性更高，关闭可能被 UIPI 拦截: {}",
            "[{}] Target window has higher integrity, close may be blocked by UIPI: {}"
        }},
        {"关闭已生效，不再补发 Alt+F4: {}", {
            "[{}] 关闭已生效，不再补发 Alt+F4: {}",
            "[{}] operation，operation Alt+F4: {}"
        }},
        {"关闭投递后窗口仍在，补发 Alt+F4: {}", {
            "[{}] 关闭投递后窗口仍在，补发 Alt+F4: {}",
            "[{}] operationwindow operation，operation Alt+F4: {}"
        }},
        {"目标窗口完整性更高，按键注入可能被 UIPI 拦截: keys={}, target={}", {
            "[{}] 目标窗口完整性更高，按键注入可能被 UIPI 拦截: keys={}, target={}",
            "[{}] Target window has higher integrity, key injection may be blocked by UIPI: keys={}, target={}"
        }},
        {"未能激活目标窗口，仅保留已投递的关闭消息: target={}", {
            "[{}] 未能激活目标窗口，仅保留已投递的关闭消息: target={}",
            "[{}] Failed to activate target window, keeping dispatched close message: target={}"
        }},
        {"未能激活目标窗口，放弃注入: keys={}, target={}, fg={}", {
            "[{}] 未能激活目标窗口，放弃注入: keys={}, target={}, fg={}",
            "[{}] Failed to activate target window, aborting key injection: keys={}, target={}, fg={}"
        }},
        {"SendInput 未完全发送: expected={}, sent={}, lastError={}", {
            "[{}] SendInput 未完全发送: expected={}, sent={}, lastError={}",
            "[{}] SendInput operation: expected={}, sent={}, lastError={}"
        }},
        {"执行手势动作: SendKeys, keys={}", {
            "[{}] 执行手势动作: SendKeys, keys={}",
            "[{}] operation: SendKeys, keys={}"
        }},
        {"执行手势动作: LuaScript, name={}, script={}", {
            "[{}] 执行手势动作: LuaScript, name={}, script={}",
            "[{}] operation: LuaScript, name={}, script={}"
        }},
        {"执行手势动作: BuiltinCommand, cmd={}", {
            "[{}] 执行手势动作: BuiltinCommand, cmd={}",
            "[{}] operation: BuiltinCommand, cmd={}"
        }},
        {"执行手势动作: RunProgram, path={}", {
            "[{}] 执行手势动作: RunProgram, path={}",
            "[{}] operation: RunProgram, path={}"
        }},
        {"手势引擎启动失败: 无法安装鼠标钩子", {
            "[{}] 手势引擎启动失败: 无法安装鼠标钩子",
            "[{}] operationfailed : operation"
        }},
        {"手势轨迹覆盖层不可用，手势识别与动作执行将继续运行", {
            "[{}] 手势轨迹覆盖层不可用，手势识别与动作执行将继续运行",
            "[{}] operation，operation"
        }},
        {"手势引擎已启动, 默认Profile手势数={}, 浏览器Profile手势数={}", {
            "[{}] 手势引擎已启动, 默认Profile手势数={}, 浏览器Profile手势数={}",
            "[{}] Gesture engine started, default profile gestures={}, browser profile gestures={}"
        }},
        {"手势引擎已停止", {
            "[{}] 手势引擎已停止",
            "[{}] operationstopped"
        }},
        {"无法监视前台窗口，高完整性窗口将不会提示手势不可用", {
            "[{}] 无法监视前台窗口，高完整性窗口将不会提示手势不可用",
            "[{}] Unable to monitor foreground window, high integrity windows will not prompt gesture status"
        }},
        {"无法在主线程安装前台窗口监视", {
            "[{}] 无法在主线程安装前台窗口监视",
            "[{}] Failed to install foreground window monitor on main thread"
        }},
        {"前台窗口完整性更高，低完整性 WH_MOUSE_LL 收不到事件: hwnd=0x{:X} class={} pid={}", {
            "[{}] 前台窗口完整性更高，低完整性 WH_MOUSE_LL 收不到事件: hwnd=0x{:X} class={} pid={}",
            "[{}] operationwindow operation，operation WH_MOUSE_LL operation: hwnd=0x{:X} class={} pid={}"
        }},
        {"手势暂停状态持久化失败，已回滚: paused={}", {
            "[{}] 手势暂停状态持久化失败，已回滚: paused={}",
            "[{}] operationpaused operationfailed ，operation: paused={}"
        }},
        {"手势暂停状态回调异常，已回滚: {}", {
            "[{}] 手势暂停状态回调异常，已回滚: {}",
            "[{}] operationpaused operationexception ，operation: {}"
        }},
        {"手势暂停状态回调发生未知异常，已回滚", {
            "[{}] 手势暂停状态回调发生未知异常，已回滚",
            "[{}] operationpaused operationexception ，operation"
        }},
        {"手势引擎暂停状态: paused={}", {
            "[{}] 手势引擎暂停状态: paused={}",
            "[{}] operationpaused operation: paused={}"
        }},
        {"手势引擎录制模式更新: recording={}", {
            "[{}] 手势引擎录制模式更新: recording={}",
            "[{}] operation: recording={}"
        }},
        {"手势触发按钮已设置: {}", {
            "[{}] 手势触发按钮已设置: {}",
            "[{}] operation: {}"
        }},
        {"手势轨迹显示状态: visible={}", {
            "[{}] 手势轨迹显示状态: visible={}",
            "[{}] operation: visible={}"
        }},
        {"手势全屏自动免打扰状态: enable={}", {
            "[{}] 手势全屏自动免打扰状态: enable={}",
            "[{}] operation: enable={}"
        }},
        {"手势目标窗口模式: {}", {
            "[{}] 手势目标窗口模式: {}",
            "[{}] operationwindow operation: {}"
        }},
        {"手势起始超时已设置: initialTimeoutMs={}ms", {
            "[{}] 手势起始超时已设置: initialTimeoutMs={}ms",
            "[{}] operation: initialTimeoutMs={}ms"
        }},
        {"手势最小识别距离已设置: minSegmentDistance={}px", {
            "[{}] 手势最小识别距离已设置: minSegmentDistance={}px",
            "[{}] operation: minSegmentDistance={}px"
        }},
        {"设置手势配置集: name={}, 手势数={}", {
            "[{}] 设置手势配置集: name={}, 手势数={}",
            "[{}] Set gesture profile: name={}, gesturesCount={}"
        }},
        {"手势引擎处于暂停或录制模式，放行所有按键", {
            "[{}] 手势引擎处于暂停或录制模式，放行所有按键",
            "[{}] operationpaused operation，operation"
        }},
        {"触发键按下: trigger={}, pos=({},{}), hwnd=0x{:X}", {
            "[{}] 触发键按下: trigger={}, pos=({},{}), hwnd=0x{:X}",
            "[{}] Trigger key down: trigger={}, pos=({},{}), hwnd=0x{:X}"
        }},
        {"前台窗口处于全屏独占，手势引擎自动放行: hwnd=0x{:X} class={}", {
            "[{}] 前台窗口处于全屏独占，手势引擎自动放行: hwnd=0x{:X} class={}",
            "[{}] operationwindow operation，operation: hwnd=0x{:X} class={}"
        }},
        {"全屏但是生产力窗口，继续手势: hwnd=0x{:X} class={}", {
            "[{}] 全屏但是生产力窗口，继续手势: hwnd=0x{:X} class={}",
            "[{}] operationwindow ，operation: hwnd=0x{:X} class={}"
        }},
        {"窗口在手势黑名单, 不拦截: process='{}', class='{}'", {
            "[{}] 窗口在手势黑名单, 不拦截: process='{}', class='{}'",
            "[{}] Window in gesture blacklist, skipping: process='{}', class='{}'"
        }},
        {"GestureEngine 发生未捕获异常: {}", {
            "[{}] GestureEngine 发生未捕获异常: {}",
            "[{}] GestureEngine operationexception : {}"
        }},
        {"GestureEngine 发生未知异常", {
            "[{}] GestureEngine 发生未知异常",
            "[{}] GestureEngine operationexception"
        }},
        {"手势追踪开始: pos=({},{}) hwnd=0x{:X} class={} compositorSurface={}", {
            "[{}] 手势追踪开始: pos=({},{}) hwnd=0x{:X} class={} compositorSurface={}",
            "[{}] operationstarting : pos=({},{}) hwnd=0x{:X} class={} compositorSurface={}"
        }},
        {"手势追踪开始: pos=({},{}), modifiers=0x{:02X}, edgeZone={}, trailVisible={}", {
            "[{}] 手势追踪开始: pos=({},{}), modifiers=0x{:02X}, edgeZone={}, trailVisible={}",
            "[{}] operationstarting : pos=({},{}), modifiers=0x{:02X}, edgeZone={}, trailVisible={}"
        }},
        {"手势绘制超过 10 秒后松手结束 ({}ms)，红底调侃淡出", {
            "[{}] 手势绘制超过 10 秒后松手结束 ({}ms)，红底调侃淡出",
            "[{}] operation 10 operation ({}ms)，operation"
        }},
        {"手势追踪结束: 轨迹太短，还原为普通点击", {
            "[{}] 手势追踪结束: 轨迹太短，还原为普通点击",
            "[{}] operation: operation，operation"
        }},
        {"手势追踪结束: 滚轮手势已执行，成功抑制右键上下文菜单", {
            "[{}] 手势追踪结束: 滚轮手势已执行，成功抑制右键上下文菜单",
            "[{}] Gesture tracking ended: wheel gesture executed, context menu suppressed"
        }},
        {"手势识别成功: code={}, fullCode={}, arrows={}, 点数={}, 距离={:.0f}px", {
            "[{}] 手势识别成功: code={}, fullCode={}, arrows={}, 点数={}, 距离={:.0f}px",
            "[{}] operationsucceeded : code={}, fullCode={}, arrows={}, operation={}, operation={:.0f}px"
        }},
        {"手势在当前窗口被禁用", {
            "[{}] 手势在当前窗口被禁用",
            "[{}] operationwindow operation"
        }},
        {"执行手势动作: gesture={}, matchedCode={}, action={}, profile={}", {
            "[{}] 执行手势动作: gesture={}, matchedCode={}, action={}, profile={}",
            "[{}] operation: gesture={}, matchedCode={}, action={}, profile={}"
        }},
        {"手势选窗: mode={} start=({},{}) end=({},{}) hwnd=0x{:X} class={} exe={}", {
            "[{}] 手势选窗: mode={} start=({},{}) end=({},{}) hwnd=0x{:X} class={} exe={}",
            "[{}] operation: mode={} start=({},{}) end=({},{}) hwnd=0x{:X} class={} exe={}"
        }},
        {"手势动作开始执行(输入线程): action={}, type={}, targetHwnd=0x{:X}", {
            "[{}] 手势动作开始执行(输入线程): action={}, type={}, targetHwnd=0x{:X}",
            "[{}] operationstarting operation(operation): action={}, type={}, targetHwnd=0x{:X}"
        }},
        {"手势动作执行完毕: action={}", {
            "[{}] 手势动作执行完毕: action={}",
            "[{}] operation: action={}"
        }},
        {"手势动作执行异常: action={}, error={}", {
            "[{}] 手势动作执行异常: action={}, error={}",
            "[{}] operationexception : action={}, error={}"
        }},
        {"手势动作执行未知异常: action={}", {
            "[{}] 手势动作执行未知异常: action={}",
            "[{}] operationexception : action={}"
        }},
        {"未找到手势映射: fullCode={}, bareCode={}", {
            "[{}] 未找到手势映射: fullCode={}, bareCode={}",
            "[{}] Gesture mapping not found: fullCode={}, bareCode={}"
        }},
        {"检测到手势物理按键已松开但抬起事件丢失，自动恢复状态机: triggerVk=0x{:X}", {
            "[{}] 检测到手势物理按键已松开但抬起事件丢失，自动恢复状态机: triggerVk=0x{:X}",
            "[{}] Detected physical gesture trigger button released but UP event lost, auto-recovering state machine: triggerVk=0x{:X}"
        }},
        {"滚轮切换虚拟桌面节流中 (间隔 {}ms < 120ms)，抑制过频切换", {
            "[{}] 滚轮切换虚拟桌面节流中 (间隔 {}ms < 120ms)，抑制过频切换",
            "[{}] Wheel virtual desktop switch throttled (interval {}ms < 120ms), suppressing excessive switching"
        }},
        {"手势动作队列已满，丢弃最旧动作", {
            "[{}] 手势动作队列已满，丢弃最旧动作",
            "[{}] operation，operation"
        }},
        {"手势动作开始执行(后台队列): action={}, type={}, targetHwnd=0x{:X}", {
            "[{}] 手势动作开始执行(后台队列): action={}, type={}, targetHwnd=0x{:X}",
            "[{}] operationstarting operation(operation): action={}, type={}, targetHwnd=0x{:X}"
        }},
        {"无手势, 补发{}键点击以还原正常操作", {
            "[{}] 无手势, 补发{}键点击以还原正常操作",
            "[{}] No gesture recognized, dispatching {} click to restore normal action"
        }},
        {"补发{}键点击失败: sent={}, error={}", {
            "[{}] 补发{}键点击失败: sent={}, error={}",
            "[{}] Failed to replay {} key click: sent={}, error={}"
        }},
        {"主线程延迟队列不可用，立即补发触发键点击", {
            "[{}] 主线程延迟队列不可用，立即补发触发键点击",
            "[{}] operation，operation"
        }},
        {"手势追踪已取消", {
            "[{}] 手势追踪已取消",
            "[{}] operation"
        }},
        {"指定的 Profile 不存在: {}, 使用默认 Profile", {
            "[{}] 指定的 Profile 不存在: {}, 使用默认 Profile",
            "[{}] operation Profile operation: {}, operation Profile"
        }},
        {"从配置加载手势配置集, 数量={}", {
            "[{}] 从配置加载手势配置集, 数量={}",
            "[{}] Loaded gesture profiles from config, count={}"
        }},
        {"手势配置已保存", {
            "[{}] 手势配置已保存",
            "[{}] operationconfig operation"
        }},
        {"手势配置持久化失败", {
            "[{}] 手势配置持久化失败",
            "[{}] operationconfig operationfailed"
        }},
        {"覆盖手势映射: profile={}, code={}, action={}", {
            "[{}] 覆盖手势映射: profile={}, code={}, action={}",
            "[{}] Override gesture mapping: profile={}, code={}, action={}"
        }},
        {"添加手势映射: profile={}, code={}, action={}", {
            "[{}] 添加手势映射: profile={}, code={}, action={}",
            "[{}] Add gesture mapping: profile={}, code={}, action={}"
        }},
        {"设置手势状态: profile={}, code={}, enabled={}", {
            "[{}] 设置手势状态: profile={}, code={}, enabled={}",
            "[{}] Set gesture state: profile={}, code={}, enabled={}"
        }},
        {"手势已禁用，忽略动作: profile={}, code={}", {
            "[{}] 手势已禁用，忽略动作: profile={}, code={}",
            "[{}] operation，operation: profile={}, code={}"
        }},
        {"设置触发方式状态: profile={}, trigger={}, state={}", {
            "[{}] 设置触发方式状态: profile={}, trigger={}, state={}",
            "[{}] Set trigger state: profile={}, trigger={}, state={}"
        }},
        {"批量设置触发方式: profile={}, state={}", {
            "[{}] 批量设置触发方式: profile={}, state={}",
            "[{}] operation: profile={}, state={}"
        }},
        {"创建默认全局手势配置集, 手势数量={}", {
            "[{}] 创建默认全局手势配置集, 手势数量={}",
            "[{}] create operationconfig operation, operationcount ={}"
        }},
        {"创建浏览器专用手势配置集, 手势数量={}", {
            "[{}] 创建浏览器专用手势配置集, 手势数量={}",
            "[{}] create operationconfig operation, operationcount ={}"
        }},
        {"创建桌面专属手势配置集, 手势数量={}", {
            "[{}] 创建桌面专属手势配置集, 手势数量={}",
            "[{}] create operationconfig operation, operationcount ={}"
        }},
        {"创建任务栏专属手势配置集, 手势数量={}", {
            "[{}] 创建任务栏专属手势配置集, 手势数量={}",
            "[{}] create operationconfig operation, operationcount ={}"
        }},
        {"手势识别: 检测到乱晃/原地反悔操作，已自动取消手势执行", {
            "[{}] 手势识别: 检测到乱晃/原地反悔操作，已自动取消手势执行",
            "[{}] operation: operation/operation，operation"
        }},
        {"手势识别: 轨迹太短或无有效方向段, 点数={}", {
            "[{}] 手势识别: 轨迹太短或无有效方向段, 点数={}",
            "[{}] operation: operation, operation={}"
        }},
        {"手势识别完成: code={}, arrows={}, 点数={}, 总距离={:.1f}px", {
            "[{}] 手势识别完成: code={}, arrows={}, 点数={}, 总距离={:.1f}px",
            "[{}] operationcompleted : code={}, arrows={}, operation={}, operation={:.1f}px"
        }},
        {"创建手势轨迹覆盖层窗口失败", {
            "[{}] 创建手势轨迹覆盖层窗口失败",
            "[{}] create operationwindow failed"
        }},
        {"手势轨迹覆盖层初始化成功 (专用异步渲染管线已启动)", {
            "[{}] 手势轨迹覆盖层初始化成功 (专用异步渲染管线已启动)",
            "[{}] Gesture trail overlay initialized (Dedicated async render pipeline started)"
        }},
        {"手势轨迹覆盖层已关闭", {
            "[{}] 手势轨迹覆盖层已关闭",
            "[{}] operation"
        }},
        {"手势轨迹结束: overlayPoints={}, label={}", {
            "[{}] 手势轨迹结束: overlayPoints={}, label={}",
            "[{}] operation: overlayPoints={}, label={}"
        }},
        {"手势轨迹表面重建: {}x{} at ({},{})", {
            "[{}] 手势轨迹表面重建: {}x{} at ({},{})",
            "[{}] operation: {}x{} at ({},{})"
        }},
        {"手势覆盖层提交失败: {}x{} error={}", {
            "[{}] 手势覆盖层提交失败: {}x{} error={}",
            "[{}] operationfailed : {}x{} error={}"
        }},
        {"创建手势轨迹窗口失败", {
            "[{}] 创建手势轨迹窗口失败",
            "[{}] create operationwindow failed"
        }},
        {"创建手势结果卡片窗口失败，轨迹仍可绘制", {
            "[{}] 创建手势结果卡片窗口失败，轨迹仍可绘制",
            "[{}] create operationwindow failed ，operation"
        }},
        {"手势覆盖层初始化完成 (原生分层窗口硬件合成加速管线已就绪)", {
            "[{}] 手势覆盖层初始化完成 (原生分层窗口硬件合成加速管线已就绪)",
            "[{}] Gesture overlay initialized (Native layered window hardware compositing pipeline ready)"
        }},
        {"手势轨迹 Direct2D 帧提交失败", {
            "[{}] 手势轨迹 Direct2D 帧提交失败",
            "[{}] operation Direct2D operationfailed"
        }},
        {"手势轨迹已提交: points={}, {}x{}", {
            "[{}] 手势轨迹已提交: points={}, {}x{}",
            "[{}] operation: points={}, {}x{}"
        }},
        {"手势结果卡片 Direct2D 帧提交失败", {
            "[{}] 手势结果卡片 Direct2D 帧提交失败",
            "[{}] operation Direct2D operationfailed"
        }},
        {"HotCornerEngine: 屏幕触发角引擎已启动", {
            "[{}] HotCornerEngine: 屏幕触发角引擎已启动",
            "[{}] HotCornerEngine: hot corner engine started"
        }},
        {"HotCornerEngine: 屏幕触发角引擎已停止", {
            "[{}] HotCornerEngine: 屏幕触发角引擎已停止",
            "[{}] HotCornerEngine: operationstopped"
        }},
        {"前台处于全屏独占，触发角自动静默: hwnd=0x{:X}", {
            "[{}] 前台处于全屏独占，触发角自动静默: hwnd=0x{:X}",
            "[{}] operation，operation: hwnd=0x{:X}"
        }},
        {"HotCornerEngine: 触发角生效角={}, 执行命令='{}'", {
            "[{}] HotCornerEngine: 触发角生效角={}, 执行命令='{}'",
            "[{}] HotCornerEngine: operation={}, operation='{}'"
        }},
        {"HotCornerEngine: 忽略无效动作 '{}': {}", {
            "[{}] HotCornerEngine: 忽略无效动作 '{}': {}",
            "[{}] HotCornerEngine: operation '{}': {}"
        }},
        {"鼠标钩子已安装, 跳过重复安装", {
            "[{}] 鼠标钩子已安装, 跳过重复安装",
            "[{}] Mouse hook already installed, skipping duplicate installation"
        }},
        {"安装鼠标钩子失败, error={}", {
            "[{}] 安装鼠标钩子失败, error={}",
            "[{}] operationfailed , error={}"
        }},
        {"低级鼠标钩子已安装", {
            "[{}] 低级鼠标钩子已安装",
            "[{}] Low-level mouse hook installed"
        }},
        {"低级鼠标钩子已卸载", {
            "[{}] 低级鼠标钩子已卸载",
            "[{}] operation"
        }},
        {"鼠标钩子暂停状态: paused={}", {
            "[{}] 鼠标钩子暂停状态: paused={}",
            "[{}] Mouse hook paused state: paused={}"
        }},
        {"鼠标手势触发模式已更新: mode={}", {
            "[{}] 鼠标手势触发模式已更新: mode={}",
            "[{}] Mouse gesture trigger mode updated: mode={}"
        }},
        {"鼠标钩子熔断冷却期满，自动尝试恢复工作状态", {
            "[{}] 鼠标钩子熔断冷却期满，自动尝试恢复工作状态",
            "[{}] Mouse hook breaker cooldown elapsed, restoring active state"
        }},
        {"MouseHook 发生未捕获异常: {}", {
            "[{}] MouseHook 发生未捕获异常: {}",
            "[{}] MouseHook operationexception : {}"
        }},
        {"MouseHook 发生未知异常", {
            "[{}] MouseHook 发生未知异常",
            "[{}] MouseHook operationexception"
        }},
        {"鼠标钩子回调执行耗时过长: {} ms ({} us) type={}", {
            "[{}] 鼠标钩子回调执行耗时过长: {} ms ({} us) type={}",
            "[{}] Mouse hook callback execution exceeded budget: {} ms ({} us) type={}"
        }},
        {"【熔断告警】鼠标钩子回调耗时 {} ms (阈值 {} ms)，触发全局熔断机制保护系统！", {
            "[{}] 【熔断告警】鼠标钩子回调耗时 {} ms (阈值 {} ms)，触发全局熔断机制保护系统！",
            "[{}] 【operation】operation {} ms (operation {} ms)，operation！"
        }},
        {"鼠标钩子熔断复位回调异常", {
            "[{}] 鼠标钩子熔断复位回调异常",
            "[{}] Mouse hook breaker reset callback exception"
        }},
        {"GesturePlugin: 初始化手势引擎", {
            "[{}] GesturePlugin: 初始化手势引擎",
            "[{}] GesturePlugin: initialize operation"
        }},
        {"GesturePlugin: 内置命令触发轮盘菜单", {
            "[{}] GesturePlugin: 内置命令触发轮盘菜单",
            "[{}] GesturePlugin: operation"
        }},
        {"IPC: hotcorner.getSettings 查询触发角配置", {
            "[{}] IPC: hotcorner.getSettings 查询触发角配置",
            "[{}] IPC: hotcorner.getSettings operationconfig"
        }},
        {"IPC: hotcorner.updateSettings 更新触发角配置", {
            "[{}] IPC: hotcorner.updateSettings 更新触发角配置",
            "[{}] IPC: hotcorner.updateSettings operationconfig"
        }},
        {"IPC: radialmenu.getItems 查询轮盘菜单项", {
            "[{}] IPC: radialmenu.getItems 查询轮盘菜单项",
            "[{}] IPC: radialmenu.getItems operation"
        }},
        {"IPC: radialmenu.updateItems 更新轮盘菜单项", {
            "[{}] IPC: radialmenu.updateItems 更新轮盘菜单项",
            "[{}] IPC: radialmenu.updateItems operation"
        }},
        {"radialmenu.updateItems: 缺少 items 数组参数", {
            "[{}] radialmenu.updateItems: 缺少 items 数组参数",
            "[{}] radialmenu.updateItems: operation items operation"
        }},
        {"radialmenu.updateItems: 已更新 {} 个菜单项", {
            "[{}] radialmenu.updateItems: 已更新 {} 个菜单项",
            "[{}] radialmenu.updateItems: operation {} operation"
        }},
        {"GesturePlugin: 从配置加载 {} 个轮盘菜单项", {
            "[{}] GesturePlugin: 从配置加载 {} 个轮盘菜单项",
            "[{}] GesturePlugin: operationconfig operation {} operation"
        }},
        {"GesturePlugin: 使用默认轮盘菜单项, 数量={}", {
            "[{}] GesturePlugin: 使用默认轮盘菜单项, 数量={}",
            "[{}] GesturePlugin: using default pie menu items, count={}"
        }},
        {"GesturePlugin: 手势引擎启动失败", {
            "[{}] GesturePlugin: 手势引擎启动失败",
            "[{}] GesturePlugin: operationfailed"
        }},
        {"GesturePlugin: 卸载手势引擎", {
            "[{}] GesturePlugin: 卸载手势引擎",
            "[{}] GesturePlugin: operation"
        }},
        {"手势轮盘高 DPI 渲染资源创建失败", {
            "[{}] 手势轮盘高 DPI 渲染资源创建失败",
            "[{}] operation DPI operationcreate failed"
        }},
        {"RadialMenu: 忽略未知命令 '{}'", {
            "[{}] RadialMenu: 忽略未知命令 '{}'",
            "[{}] RadialMenu: operation '{}'"
        }},
        {"添加作用域规则: id={}, name={}, process={}, class={}", {
            "[{}] 添加作用域规则: id={}, name={}, process={}, class={}",
            "[{}] Add scope rule: id={}, name={}, process={}, class={}"
        }},
        {"作用域规则已加载, 规则数量={}", {
            "[{}] 作用域规则已加载, 规则数量={}",
            "[{}] Scope rules loaded, rule count={}"
        }},
        {"ScopeRuleEngine: 缓存已失效", {
            "[{}] ScopeRuleEngine: 缓存已失效",
            "[{}] ScopeRuleEngine: operation"
        }},
        {"按键回显初始化失败: 无法创建渲染表面", {
            "[{}] 按键回显初始化失败: 无法创建渲染表面",
            "[{}] Keycast overlay initialization failed: unable to create render surface"
        }},
        {"Tools3000 v{} 启动", {
            "[{}] Tools3000 v{} 启动",
            "[{}] Tools3000 v{} started"
        }},
        {"配置管理器初始化失败，应用无法安全启动", {
            "[{}] 配置管理器初始化失败，应用无法安全启动",
            "[{}] ConfigManager initialization failed, application cannot start safely"
        }},
        {"成功从安装包初始配置同步模块开关并写入当前用户配置", {
            "[{}] 成功从安装包初始配置同步模块开关并写入当前用户配置",
            "[{}] succeeded operationconfig operationconfig"
        }},
        {"解析或应用 initial_modules.json 失败: {}", {
            "[{}] 解析或应用 initial_modules.json 失败: {}",
            "[{}] Failed to parse or apply initial_modules.json: {}"
        }},
        {"已按设置拉起管理员实例，当前进程退出", {
            "[{}] 已按设置拉起管理员实例，当前进程退出",
            "[{}] Elevated admin instance launched per settings, exiting current process"
        }},
        {"自动提权被取消或失败，以普通权限继续运行", {
            "[{}] 自动提权被取消或失败，以普通权限继续运行",
            "[{}] Elevation canceled or failed, continuing as standard user"
        }},
        {"无法创建消息窗口", {
            "[{}] 无法创建消息窗口",
            "[{}] Failed to create message window"
        }},
        {"程序启动完成，进入消息循环", {
            "[{}] 程序启动完成，进入消息循环",
            "[{}] Application startup complete, entering message loop"
        }},
        {"消息循环失败, error={}", {
            "[{}] 消息循环失败, error={}",
            "[{}] Message loop failure, error={}"
        }},
        {"收到退出消息，准备清理", {
            "[{}] 收到退出消息，准备清理",
            "[{}] Received exit message, preparing cleanup"
        }},
        {"前台处于全屏独占应用，自动免打扰跳过搜索窗口呼出: hwnd=0x{:X}", {
            "[{}] 前台处于全屏独占应用，自动免打扰跳过搜索窗口呼出: hwnd=0x{:X}",
            "[{}] operation，operationwindow operation: hwnd=0x{:X}"
        }},
        {"检测到系统任务栏重建 (TaskbarCreated)，重新注册托盘图标", {
            "[{}] 检测到系统任务栏重建 (TaskbarCreated)，重新注册托盘图标",
            "[{}] Taskbar recreated (TaskbarCreated), re-registering tray icon"
        }},
        {"检测到 Windows 系统会话锁屏或注销 (0x{:X})，主动挂起渲染并释放物理内存", {
            "[{}] 检测到 Windows 系统会话锁屏或注销 (0x{:X})，主动挂起渲染并释放物理内存",
            "[{}] Windows session locked/logged off (0x{:X}), suspending rendering and trimming memory"
        }},
        {"检测到 Windows 系统会话解锁，恢复正常工作状态", {
            "[{}] 检测到 Windows 系统会话解锁，恢复正常工作状态",
            "[{}] Windows session unlocked, resuming normal operations"
        }},
        {"OCR 引擎已就绪 (Windows.Media.Ocr)", {
            "[{}] OCR 引擎已就绪 (Windows.Media.Ocr)",
            "[{}] OCR operation (Windows.Media.Ocr)"
        }},
        {"未检测到 OCR 语言包，OCR 功能不可用。可在系统设置中添加语言的“可选功能 → 光学字符识别”", {
            "[{}] 未检测到 OCR 语言包，OCR 功能不可用。可在系统设置中添加语言的“可选功能 → 光学字符识别”",
            "[{}] No OCR language pack detected, OCR unavailable. Install OCR pack in Windows Settings -> Optional Features"
        }},
        {"OCR 引擎初始化异常: {}", {
            "[{}] OCR 引擎初始化异常: {}",
            "[{}] OCR operationinitialize exception : {}"
        }},
        {"OCR 引擎已关闭", {
            "[{}] OCR 引擎已关闭",
            "[{}] OCR operation"
        }},
        {"OCR 输入图像为空", {
            "[{}] OCR 输入图像为空",
            "[{}] OCR operation"
        }},
        {"OCR 不可用: 无语言包", {
            "[{}] OCR 不可用: 无语言包",
            "[{}] OCR operation: operation"
        }},
        {"OCR: 不支持的图像通道数: {}", {
            "[{}] OCR: 不支持的图像通道数: {}",
            "[{}] OCR: operation: {}"
        }},
        {"OCR 完成, 识别行数: {}", {
            "[{}] OCR 完成, 识别行数: {}",
            "[{}] OCR completed , operation: {}"
        }},
        {"OCR WinRT 错误: 0x{:08X} {}", {
            "[{}] OCR WinRT 错误: 0x{:08X} {}",
            "[{}] OCR WinRT error : 0x{:08X} {}"
        }},
        {"OCR 异常: {}", {
            "[{}] OCR 异常: {}",
            "[{}] OCR exception : {}"
        }},
        {"OCR 读取图片失败: {} ({})", {
            "[{}] OCR 读取图片失败: {} ({})",
            "[{}] OCR operationfailed : {} ({})"
        }},
        {"OCR: 无法加载图片 {}", {
            "[{}] OCR: 无法加载图片 {}",
            "[{}] OCR: operation {}"
        }},
        {"SearchPlugin: 无法取得当前用户 SID，拒绝创建搜索 IPC 端点", {
            "[{}] SearchPlugin: 无法取得当前用户 SID，拒绝创建搜索 IPC 端点",
            "[{}] SearchPlugin: operation SID，operationcreate operation IPC operation"
        }},
        {"SearchPlugin: 无法安全生成或保存搜索 IPC 端点令牌", {
            "[{}] SearchPlugin: 无法安全生成或保存搜索 IPC 端点令牌",
            "[{}] SearchPlugin: operation IPC operation"
        }},
        {"SearchPlugin: 无法查询 SCM 搜索服务，回退便携进程, error={}", {
            "[{}] SearchPlugin: 无法查询 SCM 搜索服务，回退便携进程, error={}",
            "[{}] SearchPlugin: operation SCM operation，operation, error={}"
        }},
        {"SearchPlugin: SCM 无法为本用户启动（无启动权限或服务已停止），回退便携索引进程", {
            "[{}] SearchPlugin: SCM 无法为本用户启动（无启动权限或服务已停止），回退便携索引进程",
            "[{}] SearchPlugin: SCM operation（operationstopped ），operationindex operation"
        }},
        {"SearchPlugin: SCM 服务未提供当前用户端点，拒绝启动第二个索引进程, error={}", {
            "[{}] SearchPlugin: SCM 服务未提供当前用户端点，拒绝启动第二个索引进程, error={}",
            "[{}] SearchPlugin: SCM operation，operationindex operation, error={}"
        }},
        {"SearchPlugin: 已请求索引服务停机", {
            "[{}] SearchPlugin: 已请求索引服务停机",
            "[{}] SearchPlugin: operationindex operation"
        }},
        {"SearchPlugin: 索引服务停机请求失败, error={}", {
            "[{}] SearchPlugin: 索引服务停机请求失败, error={}",
            "[{}] SearchPlugin: index operationfailed , error={}"
        }},
        {"SearchPlugin: 初始化搜索引擎", {
            "[{}] SearchPlugin: 初始化搜索引擎",
            "[{}] SearchPlugin: Initializing search engine"
        }},
        {"SearchPlugin: 无法解析 JSON 结果", {
            "[{}] SearchPlugin: 无法解析 JSON 结果",
            "[{}] SearchPlugin: operation JSON operation"
        }},
        {"SearchPlugin: 管道调用超时或返回空, error={}", {
            "[{}] SearchPlugin: 管道调用超时或返回空, error={}",
            "[{}] SearchPlugin: operation, error={}"
        }},
        {"SearchPlugin: 重命名失败 {} -> {}, error={}", {
            "[{}] SearchPlugin: 重命名失败 {} -> {}, error={}",
            "[{}] SearchPlugin: operationfailed {} -> {}, error={}"
        }},
        {"SearchPlugin: 关闭", {
            "[{}] SearchPlugin: 关闭",
            "[{}] SearchPlugin: Stopped"
        }},
        {"SearchPlugin: 启动 2 分钟空闲索引服务休眠看门狗 (timeout={}s)", {
            "[{}] SearchPlugin: 启动 2 分钟空闲索引服务休眠看门狗 (timeout={}s)",
            "[{}] SearchPlugin: Starting 2-minute idle index service shutdown watchdog (timeout={}s)"
        }},
        {"SearchPlugin: 启动空闲索引服务休眠看门狗 (timeout={}s)", {
            "[{}] SearchPlugin: 启动空闲索引服务休眠看门狗 (timeout={}s)",
            "[{}] SearchPlugin: Starting idle index service shutdown watchdog (timeout={}s)"
        }},
        {"SearchPlugin: 用户重新唤起搜索，已取消索引服务休眠看门狗", {
            "[{}] SearchPlugin: 用户重新唤起搜索，已取消索引服务休眠看门狗",
            "[{}] SearchPlugin: Search summoned by user, cancelled idle index service shutdown watchdog"
        }},
        {"SearchPlugin: 搜索窗口闲置达到 2 分钟且未开启常驻后台，自动安全休眠索引服务", {
            "[{}] SearchPlugin: 搜索窗口闲置达到 2 分钟且未开启常驻后台，自动安全休眠索引服务",
            "[{}] SearchPlugin: Search window idle for 2 minutes and background residency disabled, shutting down index service safely"
        }},
        {"SearchPlugin: 搜索窗口闲置达到超时时间且未开启常驻后台，自动安全休眠索引服务", {
            "[{}] SearchPlugin: 搜索窗口闲置达到超时时间且未开启常驻后台，自动安全休眠索引服务",
            "[{}] SearchPlugin: Search window idle reached timeout and background residency disabled, shutting down index service safely"
        }},
        {"SearchPlugin: 搜索窗口已隐藏且空闲超时设置为 0，立即安全休眠索引服务", {
            "[{}] SearchPlugin: 搜索窗口已隐藏且空闲超时设置为 0，立即安全休眠索引服务",
            "[{}] SearchPlugin: Search window hidden with 0-timeout, shutting down index service immediately"
        }},
        {"创建/更新托盘图标未成功，启动自愈定时器, error={}", {
            "[{}] 创建/更新托盘图标未成功，启动自愈定时器, error={}",
            "[{}] Tray icon create/update failed, starting self-healing timer, error={}"
        }},
        {"创建/更新托盘图标未成功(第{}次尝试)，启动自愈定时器, error={}, hwnd=0x{:X}", {
            "[{}] 创建/更新托盘图标未成功(第{}次尝试)，启动自愈定时器, error={}, hwnd=0x{:X}",
            "[{}] Failed to create/update tray icon (attempt {}), starting self-healing timer, error={}, hwnd=0x{:X}"
        }},
        {"系统托盘图标自愈重试成功！(历经{}次重试)", {
            "[{}] 系统托盘图标自愈重试成功！(历经{}次重试)",
            "[{}] System tray icon self-healing retry succeeded! (after {} retries)"
        }},
        {"系统托盘图标已成功创建并显示 (cbSize={})", {
            "[{}] 系统托盘图标已成功创建并显示 (cbSize={})",
            "[{}] System tray icon created and shown (cbSize={})"
        }},
        {"系统托盘图标已销毁", {
            "[{}] 系统托盘图标已销毁",
            "[{}] System tray icon destroyed"
        }},
        {"托盘菜单回调异常: menuId={}, error={}", {
            "[{}] 托盘菜单回调异常: menuId={}, error={}",
            "[{}] operationexception : menuId={}, error={}"
        }},
        {"SearchWindow: 打包 UI 缺失，已拒绝连接开发服务器", {
            "[{}] SearchWindow: 打包 UI 缺失，已拒绝连接开发服务器",
            "[{}] SearchWindow: operation UI operation，operation"
        }},
        {"设置窗口已激活（复用已有窗口）", {
            "[{}] 设置窗口已激活（复用已有窗口）",
            "[{}] Settings window activated (reused existing)"
        }},
        {"设置窗口已激活（复用已有窗口，已取消闲置销毁定时器）", {
            "[{}] 设置窗口已激活（复用已有窗口，已取消闲置销毁定时器）",
            "[{}] Settings window activated (reused existing window, idle destroy timer cancelled)"
        }},
        {"创建设置窗口失败", {
            "[{}] 创建设置窗口失败",
            "[{}] create operationwindow failed"
        }},
        {"设置窗口已创建并显示", {
            "[{}] 设置窗口已创建并显示",
            "[{}] Settings window created and shown"
        }},
        {"设置窗口已按需创建并显示", {
            "[{}] 设置窗口已按需创建并显示",
            "[{}] Settings window created on-demand and shown"
        }},
        {"设置窗口已按需创建，正等待 WebView2 首帧就绪后丝滑呈现", {
            "[{}] 设置窗口已按需创建，正等待 WebView2 首帧就绪后丝滑呈现",
            "[{}] Settings window created on-demand, awaiting WebView2 first-frame ready for smooth presentation"
        }},
        {"设置窗口后台静默预热完成", {
            "[{}] 设置窗口后台静默预热完成",
            "[{}] Settings window background warmup completed"
        }},
        {"设置窗口后台静默预热失败", {
            "[{}] 设置窗口后台静默预热失败",
            "[{}] Settings window background warmup failed"
        }},
        {"设置窗口已隐藏", {
            "[{}] 设置窗口已隐藏",
            "[{}] Settings window hidden"
        }},
        {"设置窗口已隐藏，已启动 1 分钟闲置自动销毁倒计时", {
            "[{}] 设置窗口已隐藏，已启动 1 分钟闲置自动销毁倒计时",
            "[{}] Settings window hidden, 1-minute idle auto-destroy timer started"
        }},
        {"设置窗口已隐藏（自动释放内存开关已关闭，保持后台常驻）", {
            "[{}] 设置窗口已隐藏（自动释放内存开关已关闭，保持后台常驻）",
            "[{}] Settings window hidden (auto-release disabled, keeping resident in background)"
        }},
        {"设置窗口已销毁", {
            "[{}] 设置窗口已销毁",
            "[{}] Settings window destroyed"
        }},
        {"设置窗口已彻底销毁并释放 WebView2 渲染器", {
            "[{}] 设置窗口已彻底销毁并释放 WebView2 渲染器",
            "[{}] Settings window completely destroyed and WebView2 renderer released"
        }},
        {"设置窗口已闲置 1 分钟，自动销毁 Win32 窗口并释放 WebView2 渲染进程物理内存", {
            "[{}] 设置窗口已闲置 1 分钟，自动销毁 Win32 窗口并释放 WebView2 渲染进程物理内存",
            "[{}] Settings window has been idle for 1 minute; auto-destroying Win32 window and releasing WebView2 renderer RAM"
        }},
        {"CreateWindowExW 失败, error={}", {
            "[{}] CreateWindowExW 失败, error={}",
            "[{}] CreateWindowExW failed , error={}"
        }},
        {"Win32 设置窗口已创建, size={}x{} on DPI {}", {
            "[{}] Win32 设置窗口已创建, size={}x{} on DPI {}",
            "[{}] Win32 SettingsWindow created, size={}x{} on DPI {}"
        }},
        {"WebView2 环境获取失败, hr=0x{:08X}", {
            "[{}] WebView2 环境获取失败, hr=0x{:08X}",
            "[{}] WebView2 operationfailed , hr=0x{:08X}"
        }},
        {"WebView2 控件创建已因窗口销毁而取消", {
            "[{}] WebView2 控件创建已因窗口销毁而取消",
            "[{}] WebView2 operationcreate operationwindow destroy operation"
        }},
        {"WebView2 控件创建失败, hr=0x{:08X}", {
            "[{}] WebView2 控件创建失败, hr=0x{:08X}",
            "[{}] WebView2 control creation failed, hr=0x{:08X}"
        }},
        {"WebView2 控件创建请求失败, hr=0x{:08X}", {
            "[{}] WebView2 控件创建请求失败, hr=0x{:08X}",
            "[{}] WebView2 operationcreate operationfailed , hr=0x{:08X}"
        }},
        {"读取开发服务器动态端口文件异常: {}", {
            "[{}] 读取开发服务器动态端口文件异常: {}",
            "[{}] Failed to read dynamic dev server port file: {}"
        }},
        {"读取开发服务器动态端口文件发生未知异常", {
            "[{}] 读取开发服务器动态端口文件发生未知异常",
            "[{}] Unknown exception occurred while reading dynamic dev server port file"
        }},
        {"WebView2 控件就绪", {
            "[{}] WebView2 控件就绪",
            "[{}] WebView2 control ready"
        }},
        {"WebMessageReceived 处理异常: {}", {
            "[{}] WebMessageReceived 处理异常: {}",
            "[{}] WebMessageReceived operationexception : {}"
        }},
        {"WebMessageReceived 处理未知异常", {
            "[{}] WebMessageReceived 处理未知异常",
            "[{}] WebMessageReceived operationexception"
        }},
        {"WebView2 导航成功: {}", {
            "[{}] WebView2 导航成功: {}",
            "[{}] WebView2 navigation succeeded: {}"
        }},
        {"WebView2 导航失败, status={}", {
            "[{}] WebView2 导航失败, status={}",
            "[{}] WebView2 operationfailed , status={}"
        }},
        {"WebView2 正在加载前端 UI: {}", {
            "[{}] WebView2 正在加载前端 UI: {}",
            "[{}] WebView2 loading frontend UI: {}"
        }},
        {"打包 UI 缺失: {}", {
            "[{}] 打包 UI 缺失: {}",
            "[{}] operation UI operation: {}"
        }},
        {"成功读取到动态开发服务器地址: {}", {
            "[{}] 成功读取到动态开发服务器地址: {}",
            "[{}] succeeded operation: {}"
        }},
        {"未找到本地 UI 文件及动态端口文件, 尝试连接默认开发服务器 http://localhost:5173", {
            "[{}] 未找到本地 UI 文件及动态端口文件, 尝试连接默认开发服务器 http://localhost:5173",
            "[{}] Local UI files not found, connecting to dev server http://localhost:5173"
        }},
        {"鼠标演示与特效 Overlay 初始化完成 (Taskbar Safe)", {
            "[{}] 鼠标演示与特效 Overlay 初始化完成 (Taskbar Safe)",
            "[{}] Mouse demonstration & overlay initialized (Taskbar Safe)"
        }},
        {"前台处于全屏独占应用，自动免打扰跳过鼠标聚光灯触发: hwnd=0x{:X}", {
            "[{}] 前台处于全屏独占应用，自动免打扰跳过鼠标聚光灯触发: hwnd=0x{:X}",
            "[{}] operation，operation: hwnd=0x{:X}"
        }},
        {"系统通知 Overlay 初始化完成 (Taskbar Safe)", {
            "[{}] 系统通知 Overlay 初始化完成 (Taskbar Safe)",
            "[{}] System notification overlay initialized (Taskbar Safe)"
        }},
        {"ToastOverlay 窗口过程异常: {}", {
            "[{}] ToastOverlay 窗口过程异常: {}",
            "[{}] ToastOverlay window operationexception : {}"
        }},
        {"ToastOverlay 窗口过程未知异常", {
            "[{}] ToastOverlay 窗口过程未知异常",
            "[{}] ToastOverlay window operationexception"
        }},
        {"TrayWindow: 打包 UI 缺失，已拒绝连接开发服务器", {
            "[{}] TrayWindow: 打包 UI 缺失，已拒绝连接开发服务器",
            "[{}] TrayWindow: operation UI operation，operation"
        }},
        {"TrayWindow: WebView2 导航失败, status={}", {
            "[{}] TrayWindow: WebView2 导航失败, status={}",
            "[{}] TrayWindow: WebView2 operationfailed , status={}"
        }},
        {"WebView2 环境获取回调异常: {}", {
            "[{}] WebView2 环境获取回调异常: {}",
            "[{}] WebView2 operationexception : {}"
        }},
        {"WebView2 环境获取回调未知异常", {
            "[{}] WebView2 环境获取回调未知异常",
            "[{}] WebView2 operationexception"
        }},
        {"共享 WebView2 环境创建失败, hr=0x{:08X}", {
            "[{}] 共享 WebView2 环境创建失败, hr=0x{:08X}",
            "[{}] operation WebView2 operationcreate failed , hr=0x{:08X}"
        }},
        {"共享 WebView2 环境已就绪", {
            "[{}] 共享 WebView2 环境已就绪",
            "[{}] Shared WebView2 environment ready"
        }},
        {"WebView2 环境就绪回调异常: {}", {
            "[{}] WebView2 环境就绪回调异常: {}",
            "[{}] WebView2 operationexception : {}"
        }},
        {"WebView2 环境就绪回调未知异常", {
            "[{}] WebView2 环境就绪回调未知异常",
            "[{}] WebView2 operationexception"
        }},
        {"已拒绝非可信 WebView 消息来源", {
            "[{}] 已拒绝非可信 WebView 消息来源",
            "[{}] operation WebView operation"
        }},
        {"已阻止 WebView 导航到非可信来源", {
            "[{}] 已阻止 WebView 导航到非可信来源",
            "[{}] operation WebView operation"
        }},
        {"贴图编辑底图复制异常: {}", {
            "[{}] 贴图编辑底图复制异常: {}",
            "[{}] Exception copying pinned screenshot background: {}"
        }},
        {"贴图编辑底图复制发生未知异常", {
            "[{}] 贴图编辑底图复制发生未知异常",
            "[{}] Unknown exception copying pinned screenshot background"
        }},
        {"拒绝 WebView IPC 提供的未授权配置导入路径", {
            "[{}] 拒绝 WebView IPC 提供的未授权配置导入路径",
            "[{}] Refused unauthorized config import path provided by WebView IPC"
        }},
        {"插件初始化失败后的清理异常: {}, error={}", {
            "[{}] 插件初始化失败后的清理异常: {}, error={}",
            "[{}] Exception during plugin cleanup after initialization failure: {}, error={}"
        }},
        {"插件初始化失败后的清理发生未知异常: {}", {
            "[{}] 插件初始化失败后的清理发生未知异常: {}",
            "[{}] Unknown exception during plugin cleanup after initialization failure: {}"
        }},
        {"读取活动资源管理器路径失败: {}", {
            "[{}] 读取活动资源管理器路径失败: {}",
            "[{}] Failed to read active File Explorer path: {}"
        }},
        {"读取活动资源管理器路径发生未知异常", {
            "[{}] 读取活动资源管理器路径发生未知异常",
            "[{}] Unknown exception reading active File Explorer path"
        }},
        {"构建最近路径按钮失败: {}", {
            "[{}] 构建最近路径按钮失败: {}",
            "[{}] Failed to build recent path button: {}"
        }},
        {"构建最近路径按钮发生未知异常", {
            "[{}] 构建最近路径按钮发生未知异常",
            "[{}] Unknown exception building recent path button"
        }},
        {"作用域窗口类通配符规则无效: {}", {
            "[{}] 作用域窗口类通配符规则无效: {}",
            "[{}] Invalid scope window class wildcard rule: {}"
        }},
        {"作用域窗口类通配符规则发生未知异常", {
            "[{}] 作用域窗口类通配符规则发生未知异常",
            "[{}] Unknown exception in scope window class wildcard rule"
        }},
        {"作用域窗口类正则规则无效: {}", {
            "[{}] 作用域窗口类正则规则无效: {}",
            "[{}] Invalid scope window class regex rule: {}"
        }},
        {"作用域窗口类正则规则发生未知异常", {
            "[{}] 作用域窗口类正则规则发生未知异常",
            "[{}] Unknown exception in scope window class regex rule"
        }},
        {"作用域进程名通配符规则无效: {}", {
            "[{}] 作用域进程名通配符规则无效: {}",
            "[{}] Invalid scope process name wildcard rule: {}"
        }},
        {"作用域进程名通配符规则发生未知异常", {
            "[{}] 作用域进程名通配符规则发生未知异常",
            "[{}] Unknown exception in scope process name wildcard rule"
        }},
        {"作用域进程名正则规则无效: {}", {
            "[{}] 作用域进程名正则规则无效: {}",
            "[{}] Invalid scope process name regex rule: {}"
        }},
        {"作用域进程名正则规则发生未知异常", {
            "[{}] 作用域进程名正则规则发生未知异常",
            "[{}] Unknown exception in scope process name regex rule"
        }},
        {"忽略无效的 --window-pos 参数: {}", {
            "[{}] 忽略无效的 --window-pos 参数: {}",
            "[{}] Ignored invalid --window-pos argument: {}"
        }},
        {"解析 --window-pos 参数时发生未知异常", {
            "[{}] 解析 --window-pos 参数时发生未知异常",
            "[{}] Unknown exception parsing --window-pos argument"
        }},
        {"search.rebuildIndex 返回了无效 JSON: {}", {
            "[{}] search.rebuildIndex 返回了无效 JSON: {}",
            "[{}] search.rebuildIndex returned invalid JSON: {}"
        }},
        {"search.sync 返回了无效 JSON: {}", {
            "[{}] search.sync 返回了无效 JSON: {}",
            "[{}] search.sync returned invalid JSON: {}"
        }},
        {"search.getSearchHistory 返回了无效 JSON: {}", {
            "[{}] search.getSearchHistory 返回了无效 JSON: {}",
            "[{}] search.getSearchHistory returned invalid JSON: {}"
        }},
        {"search.getDbStats 返回了无效 JSON: {}", {
            "[{}] search.getDbStats 返回了无效 JSON: {}",
            "[{}] search.getDbStats returned invalid JSON: {}"
        }},
        {"QuickLook WebMessage 处理异常: {}", {
            "[{}] QuickLook WebMessage 处理异常: {}",
            "[{}] QuickLook WebMessage handling exception: {}"
        }},
        {"QuickLook WebMessage 处理发生未知异常", {
            "[{}] QuickLook WebMessage 处理发生未知异常",
            "[{}] QuickLook WebMessage handling unknown exception"
        }},
        {"聚光灯颜色格式无效，使用默认蓝色: {}", {
            "[{}] 聚光灯颜色格式无效，使用默认蓝色: {}",
            "[{}] Invalid spotlight color format, falling back to default blue: {}"
        }},
        {"WebView2 关闭回调异常: {}", {
            "[{}] WebView2 关闭回调异常: {}",
            "[{}] WebView2 close callback exception: {}"
        }},
        {"WebView2 关闭回调发生未知异常", {
            "[{}] WebView2 关闭回调发生未知异常",
            "[{}] WebView2 close callback unknown exception"
        }},
        {"拒绝不安全的配置导出路径: {}", {
            "[{}] 拒绝不安全的配置导出路径: {}",
            "[{}] Refused unsafe config export path: {}"
        }},
        {"拒绝不安全的配置导入路径: {}", {
            "[{}] 拒绝不安全的配置导入路径: {}",
            "[{}] Refused unsafe config import path: {}"
        }},
        {"快捷键操作派发到了错误线程: operation={}, expected={}, actual={}", {
            "[{}] 快捷键操作派发到了错误线程: operation={}, expected={}, actual={}",
            "[{}] Hotkey operation dispatched to wrong thread: operation={}, expected={}, actual={}"
        }},
        {"快捷键主线程操作异常: operation={}, error={}", {
            "[{}] 快捷键主线程操作异常: operation={}, error={}",
            "[{}] Hotkey main thread operation exception: operation={}, error={}"
        }},
        {"快捷键主线程操作未知异常: operation={}", {
            "[{}] 快捷键主线程操作未知异常: operation={}",
            "[{}] Hotkey main thread operation unknown exception: operation={}"
        }},
        {"快捷键操作无法派发到窗口线程: operation={}", {
            "[{}] 快捷键操作无法派发到窗口线程: operation={}",
            "[{}] Unable to dispatch hotkey operation to window thread: operation={}"
        }},
        {"快捷键操作等待窗口线程超时: operation={}", {
            "[{}] 快捷键操作等待窗口线程超时: operation={}",
            "[{}] Timed out waiting for window thread during hotkey operation: operation={}"
        }},
        {"IPC handler 在关闭期限内未返回，已隔离保留以避免卸载 UAF: activeCalls={}", {
            "[{}] IPC handler 在关闭期限内未返回，已隔离保留以避免卸载 UAF: activeCalls={}",
            "[{}] IPC handler did not return within shutdown deadline, quarantined to prevent unload UAF: activeCalls={}"
        }},
        {"异步 IPC 响应回调异常: context={}, error={}", {
            "[{}] 异步 IPC 响应回调异常: context={}, error={}",
            "[{}] Async IPC response callback exception: context={}, error={}"
        }},
        {"异步 IPC 响应回调未知异常: context={}", {
            "[{}] 异步 IPC 响应回调未知异常: context={}",
            "[{}] Async IPC response callback unknown exception: context={}"
        }},
        {"异步 IPC 熔断器已恢复: availableWorkers={}", {
            "[{}] 异步 IPC 熔断器已恢复: availableWorkers={}",
            "[{}] Async IPC circuit breaker restored: availableWorkers={}"
        }},
        {"异步 IPC handler 超时: requestId={}, timeoutSeconds={}", {
            "[{}] 异步 IPC handler 超时: requestId={}, timeoutSeconds={}",
            "[{}] Async IPC handler timed out: requestId={}, timeoutSeconds={}"
        }},
        {"异步 IPC 所有 worker 均超时，熔断并拒绝排队请求: count={}", {
            "[{}] 异步 IPC 所有 worker 均超时，熔断并拒绝排队请求: count={}",
            "[{}] All async IPC workers timed out, circuit breaker tripped and queued requests rejected: count={}"
        }},
        {"异步 IPC 关闭时仍有 handler 卡住；已脱离线程并保留池状态以避免 join 死锁/UAF", {
            "[{}] 异步 IPC 关闭时仍有 handler 卡住；已脱离线程并保留池状态以避免 join 死锁/UAF",
            "[{}] Handlers still blocked during async IPC shutdown; detached threads and retained pool state to avoid join deadlock/UAF"
        }},
        {"拒绝 WebView IPC 提供的未授权配置导出路径", {
            "[{}] 拒绝 WebView IPC 提供的未授权配置导出路径",
            "[{}] Refused unauthorized config export path provided by WebView IPC"
        }},
        {"SearchWindow: get_CoreWebView2 失败: HRESULT=0x{:08X}", {
            "[{}] SearchWindow: get_CoreWebView2 失败: HRESULT=0x{:08X}",
            "[{}] SearchWindow: get_CoreWebView2 failed: HRESULT=0x{:08X}"
        }},
        {"SettingsWindow: get_CoreWebView2 失败, hr=0x{:08X}", {
            "[{}] SettingsWindow: get_CoreWebView2 失败, hr=0x{:08X}",
            "[{}] SettingsWindow: get_CoreWebView2 failed, hr=0x{:08X}"
        }},
        {"SettingsWindow: get_Settings 失败, hr=0x{:08X}", {
            "[{}] SettingsWindow: get_Settings 失败, hr=0x{:08X}",
            "[{}] SettingsWindow: get_Settings failed, hr=0x{:08X}"
        }},
        {"保存截图失败: 数据或路径为空", {
            "[{}] 保存截图失败: 数据或路径为空",
            "[{}] Failed to save screenshot: data or path is empty"
        }},
        {"无法创建截图保存目录: path={}, error={}", {
            "[{}] 无法创建截图保存目录: path={}, error={}",
            "[{}] Failed to create screenshot directory: path={}, error={}"
        }},
        {"原子写入或刷新截图文件失败: {}", {
            "[{}] 原子写入或刷新截图文件失败: {}",
            "[{}] Atomic write or flush of screenshot file failed: {}"
        }},
        {"录屏结束封装或原子重命名未完全成功，保留临时文件供手动恢复: {}", {
            "[{}] 录屏结束封装或原子重命名未完全成功，保留临时文件供手动恢复: {}",
            "[{}] Screen recording finalization or atomic rename was incomplete, preserved temporary file for recovery: {}"
        }},
        {"av_write_trailer 写入封装尾部失败: error={}", {
            "[{}] av_write_trailer 写入封装尾部失败: error={}",
            "[{}] av_write_trailer failed to write trailer: error={}"
        }},
        {"avio_closep 关闭输出文件失败: error={}", {
            "[{}] avio_closep 关闭输出文件失败: error={}",
            "[{}] avio_closep failed to close output file: error={}"
        }},
        {"截图覆盖层 BitBlt 受系统环境限制 (error={})，启用安全底图兜底", {
            "[{}] 截图覆盖层 BitBlt 受系统环境限制 (error={})，启用安全底图兜底",
            "[{}] Capture overlay BitBlt restricted by system environment (error={}), activating safe fallback background"
        }},
        {"创建输入线程就绪事件失败, error={}", {
            "[{}] 创建输入线程就绪事件失败, error={}",
            "[{}] Failed to create input thread ready event, error={}"
        }},
        {"安装全局独立高优先级鼠标钩子失败", {
            "[{}] 安装全局独立高优先级鼠标钩子失败",
            "[{}] Failed to install global dedicated high-priority mouse hook"
        }},
        {"全局独立高优先级输入线程已启动，鼠标钩子安装成功 (THREAD_PRIORITY_HIGHEST)", {
            "[{}] 全局独立高优先级输入线程已启动，鼠标钩子安装成功 (THREAD_PRIORITY_HIGHEST)",
            "[{}] Global dedicated high-priority input thread started, mouse hook installed (THREAD_PRIORITY_HIGHEST)"
        }},
        {"SetWindowsHookExW(WH_MOUSE_LL) 失败, error={}", {
            "[{}] SetWindowsHookExW(WH_MOUSE_LL) 失败, error={}",
            "[{}] SetWindowsHookExW(WH_MOUSE_LL) failed, error={}"
        }},
        {"全局独立输入线程鼠标钩子已卸载", {
            "[{}] 全局独立输入线程鼠标钩子已卸载",
            "[{}] Global dedicated input thread mouse hook uninstalled"
        }},
        {"MouseHook 拦截器异常: {}", {
            "[{}] MouseHook 拦截器异常: {}",
            "[{}] MouseHook interceptor exception: {}"
        }},
        {"MouseHook 拦截器未知异常", {
            "[{}] MouseHook 拦截器未知异常",
            "[{}] MouseHook interceptor unknown exception"
        }},
        {"手势异步检测命中黑名单或全屏独占，取消手势并补发原按键", {
            "[{}] 手势异步检测命中黑名单或全屏独占，取消手势并补发原按键",
            "[{}] Gesture async check hit blacklist or exclusive fullscreen, canceling gesture and reinjecting original key"
        }},
        {"手势异步检测命中黑名单或全屏独占，放行滚轮事件", {
            "[{}] 手势异步检测命中黑名单或全屏独占，放行滚轮事件",
            "[{}] Gesture async check hit blacklist or exclusive fullscreen, passing wheel event"
        }},
        {"手势抬起检测到当前窗口在手势黑名单或全屏放行，还原为原生点击", {
            "[{}] 手势抬起检测到当前窗口在手势黑名单或全屏放行，还原为原生点击",
            "[{}] Gesture release detected window in blacklist or fullscreen bypass, restored to native click"
        }},
        {"创建手势渲染同步事件失败", {
            "[{}] 创建手势渲染同步事件失败",
            "[{}] Failed to create gesture render synchronization event"
        }},
        {"渲染线程创建手势覆盖层窗口失败", {
            "[{}] 渲染线程创建手势覆盖层窗口失败",
            "[{}] Render thread failed to create gesture overlay window"
        }},
        {"创建 DirectComposition 轨迹表面失败: {}x{}, hr=0x{:X}", {
            "[{}] 创建 DirectComposition 轨迹表面失败: {}x{}, hr=0x{:X}",
            "[{}] Failed to create DirectComposition trail surface: {}x{}, hr=0x{:X}"
        }},
        {"DirectComposition 离屏表面 BeginDraw 失败, hr=0x{:X}", {
            "[{}] DirectComposition 离屏表面 BeginDraw 失败, hr=0x{:X}",
            "[{}] Failed to BeginDraw on DirectComposition offscreen surface, hr=0x{:X}"
        }},
        {"手势轨迹已提交: points={}, {}x{}, dcomp={}", {
            "[{}] 手势轨迹已提交: points={}, {}x{}, dcomp={}",
            "[{}] Gesture trail committed: points={}, {}x{}, dcomp={}"
        }},
        {"安装核心鼠标输入线程失败", {
            "[{}] 安装核心鼠标输入线程失败",
            "[{}] Failed to install core mouse input thread"
        }},
        {"手势低级鼠标钩子已接入全局单一输入源 (线程隔离 + 无锁 SPSC 环形队列已就绪)", {
            "[{}] 手势低级鼠标钩子已接入全局单一输入源 (线程隔离 + 无锁 SPSC 环形队列已就绪)",
            "[{}] Gesture low-level mouse hook connected to global single input source (thread isolation + lock-free SPSC queue ready)"
        }},
        {"手势低级鼠标钩子已卸载拦截器", {
            "[{}] 手势低级鼠标钩子已卸载拦截器",
            "[{}] Gesture low-level mouse hook uninstalled interceptor"
        }},
        {"手势鼠标钩子暂停状态: paused={}", {
            "[{}] 手势鼠标钩子暂停状态: paused={}",
            "[{}] Gesture mouse hook pause state: paused={}"
        }},
        {"预编译作用域窗口类正则失败: {}", {
            "[{}] 预编译作用域窗口类正则失败: {}",
            "[{}] Failed to precompile scope window class regex: {}"
        }},
        {"预编译作用域进程名正则失败: {}", {
            "[{}] 预编译作用域进程名正则失败: {}",
            "[{}] Failed to precompile scope process name regex: {}"
        }},
        {"呼出 Windows 原生托盘快捷菜单 (自愈降级/Shift直通管线)", {
            "[{}] 呼出 Windows 原生托盘快捷菜单 (自愈降级/Shift直通管线)",
            "[{}] Showing Windows native tray context menu (fallback / Shift pass-through pipeline)"
        }},
        {"TrayWindow: CreateCoreWebView2Controller 失败, res=0x{:08X}", {
            "[{}] TrayWindow: CreateCoreWebView2Controller 失败, res=0x{:08X}",
            "[{}] TrayWindow: CreateCoreWebView2Controller failed, res=0x{:08X}"
        }},
        {"TrayWindow: WebView2 渲染环境已就绪", {
            "[{}] TrayWindow: WebView2 渲染环境已就绪",
            "[{}] TrayWindow: WebView2 render environment ready"
        }},
        {"手势触发掩码已同步: 0x{:08X}", {
            "[{}] 手势触发掩码已同步: 0x{:08X}",
            "[{}] Gesture trigger mask synchronized: 0x{:08X}"
        }},
        {"创建 DirectComposition 前台轨迹表面失败: {}x{}, hr=0x{:X}", {
            "[{}] 创建 DirectComposition 前台轨迹表面失败: {}x{}, hr=0x{:X}",
            "[{}] Failed to create DirectComposition front trail surface: {}x{}, hr=0x{:X}"
        }},
        {"创建 DirectComposition 后台轨迹表面失败: {}x{}, hr=0x{:X}", {
            "[{}] 创建 DirectComposition 后台轨迹表面失败: {}x{}, hr=0x{:X}",
            "[{}] Failed to create DirectComposition back trail surface: {}x{}, hr=0x{:X}"
        }},
        {"在新 DirectComposition 表面上预渲染失败", {
            "[{}] 在新 DirectComposition 表面上预渲染失败",
            "[{}] Failed to prerender on new DirectComposition surface"
        }},
        {"DirectComposition 表面 BeginDraw 失败: hr=0x{:X}", {
            "[{}] DirectComposition 表面 BeginDraw 失败: hr=0x{:X}",
            "[{}] DirectComposition surface BeginDraw failed: hr=0x{:X}"
        }},
        {"创建 DirectComposition 后台翻转表面失败: hr=0x{:X}", {
            "[{}] 创建 DirectComposition 后台翻转表面失败: hr=0x{:X}",
            "[{}] Failed to create DirectComposition back flip surface: hr=0x{:X}"
        }},
        {"手势轨迹后台表面渲染失败", {
            "[{}] 手势轨迹后台表面渲染失败",
            "[{}] Failed to render gesture trail to back surface"
        }},
        {"创建 DirectComposition Toast 双表面失败: hr1=0x{:X}, hr2=0x{:X}", {
            "[{}] 创建 DirectComposition Toast 双表面失败: hr1=0x{:X}, hr2=0x{:X}",
            "[{}] Failed to create DirectComposition toast double surface: hr1=0x{:X}, hr2=0x{:X}"
        }},
        {"PinWindow: 智能计算最佳生成坐标 @ ({}, {})", {
            "[{}] PinWindow: 智能计算最佳生成坐标 @ ({}, {})",
            "[{}] PinWindow: smart calculated optimal spawn position @ ({}, {})"
        }},
    };
    return catalog;
}

const char* getLocalizedLogFormatWithTrace(std::string_view rawFmt, LogLanguage lang) {
    const auto& catalog = getCatalog();
    auto it = catalog.find(rawFmt);
    if (it != catalog.end()) {
        if (lang == LogLanguage::EnUS) {
            return it->second.enWithTrace;
        } else {
            return it->second.zhWithTrace;
        }
    }
    return nullptr;
}

}  // namespace tools3000::core

// ─────────────────────────────────────────────────────────────────────────────
// useBridge — WebView2 ↔ C++ IPC 通信 Hook
//
// 提供 request(method, params) 方法调用 C++ 处理器，
// 自动管理消息 ID、响应匹配和事件监听。
// ─────────────────────────────────────────────────────────────────────────────

import { useCallback, useEffect, useRef } from 'react';

type MessageHandler = (data: unknown) => void;

export class BridgeError extends Error {
  readonly code?: string | number;

  constructor(reason: unknown) {
    const record = reason && typeof reason === 'object' ? reason as Record<string, unknown> : null;
    super(typeof record?.message === 'string' ? record.message : String(reason || 'Bridge request failed'));
    this.name = 'BridgeError';
    if (typeof record?.code === 'string' || typeof record?.code === 'number') this.code = record.code;
  }
}

let nextId = 1;
const pendingRequests = new Map<number, {
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
  timeoutId: ReturnType<typeof setTimeout>;
}>();
const eventListeners = new Map<string, Set<MessageHandler>>();
const DEFAULT_IPC_TIMEOUT_MS = 10_000;
const STANDARD_SEARCH_TIMEOUT_MS = 20_000;
const LONG_RUNNING_TIMEOUT_MS = 5 * 60_000;
const HEAVY_SEARCH_TIMEOUT_MS = 30 * 60_000;

const LONG_RUNNING_METHODS = new Set([
  'config.export',
  'config.import',
  'capture.browseDirectory',
  'capture.saveHistoryItemAs',
  'app.exportLogs',
  'app.checkForUpdates',
  'plugins.selectExtensionFile',
  'system.pickForegroundWindow',
  'search.warmup',
]);

const HEAVY_SEARCH_METHODS = new Set([
  'search.rebuildIndex',
  'search.sync',
  'search.export',
  'search.exportCsv',
  'search.exportResults',
]);

/**
 * 按 IPC 方法语义分级计算超时预算：
 * 仅对重型索引重建/同步/导出开放 30 分钟长预算，深度全文检索开放 5 分钟预算；
 * 常规 search.query / search.getStatus / search.getHistory 等恢复 20s 快速失败，杜绝挂死 30 分钟。
 */
export function resolveBridgeTimeoutMs(method: string, params: Record<string, unknown> = {}): number {
  if (HEAVY_SEARCH_METHODS.has(method)) {
    return HEAVY_SEARCH_TIMEOUT_MS;
  }
  if (
    method === 'search.query' &&
    (params.mode === 'content' ||
      params.searchContent === true ||
      (typeof params.query === 'string' && /(?:^|\s)content:/i.test(params.query)))
  ) {
    return LONG_RUNNING_TIMEOUT_MS;
  }
  if (LONG_RUNNING_METHODS.has(method)) {
    return LONG_RUNNING_TIMEOUT_MS;
  }
  if (method.startsWith('search.')) {
    return STANDARD_SEARCH_TIMEOUT_MS;
  }
  return DEFAULT_IPC_TIMEOUT_MS;
}

// 全局消息监听（只注册一次）
let initialized = false;
function initGlobalListener() {
  if (initialized) return;
  initialized = true;

  window.chrome?.webview?.addEventListener('message', (e: MessageEvent) => {
    try {
      const msg = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;

      if (msg.type === 'event') {
        // C++ → JS 事件推送
        const listeners = eventListeners.get(msg.event);
        if (listeners) {
          listeners.forEach(handler => {
            try {
              handler(msg.data);
            } catch (error) {
              console.error(`[Bridge] Event handler failed (${String(msg.event)}):`, error);
            }
          });
        }
      } else if (msg.id !== undefined) {
        // 响应匹配
        const pending = pendingRequests.get(msg.id);
        if (pending) {
          pendingRequests.delete(msg.id);
          clearTimeout(pending.timeoutId);
          if (msg.error) {
            pending.reject(new BridgeError(msg.error));
          } else {
            pending.resolve(msg.result);
          }
        }
      }
    } catch (err) {
      console.error('[Bridge] Failed to parse message:', err);
    }
  });
}

export interface BridgeRequestOptions {
  silent?: boolean;
  toastMessage?: string;
}

const EXPLICIT_SETTINGS_METHODS = new Set([
  'general.updateSettings',
  'capture.updateSettings',
  'recording.updateSettings',
  'gesture.updateSettings',
  'gesture.updateScopeRules',
  'gesture.setTriggerState',
  'gesture.saveMappings',
  'search.saveSettings',
  'dialog.updateConfig',
  'dialog.addFavorite',
  'dialog.removeFavorite',
  'dialog.setWorkspace',
  'dialog.toggleFixState',
  'dialog.setAppFixedWorkspace',
  'dialog.addBlacklist',
  'dialog.removeBlacklist',
  'dialog.setBlacklist',
  'dialog.removeAppMemory',
  'dialog.clearAppMemories',
  'hotcorner.updateSettings',
  'ocr.updateSettings',
  'keycast.updateSettings',
  'spotlight.updateSettings',
  'config.set',
  'plugins.setEnabled',
  'hotkey.rebind',
]);

/**
 * 智能判定当前 IPC 请求是否为设置保存/更新操作
 * 包含精确匹配与通配后缀规则，天然兼容未来所有新增模块与设置项
 */
export function isSettingsMutationMethod(method: string): boolean {
  if (EXPLICIT_SETTINGS_METHODS.has(method)) return true;
  return (
    method.endsWith('.updateSettings') ||
    method.endsWith('.saveSettings') ||
    method.endsWith('.updateConfig') ||
    method.endsWith('.saveConfig') ||
    method.endsWith('.setSettings')
  );
}

let generalGetSettingsFlight: Promise<unknown> | null = null;

function executeBridgeRequest<T = unknown>(
  method: string,
  params: Record<string, unknown> = {},
  options?: BridgeRequestOptions
): Promise<T> {
  initGlobalListener();

  return new Promise((resolve, reject) => {
    if (nextId >= Number.MAX_SAFE_INTEGER) nextId = 1;
    while (pendingRequests.has(nextId)) nextId += 1;
    const id = nextId++;
    const timeoutMs = resolveBridgeTimeoutMs(method, params);
    const timeoutId = setTimeout(() => {
      if (pendingRequests.delete(id)) {
        const error = new Error(`Bridge request timeout: ${method}`);
        if (!options?.silent && isSettingsMutationMethod(method)) {
          window.dispatchEvent(new CustomEvent('tools3000:settings-save-failed', {
            detail: { message: error.message, type: 'error' }
          }));
        }
        reject(error);
      }
    }, timeoutMs);

    const handleSuccess = (rawResult: unknown) => {
      const resultObj = rawResult && typeof rawResult === 'object' ? (rawResult as Record<string, unknown>) : null;
      const isFailed = resultObj?.success === false;

      if (!options?.silent && isSettingsMutationMethod(method)) {
        if (isFailed) {
          const errDetail = typeof resultObj?.error === 'string' ? resultObj.error : undefined;
          window.dispatchEvent(new CustomEvent('tools3000:settings-save-failed', {
            detail: { message: errDetail, type: 'error' }
          }));
        } else {
          window.dispatchEvent(new CustomEvent('tools3000:settings-saved', {
            detail: { message: options?.toastMessage, type: 'success' }
          }));
        }
      }
      resolve(rawResult as T);
    };

    const handleFailure = (reason: unknown) => {
      if (!options?.silent && isSettingsMutationMethod(method)) {
        let errMessage: string | undefined;
        if (typeof reason === 'string') {
          errMessage = reason;
        } else if (reason && typeof reason === 'object' && 'message' in reason && typeof (reason as Error).message === 'string') {
          errMessage = (reason as Error).message;
        }
        window.dispatchEvent(new CustomEvent('tools3000:settings-save-failed', {
          detail: { message: errMessage, type: 'error' }
        }));
      }
      reject(reason instanceof BridgeError ? reason : new BridgeError(reason));
    };

    pendingRequests.set(id, {
      resolve: handleSuccess,
      reject: handleFailure,
      timeoutId,
    });

    const message = JSON.stringify({ id, method, params });

    if (window.chrome?.webview) {
      try {
        window.chrome.webview.postMessage(message);
      } catch (error) {
        pendingRequests.delete(id);
        clearTimeout(timeoutId);
        handleFailure(error);
      }
    } else {
      // 开发模式下模拟响应
      console.warn('[Bridge] WebView2 not available, mocking response for:', method);
      pendingRequests.delete(id);
      clearTimeout(timeoutId);
      handleSuccess(getMockResponse(method, params));
    }
  });
}

/**
 * 向 C++ 发送请求（内置设置保存状态全链路智能拦截与高频启动请求单飞锁去重）
 */
export function bridgeRequest<T = unknown>(
  method: string,
  params: Record<string, unknown> = {},
  options?: BridgeRequestOptions
): Promise<T> {
  // 启动去重单飞锁（Single-flight Promise）：
  // 多窗口及多页面并发启动时向主线程并发发送 5 次 general.getSettings，收敛至单次在途请求
  if (method === 'general.getSettings') {
    if (generalGetSettingsFlight) {
      return generalGetSettingsFlight as Promise<T>;
    }
    const flight = executeBridgeRequest<T>(method, params, options)
      .finally(() => {
        if (generalGetSettingsFlight === flight) {
          generalGetSettingsFlight = null;
        }
      });
    generalGetSettingsFlight = flight;
    return flight;
  }

  return executeBridgeRequest<T>(method, params, options);
}

/**
 * React Hook: 使用 IPC 通信
 */
export function useBridge() {
  const request = useCallback(
    <T = unknown>(method: string, params: Record<string, unknown> = {}, options?: BridgeRequestOptions) => {
      return bridgeRequest<T>(method, params, options);
    },
    []
  );

  return { request };
}

/**
 * 唤起系统默认浏览器打开外部网页
 * 具备 Bridge IPC (WebView2) -> window.open (网页预览/测试环境) 双重保障
 */
export async function openExternalUrl(url: string): Promise<boolean> {
  if (!url || typeof url !== 'string') return false;
  const trimmed = url.trim();
  if (!trimmed.startsWith('http://') && !trimmed.startsWith('https://')) return false;

  // 1. 若处于宿主 WebView2 容器内，优先通过 IPC 安全交由 C++ WinUtils::openUrl 唤起系统默认浏览器
  if (typeof window !== 'undefined' && window.chrome?.webview) {
    try {
      const res = await bridgeRequest<{ success?: boolean }>('system.openUrl', { url: trimmed });
      if (res && res.success !== false) return true;
    } catch (error) {
      console.warn('[Bridge] system.openUrl failed, falling back to window.open', error);
    }
  }

  // 2. 网页端预览（Vite 开发环境）或无 WebView2 环境下的降级方案
  try {
    if (typeof window !== 'undefined' && typeof window.open === 'function') {
      const opened = window.open(trimmed, '_blank', 'noopener,noreferrer');
      return opened !== null;
    }
  } catch {
    return false;
  }

  return false;
}

/**
 * React Hook: 监听 C++ 推送的事件
 */
export function useBridgeEvent(eventName: string, handler: MessageHandler) {
  const handlerRef = useRef(handler);
  // 在 effect 中同步最新 handler，避免在渲染期间写入 ref
  useEffect(() => {
    handlerRef.current = handler;
  });

  useEffect(() => {
    initGlobalListener();

    const wrappedHandler: MessageHandler = (data) => handlerRef.current(data);

    if (!eventListeners.has(eventName)) {
      eventListeners.set(eventName, new Set());
    }
    eventListeners.get(eventName)!.add(wrappedHandler);

    return () => {
      eventListeners.get(eventName)?.delete(wrappedHandler);
    };
  }, [eventName]);
}

// ── 开发模式 Mock 数据 ──────────────────────────────────────────────────────
let mockPlugins = [
  { id: 'capture', name: 'Capture', version: __TOOLS3000_VERSION__, fileName: 'Plugin_Capture.dll', enabled: true, active: true, restartRequired: false, state: 'running' },
  { id: 'gesture', name: 'Gesture', version: __TOOLS3000_VERSION__, fileName: 'Plugin_Gesture.dll', enabled: true, active: true, restartRequired: false, state: 'running' },
  { id: 'keycast', name: 'Keycast', version: __TOOLS3000_VERSION__, fileName: 'Plugin_Keycast.dll', enabled: false, active: false, restartRequired: false, state: 'disabled' },
  { id: 'search', name: 'Search', version: __TOOLS3000_VERSION__, fileName: 'Plugin_Search.dll', enabled: true, active: true, restartRequired: false, state: 'running' },
];

function getMockResponse(method: string, params: Record<string, unknown> = {}): unknown {
  switch (method) {
    case 'config.getAll':
      return {
        gesture: { enabled: true, triggerButton: 'right', trailVisible: true },
        capture: { format: 'png', quality: 95, copyToClipboard: true, saveToFile: true },
        recording: { format: 'mp4_h264', fps: 30, bitrate: 8 },
        general: { theme: 'light', language: 'zh-CN', autoStart: false },
        ocr: { engine: 'windows', language: 'system', copyResult: true, showResultWindow: true },
      };

    case 'plugins.getAll':
      return mockPlugins;

    case 'plugins.setEnabled': {
      const id = String(params.id ?? '');
      const enabled = Boolean(params.enabled);
      mockPlugins = mockPlugins.map((plugin) => plugin.id === id
        ? { ...plugin, enabled, restartRequired: enabled !== plugin.active, state: enabled !== plugin.active ? 'pendingRestart' : (plugin.active ? 'running' : 'disabled') }
        : plugin);
      return { success: mockPlugins.some((plugin) => plugin.id === id), restartRequired: true };
    }

    case 'gesture.getProfiles':
      return [
        {
          name: 'default',
          mappings: [
            { gestureCode: 'L', action: { type: 0, name: '后退', description: '网页/浏览器/文件管理器后退', keyStroke: 'Alt+Left' } },
            { gestureCode: 'R', action: { type: 0, name: '前进', description: '网页/浏览器/文件管理器前进', keyStroke: 'Alt+Right' } },
            { gestureCode: 'WheelUp', action: { type: 2, name: '上一个虚拟桌面', description: '切换到左侧虚拟桌面', builtinCmd: 25 } },
            { gestureCode: 'WheelDown', action: { type: 2, name: '下一个虚拟桌面', description: '切换到右侧虚拟桌面', builtinCmd: 26 } },
            { gestureCode: 'Middle+L', action: { type: 0, name: '上一曲', description: '全局多媒体上一曲 (鼠标中键向左滑动)', keyStroke: 'MediaPrev' } },
            { gestureCode: 'Middle+R', action: { type: 0, name: '下一曲', description: '全局多媒体下一曲 (鼠标中键向右滑动)', keyStroke: 'MediaNext' } },
            { gestureCode: 'U', action: { type: 2, name: '最大化/还原', description: '最大化或还原当前窗口', builtinCmd: 2 } },
            { gestureCode: 'D', action: { type: 2, name: '最小化', description: '最小化当前窗口', builtinCmd: 3 } },
            { gestureCode: 'D-R', action: { type: 0, name: '关闭标签页', description: '关闭当前标签页或文档', keyStroke: 'Ctrl+W' } },
            { gestureCode: 'R-D', action: { type: 0, name: '恢复关闭的标签页', description: '重新打开最近关闭的标签页 (如 Chrome/Edge 恢复标签)', keyStroke: 'Ctrl+Shift+T' } },
            { gestureCode: 'D-L', action: { type: 0, name: '关闭窗口', description: '关闭当前活动窗口或应用程序', keyStroke: 'Alt+F4' } },
            { gestureCode: 'U-R', action: { type: 0, name: '下一个标签页', description: '切换到下一个标签页', keyStroke: 'Ctrl+Tab' } },
            { gestureCode: 'U-L', action: { type: 0, name: '上一个标签页', description: '切换到上一个标签页', keyStroke: 'Ctrl+Shift+Tab' } },
            { gestureCode: 'L-U-R', action: { type: 0, name: '刷新', description: '刷新页面', keyStroke: 'F5' } },
            { gestureCode: 'U-D', action: { type: 0, name: '新建标签页', description: '新建标签页', keyStroke: 'Ctrl+T' } },
            { gestureCode: 'L-D', action: { type: 0, name: '显示桌面', description: '一键显示/隐藏桌面', keyStroke: 'Win+D' } },
            { gestureCode: 'R-U', action: { type: 0, name: '任务视图', description: '打开 Windows 任务视图', keyStroke: 'Win+Tab' } },
            { gestureCode: 'D-R-D', action: { type: 0, name: '屏幕截图', description: '唤起屏幕截图工具', keyStroke: 'Win+Shift+S' } },
            { gestureCode: 'L-U', action: { type: 0, name: '剪切', description: '剪切选中内容', keyStroke: 'Ctrl+X' } },
            { gestureCode: 'R-L', action: { type: 0, name: '全选', description: '全选当前内容', keyStroke: 'Ctrl+A' } },
            { gestureCode: 'L-R', action: { type: 0, name: '撤销', description: '撤销上一步操作', keyStroke: 'Ctrl+Z' } },
          ],
        },
      ];

    case 'gesture.getState':
      return {
        enabled: true,
        paused: false,
        triggerButton: 'right',
        trailVisible: true,
        targetMode: 'underPointer',
        elevated: false,
        runAsAdmin: true,
      };

    case 'gesture.getScopeRules':
      return [
        {
          id: 'mock-1', name: 'Chrome 浏览器', enabled: true,
          processName: 'chrome.exe', windowClass: '', matchMode: 0, effect: 2, profileName: 'browser',
        },
        {
          id: 'mock-2', name: '游戏全屏禁用', enabled: true,
          processName: '*.exe', windowClass: '', matchMode: 1, effect: 1, profileName: '',
        },
      ];

    case 'capture.getSettings':
      return {
        format: 'png', quality: 90, saveToFile: true, copyToClipboard: true,
        saveDirectory: '', showCrosshair: false, autoDetectWindow: true,
        showShortcutHints: true,
      };

    case 'recording.getSettings':
      return {
        format: 'mp4_h264', fps: 30, bitrate: 8, saveDirectory: '',
      };

    case 'general.getSettings':
      return {
        language: 'zh-CN', autoStart: false, runAsAdmin: true, elevated: false, theme: 'light',
        logLevel: 'info', minimizeToTray: true, checkUpdates: true,
      };

    case 'ocr.getSettings':
      return {
        engine: 'windows', language: 'system', copyResult: true,
        showResultWindow: true,
      };

    case 'ocr.getStatus':
      return { available: true };

    case 'ocr.recognizeImageFile':
      return { success: true, text: '(mock) 识别到的示例文字', copied: true };

    case 'history.getAll':
      return [];

    case 'history.open':
      return { success: true };

    case 'stats.getHistory': {
      const today = new Date();
      return Object.fromEntries(Array.from({ length: 7 }, (_, index) => {
        const date = new Date(today);
        date.setDate(today.getDate() - (6 - index));
        const dateKey = [
          date.getFullYear(),
          String(date.getMonth() + 1).padStart(2, '0'),
          String(date.getDate()).padStart(2, '0'),
        ].join('-');
        return [dateKey, {
          totalKeys: 7800 + index * 1260,
          leftClicks: 980 + index * 94,
          rightClicks: 180 + index * 21,
          mouseDistance: 182000 + index * 24500,
          keyMap: { 8: 315, 13: 482, 32: 1210, 65: 720, 69: 884, 73: 691, 78: 742, 79: 804, 83: 765, 84: 910 },
        }];
      }));
    }

    case 'stats.getTotal':
      return { totalKeystrokes: 128640 };

    case 'search.query':
      return { available: true, results: [] };

    case 'perf.getMetrics':
      return { memoryMB: 42.5, cpuPercent: 0.8, screenshotLatencyMs: 0, gestureLatencyMs: 1.2, uiRenderLatencyMs: 4.1 };

    case 'app.getSystemInfo':
      return { version: __TOOLS3000_VERSION__, cpuArch: 'x64', cpuCores: 12, totalMemoryGB: 32, dpiScale: 1 };

    case 'app.checkForUpdates':
      return { success: true, started: false };

    case 'hotcorner.getSettings':
      return {
        enabled: false,
        delay: 300,
        corners: {
          topLeft:     { commandIndex: -1 },
          topRight:    { commandIndex: 2 },
          bottomLeft:  { commandIndex: 5 },
          bottomRight: { commandIndex: -1 },
        },
      };

    case 'radialmenu.getItems':
      return { items: [
        { label: '截图', command: '10' },
        { label: '搜索', command: '16' },
        { label: '锁屏', command: '8' },
        { label: '贴图', command: '18' },
      ] };

    case 'hotkey.getAll':
      return [
        { name: 'Screenshot', shortcut: 'F1' },
        { name: 'Record', shortcut: 'Ctrl+Shift+R' },
        { name: 'OCR', shortcut: 'Ctrl+Shift+O' },
        { name: 'Pause Gestures', shortcut: 'Ctrl+Alt+Shift+W' },
        { name: 'Toggle Search', shortcut: 'Alt+Space' },
        { name: 'Pin Toggle', shortcut: 'Ctrl+Alt+Shift+X' },
        { name: 'Pin Paste', shortcut: 'F3' },
        { name: 'Pin Hide All', shortcut: 'Ctrl+Alt+Shift+H' },
        { name: 'Pin Arrange', shortcut: 'Ctrl+Alt+Shift+G' },
      ];

    case 'config.get':
      return false;

    // 所有 update / action 方法返回成功
    case 'system.openUrl':
    case 'system.openExternal':
    case 'system.openFile':
    case 'system.openFolder':
    case 'config.export':
    case 'config.import':
    case 'config.reset':
    case 'capture.updateSettings':
    case 'recording.updateSettings':
    case 'general.updateSettings':
    case 'ocr.updateSettings':
    case 'gesture.updateProfile':
    case 'gesture.updateScopeRules':
    case 'gesture.setPaused':
    case 'gesture.updateSettings':
    case 'hotcorner.updateSettings':
    case 'radialmenu.updateItems':
    case 'hotkey.rebind':
    case 'config.set':
    case 'app.restart':
    case 'app.restartElevated':
      return { success: true };

    default:
      return {};
  }
}

// ── TypeScript 声明扩展 ─────────────────────────────────────────────────────
declare global {
  interface Window {
    chrome?: {
      webview?: {
        postMessage: (message: string) => void;
        addEventListener: (type: string, listener: (e: MessageEvent) => void) => void;
      };
    };
  }
}

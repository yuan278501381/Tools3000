import { useState, useEffect, useRef, useLayoutEffect, useCallback, useMemo } from 'react';
import {
  Settings,
  Crop,
  Video,
  Search,
  ClipboardList,
  Shield,
  ShieldCheck,
  LogOut,
  RotateCw,
  RotateCcw,
  Pin,
  Download,
  CheckCircle2,
} from 'lucide-react';
import { bridgeRequest, useBridgeEvent } from './hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { useAppearance } from './hooks/useAppearance';
import { Tools3000Bolt } from './components/Tools3000Bolt';
import {
  TRAY_CONTROL_REGISTRY,
  formatCompactShortcut,
  DEFAULT_TRAY_PIN_GROUP_STATE,
  normalizePinGroupState,
  type TrayControlItem,
  type TrayPinGroupState,
  type PinGroupControlPayload,
} from './tray/trayRegistry';
import { TrayPinGroupPanel } from './tray/TrayPinGroupPanel';
import './TrayApp.css';

interface TrayHotkeyItem {
  name: string;
  shortcut: string;
}

interface TrayShortcuts {
  screenshot: string;
  recording: string;
  pinPaste: string;
  search: string;
  clipboard: string;
  emergencyFlush: string;
}

const EMPTY_SHORTCUTS: TrayShortcuts = {
  screenshot: '',
  recording: '',
  pinPaste: '',
  search: '',
  clipboard: '',
  emergencyFlush: '',
};


interface TrayUpdateBannerState {
  hasUpdate: boolean;
  badgeVisible: boolean;
  dismissed: boolean;
  status: string;
  latestVersion: string;
  progress: number;
  readyToInstall: boolean;
}

const DEFAULT_UPDATE_BANNER: TrayUpdateBannerState = {
  hasUpdate: false,
  badgeVisible: false,
  dismissed: false,
  status: 'idle',
  latestVersion: '',
  progress: 0,
  readyToInstall: false,
};

function normalizeTrayUpdateBanner(raw: unknown): TrayUpdateBannerState | null {
  if (!raw || typeof raw !== 'object') return null;
  const st = raw as {
    hasUpdate?: boolean;
    badgeVisible?: boolean;
    dismissed?: boolean;
    status?: string;
    latestVersion?: string;
    progress?: number;
    readyToInstall?: boolean;
  };
  if (typeof st.hasUpdate !== 'boolean' && typeof st.badgeVisible !== 'boolean') {
    return null;
  }
  const hasUpdate = Boolean(st.hasUpdate);
  const dismissed = Boolean(st.dismissed);
  const badgeVisible = typeof st.badgeVisible === 'boolean'
    ? st.badgeVisible
    : (hasUpdate && !dismissed);
  return {
    hasUpdate,
    badgeVisible,
    dismissed,
    status: st.status ?? 'idle',
    latestVersion: st.latestVersion ?? '',
    progress: typeof st.progress === 'number' ? st.progress : 0,
    readyToInstall: Boolean(st.readyToInstall),
  };
}

export default function TrayApp() {
  useAppearance();
  const { t } = useTranslation();
  const menuRef = useRef<HTMLDivElement>(null);
  const headerRef = useRef<HTMLDivElement>(null);
  const scrollInnerRef = useRef<HTMLDivElement>(null);
  const footerRef = useRef<HTMLDivElement>(null);
  const [gesturePaused, setGesturePaused] = useState(false);
  const [remoteBoostEnabled, setRemoteBoostEnabled] = useState(true);
  const [elevated, setElevated] = useState(false);
  const [elevating, setElevating] = useState(false);
  const [busy, setBusy] = useState(false);
  const [pendingRestart, setPendingRestart] = useState(false);
  const [shortcuts, setShortcuts] = useState<TrayShortcuts>(EMPTY_SHORTCUTS);
  const [pinPanelExpanded, setPinPanelExpanded] = useState(false);
  const [pinGroupState, setPinGroupState] = useState<TrayPinGroupState>(DEFAULT_TRAY_PIN_GROUP_STATE);
  const [updateBanner, setUpdateBanner] = useState<TrayUpdateBannerState>(DEFAULT_UPDATE_BANNER);
  const pendingRestartRef = useRef(false);
  const lastReportedSizeRef = useRef({ width: 0, height: 0 });
  const sizeFrameRef = useRef<number | null>(null);
  const refreshInFlightRef = useRef(false);
  const [activePlugins, setActivePlugins] = useState<Set<string>>(() => new Set([
    'capture', 'search', 'clipboard', 'text_expander', 'textexpander', 'gesture', 'keycast', 'spotlight', 'dialogenhancer', 'dialog_enhancer', 'file_locksmith', 'filelocksmith', 'remote_boost',
  ]));

  // 零形变精准测高引擎：严格使用不受 CSS transform 缩放污染的 offsetHeight / scrollHeight 求和，
  // 彻底根治动画帧竞态导致的「偶发切掉半个退出按钮」问题。
  const reportSizeDirect = useCallback((force = false) => {
    const el = menuRef.current;
    if (!el) return;

    const style = window.getComputedStyle(el);
    const padTop = parseFloat(style.paddingTop) || 0;
    const padBottom = parseFloat(style.paddingBottom) || 0;
    const borderTop = parseFloat(style.borderTopWidth) || 0;
    const borderBottom = parseFloat(style.borderBottomWidth) || 0;
    const chromeY = padTop + padBottom + borderTop + borderBottom;

    let totalHeight = 0;
    if (headerRef.current && scrollInnerRef.current && footerRef.current) {
      const headerH = headerRef.current.offsetHeight;
      const bodyH = Math.max(scrollInnerRef.current.offsetHeight, scrollInnerRef.current.scrollHeight);
      const footerH = footerRef.current.offsetHeight;
      if (headerH > 0 || bodyH > 0 || footerH > 0) {
        // 额外预留 2px 安全余量，防止 125%/150% DPI 次像素舍入吃掉 1px 底边线
        totalHeight = Math.ceil(headerH + bodyH + footerH + chromeY + 2);
      }
    }
    if (totalHeight <= 20) {
      const borderY = el.offsetHeight - el.clientHeight;
      totalHeight = Math.ceil(Math.max(el.offsetHeight, el.scrollHeight + borderY));
    }

    const fixedWidth = 320;
    const previous = lastReportedSizeRef.current;
    if (totalHeight <= 20) return;
    if (!force && previous.width === fixedWidth && previous.height === totalHeight) return;
    lastReportedSizeRef.current = { width: fixedWidth, height: totalHeight };
    void bridgeRequest('tray.resize', { width: fixedWidth, height: totalHeight }).catch(() => {
      lastReportedSizeRef.current = { width: 0, height: 0 };
    });
  }, []);

  // 合并同一帧内的多次尺寸通知，避免与宿主窗口形成回声循环
  const reportSize = useCallback(() => {
    if (sizeFrameRef.current !== null) return;
    sizeFrameRef.current = window.requestAnimationFrame(() => {
      sizeFrameRef.current = null;
      reportSizeDirect(false);
    });
  }, [reportSizeDirect]);

  useLayoutEffect(() => {
    reportSize();
    if (!menuRef.current || typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver(() => {
      reportSize();
    });
    if (headerRef.current) ro.observe(headerRef.current);
    if (scrollInnerRef.current) ro.observe(scrollInnerRef.current);
    if (footerRef.current) ro.observe(footerRef.current);
    return () => {
      ro.disconnect();
      if (sizeFrameRef.current !== null) {
        window.cancelAnimationFrame(sizeFrameRef.current);
        sizeFrameRef.current = null;
      }
    };
  }, [reportSize, activePlugins, remoteBoostEnabled, pendingRestart, elevated, shortcuts, pinPanelExpanded, pinGroupState, updateBanner]);

  const refreshState = useCallback(() => {
    if (refreshInFlightRef.current) return;
    refreshInFlightRef.current = true;
    const settingsRequest = bridgeRequest<{ elevated?: boolean }>('general.getSettings')
      .then((res) => setElevated(Boolean(res?.elevated)))
      .catch(() => {});
    const remoteRequest = bridgeRequest<{ enabled?: boolean; emergencyShortcut?: string }>('remote.getSettings')
      .then((res) => {
        setRemoteBoostEnabled(Boolean(res?.enabled));
        if (typeof res?.emergencyShortcut === 'string') {
          setShortcuts((prev) => {
            const formatted = formatCompactShortcut(res.emergencyShortcut);
            return prev.emergencyFlush === formatted ? prev : { ...prev, emergencyFlush: formatted };
          });
        }
      })
      .catch(() => {});
    const hotkeysRequest = bridgeRequest<TrayHotkeyItem[]>('hotkey.getAll')
      .then((items) => {
        if (!Array.isArray(items)) return;
        const byName = new Map(items.map((entry) => [entry.name, formatCompactShortcut(entry.shortcut)]));
        setShortcuts((prev) => {
          const next: TrayShortcuts = {
            screenshot: byName.get('Screenshot') ?? prev.screenshot,
            recording: byName.get('Record') ?? prev.recording,
            pinPaste: byName.get('Pin Paste') ?? prev.pinPaste,
            search: byName.get('Toggle Search') ?? prev.search,
            clipboard: byName.get('Toggle Clipboard') ?? prev.clipboard,
            emergencyFlush: prev.emergencyFlush,
          };
          if (
            prev.screenshot === next.screenshot &&
            prev.recording === next.recording &&
            prev.pinPaste === next.pinPaste &&
            prev.search === next.search &&
            prev.clipboard === next.clipboard
          ) {
            return prev;
          }
          return next;
        });
      })
      .catch(() => {});
    const pluginsRequest = bridgeRequest<Array<{ id: string; active: boolean; enabled?: boolean; restartRequired?: boolean }>>('plugins.getAll')
      .then((plugins) => {
        if (plugins.some((plugin) => typeof plugin.restartRequired === 'boolean')) {
          const needsRestart = plugins.some((plugin) => Boolean(plugin.restartRequired));
          pendingRestartRef.current = needsRestart;
          setPendingRestart(needsRestart);
        }
        const active = new Set(
          plugins
            .filter((plugin) => plugin.enabled !== false && plugin.active)
            .flatMap((plugin) => {
              if (plugin.id === 'dialogenhancer' || plugin.id === 'dialog_enhancer') {
                return ['dialogenhancer', 'dialog_enhancer'];
              }
              if (plugin.id === 'file_locksmith' || plugin.id === 'filelocksmith') {
                return ['file_locksmith', 'filelocksmith'];
              }
              if (plugin.id === 'remote_boost' || plugin.id === 'remote') {
                return ['remote_boost', 'remote'];
              }
              if (plugin.id === 'text_expander' || plugin.id === 'textexpander') {
                return ['text_expander', 'textexpander'];
              }
              return [plugin.id];
            })
        );
        setActivePlugins((previous) => {
          if (previous.size === active.size && [...previous].every((id) => active.has(id))) return previous;
          return active;
        });
        if (active.has('gesture')) {
          return bridgeRequest<{ paused: boolean }>('gesture.getState')
            .then((state) => setGesturePaused(Boolean(state?.paused)))
            .catch(() => {});
        }
      })
      .catch(() => {});
    const pinGroupRequest = bridgeRequest<unknown>('capture.pinGroupControl')
      .then((res) => setPinGroupState(normalizePinGroupState(res)))
      .catch(() => {});
    const updateRequest = bridgeRequest<unknown>('app.getUpdateState')
      .then((st) => {
        const normalized = normalizeTrayUpdateBanner(st);
        if (normalized) {
          setUpdateBanner(normalized);
        }
      })
      .catch(() => {});
    void Promise.allSettled([settingsRequest, pluginsRequest, remoteRequest, hotkeysRequest, pinGroupRequest, updateRequest]).finally(() => {
      refreshInFlightRef.current = false;
      reportSize();
    });
  }, [reportSize]);

  useBridgeEvent('update.state', (raw: unknown) => {
    const normalized = normalizeTrayUpdateBanner(raw);
    if (normalized) {
      setUpdateBanner(normalized);
    }
  });

  useBridgeEvent('plugins:changed', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { id?: string; enabled?: boolean; restartRequired?: boolean }) : null;
    if (payload?.restartRequired) {
      pendingRestartRef.current = true;
      setPendingRestart(true);
    }
    if (!payload?.id) {
      refreshState();
      return;
    }
    const { id, enabled } = payload;
    const aliases = (id === 'dialogenhancer' || id === 'dialog_enhancer')
      ? ['dialogenhancer', 'dialog_enhancer']
      : (id === 'file_locksmith' || id === 'filelocksmith')
        ? ['file_locksmith', 'filelocksmith']
        : (id === 'remote_boost' || id === 'remote')
          ? ['remote_boost', 'remote']
          : (id === 'text_expander' || id === 'textexpander')
            ? ['text_expander', 'textexpander']
            : [id];
    setActivePlugins((prev) => {
      const next = new Set(prev);
      if (enabled) {
        aliases.forEach((k) => next.add(k));
      } else {
        aliases.forEach((k) => next.delete(k));
      }
      return next;
    });
    if (id === 'remote_boost' || id === 'remote') {
      setRemoteBoostEnabled(Boolean(enabled));
    }
  });

  useBridgeEvent('dialog:configChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      setActivePlugins((prev) => {
        const next = new Set(prev);
        if (payload.enabled) {
          next.add('dialogenhancer');
          next.add('dialog_enhancer');
        } else {
          next.delete('dialogenhancer');
          next.delete('dialog_enhancer');
        }
        return next;
      });
    }
  });

  useBridgeEvent('keycast:configChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      setActivePlugins((prev) => {
        const next = new Set(prev);
        if (payload.enabled) next.add('keycast');
        else next.delete('keycast');
        return next;
      });
    }
  });

  useBridgeEvent('spotlight:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      setActivePlugins((prev) => {
        const next = new Set(prev);
        if (payload.enabled) next.add('spotlight');
        else next.delete('spotlight');
        return next;
      });
    }
  });

  useBridgeEvent('clipboard:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      setActivePlugins((prev) => {
        const next = new Set(prev);
        if (payload.enabled) next.add('clipboard');
        else next.delete('clipboard');
        return next;
      });
    }
  });

  useBridgeEvent('textExpander:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      setActivePlugins((prev) => {
        const next = new Set(prev);
        if (payload.enabled) {
          next.add('text_expander');
          next.add('textexpander');
        } else {
          next.delete('text_expander');
          next.delete('textexpander');
        }
        return next;
      });
    }
  });

  useBridgeEvent('fileLocksmith:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      setActivePlugins((prev) => {
        const next = new Set(prev);
        if (payload.enabled) {
          next.add('file_locksmith');
          next.add('filelocksmith');
        } else {
          next.delete('file_locksmith');
          next.delete('filelocksmith');
        }
        return next;
      });
    }
  });

  useBridgeEvent('pin:groupsChanged', (data: unknown) => {
    setPinGroupState(normalizePinGroupState(data));
  });

  useBridgeEvent('remote:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      setRemoteBoostEnabled(payload.enabled);
      setActivePlugins((prev) => {
        const next = new Set(prev);
        if (payload.enabled) {
          next.add('remote_boost');
          next.add('remote');
        } else {
          next.delete('remote_boost');
          next.delete('remote');
        }
        return next;
      });
    }
  });

  const applyPendingRestart = useCallback(async () => {
    if (!pendingRestartRef.current) return;
    pendingRestartRef.current = false;
    setPendingRestart(false);
    void bridgeRequest('tray.hide').catch(() => {});
    await bridgeRequest('app.restart').catch(() => {});
  }, []);

  useEffect(() => {
    document.documentElement.dataset.surface = 'tray';
    refreshState();
    let cancelled = false;
    if (document.fonts && typeof document.fonts.ready?.then === 'function') {
      void document.fonts.ready.then(() => {
        if (!cancelled) reportSizeDirect(true);
      });
    }
    const handleShow = () => {
      refreshState();
      reportSizeDirect(true);
    };
    const handleVisibility = () => {
      if (!document.hidden) {
        refreshState();
        reportSizeDirect(true);
      }
    };
    window.addEventListener('tray:show', handleShow);
    window.addEventListener('visibilitychange', handleVisibility);
    return () => {
      cancelled = true;
      window.removeEventListener('tray:show', handleShow);
      window.removeEventListener('visibilitychange', handleVisibility);
      delete document.documentElement.dataset.surface;
    };
  }, [refreshState, reportSizeDirect]);

  // 键盘快捷导航 (Esc 安全收起托盘，方向键切换焦点；豁免输入控件与中文输入法组字态)
  useEffect(() => {
    const isEditableElement = (el: EventTarget | Element | null): boolean => {
      if (!(el instanceof HTMLElement)) return false;
      const tag = el.tagName;
      return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || el.isContentEditable;
    };

    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.isComposing || e.keyCode === 229 || e.key === 'Process') {
        return;
      }
      const inEditable = isEditableElement(e.target) || isEditableElement(document.activeElement);
      if (e.key === 'Escape') {
        if (inEditable) {
          // 交由输入框自身撤销草稿并失焦退出编辑态，二次按 Esc 再收起托盘
          if (document.activeElement instanceof HTMLElement) {
            document.activeElement.blur();
          }
          return;
        }
        e.preventDefault();
        void bridgeRequest('tray.hide').catch(() => {});
        return;
      }
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp' || e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
        if (inEditable) {
          return;
        }
        e.preventDefault();
        const buttons = Array.from(menuRef.current?.querySelectorAll<HTMLButtonElement>('button:not(:disabled)') ?? []);
        if (buttons.length === 0) return;
        const currentIndex = buttons.indexOf(document.activeElement as HTMLButtonElement);
        let nextIndex: number;
        if (currentIndex === -1) {
          nextIndex = (e.key === 'ArrowDown' || e.key === 'ArrowRight') ? 0 : buttons.length - 1;
        } else {
          const delta = (e.key === 'ArrowDown' || e.key === 'ArrowRight') ? 1 : -1;
          nextIndex = (currentIndex + delta + buttons.length) % buttons.length;
        }
        buttons[nextIndex]?.focus();
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => {
      window.removeEventListener('keydown', handleKeyDown);
    };
  }, []);

  // 快捷胶囊开关：手势特殊暂停/恢复处理
  const toggleGesture = async (e: React.MouseEvent) => {
    e.stopPropagation();
    if (!activePlugins.has('gesture')) {
      setActivePlugins((prev) => new Set(prev).add('gesture'));
      setGesturePaused(false);
      try {
        const res = await bridgeRequest<{ success: boolean; restartRequired?: boolean }>('plugins.setEnabled', { id: 'gesture', enabled: true });
        if (res?.restartRequired) {
          setPendingRestart(true);
          pendingRestartRef.current = true;
        }
      } catch {
        setActivePlugins((prev) => {
          const next = new Set(prev);
          next.delete('gesture');
          return next;
        });
      }
      return;
    }
    const nextPaused = !gesturePaused;
    setGesturePaused(nextPaused);
    try {
      await bridgeRequest('gesture.updateSettings', { paused: nextPaused });
    } catch {
      setGesturePaused(!nextPaused);
    }
  };

  // 通用插件开关逻辑（含别名同步与乐观回滚机制）
  const togglePlugin = async (e: React.MouseEvent, id: string, aliases?: string[]) => {
    e.stopPropagation();
    const allIds = [id, ...(aliases || [])];
    const currentlyActive = allIds.some((k) => activePlugins.has(k));
    const willEnable = !currentlyActive;

    setActivePlugins((prev) => {
      const next = new Set(prev);
      if (willEnable) {
        allIds.forEach((k) => next.add(k));
      } else {
        allIds.forEach((k) => next.delete(k));
      }
      return next;
    });

    try {
      const res = await bridgeRequest<{ success: boolean; restartRequired?: boolean }>('plugins.setEnabled', { id, enabled: willEnable });
      if (res?.restartRequired) {
        setPendingRestart(true);
        pendingRestartRef.current = true;
      }
    } catch {
      setActivePlugins((prev) => {
        const next = new Set(prev);
        if (willEnable) {
          allIds.forEach((k) => next.delete(k));
        } else {
          allIds.forEach((k) => next.add(k));
        }
        return next;
      });
    }
  };

  // 框架化判定某项是否处于活跃状态
  const isItemActive = useCallback((item: TrayControlItem): boolean => {
    if (item.getCustomActive) {
      return item.getCustomActive(activePlugins, { gesturePaused, remoteBoostEnabled });
    }
    if (activePlugins.has(item.pluginId)) return true;
    if (item.aliasPluginIds?.some((id) => activePlugins.has(id))) return true;
    return false;
  }, [activePlugins, gesturePaused, remoteBoostEnabled]);

  const activeModuleCount = useMemo(
    () => TRAY_CONTROL_REGISTRY.reduce((acc, item) => acc + (isItemActive(item) ? 1 : 0), 0),
    [isItemActive]
  );

  // 框架化分发点击切换
  const handleToggleItem = async (e: React.MouseEvent, item: TrayControlItem) => {
    e.stopPropagation();
    if (item.id === 'gesture') {
      await toggleGesture(e);
      return;
    }
    if (item.id === 'remote_boost') {
      const next = !remoteBoostEnabled;
      setRemoteBoostEnabled(next);
      setActivePlugins((prev) => {
        const set = new Set(prev);
        if (next) set.add('remote_boost');
        else set.delete('remote_boost');
        return set;
      });
      try {
        await bridgeRequest('remote.updateSettings', { enabled: next });
      } catch {
        setRemoteBoostEnabled(!next);
        setActivePlugins((prev) => {
          const set = new Set(prev);
          if (!next) set.add('remote_boost');
          else set.delete('remote_boost');
          return set;
        });
      }
      return;
    }
    await togglePlugin(e, item.pluginId, item.aliasPluginIds);
  };

  const captureActive = activePlugins.has('capture');
  const searchActive = activePlugins.has('search');
  const clipboardActive = activePlugins.has('clipboard');

  // 触发全局命令动作（立即乐观收起托盘，带来零延迟体验）
  const handleAction = async (action: string) => {
    if (busy) return;
    setBusy(true);
    void bridgeRequest('tray.hide').catch(() => {});
    try {
      const result = await bridgeRequest<{ success: boolean }>('tray.action', { action });
      if (!result.success) throw new Error('Tray action failed');
    } catch (error) {
      console.error(error);
    } finally {
      setBusy(false);
    }
  };

  const handlePinPaste = async () => {
    if (busy) return;
    setBusy(true);
    void bridgeRequest('tray.hide').catch(() => {});
    try {
      await bridgeRequest('capture.pasteAsPin');
    } catch {
      // Ignore transient clipboard empty errors
    } finally {
      setBusy(false);
    }
  };

  const handlePinGroupControl = async (payload: PinGroupControlPayload) => {
    try {
      const next = await bridgeRequest<unknown>('capture.pinGroupControl', payload as unknown as Record<string, unknown>);
      setPinGroupState(normalizePinGroupState(next));
    } catch {
      // Ignore if capture plugin is temporarily unavailable
    }
  };

  // 触发管理员提权或降权重启
  const handleToggleElevated = async () => {
    if (busy || elevating) return;
    setElevating(true);
    void bridgeRequest('tray.hide').catch(() => {});
    try {
      if (!elevated) {
        const result = await bridgeRequest<{ success: boolean }>('tray.action', { action: 'restartElevated' });
        if (!result || !result.success) {
          setElevating(false);
        }
      } else {
        const result = await bridgeRequest<{ success: boolean }>('tray.action', { action: 'restartDemoted' });
        if (!result || !result.success) {
          setElevating(false);
        }
      }
    } catch {
      setElevating(false);
    }
  };

  return (
    <div ref={menuRef} className="tray-menu" role="menu">
      {/* 1. 吸顶固定头部（永不随滚动消失：品牌状态 + 更新横幅 + 待重启提醒） */}
      <div ref={headerRef} className="tray-header-dock">
        <div className="tray-header">
          <div className="tray-header__brand">
            <Tools3000Bolt size={15} colorVariant />
            <span className="tray-header__title">{t('tray.brandName', 'Tools3000')}</span>
          </div>
          <span className="tray-header__badge">
            {t('tray.modulesActive', {
              active: activeModuleCount,
              total: TRAY_CONTROL_REGISTRY.length,
            })}
          </span>
        </div>

        {/* 版本更新首屏横幅（置于 Bento 模块舱正上方第一视觉位，双交互区：左查详情 / 右直装） */}
        {updateBanner.badgeVisible && (
          <div
            className={`tray-update-banner${updateBanner.readyToInstall ? ' tray-update-banner--ready' : ''}`}
            role="region"
            aria-label={
              updateBanner.readyToInstall
                ? t('about.updateReadyShort', { version: updateBanner.latestVersion, defaultValue: `${updateBanner.latestVersion} Ready` })
                : t('about.updateAvailableShort', { version: updateBanner.latestVersion, defaultValue: `New ${updateBanner.latestVersion}` })
            }
          >
            <button
              type="button"
              className="tray-update-banner__main"
              disabled={busy}
              onClick={() => void handleAction('openAbout')}
            >
              <span className="tray-update-banner__dot" aria-hidden="true" />
              {updateBanner.status === 'downloading' ? (
                <RotateCw size={13} className="tray-update-banner__icon tray-menu__icon--spinning" aria-hidden="true" />
              ) : updateBanner.readyToInstall ? (
                <CheckCircle2 size={13} className="tray-update-banner__icon" aria-hidden="true" />
              ) : (
                <Download size={13} className="tray-update-banner__icon" aria-hidden="true" />
              )}
              <span className="tray-update-banner__copy">
                <span className="tray-update-banner__text">
                  {updateBanner.status === 'downloading'
                    ? t('about.downloadingProgress', { progress: updateBanner.progress, defaultValue: `Downloading ${updateBanner.progress}%` })
                    : updateBanner.readyToInstall
                      ? t('about.updateReadyShort', { version: updateBanner.latestVersion, defaultValue: `${updateBanner.latestVersion} Ready` })
                      : t('about.updateAvailableShort', { version: updateBanner.latestVersion, defaultValue: `New ${updateBanner.latestVersion}` })}
                </span>
                <span className="tray-update-banner__sub">
                  {t('about.viewChangelogShort' as never, 'View release notes')}
                </span>
              </span>
            </button>
            <button
              type="button"
              className="tray-update-banner__action"
              disabled={busy || updateBanner.status === 'downloading'}
              onClick={() => {
                if (updateBanner.readyToInstall) {
                  setBusy(true);
                  void bridgeRequest('tray.hide').catch(() => {});
                  void bridgeRequest('app.installUpdate')
                    .catch(() => {})
                    .finally(() => setBusy(false));
                } else {
                  void bridgeRequest('app.downloadUpdate').catch(() => {});
                }
              }}
            >
              {updateBanner.status === 'downloading'
                ? `${updateBanner.progress}%`
                : updateBanner.readyToInstall
                  ? t('about.installNowShort', 'Install')
                  : t('about.downloadShort', 'Download')}
            </button>
            {updateBanner.status === 'downloading' && (
              <span className="tray-update-banner__progress" aria-hidden="true">
                <span
                  className="tray-update-banner__progress-fill"
                  style={{ width: `${Math.min(100, Math.max(4, updateBanner.progress))}%` }}
                />
              </span>
            )}
          </div>
        )}

        {/* 待生效智能提示条 */}
        {pendingRestart && (
          <button
            type="button"
            className="tray-restart-banner"
            onClick={() => void applyPendingRestart()}
            title={t('tray.restartToApply', 'Plugin settings updated')}
          >
            <span className="tray-restart-banner__info">
              <RotateCw size={12} className="tray-menu__icon--spinning" aria-hidden="true" />
              <span className="tray-restart-banner__text">{t('tray.restartToApply', 'Plugin settings updated')}</span>
            </span>
            <span className="tray-restart-banner__btn">
              {t('tray.restartNow', 'Restart')}
            </span>
          </button>
        )}
      </div>

      {/* 2. 中央独立弹性滚动区（面向未来无限扩展：超屏仅此处优雅滚动，顶栏与底栏永不被切） */}
      <div className="tray-scroll-body">
        <div ref={scrollInnerRef} className="tray-scroll-inner">
          {/* Bento 模块控制舱 (数据驱动自适应 3 列微晶网格 + 满宽拨杆卡) */}
          <div className="tray-control-center" role="group" aria-label={t('tray.quickControls', 'Module Controls')}>
            {TRAY_CONTROL_REGISTRY.map((item) => {
              const active = isItemActive(item);
              const IconComponent = item.icon;
              const label = t(item.labelKey as never, item.fallbackLabel);
              const subLabel = item.subKey ? t(item.subKey as never, item.fallbackSub ?? '') : '';
              const desc = t(item.descKey as never, item.fallbackDesc);
              const statusText = active ? t('tray.enabled', 'Enabled') : t('tray.disabled', 'Disabled');
              const isWide = Boolean(item.wide);

              return (
                <button
                  key={item.id}
                  type="button"
                  role="menuitemcheckbox"
                  aria-checked={active}
                  className={`tray-pill ${active ? 'tray-pill--active' : ''} ${isWide ? 'tray-pill--wide' : ''}`}
                  onClick={(e) => void handleToggleItem(e, item)}
                  title={`${desc}: ${statusText}`}
                  aria-label={`${desc}: ${statusText}`}
                >
                  <span className="tray-pill__icon-wrap" aria-hidden="true">
                    <IconComponent size={13} className="tray-pill__icon" />
                  </span>
                  {isWide ? (
                    <span className="tray-pill__copy">
                      <span className="tray-pill__label">{label}</span>
                      {subLabel && <span className="tray-pill__sub">{subLabel}</span>}
                    </span>
                  ) : (
                    <span className="tray-pill__label">{label}</span>
                  )}
                  {isWide ? (
                    <span className={`tray-pill__switch ${active ? 'tray-pill__switch--on' : ''}`} aria-hidden="true">
                      <span className="tray-pill__switch-thumb" />
                    </span>
                  ) : (
                    <span className="tray-pill__dot" aria-hidden="true" />
                  )}
                </button>
              );
            })}
          </div>

          <div className="tray-menu__divider" role="separator" />

          {/* 快捷动作分组 */}
          <div className="tray-menu__group" role="group">
            <button
              type="button"
              role="menuitem"
              className="tray-menu__item"
              disabled={busy}
              onClick={() => void handleAction('openSettings')}
            >
              <Settings size={15} className="tray-menu__icon" aria-hidden="true" />
              <span className="tray-menu__label">{t('tray.settings', 'Preferences')}</span>
            </button>

            {captureActive && (
              <>
                <button
                  type="button"
                  role="menuitem"
                  className="tray-menu__item"
                  disabled={busy}
                  onClick={() => void handleAction('screenshot')}
                >
                  <Crop size={15} className="tray-menu__icon" aria-hidden="true" />
                  <span className="tray-menu__label">{t('tray.capture', 'Capture Area')}</span>
                  {shortcuts.screenshot && (
                    <kbd className="tray-menu__shortcut">{shortcuts.screenshot}</kbd>
                  )}
                </button>
                <button
                  type="button"
                  role="menuitem"
                  className="tray-menu__item"
                  disabled={busy}
                  onClick={() => void handleAction('recording')}
                >
                  <Video size={15} className="tray-menu__icon" aria-hidden="true" />
                  <span className="tray-menu__label">{t('tray.recording', 'Record Screen')}</span>
                  {shortcuts.recording && (
                    <kbd className="tray-menu__shortcut">{shortcuts.recording}</kbd>
                  )}
                </button>
                <button
                  type="button"
                  role="menuitem"
                  className="tray-menu__item"
                  disabled={busy}
                  onClick={() => void handlePinPaste()}
                >
                  <Pin size={15} className="tray-menu__icon" aria-hidden="true" />
                  <span className="tray-menu__label">{t('tray.pinPaste', 'Paste as Pin')}</span>
                  {shortcuts.pinPaste && (
                    <kbd className="tray-menu__shortcut">{shortcuts.pinPaste}</kbd>
                  )}
                </button>
              </>
            )}

            {searchActive && (
              <button
                type="button"
                role="menuitem"
                className="tray-menu__item"
                disabled={busy}
                onClick={() => void handleAction('search')}
              >
                <Search size={15} className="tray-menu__icon" aria-hidden="true" />
                <span className="tray-menu__label">{t('tray.search', 'Search Files')}</span>
                {shortcuts.search && (
                  <kbd className="tray-menu__shortcut">{shortcuts.search}</kbd>
                )}
              </button>
            )}

            {clipboardActive && (
              <button
                type="button"
                role="menuitem"
                className="tray-menu__item"
                disabled={busy}
                onClick={() => void handleAction('clipboard')}
              >
                <ClipboardList size={15} className="tray-menu__icon" aria-hidden="true" />
                <span className="tray-menu__label">{t('tray.clipboard', 'Clipboard History')}</span>
                {shortcuts.clipboard && (
                  <kbd className="tray-menu__shortcut">{shortcuts.clipboard}</kbd>
                )}
              </button>
            )}

            {remoteBoostEnabled && (
              <button
                type="button"
                role="menuitem"
                className="tray-menu__item"
                disabled={busy}
                onClick={() => void handleAction('emergencyFlush')}
              >
                <RotateCcw size={15} className="tray-menu__icon" aria-hidden="true" />
                <span className="tray-menu__label">{t('tray.emergencyFlush', 'Release Stuck Keys')}</span>
                {shortcuts.emergencyFlush && (
                  <kbd className="tray-menu__shortcut">{shortcuts.emergencyFlush}</kbd>
                )}
              </button>
            )}
          </div>

          {/* 独立贴图与分组管理 Bento 专区 */}
          {captureActive && (
            <>
              <div className="tray-menu__divider" role="separator" />
              <TrayPinGroupPanel
                state={pinGroupState}
                expanded={pinPanelExpanded}
                disabled={busy}
                onToggleExpand={() => setPinPanelExpanded((prev) => !prev)}
                onControl={(payload) => void handlePinGroupControl(payload)}
              />
            </>
          )}
        </div>
      </div>

      {/* 3. 吸底固定系统底栏（管理员权限切换 + 退出，永不被任何高度截断或遮挡） */}
      <div ref={footerRef} className="tray-footer-dock" role="group">
        <button
          type="button"
          role="menuitem"
          className={`tray-footer-btn tray-footer-btn--admin ${elevated ? 'tray-menu__item--admin-active' : ''} ${elevating ? 'tray-menu__item--elevating' : ''}`}
          disabled={busy || elevating}
          onClick={() => void handleToggleElevated()}
          title={elevated
            ? t('tray.adminActiveDesc', 'Running as Administrator (click to switch to standard mode)')
            : t('tray.restartElevated', 'Run as Administrator')}
        >
          {elevating ? (
            <RotateCw size={14} className="tray-menu__icon tray-menu__icon--spinning" aria-hidden="true" />
          ) : elevated ? (
            <ShieldCheck size={14} className="tray-menu__icon tray-menu__icon--admin" aria-hidden="true" />
          ) : (
            <Shield size={14} className="tray-menu__icon" aria-hidden="true" />
          )}
          <span className="tray-menu__label">
            {elevating
              ? t('tray.elevating', 'Switching privilege...')
              : elevated
                ? t('tray.adminActive', 'Administrator Mode')
                : t('tray.restartElevated', 'Run as Administrator')}
          </span>
          <span className={`tray-menu__status-badge ${elevated ? 'tray-menu__status-badge--admin' : ''}`}>
            {elevated ? t('tray.adminBadge', 'Admin') : t('tray.standardBadge', 'Standard')}
          </span>
        </button>

        <button
          type="button"
          role="menuitem"
          className="tray-footer-btn tray-footer-btn--exit tray-menu__item--danger"
          disabled={busy}
          onClick={() => void handleAction('exit')}
          title={t('tray.exit', 'Quit')}
        >
          <LogOut size={14} className="tray-menu__icon" aria-hidden="true" />
          <span>{t('tray.exit', 'Quit')}</span>
        </button>
      </div>
    </div>
  );
}

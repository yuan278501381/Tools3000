/* ─────────────────────────────────────────────────────────────────────────────
 * WindowManagerPage.tsx — 窗口管理总控与可视化布局工作台
 *
 * 架构设计:
 *   1. 默认全关、零开销承诺：顶部展示实时进程态（关闭时 0 MB / 0 进程，开启后实时 PID/内存/计数）
 *   2. 8 大独立子功能各自默认关闭，支持按需组合
 *   3. 4 枚精工分段工作台：SnapZones（矢量编辑器 + PowerToys 导入）/ Workspaces / Quick Switch / 轻量窗口工具
 * ───────────────────────────────────────────────────────────────────────────── */

import { useCallback, useEffect, useMemo, useState, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  Activity,
  AlertCircle,
  AppWindow,
  Camera,
  Contrast,
  Download,
  Eye,
  Focus,
  Layers,
  LayoutGrid,
  Loader2,
  Maximize2,
  Monitor,
  Move,
  Pencil,
  Pin,
  Play,
  RefreshCw,
  RotateCcw,
  Sparkles,
  SquareDashed,
  Trash2,
  Wrench,
} from 'lucide-react';
import {
  Badge,
  Button,
  Card,
  CodeBadge,
  ColorPickerInput,
  DisabledCascade,
  Field,
  Modal,
  NumberInput,
  PageSkeleton,
  Select,
  SettingGroup,
  SettingRow,
  Tabs,
  TextInput,
  Toggle,
} from '../components/UIKit';
import { HotkeyAppExclusionBar } from '../components/business/HotkeyAppExclusionBar';
import { ZoneLayoutEditor } from '../components/business/ZoneLayoutEditor';
import {
  consumePendingDeepNavigation,
  type DeepNavigationPayload,
  type WindowManagerPageSubTab,
} from '../config/settingsSearchIndex';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import {
  BUILTIN_DEFAULT_LAYOUTS,
  DEFAULT_WINDOW_MANAGER_RESPONSE,
  DEFAULT_WINDOW_MANAGER_SETTINGS,
  type AltDragModifier,
  type MonitorDescriptor,
  type QuickSwitchDensity,
  type SnapTriggerMode,
  type SnapZoneLayout,
  type VirtualDesktopDescriptor,
  type WindowManagerActionName,
  type WindowManagerFeatures,
  type WindowManagerResponse,
  type WindowManagerRuntimeStats,
  type WindowManagerSettings,
  type WorkspaceItem,
} from '../types/windowManager';
import './WindowManagerPage.css';

function normalizeResponse(raw: unknown): WindowManagerResponse {
  if (!raw || typeof raw !== 'object') {
    return DEFAULT_WINDOW_MANAGER_RESPONSE;
  }
  const obj = raw as Partial<WindowManagerResponse>;
  const rawSettings = (obj.settings && typeof obj.settings === 'object'
    ? obj.settings
    : {}) as Partial<WindowManagerSettings>;

  const layouts =
    Array.isArray(rawSettings.snapZones?.layouts) && rawSettings.snapZones.layouts.length > 0
      ? rawSettings.snapZones.layouts
      : BUILTIN_DEFAULT_LAYOUTS;

  const settings: WindowManagerSettings = {
    features: {
      ...DEFAULT_WINDOW_MANAGER_SETTINGS.features,
      ...(rawSettings.features ?? {}),
    },
    snapZones: {
      ...DEFAULT_WINDOW_MANAGER_SETTINGS.snapZones,
      ...(rawSettings.snapZones ?? {}),
      excludedApps: Array.isArray(rawSettings.snapZones?.excludedApps)
        ? rawSettings.snapZones.excludedApps
        : [],
      layouts,
      monitorBindings:
        rawSettings.snapZones?.monitorBindings && typeof rawSettings.snapZones.monitorBindings === 'object'
          ? rawSettings.snapZones.monitorBindings
          : {},
    },
    workspaces: {
      ...DEFAULT_WINDOW_MANAGER_SETTINGS.workspaces,
      ...(rawSettings.workspaces ?? {}),
      items: Array.isArray(rawSettings.workspaces?.items) ? rawSettings.workspaces.items : [],
    },
    quickSwitch: {
      ...DEFAULT_WINDOW_MANAGER_SETTINGS.quickSwitch,
      ...(rawSettings.quickSwitch ?? {}),
    },
    alwaysOnTop: {
      ...DEFAULT_WINDOW_MANAGER_SETTINGS.alwaysOnTop,
      ...(rawSettings.alwaysOnTop ?? {}),
    },
    opacity: {
      ...DEFAULT_WINDOW_MANAGER_SETTINGS.opacity,
      ...(rawSettings.opacity ?? {}),
    },
    altDrag: {
      ...DEFAULT_WINDOW_MANAGER_SETTINGS.altDrag,
      ...(rawSettings.altDrag ?? {}),
      excludedApps: Array.isArray(rawSettings.altDrag?.excludedApps)
        ? rawSettings.altDrag.excludedApps
        : [],
    },
    focusBorder: {
      ...DEFAULT_WINDOW_MANAGER_SETTINGS.focusBorder,
      ...(rawSettings.focusBorder ?? {}),
    },
    multiMonitor: {
      ...DEFAULT_WINDOW_MANAGER_SETTINGS.multiMonitor,
      ...(rawSettings.multiMonitor ?? {}),
    },
  };

  return {
    enabled: Boolean(obj.enabled),
    running: Boolean(obj.running),
    exeAvailable: obj.exeAvailable !== false,
    runtimeStats: obj.runtimeStats,
    settings,
  };
}

function formatBytesMb(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0.0 MB';
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function countEnabledFeatures(features: WindowManagerFeatures): number {
  return Object.values(features).filter(Boolean).length;
}

const VALID_SUBTABS = new Set<WindowManagerPageSubTab>([
  'snap_zones',
  'workspaces',
  'quick_switch',
  'utilities',
]);

export const WindowManagerPage: FC = () => {
  const { t } = useTranslation();
  const [loading, setLoading] = useState(true);
  const [data, setData] = useState<WindowManagerResponse>(DEFAULT_WINDOW_MANAGER_RESPONSE);
  const [monitors, setMonitors] = useState<MonitorDescriptor[]>([]);
  const [selectedMonitorId, setSelectedMonitorId] = useState<string>('');
  const [virtualDesktops, setVirtualDesktops] = useState<VirtualDesktopDescriptor[]>([]);
  const [selectedDesktopId, setSelectedDesktopId] = useState<string>('');
  const [activeTab, setActiveTab] = useState<WindowManagerPageSubTab>(() => {
    const pending = consumePendingDeepNavigation('window_manager');
    if (pending?.tabId && VALID_SUBTABS.has(pending.tabId as WindowManagerPageSubTab)) {
      return pending.tabId as WindowManagerPageSubTab;
    }
    return 'snap_zones';
  });
  const [saving, setSaving] = useState(false);
  const [busyAction, setBusyAction] = useState<string | null>(null);
  const [newWorkspaceName, setNewWorkspaceName] = useState('');
  const [editingWorkspace, setEditingWorkspace] = useState<WorkspaceItem | null>(null);

  const loadAll = useCallback(async () => {
    try {
      const [settingsRes, monitorsRes] = await Promise.allSettled([
        bridgeRequest<WindowManagerResponse>('windowManager.getSettings', undefined, { silent: true }),
        bridgeRequest<{
          monitors?: MonitorDescriptor[];
          virtualDesktops?: VirtualDesktopDescriptor[];
          currentDesktopId?: string;
        }>('windowManager.getMonitors', undefined, { silent: true }),
      ]);

      if (settingsRes.status === 'fulfilled' && settingsRes.value) {
        setData(normalizeResponse(settingsRes.value));
      }
      if (monitorsRes.status === 'fulfilled' && monitorsRes.value) {
        if (Array.isArray(monitorsRes.value.monitors)) {
          const list = monitorsRes.value.monitors;
          setMonitors(list);
          setSelectedMonitorId((prev) => {
            if (prev && list.some((m) => m.id === prev)) return prev;
            const primary = list.find((m) => m.isPrimary) ?? list[0];
            return primary?.id ?? '';
          });
        }
        if (Array.isArray(monitorsRes.value.virtualDesktops)) {
          const desktops = monitorsRes.value.virtualDesktops;
          const curId = monitorsRes.value.currentDesktopId ?? desktops.find((d) => d.isCurrent)?.id ?? '';
          setVirtualDesktops(desktops);
          setSelectedDesktopId((prev) => {
            if (prev && desktops.some((d) => d.id === prev)) return prev;
            return curId;
          });
        }
      }
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void loadAll();
  }, [loadAll]);

  // 深度搜索直达子页签
  useEffect(() => {
    const onDeepNav = (ev: Event) => {
      const detail = (ev as CustomEvent<DeepNavigationPayload>).detail;
      if (detail?.navId === 'window_manager' && detail.tabId && VALID_SUBTABS.has(detail.tabId as WindowManagerPageSubTab)) {
        setActiveTab(detail.tabId as WindowManagerPageSubTab);
      }
    };
    window.addEventListener('tools3000:navigate-deep', onDeepNav);
    return () => window.removeEventListener('tools3000:navigate-deep', onDeepNav);
  }, []);

  useBridgeEvent('windowManager:stateChanged', (raw: unknown) => {
    if (raw && typeof raw === 'object' && 'settings' in raw) {
      setData(normalizeResponse(raw));
    } else {
      void loadAll();
    }
  });

  useBridgeEvent('window_manager:stateChanged', (raw: unknown) => {
    if (raw && typeof raw === 'object' && 'settings' in raw) {
      setData(normalizeResponse(raw));
    } else {
      void loadAll();
    }
  });

  const commitPatch = useCallback(
    async (
      patch: Record<string, unknown>,
      optimisticNext: WindowManagerResponse,
      toastOk = false,
    ) => {
      const prev = data;
      setData(optimisticNext);
      setSaving(true);
      try {
        const res = await bridgeRequest<WindowManagerResponse>(
          'windowManager.updateSettings',
          patch,
          { silent: true },
        );
        if (res && typeof res === 'object' && ('settings' in res || 'enabled' in res)) {
          setData(normalizeResponse(res));
        }
        if (toastOk) {
          toast.success(t('general.toastSaved', 'Settings saved'));
        }
      } catch (err) {
        setData(prev);
        toast.error(t('windowManager.saveFailed', 'Failed to save window manager settings'), {
          description: String(err),
        });
      } finally {
        setSaving(false);
      }
    },
    [data, t],
  );

  const handleToggleMaster = (nextEnabled: boolean) => {
    void commitPatch(
      { enabled: nextEnabled },
      { ...data, enabled: nextEnabled, running: nextEnabled ? data.running : false },
      true,
    );
  };

  const handleToggleFeature = (feature: keyof WindowManagerFeatures, checked: boolean) => {
    const nextSettings: WindowManagerSettings = {
      ...data.settings,
      features: {
        ...data.settings.features,
        [feature]: checked,
      },
    };
    const nextEnabled = checked ? true : data.enabled;
    void commitPatch(
      checked && !data.enabled
        ? { enabled: true, settings: nextSettings }
        : { settings: nextSettings },
      { ...data, enabled: nextEnabled, settings: nextSettings },
    );
  };

  const updateSettingsSection = <K extends keyof WindowManagerSettings>(
    section: K,
    nextValue: WindowManagerSettings[K],
  ) => {
    const nextSettings: WindowManagerSettings = {
      ...data.settings,
      [section]: nextValue,
    };
    void commitPatch(
      { settings: nextSettings },
      { ...data, settings: nextSettings },
    );
  };

  const handleRefreshStats = async () => {
    setBusyAction('stats');
    try {
      const stats = await bridgeRequest<WindowManagerRuntimeStats>(
        'windowManager.getRuntimeStats',
        undefined,
        { silent: true },
      );
      if (stats && typeof stats === 'object') {
        setData((prev) => ({
          ...prev,
          running: typeof stats.running === 'boolean' ? stats.running : prev.running,
          runtimeStats: stats,
        }));
      }
    } catch (err) {
      toast.error(t('windowManager.statsLoadFailed', 'Failed to refresh runtime statistics'), {
        description: String(err),
      });
    } finally {
      setBusyAction(null);
    }
  };

  const handleExecAction = async (
    action: WindowManagerActionName,
    extra?: { index?: number; workspaceId?: string; monitorId?: string; desktopId?: string; layoutId?: string },
  ) => {
    setBusyAction(action);
    try {
      await bridgeRequest(
        'windowManager.execAction',
        { action, ...(extra ?? {}) },
        { silent: true },
      );
      if (action !== 'open_layout_editor') {
        toast.success(t('windowManager.actionTriggered', 'Action executed'));
      }
    } catch (err) {
      toast.error(t('windowManager.actionFailed', 'Failed to execute window action'), {
        description: String(err),
      });
    } finally {
      setBusyAction(null);
    }
  };

  const handleImportPowerToys = async () => {
    setBusyAction('import_powertoys');
    try {
      const res = await bridgeRequest<{ importedCount?: number; layouts?: SnapZoneLayout[] }>(
        'windowManager.importPowerToysLayouts',
        undefined,
        { silent: true },
      );
      const count = typeof res?.importedCount === 'number' ? res.importedCount : 0;
      if (Array.isArray(res?.layouts) && res.layouts.length > 0) {
        const merged = [...data.settings.snapZones.layouts];
        for (const incoming of res.layouts) {
          const idx = merged.findIndex((item) => item.id === incoming.id);
          if (idx >= 0) merged[idx] = incoming;
          else merged.push(incoming);
        }
        const nextSnap = { ...data.settings.snapZones, layouts: merged };
        const nextSettings: WindowManagerSettings = { ...data.settings, snapZones: nextSnap };
        setData((prev) => ({ ...prev, settings: nextSettings }));
        await bridgeRequest('windowManager.updateSettings', { settings: nextSettings }, { silent: true }).catch(() => {});
      }
      if (count > 0) {
        toast.success(
          t('windowManager.importPowerToysSuccess', 'Imported {{count}} layout(s) from PowerToys FancyZones', {
            count,
          }),
        );
      } else {
        toast.info(
          t('windowManager.importPowerToysEmpty', 'No PowerToys FancyZones custom layouts were found on this machine'),
        );
      }
    } catch (err) {
      toast.error(t('windowManager.importPowerToysFailed', 'Failed to import PowerToys layouts'), {
        description: String(err),
      });
    } finally {
      setBusyAction(null);
    }
  };

  const handleCaptureWorkspace = async () => {
    const trimmed = newWorkspaceName.trim() || t('windowManager.defaultWorkspaceName', 'Workspace {{num}}', {
      num: data.settings.workspaces.items.length + 1,
    });
    setBusyAction('capture_workspace');
    try {
      const res = await bridgeRequest<{ workspace?: WorkspaceItem }>(
        'windowManager.captureWorkspace',
        { name: trimmed },
        { silent: true },
      );
      if (res?.workspace) {
        const exists = data.settings.workspaces.items.some((w) => w.id === res.workspace!.id);
        const nextItems = exists
          ? data.settings.workspaces.items.map((w) => (w.id === res.workspace!.id ? res.workspace! : w))
          : [...data.settings.workspaces.items, res.workspace];
        updateSettingsSection('workspaces', {
          ...data.settings.workspaces,
          items: nextItems,
        });
        setNewWorkspaceName('');
        toast.success(
          t('windowManager.workspaceCapturedToast', 'Captured workspace "{{name}}"', {
            name: res.workspace.name,
          }),
        );
      }
    } catch (err) {
      toast.error(t('windowManager.workspaceCaptureFailed', 'Failed to capture current desktop workspace'), {
        description: String(err),
      });
    } finally {
      setBusyAction(null);
    }
  };

  const handleDeleteWorkspace = (id: string) => {
    const nextItems = data.settings.workspaces.items.filter((item) => item.id !== id);
    updateSettingsSection('workspaces', {
      ...data.settings.workspaces,
      items: nextItems,
    });
    toast.success(t('windowManager.workspaceDeletedToast', 'Workspace deleted'));
  };

  const handleSaveEditedWorkspace = () => {
    if (!editingWorkspace) return;
    const cleanName = editingWorkspace.name.trim();
    if (!cleanName) return;
    const nextItems = data.settings.workspaces.items.map((item) =>
      item.id === editingWorkspace.id ? { ...editingWorkspace, name: cleanName } : item,
    );
    updateSettingsSection('workspaces', {
      ...data.settings.workspaces,
      items: nextItems,
    });
    setEditingWorkspace(null);
    toast.success(t('general.toastSaved', 'Settings saved'));
  };

  const effectiveMonitorId = useMemo(() => {
    if (selectedMonitorId) return selectedMonitorId;
    return monitors[0]?.id ?? 'primary';
  }, [selectedMonitorId, monitors]);

  const perDesktopEnabled = data.settings.snapZones.perVirtualDesktop;

  const effectiveDesktopId = useMemo(() => {
    if (!perDesktopEnabled) return '';
    if (selectedDesktopId && virtualDesktops.some((d) => d.id === selectedDesktopId)) {
      return selectedDesktopId;
    }
    return virtualDesktops.find((d) => d.isCurrent)?.id ?? virtualDesktops[0]?.id ?? '';
  }, [perDesktopEnabled, selectedDesktopId, virtualDesktops]);

  const effectiveBindingKey = useMemo(() => {
    if (perDesktopEnabled && effectiveDesktopId) {
      return `${effectiveMonitorId}@${effectiveDesktopId}`;
    }
    return effectiveMonitorId;
  }, [perDesktopEnabled, effectiveMonitorId, effectiveDesktopId]);

  const activeLayoutId = useMemo(() => {
    const bindings = data.settings.snapZones.monitorBindings;
    const perDesktopBound = perDesktopEnabled && effectiveDesktopId ? bindings[effectiveBindingKey] : undefined;
    if (perDesktopBound && data.settings.snapZones.layouts.some((l) => l.id === perDesktopBound)) {
      return perDesktopBound;
    }
    const monitorBound = bindings[effectiveMonitorId];
    if (monitorBound && data.settings.snapZones.layouts.some((l) => l.id === monitorBound)) {
      return monitorBound;
    }
    return data.settings.snapZones.layouts[0]?.id ?? '';
  }, [
    data.settings.snapZones.layouts,
    data.settings.snapZones.monitorBindings,
    perDesktopEnabled,
    effectiveDesktopId,
    effectiveBindingKey,
    effectiveMonitorId,
  ]);

  const handleSelectLayoutForMonitor = (layoutId: string) => {
    const nextBindings = {
      ...data.settings.snapZones.monitorBindings,
      [effectiveBindingKey]: layoutId,
    };
    updateSettingsSection('snapZones', {
      ...data.settings.snapZones,
      monitorBindings: nextBindings,
    });
  };

  const handleClearDesktopOverride = () => {
    if (!perDesktopEnabled || !effectiveDesktopId) return;
    if (!(effectiveBindingKey in data.settings.snapZones.monitorBindings)) return;
    const nextBindings = { ...data.settings.snapZones.monitorBindings };
    delete nextBindings[effectiveBindingKey];
    updateSettingsSection('snapZones', {
      ...data.settings.snapZones,
      monitorBindings: nextBindings,
    });
  };

  const handleRefreshVirtualDesktops = async () => {
    setBusyAction('refresh_desktops');
    try {
      const res = await bridgeRequest<{
        desktops?: VirtualDesktopDescriptor[];
        currentDesktopId?: string;
      }>('windowManager.getVirtualDesktops', undefined, { silent: true });
      if (Array.isArray(res?.desktops)) {
        setVirtualDesktops(res.desktops);
        if (res.currentDesktopId && !res.desktops.some((d) => d.id === selectedDesktopId)) {
          setSelectedDesktopId(res.currentDesktopId);
        }
      }
    } finally {
      setBusyAction(null);
    }
  };

  const handleLayoutsChanged = (nextLayouts: SnapZoneLayout[], nextActiveId?: string) => {
    const nextBindings = nextActiveId
      ? {
          ...data.settings.snapZones.monitorBindings,
          [effectiveBindingKey]: nextActiveId,
        }
      : data.settings.snapZones.monitorBindings;
    updateSettingsSection('snapZones', {
      ...data.settings.snapZones,
      layouts: nextLayouts,
      monitorBindings: nextBindings,
    });
  };

  const handleOpenFullscreenEditor = () => {
    void handleExecAction('open_layout_editor', {
      monitorId: effectiveMonitorId,
      desktopId: perDesktopEnabled ? effectiveDesktopId : undefined,
      layoutId: activeLayoutId || undefined,
    });
  };

  const formatMonitorLabel = (mon: MonitorDescriptor, index: number) => {
    const raw = (mon.name || '').trim();
    if (!raw || raw.startsWith('\\\\.\\')) {
      return t('windowManager.monitorIndexedFallback', 'Display {{num}}', { num: index + 1 });
    }
    return raw;
  };

  const formatDesktopLabel = (desk: VirtualDesktopDescriptor) => {
    const raw = (desk.name || '').trim();
    if (raw) return raw;
    return t('windowManager.virtualDesktopIndexed', 'Desktop {{num}}', { num: desk.index });
  };

  if (loading) {
    return <PageSkeleton cards={3} rowsPerCard={3} />;
  }

  const { settings } = data;
  const activeCount = countEnabledFeatures(settings.features);
  const stats = data.runtimeStats;
  const isRuntimeLive = data.enabled && (data.running || Boolean(stats?.running));

  return (
    <div className="wm-page">
      {/* ── 1. 顶部总开关与实时运行时遥测卡片 ──────────────────────────────── */}
      <Card className="wm-hero-card">
        <div className="wm-hero">
          <div className="wm-hero__info">
            <div className="wm-hero__title-row">
              <LayoutGrid size={20} className="wm-hero__icon" aria-hidden="true" />
              <h2 className="wm-hero__title">
                {t('windowManager.heroTitle', 'Window Manager')}
              </h2>
              <Badge
                variant={isRuntimeLive ? 'success' : 'muted'}
                text={
                  isRuntimeLive
                    ? t('windowManager.badgeActiveFeatures', '{{count}} Active', { count: activeCount })
                    : t('windowManager.badgeZeroIdle', 'Zero Overhead')
                }
              />
            </div>
            <p className="wm-hero__desc">
              {t(
                'windowManager.heroDesc',
                'Modular Rust-powered window tiling, desktop workspaces, instant switcher, and lightweight window utilities. All 8 capabilities default to OFF with zero background footprint until enabled.',
              )}
            </p>
          </div>
          <div className="wm-hero__switch">
            <Toggle
              id="window-manager-master-toggle"
              checked={data.enabled}
              disabled={!data.exeAvailable || saving}
              onChange={handleToggleMaster}
            />
          </div>
        </div>

        {/* 实时进程与内存状态条 */}
        <div className="wm-telemetry-bar">
          <div className="wm-telemetry-status">
            <span
              className={`wm-status-dot ${
                !data.exeAvailable
                  ? 'wm-status-dot--error'
                  : isRuntimeLive
                    ? 'wm-status-dot--active'
                    : 'wm-status-dot--idle'
              }`}
              aria-hidden="true"
            />
            <span className="wm-telemetry-label">
              {!data.exeAvailable
                ? t('windowManager.statusMissingBinary', 'Window Manager helper binary not found')
                : isRuntimeLive
                  ? t('windowManager.statusRunning', 'Helper Process Running')
                  : t('windowManager.statusStopped', 'Stopped (0 MB · 0 Processes)')}
            </span>
          </div>

          <div className="wm-telemetry-pills">
            {isRuntimeLive && stats ? (
              <>
                <span className="wm-metric-pill">
                  <Activity size={13} aria-hidden="true" />
                  <span>{t('windowManager.metricPid', 'PID {{pid}}', { pid: stats.pid })}</span>
                </span>
                <span className="wm-metric-pill">
                  <span>{t('windowManager.metricMemory', 'RAM {{mem}}', { mem: formatBytesMb(stats.workingSetBytes) })}</span>
                </span>
                {stats.pinnedCount > 0 && (
                  <span className="wm-metric-pill">
                    <Pin size={13} aria-hidden="true" />
                    <span>{t('windowManager.metricPinned', '{{count}} Pinned', { count: stats.pinnedCount })}</span>
                  </span>
                )}
                {stats.trayMinimizedCount > 0 && (
                  <span className="wm-metric-pill">
                    <AppWindow size={13} aria-hidden="true" />
                    <span>{t('windowManager.metricTray', '{{count}} In Tray', { count: stats.trayMinimizedCount })}</span>
                  </span>
                )}
              </>
            ) : (
              <span className="wm-metric-pill wm-metric-pill--muted">
                <span>{t('windowManager.runtimeZeroOverhead', '0 MB RAM · 0 Active Hooks')}</span>
              </span>
            )}
            <Button
              variant="ghost"
              size="sm"
              disabled={busyAction === 'stats'}
              onClick={() => void handleRefreshStats()}
            >
              <RefreshCw
                size={13}
                className={busyAction === 'stats' ? 'wm-spin' : ''}
                aria-hidden="true"
              />
              <span>{t('common.refresh', 'Refresh')}</span>
            </Button>
          </div>
        </div>

        {!data.exeAvailable && (
          <div className="wm-warning-banner" role="alert">
            <AlertCircle size={16} aria-hidden="true" />
            <span>
              {t(
                'windowManager.missingBinaryNotice',
                'The standalone Window Manager companion executable is missing from this installation. Reinstall or repair Tools3000 to enable it.',
              )}
            </span>
          </div>
        )}
      </Card>

      {/* ── 2. 四大分段工作台导航 ──────────────────────────────────────────── */}
      <Tabs<WindowManagerPageSubTab>
        activeId={activeTab}
        onChange={setActiveTab}
        ariaLabel={t('windowManager.tabsAria', 'Window Manager Sections')}
        tabs={[
          {
            id: 'snap_zones',
            label: t('windowManager.tabSnapZones', 'SnapZones'),
            icon: <SquareDashed size={15} aria-hidden="true" />,
            badge: settings.features.snapZones ? <span className="wm-tab-dot" aria-hidden="true" /> : undefined,
          },
          {
            id: 'workspaces',
            label: t('windowManager.tabWorkspaces', 'Workspaces'),
            icon: <Layers size={15} aria-hidden="true" />,
            badge: settings.features.workspaces ? <span className="wm-tab-dot" aria-hidden="true" /> : undefined,
          },
          {
            id: 'quick_switch',
            label: t('windowManager.tabQuickSwitch', 'Quick Switch'),
            icon: <Sparkles size={15} aria-hidden="true" />,
            badge: settings.features.quickSwitch ? <span className="wm-tab-dot" aria-hidden="true" /> : undefined,
          },
          {
            id: 'utilities',
            label: t('windowManager.tabUtilities', 'Window Utilities'),
            icon: <Wrench size={15} aria-hidden="true" />,
            badge:
              settings.features.alwaysOnTop ||
              settings.features.opacity ||
              settings.features.altDrag ||
              settings.features.focusBorder ||
              settings.features.multiMonitor ? (
                <span className="wm-tab-dot" aria-hidden="true" />
              ) : undefined,
          },
        ]}
      />

      {/* ── TAB 1: SnapZones 可视化分屏 ── */}
      {activeTab === 'snap_zones' && (
        <div className="wm-tab-panel">
          <Card className="wm-feature-head-card">
            <div className="wm-feature-head">
              <div className="wm-feature-head__info">
                <div className="wm-feature-head__title">
                  <SquareDashed size={17} aria-hidden="true" />
                  <span>{t('windowManager.snapZonesTitle', 'SnapZones Magnetic Tiling')}</span>
                </div>
                <p className="wm-feature-head__desc">
                  {t(
                    'windowManager.snapZonesDesc',
                    'Hold Shift (or right-click) while dragging any window to snap cleanly into custom grid or canvas zones per monitor.',
                  )}
                </p>
              </div>
              <Toggle
                id="wm-toggle-snapzones"
                checked={settings.features.snapZones}
                disabled={!data.exeAvailable}
                onChange={(val) => handleToggleFeature('snapZones', val)}
              />
            </div>
          </Card>

          <DisabledCascade disabled={!settings.features.snapZones} className="wm-cascade-stack">
            {/* 显示器选择与 PowerToys 一键迁移栏 */}
            <SettingGroup
              title={t('windowManager.layoutEditorSection', 'Monitor & Visual Layout Editor')}
              icon={<Monitor size={16} aria-hidden="true" />}
            >
              <Card>
                <div className="wm-monitor-bar">
                  <div className="wm-monitor-list" role="tablist" aria-label={t('windowManager.monitorsAria', 'Displays')}>
                    {(monitors.length > 0
                      ? monitors
                      : [{ id: 'primary', name: t('windowManager.primaryMonitorFallback', 'Primary Display'), width: 1920, height: 1080, dpi: 96, isPrimary: true }]
                    ).map((mon, idx) => {
                      const isSelected = mon.id === effectiveMonitorId;
                      const scalePct = Math.round((mon.dpi / 96) * 100);
                      return (
                        <button
                          key={mon.id}
                          type="button"
                          role="tab"
                          aria-selected={isSelected}
                          className={`wm-monitor-pill ${isSelected ? 'is-selected' : ''}`}
                          onClick={() => setSelectedMonitorId(mon.id)}
                        >
                          <Monitor size={14} aria-hidden="true" />
                          <span className="wm-monitor-pill__name">{formatMonitorLabel(mon, idx)}</span>
                          <CodeBadge variant="muted">
                            {mon.width}x{mon.height} · {scalePct}%
                          </CodeBadge>
                          {mon.isPrimary && (
                            <Badge variant="primary" text={t('windowManager.primaryBadge', 'Primary')} />
                          )}
                        </button>
                      );
                    })}
                  </div>

                  <div className="wm-monitor-actions">
                    <Button
                      variant="primary"
                      size="sm"
                      disabled={busyAction === 'open_layout_editor'}
                      onClick={handleOpenFullscreenEditor}
                    >
                      <Maximize2 size={14} aria-hidden="true" />
                      <span>{t('windowManager.launchFullscreenEditorBtn', '1:1 Fullscreen Editor')}</span>
                      <CodeBadge variant="muted">Win+Shift+~</CodeBadge>
                    </Button>
                    <Button
                      variant="secondary"
                      size="sm"
                      disabled={busyAction === 'import_powertoys'}
                      onClick={() => void handleImportPowerToys()}
                    >
                      {busyAction === 'import_powertoys' ? (
                        <Loader2 size={14} className="wm-spin" aria-hidden="true" />
                      ) : (
                        <Download size={14} aria-hidden="true" />
                      )}
                      <span>{t('windowManager.importPowerToysBtn', 'Import PowerToys')}</span>
                    </Button>
                    <Button
                      variant="ghost"
                      size="sm"
                      disabled={busyAction === 'show_zones_preview'}
                      onClick={() => void handleExecAction('show_zones_preview')}
                    >
                      <Eye size={14} aria-hidden="true" />
                      <span>{t('windowManager.previewZonesBtn', 'Preview Overlay')}</span>
                    </Button>
                  </div>
                </div>

                {/* 1:1 全屏原生透明编辑引导横幅（凸显真实桌面透视、自动磁吸真实窗口与一键捕获） */}
                <div className="wm-fullscreen-callout">
                  <div className="wm-fullscreen-callout__body">
                    <div className="wm-fullscreen-callout__title">
                      <Sparkles size={14} aria-hidden="true" />
                      <span>
                        {t(
                          'windowManager.fullscreenCalloutTitle',
                          'Edit Directly on Your Real Desktop (1:1 WYSIWYG)',
                        )}
                      </span>
                    </div>
                    <p className="wm-fullscreen-callout__desc">
                      {t(
                        'windowManager.fullscreenCalloutDesc',
                        'Press Win+Shift+~ anytime to overlay your live desktop windows at exact pixel scale, snap zone edges to real open apps, or capture your current windows into zones in one click.',
                      )}
                    </p>
                  </div>
                </div>

                {/* 按虚拟桌面独立绑定选择器（开启按虚拟桌面独立保存布局时呈现） */}
                {perDesktopEnabled && (
                  <div className="wm-desktop-bar" role="region" aria-label={t('windowManager.virtualDesktopBarAria', 'Virtual Desktops')}>
                    <div className="wm-desktop-bar__head">
                      <span className="wm-desktop-bar__label">
                        <Layers size={14} aria-hidden="true" />
                        <span>{t('windowManager.virtualDesktopScopeLabel', 'Virtual Desktop Binding')}</span>
                      </span>
                      <div className="wm-desktop-bar__head-actions">
                        {effectiveDesktopId &&
                          `${effectiveMonitorId}@${effectiveDesktopId}` in settings.snapZones.monitorBindings && (
                            <Button
                              variant="ghost"
                              size="sm"
                              onClick={handleClearDesktopOverride}
                            >
                              <RotateCcw size={12} aria-hidden="true" />
                              <span>{t('windowManager.resetDesktopBindingBtn', 'Follow Default')}</span>
                            </Button>
                          )}
                        <Button
                          variant="ghost"
                          size="sm"
                          disabled={busyAction === 'refresh_desktops'}
                          onClick={() => void handleRefreshVirtualDesktops()}
                        >
                          <RefreshCw
                            size={12}
                            className={busyAction === 'refresh_desktops' ? 'wm-spin' : ''}
                            aria-hidden="true"
                          />
                          <span>{t('common.refresh', 'Refresh')}</span>
                        </Button>
                      </div>
                    </div>

                    <div className="wm-desktop-list" role="tablist" aria-label={t('windowManager.virtualDesktopBarAria', 'Virtual Desktops')}>
                      {virtualDesktops.length === 0 ? (
                        <span className="wm-desktop-empty">
                          {t('windowManager.noVirtualDesktopsHint', 'Only a single default Windows desktop was detected.')}
                        </span>
                      ) : (
                        virtualDesktops.map((desk) => {
                          const isSelected = desk.id === effectiveDesktopId;
                          const boundId = settings.snapZones.monitorBindings[`${effectiveMonitorId}@${desk.id}`];
                          const boundLayout = boundId
                            ? settings.snapZones.layouts.find((l) => l.id === boundId)
                            : null;
                          return (
                            <button
                              key={desk.id}
                              type="button"
                              role="tab"
                              aria-selected={isSelected}
                              className={`wm-desktop-pill ${isSelected ? 'is-selected' : ''}`}
                              onClick={() => setSelectedDesktopId(desk.id)}
                            >
                              {desk.isCurrent && (
                                <span
                                  className="wm-desktop-pill__dot"
                                  title={t('windowManager.currentDesktopBadge', 'Current')}
                                  aria-hidden="true"
                                />
                              )}
                              <span className="wm-desktop-pill__name">{formatDesktopLabel(desk)}</span>
                              {desk.isCurrent && (
                                <Badge variant="success" text={t('windowManager.currentDesktopBadge', 'Current')} />
                              )}
                              <CodeBadge variant={boundLayout ? 'default' : 'muted'}>
                                {boundLayout
                                  ? boundLayout.name
                                  : t('windowManager.inheritMonitorDefault', 'Default')}
                              </CodeBadge>
                            </button>
                          );
                        })
                      )}
                    </div>
                  </div>
                )}

                <ZoneLayoutEditor
                  layouts={settings.snapZones.layouts}
                  activeLayoutId={activeLayoutId}
                  showZoneNumbers={settings.snapZones.showZoneNumbers}
                  zoneGap={settings.snapZones.zoneGap}
                  disabled={!settings.features.snapZones}
                  onSelectLayout={handleSelectLayoutForMonitor}
                  onChangeLayouts={handleLayoutsChanged}
                />
              </Card>
            </SettingGroup>

            {/* 分屏行为参数 */}
            <SettingGroup title={t('windowManager.snapBehaviorSection', 'Trigger & Behavior')}>
              <Card>
                <SettingRow
                  label={t('windowManager.triggerModeLabel', 'Zone Activation Trigger')}
                  description={t('windowManager.triggerModeDesc', 'Choose how to reveal the zone overlay while dragging a window')}
                >
                  <Select
                    value={settings.snapZones.triggerMode}
                    options={[
                      { value: 'shift', label: t('windowManager.triggerShift', 'Hold Shift While Dragging') },
                      { value: 'right_click', label: t('windowManager.triggerRightClick', 'Right-Click While Dragging') },
                      { value: 'edge', label: t('windowManager.triggerEdge', 'Screen Edge Hover') },
                    ]}
                    onChange={(val) =>
                      updateSettingsSection('snapZones', {
                        ...settings.snapZones,
                        triggerMode: val as SnapTriggerMode,
                      })
                    }
                  />
                </SettingRow>

                <SettingRow
                  label={t('windowManager.spanMultipleLabel', 'Allow Spanning Multiple Zones')}
                  description={t('windowManager.spanMultipleDesc', 'Hold Ctrl or drag across zone boundaries to span adjacent zones')}
                >
                  <Toggle
                    id="wm-snap-span"
                    checked={settings.snapZones.spanMultipleZones}
                    onChange={(checked) =>
                      updateSettingsSection('snapZones', { ...settings.snapZones, spanMultipleZones: checked })
                    }
                  />
                </SettingRow>

                <SettingRow
                  label={t('windowManager.showNumbersLabel', 'Show Zone Numbers')}
                  description={t('windowManager.showNumbersDesc', 'Display large zone index badges on the overlay for quick keyboard snapping')}
                >
                  <Toggle
                    id="wm-snap-numbers"
                    checked={settings.snapZones.showZoneNumbers}
                    onChange={(checked) =>
                      updateSettingsSection('snapZones', { ...settings.snapZones, showZoneNumbers: checked })
                    }
                  />
                </SettingRow>

                <SettingRow
                  label={t('windowManager.zoneGapLabel', 'Spacing Around Zones')}
                  description={t('windowManager.zoneGapDesc', 'Visual padding between snapped windows in pixels')}
                >
                  <NumberInput
                    value={settings.snapZones.zoneGap}
                    min={0}
                    max={48}
                    step={2}
                    unit="px"
                    ariaLabel={t('windowManager.zoneGapLabel', 'Spacing Around Zones')}
                    onChange={(val) =>
                      updateSettingsSection('snapZones', { ...settings.snapZones, zoneGap: val })
                    }
                  />
                </SettingRow>

                <SettingRow
                  label={t('windowManager.overlayOpacityLabel', 'Overlay Opacity')}
                  description={t('windowManager.overlayOpacityDesc', 'Transparency of zone preview cards while dragging')}
                >
                  <div className="wm-range-control">
                    <input
                      type="range"
                      min={15}
                      max={95}
                      step={5}
                      value={settings.snapZones.overlayOpacity}
                      aria-label={t('windowManager.overlayOpacityLabel', 'Overlay Opacity')}
                      className="wm-range-slider"
                      onChange={(e) =>
                        updateSettingsSection('snapZones', {
                          ...settings.snapZones,
                          overlayOpacity: Number(e.target.value),
                        })
                      }
                    />
                    <CodeBadge>{settings.snapZones.overlayOpacity}%</CodeBadge>
                  </div>
                </SettingRow>

                <SettingRow
                  label={t('windowManager.rememberLastZoneLabel', 'Remember Last Known Zone')}
                  description={t('windowManager.rememberLastZoneDesc', 'Automatically snap newly opened app windows back to their previous zone')}
                >
                  <Toggle
                    id="wm-snap-remember"
                    checked={settings.snapZones.rememberLastZone}
                    onChange={(checked) =>
                      updateSettingsSection('snapZones', { ...settings.snapZones, rememberLastZone: checked })
                    }
                  />
                </SettingRow>

                <SettingRow
                  label={t('windowManager.perDesktopLabel', 'Separate Layout per Virtual Desktop')}
                  description={t('windowManager.perDesktopDesc', 'Allow each Windows virtual desktop to bind an independent layout')}
                >
                  <Toggle
                    id="wm-snap-per-desktop"
                    checked={settings.snapZones.perVirtualDesktop}
                    onChange={(checked) =>
                      updateSettingsSection('snapZones', { ...settings.snapZones, perVirtualDesktop: checked })
                    }
                  />
                </SettingRow>

                <SettingRow
                  label={t('windowManager.snapExcludedAppsLabel', 'Excluded Applications')}
                  description={t('windowManager.snapExcludedAppsDesc', 'Never trigger SnapZones overlay while dragging windows from these processes')}
                  layout="vertical"
                >
                  <HotkeyAppExclusionBar
                    apps={settings.snapZones.excludedApps}
                    onChange={(nextApps) =>
                      updateSettingsSection('snapZones', { ...settings.snapZones, excludedApps: nextApps })
                    }
                    emptyText={t('windowManager.noExcludedApps', 'No excluded processes yet')}
                    pickerTitle={t('windowManager.pickExcludedAppTitle', 'Pick Application to Exclude')}
                  />
                </SettingRow>
              </Card>
            </SettingGroup>
          </DisabledCascade>
        </div>
      )}

      {/* ── TAB 2: Workspaces 桌面工作区一键编排 ── */}
      {activeTab === 'workspaces' && (
        <div className="wm-tab-panel">
          <Card className="wm-feature-head-card">
            <div className="wm-feature-head">
              <div className="wm-feature-head__info">
                <div className="wm-feature-head__title">
                  <Layers size={17} aria-hidden="true" />
                  <span>{t('windowManager.workspacesTitle', 'Desktop Workspaces')}</span>
                </div>
                <p className="wm-feature-head__desc">
                  {t(
                    'windowManager.workspacesDesc',
                    'Snapshot your current running apps and exact window coordinates, then restore an entire coding, writing, or trading setup in one click.',
                  )}
                </p>
              </div>
              <Toggle
                id="wm-toggle-workspaces"
                checked={settings.features.workspaces}
                disabled={!data.exeAvailable}
                onChange={(val) => handleToggleFeature('workspaces', val)}
              />
            </div>
          </Card>

          <DisabledCascade disabled={!settings.features.workspaces} className="wm-cascade-stack">
            <Card>
              <SettingRow
                label={t('windowManager.minimizeUnrelatedLabel', 'Minimize Unrelated Windows on Launch')}
                description={t('windowManager.minimizeUnrelatedDesc', 'Automatically minimize windows not listed in the launched workspace')}
              >
                <Toggle
                  id="wm-ws-minimize-unrelated"
                  checked={settings.workspaces.minimizeUnrelated}
                  onChange={(checked) =>
                    updateSettingsSection('workspaces', {
                      ...settings.workspaces,
                      minimizeUnrelated: checked,
                    })
                  }
                />
              </SettingRow>

              <div className="wm-ws-capture-bar">
                <div className="wm-ws-capture-bar__input">
                  <TextInput
                    value={newWorkspaceName}
                    onChange={setNewWorkspaceName}
                    placeholder={t('windowManager.workspaceNamePlaceholder', 'Workspace name (e.g. Deep Coding, Morning Briefing)')}
                    ariaLabel={t('windowManager.workspaceNameAria', 'New workspace name')}
                  />
                </div>
                <Button
                  variant="primary"
                  size="sm"
                  disabled={busyAction === 'capture_workspace'}
                  onClick={() => void handleCaptureWorkspace()}
                >
                  {busyAction === 'capture_workspace' ? (
                    <Loader2 size={14} className="wm-spin" aria-hidden="true" />
                  ) : (
                    <Camera size={14} aria-hidden="true" />
                  )}
                  <span>{t('windowManager.captureDesktopBtn', 'Capture Current Desktop')}</span>
                </Button>
              </div>
            </Card>

            {settings.workspaces.items.length === 0 ? (
              <Card>
                <div className="wm-empty-state">
                  <Layers size={28} aria-hidden="true" />
                  <div className="wm-empty-state__title">
                    {t('windowManager.workspacesEmptyTitle', 'No Saved Workspaces Yet')}
                  </div>
                  <p className="wm-empty-state__desc">
                    {t(
                      'windowManager.workspacesEmptyDesc',
                      'Arrange your favorite apps across monitors, then click Capture Current Desktop above to save a reusable snapshot.',
                    )}
                  </p>
                </div>
              </Card>
            ) : (
              <div className="wm-ws-grid">
                {settings.workspaces.items.map((ws) => (
                  <Card key={ws.id} className="wm-ws-card">
                    <div className="wm-ws-card__header">
                      <div className="wm-ws-card__identity">
                        <h3 className="wm-ws-card__title">{ws.name}</h3>
                        <Badge
                          variant="muted"
                          text={t('windowManager.workspaceAppsCount', '{{count}} App(s)', {
                            count: ws.apps.length,
                          })}
                        />
                      </div>
                      <div className="wm-ws-card__actions">
                        <Button
                          variant="primary"
                          size="sm"
                          disabled={busyAction === 'launch_workspace'}
                          onClick={() =>
                            void handleExecAction('launch_workspace', { workspaceId: ws.id })
                          }
                        >
                          <Play size={13} aria-hidden="true" />
                          <span>{t('windowManager.launchWorkspaceBtn', 'Launch')}</span>
                        </Button>
                        <Button
                          variant="ghost"
                          size="sm"
                          onClick={() => setEditingWorkspace({ ...ws, apps: ws.apps.map((a) => ({ ...a })) })}
                          ariaLabel={t('common.edit', 'Edit')}
                          title={t('common.edit', 'Edit')}
                        >
                          <Pencil size={13} aria-hidden="true" />
                        </Button>
                        <Button
                          variant="ghost"
                          size="sm"
                          onClick={() => handleDeleteWorkspace(ws.id)}
                          ariaLabel={t('common.delete', 'Delete')}
                          title={t('common.delete', 'Delete')}
                        >
                          <Trash2 size={13} aria-hidden="true" />
                        </Button>
                      </div>
                    </div>

                    <div className="wm-ws-card__apps">
                      {ws.apps.length === 0 ? (
                        <span className="wm-ws-card__empty-apps">
                          {t('windowManager.workspaceNoApps', 'No applications recorded in this workspace')}
                        </span>
                      ) : (
                        ws.apps.map((app, idx) => (
                          <div key={`${app.processName}-${idx}`} className="wm-ws-app-chip">
                            <CodeBadge>{app.processName || app.exePath}</CodeBadge>
                            {typeof app.zoneIndex === 'number' && (
                              <span className="wm-ws-app-meta">
                                {t('windowManager.zoneBadge', 'Zone {{num}}', { num: app.zoneIndex + 1 })}
                              </span>
                            )}
                            {app.maximized && (
                              <span className="wm-ws-app-meta">
                                {t('windowManager.maximizedBadge', 'Max')}
                              </span>
                            )}
                          </div>
                        ))
                      )}
                    </div>
                  </Card>
                ))}
              </div>
            )}
          </DisabledCascade>
        </div>
      )}

      {/* ── TAB 3: Quick Switch 毫秒级窗口速切器 ── */}
      {activeTab === 'quick_switch' && (
        <div className="wm-tab-panel">
          <Card className="wm-feature-head-card">
            <div className="wm-feature-head">
              <div className="wm-feature-head__info">
                <div className="wm-feature-head__title">
                  <Sparkles size={17} aria-hidden="true" />
                  <span>{t('windowManager.quickSwitchTitle', 'Quick Switch Window Launcher')}</span>
                </div>
                <p className="wm-feature-head__desc">
                  {t(
                    'windowManager.quickSwitchDesc',
                    'Keyboard-first window switcher with instant title/process filtering, Pinyin initials search, and 1-9 number jumping.',
                  )}
                </p>
              </div>
              <div className="wm-feature-head__right">
                <Button
                  variant="secondary"
                  size="sm"
                  disabled={!settings.features.quickSwitch || busyAction === 'open_quick_switch'}
                  onClick={() => void handleExecAction('open_quick_switch')}
                >
                  <Play size={13} aria-hidden="true" />
                  <span>{t('windowManager.tryQuickSwitchBtn', 'Try Now')}</span>
                </Button>
                <Toggle
                  id="wm-toggle-quickswitch"
                  checked={settings.features.quickSwitch}
                  disabled={!data.exeAvailable}
                  onChange={(val) => handleToggleFeature('quickSwitch', val)}
                />
              </div>
            </div>
          </Card>

          <DisabledCascade disabled={!settings.features.quickSwitch} className="wm-cascade-stack">
            <Card>
              <SettingRow
                label={t('windowManager.qsCurrentMonitorLabel', 'Current Monitor Windows Only')}
                description={t('windowManager.qsCurrentMonitorDesc', 'Show only windows located on the active display')}
              >
                <Toggle
                  id="wm-qs-current-monitor"
                  checked={settings.quickSwitch.currentMonitorOnly}
                  onChange={(checked) =>
                    updateSettingsSection('quickSwitch', { ...settings.quickSwitch, currentMonitorOnly: checked })
                  }
                />
              </SettingRow>

              <SettingRow
                label={t('windowManager.qsShowPreviewLabel', 'Show Live Window Preview')}
                description={t('windowManager.qsShowPreviewDesc', 'Render DWM thumbnail preview of the highlighted window')}
              >
                <Toggle
                  id="wm-qs-preview"
                  checked={settings.quickSwitch.showPreview}
                  onChange={(checked) =>
                    updateSettingsSection('quickSwitch', { ...settings.quickSwitch, showPreview: checked })
                  }
                />
              </SettingRow>

              <SettingRow
                label={t('windowManager.qsShowNumbersLabel', 'Show 1-9 Quick Jump Badges')}
                description={t('windowManager.qsShowNumbersDesc', 'Press Alt+1..9 or number keys to focus matching windows immediately')}
              >
                <Toggle
                  id="wm-qs-numbers"
                  checked={settings.quickSwitch.showNumbers}
                  onChange={(checked) =>
                    updateSettingsSection('quickSwitch', { ...settings.quickSwitch, showNumbers: checked })
                  }
                />
              </SettingRow>

              <SettingRow
                label={t('windowManager.qsDensityLabel', 'List Density')}
                description={t('windowManager.qsDensityDesc', 'Choose between dense multi-window view or relaxed spacing')}
              >
                <div className="wm-segmented" role="group" aria-label={t('windowManager.qsDensityLabel', 'List Density')}>
                  {(['compact', 'comfortable'] as QuickSwitchDensity[]).map((mode) => (
                    <button
                      key={mode}
                      type="button"
                      className={`wm-segmented__btn ${settings.quickSwitch.density === mode ? 'is-active' : ''}`}
                      onClick={() =>
                        updateSettingsSection('quickSwitch', { ...settings.quickSwitch, density: mode })
                      }
                    >
                      {mode === 'compact'
                        ? t('windowManager.densityCompact', 'Compact')
                        : t('windowManager.densityComfortable', 'Comfortable')}
                    </button>
                  ))}
                </div>
              </SettingRow>

              <SettingRow
                label={t('windowManager.qsPinyinLabel', 'Enable Pinyin Initials Search')}
                description={t('windowManager.qsPinyinDesc', 'Match Chinese window titles using Pinyin initials (e.g. "wx" for WeChat)')}
              >
                <Toggle
                  id="wm-qs-pinyin"
                  checked={settings.quickSwitch.enablePinyinSearch}
                  onChange={(checked) =>
                    updateSettingsSection('quickSwitch', { ...settings.quickSwitch, enablePinyinSearch: checked })
                  }
                />
              </SettingRow>
            </Card>
          </DisabledCascade>
        </div>
      )}

      {/* ── TAB 4: 5 项轻量窗口工具（各自独立开关） ── */}
      {activeTab === 'utilities' && (
        <div className="wm-tab-panel wm-utilities-stack">
          {/* 4.1 窗口置顶 Always on Top */}
          <Card className="wm-util-card">
            <div className="wm-feature-head">
              <div className="wm-feature-head__info">
                <div className="wm-feature-head__title">
                  <Pin size={16} aria-hidden="true" />
                  <span>{t('windowManager.utilAlwaysOnTopTitle', 'Always on Top')}</span>
                </div>
                <p className="wm-feature-head__desc">
                  {t('windowManager.utilAlwaysOnTopDesc', 'Pin any window above all others with an optional glowing indicator border.')}
                </p>
              </div>
              <div className="wm-feature-head__right">
                <Button
                  variant="ghost"
                  size="sm"
                  disabled={!settings.features.alwaysOnTop}
                  onClick={() => void handleExecAction('toggle_always_on_top')}
                >
                  <Pin size={13} aria-hidden="true" />
                  <span>{t('windowManager.pinActiveBtn', 'Toggle Pin')}</span>
                </Button>
                <Toggle
                  id="wm-toggle-always-on-top"
                  checked={settings.features.alwaysOnTop}
                  disabled={!data.exeAvailable}
                  onChange={(val) => handleToggleFeature('alwaysOnTop', val)}
                />
              </div>
            </div>
            <DisabledCascade disabled={!settings.features.alwaysOnTop}>
              <div className="wm-util-body">
                <SettingRow label={t('windowManager.aotShowBorderLabel', 'Highlight Pinned Window Border')}>
                  <Toggle
                    id="wm-aot-border"
                    checked={settings.alwaysOnTop.showBorder}
                    onChange={(checked) =>
                      updateSettingsSection('alwaysOnTop', { ...settings.alwaysOnTop, showBorder: checked })
                    }
                  />
                </SettingRow>
                <SettingRow label={t('windowManager.aotThicknessLabel', 'Border Thickness')}>
                  <NumberInput
                    value={settings.alwaysOnTop.borderThickness}
                    min={1}
                    max={12}
                    step={1}
                    unit="px"
                    ariaLabel={t('windowManager.aotThicknessLabel', 'Border Thickness')}
                    onChange={(val) =>
                      updateSettingsSection('alwaysOnTop', { ...settings.alwaysOnTop, borderThickness: val })
                    }
                  />
                </SettingRow>
                <SettingRow label={t('windowManager.aotColorLabel', 'Border Color')}>
                  <div className="wm-color-control">
                    <ColorPickerInput
                      value={settings.alwaysOnTop.borderColor}
                      ariaLabel={t('windowManager.aotColorLabel', 'Border Color')}
                      className="wm-color-swatch"
                      onChange={(hex) =>
                        updateSettingsSection('alwaysOnTop', { ...settings.alwaysOnTop, borderColor: hex })
                      }
                    />
                    <CodeBadge>{settings.alwaysOnTop.borderColor.toUpperCase()}</CodeBadge>
                  </div>
                </SettingRow>
              </div>
            </DisabledCascade>
          </Card>

          {/* 4.2 窗口透明度 Window Opacity */}
          <Card className="wm-util-card">
            <div className="wm-feature-head">
              <div className="wm-feature-head__info">
                <div className="wm-feature-head__title">
                  <Contrast size={16} aria-hidden="true" />
                  <span>{t('windowManager.utilOpacityTitle', 'Window Opacity')}</span>
                </div>
                <p className="wm-feature-head__desc">
                  {t('windowManager.utilOpacityDesc', 'Adjust any window transparency on the fly via shortcut or titlebar mouse wheel.')}
                </p>
              </div>
              <div className="wm-feature-head__right">
                <Button
                  variant="ghost"
                  size="sm"
                  disabled={!settings.features.opacity}
                  onClick={() => void handleExecAction('opacity_reset')}
                >
                  <RotateCcw size={13} aria-hidden="true" />
                  <span>{t('windowManager.resetOpacityBtn', 'Reset Opacity')}</span>
                </Button>
                <Toggle
                  id="wm-toggle-opacity"
                  checked={settings.features.opacity}
                  disabled={!data.exeAvailable}
                  onChange={(val) => handleToggleFeature('opacity', val)}
                />
              </div>
            </div>
            <DisabledCascade disabled={!settings.features.opacity}>
              <div className="wm-util-body">
                <SettingRow label={t('windowManager.opacityStepLabel', 'Opacity Step')}>
                  <NumberInput
                    value={settings.opacity.stepPercent}
                    min={5}
                    max={25}
                    step={5}
                    unit="%"
                    ariaLabel={t('windowManager.opacityStepLabel', 'Opacity Step')}
                    onChange={(val) =>
                      updateSettingsSection('opacity', { ...settings.opacity, stepPercent: val })
                    }
                  />
                </SettingRow>
                <SettingRow label={t('windowManager.opacityMinLabel', 'Minimum Opacity Floor')}>
                  <NumberInput
                    value={settings.opacity.minPercent}
                    min={10}
                    max={80}
                    step={5}
                    unit="%"
                    ariaLabel={t('windowManager.opacityMinLabel', 'Minimum Opacity Floor')}
                    onChange={(val) =>
                      updateSettingsSection('opacity', { ...settings.opacity, minPercent: val })
                    }
                  />
                </SettingRow>
                <SettingRow
                  label={t('windowManager.opacityWheelLabel', 'Scroll Wheel on Titlebar')}
                  description={t('windowManager.opacityWheelDesc', 'Roll the mouse wheel over a window titlebar to fade transparency smoothly')}
                >
                  <Toggle
                    id="wm-opacity-wheel"
                    checked={settings.opacity.wheelOnTitlebar}
                    onChange={(checked) =>
                      updateSettingsSection('opacity', { ...settings.opacity, wheelOnTitlebar: checked })
                    }
                  />
                </SettingRow>
              </div>
            </DisabledCascade>
          </Card>

          {/* 4.3 任意位置拖拽与缩放 Easy Drag & Resize */}
          <Card className="wm-util-card">
            <div className="wm-feature-head">
              <div className="wm-feature-head__info">
                <div className="wm-feature-head__title">
                  <Move size={16} aria-hidden="true" />
                  <span>{t('windowManager.utilAltDragTitle', 'Easy Drag & Resize (Alt-Drag)')}</span>
                </div>
                <p className="wm-feature-head__desc">
                  {t('windowManager.utilAltDragDesc', 'Hold modifier key + left drag anywhere to move a window, or right drag to resize from the nearest corner.')}
                </p>
              </div>
              <Toggle
                id="wm-toggle-alt-drag"
                checked={settings.features.altDrag}
                disabled={!data.exeAvailable}
                onChange={(val) => handleToggleFeature('altDrag', val)}
              />
            </div>
            <DisabledCascade disabled={!settings.features.altDrag}>
              <div className="wm-util-body">
                <SettingRow label={t('windowManager.altDragModifierLabel', 'Modifier Key')}>
                  <div className="wm-segmented" role="group" aria-label={t('windowManager.altDragModifierLabel', 'Modifier Key')}>
                    {(['alt', 'ctrl_alt'] as AltDragModifier[]).map((mod) => (
                      <button
                        key={mod}
                        type="button"
                        className={`wm-segmented__btn ${settings.altDrag.modifier === mod ? 'is-active' : ''}`}
                        onClick={() =>
                          updateSettingsSection('altDrag', { ...settings.altDrag, modifier: mod })
                        }
                      >
                        {mod === 'alt' ? 'Alt' : 'Ctrl + Alt'}
                      </button>
                    ))}
                  </div>
                </SettingRow>
                <SettingRow label={t('windowManager.altDragSnapLabel', 'Snap to SnapZones During Alt-Drag')}>
                  <Toggle
                    id="wm-altdrag-snap"
                    checked={settings.altDrag.snapToZones}
                    onChange={(checked) =>
                      updateSettingsSection('altDrag', { ...settings.altDrag, snapToZones: checked })
                    }
                  />
                </SettingRow>
                <SettingRow
                  label={t('windowManager.altDragExcludedLabel', 'Excluded Applications')}
                  description={t('windowManager.altDragExcludedDesc', 'Preserve native Alt+drag in CAD, Blender, Photoshop, or games')}
                  layout="vertical"
                >
                  <HotkeyAppExclusionBar
                    apps={settings.altDrag.excludedApps}
                    onChange={(nextApps) =>
                      updateSettingsSection('altDrag', { ...settings.altDrag, excludedApps: nextApps })
                    }
                    emptyText={t('windowManager.noExcludedApps', 'No excluded processes yet')}
                    pickerTitle={t('windowManager.pickExcludedAppTitle', 'Pick Application to Exclude')}
                  />
                </SettingRow>
              </div>
            </DisabledCascade>
          </Card>

          {/* 4.4 活动窗口焦点边框 Focus Border */}
          <Card className="wm-util-card">
            <div className="wm-feature-head">
              <div className="wm-feature-head__info">
                <div className="wm-feature-head__title">
                  <Focus size={16} aria-hidden="true" />
                  <span>{t('windowManager.utilFocusBorderTitle', 'Active Window Focus Border')}</span>
                </div>
                <p className="wm-feature-head__desc">
                  {t('windowManager.utilFocusBorderDesc', 'Draw a crisp non-intrusive accent outline around the currently focused window on dense multi-monitor desks.')}
                </p>
              </div>
              <Toggle
                id="wm-toggle-focus-border"
                checked={settings.features.focusBorder}
                disabled={!data.exeAvailable}
                onChange={(val) => handleToggleFeature('focusBorder', val)}
              />
            </div>
            <DisabledCascade disabled={!settings.features.focusBorder}>
              <div className="wm-util-body">
                <SettingRow label={t('windowManager.focusThicknessLabel', 'Outline Thickness')}>
                  <NumberInput
                    value={settings.focusBorder.thickness}
                    min={1}
                    max={10}
                    step={1}
                    unit="px"
                    ariaLabel={t('windowManager.focusThicknessLabel', 'Outline Thickness')}
                    onChange={(val) =>
                      updateSettingsSection('focusBorder', { ...settings.focusBorder, thickness: val })
                    }
                  />
                </SettingRow>
                <SettingRow label={t('windowManager.focusColorLabel', 'Outline Color')}>
                  <div className="wm-color-control">
                    <ColorPickerInput
                      value={settings.focusBorder.color}
                      ariaLabel={t('windowManager.focusColorLabel', 'Outline Color')}
                      className="wm-color-swatch"
                      onChange={(hex) =>
                        updateSettingsSection('focusBorder', { ...settings.focusBorder, color: hex })
                      }
                    />
                    <CodeBadge>{settings.focusBorder.color.toUpperCase()}</CodeBadge>
                  </div>
                </SettingRow>
                <SettingRow label={t('windowManager.focusRadiusLabel', 'Corner Radius')}>
                  <NumberInput
                    value={settings.focusBorder.cornerRadius}
                    min={0}
                    max={24}
                    step={2}
                    unit="px"
                    ariaLabel={t('windowManager.focusRadiusLabel', 'Corner Radius')}
                    onChange={(val) =>
                      updateSettingsSection('focusBorder', { ...settings.focusBorder, cornerRadius: val })
                    }
                  />
                </SettingRow>
              </div>
            </DisabledCascade>
          </Card>

          {/* 4.5 多屏协同与托盘收纳 Multi-Monitor & Tray */}
          <Card className="wm-util-card">
            <div className="wm-feature-head">
              <div className="wm-feature-head__info">
                <div className="wm-feature-head__title">
                  <Monitor size={16} aria-hidden="true" />
                  <span>{t('windowManager.utilMultiMonitorTitle', 'Multi-Monitor & Minimize to Tray')}</span>
                </div>
                <p className="wm-feature-head__desc">
                  {t('windowManager.utilMultiMonitorDesc', 'Center new windows, toss windows across displays, tuck any window into the system tray, and rescue off-screen windows.')}
                </p>
              </div>
              <Toggle
                id="wm-toggle-multi-monitor"
                checked={settings.features.multiMonitor}
                disabled={!data.exeAvailable}
                onChange={(val) => handleToggleFeature('multiMonitor', val)}
              />
            </div>
            <DisabledCascade disabled={!settings.features.multiMonitor}>
              <div className="wm-util-body">
                <SettingRow
                  label={t('windowManager.mmCenterNewLabel', 'Center New Windows on Active Monitor')}
                  description={t('windowManager.mmCenterNewDesc', 'Open newly launched windows neatly centered on the screen where your pointer is')}
                >
                  <Toggle
                    id="wm-mm-center"
                    checked={settings.multiMonitor.centerNewWindows}
                    onChange={(checked) =>
                      updateSettingsSection('multiMonitor', { ...settings.multiMonitor, centerNewWindows: checked })
                    }
                  />
                </SettingRow>
                <SettingRow
                  label={t('windowManager.mmTrayMinimizeLabel', 'Enable Minimize Any Window to Tray')}
                  description={t('windowManager.mmTrayMinimizeDesc', 'Allow tucking background apps into the system tray notification area')}
                >
                  <Toggle
                    id="wm-mm-tray"
                    checked={settings.multiMonitor.enableTrayMinimize}
                    onChange={(checked) =>
                      updateSettingsSection('multiMonitor', { ...settings.multiMonitor, enableTrayMinimize: checked })
                    }
                  />
                </SettingRow>

                <div className="wm-quick-actions-bar">
                  <Button
                    variant="secondary"
                    size="sm"
                    onClick={() => void handleExecAction('move_next_monitor')}
                  >
                    <Monitor size={13} aria-hidden="true" />
                    <span>{t('windowManager.actionNextMonitor', 'Move to Next Monitor')}</span>
                  </Button>
                  <Button
                    variant="secondary"
                    size="sm"
                    onClick={() => void handleExecAction('move_next_zone')}
                  >
                    <SquareDashed size={13} aria-hidden="true" />
                    <span>{t('windowManager.actionNextZone', 'Move to Next Zone')}</span>
                  </Button>
                  <Button
                    variant="secondary"
                    size="sm"
                    onClick={() => void handleExecAction('rescue_offscreen')}
                  >
                    <RotateCcw size={13} aria-hidden="true" />
                    <span>{t('windowManager.actionRescueOffscreen', 'Rescue Off-Screen Windows')}</span>
                  </Button>
                  <Button
                    variant="secondary"
                    size="sm"
                    onClick={() => void handleExecAction('restore_all_tray')}
                  >
                    <AppWindow size={13} aria-hidden="true" />
                    <span>{t('windowManager.actionRestoreTray', 'Restore All Tray Windows')}</span>
                  </Button>
                </div>
              </div>
            </DisabledCascade>
          </Card>
        </div>
      )}

      {/* ── 编辑工作区弹窗 ─────────────────────────────────────────────────── */}
      {editingWorkspace && (
        <Modal
          open
          title={t('windowManager.editWorkspaceModalTitle', 'Edit Workspace')}
          onClose={() => setEditingWorkspace(null)}
          footer={
            <>
              <Button variant="ghost" onClick={() => setEditingWorkspace(null)}>
                {t('common.cancel', 'Cancel')}
              </Button>
              <Button
                variant="primary"
                disabled={!editingWorkspace.name.trim()}
                onClick={handleSaveEditedWorkspace}
              >
                {t('common.save', 'Save')}
              </Button>
            </>
          }
        >
          <div className="wm-edit-ws-form">
            <Field label={t('common.name', 'Name')}>
              <TextInput
                value={editingWorkspace.name}
                onChange={(val) => setEditingWorkspace({ ...editingWorkspace, name: val })}
              />
            </Field>
            <div className="wm-edit-ws-apps">
              <div className="wm-edit-ws-apps__label">
                {t('windowManager.workspaceAppsLabel', 'Included Applications')}
              </div>
              {editingWorkspace.apps.length === 0 ? (
                <p className="wm-ws-card__empty-apps">
                  {t('windowManager.workspaceNoApps', 'No applications recorded in this workspace')}
                </p>
              ) : (
                editingWorkspace.apps.map((app, idx) => (
                  <div key={`${app.processName}-${idx}`} className="wm-edit-ws-app-row">
                    <div className="wm-edit-ws-app-row__meta">
                      <CodeBadge>{app.processName || app.exePath}</CodeBadge>
                      <TextInput
                        value={app.args}
                        placeholder={t('windowManager.launchArgsPlaceholder', 'Optional launch arguments')}
                        ariaLabel={t('windowManager.launchArgsPlaceholder', 'Optional launch arguments')}
                        onChange={(args) => {
                          const nextApps = editingWorkspace.apps.map((a, i) =>
                            i === idx ? { ...a, args } : a,
                          );
                          setEditingWorkspace({ ...editingWorkspace, apps: nextApps });
                        }}
                      />
                    </div>
                    <Button
                      variant="ghost"
                      size="sm"
                      ariaLabel={t('common.delete', 'Delete')}
                      onClick={() => {
                        const nextApps = editingWorkspace.apps.filter((_, i) => i !== idx);
                        setEditingWorkspace({ ...editingWorkspace, apps: nextApps });
                      }}
                    >
                      <Trash2 size={13} aria-hidden="true" />
                    </Button>
                  </div>
                ))
              )}
            </div>
          </div>
        </Modal>
      )}
    </div>
  );
};

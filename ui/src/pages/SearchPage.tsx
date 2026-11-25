/* ─────────────────────────────────────────────────────────────────────────────
 * SearchPage.tsx — 文件搜索配置、服务观测、外部 CLI/MCP 接口与语法手册
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, type FC } from 'react';
import {
  Card,
  Toggle,
  SettingRow,
  SettingGroup,
  SubSettingGroup,
  DisabledCascade,
  Button,
  Tabs,
  Select,
  PageSkeleton,
  type TabItem,
  CodeBadge,
} from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { SearchExternalApiCard } from '../components/SearchExternalApiCard';
import { SearchExcludedPathsManager } from '../components/SearchExcludedPathsManager';
import {
  CUSTOM_EXCLUDED_PATHS_STORAGE_KEY,
  mergeExcludedPaths,
} from '../search/excludedPathsUtils';
import { CATEGORY_DEFS } from '../search/searchConstants';
import type { SearchMemoryMode, SearchMode, SearchRankingProfile } from '../search/searchTypes';
import { formatBytes } from '../search/searchUtils';
import { safeStorageGetEnum, safeStorageGetJsonArray, safeStorageSetItem } from '../search/storageUtils';
import { consumePendingDeepNavigation, type DeepNavigationPayload } from '../config/settingsSearchIndex';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import {
  Search,
  HardDrive,
  Code2,
  BookOpen,
  CheckCircle2,
  FolderSearch,
  FolderX,
  Sparkles,
  Zap,
  Play,
  RotateCw,
  Cpu,
  Power,
  ShieldCheck,
  Sliders,
  Terminal,
} from 'lucide-react';
import './SearchPage.css';

type SearchTabId = 'settings' | 'status' | 'external' | 'syntax';

interface SearchSettings {
  hotkey: string;
  maxResults: number;
  defaultCategory: string;
  caseSensitive: boolean;
  matchPath: boolean;
  pinyinEnabled: boolean;
  includeBookmarks?: boolean;
  enableAppsAndCommands?: boolean;
  liveRefreshEnabled?: boolean;
  rankingProfile?: SearchRankingProfile;
  memoryMode?: SearchMemoryMode;
  idleTrimMinutes?: number;
  idleStopMinutes?: number;
  iconStyle?: 'native' | 'vector';
  autoBypassFullscreen?: boolean;
  customExcludedPaths?: string[];
  externalApiEnabled?: boolean;
  externalApiExcludePaths?: string;
  externalApiAllowedRoots?: string;
}

interface ServiceStatus {
  available: boolean;
  running?: boolean;
  pipeName: string;
  workingSetBytes?: number;
  privateBytes?: number;
  memoryMode?: SearchMemoryMode;
  idleTrimMinutes?: number;
  idleStopMinutes?: number;
  idleSeconds?: number;
  lastTrimmedAt?: number;
}

interface PluginListItem {
  id: string;
  enabled: boolean;
  active?: boolean;
  state?: string;
}

const IDLE_TRIM_MINUTE_OPTIONS = [2, 5, 10, 15, 30] as const;
const IDLE_STOP_MINUTE_OPTIONS = [10, 15, 30, 60, 120] as const;

const normalizeSearchTab = (raw?: string): SearchTabId => {
  if (raw === 'status') return 'status';
  if (raw === 'external') return 'external';
  if (raw === 'syntax' || raw === 'regex') return 'syntax';
  return 'settings';
};

export const SearchPage: FC = () => {
  const { t, i18n } = useTranslation();
  const isChineseUi = (i18n.resolvedLanguage ?? i18n.language ?? '').toLowerCase().startsWith('zh');
  const [activeTab, setActiveTab] = useState<SearchTabId>(
    () => normalizeSearchTab(consumePendingDeepNavigation('search')?.tabId),
  );
  const [searchEnabled, setSearchEnabled] = useState(true);
  const [savingMaster, setSavingMaster] = useState(false);

  useEffect(() => {
    const onDeepNavigate = (e: Event) => {
      const detail = (e as CustomEvent<DeepNavigationPayload>).detail;
      if (detail?.navId === 'search' && detail.tabId) {
        consumePendingDeepNavigation('search');
        setActiveTab(normalizeSearchTab(detail.tabId));
      }
    };
    window.addEventListener('tools3000:navigate-deep', onDeepNavigate);
    return () => window.removeEventListener('tools3000:navigate-deep', onDeepNavigate);
  }, []);

  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [settings, setSettings] = useState<SearchSettings>({
    hotkey: 'Alt+Space',
    maxResults: 50,
    defaultCategory: 'all',
    caseSensitive: false,
    matchPath: false,
    pinyinEnabled: isChineseUi,
    includeBookmarks: true,
    enableAppsAndCommands: true,
    liveRefreshEnabled: true,
    rankingProfile: 'balanced_frecency',
    memoryMode: 'idle_trim',
    idleTrimMinutes: 2,
    idleStopMinutes: 30,
    iconStyle: 'native',
    autoBypassFullscreen: true,
    customExcludedPaths: [],
    externalApiEnabled: false,
    externalApiExcludePaths: '',
    externalApiAllowedRoots: '',
  });
  const [searchMode, setSearchMode] = useState<SearchMode>(() =>
    safeStorageGetEnum('tools3000_search_default_mode', ['name', 'both', 'content'] as const, 'name'),
  );
  const [serviceStatus, setServiceStatus] = useState<ServiceStatus>({
    available: false,
    running: false,
    pipeName: '\\\\.\\pipe\\Tools3000SearchPipe',
    workingSetBytes: 0,
    privateBytes: 0,
    memoryMode: 'idle_trim',
    idleTrimMinutes: 2,
    idleStopMinutes: 30,
    idleSeconds: 0,
    lastTrimmedAt: 0,
  });
  const [loading, setLoading] = useState(true);
  const [checking, setChecking] = useState(false);
  const [waking, setWaking] = useState(false);
  const [trimming, setTrimming] = useState(false);
  const [stopping, setStopping] = useState(false);

  const getHotkey = (name: string) => hotkeys.find((h) => h.name === name);

  useBridgeEvent('plugins:changed', (raw: unknown) => {
    const payload = raw && typeof raw === 'object' ? (raw as { id?: string; enabled?: boolean }) : null;
    if (payload?.id === 'search' && typeof payload.enabled === 'boolean') {
      setSearchEnabled(payload.enabled);
    }
  });

  useEffect(() => {
    let cancelled = false;
    let remoteHasMode = false;
    void Promise.allSettled([
      bridgeRequest<SearchSettings>('search.getSettings')
        .then((res) => {
          if (!cancelled && res && typeof res === 'object') {
            // 兼容老配置：若 defaultCategory 曾设为 'content'，自动迁移归一化为 searchMode='content' + defaultCategory='all'
            const normalizedCategory = res.defaultCategory === 'content' ? 'all' : (res.defaultCategory || 'all');
            const cleanExcluded = Array.isArray(res.customExcludedPaths)
              ? mergeExcludedPaths([], res.customExcludedPaths.filter((x): x is string => typeof x === 'string')).paths
              : [];
            setSettings((prev) => ({
              ...prev,
              ...res,
              defaultCategory: normalizedCategory,
              customExcludedPaths: cleanExcluded,
            }));
            safeStorageSetItem(CUSTOM_EXCLUDED_PATHS_STORAGE_KEY, JSON.stringify(cleanExcluded));
            if (res.defaultCategory === 'content' && !remoteHasMode) {
              setSearchMode('content');
              safeStorageSetItem('tools3000_search_default_mode', 'content');
            }
          }
        })
        .catch(() => undefined),
      bridgeRequest<PluginListItem[]>('plugins.getAll')
        .then((list) => {
          if (!cancelled && Array.isArray(list)) {
            const searchPlugin = list.find((item) => item.id === 'search');
            if (searchPlugin && typeof searchPlugin.enabled === 'boolean') {
              setSearchEnabled(searchPlugin.enabled);
            }
          }
        })
        .catch(() => undefined),
      bridgeRequest<{ value?: unknown }>('config.get', { key: '/search/uiPreferences' })
        .then((raw) => {
          if (cancelled || !raw || typeof raw !== 'object') return;
          const prefs = ('value' in raw && raw.value && typeof raw.value === 'object'
            ? raw.value
            : raw) as Record<string, unknown>;
          if (prefs.searchMode === 'name' || prefs.searchMode === 'both' || prefs.searchMode === 'content') {
            remoteHasMode = true;
            setSearchMode(prefs.searchMode);
            safeStorageSetItem('tools3000_search_default_mode', prefs.searchMode);
          }
        })
        .catch(() => undefined),
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll')
        .then((res) => {
          if (!cancelled) {
            setHotkeys(Array.isArray(res) ? res : []);
          }
        })
        .catch(() => undefined),
      bridgeRequest<ServiceStatus>('search.getServiceStatus')
        .then((res) => {
          if (!cancelled && res && typeof res === 'object') {
            setServiceStatus((prev) => ({ ...prev, ...res }));
          }
        })
        .catch(() => undefined),
    ]).then(() => {
      if (!cancelled) {
        setLoading(false);
      }
    });

    const onStorage = (e: StorageEvent) => {
      if (e.key === 'tools3000_search_default_mode' && (e.newValue === 'name' || e.newValue === 'both' || e.newValue === 'content')) {
        setSearchMode(e.newValue);
      } else if (e.key === 'tools3000_search_default_category' && e.newValue) {
        setSettings((prev) => ({ ...prev, defaultCategory: e.newValue === 'content' ? 'all' : e.newValue! }));
      } else if (e.key === CUSTOM_EXCLUDED_PATHS_STORAGE_KEY) {
        const synced = safeStorageGetJsonArray<string>(
          CUSTOM_EXCLUDED_PATHS_STORAGE_KEY,
          (v): v is string => typeof v === 'string' && v.trim().length > 0,
          [],
        );
        const clean = mergeExcludedPaths([], synced).paths;
        setSettings((prev) => ({ ...prev, customExcludedPaths: clean }));
      }
    };
    window.addEventListener('storage', onStorage);
    return () => {
      cancelled = true;
      window.removeEventListener('storage', onStorage);
    };
  }, []);

  useEffect(() => {
    let cancelled = false;
    void bridgeRequest<SearchSettings>('search.getSettings')
      .then((res) => {
        if (!cancelled && res && typeof res === 'object' && typeof res.pinyinEnabled === 'boolean') {
          setSettings((prev) => (prev.pinyinEnabled === res.pinyinEnabled ? prev : { ...prev, pinyinEnabled: res.pinyinEnabled }));
        }
      })
      .catch(() => undefined);
    return () => {
      cancelled = true;
    };
  }, [i18n.resolvedLanguage, i18n.language]);

  // 切换到「服务与索引」Tab 时按 DEMAND_START 原则静默刷新一次内存观测，绝不拉起未运行服务
  useEffect(() => {
    if (activeTab !== 'status') return;
    let cancelled = false;
    void bridgeRequest<ServiceStatus>('search.getServiceStatus')
      .then((res) => {
        if (!cancelled && res && typeof res === 'object') {
          setServiceStatus((prev) => ({ ...prev, ...res, running: res.running ?? res.available }));
        }
      })
      .catch(() => undefined);
    return () => {
      cancelled = true;
    };
  }, [activeTab]);

  const handleToggleSearchEnabled = async (nextEnabled: boolean) => {
    if (savingMaster) return;
    const prevEnabled = searchEnabled;
    setSearchEnabled(nextEnabled);
    setSavingMaster(true);
    try {
      const res = await bridgeRequest<{ success?: boolean; restartRequired?: boolean }>('plugins.setEnabled', {
        id: 'search',
        enabled: nextEnabled,
      });
      if (res && res.success === false) {
        setSearchEnabled(prevEnabled);
        toast.error(t('plugins.saveFailed', 'Failed to update plugin state'));
        return;
      }
      const latestPlugins = await bridgeRequest<PluginListItem[]>('plugins.getAll').catch(() => null);
      if (Array.isArray(latestPlugins)) {
        window.dispatchEvent(new CustomEvent('tools3000:plugins-changed', { detail: latestPlugins }));
      }
      const refreshedHotkeys = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll').catch(() => null);
      if (Array.isArray(refreshedHotkeys)) {
        setHotkeys(refreshedHotkeys);
      }
      if (res?.restartRequired) {
        toast.info(t('plugins.restartHint'));
      }
    } catch {
      setSearchEnabled(prevEnabled);
      toast.error(t('plugins.saveFailed', 'Failed to update plugin state'));
    } finally {
      setSavingMaster(false);
    }
  };

  const saveSearchMode = async (mode: SearchMode) => {
    setSearchMode(mode);
    safeStorageSetItem('tools3000_search_default_mode', mode);
    try {
      const raw = await bridgeRequest<{ value?: unknown }>('config.get', { key: '/search/uiPreferences' });
      const prevObj = raw && typeof raw === 'object' && 'value' in raw && raw.value && typeof raw.value === 'object'
        ? (raw.value as Record<string, unknown>)
        : (raw && typeof raw === 'object' && !('value' in raw) ? (raw as Record<string, unknown>) : {});
      await bridgeRequest('config.set', {
        key: '/search/uiPreferences',
        value: { ...prevObj, searchMode: mode },
      });
    } catch {
      void bridgeRequest('config.set', {
        key: '/search/uiPreferences',
        value: { searchMode: mode },
      }).catch(() => undefined);
    }
  };

  const saveSetting = async <K extends keyof SearchSettings>(key: K, value: SearchSettings[K]) => {
    setSettings((prev) => ({ ...prev, [key]: value }));
    if (key === 'defaultCategory') {
      safeStorageSetItem('tools3000_search_default_category', String(value));
    } else if (key === 'iconStyle' && value) {
      safeStorageSetItem('tools3000_search_icon_style', String(value));
    } else if (key === 'customExcludedPaths' && Array.isArray(value)) {
      safeStorageSetItem(CUSTOM_EXCLUDED_PATHS_STORAGE_KEY, JSON.stringify(value));
    }
    try {
      await bridgeRequest('search.saveSettings', { [key]: value });
    } catch {
      void bridgeRequest<SearchSettings>('search.getSettings')
        .then((res) => setSettings((prev) => ({ ...prev, ...res })))
        .catch(() => {});
    }
  };

  const handleHotkeyChange = async (newKey: string) => {
    try {
      const res = await bridgeRequest<{ success: boolean; error?: string; shortcut?: string }>('hotkey.rebind', {
        name: 'Toggle Search',
        hotkey: newKey,
      });
      if (res.success) {
        await saveSetting('hotkey', res.shortcut ?? newKey);
        const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
        if (Array.isArray(refreshed)) setHotkeys(refreshed);
      }
    } catch {
      // 错误已由 bridgeRequest 自动提示失败
    }
  };

  const checkService = async () => {
    if (checking) return;
    setChecking(true);
    try {
      const res = await bridgeRequest<ServiceStatus>('search.getServiceStatus');
      if (res && typeof res === 'object') {
        setServiceStatus((prev) => ({ ...prev, ...res, running: res.running ?? res.available }));
      }
      if (res?.available) {
        toast.success(t('searchPage.serviceOk', 'Index daemon connected via local named pipe'));
      } else {
        toast.info(t('searchPage.serviceStandbyToast', 'Engine is in on-demand standby; click Wake Engine Now to start immediately'));
      }
    } catch {
      toast.info(t('searchPage.serviceStandbyToast', 'Engine is in on-demand standby; click Wake Engine Now to start immediately'));
    } finally {
      setChecking(false);
    }
  };

  const wakeService = async () => {
    if (waking) return;
    setWaking(true);
    try {
      await bridgeRequest('search.windowShown').catch(() => undefined);
      const warmRes = await bridgeRequest<ServiceStatus>('search.warmup');
      const statusRes = await bridgeRequest<ServiceStatus>('search.getServiceStatus').catch(() => warmRes);
      const effective = statusRes && typeof statusRes === 'object' ? statusRes : warmRes;
      if (effective && typeof effective === 'object') {
        setServiceStatus((prev) => ({ ...prev, ...effective, running: effective.running ?? effective.available }));
      }
      if (effective?.available) {
        toast.success(t('searchPage.serviceWoken', 'Indexing engine woken and ready'));
      } else {
        toast.error(t('searchPage.serviceDown', 'Index service not responding, check administrator privileges'));
      }
    } catch {
      toast.error(t('searchPage.serviceDown', 'Index service not responding, check administrator privileges'));
    } finally {
      setWaking(false);
    }
  };

  const trimServiceMemory = async () => {
    if (trimming || stopping) return;
    setTrimming(true);
    try {
      const res = await bridgeRequest<{
        success?: boolean;
        running?: boolean;
        workingSetBytes?: number;
        privateBytes?: number;
        lastTrimmedAt?: number;
      }>('search.trimMemory');
      if (res && typeof res === 'object') {
        setServiceStatus((prev) => ({
          ...prev,
          available: Boolean(res.running ?? prev.available),
          running: Boolean(res.running ?? prev.running),
          workingSetBytes: typeof res.workingSetBytes === 'number' ? res.workingSetBytes : prev.workingSetBytes,
          privateBytes: typeof res.privateBytes === 'number' ? res.privateBytes : prev.privateBytes,
          lastTrimmedAt: typeof res.lastTrimmedAt === 'number' ? res.lastTrimmedAt : prev.lastTrimmedAt,
        }));
      }
      if (res?.success) {
        const wsText = formatBytes(res.workingSetBytes ?? 0);
        toast.success(
          t('searchPage.trimMemorySuccess', 'Search service memory trimmed (Working Set: {{size}})', { size: wsText }),
        );
      } else {
        toast.info(t('searchPage.memoryServiceOfflineToast', 'Search service is not running (0 MB)'));
      }
    } catch {
      toast.error(t('searchPage.trimMemoryFailed', 'Failed to trim search service memory'));
    } finally {
      setTrimming(false);
    }
  };

  const stopSearchService = async () => {
    if (stopping || trimming) return;
    setStopping(true);
    try {
      const res = await bridgeRequest<{
        success?: boolean;
        running?: boolean;
        snapshotSaved?: boolean;
      }>('search.stopService');
      const stillRunning = Boolean(res?.running);
      setServiceStatus((prev) => ({
        ...prev,
        available: stillRunning,
        running: stillRunning,
        workingSetBytes: stillRunning ? prev.workingSetBytes : 0,
        privateBytes: stillRunning ? prev.privateBytes : 0,
        idleSeconds: 0,
      }));
      if (!stillRunning) {
        toast.success(
          t('searchPage.stopServiceSuccess', 'Search service stopped cleanly (0 MB). Next search will wake and catch up USN automatically.'),
        );
      } else {
        toast.error(t('searchPage.stopServiceFailed', 'Search service is still stopping; click Check Now shortly'));
      }
    } catch {
      toast.error(t('searchPage.stopServiceFailed', 'Search service is still stopping; click Check Now shortly'));
    } finally {
      setStopping(false);
    }
  };

  const launchSearch = () => {
    void bridgeRequest('search.toggle').catch(console.error);
  };

  // 文件类型筛选与「搜索模式 (文件名/双搜/全文)」正交解耦：剔除 content，避免与「仅搜文件内容」重叠打架
  const categoryOptions = CATEGORY_DEFS
    .filter((def) => def.id !== 'content')
    .map((def) => ({
      value: def.id,
      label: t(def.labelKey, def.defaultLabel),
    }));

  const searchModeOptions: Array<{ value: SearchMode; label: string }> = [
    { value: 'name', label: t('search.modeFilenameOnly', 'Search File Name Only (Fast · Default)') },
    { value: 'both', label: t('search.modeHybrid', 'Filename & Content Hybrid') },
    { value: 'content', label: t('search.modeContentOnly', 'Content Only (Full-Text)') },
  ];

  const searchModeDesc =
    searchMode === 'both'
      ? t('search.modeHybridDesc', 'Search both filename and document contents simultaneously')
      : searchMode === 'content'
        ? t('search.modeContentOnlyDesc', 'Full-text search inside documents, spreadsheets, PDFs, CAD and code files')
        : t('search.modeFilenameOnlyDesc', 'Sub-millisecond response (<5ms); to search content, click the top "Content" tab or prefix with content:');

  const activeRankingProfile: SearchRankingProfile = settings.rankingProfile ?? 'balanced_frecency';
  const rankingProfileOptions: Array<{ value: SearchRankingProfile; label: string }> = [
    {
      value: 'balanced_frecency',
      label: t('searchPage.rankingBalanced', 'Balanced Smart (Recommended · Frecency)'),
    },
    {
      value: 'files_first',
      label: t('searchPage.rankingFilesFirst', 'Files First (Exact Alias Commands Only)'),
    },
    {
      value: 'launcher_first',
      label: t('searchPage.rankingLauncherFirst', 'Apps & Commands First (Launcher Mode)'),
    },
  ];
  const rankingProfileDesc =
    activeRankingProfile === 'files_first'
      ? t('searchPage.rankingFilesFirstDesc', 'Prioritize local files at the top; apps and commands only surface above files on exact alias matches (e.g. env) or explicit app:/cmd: prefixes.')
      : activeRankingProfile === 'launcher_first'
        ? t('searchPage.rankingLauncherFirstDesc', 'Place matched apps and Windows system commands in the top tier ahead of local files, ideal for keyboard-first launcher workflows.')
        : t('searchPage.rankingBalancedDesc', 'Pin high-confidence apps and exact commands at the top (capped at 3 so files are never buried) and automatically boost frequently used items via Frecency.');

  const activeMemoryMode: SearchMemoryMode = settings.memoryMode ?? 'idle_trim';
  const activeIdleTrimMinutes = settings.idleTrimMinutes ?? 2;
  const activeIdleStopMinutes = settings.idleStopMinutes ?? 30;
  const isServiceRunning = Boolean(serviceStatus.running || serviceStatus.available);
  const workingSetText = isServiceRunning
    ? formatBytes(serviceStatus.workingSetBytes ?? 0)
    : t('searchPage.memoryStoppedValue', '0 MB · Stopped');
  const privateBytesText = isServiceRunning
    ? formatBytes(serviceStatus.privateBytes ?? 0)
    : t('searchPage.memoryStoppedValue', '0 MB · Stopped');

  const idleTrimOptions = IDLE_TRIM_MINUTE_OPTIONS.map((mins) => ({
    value: String(mins),
    label: t('searchPage.minutesOption', '{{count}} min', { count: mins }),
  }));
  const idleStopOptions = IDLE_STOP_MINUTE_OPTIONS.map((mins) => ({
    value: String(mins),
    label: t('searchPage.minutesOption', '{{count}} min', { count: mins }),
  }));

  const memoryModeDesc =
    activeMemoryMode === 'full_resident'
      ? t('searchPage.memoryModeFullDesc', 'Keep full MFT tree and query caches resident in memory with no idle trimming or shutdown for constant sub-millisecond response.')
      : activeMemoryMode === 'idle_stop'
        ? t('searchPage.memoryModeStopDesc', 'After idling past the threshold, flush the latest NTFS USN cursor snapshot to disk and exit cleanly to 0 MB. Next search wakes on demand and catches up via USN in milliseconds.')
        : t('searchPage.memoryModeTrimDesc', 'Recommended: release query/content caches and compact working set after idling while keeping live NTFS USN monitoring active with zero missed file changes.');

  if (loading) {
    return <PageSkeleton cards={2} rowsPerCard={4} />;
  }

  const tabs: TabItem<SearchTabId>[] = [
    { id: 'settings', label: t('searchPage.tabSettings', 'Settings & Hotkey'), icon: <Search size={16} /> },
    { id: 'status', label: t('searchPage.tabStatus', 'Service & Index'), icon: <HardDrive size={16} /> },
    { id: 'external', label: t('searchPage.tabExternal', 'CLI & AI Agent (MCP)'), icon: <Terminal size={16} /> },
    { id: 'syntax', label: t('searchPage.tabSyntaxCombined', 'Search & Regex Syntax'), icon: <BookOpen size={16} /> },
  ];

  return (
    <div className="search-page">
      {/* ── 顶部 4 页签分流 ──────────────────────────────────────────── */}
      <Tabs
        tabs={tabs}
        activeId={activeTab}
        onChange={(id) => setActiveTab(id)}
        ariaLabel={t('search.title', 'Quick file search')}
      />

      {/* ── 1. 基础与热键设置 ────────────────────────────────────────── */}
      {activeTab === 'settings' && (
        <div className="search-page__content">
          <SettingGroup title={t('searchPage.hotkeyConfig', 'Hotkey & Trigger')} icon={<Sparkles size={18} />}>
            <Card>
              <SettingRow
                label={t('searchPage.enableSearchTitle', 'Enable Instant File Search')}
                description={t(
                  'searchPage.enableSearchDesc',
                  'Instant NTFS filename, Pinyin, and content search; disabling unloads search entry points and hotkeys',
                )}
              >
                <Toggle
                  id="search-master-enable-toggle"
                  checked={searchEnabled}
                  disabled={savingMaster}
                  onChange={(v) => void handleToggleSearchEnabled(v)}
                />
              </SettingRow>

              <DisabledCascade disabled={!searchEnabled}>
                <SettingRow
                  label={
                    <div className="search-page__row-label">
                      <span>{t('searchPage.toggleHotkey', 'Toggle Search Bar Shortcut')}</span>
                      <HotkeyStatusBadge entry={getHotkey('Toggle Search')} />
                    </div>
                  }
                  description={t('searchPage.toggleHotkeyDesc', 'Default Alt + Space, instantly summons the Fluent search overlay')}
                >
                  <HotkeyRecorder
                    id="search-toggle-hotkey"
                    value={settings.hotkey}
                    onChange={handleHotkeyChange}
                    placeholder="Alt+Space"
                  />
                </SettingRow>

                <SettingRow
                  label={t('searchPage.autoBypassFullscreen', 'Fullscreen Game/Video Bypass')}
                  description={t('searchPage.autoBypassFullscreenDesc', 'Automatically suppress search hotkey when foreground window is in exclusive fullscreen mode to prevent disrupting gaming or video playback')}
                >
                  <Toggle
                    id="search-auto-bypass-toggle"
                    checked={settings.autoBypassFullscreen ?? true}
                    onChange={(v) => void saveSetting('autoBypassFullscreen', v)}
                  />
                </SettingRow>

                <SettingRow
                  label={t('searchPage.quickTest', 'Test Search Bar')}
                  description={t('searchPage.quickTestDesc', 'Test the floating search overlay animation and latency')}
                >
                  <Button variant="primary" disabled={!searchEnabled} onClick={launchSearch}>
                    <Play size={14} aria-hidden="true" />
                    <span>{t('searchPage.openNow', 'Summon Search Bar')}</span>
                  </Button>
                </SettingRow>
              </DisabledCascade>
            </Card>
          </SettingGroup>

          <DisabledCascade disabled={!searchEnabled} className="search-page__content">
            <SettingGroup
              title={t('searchPage.groupMatchBehavior', 'Query Matching & Defaults')}
              icon={<FolderSearch size={18} />}
            >
              <Card>
                <SettingRow
                  label={t('search.defaultSearchMode', 'Default Search Mode')}
                  description={searchModeDesc}
                >
                  <Select
                    id="search-default-mode"
                    value={searchMode}
                    onChange={(v) => void saveSearchMode(v as SearchMode)}
                    options={searchModeOptions}
                  />
                </SettingRow>

                {/* 仅在非「纯全文模式」时展示文件类型筛选，并通过 SubSettingGroup 表达从属层级 */}
                {searchMode !== 'content' && (
                  <SubSettingGroup>
                    <SettingRow
                      label={t('searchPage.defaultCategoryTitle', 'Default Filter Category')}
                      description={t('searchPage.defaultCategoryDesc', 'Initial category pill activated when opening a blank search session')}
                    >
                      <Select
                        id="search-default-category"
                        value={settings.defaultCategory && settings.defaultCategory !== 'content' ? settings.defaultCategory : 'all'}
                        onChange={(v) => void saveSetting('defaultCategory', v)}
                        options={categoryOptions}
                      />
                    </SettingRow>
                  </SubSettingGroup>
                )}

                <SettingRow
                  label={t('searchPage.pinyinTitle', 'Smart Pinyin Search')}
                  description={t('searchPage.pinyinDesc', 'Supports Chinese Pinyin initials (e.g. wx for WeChat) and full pinyin')}
                >
                  <Toggle
                    id="search-pinyin-toggle"
                    checked={settings.pinyinEnabled}
                    onChange={(v) => void saveSetting('pinyinEnabled', v)}
                  />
                </SettingRow>

                <SettingRow
                  label={t('searchPage.matchPathTitle', 'Match Full Path by Default')}
                  description={t('searchPage.matchPathDesc', 'Search within file paths in addition to file names')}
                >
                  <Toggle
                    id="search-matchpath-toggle"
                    checked={settings.matchPath}
                    onChange={(v) => void saveSetting('matchPath', v)}
                  />
                </SettingRow>

                <SettingRow
                  label={t('searchPage.caseTitle', 'Case Sensitive')}
                  description={t('searchPage.caseDesc', 'Match exact case; can also be toggled with case: prefix in query')}
                >
                  <Toggle
                    id="search-case-toggle"
                    checked={settings.caseSensitive}
                    onChange={(v) => void saveSetting('caseSensitive', v)}
                  />
                </SettingRow>
              </Card>
            </SettingGroup>

            <SettingGroup
              title={t('searchPage.groupExclusions', 'Excluded Folders & Privacy Shield')}
              icon={<FolderX size={18} />}
            >
              <Card>
                <SearchExcludedPathsManager
                  paths={settings.customExcludedPaths ?? []}
                  onChange={(nextPaths) => void saveSetting('customExcludedPaths', nextPaths)}
                />
              </Card>
            </SettingGroup>

            <SettingGroup
              title={t('searchPage.groupMixAndDisplay', 'Cross-Source Ranking & Appearance')}
              icon={<Sliders size={18} />}
            >
              <Card>
                <SettingRow
                  label={t('searchPage.rankingProfileTitle', 'Result Priority Strategy')}
                  description={rankingProfileDesc}
                >
                  <Select
                    id="search-ranking-profile"
                    value={activeRankingProfile}
                    onChange={(v) => void saveSetting('rankingProfile', v as SearchRankingProfile)}
                    options={rankingProfileOptions}
                  />
                </SettingRow>

                <SettingRow
                  label={t('searchPage.appsAndCommandsTitle', 'Quick Launch Apps & Windows System Commands')}
                  description={t('searchPage.appsAndCommandsDesc', 'Mix installed Start Menu apps and 35+ built-in geek commands (such as env, services, devmgmt, hosts) into general search offline. Explicit app: and cmd: prefixes always work without waking the index service.')}
                >
                  <Toggle
                    id="search-apps-commands-toggle"
                    checked={settings.enableAppsAndCommands ?? true}
                    onChange={(v) => void saveSetting('enableAppsAndCommands', v)}
                  />
                </SettingRow>

                <SettingRow
                  label={t('searchPage.bookmarksTitle', 'Include Browser Bookmarks in General Search')}
                  description={t('searchPage.bookmarksDesc', 'Mix matching local Edge, Chrome, and Brave bookmarks into regular search results. Dedicated b: / bm: prefixes (while keeping b:\\ as drive B) and the Bookmarks category always remain available offline.')}
                >
                  <Toggle
                    id="search-bookmarks-toggle"
                    checked={settings.includeBookmarks ?? true}
                    onChange={(v) => void saveSetting('includeBookmarks', v)}
                  />
                </SettingRow>

                <SettingRow
                  label={t('searchPage.iconStyleTitle', 'Result Icon Style')}
                  description={t('searchPage.iconStyleDesc', 'Choose between Windows native associated file icons (familiar system look with 0 cognitive overhead) and minimalist vector icons')}
                >
                  <div className="uikit-segmented" role="radiogroup" aria-label={t('searchPage.iconStyleTitle', 'Result Icon Style')}>
                    <button
                      type="button"
                      role="radio"
                      aria-checked={(settings.iconStyle ?? 'native') === 'native'}
                      className={`uikit-segmented__btn ${(settings.iconStyle ?? 'native') === 'native' ? 'uikit-segmented__btn--active' : ''}`}
                      onClick={() => void saveSetting('iconStyle', 'native')}
                    >
                      {t('searchPage.iconStyleNative', 'Windows Native')}
                    </button>
                    <button
                      type="button"
                      role="radio"
                      aria-checked={settings.iconStyle === 'vector'}
                      className={`uikit-segmented__btn ${settings.iconStyle === 'vector' ? 'uikit-segmented__btn--active' : ''}`}
                      onClick={() => void saveSetting('iconStyle', 'vector')}
                    >
                      {t('searchPage.iconStyleVector', 'Vector Minimalist')}
                    </button>
                  </div>
                </SettingRow>

                <SettingRow
                  label={t('searchPage.liveRefreshTitle', 'Real-Time Result Updates for New & Changed Files')}
                  description={t('searchPage.liveRefreshDesc', 'Automatically refresh open search results in place when files are created, renamed, or deleted via NTFS USN monitoring. Zero background polling when the search window is hidden.')}
                >
                  <Toggle
                    id="search-live-refresh-toggle"
                    checked={settings.liveRefreshEnabled ?? true}
                    onChange={(v) => {
                      safeStorageSetItem('tools3000_search_live_refresh', String(v));
                      void saveSetting('liveRefreshEnabled', v);
                    }}
                  />
                </SettingRow>
              </Card>
            </SettingGroup>
          </DisabledCascade>
        </div>
      )}

      {/* ── 2. 服务与索引状态 ────────────────────────────────────────── */}
      {activeTab === 'status' && (
        <div className="search-page__content">
          <Card title={t('searchPage.serviceMonitor', 'NTFS MFT Engine Monitor')}>
            <div className="service-status-card">
              <div className="service-status-header">
                <div className={`status-indicator ${isServiceRunning ? 'status-indicator--online' : 'status-indicator--standby'}`}>
                  {isServiceRunning ? <CheckCircle2 size={24} aria-hidden="true" /> : <Sparkles size={24} aria-hidden="true" />}
                </div>
                <div className="status-info">
                  <h3 className="status-title">
                    {isServiceRunning
                      ? t('searchPage.serviceRunning', 'Indexing Engine Running (Local Named Pipe)')
                      : t('searchPage.serviceStandby', 'On-Demand Standby (Instant Wake on First Search)')}
                  </h3>
                  <p className="status-desc">
                    {isServiceRunning
                      ? t('searchPage.serviceRunningDesc', 'NTFS MFT tree and USN journal monitoring are mounted and ready')
                      : t('searchPage.serviceStandbyDesc', 'Follows zero-boot-overhead DEMAND_START policy; wakes automatically on first search or click Wake Engine Now.')}
                  </p>
                </div>
                <div className="service-status-actions">
                  {!isServiceRunning && (
                    <Button variant="primary" onClick={() => void wakeService()} disabled={waking || checking || stopping}>
                      {waking ? (
                        <RotateCw size={14} className="uikit-spin-icon" aria-hidden="true" />
                      ) : (
                        <Zap size={14} aria-hidden="true" />
                      )}
                      {t('searchPage.wakeNow', 'Wake Engine Now')}
                    </Button>
                  )}
                  <Button variant="ghost" onClick={() => void checkService()} disabled={checking || waking || trimming || stopping}>
                    <RotateCw size={14} className={checking ? 'uikit-spin-icon' : ''} aria-hidden="true" />
                    {t('searchPage.checkNow', 'Check Now')}
                  </Button>
                </div>
              </div>

              <div className="service-details-grid">
                <div className="detail-item">
                  <span className="detail-label">{t('searchPage.memoryWorkingSet', 'Working Set Memory')}</span>
                  <span className="detail-value detail-value--numeric">
                    {workingSetText}
                  </span>
                </div>
                <div className="detail-item">
                  <span className="detail-label">{t('searchPage.memoryPrivateBytes', 'Private Commit Memory')}</span>
                  <span className="detail-value detail-value--numeric">
                    {privateBytesText}
                  </span>
                </div>
                <div className="detail-item">
                  <span className="detail-label">{t('searchPage.indexTech', 'Indexing Architecture')}</span>
                  <span className="detail-value">{t('searchPage.indexTechDesc', 'NTFS MFT memory tree + USN Journal incremental monitoring')}</span>
                </div>
                <div className="detail-item">
                  <span className="detail-label">{t('searchPage.daemonMode', 'Daemon Mode')}</span>
                  <span className="detail-value">{t('searchPage.daemonModeDesc', 'Dual-mode adaptive (Windows Service / Standalone Windowless Daemon)')}</span>
                </div>
              </div>
            </div>
          </Card>

          <SettingGroup title={t('searchPage.memoryPolicyGroup', 'Search Service Memory Policy')} icon={<Cpu size={18} />}>
            <Card>
              <SettingRow
                label={t('searchPage.memoryModeTitle', 'Idle Memory Management Mode')}
                description={memoryModeDesc}
              >
                <div
                  className="uikit-segmented"
                  role="radiogroup"
                  aria-label={t('searchPage.memoryModeTitle', 'Idle Memory Management Mode')}
                >
                  <button
                    type="button"
                    role="radio"
                    aria-checked={activeMemoryMode === 'full_resident'}
                    className={`uikit-segmented__btn ${activeMemoryMode === 'full_resident' ? 'uikit-segmented__btn--active' : ''}`}
                    onClick={() => void saveSetting('memoryMode', 'full_resident')}
                  >
                    {t('searchPage.memoryModeFull', 'Full Resident')}
                  </button>
                  <button
                    type="button"
                    role="radio"
                    aria-checked={activeMemoryMode === 'idle_trim'}
                    className={`uikit-segmented__btn ${activeMemoryMode === 'idle_trim' ? 'uikit-segmented__btn--active' : ''}`}
                    onClick={() => void saveSetting('memoryMode', 'idle_trim')}
                  >
                    {t('searchPage.memoryModeTrim', 'Idle Trim (Recommended)')}
                  </button>
                  <button
                    type="button"
                    role="radio"
                    aria-checked={activeMemoryMode === 'idle_stop'}
                    className={`uikit-segmented__btn ${activeMemoryMode === 'idle_stop' ? 'uikit-segmented__btn--active' : ''}`}
                    onClick={() => void saveSetting('memoryMode', 'idle_stop')}
                  >
                    {t('searchPage.memoryModeStop', 'Idle Sleep (0 MB)')}
                  </button>
                </div>
              </SettingRow>

              {activeMemoryMode === 'idle_trim' && (
                <SubSettingGroup>
                  <SettingRow
                    label={t('searchPage.idleTrimMinutesTitle', 'Idle Trim Threshold')}
                    description={t('searchPage.idleTrimMinutesDesc', 'Automatically release caches and trim working set after this idle duration while keeping USN monitoring active')}
                  >
                    <Select
                      id="search-idle-trim-minutes"
                      value={String(activeIdleTrimMinutes)}
                      onChange={(v) => void saveSetting('idleTrimMinutes', Number(v))}
                      options={idleTrimOptions}
                    />
                  </SettingRow>
                </SubSettingGroup>
              )}

              {activeMemoryMode === 'idle_stop' && (
                <SubSettingGroup>
                  <SettingRow
                    label={t('searchPage.idleStopMinutesTitle', 'Idle Deep Sleep Threshold')}
                    description={t('searchPage.idleStopMinutesDesc', 'Automatically persist the latest USN snapshot and exit the search daemon to 0 MB after this idle duration')}
                  >
                    <Select
                      id="search-idle-stop-minutes"
                      value={String(activeIdleStopMinutes)}
                      onChange={(v) => void saveSetting('idleStopMinutes', Number(v))}
                      options={idleStopOptions}
                    />
                  </SettingRow>
                </SubSettingGroup>
              )}

              <SettingRow
                label={t('searchPage.manualMemoryControlTitle', 'Manual Memory Control')}
                description={t('searchPage.manualMemoryControlDesc', 'Release cached search buffers immediately or cleanly stop the background index service without losing any file updates')}
              >
                <div className="service-status-actions">
                  <Button
                    variant="secondary"
                    onClick={() => void trimServiceMemory()}
                    disabled={!isServiceRunning || trimming || stopping}
                  >
                    {trimming ? (
                      <RotateCw size={14} className="uikit-spin-icon" aria-hidden="true" />
                    ) : (
                      <Sliders size={14} aria-hidden="true" />
                    )}
                    {t('searchPage.trimMemoryNow', 'Release Memory Now')}
                  </Button>
                  <Button
                    variant="ghost"
                    onClick={() => void stopSearchService()}
                    disabled={!isServiceRunning || stopping || trimming}
                  >
                    {stopping ? (
                      <RotateCw size={14} className="uikit-spin-icon" aria-hidden="true" />
                    ) : (
                      <Power size={14} aria-hidden="true" />
                    )}
                    {t('searchPage.stopServiceNow', 'Stop Search Service')}
                  </Button>
                </div>
              </SettingRow>

              <div className="search-page__safety-note">
                <ShieldCheck size={16} aria-hidden="true" className="search-page__safety-icon" />
                <span>
                  {t(
                    'searchPage.usnSafetyNote',
                    'Whether using Idle Trim or Idle Sleep, Tools3000 relies on NTFS USN Journal incremental catch-up so newly created, renamed, or modified files are never missed.',
                  )}
                </span>
              </div>
            </Card>
          </SettingGroup>
        </div>
      )}

      {/* ── 3. 命令行 (CLI) 与 AI 智能体 (MCP) 接口 ──────────────────── */}
      {activeTab === 'external' && (
        <div className="search-page__content">
          <SearchExternalApiCard
            externalApiEnabled={settings.externalApiEnabled ?? false}
            externalApiExcludePaths={settings.externalApiExcludePaths ?? ''}
            externalApiAllowedRoots={settings.externalApiAllowedRoots ?? ''}
            onSaveSettings={async (patch) => {
              setSettings((prev) => ({ ...prev, ...patch }));
              try {
                await bridgeRequest('search.saveSettings', patch);
              } catch {
                void bridgeRequest<SearchSettings>('search.getSettings')
                  .then((res) => setSettings((prev) => ({ ...prev, ...res })))
                  .catch(() => {});
              }
            }}
          />
        </div>
      )}

      {/* ── 4. 搜索与正则语法手册（上下清晰分段） ────────────────────── */}
      {activeTab === 'syntax' && (
        <div className="search-page__content">
          <SettingGroup
            title={t('searchPage.syntaxSectionOperators', 'Operators & Filter Modifiers')}
            icon={<BookOpen size={18} />}
          >
            <Card title={t('searchPage.operatorsTitle', 'Operators & Combination')}>
              <div className="syntax-table-wrapper">
                <table className="syntax-table">
                  <thead>
                    <tr>
                      <th>{t('searchPage.thOpSyntax', 'Operator / Syntax')}</th>
                      <th>{t('searchPage.thDesc', 'Description')}</th>
                      <th>{t('searchPage.thExample', 'Example')}</th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr>
                      <td><CodeBadge>space</CodeBadge></td>
                      <td>{t('searchPage.opSpaceDesc', 'Logical AND, matches all keywords simultaneously')}</td>
                      <td><CodeBadge>report 2026</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>|</CodeBadge> / <CodeBadge>OR</CodeBadge></td>
                      <td>{t('searchPage.opOrDesc', 'Logical OR, matches any condition')}</td>
                      <td><CodeBadge>ext:jpg | ext:png</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>!</CodeBadge> / <CodeBadge>NOT</CodeBadge></td>
                      <td>{t('searchPage.opNotDesc', 'Logical NOT, excludes files containing specific word')}</td>
                      <td><CodeBadge>*.cpp !test</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>" "</CodeBadge></td>
                      <td>{t('searchPage.opPhraseDesc', 'Quoted phrase, strictly matches exact text with spaces')}</td>
                      <td><CodeBadge>"Program Files"</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>*</CodeBadge></td>
                      <td>{t('searchPage.opWildcardMulti', 'Wildcard, matches 0 or more characters')}</td>
                      <td><CodeBadge>*.pdf</CodeBadge>, <CodeBadge>report_*</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>?</CodeBadge></td>
                      <td>{t('searchPage.opWildcardSingle', 'Wildcard, matches 1 character')}</td>
                      <td><CodeBadge>img_??.png</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>\</CodeBadge> / <CodeBadge>/</CodeBadge></td>
                      <td>{t('searchPage.opSlashPathPinyin', 'Cross-folder path segment search (supports Pinyin initials, full Pinyin and wildcards)')}</td>
                      <td><CodeBadge>gzml\user</CodeBadge>, <CodeBadge>*gzml*\*user*</CodeBadge></td>
                    </tr>
                  </tbody>
                </table>
              </div>
            </Card>

            <Card title={t('searchPage.modifiersTitle', 'Filters & Modifiers')}>
              <div className="syntax-table-wrapper">
                <table className="syntax-table">
                  <thead>
                    <tr>
                      <th>{t('searchPage.thModifier', 'Prefix / Modifier')}</th>
                      <th>{t('searchPage.thDesc', 'Description')}</th>
                      <th>{t('searchPage.thExample', 'Example')}</th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr>
                      <td><CodeBadge>ext:&lt;ext_list&gt;</CodeBadge></td>
                      <td>{t('searchPage.modExtDesc', 'Filter by file extensions, comma or semicolon separated')}</td>
                      <td><CodeBadge>ext:jpg;png;webp</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>file:</CodeBadge></td>
                      <td>{t('searchPage.modFileDesc', 'Match files only (exclude folders)')}</td>
                      <td><CodeBadge>file: *.txt</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>folder:</CodeBadge> / <CodeBadge>dir:</CodeBadge></td>
                      <td>{t('searchPage.modFolderDesc', 'Match folders / directories only')}</td>
                      <td><CodeBadge>folder: project</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>path:&lt;path&gt;</CodeBadge></td>
                      <td>{t('searchPage.modPathDesc', 'Search within full absolute path')}</td>
                      <td><CodeBadge>path:windows\system32</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>parent:&lt;dir&gt;</CodeBadge> / <CodeBadge>p:</CodeBadge></td>
                      <td>{t('searchPage.modParentDesc', 'Match in direct parent directory name')}</td>
                      <td><CodeBadge>parent:tools3000</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>c:</CodeBadge> / <CodeBadge>d:</CodeBadge></td>
                      <td>{t('searchPage.modDriveDesc', 'Restrict search to specified drive')}</td>
                      <td><CodeBadge>d: *.zip</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>exact:&lt;name&gt;</CodeBadge></td>
                      <td>{t('searchPage.modExactDesc', 'Strict exact filename match')}</td>
                      <td><CodeBadge>exact:README.md</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>content:&lt;query&gt;</CodeBadge></td>
                      <td>{t('searchPage.modContentDesc', 'Full-text content search, supports code, Office documents, PSD/AI/CAD drawings')}</td>
                      <td><CodeBadge>content:SELECT</CodeBadge>, <CodeBadge>ext:docx content:contract</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>case:&lt;text&gt;</CodeBadge> / <CodeBadge>cs:</CodeBadge></td>
                      <td>{t('searchPage.modCaseDesc', 'Enforce case-sensitive matching')}</td>
                      <td><CodeBadge>case:Tools3000</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>pinyin:&lt;pinyin&gt;</CodeBadge> / <CodeBadge>py:</CodeBadge></td>
                      <td>{t('searchPage.modPinyinDesc', 'Explicit pinyin search (full spell or initials)')}</td>
                      <td><CodeBadge>py:wx</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>nopy:&lt;text&gt;</CodeBadge></td>
                      <td>{t('searchPage.modNopyDesc', 'Disable pinyin, match plain text only')}</td>
                      <td><CodeBadge>nopy:wx</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>b:&lt;kw&gt;</CodeBadge> / <CodeBadge>bm:&lt;kw&gt;</CodeBadge></td>
                      <td>{t('searchPage.modBookmarkDesc', 'Search local browser bookmarks offline without waking index service (b:\\ still targets drive B)')}</td>
                      <td><CodeBadge>b:github</CodeBadge>, <CodeBadge>bm:docs</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>app:&lt;name&gt;</CodeBadge></td>
                      <td>{t('searchPage.modAppDesc', 'Search installed Start Menu, App Paths, and portable apps offline')}</td>
                      <td><CodeBadge>app:code</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>cmd:&lt;alias&gt;</CodeBadge></td>
                      <td>{t('searchPage.modCmdDesc', 'Launch 35+ built-in Windows system commands offline (supports English and Pinyin aliases)')}</td>
                      <td><CodeBadge>cmd:env</CodeBadge>, <CodeBadge>cmd:services</CodeBadge></td>
                    </tr>
                  </tbody>
                </table>
              </div>
            </Card>
          </SettingGroup>

          <SettingGroup
            title={t('searchPage.syntaxSectionRegex', 'Regular Expression Quick Reference')}
            icon={<Code2 size={18} />}
          >
            <Card title={t('searchPage.regexTitle', 'Regular Expression Query (regex: or r:)')}>
              <p className="syntax-hint-p">
                {t('searchPage.regexHint', 'Use regex:<expr> or r:<expr> prefix to enable standard C++ ECMAScript regular expression search.')}
              </p>
              <div className="syntax-table-wrapper">
                <table className="syntax-table">
                  <thead>
                    <tr>
                      <th>{t('searchPage.thRegexSyntax', 'Regex Syntax')}</th>
                      <th>{t('searchPage.thRegexRule', 'Matching Rule')}</th>
                      <th>{t('searchPage.thRegexExample', 'Practical Example')}</th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr>
                      <td><CodeBadge>a|b</CodeBadge></td>
                      <td>{t('searchPage.regOr', 'Match expression a or b')}</td>
                      <td><CodeBadge>r:test|debug</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>.</CodeBadge></td>
                      <td>{t('searchPage.regAnyChar', 'Match any single character except newline')}</td>
                      <td><CodeBadge>r:a.c\.txt</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>[abc]</CodeBadge></td>
                      <td>{t('searchPage.regCharSet', 'Match any single character in character set')}</td>
                      <td><CodeBadge>r:log_[0-9]\.txt</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>[^abc]</CodeBadge></td>
                      <td>{t('searchPage.regNegSet', 'Exclude characters in character set')}</td>
                      <td><CodeBadge>r:file_[^0-9]\.dat</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>[a-z]</CodeBadge> / <CodeBadge>[0-9]</CodeBadge></td>
                      <td>{t('searchPage.regRange', 'Match characters within specified range')}</td>
                      <td><CodeBadge>{'r:^[a-z]{3}_[0-9]{4}'}</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>^</CodeBadge></td>
                      <td>{t('searchPage.regStartAnchor', 'Match start of filename (head anchor)')}</td>
                      <td><CodeBadge>r:^Tools3000.*\.exe$</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>$</CodeBadge></td>
                      <td>{t('searchPage.regEndAnchor', 'Match end of filename (tail anchor)')}</td>
                      <td><CodeBadge>r:\.min\.js$</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>*</CodeBadge> / <CodeBadge>+</CodeBadge> / <CodeBadge>?</CodeBadge></td>
                      <td>{t('searchPage.regQuantifiers', 'Match 0 or more / 1 or more / 0 or 1 time')}</td>
                      <td><CodeBadge>r:v\d+\.\d+</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>&#123;n&#125;</CodeBadge> / <CodeBadge>&#123;n,m&#125;</CodeBadge></td>
                      <td>{t('searchPage.regExactCount', 'Specify exact match count or range')}</td>
                      <td><CodeBadge>r:\d&#123;4&#125;-\d&#123;2&#125;-\d&#123;2&#125;</CodeBadge></td>
                    </tr>
                    <tr>
                      <td><CodeBadge>\</CodeBadge></td>
                      <td>{t('searchPage.regEscape', 'Escape special characters (e.g. \\. matches literal dot)')}</td>
                      <td><CodeBadge>r:archive\.(tar\.gz|zip)</CodeBadge></td>
                    </tr>
                  </tbody>
                </table>
              </div>
            </Card>
          </SettingGroup>
        </div>
      )}
    </div>
  );
};

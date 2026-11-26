/* ─────────────────────────────────────────────────────────────────────────────
 * App.tsx — 设置界面主布局
 *
 * 布局结构 (参考 Aitiy):
 *   ┌──────────┬──────────────────────────────┐
 *   │          │  页面标题 + 面包屑           │
 *   │  侧边栏  ├──────────────────────────────┤
 *   │ (导航)   │                              │
 *   │          │       页面内容区域            │
 *   │          │     (可滚动)                 │
 *   │          │                              │
 *   └──────────┴──────────────────────────────┘
 * ───────────────────────────────────────────────────────────────────────────── */

import { lazy, Suspense, useState, useEffect, useCallback, useRef, useMemo } from 'react';
import { Download, RotateCw, CheckCircle2, X } from 'lucide-react';
import { toast } from 'sonner';
import { TitleBar } from './components/TitleBar';
import { Sidebar, type NavId } from './components/Sidebar';
import type { PluginStatus } from './pages/PluginsPage';
import { OnboardingModal } from './components/OnboardingModal';
import { SavedToast } from './components/SavedToast';
import { WindowResizeHandles } from './components/WindowResizeHandles';
import { getPageMetadata, PAGE_DEFINITIONS } from './pages/registry';
import { isBuiltinPluginId, isPluginActiveInSet, resolveExtensionRoute } from './config/routes';
import { consumePendingDeepNavigation, emitDeepNavigation, type DeepNavigationPayload } from './config/settingsSearchIndex';
import { bridgeRequest, useBridgeEvent } from './hooks/useBridge';
import {
  applyLocalAppearance,
  isAccentColor,
  isThemePreference,
  persistAppearanceToBackend,
  readStoredAccent,
  readStoredTheme,
  useAppearance,
  type AppearancePayload,
  type Theme,
  type ThemePreference,
} from './hooks/useAppearance';
import { shouldShowOnboarding } from './onboardingGate';
import { AppToaster } from './components/AppToaster';
import { useTranslation } from 'react-i18next';
import './App.css';

const GesturePage = lazy(() => import('./pages/GesturePage').then((module) => ({ default: module.GesturePage })));
const CapturePage = lazy(() => import('./pages/CapturePage').then((module) => ({ default: module.CapturePage })));
const OcrPage = lazy(() => import('./pages/OcrPage').then((module) => ({ default: module.OcrPage })));
const GeneralPage = lazy(() => import('./pages/GeneralPage').then((module) => ({ default: module.GeneralPage })));
const AboutPage = lazy(() => import('./pages/AboutPage').then((module) => ({ default: module.AboutPage })));
const PerformancePage = lazy(() => import('./pages/PerformancePage').then((module) => ({ default: module.PerformancePage })));
const PluginsPage = lazy(() => import('./pages/PluginsPage').then((module) => ({ default: module.PluginsPage })));
const HistoryPage = lazy(() => import('./pages/HistoryPage'));
const KeyStatsPage = lazy(() => import('./pages/KeyStatsPage').then((module) => ({ default: module.KeyStatsPage })));
const HotCornerPage = lazy(() => import('./pages/HotCornerPage').then((module) => ({ default: module.HotCornerPage })));
const SearchPage = lazy(() => import('./pages/SearchPage').then((module) => ({ default: module.SearchPage })));
const ClipboardPage = lazy(() => import('./pages/ClipboardPage').then((module) => ({ default: module.ClipboardPage })));
const TextExpanderPage = lazy(() => import('./pages/TextExpanderPage').then((module) => ({ default: module.TextExpanderPage })));
const KeycastPage = lazy(() => import('./pages/KeycastPage').then((module) => ({ default: module.KeycastPage })));
const SpotlightPage = lazy(() => import('./pages/SpotlightPage').then((module) => ({ default: module.SpotlightPage })));
const DialogEnhancerPage = lazy(() => import('./pages/DialogEnhancerPage').then((module) => ({ default: module.DialogEnhancerPage })));
const RemoteBoostPage = lazy(() => import('./pages/RemoteBoostPage').then((module) => ({ default: module.RemoteBoostPage })));
const PicasaViewerPage = lazy(() => import('./pages/PicasaViewerPage').then((module) => ({ default: module.PicasaViewerPage })));
const FileLocksmithPage = lazy(() => import('./pages/FileLocksmithPage').then((module) => ({ default: module.FileLocksmithPage })));
const WindowManagerPage = lazy(() => import('./pages/WindowManagerPage').then((module) => ({ default: module.WindowManagerPage })));
const CapsLayerPage = lazy(() => import('./pages/CapsLayerPage').then((module) => ({ default: module.CapsLayerPage })));
const FeedbackModal = lazy(() => import('./components/FeedbackModal').then((module) => ({ default: module.FeedbackModal })));
const WhatsNewModal = lazy(() => import('./components/WhatsNewModal').then((module) => ({ default: module.WhatsNewModal })));

interface AppChangelogState {
  currentVersion: string;
  previousVersion: string;
  shouldShow: boolean;
  showChangelogOnUpdate: boolean;
}

interface AppUpdateState {
  hasUpdate: boolean;
  badgeVisible: boolean;
  dismissed: boolean;
  isPortable: boolean;
  status: string;
  latestVersion: string;
  progress: number;
  readyToInstall: boolean;
}

const DEFAULT_APP_UPDATE_STATE: AppUpdateState = {
  hasUpdate: false,
  badgeVisible: false,
  dismissed: false,
  isPortable: false,
  status: 'idle',
  latestVersion: '',
  progress: 0,
  readyToInstall: false,
};

function normalizeAppUpdateState(raw: unknown): AppUpdateState | null {
  if (!raw || typeof raw !== 'object') return null;
  const st = raw as {
    hasUpdate?: boolean;
    badgeVisible?: boolean;
    dismissed?: boolean;
    isPortable?: boolean;
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
    isPortable: Boolean(st.isPortable),
    status: st.status ?? 'idle',
    latestVersion: st.latestVersion ?? '',
    progress: typeof st.progress === 'number' ? st.progress : 0,
    readyToInstall: Boolean(st.readyToInstall),
  };
}

function resolveNavFromHash(rawHash: string): NavId | null {
  const cleaned = rawHash.replace(/^#/, '').replace(/^\/+/, '').split(/[?#]/)[0].trim();
  if (!cleaned) return null;
  const match = PAGE_DEFINITIONS.find((p) => p.id === cleaned);
  return match ? match.id : null;
}

const getInitialActivePluginIds = (): string[] | null => {
  try {
    const raw = localStorage.getItem('tools3000:active-plugins');
    if (raw) {
      const arr = JSON.parse(raw);
      if (Array.isArray(arr)) {
        return arr;
      }
    }
  } catch {
    // 忽略本地存储解析异常
  }
  return null;
};

function App() {
  const { t } = useTranslation();
  const [isElevated, setIsElevated] = useState(false);
  const [activePluginIds, setActivePluginIds] = useState<string[] | null>(() => getInitialActivePluginIds());
  const [activeNav, setActiveNav] = useState<NavId>(() => {
    try {
      const fromHash = resolveNavFromHash(window.location.hash);
      if (fromHash) {
        return fromHash;
      }
      const saved = localStorage.getItem('tools3000:last-nav');
      if (saved) {
        const cached = getInitialActivePluginIds();
        if (cached) {
          const cachedSet = new Set(cached);
          const pageDef = PAGE_DEFINITIONS.find((p) => p.id === saved);
          if (pageDef?.requiresPlugin) {
            if (!isPluginActiveInSet(cachedSet, pageDef.requiresPlugin)) return 'general';
          }
        }
        return saved as NavId;
      }
    } catch {
      // 忽略本地存储访问异常
    }
    return 'general';
  });
  const [updateState, setUpdateState] = useState<AppUpdateState>(DEFAULT_APP_UPDATE_STATE);
  const [installingUpdate, setInstallingUpdate] = useState(false);
  const [dismissingUpdate, setDismissingUpdate] = useState(false);
  const getSystemTheme = (): Theme =>
    window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';

  const [systemTheme, setSystemTheme] = useState<Theme>(getSystemTheme());
  const [themePreference, setThemePreference] = useState<ThemePreference>(() => readStoredTheme('system'));
  const [accent, setAccent] = useState<string>(() => readStoredAccent('blue'));
  const theme: Theme = themePreference === 'system' ? systemTheme : themePreference;
  const [showOnboarding, setShowOnboarding] = useState(false);
  const [showFeedbackModal, setShowFeedbackModal] = useState(false);
  const [changelogOpen, setChangelogOpen] = useState(false);
  const [changelogMode, setChangelogMode] = useState<'upgrade' | 'manual'>('manual');
  const [changelogState, setChangelogState] = useState<AppChangelogState>({
    currentVersion: __TOOLS3000_VERSION__,
    previousVersion: '',
    shouldShow: false,
    showChangelogOnUpdate: true,
  });
  const [plugins, setPlugins] = useState<PluginStatus[]>([]);
  const [retainedDisabledNav, setRetainedDisabledNav] = useState<NavId | null>(null);
  const activeNavRef = useRef<NavId>(activeNav);
  const picasaWasActiveRef = useRef<boolean>(false);
  const locksmithWasActiveRef = useRef<boolean>(false);
  const textExpanderWasActiveRef = useRef<boolean>(false);
  const windowManagerWasActiveRef = useRef<boolean>(false);
  const capsLayerWasActiveRef = useRef<boolean>(false);
  const pageTitleRef = useRef<HTMLHeadingElement>(null);
  const contentContainerRef = useRef<HTMLDivElement>(null);

  const handleNavSelect = useCallback((nav: NavId) => {
    activeNavRef.current = nav;
    setActiveNav(nav);
    setRetainedDisabledNav((prev) => (prev === nav ? prev : null));
    try {
      localStorage.setItem('tools3000:last-nav', nav);
    } catch {
      // 忽略本地存储保存异常
    }
  }, []);

  // 响应宿主 SettingsWindow::navigateTo("/about") 驱动的 Hash 路由直达，并在消费后清理 hash 保障二次点击仍可触发
  useEffect(() => {
    const consumeHashRoute = () => {
      const target = resolveNavFromHash(window.location.hash);
      if (!target) return;
      handleNavSelect(target);
      try {
        window.history.replaceState(null, '', `${window.location.pathname}${window.location.search}`);
      } catch {
        // 忽略受限历史栈异常
      }
    };
    consumeHashRoute();
    window.addEventListener('hashchange', consumeHashRoute);
    return () => window.removeEventListener('hashchange', consumeHashRoute);
  }, [handleNavSelect]);

  useBridgeEvent('settings:navigate', (raw: unknown) => {
    const obj = (raw && typeof raw === 'object')
      ? (raw as { page?: unknown; navId?: unknown; subTab?: unknown; tabId?: unknown })
      : null;
    const page = typeof raw === 'string'
      ? raw
      : (typeof obj?.page === 'string'
        ? obj.page
        : (typeof obj?.navId === 'string' ? obj.navId : ''));
    const target = resolveNavFromHash(page);
    if (target) {
      handleNavSelect(target);
      const subTab = typeof obj?.subTab === 'string'
        ? obj.subTab
        : (typeof obj?.tabId === 'string' ? obj.tabId : '');
      if (subTab) {
        emitDeepNavigation({ navId: target, tabId: subTab });
      }
    }
  });

  useEffect(() => {
    void bridgeRequest<unknown>('app.getUpdateState', undefined, { silent: true })
      .then((st) => {
        const normalized = normalizeAppUpdateState(st);
        if (normalized) {
          setUpdateState(normalized);
        }
      })
      .catch(() => {});
  }, []);

  useBridgeEvent('update.state', (raw: unknown) => {
    const normalized = normalizeAppUpdateState(raw);
    if (normalized) {
      setUpdateState(normalized);
    }
  });

  const handleStripDownloadOrInstall = useCallback(async () => {
    if (updateState.status === 'downloading' || installingUpdate) return;
    if (updateState.readyToInstall) {
      setInstallingUpdate(true);
      try {
        const res = await bridgeRequest<{ success?: boolean; cancelled?: boolean; error?: string }>('app.installUpdate');
        if (res?.cancelled || res?.error === 'cancelled') {
          setInstallingUpdate(false);
          return;
        }
        if (!res?.success) {
          setInstallingUpdate(false);
          toast.error(t('about.installUpdateFailed', 'Failed to launch update installer'));
          return;
        }
      } catch {
        setInstallingUpdate(false);
        toast.error(t('about.installUpdateFailed', 'Failed to launch update installer'));
      }
      return;
    }
    try {
      const res = await bridgeRequest<unknown>('app.downloadUpdate');
      const normalized = normalizeAppUpdateState(res);
      if (normalized) {
        setUpdateState(normalized);
      }
    } catch {
      toast.error(t('about.updateFailed'));
    }
  }, [installingUpdate, t, updateState.readyToInstall, updateState.status]);

  const handleStripDismissUpdate = useCallback(async () => {
    if (dismissingUpdate) return;
    setDismissingUpdate(true);
    setUpdateState((prev) => ({ ...prev, badgeVisible: false, dismissed: true }));
    try {
      await bridgeRequest('app.dismissUpdate');
    } catch {
      // 若宿主尚未接通 IPC 或短暂失败，保留本地关闭体验并在下次推送时自动对齐
    } finally {
      setDismissingUpdate(false);
    }
  }, [dismissingUpdate]);

  const applyPluginsSnapshot = useCallback((items: PluginStatus[]) => {
    const safeItems = Array.isArray(items) ? items : [];
    setPlugins(safeItems);
    const activeList = safeItems
      .filter((plugin) => plugin.enabled && plugin.active)
      .map((p) => p.id);
    const picasaNowActive = activeList.includes('picasa_viewer');
    const locksmithNowActive = activeList.includes('file_locksmith');
    const textExpanderNowActive = activeList.includes('text_expander');
    const windowManagerNowActive = activeList.includes('window_manager') || activeList.includes('windowmanager');
    const capsLayerNowActive = activeList.includes('caps_layer') || activeList.includes('capslayer');
    if (!picasaNowActive && picasaWasActiveRef.current && activeNavRef.current === 'picasa_viewer') {
      // 用户在 Picasa 图片查看器页面内就地关闭总开关时，保留当前页供查看停用态或直接重开
      setRetainedDisabledNav('picasa_viewer');
    } else if (picasaNowActive) {
      setRetainedDisabledNav((prev) => (prev === 'picasa_viewer' ? null : prev));
    }
    if (!locksmithNowActive && locksmithWasActiveRef.current && activeNavRef.current === 'file_locksmith') {
      // 用户在文件占用解锁页面内就地关闭右键菜单时，保留当前页供直接使用页内检测或重新开启
      setRetainedDisabledNav('file_locksmith');
    } else if (locksmithNowActive) {
      setRetainedDisabledNav((prev) => (prev === 'file_locksmith' ? null : prev));
    }
    if (!textExpanderNowActive && textExpanderWasActiveRef.current && activeNavRef.current === 'text_expander') {
      // 用户在文本扩展器页面内就地关闭总开关时，保留当前页供继续编辑规则或随时重开
      setRetainedDisabledNav('text_expander');
    } else if (textExpanderNowActive) {
      setRetainedDisabledNav((prev) => (prev === 'text_expander' ? null : prev));
    }
    if (!windowManagerNowActive && windowManagerWasActiveRef.current && activeNavRef.current === 'window_manager') {
      // 用户在窗口管理页面内就地关闭总开关时，保留当前页供继续编排布局或随时重开
      setRetainedDisabledNav('window_manager');
    } else if (windowManagerNowActive) {
      setRetainedDisabledNav((prev) => (prev === 'window_manager' ? null : prev));
    }
    if (!capsLayerNowActive && capsLayerWasActiveRef.current && activeNavRef.current === 'caps_layer') {
      // 用户在 Caps 引导改键页面内就地关闭总开关时，保留当前页供继续编辑键位或随时重开
      setRetainedDisabledNav('caps_layer');
    } else if (capsLayerNowActive) {
      setRetainedDisabledNav((prev) => (prev === 'caps_layer' ? null : prev));
    }
    picasaWasActiveRef.current = picasaNowActive;
    locksmithWasActiveRef.current = locksmithNowActive;
    textExpanderWasActiveRef.current = textExpanderNowActive;
    windowManagerWasActiveRef.current = windowManagerNowActive;
    capsLayerWasActiveRef.current = capsLayerNowActive;
    setActivePluginIds(activeList);
    try {
      localStorage.setItem('tools3000:active-plugins', JSON.stringify(activeList));
    } catch {
      // 忽略本地存储写入异常
    }
  }, []);

  useEffect(() => {
    void bridgeRequest<PluginStatus[]>('plugins.getAll').then(applyPluginsSnapshot).catch(console.error);
    const handleChange = (event: Event) => applyPluginsSnapshot((event as CustomEvent<PluginStatus[]>).detail);
    window.addEventListener('tools3000:plugins-changed', handleChange);
    return () => window.removeEventListener('tools3000:plugins-changed', handleChange);
  }, [applyPluginsSnapshot]);

  useBridgeEvent('plugins:changed', () => {
    void bridgeRequest<PluginStatus[]>('plugins.getAll').then(applyPluginsSnapshot).catch(console.error);
  });

  const activePlugins = useMemo(() => {
    if (plugins.length > 0) {
      return new Set(
        plugins
          .filter((plugin) => plugin.enabled && plugin.active)
          .flatMap((plugin) => {
            if (plugin.id === 'dialogenhancer' || plugin.id === 'dialog_enhancer') {
              return ['dialogenhancer', 'dialog_enhancer'];
            }
            if (plugin.id === 'window_manager' || plugin.id === 'windowmanager') {
              return ['window_manager', 'windowmanager'];
            }
            if (plugin.id === 'caps_layer' || plugin.id === 'capslayer') {
              return ['caps_layer', 'capslayer'];
            }
            if (plugin.id === 'quick_look' || plugin.id === 'quicklook') {
              return ['quick_look', 'quicklook'];
            }
            return [plugin.id];
          })
      );
    }
    if (activePluginIds) {
      const s = new Set(activePluginIds);
      if (s.has('dialogenhancer') || s.has('dialog_enhancer')) {
        s.add('dialogenhancer');
        s.add('dialog_enhancer');
      }
      return s;
    }
    // 默认全开（首启初态）
    return new Set([
      'gesture',
      'capture',
      'search',
      'clipboard',
      'text_expander',
      'textexpander',
      'file_locksmith',
      'keycast',
      'spotlight',
      'dialogenhancer',
      'dialog_enhancer',
      'remote_boost',
    ]);
  }, [plugins, activePluginIds]);

  const handleAppearanceHydrated = useCallback((settings: AppearancePayload) => {
    if (typeof settings.elevated === 'boolean') {
      setIsElevated(settings.elevated);
    }
    if (isThemePreference(settings.theme)) {
      setThemePreference(settings.theme);
    }
    if (isAccentColor(settings.accentColor)) {
      setAccent(settings.accentColor);
    }
  }, []);

  useAppearance(handleAppearanceHydrated);

  useBridgeEvent('appearance:changed', (data: unknown) => {
    if (!data || typeof data !== 'object') return;
    const payload = data as { theme?: unknown; accentColor?: unknown };
    if (isThemePreference(payload.theme)) {
      setThemePreference(payload.theme);
    }
    if (isAccentColor(payload.accentColor)) {
      setAccent(payload.accentColor);
    }
  });

  // 检查是否需要显示首次引导或升级后离线更新日志（两者严格互斥，绝不重叠弹窗）
  useEffect(() => {
    let cancelled = false;
    Promise.allSettled([
      bridgeRequest<boolean | null>('config.get', { key: '/app/onboardingCompleted' }),
      bridgeRequest<boolean | null>('config.get', { key: '/general/showOnboarding' }),
      bridgeRequest<Partial<AppChangelogState>>('app.getChangelogState', undefined, { silent: true }),
    ]).then(([completedRes, showRes, changelogRes]) => {
      if (cancelled) return;
      const completed = completedRes.status === 'fulfilled' ? completedRes.value : false;
      const explicitShow = showRes.status === 'fulfilled' ? showRes.value : false;
      const openOnboarding = shouldShowOnboarding({ completed, explicitShow });
      if (openOnboarding) {
        setShowOnboarding(true);
      }
      if (changelogRes.status === 'fulfilled' && changelogRes.value && typeof changelogRes.value === 'object') {
        const st = changelogRes.value;
        const normalized: AppChangelogState = {
          currentVersion: typeof st.currentVersion === 'string' && st.currentVersion ? st.currentVersion : __TOOLS3000_VERSION__,
          previousVersion: typeof st.previousVersion === 'string' ? st.previousVersion : '',
          shouldShow: Boolean(st.shouldShow),
          showChangelogOnUpdate: typeof st.showChangelogOnUpdate === 'boolean' ? st.showChangelogOnUpdate : true,
        };
        setChangelogState(normalized);
        if (!openOnboarding && normalized.shouldShow) {
          setChangelogMode('upgrade');
          setChangelogOpen(true);
        }
      }
    }).catch(() => {
      // 容错异常时不强行阻断主界面
    });
    return () => { cancelled = true; };
  }, []);

  const handleOpenManualChangelog = useCallback(() => {
    setChangelogMode('manual');
    setChangelogOpen(true);
  }, []);

  const handleCloseChangelog = useCallback(() => {
    setChangelogOpen(false);
    if (changelogMode === 'upgrade') {
      setChangelogState((prev) => ({ ...prev, shouldShow: false }));
      void bridgeRequest('app.markChangelogSeen', undefined, { silent: true }).catch(() => undefined);
    }
  }, [changelogMode]);

  const handleOnboardingComplete = useCallback(() => {
    setShowOnboarding(false);
    bridgeRequest('config.set', { key: '/app/onboardingCompleted', value: true }).catch(console.error);
    bridgeRequest('config.set', { key: '/general/showOnboarding', value: false }).catch(console.error);
    bridgeRequest('general.updateSettings', { showOnboarding: false }, { silent: true }).catch(console.error);
  }, []);

  // 监听系统主题变化；只在“跟随系统”时影响 Toaster 等组件状态，DOM 同步由 useAppearance 统一负责。
  useEffect(() => {
    const mediaQuery = window.matchMedia('(prefers-color-scheme: dark)');
    const handleChange = (e: MediaQueryListEvent) => {
      setSystemTheme(e.matches ? 'dark' : 'light');
    };
    mediaQuery.addEventListener('change', handleChange);
    return () => mediaQuery.removeEventListener('change', handleChange);
  }, []);

  useEffect(() => {
    if (showOnboarding) return;
    const frame = requestAnimationFrame(() => pageTitleRef.current?.focus());
    return () => cancelAnimationFrame(frame);
  }, [activeNav, showOnboarding]);

  useEffect(() => {
    let highlightTimer: ReturnType<typeof setTimeout> | null = null;
    let pollTimer: ReturnType<typeof setTimeout> | null = null;

    const locateTargetInContent = (targetText: string, attempt = 0) => {
      const container = contentContainerRef.current;
      const needle = targetText.trim().toLowerCase();
      if (!container || !needle) return;
      const candidates = Array.from(
        container.querySelectorAll<HTMLElement>('.uikit-setting-row, .uikit-setting-group, .uikit-card'),
      );
      const matched = candidates.find((el) => (el.textContent || '').toLowerCase().includes(needle));
      if (matched) {
        matched.scrollIntoView?.({ behavior: 'smooth', block: 'center' });
        matched.classList.add('app__deep-highlight');
        highlightTimer = setTimeout(() => {
          matched.classList.remove('app__deep-highlight');
        }, 1600);
      } else if (attempt < 6) {
        pollTimer = setTimeout(() => locateTargetInContent(targetText, attempt + 1), 80);
      }
    };

    const pending = activeNav !== 'search' ? consumePendingDeepNavigation(activeNav) : null;
    if (pending?.targetText) {
      pollTimer = setTimeout(() => locateTargetInContent(pending.targetText!), 60);
    }

    const onDeepNav = (e: Event) => {
      const detail = (e as CustomEvent<DeepNavigationPayload>).detail;
      if (detail?.targetText && detail.navId === activeNav) {
        pollTimer = setTimeout(() => locateTargetInContent(detail.targetText!), 60);
      }
    };
    const onOpenFeedback = () => setShowFeedbackModal(true);
    const onOpenChangelog = () => handleOpenManualChangelog();
    const onChangelogPrefChanged = (e: Event) => {
      const enabled = (e as CustomEvent<boolean>).detail;
      if (typeof enabled === 'boolean') {
        setChangelogState((prev) => ({ ...prev, showChangelogOnUpdate: enabled }));
      }
    };
    window.addEventListener('tools3000:navigate-deep', onDeepNav);
    window.addEventListener('tools3000:open-feedback', onOpenFeedback);
    window.addEventListener('tools3000:open-changelog', onOpenChangelog);
    window.addEventListener('tools3000:changelog-pref-changed', onChangelogPrefChanged);
    return () => {
      window.removeEventListener('tools3000:navigate-deep', onDeepNav);
      window.removeEventListener('tools3000:open-feedback', onOpenFeedback);
      window.removeEventListener('tools3000:open-changelog', onOpenChangelog);
      window.removeEventListener('tools3000:changelog-pref-changed', onChangelogPrefChanged);
      if (highlightTimer) clearTimeout(highlightTimer);
      if (pollTimer) clearTimeout(pollTimer);
    };
  }, [activeNav, handleOpenManualChangelog]);

  useEffect(() => {
    const handlePreference = (event: Event) => {
      const preference = (event as CustomEvent<ThemePreference>).detail;
      if (preference === 'system' || preference === 'dark' || preference === 'light') {
        setThemePreference(preference);
      }
    };
    const handleAccent = (event: Event) => {
      const newAccent = (event as CustomEvent<string>).detail;
      if (newAccent) {
        setAccent(newAccent);
      }
    };
    const handleStorage = (event: StorageEvent) => {
      if (event.key === 'tools3000:theme' && (event.newValue === 'system' || event.newValue === 'dark' || event.newValue === 'light')) {
        setThemePreference(event.newValue);
      }
      if (event.key === 'tools3000:accent-color' && event.newValue) {
        setAccent(event.newValue);
      }
    };
    window.addEventListener('tools3000:theme-changed', handlePreference);
    window.addEventListener('tools3000:accent-changed', handleAccent);
    window.addEventListener('storage', handleStorage);
    return () => {
      window.removeEventListener('tools3000:theme-changed', handlePreference);
      window.removeEventListener('tools3000:accent-changed', handleAccent);
      window.removeEventListener('storage', handleStorage);
    };
  }, []);

  const installedExtensions = useMemo(() => {
    // 单一事实源：仅真正标记为外部扩展且不在内置模块集合中的插件，才自动挂载动态扩展侧边栏入口，
    // 彻底杜绝无独立页面的内置开关模块（如 quick_look）被误判为外部扩展并坠入 GeneralPage。
    return plugins.filter(
      (p) => p.isExtension !== false && !isBuiltinPluginId(p.id) && p.enabled && p.active,
    );
  }, [plugins]);

  const installedExtensionIds = useMemo(
    () => installedExtensions.map((p) => p.id),
    [installedExtensions],
  );

  const isNavActive = useCallback((navId: NavId): boolean => {
    const pageDef = PAGE_DEFINITIONS.find((p) => p.id === navId);
    if (pageDef) {
      if (pageDef.requiresPlugin) {
        return isPluginActiveInSet(activePlugins, pageDef.requiresPlugin);
      }
      if (pageDef.category === 'extension') {
        if (plugins.length === 0 && activePluginIds) {
          return activePluginIds.includes(navId);
        }
        return installedExtensionIds.includes(navId);
      }
      return true;
    }
    if (plugins.length === 0 && activePluginIds) {
      return activePluginIds.includes(navId);
    }
    return installedExtensionIds.includes(navId);
  }, [activePlugins, activePluginIds, installedExtensionIds, plugins.length]);

  const currentNavActive = isNavActive(activeNav);

  // 未启用页面（且非 Picasa 就地停用保留态）优雅回退至通用设置
  useEffect(() => {
    if (!currentNavActive && retainedDisabledNav !== activeNav) {
      const timer = setTimeout(() => {
        handleNavSelect('general');
      }, 0);
      return () => clearTimeout(timer);
    }
  }, [activeNav, currentNavActive, handleNavSelect, retainedDisabledNav]);

  // 渲染当前页面：未启用页面的组件绝不挂载到 DOM 中（就地停用保留态除外），以阻断资源预载并释放内存
  const renderPage = () => {
    if (!currentNavActive && retainedDisabledNav !== activeNav) {
      return <GeneralPage onNavigate={handleNavSelect} onOpenOnboarding={() => setShowOnboarding(true)} />;
    }
    switch (activeNav) {
      case 'stats':           return <KeyStatsPage />;
      case 'performance':     return <PerformancePage onNavigate={handleNavSelect} />;
      case 'gesture':         return <GesturePage />;
      case 'hotcorner':       return <HotCornerPage />;
      case 'capture':         return <CapturePage />;
      case 'ocr':             return <OcrPage />;
      case 'history':         return <HistoryPage />;
      case 'search':          return <SearchPage />;
      case 'clipboard':       return <ClipboardPage />;
      case 'text_expander':   return <TextExpanderPage />;
      case 'window_manager':  return <WindowManagerPage />;
      case 'caps_layer':      return <CapsLayerPage />;
      case 'keycast':         return <KeycastPage />;
      case 'spotlight':       return <SpotlightPage />;
      case 'dialog_enhancer': return <DialogEnhancerPage />;
      case 'file_locksmith':  return <FileLocksmithPage />;
      case 'remote_boost':    return <RemoteBoostPage />;
      case 'picasa_viewer':   return <PicasaViewerPage />;
      case 'plugins':         return <PluginsPage initialPlugins={plugins} onNavigate={handleNavSelect} />;
      case 'general':         return <GeneralPage onNavigate={handleNavSelect} onOpenOnboarding={() => setShowOnboarding(true)} />;
      case 'about':           return <AboutPage onNavigate={handleNavSelect} onOpenChangelog={handleOpenManualChangelog} />;
      default:
        return <GeneralPage onNavigate={handleNavSelect} onOpenOnboarding={() => setShowOnboarding(true)} />;
    }
  };

  const activePluginMeta = plugins.find((p) => p.id === activeNav);
  const pageMeta = getPageMetadata(activeNav);
  const activeRoute = resolveExtensionRoute(activeNav, activePluginMeta?.name);
  const ActiveHeaderIcon = activeRoute.icon;

  return (
    <div className="app">
      <TitleBar
        isElevated={isElevated}
        themePreference={themePreference}
        onSelectThemePreference={(pref) => {
          setThemePreference(pref);
          applyLocalAppearance({ theme: pref });
          persistAppearanceToBackend({ theme: pref, accentColor: accent }).catch(console.error);
        }}
        accent={accent}
        onSelectAccent={(newAccent) => {
          setAccent(newAccent);
          applyLocalAppearance({ accentColor: newAccent });
          persistAppearanceToBackend({ theme: themePreference, accentColor: newAccent }).catch(console.error);
        }}
      />
      <div className="app__body">
        <Sidebar
          activeNav={activeNav}
          onNavigate={handleNavSelect}
          onOpenFeedback={() => setShowFeedbackModal(true)}
          activePlugins={activePlugins}
          installedExtensionIds={installedExtensionIds}
          installedExtensions={installedExtensions}
        />
        <main className="app__main" aria-labelledby="app-page-title">
          <AppToaster theme={theme} surface="settings" />

          {/* ── 顶部更新轻提示条（红点全链路语义承接：进入设置即刻看清更新版本并可一键操作/忽略） ── */}
          {updateState.badgeVisible && activeNav !== 'about' && (
            <div
              className={`app-update-strip${updateState.readyToInstall ? ' app-update-strip--ready' : ''}`}
              role="status"
              aria-live="polite"
            >
              <div className="app-update-strip__left">
                <span className="app-update-strip__dot" aria-hidden="true" />
                {updateState.status === 'downloading' ? (
                  <RotateCw size={14} className="app-update-strip__icon app-update-strip__icon--spin" aria-hidden="true" />
                ) : updateState.readyToInstall ? (
                  <CheckCircle2 size={14} className="app-update-strip__icon" aria-hidden="true" />
                ) : (
                  <Download size={14} className="app-update-strip__icon" aria-hidden="true" />
                )}
                <span className="app-update-strip__text">
                  {updateState.status === 'downloading'
                    ? t('about.downloadingDetail', {
                        version: updateState.latestVersion,
                        progress: updateState.progress,
                        defaultValue: `Downloading ${updateState.latestVersion} (${updateState.progress}%)`,
                      })
                    : updateState.readyToInstall
                      ? String(t('about.stripReadyMessage' as never, {
                          version: updateState.latestVersion,
                          defaultValue: `${updateState.latestVersion} is ready to install — restart to complete the update`,
                        } as never))
                      : String(t('about.stripAvailableMessage' as never, {
                          version: updateState.latestVersion,
                          defaultValue: `New version ${updateState.latestVersion} is available`,
                        } as never))}
                </span>
              </div>
              <div className="app-update-strip__actions">
                <button
                  type="button"
                  className="app-update-strip__btn app-update-strip__btn--ghost"
                  onClick={() => handleNavSelect('about')}
                >
                  {t('about.viewUpdate' as never, 'View Update')}
                </button>
                <button
                  type="button"
                  className="app-update-strip__btn app-update-strip__btn--primary"
                  disabled={updateState.status === 'downloading' || installingUpdate}
                  onClick={() => void handleStripDownloadOrInstall()}
                >
                  {updateState.status === 'downloading'
                    ? `${updateState.progress}%`
                    : updateState.readyToInstall
                      ? t('about.installNowBtn' as never, 'Install Now')
                      : t('about.downloadNowBtn' as never, 'Download Now')}
                </button>
                <button
                  type="button"
                  className="app-update-strip__close"
                  disabled={dismissingUpdate}
                  onClick={() => void handleStripDismissUpdate()}
                  title={t('about.dismissVersion' as never, 'Dismiss this version')}
                  aria-label={t('about.dismissVersion' as never, 'Dismiss this version')}
                >
                  <X size={14} aria-hidden="true" />
                </button>
              </div>
            </div>
          )}

          {/* ── 页面滚动流 (流体自适应双阶画布：表单 960px / 工作台 1280px + @container 感知) ── */}
          <div
            className={`app__content ${activeRoute.canvasWidth === 'workspace' ? 'app__content--workspace' : 'app__content--reading'}`}
            ref={contentContainerRef}
            key={activeNav}
          >
            <header className="app__header">
              <div className="app__header-inner">
                <div className="app__header-badge" aria-hidden="true">
                  <ActiveHeaderIcon size={17} strokeWidth={1.85} />
                </div>
                <div className="app__header-text">
                  <h1 id="app-page-title" ref={pageTitleRef} tabIndex={-1} className="app__header-title">
                    {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                    {activePluginMeta ? t(pageMeta.titleKey as any, { defaultValue: activePluginMeta.name }) : t(pageMeta.titleKey as any)}
                  </h1>
                  <p className="app__header-subtitle">
                    {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                    {activePluginMeta ? t(pageMeta.subtitleKey as any, { defaultValue: activePluginMeta.fileName }) : t(pageMeta.subtitleKey as any)}
                  </p>
                </div>
              </div>
            </header>

            <Suspense fallback={<div role="status" aria-live="polite" className="surface-loading">{t('common.loading')}</div>}>
              {renderPage()}
            </Suspense>
          </div>

          {/* ── 世界级浮动胶囊已保存 Toast ─────────────────── */}
          <SavedToast />
        </main>
      </div>
      {showOnboarding && <OnboardingModal onComplete={handleOnboardingComplete} />}
      {showFeedbackModal && (
        <Suspense fallback={null}>
          <FeedbackModal open={showFeedbackModal} onClose={() => setShowFeedbackModal(false)} />
        </Suspense>
      )}
      {changelogOpen && (
        <Suspense fallback={null}>
          <WhatsNewModal
            open={changelogOpen}
            mode={changelogMode}
            currentVersion={changelogState.currentVersion}
            previousVersion={changelogState.previousVersion}
            showChangelogOnUpdate={changelogState.showChangelogOnUpdate}
            onToggleShowOnUpdate={(enabled) => {
              setChangelogState((prev) => ({ ...prev, showChangelogOnUpdate: enabled }));
            }}
            onClose={handleCloseChangelog}
          />
        </Suspense>
      )}
      <WindowResizeHandles />
    </div>
  );
}

export default App;

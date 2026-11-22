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
import { TitleBar } from './components/TitleBar';
import { Sidebar, type NavId } from './components/Sidebar';
import type { PluginStatus } from './pages/PluginsPage';
import { OnboardingModal } from './components/OnboardingModal';
import { SavedToast } from './components/SavedToast';
import { WindowResizeHandles } from './components/WindowResizeHandles';
import { getPageMetadata, PAGE_DEFINITIONS } from './pages/registry';
import { resolveExtensionRoute } from './config/routes';
import { consumePendingDeepNavigation, type DeepNavigationPayload } from './config/settingsSearchIndex';
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
const PluginsPage = lazy(() => import('./pages/PluginsPage').then((module) => ({ default: module.PluginsPage })));
const HistoryPage = lazy(() => import('./pages/HistoryPage'));
const KeyStatsPage = lazy(() => import('./pages/KeyStatsPage').then((module) => ({ default: module.KeyStatsPage })));
const HotCornerPage = lazy(() => import('./pages/HotCornerPage').then((module) => ({ default: module.HotCornerPage })));
const SearchPage = lazy(() => import('./pages/SearchPage').then((module) => ({ default: module.SearchPage })));
const KeycastPage = lazy(() => import('./pages/KeycastPage').then((module) => ({ default: module.KeycastPage })));
const SpotlightPage = lazy(() => import('./pages/SpotlightPage').then((module) => ({ default: module.SpotlightPage })));
const DialogEnhancerPage = lazy(() => import('./pages/DialogEnhancerPage').then((module) => ({ default: module.DialogEnhancerPage })));
const RemoteBoostPage = lazy(() => import('./pages/RemoteBoostPage').then((module) => ({ default: module.RemoteBoostPage })));
const PicasaViewerPage = lazy(() => import('./pages/PicasaViewerPage').then((module) => ({ default: module.PicasaViewerPage })));

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
      const saved = localStorage.getItem('tools3000:last-nav');
      if (saved) {
        const cached = getInitialActivePluginIds();
        if (cached) {
          const cachedSet = new Set(cached);
          const pageDef = PAGE_DEFINITIONS.find((p) => p.id === saved);
          if (pageDef?.requiresPlugin) {
            const req = pageDef.requiresPlugin;
            const ok = (req === 'dialogenhancer' || req === 'dialog_enhancer')
              ? (cachedSet.has('dialogenhancer') || cachedSet.has('dialog_enhancer'))
              : cachedSet.has(req);
            if (!ok) return 'general';
          }
        }
        return saved as NavId;
      }
    } catch {
      // 忽略本地存储访问异常
    }
    return 'general';
  });
  const getSystemTheme = (): Theme =>
    window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';

  const [systemTheme, setSystemTheme] = useState<Theme>(getSystemTheme());
  const [themePreference, setThemePreference] = useState<ThemePreference>(() => readStoredTheme('system'));
  const [accent, setAccent] = useState<string>(() => readStoredAccent('blue'));
  const theme: Theme = themePreference === 'system' ? systemTheme : themePreference;
  const [showOnboarding, setShowOnboarding] = useState(false);
  const [plugins, setPlugins] = useState<PluginStatus[]>([]);
  const [retainedDisabledNav, setRetainedDisabledNav] = useState<NavId | null>(null);
  const activeNavRef = useRef<NavId>(activeNav);
  const picasaWasActiveRef = useRef<boolean>(false);
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

  const applyPluginsSnapshot = useCallback((items: PluginStatus[]) => {
    const safeItems = Array.isArray(items) ? items : [];
    setPlugins(safeItems);
    const activeList = safeItems
      .filter((plugin) => plugin.enabled && plugin.active)
      .map((p) => p.id);
    const picasaNowActive = activeList.includes('picasa_viewer');
    if (!picasaNowActive && picasaWasActiveRef.current && activeNavRef.current === 'picasa_viewer') {
      // 用户在 Picasa 图片查看器页面内就地关闭总开关时，保留当前页供查看停用态或直接重开
      setRetainedDisabledNav('picasa_viewer');
    } else if (picasaNowActive) {
      setRetainedDisabledNav((prev) => (prev === 'picasa_viewer' ? null : prev));
    }
    picasaWasActiveRef.current = picasaNowActive;
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
    return new Set(['gesture', 'capture', 'search', 'keycast', 'spotlight', 'dialogenhancer', 'dialog_enhancer', 'remote_boost']);
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

  // 检查是否需要显示首次引导（综合判定首次运行或用户在通用设置中显式手动开启）
  useEffect(() => {
    let cancelled = false;
    Promise.allSettled([
      bridgeRequest<boolean | null>('config.get', { key: '/app/onboardingCompleted' }),
      bridgeRequest<boolean | null>('config.get', { key: '/general/showOnboarding' }),
    ]).then(([completedRes, showRes]) => {
      if (cancelled) return;
      const completed = completedRes.status === 'fulfilled' ? completedRes.value : false;
      const explicitShow = showRes.status === 'fulfilled' ? showRes.value : false;
      if (shouldShowOnboarding({ completed, explicitShow })) {
        setShowOnboarding(true);
      }
    }).catch(() => {
      // 容错异常时不强行阻断主界面
    });
    return () => { cancelled = true; };
  }, []);

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
    window.addEventListener('tools3000:navigate-deep', onDeepNav);
    return () => {
      window.removeEventListener('tools3000:navigate-deep', onDeepNav);
      if (highlightTimer) clearTimeout(highlightTimer);
      if (pollTimer) clearTimeout(pollTimer);
    };
  }, [activeNav]);

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
    const standardPluginIds = new Set([
      'gesture',
      'capture',
      'search',
      'keycast',
      'spotlight',
      'dialogenhancer',
      'dialog_enhancer',
      'remote_boost',
      'remote',
      'picasa_viewer',
      'picasaviewer',
    ]);
    return plugins.filter((p) => !standardPluginIds.has(p.id) && p.enabled && p.active);
  }, [plugins]);

  const installedExtensionIds = useMemo(
    () => installedExtensions.map((p) => p.id),
    [installedExtensions],
  );

  const isNavActive = useCallback((navId: NavId): boolean => {
    const pageDef = PAGE_DEFINITIONS.find((p) => p.id === navId);
    if (pageDef) {
      if (pageDef.requiresPlugin) {
        if (pageDef.requiresPlugin === 'dialogenhancer' || pageDef.requiresPlugin === 'dialog_enhancer') {
          return activePlugins.has('dialogenhancer') || activePlugins.has('dialog_enhancer');
        }
        return activePlugins.has(pageDef.requiresPlugin);
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
      return <GeneralPage />;
    }
    switch (activeNav) {
      case 'stats':           return <KeyStatsPage />;
      case 'gesture':         return <GesturePage />;
      case 'hotcorner':       return <HotCornerPage />;
      case 'capture':         return <CapturePage />;
      case 'ocr':             return <OcrPage />;
      case 'history':         return <HistoryPage />;
      case 'search':          return <SearchPage />;
      case 'keycast':         return <KeycastPage />;
      case 'spotlight':       return <SpotlightPage />;
      case 'dialog_enhancer': return <DialogEnhancerPage />;
      case 'remote_boost':    return <RemoteBoostPage />;
      case 'picasa_viewer':   return <PicasaViewerPage />;
      case 'plugins':         return <PluginsPage initialPlugins={plugins} onNavigate={handleNavSelect} />;
      case 'general':         return <GeneralPage />;
      case 'about':           return <AboutPage />;
      default:
        return <GeneralPage />;
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
          activePlugins={activePlugins}
          installedExtensionIds={installedExtensionIds}
          installedExtensions={installedExtensions}
        />
        <main className="app__main" aria-labelledby="app-page-title">
          <AppToaster theme={theme} surface="settings" />

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
      <WindowResizeHandles />
    </div>
  );
}

export default App;

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
import { bridgeRequest, useBridgeEvent } from './hooks/useBridge';
import { shouldShowOnboarding } from './onboardingGate';
import { Toaster } from 'sonner';
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
const ExtensionPage = lazy(() => import('./pages/ExtensionPage').then((module) => ({ default: module.ExtensionPage })));

type Theme = 'dark' | 'light';
type ThemePreference = Theme | 'system';

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
  const { t, i18n } = useTranslation();
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
  const [themePreference, setThemePreference] = useState<ThemePreference>('system');
  const [accent, setAccent] = useState<string>(() => {
    try {
      return localStorage.getItem('tools3000:accent-color') || 'blue';
    } catch {
      return 'blue';
    }
  });
  const theme: Theme = themePreference === 'system' ? systemTheme : themePreference;
  const [showOnboarding, setShowOnboarding] = useState(false);
  const [plugins, setPlugins] = useState<PluginStatus[]>([]);
  const pageTitleRef = useRef<HTMLHeadingElement>(null);

  const handleNavSelect = useCallback((nav: NavId) => {
    setActiveNav(nav);
    try {
      localStorage.setItem('tools3000:last-nav', nav);
    } catch {
      // 忽略本地存储保存异常
    }
  }, []);

  useEffect(() => {
    const applyPlugins = (items: PluginStatus[]) => {
      const safeItems = Array.isArray(items) ? items : [];
      setPlugins(safeItems);
      const activeList = safeItems
        .filter((plugin) => plugin.enabled && plugin.active)
        .map((p) => p.id);
      setActivePluginIds(activeList);
      try {
        localStorage.setItem('tools3000:active-plugins', JSON.stringify(activeList));
      } catch {
        // 忽略本地存储写入异常
      }
    };
    void bridgeRequest<PluginStatus[]>('plugins.getAll').then(applyPlugins).catch(console.error);
    const handleChange = (event: Event) => applyPlugins((event as CustomEvent<PluginStatus[]>).detail);
    window.addEventListener('tools3000:plugins-changed', handleChange);
    return () => window.removeEventListener('tools3000:plugins-changed', handleChange);
  }, []);

  useBridgeEvent('plugins:changed', () => {
    void bridgeRequest<PluginStatus[]>('plugins.getAll').then((items) => {
      const safeItems = Array.isArray(items) ? items : [];
      setPlugins(safeItems);
      const activeList = safeItems
        .filter((plugin) => plugin.enabled && plugin.active)
        .map((p) => p.id);
      setActivePluginIds(activeList);
      try {
        localStorage.setItem('tools3000:active-plugins', JSON.stringify(activeList));
      } catch {
        // 忽略本地存储写入异常
      }
    }).catch(console.error);
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

  const [isElevated, setIsElevated] = useState(false);

  useEffect(() => {
    let cancelled = false;
    bridgeRequest<{ theme?: string; language?: string; accentColor?: string; elevated?: boolean }>('general.getSettings')
      .then((settings) => {
        if (cancelled) return;
        if (typeof settings.elevated === 'boolean') {
          setIsElevated(settings.elevated);
        }
        if (settings.theme === 'light' || settings.theme === 'dark' || settings.theme === 'system') {
          setThemePreference(settings.theme);
        }
        if (typeof settings.accentColor === 'string' && settings.accentColor) {
          setAccent(settings.accentColor);
        }
        if (settings.language && settings.language !== 'auto') {
          const targetLang = settings.language.toLowerCase().startsWith('zh') ? 'zh' : 'en';
          if (i18n.language !== targetLang) {
            void i18n.changeLanguage(targetLang);
          }
        }
      })
      .catch(console.error);
    return () => { cancelled = true; };
    // 见 GeneralPage：不能把 i18n 放进依赖，否则 changeLanguage 会形成 IPC 风暴。
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

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

  // 监听系统主题变化；只在“跟随系统”时影响最终主题。
  useEffect(() => {
    const mediaQuery = window.matchMedia('(prefers-color-scheme: dark)');
    const handleChange = (e: MediaQueryListEvent) => {
      setSystemTheme(e.matches ? 'dark' : 'light');
    };
    mediaQuery.addEventListener('change', handleChange);
    return () => mediaQuery.removeEventListener('change', handleChange);
  }, []);


  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
  }, [theme]);

  useEffect(() => {
    document.documentElement.setAttribute('data-accent', accent);
  }, [accent]);

  useEffect(() => {
    if (showOnboarding) return;
    const frame = requestAnimationFrame(() => pageTitleRef.current?.focus());
    return () => cancelAnimationFrame(frame);
  }, [activeNav, showOnboarding]);

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
        try {
          localStorage.setItem('tools3000:accent-color', newAccent);
        } catch (e) {
          void e;
        }
      }
    };
    window.addEventListener('tools3000:theme-changed', handlePreference);
    window.addEventListener('tools3000:accent-changed', handleAccent);
    return () => {
      window.removeEventListener('tools3000:theme-changed', handlePreference);
      window.removeEventListener('tools3000:accent-changed', handleAccent);
    };
  }, []);

  const standardPluginIds = new Set(['gesture', 'capture', 'search', 'keycast', 'spotlight', 'dialogenhancer', 'dialog_enhancer', 'remote_boost', 'remote']);
  const installedExtensionIds = plugins
    .filter((p) => !standardPluginIds.has(p.id) && p.enabled && p.active)
    .map((p) => p.id);

  // 检查目标页面是否所属插件已启用。若未启用则阻断渲染并触发回退，节省 WebView2 物理内存
  const isNavActive = useCallback((navId: NavId): boolean => {
    const pageDef = PAGE_DEFINITIONS.find((p) => p.id === navId);
    if (pageDef?.requiresPlugin) {
      if (pageDef.requiresPlugin === 'dialogenhancer' || pageDef.requiresPlugin === 'dialog_enhancer') {
        return activePlugins.has('dialogenhancer') || activePlugins.has('dialog_enhancer');
      }
      return activePlugins.has(pageDef.requiresPlugin);
    }
    if (pageDef?.category === 'extension') {
      return installedExtensionIds.includes(navId);
    }
    return true;
  }, [activePlugins, installedExtensionIds]);

  // 当插件状态变更或初次加载完毕时，若当前页面已被停用，自动优雅回退到通用设置
  useEffect(() => {
    if (!isNavActive(activeNav)) {
      const timer = setTimeout(() => {
        handleNavSelect('general');
      }, 0);
      return () => clearTimeout(timer);
    }
  }, [activeNav, isNavActive, handleNavSelect]);

  // 渲染当前页面：未启用页面的组件绝不挂载到 DOM 中，以阻断资源预载并释放内存
  const renderPage = () => {
    if (!isNavActive(activeNav)) {
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
      case 'plugins':         return <PluginsPage initialPlugins={plugins} />;
      case 'general':         return <GeneralPage />;
      case 'about':           return <AboutPage />;
      case 'ai_assistant':
      case 'color_picker':
      case 'clipboard_manager':
      case 'markdown_preview':
        return (
          <ExtensionPage
            pluginId={activeNav}
            plugin={plugins.find((p) => p.id === activeNav)}
            onUninstall={() => {
              handleNavSelect('plugins');
              void bridgeRequest<PluginStatus[]>('plugins.getAll').then((items) =>
                setPlugins(Array.isArray(items) ? items : [])
              );
            }}
          />
        );
      default:          return <KeyStatsPage />;
    }
  };

  return (
    <div className="app">
      <TitleBar
        isElevated={isElevated}
        themePreference={themePreference}
        onSelectThemePreference={(pref) => {
          setThemePreference(pref);
          bridgeRequest<{ success: boolean }>('general.updateSettings', { theme: pref }).catch(console.error);
          window.dispatchEvent(new CustomEvent('tools3000:theme-changed', { detail: pref }));
        }}
        accent={accent}
        onSelectAccent={(newAccent) => {
          setAccent(newAccent);
          try {
            localStorage.setItem('tools3000:accent-color', newAccent);
          } catch (e) {
            void e;
          }
          bridgeRequest<{ success: boolean }>('general.updateSettings', { accentColor: newAccent }).catch(console.error);
          window.dispatchEvent(new CustomEvent('tools3000:accent-changed', { detail: newAccent }));
        }}
      />
      <div className="app__body">
        <Sidebar
          activeNav={activeNav}
          onNavigate={handleNavSelect}
          activePlugins={activePlugins}
          installedExtensionIds={installedExtensionIds}
        />
        <main className="app__main" aria-labelledby="app-page-title">
          <Toaster position="bottom-right" theme={theme} richColors expand={true} />
          {/* ── 页面头部 ────────────────────────────────────── */}
          <header className="app__header">
            <div className="app__header-text">
              {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
              <h1 id="app-page-title" ref={pageTitleRef} tabIndex={-1} className="app__header-title">{t(getPageMetadata(activeNav).titleKey as any)}</h1>
              {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
              <p className="app__header-subtitle">{t(getPageMetadata(activeNav).subtitleKey as any)}</p>
            </div>
          </header>

          {/* ── 页面内容 ────────────────────────────────────── */}
          <div className="app__content" key={activeNav}>
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

/* ─────────────────────────────────────────────────────────────────────────────
 * Sidebar — 左侧图标导航栏
 *
 * 参考 Aitiy 设计：
 *   - 窄侧边栏 + 图标 + 文字标签
 *   - 当前选中项有紫色高亮指示条
 *   - 底部有版本信息和主题切换
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useMemo, useRef, useEffect, type FC, type KeyboardEvent as ReactKeyboardEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { Search, X, CornerDownRight, Power, MessageSquarePlus } from 'lucide-react';
import { toast } from 'sonner';
import { CodeBadge } from './UIKit';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import type { PluginStatus } from '../pages/PluginsPage';
import './Sidebar.css';
import {
  APP_ROUTES,
  ROUTE_MAP,
  isPluginActiveInSet,
  resolveExtensionRoute,
  type RouteItemConfig,
  type NavId,
} from '../config/routes';
import {
  DEEP_SETTINGS_INDEX,
  emitDeepNavigation,
  type DeepSettingEntry,
} from '../config/settingsSearchIndex';

export type { NavId };

export interface SidebarExtensionDescriptor {
  id: string;
  name?: string;
}

export interface SidebarProps {
  activeNav: NavId;
  onNavigate: (id: NavId) => void;
  onOpenFeedback?: () => void;
  activePlugins?: ReadonlySet<string>;
  installedExtensionIds?: string[];
  installedExtensions?: ReadonlyArray<SidebarExtensionDescriptor>;
}

const SIDEBAR_KEYWORD_KEYS: Partial<Record<NavId, string>> = {
  general: 'sidebar.kw_general',
  plugins: 'sidebar.kw_plugins',
  search: 'sidebar.kw_search',
  clipboard: 'sidebar.kw_clipboard',
  text_expander: 'sidebar.kw_text_expander',
  gesture: 'sidebar.kw_gesture',
  hotcorner: 'sidebar.kw_hotcorner',
  capture: 'sidebar.kw_capture',
  history: 'sidebar.kw_history',
  ocr: 'sidebar.kw_ocr',
  keycast: 'sidebar.kw_keycast',
  spotlight: 'sidebar.kw_spotlight',
  dialog_enhancer: 'sidebar.kw_dialog_enhancer',
  file_locksmith: 'sidebar.kw_file_locksmith',
  remote_boost: 'sidebar.kw_remote_boost',
  picasa_viewer: 'sidebar.kw_picasa_viewer',
  stats: 'sidebar.kw_stats',
  performance: 'sidebar.kw_performance',
  about: 'sidebar.kw_about',
};

export const Sidebar: FC<SidebarProps> = ({
  activeNav,
  onNavigate,
  onOpenFeedback,
  activePlugins,
  installedExtensionIds = [],
  installedExtensions,
}) => {
  const { t } = useTranslation();
  const [filterQuery, setFilterQuery] = useState('');
  const [enablingPluginId, setEnablingPluginId] = useState<string | null>(null);
  const [updateBadge, setUpdateBadge] = useState<{
    hasUpdate: boolean;
    badgeVisible: boolean;
    readyToInstall: boolean;
    latestVersion: string;
  }>({
    hasUpdate: false,
    badgeVisible: false,
    readyToInstall: false,
    latestVersion: '',
  });
  const searchInputRef = useRef<HTMLInputElement>(null);

  const applyUpdateBadgePayload = (raw: unknown) => {
    if (!raw || typeof raw !== 'object') return;
    const st = raw as {
      hasUpdate?: boolean;
      badgeVisible?: boolean;
      dismissed?: boolean;
      readyToInstall?: boolean;
      latestVersion?: string;
    };
    if (typeof st.hasUpdate !== 'boolean' && typeof st.badgeVisible !== 'boolean') return;
    const hasUpdate = Boolean(st.hasUpdate);
    const badgeVisible = typeof st.badgeVisible === 'boolean'
      ? st.badgeVisible
      : (hasUpdate && !st.dismissed);
    setUpdateBadge({
      hasUpdate,
      badgeVisible,
      readyToInstall: Boolean(st.readyToInstall),
      latestVersion: st.latestVersion ?? '',
    });
  };

  useEffect(() => {
    bridgeRequest<unknown>('app.getUpdateState', undefined, { silent: true })
      .then((st) => {
        applyUpdateBadgePayload(st);
      })
      .catch(() => {});
  }, []);

  useBridgeEvent('update.state', (raw) => {
    applyUpdateBadgePayload(raw);
  });

  useEffect(() => {
    const mql = typeof window.matchMedia === 'function' ? window.matchMedia('(max-width: 768px)') : null;
    if (!mql) return;
    const handleViewportChange = (ev: MediaQueryListEvent | MediaQueryList) => {
      if (ev.matches) {
        setFilterQuery('');
      }
    };
    handleViewportChange(mql);
    if (typeof mql.addEventListener === 'function') {
      mql.addEventListener('change', handleViewportChange);
      return () => mql.removeEventListener('change', handleViewportChange);
    }
    return undefined;
  }, []);

  useEffect(() => {
    const handleGlobalKeyDown = (e: KeyboardEvent) => {
      if (e.defaultPrevented) return;
      if (!(e.ctrlKey || e.metaKey) || e.altKey || e.shiftKey) return;
      if (document.querySelector('[aria-modal="true"], [role="dialog"]')) return;
      if (typeof window.matchMedia === 'function' && window.matchMedia('(max-width: 768px)').matches) {
        return;
      }
      const keyLower = e.key.toLowerCase();
      if (keyLower === 'k') {
        e.preventDefault();
        searchInputRef.current?.focus();
        searchInputRef.current?.select();
        return;
      }
      if (keyLower === 'f') {
        const active = document.activeElement as HTMLElement | null;
        const inOtherEditable =
          active &&
          active !== searchInputRef.current &&
          (active.tagName === 'INPUT' ||
            active.tagName === 'TEXTAREA' ||
            active.tagName === 'SELECT' ||
            active.isContentEditable);
        if (!inOtherEditable) {
          e.preventDefault();
          searchInputRef.current?.focus();
          searchInputRef.current?.select();
        }
      }
    };
    window.addEventListener('keydown', handleGlobalKeyDown);
    return () => window.removeEventListener('keydown', handleGlobalKeyDown);
  }, []);

  const isItemAvailable = (item: RouteItemConfig) =>
    isPluginActiveInSet(activePlugins, item.requiresPlugin);

  const systemNavItems = APP_ROUTES.filter((r) => r.category === 'system');
  const availableCoreTools = APP_ROUTES.filter((r) => r.category === 'core_tools').filter(isItemAvailable);
  const insightNavItems = APP_ROUTES.filter((r) => r.category === 'insights');
  const extensionNavItems = useMemo(() => {
    const nameMap = new Map<string, string>();
    if (installedExtensions) {
      for (const ext of installedExtensions) {
        if (ext.id && ext.name) {
          nameMap.set(ext.id, ext.name);
        }
      }
    }
    const ids = installedExtensions && installedExtensions.length > 0
      ? installedExtensions.map((ext) => ext.id)
      : installedExtensionIds;
    const seen = new Set<string>();
    const routes: RouteItemConfig[] = [];
    for (const extId of ids) {
      if (!extId || seen.has(extId) || ROUTE_MAP.has(extId)) continue;
      seen.add(extId);
      routes.push(resolveExtensionRoute(extId, nameMap.get(extId)));
    }
    return routes;
  }, [installedExtensionIds, installedExtensions]);

  const resolveRouteLabel = (route: RouteItemConfig) => {
    if (route.fallbackLabel) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return String(t(route.labelKey as any, { defaultValue: route.fallbackLabel }));
    }
    return String(t(route.labelKey as unknown as TemplateStringsArray));
  };

  const normalizedTokens = useMemo(
    () =>
      filterQuery
        .trim()
        .toLowerCase()
        .split(/\s+/)
        .filter(Boolean),
    [filterQuery],
  );

  const matchesFilter = (item: RouteItemConfig) => {
    if (normalizedTokens.length === 0) return true;
    const kwKey = SIDEBAR_KEYWORD_KEYS[item.id];
    const haystack = [
      item.id,
      resolveRouteLabel(item),
      item.fallbackLabel || '',
      String(t(item.subtitleKey as unknown as TemplateStringsArray)),
      kwKey ? String(t(kwKey as unknown as TemplateStringsArray)) : '',
    ]
      .join(' ')
      .toLowerCase();
    return normalizedTokens.every((token) => haystack.includes(token));
  };

  const filteredSystem = systemNavItems.filter(matchesFilter);
  const filteredCoreTools = availableCoreTools.filter(matchesFilter);
  const filteredExtensions = extensionNavItems.filter(matchesFilter);
  const filteredInsights = insightNavItems.filter(matchesFilter);
  const allMatchedItems = [
    ...filteredSystem,
    ...filteredCoreTools,
    ...filteredExtensions,
    ...filteredInsights,
  ];

  const resolveDeepLabel = (entry: DeepSettingEntry) =>
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    String(t(entry.labelKey as any));

  const matchesDeepEntry = (entry: DeepSettingEntry, route: RouteItemConfig | undefined) => {
    if (normalizedTokens.length === 0) return false;
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const descText = entry.descKey ? String(t(entry.descKey as any)) : '';
    const routeText = route ? resolveRouteLabel(route) : '';
    const haystack = [
      entry.id,
      entry.navId,
      entry.tabId || '',
      resolveDeepLabel(entry),
      descText,
      routeText,
      entry.keywords,
    ]
      .join(' ')
      .toLowerCase();
    return normalizedTokens.every((token) => haystack.includes(token));
  };

  const routeMap = useMemo(() => {
    const map = new Map<NavId, RouteItemConfig>();
    for (const r of APP_ROUTES) {
      map.set(r.id, r);
    }
    return map;
  }, []);

  const matchedDeepEnabled = useMemo(() => {
    if (normalizedTokens.length === 0) return [];
    const results: Array<{ entry: DeepSettingEntry; route: RouteItemConfig }> = [];
    for (const entry of DEEP_SETTINGS_INDEX) {
      const route = routeMap.get(entry.navId);
      if (!route || !isItemAvailable(route)) continue;
      if (matchesDeepEntry(entry, route)) {
        results.push({ entry, route });
        if (results.length >= 8) break;
      }
    }
    return results;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [normalizedTokens, routeMap, activePlugins, t]);

  const matchedInactiveRoutes = useMemo(() => {
    if (normalizedTokens.length === 0 || !activePlugins) return [];
    const inactiveCoreRoutes = APP_ROUTES.filter(
      (r) => r.category === 'core_tools' && !isItemAvailable(r),
    );
    const results: Array<{ route: RouteItemConfig; deepEntry?: DeepSettingEntry }> = [];
    for (const route of inactiveCoreRoutes) {
      const hitDeep = DEEP_SETTINGS_INDEX.find(
        (entry) => entry.navId === route.id && matchesDeepEntry(entry, route),
      );
      if (matchesFilter(route) || hitDeep) {
        results.push({ route, deepEntry: hitDeep });
      }
    }
    return results;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [normalizedTokens, activePlugins, t]);

  const handleDeepJump = (entry: DeepSettingEntry) => {
    const labelText = resolveDeepLabel(entry);
    onNavigate(entry.navId);
    emitDeepNavigation({
      navId: entry.navId,
      tabId: entry.tabId,
      targetText: labelText,
    });
  };

  const handleEnableAndNavigate = async (route: RouteItemConfig, deepEntry?: DeepSettingEntry) => {
    const pluginId = route.requiresPlugin === 'dialog_enhancer'
      ? 'dialogenhancer'
      : route.requiresPlugin;
    if (!pluginId) {
      onNavigate(route.id);
      return;
    }
    if (enablingPluginId === pluginId) return;
    setEnablingPluginId(pluginId);
    try {
      const res = await bridgeRequest<{ success?: boolean; restartRequired?: boolean; error?: string }>(
        'plugins.setEnabled',
        { id: pluginId, enabled: true },
        { silent: true },
      );
      if (!res?.success) {
        throw new Error(res?.error || t('plugins.saveFailed'));
      }
      const latestPlugins = await bridgeRequest<PluginStatus[]>('plugins.getAll', undefined, { silent: true }).catch(() => null);
      if (Array.isArray(latestPlugins)) {
        window.dispatchEvent(new CustomEvent('tools3000:plugins-changed', { detail: latestPlugins }));
      }
      setFilterQuery('');
      if (res.restartRequired) {
        toast.success(t('plugins.enabledSaved'), { description: t('plugins.restartHint') });
        onNavigate('plugins');
      } else {
        toast.success(t('plugins.enabledSaved'));
        onNavigate(route.id);
        if (deepEntry) {
          emitDeepNavigation({
            navId: route.id,
            tabId: deepEntry.tabId,
            targetText: resolveDeepLabel(deepEntry),
          });
        }
      }
    } catch (err) {
      toast.error(t('plugins.saveFailed'), { description: String(err) });
      onNavigate('plugins');
    } finally {
      setEnablingPluginId(null);
    }
  };

  const handleSearchKeyDown = (e: ReactKeyboardEvent<HTMLInputElement>) => {
    if (e.nativeEvent.isComposing || e.keyCode === 229) return;
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      if (filterQuery) {
        setFilterQuery('');
      } else {
        searchInputRef.current?.blur();
      }
      return;
    }
    if (e.key === 'Enter') {
      if (allMatchedItems.length > 0) {
        e.preventDefault();
        const primaryRoute = allMatchedItems[0];
        onNavigate(primaryRoute.id);
        const subHit = matchedDeepEnabled.find((m) => m.entry.navId === primaryRoute.id);
        if (subHit) {
          emitDeepNavigation({
            navId: primaryRoute.id,
            tabId: subHit.entry.tabId,
            targetText: resolveDeepLabel(subHit.entry),
          });
        }
        return;
      }
      if (matchedDeepEnabled.length > 0) {
        e.preventDefault();
        handleDeepJump(matchedDeepEnabled[0].entry);
        return;
      }
      if (matchedInactiveRoutes.length > 0) {
        e.preventDefault();
        void handleEnableAndNavigate(
          matchedInactiveRoutes[0].route,
          matchedInactiveRoutes[0].deepEntry,
        );
      }
    }
  };

  const renderNavItem = (item: RouteItemConfig, customLabel?: string) => {
    const unavailable = !isItemAvailable(item);
    const IconComponent = item.icon;
    const isActive = activeNav === item.id;
    return (
      <button
        key={item.id}
        id={`nav-${item.id}`}
        className={`sidebar__item ${isActive ? 'sidebar__item--active' : ''}`}
        onClick={() => onNavigate(item.id)}
        disabled={unavailable}
        title={unavailable ? t('sidebar.pluginDisabled') : undefined}
        aria-current={isActive ? 'page' : undefined}
      >
        <span className="sidebar__item-indicator" />
        <span className="sidebar__item-icon" aria-hidden="true">
          <IconComponent size={15} strokeWidth={isActive ? 2.1 : 1.75} />
        </span>
        <span className="sidebar__item-label">
          {customLabel ?? t(item.labelKey as unknown as TemplateStringsArray)}
        </span>
        {item.id === 'about' && updateBadge.badgeVisible && (
          <span
            className={`sidebar__update-pill${updateBadge.readyToInstall ? ' sidebar__update-pill--ready' : ''}`}
            title={
              updateBadge.readyToInstall
                ? t('about.updateReadyHint', { version: updateBadge.latestVersion, defaultValue: `Update ${updateBadge.latestVersion} ready` })
                : t('about.updateAvailable', { version: updateBadge.latestVersion, defaultValue: `Update ${updateBadge.latestVersion} available` })
            }
            aria-label={
              updateBadge.readyToInstall
                ? t('about.updateReadyHint', { version: updateBadge.latestVersion, defaultValue: `Update ${updateBadge.latestVersion} ready` })
                : t('about.updateAvailable', { version: updateBadge.latestVersion, defaultValue: `Update ${updateBadge.latestVersion} available` })
            }
          >
            <span className="sidebar__update-pill-dot" aria-hidden="true" />
            <span className="sidebar__update-pill-text">
              {updateBadge.latestVersion || 'NEW'}
            </span>
          </span>
        )}
      </button>
    );
  };

  const showCoreDivider = filteredSystem.length > 0 && filteredCoreTools.length > 0;
  const showExtDivider =
    (filteredSystem.length > 0 || filteredCoreTools.length > 0) && filteredExtensions.length > 0;
  const showInsightDivider =
    (filteredSystem.length > 0 || filteredCoreTools.length > 0 || filteredExtensions.length > 0) &&
    filteredInsights.length > 0;

  return (
    <aside className="sidebar" role="navigation" aria-label={t('sidebar.mainNav')}>
      {/* ── 快速检索框 ────────────────────────────────────────────── */}
      <div className="sidebar__search-wrap">
        <div className="sidebar__search-box">
          <Search size={14} className="sidebar__search-icon" aria-hidden="true" />
          <input
            ref={searchInputRef}
            type="text"
            className="sidebar__search-input"
            value={filterQuery}
            onChange={(e) => setFilterQuery(e.target.value)}
            onKeyDown={handleSearchKeyDown}
            placeholder={t('sidebar.searchPlaceholder', 'Jump to module...')}
            aria-label={t('sidebar.searchAria', 'Filter settings modules')}
            autoComplete="off"
            spellCheck={false}
          />
          {filterQuery ? (
            <button
              type="button"
              className="sidebar__search-clear"
              onClick={() => {
                setFilterQuery('');
                searchInputRef.current?.focus();
              }}
              title={t('sidebar.searchClear', 'Clear filter')}
              aria-label={t('sidebar.searchClear', 'Clear filter')}
            >
              <X size={13} aria-hidden="true" />
            </button>
          ) : (
            <CodeBadge variant="muted" className="sidebar__search-shortcut">
              Ctrl+K
            </CodeBadge>
          )}
        </div>
      </div>

      {/* ── 导航列表 ──────────────────────────────────────────────── */}
      <nav className="sidebar__nav">
        {allMatchedItems.length === 0 && matchedDeepEnabled.length === 0 && matchedInactiveRoutes.length === 0 ? (
          <div className="sidebar__search-empty" role="status">
            {t('sidebar.searchEmpty', 'No matching module')}
          </div>
        ) : (
          <>
            {/* 1. 系统总控组 */}
            {filteredSystem.length > 0 && (
              <div className="sidebar__section-label">{t('sidebar.groupSystem')}</div>
            )}
            {filteredSystem.map((item) => renderNavItem(item))}

            {/* 分割线 1 */}
            {showCoreDivider && <div className="sidebar__divider" role="separator" />}

            {/* 2. 核心效率工具组 (仅展示当前已启用的功能) */}
            {filteredCoreTools.length > 0 && (
              <div className="sidebar__section-label">{t('sidebar.groupCoreTools')}</div>
            )}
            {filteredCoreTools.map((item) => renderNavItem(item))}

            {/* 3. 动态扩展模块导航 */}
            {filteredExtensions.length > 0 && (
              <>
                {showExtDivider && <div className="sidebar__divider" role="separator" />}
                {filteredExtensions.map((route) => renderNavItem(route, resolveRouteLabel(route)))}
              </>
            )}

            {/* 分割线 2 */}
            {showInsightDivider && <div className="sidebar__divider" role="separator" />}

            {/* 4. 统计与关于 */}
            {filteredInsights.length > 0 && (
              <div className="sidebar__section-label">{t('sidebar.groupInsights')}</div>
            )}
            {filteredInsights.map((item) => renderNavItem(item))}

            {/* 5. 细粒度设置项与子 Tab 直达结果 */}
            {matchedDeepEnabled.length > 0 && (
              <div className="sidebar__deep-group">
                {allMatchedItems.length > 0 && <div className="sidebar__divider" role="separator" />}
                <div className="sidebar__deep-heading">{t('sidebar.deepSettingsGroup')}</div>
                {matchedDeepEnabled.map(({ entry, route }) => (
                  <button
                    key={entry.id}
                    type="button"
                    id={`deep-nav-${entry.id}`}
                    className="sidebar__deep-item"
                    onClick={() => handleDeepJump(entry)}
                  >
                    <CornerDownRight size={13} className="sidebar__deep-icon" aria-hidden="true" />
                    <span className="sidebar__deep-body">
                      <span className="sidebar__deep-title">{resolveDeepLabel(entry)}</span>
                      <span className="sidebar__deep-meta">{resolveRouteLabel(route)}</span>
                    </span>
                  </button>
                ))}
              </div>
            )}

            {/* 6. 未启用插件功能智能提示与一键启用跳转 */}
            {matchedInactiveRoutes.length > 0 && (
              <div className="sidebar__inactive-group">
                {(allMatchedItems.length > 0 || matchedDeepEnabled.length > 0) && (
                  <div className="sidebar__divider" role="separator" />
                )}
                <div className="sidebar__deep-heading">{t('sidebar.inactiveSectionTitle')}</div>
                {matchedInactiveRoutes.map(({ route, deepEntry }) => {
                  const IconComponent = route.icon;
                  const pluginId = route.requiresPlugin === 'dialog_enhancer'
                    ? 'dialogenhancer'
                    : route.requiresPlugin;
                  const isBusy = Boolean(pluginId && enablingPluginId === pluginId);
                  return (
                    <div key={route.id} className="sidebar__inactive-card">
                      <div className="sidebar__inactive-header">
                        <IconComponent size={15} aria-hidden="true" className="sidebar__inactive-icon" />
                        <span className="sidebar__inactive-title">{resolveRouteLabel(route)}</span>
                        <span className="sidebar__inactive-badge">{t('sidebar.inactiveBadge')}</span>
                      </div>
                      {deepEntry && (
                        <div className="sidebar__inactive-sub">{resolveDeepLabel(deepEntry)}</div>
                      )}
                      <button
                        type="button"
                        id={`inactive-enable-${route.id}`}
                        className="sidebar__inactive-enable-btn"
                        disabled={isBusy}
                        onClick={() => void handleEnableAndNavigate(route, deepEntry)}
                      >
                        <Power size={13} aria-hidden="true" />
                        <span>{t('sidebar.enableAndOpen')}</span>
                      </button>
                    </div>
                  );
                })}
              </div>
            )}
          </>
        )}
      </nav>
      {onOpenFeedback && (
        <div className="sidebar__footer">
          <button
            type="button"
            id="sidebar-feedback-btn"
            className="sidebar__feedback-btn"
            onClick={onOpenFeedback}
          >
            <MessageSquarePlus size={15} aria-hidden="true" />
            <span>{t('feedback.entryButton', 'Give Feedback')}</span>
          </button>
        </div>
      )}
    </aside>
  );
};



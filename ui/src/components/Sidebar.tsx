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
import { Search, X } from 'lucide-react';
import { CodeBadge } from './UIKit';
import './Sidebar.css';
import {
  APP_ROUTES,
  ROUTE_MAP,
  type RouteItemConfig,
  type NavId,
} from '../config/routes';

export type { NavId };

export interface SidebarProps {
  activeNav: NavId;
  onNavigate: (id: NavId) => void;
  activePlugins?: ReadonlySet<string>;
  installedExtensionIds?: string[];
}

const SIDEBAR_KEYWORD_KEYS: Partial<Record<NavId, string>> = {
  general: 'sidebar.kw_general',
  plugins: 'sidebar.kw_plugins',
  search: 'sidebar.kw_search',
  gesture: 'sidebar.kw_gesture',
  hotcorner: 'sidebar.kw_hotcorner',
  capture: 'sidebar.kw_capture',
  history: 'sidebar.kw_history',
  ocr: 'sidebar.kw_ocr',
  keycast: 'sidebar.kw_keycast',
  spotlight: 'sidebar.kw_spotlight',
  dialog_enhancer: 'sidebar.kw_dialog_enhancer',
  remote_boost: 'sidebar.kw_remote_boost',
  stats: 'sidebar.kw_stats',
  about: 'sidebar.kw_about',
};

export const Sidebar: FC<SidebarProps> = ({
  activeNav,
  onNavigate,
  activePlugins,
  installedExtensionIds = [],
}) => {
  const { t } = useTranslation();
  const [filterQuery, setFilterQuery] = useState('');
  const searchInputRef = useRef<HTMLInputElement>(null);

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

  const isItemAvailable = (item: RouteItemConfig) => {
    if (!item.requiresPlugin) return true;
    if (!activePlugins) return false;
    if (item.requiresPlugin === 'dialogenhancer' || item.requiresPlugin === 'dialog_enhancer') {
      return activePlugins.has('dialogenhancer') || activePlugins.has('dialog_enhancer');
    }
    return activePlugins.has(item.requiresPlugin);
  };

  const systemNavItems = APP_ROUTES.filter((r) => r.category === 'system');
  const availableCoreTools = APP_ROUTES.filter((r) => r.category === 'core_tools').filter(isItemAvailable);
  const insightNavItems = APP_ROUTES.filter((r) => r.category === 'insights');
  const extensionNavItems = useMemo(
    () =>
      installedExtensionIds
        .map((extId) => ROUTE_MAP.get(extId))
        .filter((route): route is RouteItemConfig => Boolean(route)),
    [installedExtensionIds],
  );

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
      String(t(item.labelKey as unknown as TemplateStringsArray)),
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
    if (e.key === 'Enter' && allMatchedItems.length > 0) {
      e.preventDefault();
      onNavigate(allMatchedItems[0].id);
    }
  };

  const renderNavItem = (item: RouteItemConfig) => {
    const unavailable = !isItemAvailable(item);
    const IconComponent = item.icon;
    return (
      <button
        key={item.id}
        id={`nav-${item.id}`}
        className={`sidebar__item ${activeNav === item.id ? 'sidebar__item--active' : ''}`}
        onClick={() => onNavigate(item.id)}
        disabled={unavailable}
        title={unavailable ? t('sidebar.pluginDisabled') : undefined}
        aria-current={activeNav === item.id ? 'page' : undefined}
      >
        <span className="sidebar__item-indicator" />
        <span className="sidebar__item-icon">
          <IconComponent size={20} strokeWidth={2.2} />
        </span>
        <span className="sidebar__item-label">{t(item.labelKey as unknown as TemplateStringsArray)}</span>
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
        {allMatchedItems.length === 0 ? (
          <div className="sidebar__search-empty" role="status">
            {t('sidebar.searchEmpty', 'No matching module')}
          </div>
        ) : (
          <>
            {/* 1. 系统总控组 */}
            {filteredSystem.map(renderNavItem)}

            {/* 分割线 1 */}
            {showCoreDivider && <div className="sidebar__divider" role="separator" />}

            {/* 2. 核心效率工具组 (仅展示当前已启用的功能) */}
            {filteredCoreTools.map(renderNavItem)}

            {/* 3. 动态扩展模块导航 */}
            {filteredExtensions.length > 0 && (
              <>
                {showExtDivider && <div className="sidebar__divider" role="separator" />}
                {filteredExtensions.map((route) => {
                  const IconComponent = route.icon;
                  return (
                    <button
                      key={route.id}
                      id={`nav-${route.id}`}
                      className={`sidebar__item ${activeNav === route.id ? 'sidebar__item--active' : ''}`}
                      onClick={() => onNavigate(route.id)}
                      aria-current={activeNav === route.id ? 'page' : undefined}
                    >
                      <span className="sidebar__item-indicator" />
                      <span className="sidebar__item-icon">
                        <IconComponent size={20} strokeWidth={2.2} />
                      </span>
                      <span className="sidebar__item-label">{t(route.labelKey as unknown as TemplateStringsArray)}</span>
                    </button>
                  );
                })}
              </>
            )}

            {/* 分割线 2 */}
            {showInsightDivider && <div className="sidebar__divider" role="separator" />}

            {/* 4. 统计与关于 */}
            {filteredInsights.map(renderNavItem)}
          </>
        )}
      </nav>
    </aside>
  );
};



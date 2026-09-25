/* ─────────────────────────────────────────────────────────────────────────────
 * Sidebar — 左侧图标导航栏
 *
 * 参考 Aitiy 设计：
 *   - 窄侧边栏 + 图标 + 文字标签
 *   - 当前选中项有紫色高亮指示条
 *   - 底部有版本信息和主题切换
 * ───────────────────────────────────────────────────────────────────────────── */

import { type FC } from 'react';
import { useTranslation } from 'react-i18next';
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

export const Sidebar: FC<SidebarProps> = ({
  activeNav,
  onNavigate,
  activePlugins,
  installedExtensionIds = [],
}) => {
  const { t } = useTranslation();

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

  return (
    <aside className="sidebar" role="navigation" aria-label={t('sidebar.mainNav')}>
      {/* ── 导航列表 ──────────────────────────────────────────────── */}
      <nav className="sidebar__nav">
        {/* 1. 系统总控组 */}
        {systemNavItems.map(renderNavItem)}

        {/* 分割线 1 (仅在有启用的核心工具时显示) */}
        {availableCoreTools.length > 0 && (
          <div className="sidebar__divider" role="separator" />
        )}

        {/* 2. 核心效率工具组 (仅展示当前已启用的功能) */}
        {availableCoreTools.map(renderNavItem)}

        {/* 3. 动态扩展模块导航 */}
        {installedExtensionIds.length > 0 && (
          <>
            <div className="sidebar__divider" role="separator" />
            {installedExtensionIds.map((extId) => {
              const route = ROUTE_MAP.get(extId);
              if (!route) return null;
              const IconComponent = route.icon;
              return (
                <button
                  key={extId}
                  id={`nav-${extId}`}
                  className={`sidebar__item ${activeNav === extId ? 'sidebar__item--active' : ''}`}
                  onClick={() => onNavigate(extId as NavId)}
                  aria-current={activeNav === extId ? 'page' : undefined}
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
        <div className="sidebar__divider" role="separator" />

        {/* 4. 统计与关于 */}
        {insightNavItems.map(renderNavItem)}
      </nav>
    </aside>
  );
};



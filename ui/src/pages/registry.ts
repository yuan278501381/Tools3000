/* ─────────────────────────────────────────────────────────────────────────────
 * registry.ts — 设置中心页面元数据与统一注册表
 *
 * 架构目标:
 *   1. 统一管理所有设置菜单项的 ID、标题、副标题与分类，解耦 Sidebar 与 App。
 *   2. 天然支持未来随时增减菜单页、重命名或由动态扩展插件注入新设置页。
 * ───────────────────────────────────────────────────────────────────────────── */

import {
  APP_ROUTES,
  getRouteMetadata,
  type BuiltinNavId,
  type NavId,
  type NavCategory,
  type RouteItemConfig,
} from '../config/routes';

export type { BuiltinNavId, NavId, NavCategory };

export interface PageDefinition {
  id: NavId;
  titleKey: string;
  subtitleKey: string;
  category: NavCategory;
  requiresPlugin?: RouteItemConfig['requiresPlugin'];
}

/**
 * 统一页面定义列表：从单一事实源 routes.ts 投影
 */
export const PAGE_DEFINITIONS: PageDefinition[] = APP_ROUTES.map((route) => ({
  id: route.id,
  titleKey: route.labelKey,
  subtitleKey: route.subtitleKey,
  category: route.category,
  requiresPlugin: route.requiresPlugin,
}));

/**
 * 获取页面的国际化标题与副标题 key
 * 具备健壮的兜底能力：即使遇到未来新增但未在此注册的动态扩展页，也能平滑解析为 nav.<id>
 */
export const getPageMetadata = getRouteMetadata;


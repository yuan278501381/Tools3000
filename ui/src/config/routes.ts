/* ─────────────────────────────────────────────────────────────────────────────
 * routes.ts — 统一路由与导航元数据单一事实源 (SSOT)
 *
 * 架构规范:
 *   1. 单一事实源: 统一声明所有页面的 ID、分类、国际化键、图标与插件前置依赖。
 *   2. 杜绝反向依赖: Sidebar 与各页面视图仅单向消费此配置，杜绝多头维护。
 * ───────────────────────────────────────────────────────────────────────────── */

import {
  Settings,
  Boxes,
  Search,
  Mouse,
  MonitorUp,
  Camera,
  History,
  FileText,
  BarChart3,
  Info,
  FolderSymlink,
  Sparkles,
  Keyboard,
  Cast,
  Puzzle,
  type LucideIcon,
} from 'lucide-react';

export type BuiltinNavId =
  | 'general'
  | 'plugins'
  | 'search'
  | 'gesture'
  | 'hotcorner'
  | 'capture'
  | 'history'
  | 'ocr'
  | 'keycast'
  | 'spotlight'
  | 'dialog_enhancer'
  | 'remote_boost'
  | 'stats'
  | 'about';

export type NavId = BuiltinNavId | (string & {});

export type NavCategory = 'system' | 'core_tools' | 'insights' | 'extension';

export interface RouteItemConfig {
  id: NavId;
  labelKey: string;      // 对应导航与页面主标题国际化 key
  subtitleKey: string;   // 对应副标题国际化 key
  category: NavCategory;
  icon: LucideIcon;
  toneStart: string;     // 精工 Squircle 模块徽标渐变起始色 (Apple System Settings 级宝石色盘)
  toneEnd: string;       // 精工 Squircle 模块徽标渐变终止色
  fallbackLabel?: string;
  requiresPlugin?: 'gesture' | 'capture' | 'search' | 'dialogenhancer' | 'dialog_enhancer' | 'keycast' | 'spotlight' | 'remote_boost';
}

export const APP_ROUTES: readonly RouteItemConfig[] = [
  // ── 系统设置 ──────────────────────────────────────────────────────────────
  { id: 'general', labelKey: 'nav.settings', subtitleKey: 'navSubtitle.general', category: 'system', icon: Settings, toneStart: '#64748b', toneEnd: '#475569' },
  { id: 'plugins', labelKey: 'nav.plugins', subtitleKey: 'navSubtitle.plugins', category: 'system', icon: Boxes, toneStart: '#6366f1', toneEnd: '#4f46e5' },

  // ── 核心效率工具 ──────────────────────────────────────────────────────────
  { id: 'search', labelKey: 'nav.search', subtitleKey: 'navSubtitle.search', category: 'core_tools', icon: Search, toneStart: '#0ea5e9', toneEnd: '#0284c7', requiresPlugin: 'search' },
  { id: 'gesture', labelKey: 'nav.gesture', subtitleKey: 'navSubtitle.gesture', category: 'core_tools', icon: Mouse, toneStart: '#8b5cf6', toneEnd: '#7c3aed', requiresPlugin: 'gesture' },
  { id: 'hotcorner', labelKey: 'nav.hotcorner', subtitleKey: 'navSubtitle.hotcorner', category: 'core_tools', icon: MonitorUp, toneStart: '#f59e0b', toneEnd: '#d97706', requiresPlugin: 'gesture' },
  { id: 'capture', labelKey: 'nav.capture', subtitleKey: 'navSubtitle.capture', category: 'core_tools', icon: Camera, toneStart: '#f43f5e', toneEnd: '#e11d48', requiresPlugin: 'capture' },
  { id: 'history', labelKey: 'nav.history', subtitleKey: 'navSubtitle.history', category: 'core_tools', icon: History, toneStart: '#06b6d4', toneEnd: '#0891b2', requiresPlugin: 'capture' },
  { id: 'ocr', labelKey: 'nav.ocr', subtitleKey: 'navSubtitle.ocr', category: 'core_tools', icon: FileText, toneStart: '#10b981', toneEnd: '#059669', requiresPlugin: 'capture' },
  { id: 'keycast', labelKey: 'nav.keycast', subtitleKey: 'navSubtitle.keycast', category: 'core_tools', icon: Keyboard, toneStart: '#eab308', toneEnd: '#ca8a04', requiresPlugin: 'keycast' },
  { id: 'spotlight', labelKey: 'nav.spotlight', subtitleKey: 'navSubtitle.spotlight', category: 'core_tools', icon: Sparkles, toneStart: '#ec4899', toneEnd: '#db2777', requiresPlugin: 'spotlight' },
  { id: 'dialog_enhancer', labelKey: 'nav.dialog_enhancer', subtitleKey: 'navSubtitle.dialog_enhancer', category: 'core_tools', icon: FolderSymlink, toneStart: '#3b82f6', toneEnd: '#2563eb', requiresPlugin: 'dialogenhancer' },
  { id: 'remote_boost', labelKey: 'nav.remote_boost', subtitleKey: 'navSubtitle.remote_boost', category: 'core_tools', icon: Cast, toneStart: '#14b8a6', toneEnd: '#0d9488', requiresPlugin: 'remote_boost' },

  // ── 洞察与关于 ────────────────────────────────────────────────────────────
  { id: 'stats', labelKey: 'nav.stats', subtitleKey: 'navSubtitle.stats', category: 'insights', icon: BarChart3, toneStart: '#22c55e', toneEnd: '#16a34a' },
  { id: 'about', labelKey: 'nav.about', subtitleKey: 'navSubtitle.about', category: 'insights', icon: Info, toneStart: '#6366f1', toneEnd: '#8b5cf6' },
];

export const ROUTE_MAP = new Map<string, RouteItemConfig>(APP_ROUTES.map(r => [r.id, r]));

export function resolveExtensionRoute(id: string, fallbackLabel?: string): RouteItemConfig {
  const builtin = ROUTE_MAP.get(id);
  if (builtin) {
    return fallbackLabel && !builtin.fallbackLabel ? { ...builtin, fallbackLabel } : builtin;
  }
  return {
    id,
    labelKey: `nav.${id}`,
    subtitleKey: `navSubtitle.${id}`,
    category: 'extension',
    icon: Puzzle,
    toneStart: '#8b5cf6',
    toneEnd: '#6366f1',
    fallbackLabel: fallbackLabel || id,
  };
}

export function getRouteMetadata(id: string): { titleKey: string; subtitleKey: string } {
  const item = ROUTE_MAP.get(id);
  if (item) {
    return { titleKey: item.labelKey, subtitleKey: item.subtitleKey };
  }
  return {
    titleKey: `nav.${id}`,
    subtitleKey: `navSubtitle.${id}`,
  };
}


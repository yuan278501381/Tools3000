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
  Bot,
  Pipette,
  ClipboardList,
  FileCode2,
  FolderSymlink,
  Sparkles,
  Keyboard,
  Cast,
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
  | 'about'
  | 'ai_assistant'
  | 'color_picker'
  | 'clipboard_manager'
  | 'markdown_preview';

export type NavId = BuiltinNavId | (string & {});

export type NavCategory = 'system' | 'core_tools' | 'insights' | 'extension';

export interface RouteItemConfig {
  id: NavId;
  labelKey: string;      // 对应导航与页面主标题国际化 key
  subtitleKey: string;   // 对应副标题国际化 key
  category: NavCategory;
  icon: LucideIcon;
  requiresPlugin?: 'gesture' | 'capture' | 'search' | 'dialogenhancer' | 'dialog_enhancer' | 'keycast' | 'spotlight' | 'remote_boost';
}

export const APP_ROUTES: readonly RouteItemConfig[] = [
  // ── 系统设置 ──────────────────────────────────────────────────────────────
  { id: 'general', labelKey: 'nav.settings', subtitleKey: 'navSubtitle.general', category: 'system', icon: Settings },
  { id: 'plugins', labelKey: 'nav.plugins', subtitleKey: 'navSubtitle.plugins', category: 'system', icon: Boxes },

  // ── 核心效率工具 ──────────────────────────────────────────────────────────
  { id: 'search', labelKey: 'nav.search', subtitleKey: 'navSubtitle.search', category: 'core_tools', icon: Search, requiresPlugin: 'search' },
  { id: 'gesture', labelKey: 'nav.gesture', subtitleKey: 'navSubtitle.gesture', category: 'core_tools', icon: Mouse, requiresPlugin: 'gesture' },
  { id: 'hotcorner', labelKey: 'nav.hotcorner', subtitleKey: 'navSubtitle.hotcorner', category: 'core_tools', icon: MonitorUp, requiresPlugin: 'gesture' },
  { id: 'capture', labelKey: 'nav.capture', subtitleKey: 'navSubtitle.capture', category: 'core_tools', icon: Camera, requiresPlugin: 'capture' },
  { id: 'history', labelKey: 'nav.history', subtitleKey: 'navSubtitle.history', category: 'core_tools', icon: History, requiresPlugin: 'capture' },
  { id: 'ocr', labelKey: 'nav.ocr', subtitleKey: 'navSubtitle.ocr', category: 'core_tools', icon: FileText, requiresPlugin: 'capture' },
  { id: 'keycast', labelKey: 'nav.keycast', subtitleKey: 'navSubtitle.keycast', category: 'core_tools', icon: Keyboard, requiresPlugin: 'keycast' },
  { id: 'spotlight', labelKey: 'nav.spotlight', subtitleKey: 'navSubtitle.spotlight', category: 'core_tools', icon: Sparkles, requiresPlugin: 'spotlight' },
  { id: 'dialog_enhancer', labelKey: 'nav.dialog_enhancer', subtitleKey: 'navSubtitle.dialog_enhancer', category: 'core_tools', icon: FolderSymlink, requiresPlugin: 'dialogenhancer' },
  { id: 'remote_boost', labelKey: 'nav.remote_boost', subtitleKey: 'navSubtitle.remote_boost', category: 'core_tools', icon: Cast, requiresPlugin: 'remote_boost' },

  // ── 洞察与关于 ────────────────────────────────────────────────────────────
  { id: 'stats', labelKey: 'nav.stats', subtitleKey: 'navSubtitle.stats', category: 'insights', icon: BarChart3 },
  { id: 'about', labelKey: 'nav.about', subtitleKey: 'navSubtitle.about', category: 'insights', icon: Info },

  // ── 扩展应用 ──────────────────────────────────────────────────────────────
  { id: 'ai_assistant', labelKey: 'nav.ai_assistant', subtitleKey: 'navSubtitle.ai_assistant', category: 'extension', icon: Bot },
  { id: 'color_picker', labelKey: 'nav.color_picker', subtitleKey: 'navSubtitle.color_picker', category: 'extension', icon: Pipette },
  { id: 'clipboard_manager', labelKey: 'nav.clipboard_manager', subtitleKey: 'navSubtitle.clipboard_manager', category: 'extension', icon: ClipboardList },
  { id: 'markdown_preview', labelKey: 'nav.markdown_preview', subtitleKey: 'navSubtitle.markdown_preview', category: 'extension', icon: FileCode2 },
];

export const ROUTE_MAP = new Map<string, RouteItemConfig>(APP_ROUTES.map(r => [r.id, r]));

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


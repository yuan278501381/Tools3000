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
  MemoryStick,
  Info,
  FolderSymlink,
  Sparkles,
  Keyboard,
  Cast,
  Image as ImageIcon,
  ClipboardList,
  TextCursorInput,
  LockOpen,
  Puzzle,
  type LucideIcon,
} from 'lucide-react';

export type BuiltinNavId =
  | 'general'
  | 'plugins'
  | 'search'
  | 'clipboard'
  | 'text_expander'
  | 'gesture'
  | 'hotcorner'
  | 'capture'
  | 'history'
  | 'ocr'
  | 'keycast'
  | 'spotlight'
  | 'dialog_enhancer'
  | 'file_locksmith'
  | 'remote_boost'
  | 'picasa_viewer'
  | 'stats'
  | 'performance'
  | 'about';

export type NavId = BuiltinNavId | (string & {});

export type NavCategory = 'system' | 'core_tools' | 'insights' | 'extension';

export type PageCanvasWidth = 'reading' | 'workspace';

export interface RouteItemConfig {
  id: NavId;
  labelKey: string;      // 对应导航与页面主标题国际化 key
  subtitleKey: string;   // 对应副标题国际化 key
  category: NavCategory;
  icon: LucideIcon;
  toneStart: string;     // 精工 Squircle 模块徽标渐变起始色 (Apple System Settings 级宝石色盘)
  toneEnd: string;       // 精工 Squircle 模块徽标渐变终止色
  canvasWidth?: PageCanvasWidth; // 内容画布语义天花板: reading (960px) / workspace (1280px)
  fallbackLabel?: string;
  requiresPlugin?: 'gesture' | 'capture' | 'search' | 'clipboard' | 'text_expander' | 'dialogenhancer' | 'dialog_enhancer' | 'file_locksmith' | 'keycast' | 'spotlight' | 'remote_boost' | 'picasa_viewer';
}

export const APP_ROUTES: readonly RouteItemConfig[] = [
  // ── 系统设置 ──────────────────────────────────────────────────────────────
  { id: 'general', labelKey: 'nav.settings', subtitleKey: 'navSubtitle.general', category: 'system', icon: Settings, toneStart: '#64748b', toneEnd: '#475569', canvasWidth: 'reading' },
  { id: 'plugins', labelKey: 'nav.plugins', subtitleKey: 'navSubtitle.plugins', category: 'system', icon: Boxes, toneStart: '#6366f1', toneEnd: '#4f46e5', canvasWidth: 'reading' },

  // ── 核心效率工具（MECE 4 域语义成对单一事实源：侧边栏 / 功能模块 / 托盘 100% 同序） ──
  // 1. 高频捕获与剪贴流转
  { id: 'capture', labelKey: 'nav.capture', subtitleKey: 'navSubtitle.capture', category: 'core_tools', icon: Camera, toneStart: '#f43f5e', toneEnd: '#e11d48', canvasWidth: 'workspace', requiresPlugin: 'capture' },
  { id: 'history', labelKey: 'nav.history', subtitleKey: 'navSubtitle.history', category: 'core_tools', icon: History, toneStart: '#06b6d4', toneEnd: '#0891b2', canvasWidth: 'workspace', requiresPlugin: 'capture' },
  { id: 'ocr', labelKey: 'nav.ocr', subtitleKey: 'navSubtitle.ocr', category: 'core_tools', icon: FileText, toneStart: '#10b981', toneEnd: '#059669', canvasWidth: 'reading', requiresPlugin: 'capture' },
  { id: 'clipboard', labelKey: 'nav.clipboard', subtitleKey: 'navSubtitle.clipboard', category: 'core_tools', icon: ClipboardList, toneStart: '#0ea5e9', toneEnd: '#6366f1', canvasWidth: 'reading', requiresPlugin: 'clipboard' },
  // 2. 键鼠操控与输入提效
  { id: 'gesture', labelKey: 'nav.gesture', subtitleKey: 'navSubtitle.gesture', category: 'core_tools', icon: Mouse, toneStart: '#8b5cf6', toneEnd: '#7c3aed', canvasWidth: 'workspace', requiresPlugin: 'gesture' },
  { id: 'hotcorner', labelKey: 'nav.hotcorner', subtitleKey: 'navSubtitle.hotcorner', category: 'core_tools', icon: MonitorUp, toneStart: '#f59e0b', toneEnd: '#d97706', canvasWidth: 'reading', requiresPlugin: 'gesture' },
  { id: 'text_expander', labelKey: 'nav.text_expander', subtitleKey: 'navSubtitle.text_expander', category: 'core_tools', icon: TextCursorInput, toneStart: '#6366f1', toneEnd: '#8b5cf6', canvasWidth: 'reading', requiresPlugin: 'text_expander' },
  // 3. 文件检索、流转与零驻留系统扩展
  { id: 'search', labelKey: 'nav.search', subtitleKey: 'navSubtitle.search', category: 'core_tools', icon: Search, toneStart: '#0ea5e9', toneEnd: '#0284c7', canvasWidth: 'workspace', requiresPlugin: 'search' },
  { id: 'dialog_enhancer', labelKey: 'nav.dialog_enhancer', subtitleKey: 'navSubtitle.dialog_enhancer', category: 'core_tools', icon: FolderSymlink, toneStart: '#3b82f6', toneEnd: '#2563eb', canvasWidth: 'reading', requiresPlugin: 'dialogenhancer' },
  { id: 'file_locksmith', labelKey: 'nav.file_locksmith', subtitleKey: 'navSubtitle.file_locksmith', category: 'core_tools', icon: LockOpen, toneStart: '#f59e0b', toneEnd: '#ea580c', canvasWidth: 'reading', requiresPlugin: 'file_locksmith' },
  { id: 'picasa_viewer', labelKey: 'nav.picasa_viewer', subtitleKey: 'navSubtitle.picasa_viewer', category: 'core_tools', icon: ImageIcon, toneStart: '#f97316', toneEnd: '#ea580c', canvasWidth: 'reading', requiresPlugin: 'picasa_viewer' },
  // 4. 演示教学与远程协助专享
  { id: 'keycast', labelKey: 'nav.keycast', subtitleKey: 'navSubtitle.keycast', category: 'core_tools', icon: Keyboard, toneStart: '#eab308', toneEnd: '#ca8a04', canvasWidth: 'reading', requiresPlugin: 'keycast' },
  { id: 'spotlight', labelKey: 'nav.spotlight', subtitleKey: 'navSubtitle.spotlight', category: 'core_tools', icon: Sparkles, toneStart: '#ec4899', toneEnd: '#db2777', canvasWidth: 'reading', requiresPlugin: 'spotlight' },
  { id: 'remote_boost', labelKey: 'nav.remote_boost', subtitleKey: 'navSubtitle.remote_boost', category: 'core_tools', icon: Cast, toneStart: '#14b8a6', toneEnd: '#0d9488', canvasWidth: 'reading', requiresPlugin: 'remote_boost' },

  // ── 洞察与关于 ────────────────────────────────────────────────────────────
  { id: 'stats', labelKey: 'nav.stats', subtitleKey: 'navSubtitle.stats', category: 'insights', icon: BarChart3, toneStart: '#22c55e', toneEnd: '#16a34a', canvasWidth: 'workspace' },
  { id: 'performance', labelKey: 'nav.performance', subtitleKey: 'navSubtitle.performance', category: 'insights', icon: MemoryStick, toneStart: '#06b6d4', toneEnd: '#3b82f6', canvasWidth: 'reading' },
  { id: 'about', labelKey: 'nav.about', subtitleKey: 'navSubtitle.about', category: 'insights', icon: Info, toneStart: '#6366f1', toneEnd: '#8b5cf6', canvasWidth: 'reading' },
];

export type PluginDomainGroupId = 'capture_flow' | 'input_control' | 'file_system' | 'present_remote';

export interface PluginDomainGroupMeta {
  id: PluginDomainGroupId;
  titleKey: string;
  pluginIds: readonly string[];
}

/**
 * 功能模块页四大正交业务域分组单一事实源（与 APP_ROUTES 顺序严格一致）
 */
export const PLUGIN_DOMAIN_GROUPS: readonly PluginDomainGroupMeta[] = [
  {
    id: 'capture_flow',
    titleKey: 'plugins.domainCaptureFlow',
    pluginIds: ['capture', 'clipboard'],
  },
  {
    id: 'input_control',
    titleKey: 'plugins.domainInputControl',
    pluginIds: ['gesture', 'text_expander'],
  },
  {
    id: 'file_system',
    titleKey: 'plugins.domainFileSystem',
    pluginIds: ['search', 'dialog_enhancer', 'file_locksmith', 'picasa_viewer', 'quick_look'],
  },
  {
    id: 'present_remote',
    titleKey: 'plugins.domainPresentRemote',
    pluginIds: ['keycast', 'spotlight', 'remote_boost'],
  },
];

export const ROUTE_MAP = new Map<string, RouteItemConfig>(APP_ROUTES.map(r => [r.id, r]));

const BUILTIN_PLUGIN_ALIAS_TO_CANONICAL: Readonly<Record<string, string>> = {
  dialogenhancer: 'dialog_enhancer',
  dialog_enhancer: 'dialog_enhancer',
  filelocksmith: 'file_locksmith',
  file_locksmith: 'file_locksmith',
  textexpander: 'text_expander',
  text_expander: 'text_expander',
  remote: 'remote_boost',
  remote_boost: 'remote_boost',
  picasaviewer: 'picasa_viewer',
  picasa_viewer: 'picasa_viewer',
  quicklook: 'quick_look',
  quick_look: 'quick_look',
};

const BUILTIN_PLUGIN_ALIAS_GROUPS: Readonly<Record<string, readonly string[]>> = {
  dialog_enhancer: ['dialogenhancer', 'dialog_enhancer'],
  file_locksmith: ['file_locksmith', 'filelocksmith'],
  text_expander: ['text_expander', 'textexpander'],
  remote_boost: ['remote_boost', 'remote'],
  picasa_viewer: ['picasa_viewer', 'picasaviewer'],
  quick_look: ['quick_look', 'quicklook'],
};

export function normalizeBuiltinPluginId(id: string): string {
  const lowered = id.trim().toLowerCase();
  return BUILTIN_PLUGIN_ALIAS_TO_CANONICAL[lowered] ?? lowered;
}

export function getBuiltinPluginAliases(id: string): readonly string[] {
  const canonical = normalizeBuiltinPluginId(id);
  return BUILTIN_PLUGIN_ALIAS_GROUPS[canonical] ?? [canonical];
}

export function isPluginActiveInSet(
  activePlugins: ReadonlySet<string> | undefined,
  pluginId: string | undefined,
): boolean {
  if (!pluginId) return true;
  if (!activePlugins) return false;
  if (activePlugins.has(pluginId)) return true;
  const aliases = getBuiltinPluginAliases(pluginId);
  return aliases.some((alias) => activePlugins.has(alias));
}

const builtinPluginOrderList: string[] = [];
const builtinPluginPrimaryRouteMap = new Map<string, NavId>();
for (const route of APP_ROUTES) {
  if (route.category !== 'core_tools' || !route.requiresPlugin) continue;
  const canonical = normalizeBuiltinPluginId(route.requiresPlugin);
  if (!builtinPluginPrimaryRouteMap.has(canonical)) {
    builtinPluginOrderList.push(canonical);
    builtinPluginPrimaryRouteMap.set(canonical, route.id);
  }
}

/**
 * 内置功能模块标准展示序列（SSOT：自动按 APP_ROUTES 中 core_tools 首次出现顺序去重派生）。
 * 保障左侧导航「效率工具」与右侧「功能模块」矩阵在全开及任意子集启停下相对顺序 100% 一致。
 */
export const BUILTIN_PLUGIN_ORDER: readonly string[] = builtinPluginOrderList;

const BUILTIN_PLUGIN_ORDER_INDEX = new Map<string, number>(
  BUILTIN_PLUGIN_ORDER.map((id, idx) => [id, idx]),
);

export function getBuiltinPluginDisplayOrder(id: string): number {
  const canonical = normalizeBuiltinPluginId(id);
  return BUILTIN_PLUGIN_ORDER_INDEX.get(canonical) ?? Number.MAX_SAFE_INTEGER;
}

export function getBuiltinPluginPrimaryRoute(id: string): NavId | undefined {
  const canonical = normalizeBuiltinPluginId(id);
  return builtinPluginPrimaryRouteMap.get(canonical);
}

/**
 * 获取由同一底层插件驱动的附属子导航路由（如 capture -> ['history', 'ocr']，gesture -> ['hotcorner']）
 */
export function getBuiltinPluginSecondaryRoutes(id: string): readonly RouteItemConfig[] {
  const canonical = normalizeBuiltinPluginId(id);
  const primary = builtinPluginPrimaryRouteMap.get(canonical);
  return APP_ROUTES.filter(
    (r) =>
      r.category === 'core_tools' &&
      r.requiresPlugin &&
      normalizeBuiltinPluginId(r.requiresPlugin) === canonical &&
      r.id !== primary,
  );
}

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


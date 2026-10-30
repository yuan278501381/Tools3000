/* ─────────────────────────────────────────────────────────────────────────────
 * trayRegistry.ts — 托盘快捷开关数据驱动注册中心 (World-Class Bento Grid)
 * ───────────────────────────────────────────────────────────────────────────── */

import type { ComponentType } from 'react';
import {
  Mouse,
  Keyboard,
  Camera,
  Search,
  Sparkles,
  FolderSymlink,
  Cast,
} from 'lucide-react';

export interface TrayExtraState {
  gesturePaused: boolean;
  remoteBoostEnabled?: boolean;
}

/**
 * 将冗长的系统键名压缩为紧凑优雅的桌面快捷键徽章格式
 */
export function formatCompactShortcut(raw?: string): string {
  if (!raw) return '';
  const trimmed = raw.trim();
  if (!trimmed) return '';
  return trimmed
    .split('+')
    .map((part) => {
      const token = part.trim();
      const lower = token.toLowerCase();
      if (lower === 'control') return 'Ctrl';
      if (lower === 'backspace') return 'Bksp';
      if (lower === 'delete') return 'Del';
      if (lower === 'escape') return 'Esc';
      if (lower === 'printscreen' || lower === 'snapshot') return 'PrtSc';
      if (lower === 'pageup') return 'PgUp';
      if (lower === 'pagedown') return 'PgDn';
      return token;
    })
    .join('+');
}

export interface TrayControlItem {
  /** 唯一功能键名 */
  id: string;
  /** 关联的主插件/模块 ID */
  pluginId: string;
  /** 兼容备用插件 ID (例如 dialog_enhancer / dialogenhancer) */
  aliasPluginIds?: string[];
  /** i18n 极简标签键 */
  labelKey: string;
  /** 默认英文回退标签 */
  fallbackLabel: string;
  /** 可选宽幅卡片副标题键 (用于跨两列的旗舰控制项) */
  subKey?: string;
  /** 可选宽幅卡片英文回退副标题 */
  fallbackSub?: string;
  /** i18n 详尽功能描述键 (用于悬浮气泡 Tooltip 与无障碍标签) */
  descKey: string;
  /** 默认详尽功能描述 */
  fallbackDesc: string;
  /** 统一 Lucide 矢量 SVG 图标组件 */
  icon: ComponentType<{ size?: number; className?: string }>;
  /** 显示优先级排序 */
  order: number;
  /** 是否横跨两列并展示拨动滑块 */
  wide?: boolean;
  /** 自定义活跃判定 (例如手势区分 paused) */
  getCustomActive?: (activePlugins: Set<string>, extraState: TrayExtraState) => boolean;
}

/**
 * 托盘快捷功能开关注册表
 * 未来如需增减模块，只需在此处配置，全链路自适应生效
 */
export const TRAY_CONTROL_REGISTRY: TrayControlItem[] = [
  {
    id: 'gesture',
    pluginId: 'gesture',
    labelKey: 'tray.pillGesture',
    fallbackLabel: 'Gestures',
    descKey: 'tray.pillGestureFullDesc',
    fallbackDesc: 'Mouse Gestures (Global gestures & hot corners)',
    icon: Mouse,
    order: 10,
    getCustomActive: (activePlugins, { gesturePaused }) => activePlugins.has('gesture') && !gesturePaused,
  },
  {
    id: 'keycast',
    pluginId: 'keycast',
    labelKey: 'tray.pillKeycast',
    fallbackLabel: 'Keycast',
    descKey: 'tray.pillKeycastFullDesc',
    fallbackDesc: 'Keycast (Real-time keystrokes on screen)',
    icon: Keyboard,
    order: 20,
  },
  {
    id: 'capture',
    pluginId: 'capture',
    labelKey: 'tray.pillCapture',
    fallbackLabel: 'Capture',
    descKey: 'tray.pillCaptureFullDesc',
    fallbackDesc: 'Capture (Screenshots, stitching & screen recording)',
    icon: Camera,
    order: 30,
  },
  {
    id: 'search',
    pluginId: 'search',
    labelKey: 'tray.pillSearch',
    fallbackLabel: 'Search',
    descKey: 'tray.pillSearchFullDesc',
    fallbackDesc: 'File Search (Instant NTFS file search)',
    icon: Search,
    order: 40,
  },
  {
    id: 'spotlight',
    pluginId: 'spotlight',
    labelKey: 'tray.pillSpotlight',
    fallbackLabel: 'Effects',
    descKey: 'tray.pillSpotlightFullDesc',
    fallbackDesc: 'Mouse Effects (Spotlight, ripples & motion trails)',
    icon: Sparkles,
    order: 50,
  },
  {
    id: 'dialog_enhancer',
    pluginId: 'dialogenhancer',
    aliasPluginIds: ['dialog_enhancer', 'dialogenhancer'],
    labelKey: 'tray.pillDialog',
    fallbackLabel: 'Dialog',
    descKey: 'tray.pillDialogFullDesc',
    fallbackDesc: 'Dialog Enhancer (Instant jump & recent paths in dialogs)',
    icon: FolderSymlink,
    order: 60,
  },
  {
    id: 'remote_boost',
    pluginId: 'remote_boost',
    labelKey: 'tray.pillRemoteBoost',
    fallbackLabel: 'Remote Boost',
    subKey: 'tray.pillRemoteBoostSub',
    fallbackSub: 'Hotkey passthrough',
    descKey: 'tray.pillRemoteBoostFullDesc',
    fallbackDesc: 'Remote Boost (Hotkey passthrough & stuck-key release)',
    icon: Cast,
    order: 70,
    wide: true,
    getCustomActive: (_activePlugins, { remoteBoostEnabled }) => Boolean(remoteBoostEnabled),
  },
];

export interface TrayPinGroupPresetColor {
  slot: number;
  hex: string;
  name: string;
}

export const DEFAULT_PIN_GROUP_PRESET_COLORS: TrayPinGroupPresetColor[] = [
  { slot: 0, hex: '#0EA5E9', name: 'Sapphire' },
  { slot: 1, hex: '#10B981', name: 'Emerald' },
  { slot: 2, hex: '#F59E0B', name: 'Amber' },
  { slot: 3, hex: '#EC4899', name: 'Rose' },
  { slot: 4, hex: '#8B5CF6', name: 'Violet' },
  { slot: 5, hex: '#06B6D4', name: 'Cyan' },
  { slot: 6, hex: '#F97316', name: 'Coral' },
  { slot: 7, hex: '#84CC16', name: 'Lime' },
  { slot: 8, hex: '#6366F1', name: 'Indigo' },
  { slot: 9, hex: '#EF4444', name: 'Crimson' },
  { slot: 10, hex: '#14B8A6', name: 'Teal' },
  { slot: 11, hex: '#D946EF', name: 'Magenta' },
];

export interface TrayPinGroupEntry {
  id: number;
  count: number;
  explicitCreated?: boolean;
  hasCustomization?: boolean;
  shortLabel: string;
  fullLabel: string;
  customName?: string;
  customColorHex?: string;
  defaultColorHex?: string;
  colorHex: string;
}

export interface TrayPinGroupState {
  totalCount: number;
  ungroupedCount: number;
  activeFilter: number;
  allHidden: boolean;
  maxGroupId: number;
  nextAvailableGroupId: number;
  groups: TrayPinGroupEntry[];
  catalog?: TrayPinGroupEntry[];
  palettePresets?: TrayPinGroupPresetColor[];
}

export const DEFAULT_TRAY_PIN_GROUP_STATE: TrayPinGroupState = {
  totalCount: 0,
  ungroupedCount: 0,
  activeFilter: -1,
  allHidden: false,
  maxGroupId: 32,
  nextAvailableGroupId: 1,
  groups: [],
  catalog: [],
  palettePresets: DEFAULT_PIN_GROUP_PRESET_COLORS,
};

export interface PinGroupMetaUpdatePayload {
  id: number;
  name?: string;
  colorHex?: string;
  ensureCreated?: boolean;
}

export interface PinGroupControlPayload {
  createNewGroup?: boolean;
  autoGroupUngrouped?: boolean;
  filterGroup?: number;
  arrangeGroup?: number;
  dissolveGroup?: number;
  closeGroup?: number;
  arrangeAll?: boolean;
  toggleHideAll?: boolean;
  closeAll?: boolean;
  updateGroupMeta?: PinGroupMetaUpdatePayload;
  renameGroup?: { id: number; name: string };
  recolorGroup?: { id: number; colorHex: string };
  resetGroupMeta?: number;
  resetAllGroupMeta?: boolean;
}

export function normalizePinGroupState(raw: unknown): TrayPinGroupState {
  if (!raw || typeof raw !== 'object') {
    return DEFAULT_TRAY_PIN_GROUP_STATE;
  }
  const obj = raw as Partial<TrayPinGroupState>;
  return {
    totalCount: typeof obj.totalCount === 'number' ? obj.totalCount : 0,
    ungroupedCount: typeof obj.ungroupedCount === 'number' ? obj.ungroupedCount : 0,
    activeFilter: typeof obj.activeFilter === 'number' ? obj.activeFilter : -1,
    allHidden: Boolean(obj.allHidden),
    maxGroupId: typeof obj.maxGroupId === 'number' ? obj.maxGroupId : 32,
    nextAvailableGroupId: typeof obj.nextAvailableGroupId === 'number' ? obj.nextAvailableGroupId : 1,
    groups: Array.isArray(obj.groups) ? obj.groups : [],
    catalog: Array.isArray(obj.catalog) ? obj.catalog : undefined,
    palettePresets:
      Array.isArray(obj.palettePresets) && obj.palettePresets.length > 0
        ? obj.palettePresets
        : DEFAULT_PIN_GROUP_PRESET_COLORS,
  };
}

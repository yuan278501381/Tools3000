/* ─────────────────────────────────────────────────────────────────────────────
 * windowManager.ts — 窗口管理模块前后端 IPC 契约单一事实源 (SSOT)
 * ───────────────────────────────────────────────────────────────────────────── */

export type SnapTriggerMode = 'shift' | 'right_click' | 'edge';
export type QuickSwitchDensity = 'compact' | 'comfortable';
export type AltDragModifier = 'alt' | 'ctrl_alt';
export type SnapZoneLayoutType = 'grid' | 'canvas';

export type WindowManagerActionName =
  | 'toggle_always_on_top'
  | 'rescue_offscreen'
  | 'restore_all_tray'
  | 'open_quick_switch'
  | 'launch_workspace'
  | 'move_next_monitor'
  | 'move_next_zone'
  | 'opacity_reset'
  | 'show_zones_preview'
  | 'open_layout_editor';

export interface WindowManagerRuntimeStats {
  running: boolean;
  pid: number;
  workingSetBytes: number;
  privateBytes: number;
  handleCount: number;
  gdiObjects: number;
  userObjects: number;
  pinnedCount: number;
  trayMinimizedCount: number;
  activeFeatures: string[];
}

export interface SnapZoneCanvasRect {
  id: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface SnapZoneLayout {
  id: string;
  name: string;
  type: SnapZoneLayoutType;
  builtin?: boolean;
  rows?: number;
  columns?: number;
  rowPercents?: number[];
  colPercents?: number[];
  cellMap?: number[][];
  zones?: SnapZoneCanvasRect[];
}

export interface WorkspaceAppEntry {
  exePath: string;
  processName: string;
  args: string;
  monitorId: string;
  zoneIndex?: number;
  rect?: { x: number; y: number; w: number; h: number };
  maximized?: boolean;
}

export interface WorkspaceItem {
  id: string;
  name: string;
  apps: WorkspaceAppEntry[];
}

export interface WindowManagerFeatures {
  snapZones: boolean;
  workspaces: boolean;
  quickSwitch: boolean;
  alwaysOnTop: boolean;
  opacity: boolean;
  altDrag: boolean;
  focusBorder: boolean;
  multiMonitor: boolean;
}

export interface WindowManagerSettings {
  features: WindowManagerFeatures;
  snapZones: {
    triggerMode: SnapTriggerMode;
    spanMultipleZones: boolean;
    showZoneNumbers: boolean;
    zoneGap: number;
    overlayOpacity: number;
    rememberLastZone: boolean;
    perVirtualDesktop: boolean;
    excludedApps: string[];
    layouts: SnapZoneLayout[];
    monitorBindings: Record<string, string>;
  };
  workspaces: {
    minimizeUnrelated: boolean;
    items: WorkspaceItem[];
  };
  quickSwitch: {
    currentMonitorOnly: boolean;
    showPreview: boolean;
    showNumbers: boolean;
    density: QuickSwitchDensity;
    enablePinyinSearch: boolean;
  };
  alwaysOnTop: {
    showBorder: boolean;
    borderThickness: number;
    borderColor: string;
  };
  opacity: {
    stepPercent: number;
    minPercent: number;
    wheelOnTitlebar: boolean;
  };
  altDrag: {
    modifier: AltDragModifier;
    snapToZones: boolean;
    excludedApps: string[];
  };
  focusBorder: {
    thickness: number;
    color: string;
    cornerRadius: number;
  };
  multiMonitor: {
    centerNewWindows: boolean;
    enableTrayMinimize: boolean;
  };
}

export interface WindowManagerResponse {
  enabled: boolean;
  running: boolean;
  exeAvailable: boolean;
  runtimeStats?: WindowManagerRuntimeStats;
  settings: WindowManagerSettings;
}

export interface MonitorDescriptor {
  id: string;
  name: string;
  width: number;
  height: number;
  dpi: number;
  isPrimary: boolean;
}

export interface VirtualDesktopDescriptor {
  id: string;
  name: string;
  index: number;
  isCurrent: boolean;
}

export const BUILTIN_DEFAULT_LAYOUTS: SnapZoneLayout[] = [
  {
    id: 'builtin-columns-2',
    name: '2 Columns',
    type: 'grid',
    builtin: true,
    rows: 1,
    columns: 2,
    rowPercents: [100],
    colPercents: [50, 50],
    cellMap: [[0, 1]],
  },
  {
    id: 'builtin-priority-3',
    name: 'Priority Grid',
    type: 'grid',
    builtin: true,
    rows: 1,
    columns: 3,
    rowPercents: [100],
    colPercents: [25, 50, 25],
    cellMap: [[0, 1, 2]],
  },
  {
    id: 'builtin-grid-2x2',
    name: '2x2 Grid',
    type: 'grid',
    builtin: true,
    rows: 2,
    columns: 2,
    rowPercents: [50, 50],
    colPercents: [50, 50],
    cellMap: [
      [0, 1],
      [2, 3],
    ],
  },
  {
    id: 'builtin-focus-split',
    name: 'Main + Stack',
    type: 'grid',
    builtin: true,
    rows: 2,
    columns: 2,
    rowPercents: [50, 50],
    colPercents: [60, 40],
    cellMap: [
      [0, 1],
      [0, 2],
    ],
  },
];

export const DEFAULT_WINDOW_MANAGER_SETTINGS: WindowManagerSettings = {
  features: {
    snapZones: false,
    workspaces: false,
    quickSwitch: false,
    alwaysOnTop: false,
    opacity: false,
    altDrag: false,
    focusBorder: false,
    multiMonitor: false,
  },
  snapZones: {
    triggerMode: 'shift',
    spanMultipleZones: true,
    showZoneNumbers: true,
    zoneGap: 8,
    overlayOpacity: 70,
    rememberLastZone: true,
    perVirtualDesktop: false,
    excludedApps: [],
    layouts: BUILTIN_DEFAULT_LAYOUTS,
    monitorBindings: {},
  },
  workspaces: {
    minimizeUnrelated: false,
    items: [],
  },
  quickSwitch: {
    currentMonitorOnly: false,
    showPreview: true,
    showNumbers: true,
    density: 'comfortable',
    enablePinyinSearch: true,
  },
  alwaysOnTop: {
    showBorder: true,
    borderThickness: 3,
    borderColor: '#0ea5e9',
  },
  opacity: {
    stepPercent: 10,
    minPercent: 25,
    wheelOnTitlebar: true,
  },
  altDrag: {
    modifier: 'alt',
    snapToZones: true,
    excludedApps: [],
  },
  focusBorder: {
    thickness: 2,
    color: '#8b5cf6',
    cornerRadius: 10,
  },
  multiMonitor: {
    centerNewWindows: false,
    enableTrayMinimize: true,
  },
};

export const DEFAULT_WINDOW_MANAGER_RESPONSE: WindowManagerResponse = {
  enabled: false,
  running: false,
  exeAvailable: true,
  settings: DEFAULT_WINDOW_MANAGER_SETTINGS,
};

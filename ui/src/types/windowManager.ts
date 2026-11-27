/* ─────────────────────────────────────────────────────────────────────────────
 * windowManager.ts — 窗口管理模块前后端 IPC 契约单一事实源 (SSOT · Schema v3)
 * ───────────────────────────────────────────────────────────────────────────── */

export type SnapTriggerMode = 'shift' | 'right_click' | 'edge';
export type QuickSwitchDensity = 'compact' | 'comfortable';
export type AltDragModifier = 'alt' | 'ctrl_alt';
export type SnapZoneLayoutType = 'none' | 'focus' | 'grid' | 'canvas';
export type SnapZoneTemplateKind =
  | 'none'
  | 'focus'
  | 'columns'
  | 'rows'
  | 'grid'
  | 'priority_grid';
export type UnrelatedWindowAction = 'keep' | 'minimize' | 'minimize_to_tray';

export type WindowManagerActionName =
  | 'toggle_always_on_top'
  | 'rescue_offscreen'
  | 'restore_all_tray'
  | 'open_quick_switch'
  | 'launch_workspace'
  | 'move_next_monitor'
  | 'move_next_zone'
  | 'move_prev_zone'
  | 'restore_from_zone'
  | 'minimize_to_tray'
  | 'opacity_up'
  | 'opacity_down'
  | 'opacity_reset'
  | 'show_zones_preview'
  | 'open_layout_editor'
  | 'open_layout_editor_direct'
  | 'apply_layout_hotkey_digit'
  | 'start_workspace_capture'
  | 'preview_border_style';

export interface WindowManagerRuntimeStats {
  installed?: boolean;
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
  restartCount?: number;
  circuitBreakerTripped?: boolean;
}

export interface SnapZoneCanvasRect {
  id: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface NormalizedWorkRect {
  relX?: number;
  relY?: number;
  relW?: number;
  relH?: number;
  x?: number;
  y?: number;
  w?: number;
  h?: number;
}

export interface SnapZoneLayout {
  id: string;
  name: string;
  type: SnapZoneLayoutType;
  builtin?: boolean;
  templateKind?: SnapZoneTemplateKind;
  zoneCount?: number;
  showSpacing?: boolean;
  gap?: number;
  highlightDistance?: number;
  hotkeyDigit?: number;
  defaultHorizontal?: boolean;
  defaultVertical?: boolean;
  rows?: number;
  columns?: number;
  rowPercents?: number[];
  colPercents?: number[];
  cellMap?: number[][];
  zones?: SnapZoneCanvasRect[];
}

export interface WorkspaceAppEntry {
  id?: string;
  appName?: string;
  exePath: string;
  processName: string;
  args: string;
  titleHint?: string;
  aumid?: string;
  iconDataUrl?: string;
  monitorId: string;
  monitorFriendly?: string;
  monitorOrientation?: 'landscape' | 'portrait';
  monitorIndex?: number;
  zoneIndex?: number;
  zoneIndices?: number[];
  relRect?: NormalizedWorkRect;
  rect?: { x: number; y: number; w: number; h: number };
  maximized?: boolean;
  minimized?: boolean;
  runAsAdmin?: boolean;
  alwaysOnTop?: boolean;
  opacityPercent?: number;
}

export interface WorkspaceItem {
  id: string;
  name: string;
  boundLayoutId?: string;
  desktopId?: string;
  desktopName?: string;
  moveExistingWindows?: boolean;
  unrelatedAction?: UnrelatedWindowAction;
  createDesktopShortcut?: boolean;
  lastLaunchedAt?: number;
  apps: WorkspaceAppEntry[];
}

export interface WindowManagerHotkeys {
  toggleAlwaysOnTop: string;
  moveNextZone: string;
  movePrevZone: string;
  restoreFromZone: string;
  moveNextMonitor: string;
  rescueOffscreen: string;
  minimizeToTray: string;
  restoreAllTray: string;
  opacityUp: string;
  opacityDown: string;
  opacityReset: string;
  openLayoutEditor: string;
  openQuickSwitch: string;
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
    edgeThresholdPx: number;
    spanMultipleZones: boolean;
    showZoneNumbers: boolean;
    zoneGap: number;
    overlayOpacity: number;
    rememberLastZone: boolean;
    restoreOriginalSize: boolean;
    perVirtualDesktop: boolean;
    showOnAllMonitors: boolean;
    overrideWinArrows: boolean;
    moveByPosition: boolean;
    activeLayoutId: string;
    excludedApps: string[];
    layouts: SnapZoneLayout[];
    monitorBindings: Record<string, string>;
  };
  workspaces: {
    minimizeUnrelated: boolean;
    unrelatedAction: UnrelatedWindowAction;
    items: WorkspaceItem[];
  };
  quickSwitch: {
    replaceAltTab: boolean;
    sameAppEnabled: boolean;
    sameAppHotkey: string;
    currentMonitorOnly: boolean;
    currentDesktopOnly: boolean;
    showPreview: boolean;
    previewInDesktopPosition: boolean;
    showNumbers: boolean;
    showAppNames: boolean;
    largeIcons: boolean;
    visibleBorders: boolean;
    density: QuickSwitchDensity;
    enablePinyinSearch: boolean;
    rememberBounds: boolean;
    customWidthDip?: number;
    customHeightDip?: number;
    listWidthRatio: number;
    customMonitorId: string;
    customRelRect?: NormalizedWorkRect;
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
  hotkeys: WindowManagerHotkeys;
}

export interface WindowManagerResponse {
  enabled: boolean;
  running: boolean;
  exeAvailable: boolean;
  circuitBreakerTripped?: boolean;
  runtimeStats?: WindowManagerRuntimeStats;
  settings: WindowManagerSettings;
}

export interface MonitorDescriptor {
  id: string;
  name: string;
  width: number;
  height: number;
  rect?: { x: number; y: number; w: number; h: number };
  workArea?: { x: number; y: number; w: number; h: number };
  dpi: number;
  isPrimary: boolean;
}

export interface VirtualDesktopDescriptor {
  id: string;
  name: string;
  index: number;
  isCurrent: boolean;
}

export interface StaleBindingRecord {
  key: string;
  monitorId: string;
  desktopId: string;
  layoutId: string;
}

export interface BindingAuditSummary {
  success: boolean;
  totalBindings: number;
  offlineMonitorCount: number;
  removedDesktopCount: number;
  offlineMonitorBindings: StaleBindingRecord[];
  removedDesktopBindings: StaleBindingRecord[];
}

export interface WorkspaceCapturedEventPayload {
  captured: boolean;
  targetWorkspaceId?: string;
  monitors?: MonitorDescriptor[];
  workspace?: WorkspaceItem;
}

export const DEFAULT_WINDOW_MANAGER_HOTKEYS: WindowManagerHotkeys = {
  toggleAlwaysOnTop: 'Ctrl+Win+T',
  moveNextZone: 'Win+Alt+Right',
  movePrevZone: 'Win+Alt+Left',
  restoreFromZone: 'Win+Alt+Down',
  moveNextMonitor: 'Ctrl+Win+Right',
  rescueOffscreen: 'Ctrl+Win+G',
  minimizeToTray: 'Ctrl+Win+Down',
  restoreAllTray: 'Ctrl+Win+Shift+Down',
  opacityUp: 'Ctrl+Win+Up',
  opacityDown: 'Ctrl+Win+PageDown',
  opacityReset: 'Ctrl+Win+0',
  openLayoutEditor: 'Win+Shift+~',
  openQuickSwitch: '',
};

export const BUILTIN_DEFAULT_LAYOUTS: SnapZoneLayout[] = [
  {
    id: 'template_none',
    name: 'No layout',
    type: 'none',
    builtin: true,
    templateKind: 'none',
    zoneCount: 0,
    showSpacing: false,
    gap: 0,
    highlightDistance: 20,
    defaultHorizontal: false,
    defaultVertical: false,
  },
  {
    id: 'template_focus',
    name: 'Focus',
    type: 'focus',
    builtin: true,
    templateKind: 'focus',
    zoneCount: 3,
    showSpacing: true,
    gap: 16,
    highlightDistance: 20,
    defaultHorizontal: false,
    defaultVertical: false,
  },
  {
    id: 'template_columns',
    name: 'Columns',
    type: 'grid',
    builtin: true,
    templateKind: 'columns',
    zoneCount: 3,
    showSpacing: true,
    gap: 16,
    highlightDistance: 20,
    defaultHorizontal: false,
    defaultVertical: false,
    rows: 1,
    columns: 3,
    rowPercents: [100],
    colPercents: [33.3, 33.4, 33.3],
    cellMap: [[0, 1, 2]],
  },
  {
    id: 'template_rows',
    name: 'Rows',
    type: 'grid',
    builtin: true,
    templateKind: 'rows',
    zoneCount: 3,
    showSpacing: true,
    gap: 16,
    highlightDistance: 20,
    defaultHorizontal: false,
    defaultVertical: true,
    rows: 3,
    columns: 1,
    rowPercents: [33.3, 33.4, 33.3],
    colPercents: [100],
    cellMap: [[0], [1], [2]],
  },
  {
    id: 'template_grid',
    name: 'Grid',
    type: 'grid',
    builtin: true,
    templateKind: 'grid',
    zoneCount: 3,
    showSpacing: true,
    gap: 16,
    highlightDistance: 20,
    defaultHorizontal: false,
    defaultVertical: false,
    rows: 1,
    columns: 3,
    rowPercents: [100],
    colPercents: [33.3, 33.4, 33.3],
    cellMap: [[0, 1, 2]],
  },
  {
    id: 'template_priority_grid',
    name: 'Priority Grid',
    type: 'grid',
    builtin: true,
    templateKind: 'priority_grid',
    zoneCount: 3,
    showSpacing: true,
    gap: 16,
    highlightDistance: 20,
    defaultHorizontal: true,
    defaultVertical: false,
    rows: 1,
    columns: 3,
    rowPercents: [100],
    colPercents: [25, 50, 25],
    cellMap: [[0, 1, 2]],
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
    edgeThresholdPx: 20,
    spanMultipleZones: true,
    showZoneNumbers: true,
    zoneGap: 16,
    overlayOpacity: 70,
    rememberLastZone: true,
    restoreOriginalSize: true,
    perVirtualDesktop: false,
    showOnAllMonitors: false,
    overrideWinArrows: false,
    moveByPosition: false,
    activeLayoutId: 'template_priority_grid',
    excludedApps: [],
    layouts: BUILTIN_DEFAULT_LAYOUTS,
    monitorBindings: {},
  },
  workspaces: {
    minimizeUnrelated: false,
    unrelatedAction: 'keep',
    items: [],
  },
  quickSwitch: {
    replaceAltTab: true,
    sameAppEnabled: false,
    sameAppHotkey: 'Alt+`',
    currentMonitorOnly: false,
    currentDesktopOnly: true,
    showPreview: true,
    previewInDesktopPosition: false,
    showNumbers: true,
    showAppNames: false,
    largeIcons: true,
    visibleBorders: true,
    density: 'comfortable',
    enablePinyinSearch: true,
    rememberBounds: true,
    listWidthRatio: 0.34,
    customMonitorId: '',
  },
  alwaysOnTop: {
    showBorder: true,
    borderThickness: 2,
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
  hotkeys: DEFAULT_WINDOW_MANAGER_HOTKEYS,
};

export const DEFAULT_WINDOW_MANAGER_RESPONSE: WindowManagerResponse = {
  enabled: false,
  running: false,
  exeAvailable: true,
  settings: DEFAULT_WINDOW_MANAGER_SETTINGS,
};

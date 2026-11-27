/* ─────────────────────────────────────────────────────────────────────────────
 * gestureModel — 手势相关的共享类型 / 常量 / 纯函数
 * (与组件分离, 以满足 react-refresh 的「文件仅导出组件」约束)
 * ───────────────────────────────────────────────────────────────────────────── */

import i18n from '../i18n/config';

export type TriggerState = 'default' | 'enabled' | 'disabled';

export interface TriggerItemDef {
  key: string;
  name: string;
  nameKey?: string;
  category: 'mouse' | 'edge';
  iconType: 'rclick' | 'mclick' | 'lclick' | 'xbutton1' | 'xbutton2' | 'edge_slide' | 'edge_wheel' | 'edge_rclick' | 'edge_mclick' | 'edge_lclick';
}

export const TRIGGER_ITEM_DEFINITIONS: TriggerItemDef[] = [
  { key: 'right', name: 'Right Mouse Button', nameKey: 'gesture.triggerRightMouse', category: 'mouse', iconType: 'rclick' },
  { key: 'middle', name: 'Middle Mouse Button', nameKey: 'gesture.triggerMiddleMouse', category: 'mouse', iconType: 'mclick' },
  { key: 'xbutton1', name: 'Mouse Side Button 1', nameKey: 'gesture.triggerX1Mouse', category: 'mouse', iconType: 'xbutton1' },
  { key: 'xbutton2', name: 'Mouse Side Button 2', nameKey: 'gesture.triggerX2Mouse', category: 'mouse', iconType: 'xbutton2' },
  { key: 'left', name: 'Left Mouse Button', nameKey: 'gesture.triggerLeftMouse', category: 'mouse', iconType: 'lclick' },
  { key: 'edge_top_slide', name: 'Top Screen Edge + Mouse Slide', nameKey: 'gesture.triggerEdgeTopSlide', category: 'edge', iconType: 'edge_slide' },
  { key: 'edge_bottom_slide', name: 'Bottom Screen Edge + Mouse Slide', nameKey: 'gesture.triggerEdgeBottomSlide', category: 'edge', iconType: 'edge_slide' },
  { key: 'edge_left_slide', name: 'Left Screen Edge + Mouse Slide', nameKey: 'gesture.triggerEdgeLeftSlide', category: 'edge', iconType: 'edge_slide' },
  { key: 'edge_right_slide', name: 'Right Screen Edge + Mouse Slide', nameKey: 'gesture.triggerEdgeRightSlide', category: 'edge', iconType: 'edge_slide' },
  { key: 'edge_top_wheel', name: 'Top Screen Edge + Wheel', nameKey: 'gesture.triggerEdgeTopWheel', category: 'edge', iconType: 'edge_wheel' },
  { key: 'edge_bottom_wheel', name: 'Bottom Screen Edge + Wheel', nameKey: 'gesture.triggerEdgeBottomWheel', category: 'edge', iconType: 'edge_wheel' },
  { key: 'edge_left_wheel', name: 'Left Screen Edge + Wheel', nameKey: 'gesture.triggerEdgeLeftWheel', category: 'edge', iconType: 'edge_wheel' },
  { key: 'edge_right_wheel', name: 'Right Screen Edge + Wheel', nameKey: 'gesture.triggerEdgeRightWheel', category: 'edge', iconType: 'edge_wheel' },
];

export type ProgramRunMode = 'activate_or_run' | 'toggle_or_run' | 'always_new';

export const DEFAULT_LOCAL_LUA_PERMISSIONS = [
  'log',
  'url',
  'window',
  'ui',
  'keyboard',
  'mouse',
  'clipboard',
  'screen',
  'shell',
  'fs',
  'http',
] as const;

export interface GestureMapping {
  id?: string;
  enabled?: boolean;
  instantExecute?: boolean;
  silentToast?: boolean;
  gestureCode: string;
  action: {
    type: number;
    name: string;
    description?: string;
    keyStroke?: string;
    luaScript?: string;
    permissions?: string[];
    builtinCmd?: number;
    builtinParam?: string;
    programPath?: string;
    programArgs?: string;
    runMode?: ProgramRunMode;
    matchCmdLine?: string;
    matchTitle?: string;
  };
}

export interface GestureProfileData {
  name: string;
  mappings: GestureMapping[];
  triggerStates?: Record<string, TriggerState>;
}

export const ACTION_TYPE_KEYS = [
  'gestureEditor.actionTypes.hotkey',
  'gestureEditor.actionTypes.lua',
  'gestureEditor.actionTypes.builtin',
  'gestureEditor.actionTypes.program',
] as const;

// 顺序必须与 C++ BuiltinCommand 枚举一致 (gesture/GestureAction.h)
export const BUILTIN_COMMAND_KEYS = [
  'gestureEditor.builtin.closeWindow', 'gestureEditor.builtin.closeTab',
  'gestureEditor.builtin.maximize', 'gestureEditor.builtin.minimize',
  'gestureEditor.builtin.restore', 'gestureEditor.builtin.showDesktop',
  'gestureEditor.builtin.switchDesktop', 'gestureEditor.builtin.taskView',
  'gestureEditor.builtin.lockScreen', 'gestureEditor.builtin.pauseGestures',
  'gestureEditor.builtin.screenshot', 'gestureEditor.builtin.record',
  'gestureEditor.builtin.restoreTab', 'gestureEditor.builtin.topmost',
  'gestureEditor.builtin.transparency', 'gestureEditor.builtin.webSearch',
  'gestureEditor.builtin.search', 'gestureEditor.builtin.radialMenu',
  'gestureEditor.builtin.pasteAsPin', 'gestureEditor.builtin.mediaNext',
  'gestureEditor.builtin.mediaPrev', 'gestureEditor.builtin.mediaPlayPause',
  'gestureEditor.builtin.volumeUp', 'gestureEditor.builtin.volumeDown',
  'gestureEditor.builtin.volumeMute', 'gestureEditor.builtin.prevDesktop',
  'gestureEditor.builtin.nextDesktop', 'gestureEditor.builtin.deleteKey',
  'gestureEditor.builtin.backspaceKey', 'gestureEditor.builtin.brightnessUp',
  'gestureEditor.builtin.brightnessDown', 'gestureEditor.builtin.quickTranslate',
  'gestureEditor.builtin.windowKitNextZone', 'gestureEditor.builtin.windowKitPrevZone',
  'gestureEditor.builtin.windowKitRestoreZone', 'gestureEditor.builtin.windowKitMoveNextMonitor',
  'gestureEditor.builtin.windowKitMinimizeToTray', 'gestureEditor.builtin.windowKitQuickSwitch',
  'gestureEditor.builtin.windowKitOpenLayoutEditor', 'gestureEditor.builtin.windowKitApplyLayout',
  'gestureEditor.builtin.windowKitLaunchWorkspace', 'gestureEditor.builtin.windowKitStartWorkspaceCapture',
  'gestureEditor.builtin.windowKitRestoreAllTray', 'gestureEditor.builtin.windowKitRescueOffscreen',
] as const;

export const CODE_TO_ARROWS: Record<string, string> = {
  L: '←', R: '→', U: '↑', D: '↓',
  UL: '↖', UR: '↗', DL: '↙', DR: '↘',
  LU: '↖', RU: '↗', LD: '↙', RD: '↘',
  WHEELUP: 'Wheel Up',
  WHEELDOWN: 'Wheel Down',
};

export type TriggerButton = 'right' | 'middle' | 'x1' | 'x2' | 'left';
export type ScreenEdge = 'none' | 'top' | 'bottom' | 'left' | 'right';

export interface ParsedGestureCode {
  edge: ScreenEdge;
  triggerButton: TriggerButton;
  hasExplicitTrigger?: boolean;
  isMiddle: boolean;
  isX1: boolean;
  isX2: boolean;
  isLeft: boolean;
  isTopEdge: boolean;
  hasCtrl: boolean;
  hasAlt: boolean;
  hasShift: boolean;
  wheelDirection?: 'up' | 'down' | null;
  bareCode: string;
}

export function parseGestureCode(code: string): ParsedGestureCode {
  let upper = (code || '').trim().toUpperCase();
  let edge: ScreenEdge = 'none';
  let triggerButton: TriggerButton = 'right';
  let hasExplicitTrigger = false;
  let hasCtrl = false;
  let hasAlt = false;
  let hasShift = false;

  let matched = true;
  while (matched) {
    matched = false;
    if (upper.startsWith('TOPEDGE+')) {
      edge = 'top';
      upper = upper.slice(8);
      matched = true;
    } else if (upper.startsWith('BOTTOMEDGE+')) {
      edge = 'bottom';
      upper = upper.slice(11);
      matched = true;
    } else if (upper.startsWith('LEFTEDGE+')) {
      edge = 'left';
      upper = upper.slice(9);
      matched = true;
    } else if (upper.startsWith('RIGHTEDGE+')) {
      edge = 'right';
      upper = upper.slice(10);
      matched = true;
    } else if (upper.startsWith('CTRL+')) {
      hasCtrl = true;
      upper = upper.slice(5);
      matched = true;
    } else if (upper.startsWith('ALT+')) {
      hasAlt = true;
      upper = upper.slice(4);
      matched = true;
    } else if (upper.startsWith('SHIFT+')) {
      hasShift = true;
      upper = upper.slice(6);
      matched = true;
    } else if (upper.startsWith('RIGHT+') || upper.startsWith('RBUTTON+')) {
      triggerButton = 'right';
      hasExplicitTrigger = true;
      upper = upper.replace(/^(RIGHT|RBUTTON)\+/, '');
      matched = true;
    } else if (upper.startsWith('MIDDLE+')) {
      triggerButton = 'middle';
      hasExplicitTrigger = true;
      upper = upper.slice(7);
      matched = true;
    } else if (upper.startsWith('X1+') || upper.startsWith('SIDE1+') || upper.startsWith('XBUTTON1+')) {
      triggerButton = 'x1';
      hasExplicitTrigger = true;
      upper = upper.replace(/^(X1|SIDE1|XBUTTON1)\+/, '');
      matched = true;
    } else if (upper.startsWith('X2+') || upper.startsWith('SIDE2+') || upper.startsWith('XBUTTON2+')) {
      triggerButton = 'x2';
      hasExplicitTrigger = true;
      upper = upper.replace(/^(X2|SIDE2|XBUTTON2)\+/, '');
      matched = true;
    } else if (upper.startsWith('LEFT+')) {
      triggerButton = 'left';
      hasExplicitTrigger = true;
      upper = upper.slice(5);
      matched = true;
    }
  }

  const wheelDirection: 'up' | 'down' | null =
    upper === 'WHEELUP' ? 'up' : upper === 'WHEELDOWN' ? 'down' : null;

  return {
    edge,
    triggerButton,
    hasExplicitTrigger,
    isMiddle: triggerButton === 'middle',
    isX1: triggerButton === 'x1',
    isX2: triggerButton === 'x2',
    isLeft: triggerButton === 'left',
    isTopEdge: edge === 'top',
    hasCtrl,
    hasAlt,
    hasShift,
    wheelDirection,
    bareCode: upper,
  };
}

export function assembleGestureCode(params: {
  edge?: ScreenEdge;
  triggerButton?: TriggerButton;
  isMiddle?: boolean;
  isX1?: boolean;
  isX2?: boolean;
  isLeft?: boolean;
  isTopEdge?: boolean;
  hasCtrl?: boolean;
  hasAlt?: boolean;
  hasShift?: boolean;
  bareCode: string;
}): string {
  let prefix = '';

  // 1. Edge prefix
  if (params.edge === 'top' || params.isTopEdge) prefix += 'TopEdge+';
  else if (params.edge === 'bottom') prefix += 'BottomEdge+';
  else if (params.edge === 'left') prefix += 'LeftEdge+';
  else if (params.edge === 'right') prefix += 'RightEdge+';

  // 2. Modifier prefix
  if (params.hasCtrl) prefix += 'Ctrl+';
  if (params.hasAlt) prefix += 'Alt+';
  if (params.hasShift) prefix += 'Shift+';

  // 3. Trigger button prefix
  if (params.triggerButton === 'middle' || params.isMiddle) prefix += 'Middle+';
  else if (params.triggerButton === 'x1' || params.isX1) prefix += 'X1+';
  else if (params.triggerButton === 'x2' || params.isX2) prefix += 'X2+';
  else if (params.triggerButton === 'left' || params.isLeft) prefix += 'Left+';

  const bare = (params.bareCode || '').trim();
  if (!bare) return '';
  if (bare.toUpperCase() === 'WHEELUP') return prefix + 'WheelUp';
  if (bare.toUpperCase() === 'WHEELDOWN') return prefix + 'WheelDown';
  return prefix + bare.toUpperCase();
}

/** 把完整手势编码渲染为箭头与文字提示串, 用于实时预览与列表清晰展示。支持多语言自适应前缀 */
export function codeToArrows(code: string, customTranslator?: (key: string) => string): string {
  if (!code) return '';
  const parsed = parseGestureCode(code);
  let prefix = '';

  const tr = (k: string, fallback: string): string => {
    if (customTranslator) return customTranslator(k);
    if (i18n?.t) {
      const res = (i18n.t as (key: string, opt?: { defaultValue?: string }) => string)(k, { defaultValue: fallback });
      return (typeof res === 'string' && res) ? res : fallback;
    }
    return fallback;
  };

  if (parsed.edge === 'top') prefix += tr('gesture.prefixTopEdge', '[Top Edge] ');
  else if (parsed.edge === 'bottom') prefix += tr('gesture.prefixBottomEdge', '[Bottom Edge] ');
  else if (parsed.edge === 'left') prefix += tr('gesture.prefixLeftEdge', '[Left Edge] ');
  else if (parsed.edge === 'right') prefix += tr('gesture.prefixRightEdge', '[Right Edge] ');

  if (parsed.hasCtrl) prefix += 'Ctrl+';
  if (parsed.hasAlt) prefix += 'Alt+';
  if (parsed.hasShift) prefix += 'Shift+';

  if (parsed.triggerButton === 'middle') prefix += tr('gesture.prefixMiddleButton', '[Middle] ');
  else if (parsed.triggerButton === 'x1') prefix += tr('gesture.prefixX1Button', '[X1] ');
  else if (parsed.triggerButton === 'x2') prefix += tr('gesture.prefixX2Button', '[X2] ');
  else if (parsed.triggerButton === 'left') prefix += tr('gesture.prefixLeftButton', '[Left] ');

  const bareCode = parsed.bareCode;
  if (!bareCode) return prefix.trim();

  if (bareCode === 'WHEELUP') {
    return prefix + tr('gesture.wheelUpArrow', 'Wheel Up');
  }
  if (bareCode === 'WHEELDOWN') {
    return prefix + tr('gesture.wheelDownArrow', 'Wheel Down');
  }

  if (bareCode.includes('-')) {
    return prefix + bareCode.split('-').map((seg) => CODE_TO_ARROWS[seg] ?? seg).join(' ');
  }
  if (CODE_TO_ARROWS[bareCode]) {
    return prefix + CODE_TO_ARROWS[bareCode];
  }
  const chars = bareCode.split('');
  if (chars.length > 0 && chars.every((c) => CODE_TO_ARROWS[c])) {
    return prefix + chars.map((c) => CODE_TO_ARROWS[c]).join(' ');
  }
  return prefix + bareCode;
}

export type ValidStrokeToken = 'U' | 'D' | 'L' | 'R' | 'UL' | 'UR' | 'DL' | 'DR' | 'LU' | 'RU' | 'LD' | 'RD';

export const VALID_STROKE_TOKENS = new Set<string>([
  'U', 'D', 'L', 'R',
  'UL', 'UR', 'DL', 'DR',
  'LU', 'RU', 'LD', 'RD',
]);

export interface ParsedGesturePreview {
  edge: ScreenEdge;
  triggerButton: TriggerButton;
  hasCtrl: boolean;
  hasAlt: boolean;
  hasShift: boolean;
  wheelDirection: 'up' | 'down' | null;
  segments: ValidStrokeToken[];
}

/**
 * 严格从裸编码中提取合法方向笔画序列。
 * 滚轮或任何未知 token 绝不拆字母，更绝不 fallback 为向右折线。
 */
export function parseDirectionSegments(bareCode: string): ValidStrokeToken[] {
  const clean = (bareCode || '').trim().toUpperCase();
  if (!clean || clean === 'WHEELUP' || clean === 'WHEELDOWN') {
    return [];
  }

  if (clean.includes('-')) {
    const result: ValidStrokeToken[] = [];
    for (const part of clean.split('-').filter(Boolean)) {
      if (VALID_STROKE_TOKENS.has(part)) {
        result.push(part as ValidStrokeToken);
      } else if (part.length > 1 && /^[UDLR]+$/.test(part)) {
        for (const ch of part) {
          result.push(ch as ValidStrokeToken);
        }
      }
      // 未知 token 直接忽略，严禁映射为右移
    }
    return result;
  }

  if (VALID_STROKE_TOKENS.has(clean)) {
    return [clean as ValidStrokeToken];
  }

  if (/^[UDLR]+$/.test(clean)) {
    return clean.split('') as ValidStrokeToken[];
  }

  return [];
}

/**
 * 供 GestureStrokePreview 等组件复用的规范化手势预览解析单一事实源。
 */
export function parseGesturePreview(
  code: string,
  fallbackTrigger: TriggerButton = 'right',
): ParsedGesturePreview {
  const parsed = parseGestureCode(code);
  return {
    edge: parsed.edge,
    triggerButton: parsed.hasExplicitTrigger ? parsed.triggerButton : fallbackTrigger,
    hasCtrl: parsed.hasCtrl,
    hasAlt: parsed.hasAlt,
    hasShift: parsed.hasShift,
    wheelDirection: parsed.wheelDirection ?? null,
    segments: parseDirectionSegments(parsed.bareCode),
  };
}

export type GestureFilterCategory = 'all' | 'right' | 'middle' | 'side' | 'edge' | 'wheel';

/**
 * 精准匹配手势编码所属的五分类筛选标签 (U-11):
 * - right: 纯全局右键 (无边缘前缀、非滚轮、触发键为 right)
 * - middle: 中键轨迹手势 (非滚轮、触发键为 middle)
 * - side: 侧键 1/2 手势 (非滚轮、触发键为 x1 或 x2)
 * - edge: 任意屏幕四边手势 (含边缘滑动与边缘滚轮)
 * - wheel: 任意滚轮手势 (含按键+滚轮与边缘+滚轮)
 */
export function matchesGestureCategory(code: string, category: GestureFilterCategory): boolean {
  if (category === 'all') return true;
  const parsed = parseGestureCode(code);
  const isWheel = parsed.wheelDirection !== null;
  switch (category) {
    case 'right':
      return parsed.edge === 'none' && !isWheel && parsed.triggerButton === 'right';
    case 'middle':
      return !isWheel && parsed.triggerButton === 'middle';
    case 'side':
      return !isWheel && (parsed.triggerButton === 'x1' || parsed.triggerButton === 'x2');
    case 'edge':
      return parsed.edge !== 'none';
    case 'wheel':
      return isWheel;
    default:
      return true;
  }
}

export const GESTURE_CODE_PATTERN = /^((TopEdge|BottomEdge|LeftEdge|RightEdge|Right|RButton|Middle|X1|X2|Side1|Side2|XButton1|XButton2|Left|Ctrl|Alt|Shift)\+)*([UDLR]{1,3}(-[UDLR]{1,3})*|WheelUp|WheelDown)$/i;

export function normalizeGestureCode(code: string): string {
  return code.trim().toUpperCase();
}

/**
 * 编辑已有手势时原地替换，保持列表顺序；新增则追加到末尾。
 * 若保存的方向码与另一项冲突，则覆盖那一项（编辑中的项仍留在原位）。
 */
export function upsertGestureMapping(
  list: readonly GestureMapping[],
  saved: GestureMapping,
  editing: GestureMapping | null,
): GestureMapping[] {
  const savedCode = normalizeGestureCode(saved.gestureCode);
  const editingId = editing?.id;
  const editingCode = editing ? normalizeGestureCode(editing.gestureCode) : '';

  const isEditingItem = (m: GestureMapping) => {
    if (!editing) return false;
    if (editingId && m.id) return m.id === editingId;
    return editingCode !== '' && normalizeGestureCode(m.gestureCode) === editingCode;
  };

  const next = list.slice();
  let editIdx = next.findIndex(isEditingItem);

  if (editIdx >= 0) {
    for (let i = next.length - 1; i >= 0; i--) {
      if (i === editIdx) continue;
      if (normalizeGestureCode(next[i].gestureCode) === savedCode) {
        next.splice(i, 1);
        if (i < editIdx) editIdx -= 1;
      }
    }
    next[editIdx] = saved;
    return next;
  }

  const conflictIdx = next.findIndex((m) => normalizeGestureCode(m.gestureCode) === savedCode);
  if (conflictIdx >= 0) {
    next[conflictIdx] = saved;
    return next;
  }

  next.push(saved);
  return next;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * 手势轨迹采样与消抖平滑管线 (World-Class Stroke Recognition & Jitter Elimination)
 * ───────────────────────────────────────────────────────────────────────────── */

export type Direction = 'U' | 'D' | 'L' | 'R' | 'UL' | 'UR' | 'DL' | 'DR';

export interface Point {
  x: number;
  y: number;
}

export function calculateDistance(x1: number, y1: number, x2: number, y2: number): number {
  const dx = x2 - x1;
  const dy = y2 - y1;
  return Math.sqrt(dx * dx + dy * dy);
}

/**
 * 将相对位移矢量转换为 8 方向枚举
 * 与 C++ 后端 100% 同源：给四个基准正交方向 (R, U, L, D) ±28° 磁吸区，
 * 同时给 45° 对角方向 (DR, UR, DL, UL) 保留 ±17° 充足扇区，兼顾防手抖与 V 形斜角易画性。
 */
export function angleToDirection(dx: number, dy: number): Direction | null {
  const angleRad = Math.atan2(-dy, dx);
  let angleDeg = (angleRad * 180) / Math.PI;
  if (angleDeg < 0) angleDeg += 360;

  const cardinalTolerance = 28;
  const circularDelta = (a: number, b: number) => {
    let d = Math.abs(a - b) % 360;
    if (d > 180) d = 360 - d;
    return d;
  };

  if (circularDelta(angleDeg, 0) <= cardinalTolerance) return 'R';
  if (circularDelta(angleDeg, 90) <= cardinalTolerance) return 'U';
  if (circularDelta(angleDeg, 180) <= cardinalTolerance) return 'L';
  if (circularDelta(angleDeg, 270) <= cardinalTolerance) return 'D';

  if (angleDeg < 90) return 'UR';
  if (angleDeg < 180) return 'UL';
  if (angleDeg < 270) return 'DL';
  return 'DR';
}

function isDiagonalDirection(d: Direction): boolean {
  return d === 'UL' || d === 'UR' || d === 'DL' || d === 'DR';
}

function oppositeCardinal(d: Direction): Direction | null {
  if (d === 'U') return 'D';
  if (d === 'D') return 'U';
  if (d === 'L') return 'R';
  if (d === 'R') return 'L';
  return null;
}

function sharedCardinalBetweenDiagonals(d1: Direction, d2: Direction): Direction | null {
  if ((d1 === 'DR' && d2 === 'DL') || (d1 === 'DL' && d2 === 'DR')) return 'D';
  if ((d1 === 'UR' && d2 === 'UL') || (d1 === 'UL' && d2 === 'UR')) return 'U';
  if ((d1 === 'DR' && d2 === 'UR') || (d1 === 'UR' && d2 === 'DR')) return 'R';
  if ((d1 === 'DL' && d2 === 'UL') || (d1 === 'UL' && d2 === 'DL')) return 'L';
  return null;
}

function isDiagonalCornerFillet(a: Direction, b: Direction, c: Direction): boolean {
  if (!isDiagonalDirection(a) || !isDiagonalDirection(c) || a === c) return false;
  const shared = sharedCardinalBetweenDiagonals(a, c);
  return shared !== null && b === shared;
}

function directionCenterDeg(dir: Direction): number {
  switch (dir) {
    case 'R': return 0;
    case 'UR': return 45;
    case 'U': return 90;
    case 'UL': return 135;
    case 'L': return 180;
    case 'DL': return 225;
    case 'D': return 270;
    case 'DR': return 315;
  }
}

function circularDeltaDeg(a: number, b: number): number {
  let d = Math.abs(a - b) % 360;
  if (d > 180) d = 360 - d;
  return d;
}

/**
 * 判定 b 是否属于 a -> c 直角转弯过程中由手腕/手指产生的自然圆角对角过渡
 */
export function isCornerFillet(a: Direction, b: Direction, c: Direction): boolean {
  return (
    (a === 'D' && c === 'R' && b === 'DR') ||
    (a === 'R' && c === 'D' && b === 'DR') ||
    (a === 'D' && c === 'L' && b === 'DL') ||
    (a === 'L' && c === 'D' && b === 'DL') ||
    (a === 'U' && c === 'R' && b === 'UR') ||
    (a === 'R' && c === 'U' && b === 'UR') ||
    (a === 'U' && c === 'L' && b === 'UL') ||
    (a === 'L' && c === 'U' && b === 'UL')
  );
}

/**
 * 判定是否属于孤立回弹抖动或 45° 邻近抖动（与 C++ 后端 100% 同源）
 * 严正红线: 阶梯状正交折线 (如 R-D-R、D-R-D、L-U-L) 及对角折线 (DR-UR-DR) 绝不误判为抖动！
 */
export function isJitterRebound(a: Direction, b: Direction, c: Direction): boolean {
  if (a !== c) return false;
  // 1. 180度反向回弹抖动
  if (
    (a === 'L' && b === 'R') ||
    (a === 'R' && b === 'L') ||
    (a === 'U' && b === 'D') ||
    (a === 'D' && b === 'U')
  ) {
    return true;
  }
  // 2. 正交 45° 邻近对角抖动
  if (
    (a === 'D' && (b === 'DR' || b === 'DL')) ||
    (a === 'U' && (b === 'UR' || b === 'UL')) ||
    (a === 'L' && (b === 'UL' || b === 'DL')) ||
    (a === 'R' && (b === 'UR' || b === 'DR'))
  ) {
    return true;
  }
  // 3. 对角 45° 邻近正交抖动（如 DR-D-DR, UR-R-UR）
  if (
    (a === 'DR' && (b === 'D' || b === 'R')) ||
    (a === 'DL' && (b === 'D' || b === 'L')) ||
    (a === 'UR' && (b === 'U' || b === 'R')) ||
    (a === 'UL' && (b === 'U' || b === 'L'))
  ) {
    return true;
  }
  return false;
}

/**
 * 补全未走完的转角圆角 (用户常在对角圆角上松手)
 */
export function completeIncompleteFillet(known: Direction, diagonal: Direction): Direction | null {
  if (diagonal === 'DR') {
    if (known === 'D') return 'R';
    if (known === 'R') return 'D';
  } else if (diagonal === 'DL') {
    if (known === 'D') return 'L';
    if (known === 'L') return 'D';
  } else if (diagonal === 'UR') {
    if (known === 'U') return 'R';
    if (known === 'R') return 'U';
  } else if (diagonal === 'UL') {
    if (known === 'U') return 'L';
    if (known === 'L') return 'U';
  }
  return null;
}

interface DirectionSegment {
  dir: Direction;
  length: number;
  dx: number;
  dy: number;
}

const SHARP_DIAGONAL_PAIR_MIN_TILT_DEG = 6;
const ARC_ARM_DIAGONAL_MERGE_MIN_TILT_DEG = 12;
const ARC_ARM_SHORT_BEND_MERGE_MIN_TILT_DEG = 8;
const ARC_ARM_SHORT_BEND_MAX_RATIO = 0.45;
const NARROW_CHEVRON_EACH_MIN_TILT_DEG = 15;
const NARROW_CHEVRON_TOTAL_SPREAD_DEG = 36;

function sharpDiagonalPartner(cardinal: Direction, diagonal: Direction): Direction | null {
  switch (cardinal) {
    case 'D':
      if (diagonal === 'UR') return 'DR';
      if (diagonal === 'UL') return 'DL';
      return null;
    case 'U':
      if (diagonal === 'DR') return 'UR';
      if (diagonal === 'DL') return 'UL';
      return null;
    case 'R':
      if (diagonal === 'DL') return 'DR';
      if (diagonal === 'UL') return 'UR';
      return null;
    case 'L':
      if (diagonal === 'DR') return 'DL';
      if (diagonal === 'UR') return 'UL';
      return null;
    default:
      return null;
  }
}

function tiltTowardDiagonalDeg(
  seg: DirectionSegment,
  cardinal: Direction,
  targetDiag: Direction,
): number {
  if (Math.hypot(seg.dx, seg.dy) < 1) return 0;
  let primary: number;
  let side: number;
  switch (cardinal) {
    case 'D':
      primary = seg.dy;
      side = targetDiag === 'DR' ? seg.dx : targetDiag === 'DL' ? -seg.dx : 0;
      break;
    case 'U':
      primary = -seg.dy;
      side = targetDiag === 'UR' ? seg.dx : targetDiag === 'UL' ? -seg.dx : 0;
      break;
    case 'R':
      primary = seg.dx;
      side = targetDiag === 'DR' ? seg.dy : targetDiag === 'UR' ? -seg.dy : 0;
      break;
    case 'L':
      primary = -seg.dx;
      side = targetDiag === 'DL' ? seg.dy : targetDiag === 'UL' ? -seg.dy : 0;
      break;
    default:
      return 0;
  }
  if (primary <= 1e-6 || side <= 0) return 0;
  return (Math.atan2(side, primary) * 180) / Math.PI;
}

function compressSegments(segs: DirectionSegment[]): DirectionSegment[] {
  const out: DirectionSegment[] = [];
  for (const s of segs) {
    if (s.length <= 0) continue;
    if (out.length > 0 && out[out.length - 1].dir === s.dir) {
      out[out.length - 1].length += s.length;
      out[out.length - 1].dx += s.dx;
      out[out.length - 1].dy += s.dy;
    } else {
      out.push({ dir: s.dir, length: s.length, dx: s.dx, dy: s.dy });
    }
  }
  return out;
}

function resolveSharpDiagonalSegments(current: DirectionSegment[]): void {
  if (current.length < 2) return;

  // 1. 正交 + 135° 反向跨象限对角（如手写 V 左笔稍陡初判为 D-UR，或右笔稍陡初判为 DR-U）
  let changed = true;
  while (changed) {
    changed = false;
    for (let i = 0; i + 1 < current.length; i++) {
      const a = current[i];
      const b = current[i + 1];
      if (!isDiagonalDirection(a.dir) && isDiagonalDirection(b.dir)) {
        const partner = sharpDiagonalPartner(a.dir, b.dir);
        if (partner && tiltTowardDiagonalDeg(a, a.dir, partner) >= SHARP_DIAGONAL_PAIR_MIN_TILT_DEG) {
          a.dir = partner;
          changed = true;
        }
      } else if (isDiagonalDirection(a.dir) && !isDiagonalDirection(b.dir)) {
        const partner = sharpDiagonalPartner(b.dir, a.dir);
        if (partner && tiltTowardDiagonalDeg(b, b.dir, partner) >= SHARP_DIAGONAL_PAIR_MIN_TILT_DEG) {
          b.dir = partner;
          changed = true;
        }
      }
    }
  }

  // 2. 两段 180° 反向正交且向同侧大幅张开的对称窄尖角（窄印刷体 V / ∧ / > / <）
  if (current.length === 2 && oppositeCardinal(current[0].dir) === current[1].dir) {
    const tryPromoteNarrowChevron = (diag0: Direction, diag1: Direction): boolean => {
      const t0 = tiltTowardDiagonalDeg(current[0], current[0].dir, diag0);
      const t1 = tiltTowardDiagonalDeg(current[1], current[1].dir, diag1);
      if (
        t0 >= NARROW_CHEVRON_EACH_MIN_TILT_DEG &&
        t1 >= NARROW_CHEVRON_EACH_MIN_TILT_DEG &&
        t0 + t1 >= NARROW_CHEVRON_TOTAL_SPREAD_DEG
      ) {
        current[0].dir = diag0;
        current[1].dir = diag1;
        return true;
      }
      return false;
    };

    const d0 = current[0].dir;
    if (d0 === 'D') {
      if (!tryPromoteNarrowChevron('DR', 'UR')) tryPromoteNarrowChevron('DL', 'UL');
    } else if (d0 === 'U') {
      if (!tryPromoteNarrowChevron('UR', 'DR')) tryPromoteNarrowChevron('UL', 'DL');
    } else if (d0 === 'R') {
      if (!tryPromoteNarrowChevron('DR', 'DL')) tryPromoteNarrowChevron('UR', 'UL');
    } else if (d0 === 'L') {
      if (!tryPromoteNarrowChevron('DL', 'DR')) tryPromoteNarrowChevron('UL', 'UR');
    }
  }
}

function simplifyDirectionSegments(rawSegs: DirectionSegment[]): Direction[] {
  let current = compressSegments(rawSegs);
  if (current.length === 0) return [];

  let modified = true;
  while (modified && current.length >= 3) {
    modified = false;
    const next: DirectionSegment[] = [];
    for (let i = 0; i < current.length; ) {
      // 1. 四段平滑半圆弧还原（R-DR-DL-L -> R-D-L 等 8 种）
      if (i + 3 < current.length) {
        const a = current[i].dir;
        const d1 = current[i + 1].dir;
        const d2 = current[i + 2].dir;
        const c = current[i + 3].dir;
        const shared = sharedCardinalBetweenDiagonals(d1, d2);
        if (shared && isCornerFillet(a, d1, shared) && isCornerFillet(shared, d2, c)) {
          next.push(current[i]);
          next.push({
            dir: shared,
            length: current[i + 1].length + current[i + 2].length,
            dx: current[i + 1].dx + current[i + 2].dx,
            dy: current[i + 1].dy + current[i + 2].dy,
          });
          next.push(current[i + 3]);
          i += 4;
          modified = true;
          continue;
        }
      }
      if (i + 2 < current.length) {
        const a = current[i].dir;
        const b = current[i + 1].dir;
        const c = current[i + 2].dir;

        if (isJitterRebound(a, b, c)) {
          next.push({
            dir: a,
            length: current[i].length + current[i + 1].length + current[i + 2].length,
            dx: current[i].dx + current[i + 1].dx + current[i + 2].dx,
            dy: current[i].dy + current[i + 1].dy + current[i + 2].dy,
          });
          i += 3;
          modified = true;
          continue;
        }

        if (isCornerFillet(a, b, c) || isDiagonalCornerFillet(a, b, c)) {
          next.push(current[i]);
          next.push(current[i + 2]);
          i += 3;
          modified = true;
          continue;
        }

        // 反向夹心单对角圆弧（如 R-DR-L -> R-D-L）
        const oppA = oppositeCardinal(a);
        if (oppA && c === oppA) {
          const mid = completeIncompleteFillet(a, b);
          if (mid) {
            next.push(current[i]);
            next.push({ dir: mid, length: current[i + 1].length, dx: current[i + 1].dx, dy: current[i + 1].dy });
            next.push(current[i + 2]);
            i += 3;
            modified = true;
            continue;
          }
        }

        // 三段未拉平圆弧双对角（如 R-DR-DL -> R-D-DL）；若首段正交已明显朝 b 倾斜（弧臂 V: D->DR->UR），则合入斜臂 b
        const sharedTail = sharedCardinalBetweenDiagonals(b, c);
        if (sharedTail && isCornerFillet(a, b, sharedTail)) {
          const tiltA = tiltTowardDiagonalDeg(current[i], a, b);
          const isArcArmBend =
            tiltA >= ARC_ARM_DIAGONAL_MERGE_MIN_TILT_DEG ||
            (tiltA >= ARC_ARM_SHORT_BEND_MERGE_MIN_TILT_DEG &&
              current[i + 1].length < current[i + 2].length * ARC_ARM_SHORT_BEND_MAX_RATIO);
          if (isArcArmBend) {
            next.push({
              dir: b,
              length: current[i].length + current[i + 1].length,
              dx: current[i].dx + current[i + 1].dx,
              dy: current[i].dy + current[i + 1].dy,
            });
            next.push(current[i + 2]);
          } else {
            next.push(current[i]);
            next.push({ dir: sharedTail, length: current[i + 1].length, dx: current[i + 1].dx, dy: current[i + 1].dy });
            next.push(current[i + 2]);
          }
          i += 3;
          modified = true;
          continue;
        }
      }
      next.push(current[i]);
      i++;
    }
    current = compressSegments(next);
  }

  let filletModified = true;
  while (filletModified && current.length >= 2) {
    filletModified = false;
    if (current.length === 2) {
      const completed = completeIncompleteFillet(current[0].dir, current[1].dir);
      if (completed) {
        current[1].dir = completed;
        filletModified = true;
      }
    } else {
      const completed = completeIncompleteFillet(
        current[current.length - 2].dir,
        current[current.length - 1].dir,
      );
      if (completed) {
        const prevPrev = current[current.length - 3].dir;
        const oppPrevPrev = oppositeCardinal(prevPrev);
        if (oppPrevPrev && completed === oppPrevPrev) {
          // 反向回折第三笔（反 C / 正 C / U 形）补全为正交收笔
          current[current.length - 1].dir = completed;
          filletModified = true;
        } else if (completed === prevPrev) {
          // 同象限收笔微垂（D-R-DR -> D-R）丢弃
          current.pop();
          filletModified = true;
        } else if (
          current[current.length - 1].length >= 24 &&
          current[current.length - 1].length >= current[current.length - 2].length * 0.45
        ) {
          current[current.length - 1].dir = completed;
          filletModified = true;
        } else {
          current.pop();
          filletModified = true;
        }
      }
    }
    if (current.length >= 2) {
      const invented = completeIncompleteFillet(current[1].dir, current[0].dir);
      if (invented) {
        const isReversingThreeSeg =
          current.length >= 3 &&
          oppositeCardinal(current[2].dir) !== null &&
          invented === oppositeCardinal(current[2].dir);
        const dropRatio = isReversingThreeSeg ? 0.22 : 0.4;
        if (current[0].length < current[1].length * dropRatio) {
          current.shift();
        } else {
          current[0].dir = invented;
        }
        filletModified = true;
      }
    }
    current = compressSegments(current);
  }

  resolveSharpDiagonalSegments(current);
  current = compressSegments(current);

  return current.map((s) => s.dir);
}

/**
 * 智能平滑方向序列：与 C++ GestureRecognizer::simplifyDirections 100% 同源
 */
export function simplifyDirections(raw: Direction[]): Direction[] {
  return simplifyDirectionSegments(raw.map((dir) => ({ dir, length: 1, dx: 0, dy: 0 })));
}

function isAdvancingInCone(dir: Direction, peak: Point, current: Point): boolean {
  const dist = calculateDistance(peak.x, peak.y, current.x, current.y);
  if (dist <= 0.5) return true;
  let angleDeg = (Math.atan2(-(current.y - peak.y), current.x - peak.x) * 180) / Math.PI;
  if (angleDeg < 0) angleDeg += 360;
  const cone = isDiagonalDirection(dir) ? 28.125 : 34.875;
  return circularDeltaDeg(angleDeg, directionCenterDeg(dir)) <= cone;
}

/**
 * 从轨迹点集高保真识别手势方向编码（与 C++ 后端拐点检测 + 推进锥 + 段长化简 100% 同源）
 */
export function recognizeStrokes(points: Point[], minSegmentDist = 16): string {
  if (points.length < 2) return '';

  const segments: DirectionSegment[] = [];
  let segStart = points[0];
  let peakPoint = points[0];
  let currentDir: Direction | null = null;
  const deadzone = 4;
  const maxDirections = 10;

  for (let i = 1; i < points.length; i++) {
    const pt = points[i];

    if (!currentDir) {
      const dist = calculateDistance(segStart.x, segStart.y, pt.x, pt.y);
      if (dist < minSegmentDist) continue;
      const dir = angleToDirection(pt.x - segStart.x, pt.y - segStart.y);
      if (dir) {
        currentDir = dir;
        peakPoint = pt;
      }
      continue;
    }

    const distFromPeak = calculateDistance(peakPoint.x, peakPoint.y, pt.x, pt.y);
    if (distFromPeak < deadzone) continue;

    if (isAdvancingInCone(currentDir, peakPoint, pt)) {
      peakPoint = pt;
      continue;
    }

    if (distFromPeak >= minSegmentDist) {
      const turnDir = angleToDirection(pt.x - peakPoint.x, pt.y - peakPoint.y);
      if (turnDir && turnDir !== currentDir && segments.length + 1 < maxDirections) {
        const len = calculateDistance(segStart.x, segStart.y, peakPoint.x, peakPoint.y);
        segments.push({
          dir: currentDir,
          length: Math.max(len, 1),
          dx: peakPoint.x - segStart.x,
          dy: peakPoint.y - segStart.y,
        });
        segStart = peakPoint;
        currentDir = turnDir;
        peakPoint = pt;
      }
    }
  }

  if (currentDir) {
    const len = calculateDistance(segStart.x, segStart.y, peakPoint.x, peakPoint.y);
    segments.push({
      dir: currentDir,
      length: Math.max(len, 1),
      dx: peakPoint.x - segStart.x,
      dy: peakPoint.y - segStart.y,
    });
  }

  const simplified = simplifyDirectionSegments(segments);
  return simplified.join('-');
}

/**
 * 段级前缀冲突检测：仅当触发前缀一致且方向段序列严格互为前缀时才判冲突（杜绝 D 与 DR-UR 误报）
 */
export function isGesturePrefixConflict(codeA: string, codeB: string): boolean {
  const normA = codeA.trim().toUpperCase();
  const normB = codeB.trim().toUpperCase();
  if (!normA || !normB || normA === normB) return false;

  const split = (code: string) => {
    const plusIdx = code.lastIndexOf('+');
    const prefix = plusIdx >= 0 ? code.slice(0, plusIdx + 1) : '';
    const bare = plusIdx >= 0 ? code.slice(plusIdx + 1) : code;
    const segs = bare.split('-').filter(Boolean);
    return { prefix, segs };
  };

  const a = split(normA);
  const b = split(normB);
  if (a.prefix !== b.prefix || a.segs.length === 0 || b.segs.length === 0) return false;
  if (a.segs.length === b.segs.length) return false;

  const [shorter, longer] = a.segs.length < b.segs.length ? [a.segs, b.segs] : [b.segs, a.segs];
  for (let i = 0; i < shorter.length; i++) {
    if (shorter[i] !== longer[i]) return false;
  }
  return true;
}

/**
 * 解析屏幕边缘手势所依赖的触发方式 key（如 LeftEdge+WheelUp -> edge_left_wheel）。
 * 非边缘手势返回 null。
 */
export function resolveRequiredEdgeTriggerKey(gestureCode: string): string | null {
  const parsed = parseGestureCode(gestureCode);
  if (parsed.edge === 'none') {
    return null;
  }
  const suffix = parsed.wheelDirection ? 'wheel' : 'slide';
  return `edge_${parsed.edge}_${suffix}`;
}

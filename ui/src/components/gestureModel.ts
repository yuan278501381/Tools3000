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
];

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
    builtinCmd?: number;
    programPath?: string;
    programArgs?: string;
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
  'gestureEditor.builtin.nextDesktop',
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
  isMiddle: boolean;
  isX1: boolean;
  isX2: boolean;
  isLeft: boolean;
  isTopEdge: boolean;
  hasCtrl: boolean;
  hasAlt: boolean;
  hasShift: boolean;
  bareCode: string;
}

export function parseGestureCode(code: string): ParsedGestureCode {
  let upper = (code || '').trim().toUpperCase();
  let edge: ScreenEdge = 'none';
  let triggerButton: TriggerButton = 'right';
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
    } else if (upper.startsWith('MIDDLE+')) {
      triggerButton = 'middle';
      upper = upper.slice(7);
      matched = true;
    } else if (upper.startsWith('X1+') || upper.startsWith('SIDE1+') || upper.startsWith('XBUTTON1+')) {
      triggerButton = 'x1';
      upper = upper.replace(/^(X1|SIDE1|XBUTTON1)\+/, '');
      matched = true;
    } else if (upper.startsWith('X2+') || upper.startsWith('SIDE2+') || upper.startsWith('XBUTTON2+')) {
      triggerButton = 'x2';
      upper = upper.replace(/^(X2|SIDE2|XBUTTON2)\+/, '');
      matched = true;
    } else if (upper.startsWith('LEFT+')) {
      triggerButton = 'left';
      upper = upper.slice(5);
      matched = true;
    }
  }

  return {
    edge,
    triggerButton,
    isMiddle: triggerButton === 'middle',
    isX1: triggerButton === 'x1',
    isX2: triggerButton === 'x2',
    isLeft: triggerButton === 'left',
    isTopEdge: edge === 'top',
    hasCtrl,
    hasAlt,
    hasShift,
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

export const GESTURE_CODE_PATTERN = /^((TopEdge|BottomEdge|LeftEdge|RightEdge|Middle|X1|X2|Left|Ctrl|Alt|Shift)\+)*([UDLR]{1,3}(-[UDLR]{1,3})*|WheelUp|WheelDown)$/i;

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
 * 与 C++ 后端对齐：给四个主要正交方向 (R, U, L, D) ±30° 磁吸容错区，
 * 剩余 30° 区间归为对角线，消除自然绘制水平/垂直折线时的轻微手抖。
 */
export function angleToDirection(dx: number, dy: number): Direction | null {
  const angleRad = Math.atan2(-dy, dx);
  let angleDeg = (angleRad * 180) / Math.PI;
  if (angleDeg < 0) angleDeg += 360;

  const cardinalTolerance = 30;
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
 * 判定是否属于孤立回弹抖动或微小对角抖动
 * 严格限制在:
 * 1. 180度反向回弹 (如 Left -> Right -> Left, Down -> Up -> Down)
 * 2. 45度微小对角抖动 (如 Down -> DownRight -> Down, Right -> UpRight -> Right)
 * 严正红线: 阶梯状正交折线 (如 R-D-R、D-R-D、L-U-L) 绝对禁止误判为抖动！
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
  // 2. 45度微小对角抖动
  if (
    (a === 'D' && (b === 'DR' || b === 'DL')) ||
    (a === 'U' && (b === 'UR' || b === 'UL')) ||
    (a === 'L' && (b === 'UL' || b === 'DL')) ||
    (a === 'R' && (b === 'UR' || b === 'DR'))
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

/**
 * 智能平滑方向序列：消除抖动、折叠转弯圆角、修剪末尾收笔多余斜角
 */
export function simplifyDirections(raw: Direction[]): Direction[] {
  if (raw.length === 0) return [];

  const current: Direction[] = [];
  for (const d of raw) {
    if (current.length === 0 || current[current.length - 1] !== d) {
      current.push(d);
    }
  }

  let modified = true;
  while (modified && current.length >= 3) {
    modified = false;
    const next: Direction[] = [];
    for (let i = 0; i < current.length; ) {
      if (i + 2 < current.length) {
        const a = current[i];
        const b = current[i + 1];
        const c = current[i + 2];

        // 抖动回弹消除 (注意: 绝不能无脑判断 a === c, 阶梯手势如 R-D-R、D-R-D 绝不能被吞噬)
        if (isJitterRebound(a, b, c)) {
          next.push(a);
          i += 3;
          modified = true;
          continue;
        }

        // 转弯对角圆角折叠 (Corner Fillet Folding)
        if (isCornerFillet(a, b, c)) {
          next.push(a);
          next.push(c);
          i += 3;
          modified = true;
          continue;
        }
      }
      next.push(current[i]);
      i++;
    }

    current.length = 0;
    for (const d of next) {
      if (current.length === 0 || current[current.length - 1] !== d) {
        current.push(d);
      }
    }
  }

  // 处理松笔圆角或未完成圆角 (与 C++ 算法保持 100% 对齐)
  let filletModified = true;
  while (filletModified && current.length >= 2) {
    filletModified = false;
    if (current.length === 2) {
      const completed = completeIncompleteFillet(current[0], current[1]);
      if (completed) {
        current[1] = completed;
        filletModified = true;
      }
    } else if (completeIncompleteFillet(current[current.length - 2], current[current.length - 1])) {
      // 3段以上收笔对角是自然下垂，不发明新的正交段
      current.pop();
      filletModified = true;
    }
  }

  return current;
}

/**
 * 从轨迹点集高保真识别手势方向编码 (如 "R-D-R")
 */
export function recognizeStrokes(points: Point[], minSegmentDist = 16): string {
  if (points.length < 2) return '';

  const rawDirs: Direction[] = [];
  let segStart = points[0];
  let currentDir: Direction | null = null;

  for (let i = 1; i < points.length; i++) {
    const pt = points[i];
    const dist = calculateDistance(segStart.x, segStart.y, pt.x, pt.y);
    if (dist < minSegmentDist) continue;

    const dir = angleToDirection(pt.x - segStart.x, pt.y - segStart.y);
    if (!dir) continue;

    if (dir !== currentDir) {
      if (currentDir) rawDirs.push(currentDir);
      currentDir = dir;
      segStart = pt;
    } else {
      segStart = pt;
    }
  }
  if (currentDir) rawDirs.push(currentDir);

  const simplified = simplifyDirections(rawDirs);
  return simplified.join('-');
}

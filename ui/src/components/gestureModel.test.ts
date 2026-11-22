import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import i18n from '../i18n/config';
import {
  upsertGestureMapping,
  codeToArrows,
  parseGestureCode,
  parseDirectionSegments,
  parseGesturePreview,
  matchesGestureCategory,
  resolveRequiredEdgeTriggerKey,
  assembleGestureCode,
  GESTURE_CODE_PATTERN,
  isGesturePrefixConflict,
  simplifyDirections,
  recognizeStrokes,
  type GestureMapping,
} from './gestureModel';

beforeAll(async () => {
  await i18n.changeLanguage('zh');
});

beforeEach(async () => {
  await i18n.changeLanguage('zh');
});

afterEach(async () => {
  await i18n.changeLanguage('zh');
});

function mapping(code: string, name: string, id?: string): GestureMapping {
  return { id, gestureCode: code, action: { type: 0, name, keyStroke: 'Ctrl+A' } };
}

describe('upsertGestureMapping', () => {
  const list = [
    mapping('D-R', '关闭标签页', 'a'),
    mapping('LU', '剪切', 'b'),
    mapping('UR', '最大化', 'c'),
  ];

  it('keeps an edited mapping in its original slot', () => {
    const saved = mapping('D-R', '关标签', 'a');
    const next = upsertGestureMapping(list, saved, list[0]);
    expect(next.map((m) => m.id)).toEqual(['a', 'b', 'c']);
    expect(next[0].action.name).toBe('关标签');
  });

  it('matches items without an id by the original gesture code', () => {
    const noId = [mapping('U', '关闭窗口'), mapping('D', '新建标签页')];
    const saved = mapping('U', '关窗');
    const next = upsertGestureMapping(noId, saved, noId[0]);
    expect(next).toHaveLength(2);
    expect(next[0].action.name).toBe('关窗');
    expect(next[1].gestureCode).toBe('D');
  });

  it('appends a brand-new mapping', () => {
    const saved = mapping('R-L', '全选', 'd');
    const next = upsertGestureMapping(list, saved, null);
    expect(next.map((m) => m.id)).toEqual(['a', 'b', 'c', 'd']);
  });

  it('keeps position when the gesture stroke itself changes', () => {
    const saved = mapping('R-D', '关闭标签页', 'a');
    const next = upsertGestureMapping(list, saved, list[0]);
    expect(next.map((m) => m.gestureCode)).toEqual(['R-D', 'LU', 'UR']);
  });

  it('drops a colliding mapping but keeps the edited row in place', () => {
    const saved = mapping('LU', '关闭标签页', 'a');
    const next = upsertGestureMapping(list, saved, list[0]);
    expect(next.map((m) => m.id)).toEqual(['a', 'c']);
    expect(next[0].gestureCode).toBe('LU');
  });

  it('replaces an existing code in place when adding would collide', () => {
    const saved = mapping('LU', '新剪切', 'x');
    const next = upsertGestureMapping(list, saved, null);
    expect(next.map((m) => m.id)).toEqual(['a', 'x', 'c']);
    expect(next[1].action.name).toBe('新剪切');
  });

  it('keeps the edited row in place when a collision sits earlier in the list', () => {
    const saved = mapping('D-R', '最大化改关标签', 'c');
    const next = upsertGestureMapping(list, saved, list[2]);
    expect(next.map((m) => m.id)).toEqual(['b', 'c']);
    expect(next[1].action.name).toBe('最大化改关标签');
  });
});

describe('codeToArrows, parseGestureCode, assembleGestureCode and GESTURE_CODE_PATTERN', () => {
  it('correctly renders arrows with Middle+, X1+, X2+, TopEdge+ and modifier prefixes', async () => {
    expect(codeToArrows('L')).toBe('←');
    expect(codeToArrows('R')).toBe('→');
    expect(codeToArrows('WheelUp')).toBe('滚轮向上');
    expect(codeToArrows('WheelDown')).toBe('滚轮向下');
    expect(codeToArrows('Middle+L')).toBe('[中键] ←');
    expect(codeToArrows('Middle+R')).toBe('[中键] →');
    expect(codeToArrows('Middle+WheelUp')).toBe('[中键] 滚轮向上');
    expect(codeToArrows('X1+L')).toBe('[侧键1] ←');
    expect(codeToArrows('X2+R')).toBe('[侧键2] →');
    expect(codeToArrows('TopEdge+D')).toBe('[上边缘] ↓');
    expect(codeToArrows('TopEdge+WheelDown')).toBe('[上边缘] 滚轮向下');
    expect(codeToArrows('Ctrl+U-R')).toBe('Ctrl+↑ →');
    expect(codeToArrows('Ctrl+WheelUp')).toBe('Ctrl+滚轮向上');
    expect(codeToArrows('Ctrl+X1+L')).toBe('Ctrl+[侧键1] ←');

    // 验证多语言前缀渲染 (自定义翻译器)
    const customEnT = (k: string) => {
      if (k === 'gesture.prefixMiddleButton') return '[Middle] ';
      if (k === 'gesture.prefixTopEdge') return '[Top Edge] ';
      if (k === 'gesture.prefixX1Button') return '[X1] ';
      if (k === 'gesture.wheelUpArrow') return 'Wheel Up';
      if (k === 'gesture.wheelDownArrow') return 'Wheel Down';
      return '';
    };
    expect(codeToArrows('Middle+L', customEnT)).toBe('[Middle] ←');
    expect(codeToArrows('TopEdge+D', customEnT)).toBe('[Top Edge] ↓');
    expect(codeToArrows('X1+L', customEnT)).toBe('[X1] ←');
    expect(codeToArrows('WheelUp', customEnT)).toBe('Wheel Up');
    expect(codeToArrows('WheelDown', customEnT)).toBe('Wheel Down');
    expect(codeToArrows('Middle+WheelUp', customEnT)).toBe('[Middle] Wheel Up');
    expect(codeToArrows('TopEdge+WheelDown', customEnT)).toBe('[Top Edge] Wheel Down');

    // 验证 i18n 实例语言动态切换环境 (中英双向切换并保证状态自愈)
    try {
      await i18n.changeLanguage('en');
      expect(codeToArrows('Middle+L')).toBe('[Middle] ←');
      expect(codeToArrows('WheelUp')).toBe('Wheel Up');
    } finally {
      await i18n.changeLanguage('zh');
    }
    expect(codeToArrows('Middle+L')).toBe('[中键] ←');
    expect(codeToArrows('WheelUp')).toBe('滚轮向上');

    expect(GESTURE_CODE_PATTERN.test('L')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('R')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('WheelUp')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('WheelDown')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('Middle+L')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('Middle+WheelUp')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('X1+L')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('X2+R')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('TopEdge+D')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('TopEdge+WheelDown')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('TopEdge+Left+D')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('BottomEdge+X1+L')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('Ctrl+Shift+U-R')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('Ctrl+WheelUp')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('invalid_xyz')).toBe(false);

    expect(assembleGestureCode({ bareCode: 'WheelUp' })).toBe('WheelUp');
    expect(assembleGestureCode({ bareCode: 'wheeldown' })).toBe('WheelDown');
    expect(assembleGestureCode({ triggerButton: 'middle', bareCode: 'WheelUp' })).toBe('Middle+WheelUp');

    expect(parseGestureCode('WheelUp').bareCode).toBe('WHEELUP');
    expect(parseGestureCode('TopEdge+Ctrl+WheelDown').edge).toBe('top');
    expect(parseGestureCode('TopEdge+Ctrl+WheelDown').hasCtrl).toBe(true);
    expect(parseGestureCode('TopEdge+Ctrl+WheelDown').bareCode).toBe('WHEELDOWN');
  });
});

describe('simplifyDirections and recognizeStrokes (World-Class Recognition & Anti-Folding)', () => {
  it('preserves staircase gestures (R-D-R, D-R-D, L-U-L) without collapsing them into a single direction', () => {
    // 关键回归测试：防止「向右-向下-向右」被错误吞噬为「向右」
    expect(simplifyDirections(['R', 'D', 'R'])).toEqual(['R', 'D', 'R']);
    expect(simplifyDirections(['D', 'R', 'D'])).toEqual(['D', 'R', 'D']);
    expect(simplifyDirections(['L', 'U', 'L'])).toEqual(['L', 'U', 'L']);
    expect(simplifyDirections(['U', 'R', 'U'])).toEqual(['U', 'R', 'U']);
    expect(simplifyDirections(['R', 'U', 'R'])).toEqual(['R', 'U', 'R']);
    expect(simplifyDirections(['D', 'L', 'D'])).toEqual(['D', 'L', 'D']);
    expect(simplifyDirections(['L', 'D', 'L'])).toEqual(['L', 'D', 'L']);
  });

  it('folds natural corner fillets while keeping the multi-stroke structure', () => {
    // 真实手写轨迹：转角处常附带对角过渡 (如 R -> DR -> D -> DR -> R)
    expect(simplifyDirections(['R', 'DR', 'D', 'DR', 'R'])).toEqual(['R', 'D', 'R']);
    expect(simplifyDirections(['D', 'DR', 'R'])).toEqual(['D', 'R']);
    expect(simplifyDirections(['R', 'DR', 'D'])).toEqual(['R', 'D']);
    expect(simplifyDirections(['D', 'DL', 'L'])).toEqual(['D', 'L']);
    expect(simplifyDirections(['U', 'UR', 'R'])).toEqual(['U', 'R']);
  });

  it('eliminates genuine 180° rebound jitter and 45° minor diagonal jitter', () => {
    expect(simplifyDirections(['R', 'L', 'R'])).toEqual(['R']);
    expect(simplifyDirections(['D', 'U', 'D'])).toEqual(['D']);
    expect(simplifyDirections(['R', 'UR', 'R'])).toEqual(['R']);
    expect(simplifyDirections(['D', 'DR', 'D'])).toEqual(['D']);
    expect(simplifyDirections(['L', 'DL', 'L'])).toEqual(['L']);
  });

  it('completes incomplete corner fillets when user releases on diagonal and trims trailing fillets', () => {
    expect(simplifyDirections(['D', 'DR'])).toEqual(['D', 'R']);
    expect(simplifyDirections(['R', 'DR'])).toEqual(['R', 'D']);
    expect(simplifyDirections(['D', 'R', 'DR'])).toEqual(['D', 'R']);
  });

  it('recognizes staircase point sequences into high-fidelity gesture codes', () => {
    // 阶梯轨迹: 右 -> 下 -> 右
    const rdrPoints = [
      { x: 0, y: 0 },
      { x: 30, y: 0 },
      { x: 60, y: 0 },
      { x: 60, y: 30 },
      { x: 60, y: 60 },
      { x: 90, y: 60 },
      { x: 120, y: 60 },
    ];
    expect(recognizeStrokes(rdrPoints)).toBe('R-D-R');

    // 经典截图手势: 下 -> 右 -> 下
    const drdPoints = [
      { x: 0, y: 0 },
      { x: 0, y: 30 },
      { x: 0, y: 60 },
      { x: 30, y: 60 },
      { x: 60, y: 60 },
      { x: 60, y: 90 },
      { x: 60, y: 120 },
    ];
    expect(recognizeStrokes(drdPoints)).toBe('D-R-D');

    // 经典折线: 左 -> 上 -> 左
    const lulPoints = [
      { x: 120, y: 60 },
      { x: 90, y: 60 },
      { x: 60, y: 60 },
      { x: 60, y: 30 },
      { x: 60, y: 0 },
      { x: 30, y: 0 },
      { x: 0, y: 0 },
    ];
    expect(recognizeStrokes(lulPoints)).toBe('L-U-L');

    // 真实手写平滑圆角弧度阶梯: 右 -> 下 -> 右 (含自然手腕过渡弧度)
    const smoothRdrPoints = [
      { x: 10, y: 10 },
      { x: 30, y: 11 },
      { x: 50, y: 12 },
      { x: 70, y: 15 },
      { x: 80, y: 25 },
      { x: 82, y: 45 },
      { x: 83, y: 65 },
      { x: 85, y: 85 },
      { x: 95, y: 95 },
      { x: 115, y: 98 },
      { x: 140, y: 100 },
      { x: 165, y: 101 },
    ];
    expect(recognizeStrokes(smoothRdrPoints)).toBe('R-D-R');
  });
});

describe('parseDirectionSegments and parseGesturePreview (U-06 regression)', () => {
  it('parses wheel gestures with edge and button prefixes without splitting letters into stroke segments', () => {
    const up = parseGesturePreview('WheelUp');
    expect(up.wheelDirection).toBe('up');
    expect(up.segments).toEqual([]);
    expect(up.edge).toBe('none');
    expect(up.triggerButton).toBe('right');

    const edgeDown = parseGesturePreview('TopEdge+Middle+WheelDown');
    expect(edgeDown.wheelDirection).toBe('down');
    expect(edgeDown.edge).toBe('top');
    expect(edgeDown.triggerButton).toBe('middle');
    expect(edgeDown.segments).toEqual([]);

    const rightExplicit = parseGesturePreview('RightEdge+Right+WheelUp');
    expect(rightExplicit.wheelDirection).toBe('up');
    expect(rightExplicit.edge).toBe('right');
    expect(rightExplicit.triggerButton).toBe('right');
    expect(rightExplicit.segments).toEqual([]);
  });

  it('strips all 4 screen edges and 5 mouse button prefixes cleanly before extracting direction segments', () => {
    expect(parseGesturePreview('LeftEdge+R')).toMatchObject({
      edge: 'left',
      triggerButton: 'right',
      wheelDirection: null,
      segments: ['R'],
    });
    expect(parseGesturePreview('BottomEdge+Left+U-R')).toMatchObject({
      edge: 'bottom',
      triggerButton: 'left',
      wheelDirection: null,
      segments: ['U', 'R'],
    });
    expect(parseGesturePreview('X1+L')).toMatchObject({
      edge: 'none',
      triggerButton: 'x1',
      segments: ['L'],
    });
    expect(parseGesturePreview('X2+D-R')).toMatchObject({
      edge: 'none',
      triggerButton: 'x2',
      segments: ['D', 'R'],
    });
    expect(parseGesturePreview('LU')).toMatchObject({
      wheelDirection: null,
      segments: ['LU'],
    });
  });

  it('never falls back unknown tokens into rightward segments', () => {
    expect(parseDirectionSegments('WHEELUP')).toEqual([]);
    expect(parseDirectionSegments('UNKNOWN_TOKEN')).toEqual([]);
    expect(parseDirectionSegments('R-BOGUS-D')).toEqual(['R', 'D']);
    expect(parseGesturePreview('InvalidGesture').segments).toEqual([]);
    expect(parseGesturePreview('InvalidGesture').wheelDirection).toBeNull();
  });

  it('supports LeftEdge/RightEdge wheel gestures and classifies all 5 trigger categories accurately (U-11)', () => {
    expect(GESTURE_CODE_PATTERN.test('LeftEdge+WheelUp')).toBe(true);
    expect(GESTURE_CODE_PATTERN.test('RightEdge+WheelDown')).toBe(true);
    expect(assembleGestureCode({ edge: 'left', bareCode: 'WheelUp' })).toBe('LeftEdge+WheelUp');
    expect(assembleGestureCode({ edge: 'right', bareCode: 'WheelDown' })).toBe('RightEdge+WheelDown');

    expect(matchesGestureCategory('R', 'right')).toBe(true);
    expect(matchesGestureCategory('R', 'middle')).toBe(false);
    expect(matchesGestureCategory('Middle+L', 'middle')).toBe(true);
    expect(matchesGestureCategory('X1+L', 'side')).toBe(true);
    expect(matchesGestureCategory('X2+R', 'side')).toBe(true);
    expect(matchesGestureCategory('X1+L', 'right')).toBe(false);
    expect(matchesGestureCategory('TopEdge+D', 'edge')).toBe(true);
    expect(matchesGestureCategory('TopEdge+D', 'right')).toBe(false);
    expect(matchesGestureCategory('LeftEdge+WheelUp', 'edge')).toBe(true);
    expect(matchesGestureCategory('LeftEdge+WheelUp', 'wheel')).toBe(true);
    expect(matchesGestureCategory('LeftEdge+WheelUp', 'right')).toBe(false);
  });

  it('resolves the required screen-edge trigger key for edge wheel and slide gestures', () => {
    expect(resolveRequiredEdgeTriggerKey('LeftEdge+WheelUp')).toBe('edge_left_wheel');
    expect(resolveRequiredEdgeTriggerKey('LeftEdge+WheelDown')).toBe('edge_left_wheel');
    expect(resolveRequiredEdgeTriggerKey('RightEdge+WheelUp')).toBe('edge_right_wheel');
    expect(resolveRequiredEdgeTriggerKey('TopEdge+WheelDown')).toBe('edge_top_wheel');
    expect(resolveRequiredEdgeTriggerKey('BottomEdge+WheelUp')).toBe('edge_bottom_wheel');
    expect(resolveRequiredEdgeTriggerKey('LeftEdge+R')).toBe('edge_left_slide');
    expect(resolveRequiredEdgeTriggerKey('TopEdge+D')).toBe('edge_top_slide');
    expect(resolveRequiredEdgeTriggerKey('RightEdge+L')).toBe('edge_right_slide');
    expect(resolveRequiredEdgeTriggerKey('BottomEdge+U')).toBe('edge_bottom_slide');
    expect(resolveRequiredEdgeTriggerKey('WheelUp')).toBeNull();
    expect(resolveRequiredEdgeTriggerKey('D-R')).toBeNull();
    expect(resolveRequiredEdgeTriggerKey('Middle+L')).toBeNull();
  });

  it('recognizes V shape (DR-UR) and Reverse C (R-D-L) arcs cleanly and avoids D vs DR-UR prefix false positives', () => {
    // 反 C 圆弧与末端偏斜归约
    expect(simplifyDirections(['R', 'DR', 'DL', 'L'])).toEqual(['R', 'D', 'L']);
    expect(simplifyDirections(['R', 'D', 'DL'])).toEqual(['R', 'D', 'L']);
    expect(simplifyDirections(['R', 'DR', 'L'])).toEqual(['R', 'D', 'L']);
    expect(simplifyDirections(['L', 'DL', 'DR', 'R'])).toEqual(['L', 'D', 'R']);

    // V 形对角抖动与底部微圆归约
    expect(simplifyDirections(['DR', 'D', 'DR', 'UR'])).toEqual(['DR', 'UR']);
    expect(simplifyDirections(['DR', 'R', 'UR'])).toEqual(['DR', 'UR']);
    expect(simplifyDirections(['UR', 'R', 'DR'])).toEqual(['UR', 'DR']);

    // 真实轨迹：60° 倾角 V 形与底部略圆 V 形
    const naturalV: { x: number; y: number }[] = [];
    for (let i = 0; i <= 20; i++) naturalV.push({ x: 80 + (i * 58) / 20, y: 40 + (i * 100) / 20 });
    for (let i = 1; i <= 20; i++) naturalV.push({ x: 138 + (i * 58) / 20, y: 140 - (i * 100) / 20 });
    expect(recognizeStrokes(naturalV)).toBe('DR-UR');

    const roundedV: { x: number; y: number }[] = [];
    for (let i = 0; i <= 15; i++) roundedV.push({ x: 60 + i * 4, y: 40 + i * 4 });
    for (let i = 1; i <= 5; i++) roundedV.push({ x: 120 + i * 4, y: 100 });
    for (let i = 1; i <= 15; i++) roundedV.push({ x: 140 + i * 4, y: 100 - i * 4 });
    expect(recognizeStrokes(roundedV)).toBe('DR-UR');

    // 实机用户画板 V 截图回归：左臂偏陡 (20.7° / 13.4°)、左臂带弧度 (D->DR->UR) 与对称窄印刷体 V
    const samplePolyline = (verts: { x: number; y: number }[], steps = 10) => {
      const pts: { x: number; y: number }[] = [];
      for (let e = 0; e + 1 < verts.length; e++) {
        const a = verts[e];
        const b = verts[e + 1];
        for (let i = e === 0 ? 0 : 1; i <= steps; i++) {
          pts.push({
            x: a.x + ((b.x - a.x) * i) / steps,
            y: a.y + ((b.y - a.y) * i) / steps,
          });
        }
      }
      return pts;
    };
    expect(recognizeStrokes(samplePolyline([{ x: 176, y: 46 }, { x: 210, y: 136 }, { x: 296, y: 34 }]))).toBe('DR-UR');
    expect(recognizeStrokes(samplePolyline([{ x: 200, y: 100 }, { x: 220, y: 184 }, { x: 309, y: 92 }]))).toBe('DR-UR');
    expect(recognizeStrokes(samplePolyline([{ x: 200, y: 100 }, { x: 219, y: 158 }, { x: 244, y: 183 }, { x: 336, y: 91 }]))).toBe('DR-UR');
    expect(recognizeStrokes(samplePolyline([{ x: 200, y: 100 }, { x: 238, y: 200 }, { x: 276, y: 100 }]))).toBe('DR-UR');
    // 保护日常往返 U-D 绝不误判为倒 V
    expect(recognizeStrokes(samplePolyline([{ x: 200, y: 200 }, { x: 210, y: 100 }, { x: 214, y: 205 }]))).toBe('U-D');

    // 真实轨迹：平滑半圆反 C
    const reverseC: { x: number; y: number }[] = [];
    for (let step = 0; step <= 32; step++) {
      const theta = -Math.PI * 0.5 + Math.PI * (step / 32);
      reverseC.push({
        x: 100 + Math.round(70 * Math.cos(theta)),
        y: 120 + Math.round(70 * Math.sin(theta)),
      });
    }
    expect(recognizeStrokes(reverseC)).toBe('R-D-L');

    // 段级前缀冲突检测：D 与 DR-UR 绝不误报，D 与 D-R 精准报冲突
    expect(isGesturePrefixConflict('D', 'DR-UR')).toBe(false);
    expect(isGesturePrefixConflict('D', 'DR')).toBe(false);
    expect(isGesturePrefixConflict('D', 'D-R')).toBe(true);
    expect(isGesturePrefixConflict('DR', 'DR-UR')).toBe(true);
    expect(isGesturePrefixConflict('Middle+L', 'L-U')).toBe(false);
    expect(isGesturePrefixConflict('Middle+L', 'Middle+L-U')).toBe(true);
  });
});



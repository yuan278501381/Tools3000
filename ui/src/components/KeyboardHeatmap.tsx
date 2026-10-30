import { type FC, useMemo, useState, useEffect } from 'react';
import './KeyboardHeatmap.css';
import { useTranslation } from 'react-i18next';
import { Flame, LayoutGrid, Keyboard } from 'lucide-react';
import { bridgeRequest } from '../hooks/useBridge';

interface KeyboardHeatmapProps {
  keyMap: Record<number, number>;
}

interface KeyDef {
  label: string;
  fullName?: string;
  vkCode: number | number[];
  flex?: number;
  gridArea?: string;
}

export type KeyboardLayoutMode = '104' | '87' | '60';

// ── 1. 主键区 (Main Alphanumeric Cluster - 15u) ─────────────────────────────────
const MAIN_F_ROW: (KeyDef | { gap: number })[] = [
  { label: 'Esc', fullName: 'Escape', vkCode: 0x1B, flex: 1 },
  { gap: 0.75 },
  { label: 'F1', vkCode: 0x70, flex: 1 },
  { label: 'F2', vkCode: 0x71, flex: 1 },
  { label: 'F3', vkCode: 0x72, flex: 1 },
  { label: 'F4', vkCode: 0x73, flex: 1 },
  { gap: 0.5 },
  { label: 'F5', vkCode: 0x74, flex: 1 },
  { label: 'F6', vkCode: 0x75, flex: 1 },
  { label: 'F7', vkCode: 0x76, flex: 1 },
  { label: 'F8', vkCode: 0x77, flex: 1 },
  { gap: 0.5 },
  { label: 'F9', vkCode: 0x78, flex: 1 },
  { label: 'F10', vkCode: 0x79, flex: 1 },
  { label: 'F11', vkCode: 0x7A, flex: 1 },
  { label: 'F12', vkCode: 0x7B, flex: 1 },
];

const MAIN_ALPHA_ROWS: KeyDef[][] = [
  // Numbers Row (15u)
  [
    { label: '`', fullName: 'Backquote / Tilde', vkCode: 0xC0, flex: 1 },
    { label: '1', vkCode: 0x31, flex: 1 }, { label: '2', vkCode: 0x32, flex: 1 },
    { label: '3', vkCode: 0x33, flex: 1 }, { label: '4', vkCode: 0x34, flex: 1 },
    { label: '5', vkCode: 0x35, flex: 1 }, { label: '6', vkCode: 0x36, flex: 1 },
    { label: '7', vkCode: 0x37, flex: 1 }, { label: '8', vkCode: 0x38, flex: 1 },
    { label: '9', vkCode: 0x39, flex: 1 }, { label: '0', vkCode: 0x30, flex: 1 },
    { label: '-', fullName: 'Minus', vkCode: 0xBD, flex: 1 },
    { label: '=', fullName: 'Equals', vkCode: 0xBB, flex: 1 },
    { label: 'Backspace', fullName: 'Backspace', vkCode: 0x08, flex: 2 }
  ],
  // QWERTY Row (15u)
  [
    { label: 'Tab', fullName: 'Tab', vkCode: 0x09, flex: 1.5 },
    { label: 'Q', vkCode: 0x51, flex: 1 }, { label: 'W', vkCode: 0x57, flex: 1 },
    { label: 'E', vkCode: 0x45, flex: 1 }, { label: 'R', vkCode: 0x52, flex: 1 },
    { label: 'T', vkCode: 0x54, flex: 1 }, { label: 'Y', vkCode: 0x59, flex: 1 },
    { label: 'U', vkCode: 0x55, flex: 1 }, { label: 'I', vkCode: 0x49, flex: 1 },
    { label: 'O', vkCode: 0x4F, flex: 1 }, { label: 'P', vkCode: 0x50, flex: 1 },
    { label: '[', fullName: 'Left Bracket', vkCode: 0xDB, flex: 1 },
    { label: ']', fullName: 'Right Bracket', vkCode: 0xDD, flex: 1 },
    { label: '\\', fullName: 'Backslash', vkCode: 0xDC, flex: 1.5 }
  ],
  // ASDF Row (15u)
  [
    { label: 'Caps', fullName: 'Caps Lock', vkCode: 0x14, flex: 1.75 },
    { label: 'A', vkCode: 0x41, flex: 1 }, { label: 'S', vkCode: 0x53, flex: 1 },
    { label: 'D', vkCode: 0x44, flex: 1 }, { label: 'F', vkCode: 0x46, flex: 1 },
    { label: 'G', vkCode: 0x47, flex: 1 }, { label: 'H', vkCode: 0x48, flex: 1 },
    { label: 'J', vkCode: 0x4A, flex: 1 }, { label: 'K', vkCode: 0x4B, flex: 1 },
    { label: 'L', vkCode: 0x4C, flex: 1 }, { label: ';', fullName: 'Semicolon', vkCode: 0xBA, flex: 1 },
    { label: "'", fullName: 'Quote', vkCode: 0xDE, flex: 1 },
    { label: 'Enter', fullName: 'Enter', vkCode: 0x0D, flex: 2.25 }
  ],
  // ZXCV Row (15u)
  [
    { label: 'Shift', fullName: 'Left Shift', vkCode: [0xA0, 0x10], flex: 2.25 },
    { label: 'Z', vkCode: 0x5A, flex: 1 }, { label: 'X', vkCode: 0x58, flex: 1 },
    { label: 'C', vkCode: 0x43, flex: 1 }, { label: 'V', vkCode: 0x56, flex: 1 },
    { label: 'B', vkCode: 0x42, flex: 1 }, { label: 'N', vkCode: 0x4E, flex: 1 },
    { label: 'M', vkCode: 0x4D, flex: 1 }, { label: ',', fullName: 'Comma', vkCode: 0xBC, flex: 1 },
    { label: '.', fullName: 'Period', vkCode: 0xBE, flex: 1 }, { label: '/', fullName: 'Slash', vkCode: 0xBF, flex: 1 },
    { label: 'Shift', fullName: 'Right Shift', vkCode: [0xA1], flex: 2.75 }
  ],
  // Bottom Row (15u)
  [
    { label: 'Ctrl', fullName: 'Left Control', vkCode: [0xA2, 0x11], flex: 1.25 },
    { label: 'Win', fullName: 'Windows Key', vkCode: 0x5B, flex: 1.25 },
    { label: 'Alt', fullName: 'Left Alt', vkCode: [0xA4, 0x12], flex: 1.25 },
    { label: 'Space', fullName: 'Spacebar', vkCode: 0x20, flex: 6.25 },
    { label: 'Alt', fullName: 'Right Alt', vkCode: 0xA5, flex: 1.25 },
    { label: 'Win', fullName: 'Right Windows', vkCode: 0x5C, flex: 1.25 },
    { label: 'Menu', fullName: 'Context Menu', vkCode: 0x5D, flex: 1.25 },
    { label: 'Ctrl', fullName: 'Right Control', vkCode: 0xA3, flex: 1.25 }
  ]
];

// ── 2. 编辑与方向键区 (Navigation / Editing / Arrows Cluster - 3u) ────────────
const NAV_SYS_ROW: KeyDef[] = [
  { label: 'PrtSc', fullName: 'Print Screen', vkCode: 0x2C, flex: 1 },
  { label: 'ScrLk', fullName: 'Scroll Lock', vkCode: 0x91, flex: 1 },
  { label: 'Pause', fullName: 'Pause Break', vkCode: 0x13, flex: 1 }
];

const NAV_EDIT_ROWS: KeyDef[][] = [
  [
    { label: 'Ins', fullName: 'Insert', vkCode: 0x2D, flex: 1 },
    { label: 'Home', fullName: 'Home', vkCode: 0x24, flex: 1 },
    { label: 'PgUp', fullName: 'Page Up', vkCode: 0x21, flex: 1 }
  ],
  [
    { label: 'Del', fullName: 'Delete', vkCode: 0x2E, flex: 1 },
    { label: 'End', fullName: 'End', vkCode: 0x23, flex: 1 },
    { label: 'PgDn', fullName: 'Page Down', vkCode: 0x22, flex: 1 }
  ]
];

const NAV_ARROW_ROWS: (KeyDef | { empty: true })[][] = [
  [
    { empty: true },
    { label: '↑', fullName: 'Up Arrow', vkCode: 0x26, flex: 1 },
    { empty: true }
  ],
  [
    { label: '←', fullName: 'Left Arrow', vkCode: 0x25, flex: 1 },
    { label: '↓', fullName: 'Down Arrow', vkCode: 0x28, flex: 1 },
    { label: '→', fullName: 'Right Arrow', vkCode: 0x27, flex: 1 }
  ]
];

// ── 3. 数字小键盘区 (Numpad Cluster - 4u Grid) ────────────────────────────────
const NUMPAD_GRID_KEYS: (KeyDef & { gridRow?: string; gridCol?: string })[] = [
  // Row 1
  { label: 'Num', fullName: 'Num Lock', vkCode: 0x90, gridRow: '1', gridCol: '1' },
  { label: '/', fullName: 'Numpad /', vkCode: 0x6F, gridRow: '1', gridCol: '2' },
  { label: '*', fullName: 'Numpad *', vkCode: 0x6A, gridRow: '1', gridCol: '3' },
  { label: '-', fullName: 'Numpad -', vkCode: 0x6D, gridRow: '1', gridCol: '4' },
  // Row 2
  { label: '7', fullName: 'Numpad 7', vkCode: 0x67, gridRow: '2', gridCol: '1' },
  { label: '8', fullName: 'Numpad 8', vkCode: 0x68, gridRow: '2', gridCol: '2' },
  { label: '9', fullName: 'Numpad 9', vkCode: 0x69, gridRow: '2', gridCol: '3' },
  { label: '+', fullName: 'Numpad +', vkCode: 0x6B, gridRow: '2 / span 2', gridCol: '4' },
  // Row 3
  { label: '4', fullName: 'Numpad 4', vkCode: 0x64, gridRow: '3', gridCol: '1' },
  { label: '5', fullName: 'Numpad 5', vkCode: 0x65, gridRow: '3', gridCol: '2' },
  { label: '6', fullName: 'Numpad 6', vkCode: 0x66, gridRow: '3', gridCol: '3' },
  // Row 4
  { label: '1', fullName: 'Numpad 1', vkCode: 0x61, gridRow: '4', gridCol: '1' },
  { label: '2', fullName: 'Numpad 2', vkCode: 0x62, gridRow: '4', gridCol: '2' },
  { label: '3', fullName: 'Numpad 3', vkCode: 0x63, gridRow: '4', gridCol: '3' },
  { label: 'Enter', fullName: 'Numpad Enter', vkCode: [0x0D], gridRow: '4 / span 2', gridCol: '4' },
  // Row 5
  { label: '0', fullName: 'Numpad 0', vkCode: 0x60, gridRow: '5', gridCol: '1 / span 2' },
  { label: '.', fullName: 'Numpad .', vkCode: 0x6E, gridRow: '5', gridCol: '3' },
];

function formatMicroCount(num: number): string {
  if (num <= 0) return '';
  if (num >= 10000) return `${(num / 1000).toFixed(0)}k`;
  if (num >= 1000) return `${(num / 1000).toFixed(1)}k`;
  return String(num);
}

interface KeyHeatStyle {
  background: string;
  borderColor: string;
  color: string;
  subColor: string;
  boxShadow: string;
  level: number;
}

function computeKeyHeatStyle(count: number, intensity: number): KeyHeatStyle {
  if (count <= 0) {
    return {
      background: 'var(--key-bg-idle)',
      borderColor: 'var(--key-border-idle)',
      color: 'var(--key-text-idle)',
      subColor: 'transparent',
      boxShadow: 'var(--key-shadow-idle)',
      level: 0,
    };
  }

  // 优雅的平方根非线性感知色阶映射
  const s = Math.min(1, Math.max(0, Math.pow(intensity, 0.45)));

  if (s < 0.28) {
    const t = s / 0.28;
    return {
      background: `rgba(139, 92, 246, ${0.14 + t * 0.16})`,
      borderColor: `rgba(167, 139, 250, ${0.3 + t * 0.25})`,
      color: 'var(--key-text-level1)',
      subColor: 'var(--key-subtext-level1)',
      boxShadow: `0 2px 0 rgba(0,0,0,0.06), 0 0 ${4 + t * 6}px rgba(139, 92, 246, ${0.12 + t * 0.15})`,
      level: 1,
    };
  } else if (s < 0.60) {
    const t = (s - 0.28) / 0.32;
    return {
      background: `linear-gradient(135deg, rgba(147, 51, 234, ${0.42 + t * 0.22}), rgba(192, 38, 211, ${0.32 + t * 0.22}))`,
      borderColor: `rgba(216, 180, 254, ${0.55 + t * 0.3})`,
      color: 'var(--key-text-level2)',
      subColor: 'var(--key-subtext-level2)',
      boxShadow: `0 2px 0 rgba(0,0,0,0.1), 0 0 ${8 + t * 8}px rgba(168, 85, 247, ${0.28 + t * 0.2})`,
      level: 2,
    };
  } else if (s < 0.88) {
    const t = (s - 0.60) / 0.28;
    return {
      background: `linear-gradient(135deg, rgba(236, 72, 153, ${0.75 + t * 0.2}), rgba(249, 115, 22, ${0.8 + t * 0.2}))`,
      borderColor: `rgba(253, 186, 116, ${0.7 + t * 0.3})`,
      color: '#ffffff',
      subColor: 'rgba(255, 255, 255, 0.9)',
      boxShadow: `0 2px 0 rgba(0,0,0,0.14), 0 0 ${12 + t * 8}px rgba(249, 115, 22, ${0.38 + t * 0.2})`,
      level: 3,
    };
  } else {
    // 巅峰热力峰值
    const t = (s - 0.88) / 0.12;
    return {
      background: `linear-gradient(135deg, #f43f5e, #f59e0b)`,
      borderColor: '#fef08a',
      color: '#ffffff',
      subColor: 'rgba(255, 255, 255, 0.98)',
      boxShadow: `0 2px 0 rgba(180, 83, 9, 0.6), 0 0 ${16 + t * 10}px rgba(245, 158, 11, 0.6)`,
      level: 4,
    };
  }
}

export const KeyboardHeatmap: FC<KeyboardHeatmapProps> = ({ keyMap }) => {
  const { t } = useTranslation();
  const [layoutMode, setLayoutMode] = useState<KeyboardLayoutMode>('104');

  // 物理键盘硬件锁定状态联动 (NUM / CAPS / SCROLL)
  const [lockStates, setLockStates] = useState({
    numLock: true,
    capsLock: false,
    scrollLock: false
  });

  useEffect(() => {
    const syncLockStates = async () => {
      try {
        const res = await bridgeRequest<{ numLock: boolean; capsLock: boolean; scrollLock: boolean }>('stats.getKeyboardLockStates');
        if (res && typeof res.numLock === 'boolean') {
          setLockStates({
            numLock: res.numLock,
            capsLock: res.capsLock,
            scrollLock: res.scrollLock,
          });
        }
      } catch {
        // browser fallback
      }
    };

    void syncLockStates();

    const handleKeyActivity = (e: KeyboardEvent) => {
      setLockStates({
        numLock: e.getModifierState('NumLock'),
        capsLock: e.getModifierState('CapsLock'),
        scrollLock: e.getModifierState('ScrollLock'),
      });
    };

    window.addEventListener('keydown', handleKeyActivity);
    window.addEventListener('keyup', handleKeyActivity);
    window.addEventListener('focus', syncLockStates);

    return () => {
      window.removeEventListener('keydown', handleKeyActivity);
      window.removeEventListener('keyup', handleKeyActivity);
      window.removeEventListener('focus', syncLockStates);
    };
  }, []);

  // 计算今日总击键数
  const totalKeystrokes = useMemo(() => {
    return Object.values(keyMap).reduce((sum, v) => sum + v, 0);
  }, [keyMap]);

  // 计算最高频按键 (全键盘所有区域综合统计)
  const { maxCount, topKey } = useMemo(() => {
    let max = 0;
    let best = { label: '', fullName: '', count: 0 };

    const inspectKey = (k: KeyDef) => {
      const count = Array.isArray(k.vkCode)
        ? k.vkCode.reduce((sum, code) => sum + (keyMap[code] || 0), 0)
        : keyMap[k.vkCode] || 0;
      if (count > max) max = count;
      if (count > best.count) {
        best = { label: k.label, fullName: k.fullName || k.label, count };
      }
    };

    MAIN_F_ROW.forEach((k) => { if ('label' in k) inspectKey(k); });
    MAIN_ALPHA_ROWS.forEach((row) => row.forEach(inspectKey));
    NAV_SYS_ROW.forEach(inspectKey);
    NAV_EDIT_ROWS.forEach((row) => row.forEach(inspectKey));
    NAV_ARROW_ROWS.forEach((row) => row.forEach((k) => { if ('label' in k) inspectKey(k); }));
    NUMPAD_GRID_KEYS.forEach(inspectKey);

    return { maxCount: Math.max(max, 5), topKey: best };
  }, [keyMap]);

  const getHeatData = (keyDef: KeyDef) => {
    const count = Array.isArray(keyDef.vkCode)
      ? keyDef.vkCode.reduce((sum, code) => sum + (keyMap[code] || 0), 0)
      : keyMap[keyDef.vkCode] || 0;
    const intensity = count / maxCount;
    const percentage = totalKeystrokes > 0 ? ((count / totalKeystrokes) * 100).toFixed(1) : '0.0';
    const style = computeKeyHeatStyle(count, intensity);
    return { count, intensity, percentage, style };
  };

  const renderKeyCap = (
    keyDef: KeyDef,
    customClass = '',
    customStyle: React.CSSProperties = {},
    tooltipPlacement: 'top' | 'bottom' = 'top'
  ) => {
    const { count, percentage, style } = getHeatData(keyDef);
    const keyIdentity = keyDef.fullName || keyDef.label;
    const isPeak = topKey.count > 0 && (topKey.fullName || topKey.label) === keyIdentity;

    // 是否处于键盘锁定激活态 (如 Caps 开启时高亮 Caps 键，Num 开启时高亮 Num 键)
    const isLockedOn =
      (keyDef.label === 'Caps' && lockStates.capsLock) ||
      (keyDef.label === 'Num' && lockStates.numLock) ||
      (keyDef.label === 'ScrLk' && lockStates.scrollLock);

    return (
      <div
        key={keyIdentity}
        className={`keyboard-keycap ${customClass} ${count > 0 ? 'has-heat' : ''} ${isPeak ? 'is-peak' : ''} ${isLockedOn ? 'is-locked-on' : ''}`}
        style={{
          flex: keyDef.flex !== undefined ? `${keyDef.flex} 1 0%` : undefined,
          background: style.background,
          borderColor: isLockedOn ? 'var(--success)' : style.borderColor,
          color: style.color,
          boxShadow: isLockedOn ? '0 0 8px rgba(16, 185, 129, 0.45)' : style.boxShadow,
          ...customStyle,
        }}
      >
        <span className="keyboard-keycap__label">{keyDef.label}</span>
        {count > 0 && (
          <span
            className="keyboard-keycap__count-pill"
            style={{ color: style.subColor }}
          >
            {formatMicroCount(count)}
          </span>
        )}
        {isPeak && <span className="keyboard-keycap__crown"><Flame size={10} color="#f97316" /></span>}

        {/* 浮动智能自适应轻量提示卡片 (顶部两行向下翻转，彻底杜绝顶部裁切与遮挡) */}
        <div className={`keyboard-heatmap__tooltip keyboard-heatmap__tooltip--${tooltipPlacement}`}>
          <div className="tooltip-header">
            <span className="tooltip-key-name">{keyDef.fullName || keyDef.label}</span>
            {isPeak && <span className="tooltip-badge-peak">{t('components.peakToday', 'Today\'s Peak')}</span>}
            {isLockedOn && <span className="tooltip-badge-lock">{t('components.lockedOn', 'Locked On')}</span>}
          </div>
          <div className="tooltip-body">
            <span className="tooltip-count">
              {t('heatmap.keystrokesUnit', { defaultValue: '{{count}} keystrokes', count: Number(count) })}
            </span>
            {count > 0 && (
              <span className="tooltip-pct">{t('heatmap.ratioUnit', 'Ratio {{pct}}%', { pct: percentage })}</span>
            )}
          </div>
        </div>
      </div>
    );
  };

  return (
    <div className="keyboard-heatmap-wrapper">
      {/* 顶部工具栏: 布局规格切换器 */}
      <div className="keyboard-heatmap__header-bar">
        <div className="keyboard-heatmap__title-tag">
          <Keyboard size={15} className="keyboard-heatmap__title-icon" />
          <span>{t('heatmap.distributionTitle', 'Keyboard Heatmap Distribution ({{layout}})', { layout: layoutMode === '104' ? t('heatmap.layout104', '104-Key Full') : layoutMode === '87' ? t('heatmap.layout87', '87-Key TKL') : t('heatmap.layout60', '60-Key Compact') })}</span>
        </div>

        <div className="keyboard-layout-segmented">
          <button
            type="button"
            className={`keyboard-layout-btn ${layoutMode === '104' ? 'active' : ''}`}
            onClick={() => setLayoutMode('104')}
          >
            <LayoutGrid size={12} />
            <span>{t('components.kb104', '104 Full Keyboard')}</span>
          </button>
          <button
            type="button"
            className={`keyboard-layout-btn ${layoutMode === '87' ? 'active' : ''}`}
            onClick={() => setLayoutMode('87')}
          >
            <span>{t('components.kb87', '87-Key TKL')}</span>
          </button>
          <button
            type="button"
            className={`keyboard-layout-btn ${layoutMode === '60' ? 'active' : ''}`}
            onClick={() => setLayoutMode('60')}
          >
            <span>{t('components.kb60', '60-Key Compact')}</span>
          </button>
        </div>
      </div>

      {/* 机械键盘精工盘体框架 (overflow-visible 确保气泡与阴影完整无遮挡) */}
      <div className={`keyboard-chassis keyboard-chassis--${layoutMode}`}>
        {/* ── 1. 主键区 (Main Cluster 15u) ── */}
        <div className="keyboard-cluster keyboard-cluster--main">
          {/* F 功能键行 (Tooltip 向下翻转) */}
          <div className="keyboard-row keyboard-row--f-main">
            {MAIN_F_ROW.map((item, idx) => {
              if ('gap' in item) {
                return (
                  <div
                    key={`gap-${idx}`}
                    className="keyboard-gap"
                    style={{ flex: `${item.gap} 1 0%` }}
                  />
                );
              }
              return renderKeyCap(item, 'keycap--f-row', {}, 'bottom');
            })}
          </div>

          <div className="keyboard-section-gap" />

          {/* 打字主键区 5 行 (第一行数字行向下翻转，其余向上翻转) */}
          <div className="keyboard-alpha-block">
            {MAIN_ALPHA_ROWS.map((row, rIdx) => (
              <div key={rIdx} className="keyboard-row">
                {row.map((k) => renderKeyCap(k, '', {}, rIdx === 0 ? 'bottom' : 'top'))}
              </div>
            ))}
          </div>
        </div>

        {/* ── 2. 编辑与方向键区 (Nav Cluster 3u) ── */}
        {layoutMode !== '60' && (
          <div className="keyboard-cluster keyboard-cluster--nav">
            {/* 系统键行 (PrtSc/ScrLk/Pause, Tooltip 向下翻转) */}
            <div className="keyboard-row keyboard-row--f-nav">
              {NAV_SYS_ROW.map((k) => renderKeyCap(k, 'keycap--f-row keycap--nav-sys', {}, 'bottom'))}
            </div>

            <div className="keyboard-section-gap" />

            {/* 编辑功能键 2 行 (Ins/Home/PgUp 向下翻转，Del/End/PgDn 向上翻转) */}
            <div className="keyboard-edit-block">
              {NAV_EDIT_ROWS.map((row, rIdx) => (
                <div key={rIdx} className="keyboard-row">
                  {row.map((k) => renderKeyCap(k, 'keycap--nav-edit', {}, rIdx === 0 ? 'bottom' : 'top'))}
                </div>
              ))}
            </div>

            <div className="keyboard-arrow-divider" />

            {/* 倒 T 型方向键区 (↑, ←↓→, Tooltip 向上翻转) */}
            <div className="keyboard-arrow-block">
              {NAV_ARROW_ROWS.map((row, rIdx) => (
                <div key={rIdx} className="keyboard-row">
                  {row.map((item, cIdx) => {
                    if ('empty' in item) {
                      return <div key={`empty-${cIdx}`} className="keyboard-gap" style={{ flex: '1 1 0%' }} />;
                    }
                    return renderKeyCap(item, 'keycap--arrow', {}, 'top');
                  })}
                </div>
              ))}
            </div>
          </div>
        )}

        {/* ── 3. 数字小键盘区 (Numpad Cluster 4u) ── */}
        {layoutMode === '104' && (
          <div className="keyboard-cluster keyboard-cluster--numpad">
            {/* 小键盘顶部指示灯面板 (与真实硬件键盘实时双向联动) */}
            <div className="keyboard-numpad-status">
              <div className="numpad-led-badge">
                <span className={`numpad-led-dot ${lockStates.numLock ? 'active' : ''}`} />
                <span className="numpad-led-label">NUM</span>
              </div>
              <div className="numpad-led-badge">
                <span className={`numpad-led-dot ${lockStates.capsLock ? 'active' : ''}`} />
                <span className="numpad-led-label">CAPS</span>
              </div>
              <div className="numpad-led-badge">
                <span className={`numpad-led-dot ${lockStates.scrollLock ? 'active' : ''}`} />
                <span className="numpad-led-label">SCROLL</span>
              </div>
            </div>

            <div className="keyboard-section-gap" />

            {/* 标准 4 列 5 行小键盘网格 */}
            <div className="keyboard-numpad-grid">
              {NUMPAD_GRID_KEYS.map((k) =>
                renderKeyCap(k, 'keycap--numpad', {
                  gridRow: k.gridRow,
                  gridColumn: k.gridCol,
                }, k.gridRow === '1' ? 'bottom' : 'top')
              )}
            </div>
          </div>
        )}
      </div>

      {/* 底部专业热力能谱图例栏 & 常驻今日最高频指标 */}
      <div className="keyboard-heatmap__footer">
        <div className="keyboard-heatmap__legend">
          <span className="legend-label">{t('stats.lowHeat', 'Idle / 0')}</span>
          <div className="legend-spectrum">
            <div className="spectrum-bar" />
          </div>
          <span className="legend-label">{t('stats.highHeat', 'Peak')}</span>
        </div>

        {topKey.count > 0 ? (
          <div className="keyboard-heatmap__top-badge">
            <div className="top-badge-icon-box">
              <Flame size={12} strokeWidth={2.5} className="top-badge-flame-icon" />
            </div>
            <div className="top-badge-content">
              <span className="top-badge-label">{t('stats.topKey', 'Top Key Today')}:</span>
              <span className="top-badge-key-name">{topKey.fullName || topKey.label}</span>
              <span className="top-badge-stat">
                <span className="top-badge-count">{topKey.count.toLocaleString()}</span>
                <span className="top-badge-unit">{t('stats.times', 'times')}</span>
                <span className="top-badge-divider">·</span>
                <span className="top-badge-pct">{((topKey.count / (totalKeystrokes || 1)) * 100).toFixed(1)}%</span>
              </span>
            </div>
          </div>
        ) : (
          <div className="keyboard-heatmap__top-badge keyboard-heatmap__top-badge--idle">
            <div className="top-badge-icon-box top-badge-icon-box--idle">
              <Keyboard size={12} strokeWidth={2} />
            </div>
            <div className="top-badge-content">
              <span className="top-badge-label">{t('components.todayKeystrokes', 'Today\'s Keystrokes:')}</span>
              <span className="top-badge-key-name">{t('components.noDataRecorded', 'No data recorded')}</span>
            </div>
          </div>
        )}
      </div>
    </div>
  );
};

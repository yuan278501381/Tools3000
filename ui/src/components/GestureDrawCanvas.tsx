/* ─────────────────────────────────────────────────────────────────────────────
 * GestureDrawCanvas — 交互式手势录制与识别画板
 *
 * 核心交互设计:
 *   - 显式触发区域分段胶囊选择器:
 *     默认「全局任意位置 (Global)」模式下，在画板任意坐标（含最左侧/最顶部）起笔划动
 *     一律 100% 录为纯正全局手势，彻底根除隐形 15% 边缘误录痛点
 *   - 仅当用户主动切换至「顶边 / 底边 / 左边 / 右边」模式时，画板对应侧绘制高对比度
 *     虚线感应带并联动生成对应屏幕边缘手势前缀
 *   - 硬件鼠标按键实时自动感应 + 可点击切换，键盘修饰键 (Ctrl/Shift/Alt) 实时侦测
 *   - 100% 纯高精度矢量 SVG 图标体系 (零 Unicode Emoji / 零几何替代字符)
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useRef, useCallback, useEffect, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { RotateCcw, MousePointer, Check, Globe } from 'lucide-react';
import { MouseButtonSvg, ScreenEdgeSvg } from './MouseButtonSvg';
import {
  codeToArrows,
  parseGestureCode,
  assembleGestureCode,
  type TriggerButton,
  type ScreenEdge,
  type Point,
  calculateDistance,
  recognizeStrokes,
} from './gestureModel';
import './GestureDrawCanvas.css';

function pointsToSvgPath(points: Point[]): string {
  if (points.length === 0) return '';
  if (points.length === 1) return `M ${points[0].x} ${points[0].y}`;
  if (points.length === 2) return `M ${points[0].x} ${points[0].y} L ${points[1].x} ${points[1].y}`;

  let d = `M ${points[0].x} ${points[0].y}`;
  for (let i = 1; i < points.length - 1; i++) {
    const p1 = points[i];
    const p2 = points[i + 1];
    const midX = (p1.x + p2.x) / 2;
    const midY = (p1.y + p2.y) / 2;
    d += ` Q ${p1.x} ${p1.y}, ${midX} ${midY}`;
  }
  const last = points[points.length - 1];
  d += ` L ${last.x} ${last.y}`;
  return d;
}

interface PresetGestureItem {
  code: string;
  labelKey: string;
  defaultLabel: string;
}

const RIGHT_PRESET_GESTURES: PresetGestureItem[] = [
  { code: 'WheelUp', labelKey: 'gesture.actions.prevVirtualDesktop.name', defaultLabel: 'Prev Desktop' },
  { code: 'WheelDown', labelKey: 'gesture.actions.nextVirtualDesktop.name', defaultLabel: 'Next Desktop' },
  { code: 'R', labelKey: 'gesture.actions.forward.name', defaultLabel: 'Forward' },
  { code: 'L', labelKey: 'gesture.actions.back.name', defaultLabel: 'Back' },
  { code: 'D-R', labelKey: 'gesture.actions.closeTab.name', defaultLabel: 'Close Tab' },
  { code: 'R-D', labelKey: 'gesture.actions.reopenTab.name', defaultLabel: 'Reopen Tab' },
  { code: 'D-L', labelKey: 'gesture.actions.closeWindow.name', defaultLabel: 'Close Window' },
  { code: 'U', labelKey: 'gesture.actions.maxRestore.name', defaultLabel: 'Maximize' },
  { code: 'D', labelKey: 'gesture.actions.minimize.name', defaultLabel: 'Minimize' },
  { code: 'U-R', labelKey: 'gesture.actions.nextTab.name', defaultLabel: 'Next Tab' },
  { code: 'U-L', labelKey: 'gesture.actions.prevTab.name', defaultLabel: 'Previous Tab' },
  { code: 'L-U-R', labelKey: 'gesture.actions.refresh.name', defaultLabel: 'Reload' },
  { code: 'L-D', labelKey: 'gesture.actions.showDesktop.name', defaultLabel: 'Show Desktop' },
  { code: 'D-R-D', labelKey: 'gesture.actions.screenshot.name', defaultLabel: 'Screenshot' },
];

const MIDDLE_PRESET_GESTURES: PresetGestureItem[] = [
  { code: 'Middle+L', labelKey: 'gesture.actions.prevTrack.name', defaultLabel: 'Previous Track' },
  { code: 'Middle+R', labelKey: 'gesture.actions.nextTrack.name', defaultLabel: 'Next Track' },
  { code: 'Middle+U', labelKey: 'gesture.actions.maxRestore.name', defaultLabel: 'Maximize' },
  { code: 'Middle+D', labelKey: 'gesture.actions.minimize.name', defaultLabel: 'Minimize' },
  { code: 'Middle+D-R', labelKey: 'gesture.actions.closeTab.name', defaultLabel: 'Close Tab' },
  { code: 'Middle+R-D', labelKey: 'gesture.actions.reopenTab.name', defaultLabel: 'Reopen Tab' },
  { code: 'Middle+L-U-R', labelKey: 'gesture.actions.refresh.name', defaultLabel: 'Reload' },
  { code: 'Middle+L-D', labelKey: 'gesture.actions.showDesktop.name', defaultLabel: 'Show Desktop' },
];

const SIDE1_PRESET_GESTURES: PresetGestureItem[] = [
  { code: 'X1+L', labelKey: 'gesture.actions.back.name', defaultLabel: 'Back' },
  { code: 'X1+R', labelKey: 'gesture.actions.forward.name', defaultLabel: 'Forward' },
  { code: 'X1+U', labelKey: 'gesture.actions.nextTab.name', defaultLabel: 'Next Tab' },
  { code: 'X1+D', labelKey: 'gesture.actions.prevTab.name', defaultLabel: 'Previous Tab' },
  { code: 'X1+D-R', labelKey: 'gesture.actions.closeTab.name', defaultLabel: 'Close Tab' },
  { code: 'X1+R-D', labelKey: 'gesture.actions.reopenTab.name', defaultLabel: 'Reopen Tab' },
  { code: 'X1+L-U-R', labelKey: 'gesture.actions.refresh.name', defaultLabel: 'Reload' },
];

const SIDE2_PRESET_GESTURES: PresetGestureItem[] = [
  { code: 'X2+L', labelKey: 'gesture.actions.prevTrack.name', defaultLabel: 'Previous Track' },
  { code: 'X2+R', labelKey: 'gesture.actions.nextTrack.name', defaultLabel: 'Next Track' },
  { code: 'X2+U', labelKey: 'gesture.actions.volumeUp.name', defaultLabel: 'Volume Up' },
  { code: 'X2+D', labelKey: 'gesture.actions.volumeDown.name', defaultLabel: 'Volume Down' },
  { code: 'X2+D-R', labelKey: 'gesture.actions.closeWindow.name', defaultLabel: 'Close Window' },
  { code: 'X2+L-D', labelKey: 'gesture.actions.showDesktop.name', defaultLabel: 'Show Desktop' },
];

const TOPEDGE_PRESET_GESTURES: PresetGestureItem[] = [
  { code: 'TopEdge+D', labelKey: 'gesture.actions.taskView.name', defaultLabel: 'Task View / Desktop' },
  { code: 'TopEdge+L', labelKey: 'gesture.actions.prevVirtualDesktop.name', defaultLabel: 'Previous Virtual Desktop' },
  { code: 'TopEdge+R', labelKey: 'gesture.actions.nextVirtualDesktop.name', defaultLabel: 'Next Virtual Desktop' },
  { code: 'TopEdge+WheelUp', labelKey: 'gesture.actions.prevVirtualDesktop.name', defaultLabel: 'Previous Virtual Desktop' },
  { code: 'TopEdge+WheelDown', labelKey: 'gesture.actions.nextVirtualDesktop.name', defaultLabel: 'Next Virtual Desktop' },
];

const BOTTOMEDGE_PRESET_GESTURES: PresetGestureItem[] = [
  { code: 'BottomEdge+U', labelKey: 'gesture.actions.taskView.name', defaultLabel: 'Task View' },
  { code: 'BottomEdge+L', labelKey: 'gesture.actions.prevVirtualDesktop.name', defaultLabel: 'Previous Virtual Desktop' },
  { code: 'BottomEdge+R', labelKey: 'gesture.actions.nextVirtualDesktop.name', defaultLabel: 'Next Virtual Desktop' },
  { code: 'BottomEdge+WheelUp', labelKey: 'gesture.actions.volumeUp.name', defaultLabel: 'Volume Up' },
  { code: 'BottomEdge+WheelDown', labelKey: 'gesture.actions.volumeDown.name', defaultLabel: 'Volume Down' },
];

const LEFTEDGE_PRESET_GESTURES: PresetGestureItem[] = [
  { code: 'LeftEdge+R', labelKey: 'gesture.actions.back.name', defaultLabel: 'Back' },
  { code: 'LeftEdge+U', labelKey: 'gesture.actions.volumeUp.name', defaultLabel: 'Volume Up' },
  { code: 'LeftEdge+D', labelKey: 'gesture.actions.volumeDown.name', defaultLabel: 'Volume Down' },
  { code: 'LeftEdge+WheelUp', labelKey: 'gesture.actions.volumeUp.name', defaultLabel: 'Volume Up' },
  { code: 'LeftEdge+WheelDown', labelKey: 'gesture.actions.volumeDown.name', defaultLabel: 'Volume Down' },
];

const RIGHTEDGE_PRESET_GESTURES: PresetGestureItem[] = [
  { code: 'RightEdge+L', labelKey: 'gesture.actions.forward.name', defaultLabel: 'Forward' },
  { code: 'RightEdge+U', labelKey: 'gesture.actions.volumeUp.name', defaultLabel: 'Volume Up' },
  { code: 'RightEdge+D', labelKey: 'gesture.actions.volumeDown.name', defaultLabel: 'Volume Down' },
  { code: 'RightEdge+WheelUp', labelKey: 'gesture.actions.prevVirtualDesktop.name', defaultLabel: 'Previous Virtual Desktop' },
  { code: 'RightEdge+WheelDown', labelKey: 'gesture.actions.nextVirtualDesktop.name', defaultLabel: 'Next Virtual Desktop' },
];

interface ZoneOption {
  edge: ScreenEdge;
  labelKey: string;
  defaultLabel: string;
  tipKey: string;
  defaultTip: string;
}

const ZONE_OPTIONS: ZoneOption[] = [
  {
    edge: 'none',
    labelKey: 'components.globalPosition',
    defaultLabel: 'Global',
    tipKey: 'components.globalPositionTip',
    defaultTip: 'Global anywhere (default): swiping anywhere on canvas records a global gesture',
  },
  {
    edge: 'top',
    labelKey: 'components.topEdgePosition',
    defaultLabel: 'Top Edge',
    tipKey: 'components.topEdgePositionTip',
    defaultTip: 'Top screen edge: record gestures triggered from top edge of screen',
  },
  {
    edge: 'bottom',
    labelKey: 'components.bottomEdgePosition',
    defaultLabel: 'Bottom Edge',
    tipKey: 'components.bottomEdgePositionTip',
    defaultTip: 'Bottom screen edge: record gestures triggered from bottom edge of screen',
  },
  {
    edge: 'left',
    labelKey: 'components.leftEdgePosition',
    defaultLabel: 'Left Edge',
    tipKey: 'components.leftEdgePositionTip',
    defaultTip: 'Left screen edge: record gestures triggered from left edge of screen',
  },
  {
    edge: 'right',
    labelKey: 'components.rightEdgePosition',
    defaultLabel: 'Right Edge',
    tipKey: 'components.rightEdgePositionTip',
    defaultTip: 'Right screen edge: record gestures triggered from right edge of screen',
  },
];

interface TriggerOption {
  button: TriggerButton;
  labelKey: string;
  defaultLabel: string;
  tipKey: string;
  defaultTip: string;
}

const TRIGGER_OPTIONS: TriggerOption[] = [
  {
    button: 'right',
    labelKey: 'components.rightButton',
    defaultLabel: 'Right Click',
    tipKey: 'components.rightButtonTip',
    defaultTip: 'Right mouse button (hold right button to draw)',
  },
  {
    button: 'middle',
    labelKey: 'components.middleButton',
    defaultLabel: 'Middle Click',
    tipKey: 'components.middleButtonTip',
    defaultTip: 'Middle mouse button (hold middle button/scroll wheel to draw)',
  },
  {
    button: 'x1',
    labelKey: 'components.x1Button',
    defaultLabel: 'Side 1',
    tipKey: 'components.x1ButtonTip',
    defaultTip: 'Mouse side button 1 (hold back button to draw)',
  },
  {
    button: 'x2',
    labelKey: 'components.x2Button',
    defaultLabel: 'Side 2',
    tipKey: 'components.x2ButtonTip',
    defaultTip: 'Mouse side button 2 (hold forward button to draw)',
  },
  {
    button: 'left',
    labelKey: 'components.leftButton',
    defaultLabel: 'Left Click',
    tipKey: 'components.leftButtonTip',
    defaultTip: 'Left mouse button (starts on edge or with modifiers)',
  },
];

interface Props {
  value: string;
  onChange: (code: string) => void;
  triggerButton?: TriggerButton;
  onTriggerButtonChange?: (trigger: TriggerButton) => void;
  screenEdge?: ScreenEdge;
  onScreenEdgeChange?: (edge: ScreenEdge) => void;
}

export const GestureDrawCanvas: FC<Props> = ({
  value,
  onChange,
  triggerButton = 'right',
  onTriggerButtonChange,
  screenEdge = 'none',
  onScreenEdgeChange,
}) => {
  const { t } = useTranslation();
  const canvasRef = useRef<HTMLDivElement>(null);
  const [isDrawing, setIsDrawing] = useState(false);
  const [drawingButton, setDrawingButton] = useState<number | null>(null);
  const [points, setPoints] = useState<Point[]>([]);

  const initialParsed = parseGestureCode(value);
  const [edgeMode, setEdgeMode] = useState<ScreenEdge>(() =>
    initialParsed.edge !== 'none' ? initialParsed.edge : screenEdge,
  );
  const [triggerMode, setTriggerMode] = useState<TriggerButton>(() =>
    initialParsed.hasExplicitTrigger ? initialParsed.triggerButton : triggerButton,
  );

  // 实时监听键盘物理按键状态 (Ctrl / Shift / Alt)
  const [liveKeys, setLiveKeys] = useState<{ ctrl: boolean; shift: boolean; alt: boolean }>({
    ctrl: false,
    shift: false,
    alt: false,
  });

  // 解析当前手势编码自带的修饰与边缘状态
  const parsed = parseGestureCode(value);
  const currentCtrl = parsed.hasCtrl || liveKeys.ctrl;
  const currentShift = parsed.hasShift || liveKeys.shift;
  const currentAlt = parsed.hasAlt || liveKeys.alt;
  const currentEdge: ScreenEdge = value ? parsed.edge : edgeMode;
  const currentTrigger: TriggerButton = value ? parsed.triggerButton : triggerMode;

  // 监听物理键盘修饰键实时按下/松开
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (['INPUT', 'TEXTAREA'].includes((e.target as HTMLElement)?.tagName)) return;
      if (e.key === 'Control') setLiveKeys((k) => ({ ...k, ctrl: true }));
      if (e.key === 'Shift') setLiveKeys((k) => ({ ...k, shift: true }));
      if (e.key === 'Alt') {
        e.preventDefault();
        setLiveKeys((k) => ({ ...k, alt: true }));
      }
    };
    const onKeyUp = (e: KeyboardEvent) => {
      if (['INPUT', 'TEXTAREA'].includes((e.target as HTMLElement)?.tagName)) return;
      if (e.key === 'Control') setLiveKeys((k) => ({ ...k, ctrl: false }));
      if (e.key === 'Shift') setLiveKeys((k) => ({ ...k, shift: false }));
      if (e.key === 'Alt') setLiveKeys((k) => ({ ...k, alt: false }));
    };

    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('keyup', onKeyUp);
    return () => {
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('keyup', onKeyUp);
    };
  }, []);

  const handleSelectZone = useCallback((nextEdge: ScreenEdge) => {
    setEdgeMode(nextEdge);
    onScreenEdgeChange?.(nextEdge);
    if (parsed.bareCode) {
      const full = assembleGestureCode({
        edge: nextEdge,
        triggerButton: currentTrigger,
        hasCtrl: currentCtrl,
        hasShift: currentShift,
        hasAlt: currentAlt,
        bareCode: parsed.bareCode,
      });
      onChange(full);
    }
  }, [currentAlt, currentCtrl, currentShift, currentTrigger, onChange, onScreenEdgeChange, parsed.bareCode]);

  const handleSelectTrigger = useCallback((nextTrigger: TriggerButton) => {
    setTriggerMode(nextTrigger);
    onTriggerButtonChange?.(nextTrigger);
    if (parsed.bareCode) {
      const full = assembleGestureCode({
        edge: currentEdge,
        triggerButton: nextTrigger,
        hasCtrl: currentCtrl,
        hasShift: currentShift,
        hasAlt: currentAlt,
        bareCode: parsed.bareCode,
      });
      onChange(full);
    }
  }, [currentAlt, currentCtrl, currentEdge, currentShift, onChange, onTriggerButtonChange, parsed.bareCode]);

  const resolvePointerTrigger = useCallback((mouseButton: number): TriggerButton => {
    if (mouseButton === 1) return 'middle';
    if (mouseButton === 2) return 'right';
    if (mouseButton === 3) return 'x1';
    if (mouseButton === 4) return 'x2';
    if (
      mouseButton === 0 &&
      (currentTrigger === 'left' ||
        liveKeys.ctrl ||
        liveKeys.shift ||
        liveKeys.alt ||
        parsed.hasCtrl ||
        parsed.hasShift ||
        parsed.hasAlt)
    ) {
      return 'left';
    }
    return currentTrigger;
  }, [currentTrigger, liveKeys, parsed.hasAlt, parsed.hasCtrl, parsed.hasShift]);

  const handlePointerDown = useCallback((e: React.PointerEvent) => {
    e.preventDefault();
    if (!canvasRef.current) return;
    const rect = canvasRef.current.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    setPoints([{ x, y }]);
    setIsDrawing(true);
    setDrawingButton(e.button);

    // 硬件鼠标按键自动感应（严格保持用户当前显式选定的触发区域模式，绝不按落笔横纵坐标静默篡改为边缘手势）
    const detectedBtn = resolvePointerTrigger(e.button);
    if (detectedBtn !== currentTrigger) {
      setTriggerMode(detectedBtn);
      onTriggerButtonChange?.(detectedBtn);
    }

    try {
      (e.target as HTMLElement).setPointerCapture?.(e.pointerId);
    } catch {
      // ignore
    }
  }, [currentTrigger, onTriggerButtonChange, resolvePointerTrigger]);

  const handlePointerMove = useCallback((e: React.PointerEvent) => {
    if (!isDrawing || !canvasRef.current) return;
    e.preventDefault();
    const rect = canvasRef.current.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;

    setPoints((prev) => {
      if (prev.length > 0) {
        const last = prev[prev.length - 1];
        const dist = calculateDistance(last.x, last.y, x, y);
        if (dist < 4) return prev;
      }
      return [...prev, { x, y }];
    });
  }, [isDrawing]);

  const handlePointerUp = useCallback((e: React.PointerEvent) => {
    if (!isDrawing) return;
    e.preventDefault();
    setIsDrawing(false);

    const effectiveButton = drawingButton ?? e.button;
    const effectiveTrigger = resolvePointerTrigger(effectiveButton);

    if (points.length >= 2) {
      const bare = recognizeStrokes(points);
      if (bare) {
        // 严格遵循顶部显式选定的触发区域 currentEdge (默认 Global='none')
        const full = assembleGestureCode({
          edge: currentEdge,
          triggerButton: effectiveTrigger,
          hasCtrl: e.ctrlKey || liveKeys.ctrl || parsed.hasCtrl,
          hasShift: e.shiftKey || liveKeys.shift || parsed.hasShift,
          hasAlt: e.altKey || liveKeys.alt || parsed.hasAlt,
          bareCode: bare,
        });
        onChange(full);
      }
    }
    setDrawingButton(null);
  }, [isDrawing, points, onChange, currentEdge, drawingButton, liveKeys, parsed, resolvePointerTrigger]);

  const handleWheel = useCallback((e: React.WheelEvent) => {
    if (e.deltaY === 0) return;
    // 在画板内按住鼠标键滚动、按住修饰键滚动或处于边缘录制模式时，支持直接录制滚轮手势
    const hasButtonsOrModifiers = e.buttons > 0 || e.ctrlKey || e.shiftKey || e.altKey || liveKeys.ctrl || liveKeys.shift || liveKeys.alt;
    if (!hasButtonsOrModifiers && currentEdge === 'none') return;
    e.preventDefault();
    const wheelBare = e.deltaY < 0 ? 'WheelUp' : 'WheelDown';
    let wheelTrigger: TriggerButton = currentEdge !== 'none' && e.buttons === 0 ? 'right' : currentTrigger;
    if ((e.buttons & 4) !== 0) wheelTrigger = 'middle';
    else if ((e.buttons & 2) !== 0) wheelTrigger = 'right';
    else if ((e.buttons & 8) !== 0) wheelTrigger = 'x1';
    else if ((e.buttons & 16) !== 0) wheelTrigger = 'x2';

    setIsDrawing(false);
    setPoints([]);
    const full = assembleGestureCode({
      edge: currentEdge,
      triggerButton: wheelTrigger,
      hasCtrl: e.ctrlKey || liveKeys.ctrl || parsed.hasCtrl,
      hasShift: e.shiftKey || liveKeys.shift || parsed.hasShift,
      hasAlt: e.altKey || liveKeys.alt || parsed.hasAlt,
      bareCode: wheelBare,
    });
    onChange(full);
  }, [currentEdge, currentTrigger, liveKeys, onChange, parsed.hasAlt, parsed.hasCtrl, parsed.hasShift]);

  const handleClear = () => {
    setEdgeMode(currentEdge);
    setTriggerMode(currentTrigger);
    setPoints([]);
    onChange('');
  };

  const handleSelectPreset = (presetCode: string) => {
    const presetParsed = parseGestureCode(presetCode);
    const nextEdge = presetParsed.edge !== 'none' ? presetParsed.edge : currentEdge;
    const nextTrigger = presetParsed.hasExplicitTrigger ? presetParsed.triggerButton : currentTrigger;
    setEdgeMode(nextEdge);
    setTriggerMode(nextTrigger);
    onScreenEdgeChange?.(nextEdge);
    onTriggerButtonChange?.(nextTrigger);
    const full = assembleGestureCode({
      edge: nextEdge,
      triggerButton: nextTrigger,
      hasCtrl: currentCtrl || presetParsed.hasCtrl,
      hasShift: currentShift || presetParsed.hasShift,
      hasAlt: currentAlt || presetParsed.hasAlt,
      bareCode: presetParsed.bareCode,
    });
    onChange(full);
  };

  const svgPath = pointsToSvgPath(points);
  const lastPoint = points.length > 0 ? points[points.length - 1] : null;

  // 根据当前显式选择的触发区域与触发键智能匹配预设列表
  const presets =
    currentEdge === 'top'
      ? TOPEDGE_PRESET_GESTURES
      : currentEdge === 'bottom'
      ? BOTTOMEDGE_PRESET_GESTURES
      : currentEdge === 'left'
      ? LEFTEDGE_PRESET_GESTURES
      : currentEdge === 'right'
      ? RIGHTEDGE_PRESET_GESTURES
      : currentTrigger === 'middle'
      ? MIDDLE_PRESET_GESTURES
      : currentTrigger === 'x1'
      ? SIDE1_PRESET_GESTURES
      : currentTrigger === 'x2'
      ? SIDE2_PRESET_GESTURES
      : RIGHT_PRESET_GESTURES;

  const activeZoneOption = ZONE_OPTIONS.find((z) => z.edge === currentEdge) ?? ZONE_OPTIONS[0];
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const activeZoneLabel = t(activeZoneOption.labelKey as any, activeZoneOption.defaultLabel);

  const edgeSensorText =
    currentEdge === 'top'
      ? t('components.topEdgeSensor', 'Top Edge Active Zone')
      : currentEdge === 'bottom'
      ? t('components.bottomEdgeSensor', 'Bottom Edge Active Zone')
      : currentEdge === 'left'
      ? t('components.leftEdgeSensor', 'Left Edge Active Zone')
      : currentEdge === 'right'
      ? t('components.rightEdgeSensor', 'Right Edge Active Zone')
      : '';

  return (
    <div className="gesture-draw-container">
      {/* 顶部一体化手势控制舱 */}
      <div
        className="gesture-cockpit-bar"
        title={t('components.cockpitBarTip', 'Gesture control bar: select trigger zone & mouse button, and auto-detect keyboard modifiers')}
      >
        {/* 第 1 行：显式触发区域分段胶囊选择器 (默认 Global 任意位置，主动切换才录边缘手势) */}
        <div className="gesture-cockpit-row">
          <div
            className="gesture-cockpit-group"
            role="radiogroup"
            aria-label={t('components.positionLabel', 'Zone:')}
          >
            <span className="gesture-cockpit-label">{t('components.positionLabel', 'Zone:')}</span>
            <div className="gesture-zone-segmented">
              {ZONE_OPTIONS.map((opt) => {
                const isSelected = currentEdge === opt.edge;
                // eslint-disable-next-line @typescript-eslint/no-explicit-any
                const label = t(opt.labelKey as any, opt.defaultLabel);
                // eslint-disable-next-line @typescript-eslint/no-explicit-any
                const tip = t(opt.tipKey as any, opt.defaultTip);
                return (
                  <button
                    key={opt.edge}
                    type="button"
                    role="radio"
                    aria-checked={isSelected}
                    data-edge-option={opt.edge}
                    className={`gesture-zone-pill ${isSelected ? 'active' : ''}`}
                    onClick={() => handleSelectZone(opt.edge)}
                    title={tip}
                  >
                    {opt.edge === 'none' ? (
                      <Globe size={13} strokeWidth={2.2} aria-hidden="true" />
                    ) : (
                      <ScreenEdgeSvg edge={opt.edge} size={13} className="gesture-cockpit-svg-icon" />
                    )}
                    <span>{label}</span>
                  </button>
                );
              })}
            </div>
          </div>

          <button
            type="button"
            className="gesture-draw-clear-btn"
            onClick={handleClear}
            title={t('components.clearBoardTip', 'Clear canvas and re-record')}
          >
            <RotateCcw size={12} aria-hidden="true" />
            <span>{t('components.clearBoard', 'Clear')}</span>
          </button>
        </div>

        {/* 第 2 行：鼠标触发按键选择/感应 + 物理键盘修饰键实时指示 */}
        <div className="gesture-cockpit-row">
          <div
            className="gesture-cockpit-group"
            role="radiogroup"
            aria-label={t('components.triggerKeyLabel', 'Trigger:')}
          >
            <span className="gesture-cockpit-label">{t('components.triggerKeyLabel', 'Trigger:')}</span>
            {TRIGGER_OPTIONS.map((opt) => {
              const isSelected = currentTrigger === opt.button;
              // eslint-disable-next-line @typescript-eslint/no-explicit-any
              const label = t(opt.labelKey as any, opt.defaultLabel);
              // eslint-disable-next-line @typescript-eslint/no-explicit-any
              const tip = t(opt.tipKey as any, opt.defaultTip);
              return (
                <button
                  key={opt.button}
                  type="button"
                  role="radio"
                  aria-checked={isSelected}
                  data-trigger-option={opt.button}
                  className={`gesture-cockpit-badge ${isSelected ? 'active' : ''}`}
                  onClick={() => handleSelectTrigger(opt.button)}
                  title={tip}
                >
                  <MouseButtonSvg button={opt.button} size={13} className="gesture-cockpit-svg-icon" />
                  <span>{label}</span>
                </button>
              );
            })}
          </div>

          <div className="gesture-cockpit-group">
            <span className="gesture-cockpit-label">{t('components.modifiersLabel', 'Modifiers:')}</span>
            <span
              className={`gesture-modifier-badge ${currentCtrl ? 'active' : ''} ${liveKeys.ctrl ? 'is-key-pressed' : ''}`}
              title={t('components.ctrlKeyTip', 'Ctrl key (hold physical Ctrl on keyboard)')}
            >
              Ctrl
            </span>
            <span
              className={`gesture-modifier-badge ${currentShift ? 'active' : ''} ${liveKeys.shift ? 'is-key-pressed' : ''}`}
              title={t('components.shiftKeyTip', 'Shift key (hold physical Shift on keyboard)')}
            >
              Shift
            </span>
            <span
              className={`gesture-modifier-badge ${currentAlt ? 'active' : ''} ${liveKeys.alt ? 'is-key-pressed' : ''}`}
              title={t('components.altKeyTip', 'Alt key (hold physical Alt on keyboard)')}
            >
              Alt
            </span>
          </div>
        </div>
      </div>

      {/* 交互式手势捕获画板 */}
      <div
        ref={canvasRef}
        data-active-edge={currentEdge}
        className={`gesture-draw-pad ${isDrawing ? 'is-drawing' : ''} ${
          currentEdge !== 'none' ? `edge-active-${currentEdge}` : ''
        }`}
        onPointerDown={handlePointerDown}
        onPointerMove={handlePointerMove}
        onPointerUp={handlePointerUp}
        onWheel={handleWheel}
        onContextMenu={(e) => e.preventDefault()}
        onAuxClick={(e) => e.preventDefault()}
      >
        {/* 背景辅助点阵网格 */}
        <div className="gesture-draw-crosshair" />

        {/* 仅在显式开启边缘模式时渲染清晰的高对比度虚线边缘感应带 */}
        {currentEdge !== 'none' && (
          <div
            className={`gesture-edge-sensor-zone gesture-edge-sensor-zone--${currentEdge} active`}
            data-testid={`edge-sensor-${currentEdge}`}
          >
            <span className="gesture-edge-sensor-tag">
              <ScreenEdgeSvg edge={currentEdge} size={13} />
              <span>{edgeSensorText}</span>
            </span>
          </div>
        )}

        {points.length === 0 && (
          <div className="gesture-draw-watermark">
            <div className="gesture-draw-watermark-icon-box">
              <MousePointer size={20} strokeWidth={2} className="gesture-draw-mouse-icon" aria-hidden="true" />
            </div>
            <div className="gesture-draw-watermark-text">
              {currentEdge === 'none'
                ? t('gesture.canvasDrawTip', 'Swipe anywhere on canvas to record · Choose trigger zone above')
                : t('components.canvasActiveEdgeHint', 'Edge mode active ({{edge}}) · Swipe or scroll on canvas to bind edge gesture', {
                    edge: activeZoneLabel,
                  })}
            </div>
            <div className="gesture-draw-watermark-hint">
              {t('gesture.canvasEdgeTip', 'Global mode records anywhere as global · Switch to an Edge mode above for screen-edge gestures')}
            </div>
          </div>
        )}

        {/* 实时平滑矢量轨迹 */}
        {points.length > 0 && (
          <svg className="gesture-draw-svg" aria-hidden="true">
            <path
              d={svgPath}
              className="gesture-stroke-glow"
              fill="none"
              strokeLinecap="round"
              strokeLinejoin="round"
            />
            <path
              d={svgPath}
              className="gesture-stroke-core"
              fill="none"
              strokeLinecap="round"
              strokeLinejoin="round"
            />
            {lastPoint && (
              <>
                <circle cx={lastPoint.x} cy={lastPoint.y} r={8} className="gesture-point-halo" />
                <circle cx={lastPoint.x} cy={lastPoint.y} r={4.5} className="gesture-point-bead" />
                <circle cx={lastPoint.x} cy={lastPoint.y} r={2} className="gesture-point-core" />
              </>
            )}
          </svg>
        )}

        {/* 识别结果悬浮卡片 */}
        {value && !isDrawing && (
          <div className="gesture-draw-badge">
            <Check size={14} className="gesture-draw-badge-icon" aria-hidden="true" />
            <span className="gesture-draw-badge-arrows">{codeToArrows(value)}</span>
          </div>
        )}
      </div>

      {/* 常用手势快捷预设栏 */}
      <div className="gesture-preset-tray">
        <span className="gesture-preset-label">{t('components.quickPresets', 'Presets:')}</span>
        <div className="gesture-preset-chips">
          {presets.map((preset) => {
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            const labelText = t(preset.labelKey as any, preset.defaultLabel);
            return (
              <button
                key={preset.code}
                type="button"
                className={`gesture-preset-chip ${value === preset.code ? 'active' : ''}`}
                onClick={() => handleSelectPreset(preset.code)}
                title={labelText}
              >
                <span className="gesture-preset-arrows">{codeToArrows(preset.code)}</span>
                <span className="gesture-preset-name">{labelText}</span>
              </button>
            );
          })}
        </div>
      </div>
    </div>
  );
};

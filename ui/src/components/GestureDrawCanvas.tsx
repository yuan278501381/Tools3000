/* ─────────────────────────────────────────────────────────────────────────────
 * GestureDrawCanvas — 交互式手势录制与识别画板 (World-Class Interactive Drawing Pad)
 *
 * 核心交互哲学 (Zero-Cognition Auto-Detection & Display Only):
 *   - 用户无需手动配置任何选项，认知负荷归零！
 *   - 硬件按键自动感应: 在画板按住右键/中键/侧键1/侧键2/左键划动，全自动识别
 *   - 屏幕位置自动感应: 在画板顶部起笔 -> 自动识别为上边缘 (TopEdge+)
 *                     在画板底部起笔 -> 自动识别为底边缘 (BottomEdge+)
 *                     在画板左/右侧起笔 -> 自动识别为左/右边缘 (LeftEdge+/RightEdge+)
 *                     在画板中央起笔 -> 自动识别为全局常规手势 (none)
 *   - 物理修饰键自动侦测: 划动时按住 Ctrl/Shift/Alt，实时侦测并自动绑定
 *   - 顶部控制舱为「全自动感应监视仪 (Read-only Sensor Telemetry)」，纯状态展示不可编辑
 *   - 搭载 RDP + 转弯圆角折叠平滑消抖算法 (Corner Fillet Folding)
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useRef, useCallback, useEffect, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { RotateCcw, MousePointer, Check } from 'lucide-react';
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
  { code: 'TopEdge+Left+D', labelKey: 'gesture.actions.pullDownClose.name', defaultLabel: 'Left Pull Down Close' },
  { code: 'TopEdge+Middle+D', labelKey: 'gesture.actions.pullDownMinimize.name', defaultLabel: 'Middle Pull Down Minimize' },
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
  const [drawingEdge, setDrawingEdge] = useState<ScreenEdge>('none');
  const [points, setPoints] = useState<Point[]>([]);

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
  const currentTrigger = parsed.triggerButton !== 'right' ? parsed.triggerButton : triggerButton;
  const currentEdge = parsed.edge !== 'none' ? parsed.edge : (isDrawing ? drawingEdge : screenEdge);

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

  const handlePointerDown = useCallback((e: React.PointerEvent) => {
    e.preventDefault();
    if (!canvasRef.current) return;
    const rect = canvasRef.current.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    setPoints([{ x, y }]);
    setIsDrawing(true);
    setDrawingButton(e.button);

    // 智能识别落笔区域坐标 (自动感应上边缘/底边缘/左边缘/右边缘/全局)
    const relX = rect.width > 0 ? x / rect.width : 0.5;
    const relY = rect.height > 0 ? y / rect.height : 0.5;
    let detectedEdge: ScreenEdge;
    if (relY <= 0.25) {
      detectedEdge = 'top';
    } else if (relY >= 0.75) {
      detectedEdge = 'bottom';
    } else if (relX <= 0.15) {
      detectedEdge = 'left';
    } else if (relX >= 0.85) {
      detectedEdge = 'right';
    } else {
      detectedEdge = 'none';
    }

    setDrawingEdge(detectedEdge);
    if (onScreenEdgeChange && detectedEdge !== currentEdge) {
      onScreenEdgeChange(detectedEdge);
    }

    // 智能联动物理按键识别
    let detectedBtn: TriggerButton = currentTrigger;
    if (e.button === 1) detectedBtn = 'middle';
    else if (e.button === 2) detectedBtn = 'right';
    else if (e.button === 3) detectedBtn = 'x1';
    else if (e.button === 4) detectedBtn = 'x2';
    else if (e.button === 0 && (detectedEdge !== 'none' || liveKeys.ctrl || liveKeys.shift || liveKeys.alt || parsed.hasCtrl || parsed.hasShift || parsed.hasAlt)) {
      detectedBtn = 'left';
    }

    if (onTriggerButtonChange && detectedBtn !== currentTrigger) {
      onTriggerButtonChange(detectedBtn);
    }

    try {
      (e.target as HTMLElement).setPointerCapture?.(e.pointerId);
    } catch {
      // ignore
    }
  }, [currentTrigger, currentEdge, liveKeys, parsed, onTriggerButtonChange, onScreenEdgeChange]);

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

    // 基于起笔点的绝对坐标核实边缘归属
    let effectiveEdge: ScreenEdge = drawingEdge;
    if (points.length > 0 && canvasRef.current) {
      const startPt = points[0];
      const rect = canvasRef.current.getBoundingClientRect();
      const relX = rect.width > 0 ? startPt.x / rect.width : 0.5;
      const relY = rect.height > 0 ? startPt.y / rect.height : 0.5;
      if (relY <= 0.25) effectiveEdge = 'top';
      else if (relY >= 0.75) effectiveEdge = 'bottom';
      else if (relX <= 0.15) effectiveEdge = 'left';
      else if (relX >= 0.85) effectiveEdge = 'right';
      else effectiveEdge = 'none';
    }

    const effectiveButton = drawingButton ?? e.button;
    let effectiveTrigger: TriggerButton = currentTrigger;
    if (effectiveButton === 1) effectiveTrigger = 'middle';
    else if (effectiveButton === 2) effectiveTrigger = 'right';
    else if (effectiveButton === 3) effectiveTrigger = 'x1';
    else if (effectiveButton === 4) effectiveTrigger = 'x2';
    else if (effectiveButton === 0 && (effectiveEdge !== 'none' || liveKeys.ctrl || liveKeys.shift || liveKeys.alt || parsed.hasCtrl || parsed.hasShift || parsed.hasAlt)) {
      effectiveTrigger = 'left';
    }

    if (points.length >= 2) {
      const bare = recognizeStrokes(points);
      if (bare) {
        const full = assembleGestureCode({
          edge: effectiveEdge,
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
  }, [isDrawing, points, onChange, currentTrigger, drawingEdge, drawingButton, liveKeys, parsed]);

  const handleClear = () => {
    setPoints([]);
    setDrawingEdge('none');
    onChange('');
  };

  const handleSelectPreset = (presetCode: string) => {
    const presetParsed = parseGestureCode(presetCode);
    const full = assembleGestureCode({
      edge: presetParsed.edge !== 'none' ? presetParsed.edge : currentEdge,
      triggerButton: presetParsed.triggerButton !== 'right' ? presetParsed.triggerButton : currentTrigger,
      hasCtrl: currentCtrl || presetParsed.hasCtrl,
      hasShift: currentShift || presetParsed.hasShift,
      hasAlt: currentAlt || presetParsed.hasAlt,
      bareCode: presetParsed.bareCode,
    });
    onChange(full);
  };

  const svgPath = pointsToSvgPath(points);
  const lastPoint = points.length > 0 ? points[points.length - 1] : null;

  // 根据当前触发方式智能挑选预设列表
  const presets =
    currentEdge === 'top'
      ? TOPEDGE_PRESET_GESTURES
      : currentTrigger === 'middle'
      ? MIDDLE_PRESET_GESTURES
      : currentTrigger === 'x1'
      ? SIDE1_PRESET_GESTURES
      : currentTrigger === 'x2'
      ? SIDE2_PRESET_GESTURES
      : RIGHT_PRESET_GESTURES;

  return (
    <div className="gesture-draw-container">
      {/* 实时硬件与位置感应监视仪 (Live Sensor Cockpit - Zero Cognition Read-only Display) */}
      <div className="gesture-cockpit-bar" title={t('components.cockpitBarTip', 'Auto-detection zone: automatically senses mouse buttons, starting points, and keyboard modifiers')}>
        {/* 触发按键自动感应指示 */}
        <div className="gesture-cockpit-group">
          <span className="gesture-cockpit-label">{t('components.triggerKeyLabel', 'Trigger:')}</span>
          <span
            className={`gesture-cockpit-badge ${currentTrigger === 'right' ? 'active' : ''}`}
            title={t('components.rightButtonTip', 'Right mouse button (hold right button to draw)')}
          >
            <span>◐</span>
            <span>{t('components.rightButton', 'Right Click')}</span>
          </span>
          <span
            className={`gesture-cockpit-badge ${currentTrigger === 'middle' ? 'active' : ''}`}
            title={t('components.middleButtonTip', 'Middle mouse button (hold middle button/scroll wheel to draw)')}
          >
            <span>◓</span>
            <span>{t('components.middleButton', 'Middle Click')}</span>
          </span>
          <span
            className={`gesture-cockpit-badge ${currentTrigger === 'x1' ? 'active' : ''}`}
            title={t('components.x1ButtonTip', 'Mouse side button 1 (hold back button to draw)')}
          >
            <span>◧</span>
            <span>{t('components.x1Button', 'Side 1')}</span>
          </span>
          <span
            className={`gesture-cockpit-badge ${currentTrigger === 'x2' ? 'active' : ''}`}
            title={t('components.x2ButtonTip', 'Mouse side button 2 (hold forward button to draw)')}
          >
            <span>◨</span>
            <span>{t('components.x2Button', 'Side 2')}</span>
          </span>
          <span
            className={`gesture-cockpit-badge ${currentTrigger === 'left' ? 'active' : ''}`}
            title={t('components.leftButtonTip', 'Left mouse button (starts on edge or with modifiers)')}
          >
            <span>◑</span>
            <span>{t('components.leftButton', 'Left Click')}</span>
          </span>
        </div>

        {/* 触发位置自动感应指示 */}
        <div className="gesture-cockpit-group">
          <span className="gesture-cockpit-label">{t('components.positionLabel', 'Position:')}</span>
          <span
            className={`gesture-cockpit-badge ${currentEdge === 'none' ? 'active' : ''}`}
            title={t('components.globalPositionTip', 'Starting from center is recognized as global gesture')}
          >
            <span>{t('components.globalPosition', 'Global')}</span>
          </span>
          <span
            className={`gesture-cockpit-badge ${currentEdge === 'top' ? 'active' : ''}`}
            title={t('components.topEdgePositionTip', 'Starting from top is recognized as top edge gesture')}
          >
            <span>{t('components.topEdgePosition', '◰ Top Edge')}</span>
          </span>
          <span
            className={`gesture-cockpit-badge ${currentEdge === 'bottom' ? 'active' : ''}`}
            title={t('components.bottomEdgePositionTip', 'Starting from bottom is recognized as bottom edge gesture')}
          >
            <span>{t('components.bottomEdgePosition', '◲ Bottom Edge')}</span>
          </span>
        </div>

        {/* 物理修饰键自动侦测指示 */}
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

          <button
            type="button"
            className="gesture-draw-clear-btn"
            onClick={handleClear}
            title={t('components.clearBoardTip', 'Clear canvas and re-record')}
          >
            <RotateCcw size={12} />
            <span>{t('components.clearBoard', 'Clear')}</span>
          </button>
        </div>
      </div>

      {/* 交互式手势捕获板 (含顶部与底部边缘自动感应指示区) */}
      <div
        ref={canvasRef}
        className={`gesture-draw-pad ${isDrawing ? 'is-drawing' : ''} ${currentEdge === 'top' ? 'edge-active-top' : currentEdge === 'bottom' ? 'edge-active-bottom' : ''}`}
        onPointerDown={handlePointerDown}
        onPointerMove={handlePointerMove}
        onPointerUp={handlePointerUp}
        onContextMenu={(e) => e.preventDefault()}
        onAuxClick={(e) => e.preventDefault()}
      >
        {/* 背景辅助网格 */}
        <div className="gesture-draw-crosshair" />

        {/* 顶部屏幕上边缘感应区指示 */}
        <div className={`gesture-edge-sensor-zone gesture-edge-sensor-zone--top ${currentEdge === 'top' ? 'active' : ''}`}>
          <span className="gesture-edge-sensor-tag">{t('components.topEdgeSensor', '◰ Top Edge Sensor Zone')}</span>
        </div>

        {/* 底部屏幕底边缘感应区指示 */}
        <div className={`gesture-edge-sensor-zone gesture-edge-sensor-zone--bottom ${currentEdge === 'bottom' ? 'active' : ''}`}>
          <span className="gesture-edge-sensor-tag">{t('components.bottomEdgeSensor', '◲ Bottom Edge Sensor Zone')}</span>
        </div>

        {points.length === 0 && (
          <div className="gesture-draw-watermark">
            <div className="gesture-draw-watermark-icon-box">
              <MousePointer size={20} strokeWidth={2} className="gesture-draw-mouse-icon" />
            </div>
            <div className="gesture-draw-watermark-text">
              {t('gesture.canvasDrawTip', 'Draw directly on canvas · Button, start pos & modifiers auto recognized')}
            </div>
            <div className="gesture-draw-watermark-hint">
              {t('gesture.canvasEdgeTip', 'Start top=Top edge · Start bottom=Bottom edge · Start middle=Global · Auto detect button pressed')}
            </div>
          </div>
        )}

        {/* 实时平滑矢量轨迹 */}
        {points.length > 0 && (
          <svg className="gesture-draw-svg">
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
            <Check size={14} className="gesture-draw-badge-icon" />
            <span className="gesture-draw-badge-arrows">{codeToArrows(value)}</span>
          </div>
        )}
      </div>

      {/* 常用手势快捷预设栏 */}
      <div className="gesture-preset-tray">
        <span className="gesture-preset-label">{t('components.quickPresets', 'Presets:')}</span>
        <div className="gesture-preset-chips">
          {presets.map((preset) => {
            const labelText = (t as (k: string, d?: string) => string)(preset.labelKey, preset.defaultLabel);
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

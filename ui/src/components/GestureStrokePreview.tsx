/* ─────────────────────────────────────────────────────────────────────────────
 * GestureStrokePreview.tsx — 动态手势矢量微缩预览组件
 *
 * 功能:
 *   - 复用 gestureModel.ts 单一事实源精确剥离边缘前缀、修饰键与鼠标按键前缀
 *   - 滚轮手势 (WheelUp / WheelDown) 渲染专属高精度矢量鼠标滚轮 + 上下方向徽章
 *   - 方向笔画序列仅绘制合法 8 方向向量，严禁将未知 token 静默回退为右移折线
 *   - 边缘手势同步渲染微缩边缘发光带与紧凑边缘修饰胶囊提示
 * ───────────────────────────────────────────────────────────────────────────── */

import { useMemo, useState, useRef, useEffect, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import {
  codeToArrows,
  parseGesturePreview,
  type ScreenEdge,
  type ValidStrokeToken,
} from './gestureModel';
import './GestureStrokePreview.css';

interface Vector {
  dx: number;
  dy: number;
}

const DIR_VECTORS: Record<ValidStrokeToken, Vector> = {
  R: { dx: 1, dy: 0 },
  L: { dx: -1, dy: 0 },
  U: { dx: 0, dy: -1 },
  D: { dx: 0, dy: 1 },
  UR: { dx: 0.707, dy: -0.707 },
  RU: { dx: 0.707, dy: -0.707 },
  UL: { dx: -0.707, dy: -0.707 },
  LU: { dx: -0.707, dy: -0.707 },
  DR: { dx: 0.707, dy: 0.707 },
  RD: { dx: 0.707, dy: 0.707 },
  DL: { dx: -0.707, dy: 0.707 },
  LD: { dx: -0.707, dy: 0.707 },
};

export interface GestureStrokePreviewProps {
  code: string;
  width?: number;
  height?: number;
  triggerButton?: 'right' | 'middle' | 'left' | 'x1' | 'x2';
  interactive?: boolean;
  autoAnimate?: boolean;
  className?: string;
  title?: string;
}

const EdgeMiniGlyph: FC<{ edge: Exclude<ScreenEdge, 'none'> }> = ({ edge }) => (
  <svg
    width="11"
    height="11"
    viewBox="0 0 12 12"
    fill="none"
    aria-hidden="true"
    className="gesture-stroke-preview__edge-glyph"
  >
    <rect
      x="1.2"
      y="1.8"
      width="9.6"
      height="8.4"
      rx="1.8"
      stroke="currentColor"
      strokeWidth="1.2"
      strokeOpacity="0.45"
    />
    {edge === 'top' && (
      <line x1="2.2" y1="2.1" x2="9.8" y2="2.1" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" />
    )}
    {edge === 'bottom' && (
      <line x1="2.2" y1="9.9" x2="9.8" y2="9.9" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" />
    )}
    {edge === 'left' && (
      <line x1="1.5" y1="3" x2="1.5" y2="9" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" />
    )}
    {edge === 'right' && (
      <line x1="10.5" y1="3" x2="10.5" y2="9" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" />
    )}
  </svg>
);

function renderCanvasEdgeBar(edge: ScreenEdge, width: number, height: number) {
  if (edge === 'none') return null;
  if (edge === 'top') {
    return (
      <rect
        x={3}
        y={1}
        width={Math.max(8, width - 6)}
        height={2.6}
        rx={1.3}
        className="gesture-stroke-preview__edge-bar"
      />
    );
  }
  if (edge === 'bottom') {
    return (
      <rect
        x={3}
        y={height - 3.6}
        width={Math.max(8, width - 6)}
        height={2.6}
        rx={1.3}
        className="gesture-stroke-preview__edge-bar"
      />
    );
  }
  if (edge === 'left') {
    return (
      <rect
        x={1}
        y={3}
        width={2.6}
        height={Math.max(8, height - 6)}
        rx={1.3}
        className="gesture-stroke-preview__edge-bar"
      />
    );
  }
  return (
    <rect
      x={width - 3.6}
      y={3}
      width={2.6}
      height={Math.max(8, height - 6)}
      rx={1.3}
      className="gesture-stroke-preview__edge-bar"
    />
  );
}

export const GestureStrokePreview: FC<GestureStrokePreviewProps> = ({
  code,
  width = 54,
  height = 36,
  triggerButton = 'right',
  interactive = true,
  autoAnimate = false,
  className = '',
  title,
}) => {
  const { t } = useTranslation();
  const [isHovered, setIsHovered] = useState(false);
  const pathRef = useRef<SVGPathElement>(null);
  const [pathLength, setPathLength] = useState(100);

  const preview = useMemo(
    () => parseGesturePreview(code, triggerButton),
    [code, triggerButton],
  );
  const { edge, triggerButton: effectiveTriggerButton, wheelDirection, segments } = preview;

  const edgePillLabel = useMemo(() => {
    if (edge === 'top') return t('gesture.edgePillTop', 'Top');
    if (edge === 'bottom') return t('gesture.edgePillBottom', 'Btm');
    if (edge === 'left') return t('gesture.edgePillLeft', 'Left');
    if (edge === 'right') return t('gesture.edgePillRight', 'Right');
    return null;
  }, [edge, t]);

  // 计算几何点坐标并等比缩放居中到画板安全区域（仅绘制合法方向段）
  const geometry = useMemo(() => {
    if (wheelDirection || segments.length === 0) return null;

    const points: { x: number; y: number }[] = [{ x: 0, y: 0 }];
    let curX = 0;
    let curY = 0;
    const step = 28;

    for (const seg of segments) {
      const vec = DIR_VECTORS[seg];
      if (!vec) continue;
      curX += vec.dx * step;
      curY += vec.dy * step;
      points.push({ x: curX, y: curY });
    }

    if (points.length < 2) return null;

    let minX = Infinity;
    let maxX = -Infinity;
    let minY = Infinity;
    let maxY = -Infinity;

    for (const p of points) {
      if (p.x < minX) minX = p.x;
      if (p.x > maxX) maxX = p.x;
      if (p.y < minY) minY = p.y;
      if (p.y > maxY) maxY = p.y;
    }

    const rawW = maxX - minX || 1;
    const rawH = maxY - minY || 1;

    // 安全留白边距，保证起点按键图标与终点光标不溢出
    const paddingX = 9;
    const paddingY = 8;
    const availW = Math.max(8, width - paddingX * 2);
    const availH = Math.max(8, height - paddingY * 2);

    const scale = Math.min(availW / rawW, availH / rawH, 1.0);

    const offsetX = (width - rawW * scale) / 2 - minX * scale;
    const offsetY = (height - rawH * scale) / 2 - minY * scale;

    const scaledPoints = points.map((p) => ({
      x: Number((p.x * scale + offsetX).toFixed(2)),
      y: Number((p.y * scale + offsetY).toFixed(2)),
    }));

    let d = `M ${scaledPoints[0].x} ${scaledPoints[0].y}`;
    for (let i = 1; i < scaledPoints.length; i++) {
      d += ` L ${scaledPoints[i].x} ${scaledPoints[i].y}`;
    }

    const startPoint = scaledPoints[0];
    const endPoint = scaledPoints[scaledPoints.length - 1];

    return {
      d,
      startPoint,
      endPoint,
    };
  }, [wheelDirection, segments, width, height]);

  // 动态测量路径实际长度以驱动 SVG 描边生长
  useEffect(() => {
    if (pathRef.current && typeof pathRef.current.getTotalLength === 'function') {
      const len = pathRef.current.getTotalLength();
      if (len > 0) setPathLength(Math.ceil(len));
    }
  }, [geometry]);

  const fallbackArrows = useMemo(() => codeToArrows(code) || code, [code]);
  const tooltipText =
    title ??
    t('gesture.previewTooltip', 'Gesture: {{arrows}} (Code: {{code}})', {
      arrows: fallbackArrows,
      code,
    });

  const isAnimating = autoAnimate || isHovered;

  // 1. 滚轮专属高精度矢量 SVG 徽章（带边缘修饰胶囊提示）
  if (wheelDirection) {
    const isUp = wheelDirection === 'up';
    return (
      <div
        className={`gesture-stroke-preview-stack ${className}`}
        title={tooltipText}
        data-gesture-kind="wheel"
        data-wheel-dir={wheelDirection}
        data-gesture-edge={edge}
      >
        {edge !== 'none' && edgePillLabel && (
          <span className="gesture-stroke-preview__edge-pill">
            <EdgeMiniGlyph edge={edge} />
            <span>{edgePillLabel}</span>
          </span>
        )}
        <div
          className={`gesture-stroke-preview gesture-stroke-preview--wheel ${isAnimating ? 'gesture-stroke-preview--animating' : ''}`}
          style={{
            width: `${width}px`,
            height: `${height}px`,
          }}
          onMouseEnter={interactive ? () => setIsHovered(true) : undefined}
          onMouseLeave={interactive ? () => setIsHovered(false) : undefined}
        >
          <svg
            width={width}
            height={height}
            viewBox="0 0 56 36"
            className="gesture-stroke-preview__svg"
            aria-hidden="true"
          >
            {renderCanvasEdgeBar(edge, 56, 36)}

            {/* 左侧高精度矢量鼠标轮廓 + 滚轮高亮 */}
            <g className="gesture-stroke-preview__wheel-mouse">
              <rect
                x="10"
                y="6"
                width="16"
                height="24"
                rx="8"
                className="gesture-stroke-preview__wheel-body"
              />
              {effectiveTriggerButton === 'right' && edge === 'none' && (
                <path
                  d="M 18 6 A 8 8 0 0 1 26 14 L 26 16 L 18 16 Z"
                  className="gesture-stroke-preview__wheel-btn-tint"
                />
              )}
              {effectiveTriggerButton === 'left' && (
                <path
                  d="M 18 6 A 8 8 0 0 0 10 14 L 10 16 L 18 16 Z"
                  className="gesture-stroke-preview__wheel-btn-tint"
                />
              )}
              <line
                x1="18"
                y1="6.5"
                x2="18"
                y2="16"
                className="gesture-stroke-preview__wheel-seam"
              />
              <rect
                x="16.3"
                y="9.5"
                width="3.4"
                height="7.2"
                rx="1.7"
                className="gesture-stroke-preview__wheel-roller"
              />
            </g>

            {/* 右侧高对比度上下滚轮方向双雪弗龙矢量徽章 */}
            <g
              className={`gesture-stroke-preview__wheel-arrows ${
                isUp ? 'gesture-stroke-preview__wheel-arrows--up' : 'gesture-stroke-preview__wheel-arrows--down'
              }`}
            >
              {isUp ? (
                <>
                  <path
                    d="M 33.5 16.5 L 39 11 L 44.5 16.5"
                    className="gesture-stroke-preview__wheel-chevron-primary"
                  />
                  <path
                    d="M 33.5 23.5 L 39 18 L 44.5 23.5"
                    className="gesture-stroke-preview__wheel-chevron-secondary"
                  />
                </>
              ) : (
                <>
                  <path
                    d="M 33.5 12.5 L 39 18 L 44.5 12.5"
                    className="gesture-stroke-preview__wheel-chevron-secondary"
                  />
                  <path
                    d="M 33.5 19.5 L 39 25 L 44.5 19.5"
                    className="gesture-stroke-preview__wheel-chevron-primary"
                  />
                </>
              )}
            </g>
          </svg>
        </div>
      </div>
    );
  }

  // 2. 无合法方向段且非滚轮：安全文本回退，绝不绘制假右移折线
  if (!geometry) {
    return (
      <span className={`gesture-arrow ${className}`} title={tooltipText} data-gesture-kind="fallback">
        {fallbackArrows}
      </span>
    );
  }

  // 3. 标准方向笔画矢量轨迹预览
  const { d, startPoint } = geometry;

  return (
    <div
      className={`gesture-stroke-preview-stack ${className}`}
      title={tooltipText}
      data-gesture-kind="stroke"
      data-gesture-edge={edge}
    >
      {edge !== 'none' && edgePillLabel && (
        <span className="gesture-stroke-preview__edge-pill">
          <EdgeMiniGlyph edge={edge} />
          <span>{edgePillLabel}</span>
        </span>
      )}
      <div
        className={`gesture-stroke-preview ${isAnimating ? 'gesture-stroke-preview--animating' : ''}`}
        style={{
          width: `${width}px`,
          height: `${height}px`,
          // @ts-expect-error CSS variable
          '--path-length': `${pathLength}px`,
        }}
        onMouseEnter={interactive ? () => setIsHovered(true) : undefined}
        onMouseLeave={interactive ? () => setIsHovered(false) : undefined}
      >
        <svg
          width={width}
          height={height}
          viewBox={`0 0 ${width} ${height}`}
          className="gesture-stroke-preview__svg"
          aria-hidden="true"
        >
          {/* 0. 边缘触发区域高亮条 */}
          {renderCanvasEdgeBar(edge, width, height)}

          {/* 1. 底层半透明轨迹轨道 */}
          <path
            d={d}
            className="gesture-stroke-preview__track"
            strokeWidth="3.2"
          />

          {/* 2. 顶层动态高亮手势线条 */}
          <path
            ref={pathRef}
            d={d}
            className="gesture-stroke-preview__stroke"
            strokeWidth="2.2"
            style={{
              strokeDasharray: pathLength,
              strokeDashoffset: isAnimating ? undefined : 0,
            }}
          />

          {/* 3. 起点按键纯矢量指示器 (支持右键 / 左键 / 中键 / 侧键 X1 / 侧键 X2) */}
          <g
            transform={`translate(${startPoint.x}, ${startPoint.y})`}
            className="gesture-stroke-preview__trigger-indicator"
          >
            <circle cx="0" cy="0" r="3.8" />
            {effectiveTriggerButton === 'right' && (
              <path d="M 0 -3.8 A 3.8 3.8 0 0 1 0 3.8 Z" />
            )}
            {effectiveTriggerButton === 'left' && (
              <path d="M 0 -3.8 A 3.8 3.8 0 0 0 0 3.8 Z" />
            )}
            {effectiveTriggerButton === 'middle' && (
              <rect x="-1" y="-3.8" width="2" height="7.6" rx="0.5" />
            )}
            {effectiveTriggerButton === 'x1' && (
              <rect x="-3.4" y="0.2" width="2.4" height="2.6" rx="0.6" />
            )}
            {effectiveTriggerButton === 'x2' && (
              <rect x="-3.4" y="-2.8" width="2.4" height="2.6" rx="0.6" />
            )}
          </g>

          {/* 4. 沿轨迹滑动的矢量鼠标光标小箭头 */}
          <g className="gesture-stroke-preview__cursor">
            <path
              d="M 0 0 L 0 8.5 L 2.1 6.5 L 4.0 10.2 L 5.2 9.5 L 3.3 5.8 L 6.0 5.8 Z"
              fill="#ffffff"
              stroke="#1e293b"
              strokeWidth="0.75"
              strokeLinejoin="round"
            />
            {isAnimating && (
              <animateMotion
                path={d}
                dur="1.6s"
                repeatCount="indefinite"
                rotate="0"
              />
            )}
          </g>
        </svg>
      </div>
    </div>
  );
};

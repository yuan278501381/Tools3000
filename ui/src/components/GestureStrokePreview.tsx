/* ─────────────────────────────────────────────────────────────────────────────
 * GestureStrokePreview.tsx — WGestures 2 风格动态手势矢量微缩画板
 *
 * 功能:
 *   - 精致 16:10 宽屏微缩画板 (56x38)，自动居中与等比缩放
 *   - 起点标有 WGestures 2 标志性的鼠标触发按键图标 (如 ◐ 右键 / ◑ 左键)
 *   - 清新青蓝色矢量手势线条 (高对比度平滑转角)
 *   - 悬停/常态下伴随一只真实精巧的 Windows 鼠标小光标沿轨迹动态滑行动画
 * ───────────────────────────────────────────────────────────────────────────── */

import { useMemo, useState, useRef, useEffect, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { codeToArrows } from './gestureModel';
import './GestureStrokePreview.css';

interface Vector {
  dx: number;
  dy: number;
}

const DIR_VECTORS: Record<string, Vector> = {
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

function parseSegments(rawCode: string): string[] {
  if (!rawCode) return [];
  let clean = rawCode.trim().toUpperCase();
  clean = clean.replace(/^((MIDDLE|CTRL|ALT|SHIFT)\+)+/g, '');
  if (clean.includes('-')) {
    return clean.split('-').filter(Boolean);
  }
  return clean.split('').filter(Boolean);
}

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

  const effectiveTriggerButton = useMemo(() => {
    const upper = code.trim().toUpperCase();
    if (upper.startsWith('MIDDLE+')) return 'middle';
    if (upper.startsWith('LEFT+')) return 'left';
    return triggerButton;
  }, [code, triggerButton]);

  const segments = useMemo(() => parseSegments(code), [code]);

  // 计算几何点坐标并等比缩放居中到画板安全区域
  const geometry = useMemo(() => {
    if (segments.length === 0) return null;

    const points: { x: number; y: number }[] = [{ x: 0, y: 0 }];
    let curX = 0;
    let curY = 0;
    const step = 28;

    for (const seg of segments) {
      const vec = DIR_VECTORS[seg] || { dx: 1, dy: 0 };
      curX += vec.dx * step;
      curY += vec.dy * step;
      points.push({ x: curX, y: curY });
    }

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

    // 安全留白边距 (保证起点 ◐ 与终点光标不溢出)
    const paddingX = 9;
    const paddingY = 8;
    const availW = width - paddingX * 2;
    const availH = height - paddingY * 2;

    const scale = Math.min(availW / rawW, availH / rawH, 1.0);

    const offsetX = (width - rawW * scale) / 2 - minX * scale;
    const offsetY = (height - rawH * scale) / 2 - minY * scale;

    const scaledPoints = points.map((p) => ({
      x: Number((p.x * scale + offsetX).toFixed(2)),
      y: Number((p.y * scale + offsetY).toFixed(2)),
    }));

    // 构建平滑 SVG 折线路径
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
  }, [segments, width, height]);

  // 动态测量路径实际长度以驱动 SVG 描边生长
  useEffect(() => {
    if (pathRef.current) {
      const len = pathRef.current.getTotalLength();
      if (len > 0) setPathLength(Math.ceil(len));
    }
  }, [geometry]);

  const fallbackArrows = useMemo(() => codeToArrows(code) || code, [code]);
  const tooltipText = title ?? t('gesture.previewTooltip', 'Gesture: {{arrows}} (Code: {{code}})', { arrows: fallbackArrows, code });

  if (!geometry) {
    return (
      <span className={`gesture-arrow ${className}`} title={tooltipText}>
        {fallbackArrows}
      </span>
    );
  }

  const { d, startPoint } = geometry;
  const isAnimating = autoAnimate || isHovered;

  return (
    <div
      className={`gesture-stroke-preview ${isAnimating ? 'gesture-stroke-preview--animating' : ''} ${className}`}
      style={{
        width: `${width}px`,
        height: `${height}px`,
        // @ts-expect-error CSS variable
        '--path-length': `${pathLength}px`,
      }}
      title={tooltipText}
      onMouseEnter={interactive ? () => setIsHovered(true) : undefined}
      onMouseLeave={interactive ? () => setIsHovered(false) : undefined}
    >
      <svg
        width={width}
        height={height}
        viewBox={`0 0 ${width} ${height}`}
        className="gesture-stroke-preview__svg"
      >
        {/* 1. 底层半透明轨迹轨道 */}
        <path
          d={d}
          className="gesture-stroke-preview__track"
          strokeWidth="3.2"
        />

        {/* 2. 顶层动态高亮手势线条 (清新青蓝) */}
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

        {/* 3. 起点按键指示器 (WGestures 2 标志性 ◐ 触发图标) */}
        <g
          transform={`translate(${startPoint.x}, ${startPoint.y})`}
          className="gesture-stroke-preview__trigger-indicator"
        >
          <circle cx="0" cy="0" r="3.8" />
          {effectiveTriggerButton === 'right' && (
            // 右半圆填充 (右键触发 ◐)
            <path d="M 0 -3.8 A 3.8 3.8 0 0 1 0 3.8 Z" />
          )}
          {effectiveTriggerButton === 'left' && (
            // 左半圆填充 (左键触发 ◑)
            <path d="M 0 -3.8 A 3.8 3.8 0 0 0 0 3.8 Z" />
          )}
          {effectiveTriggerButton === 'middle' && (
            // 中间滚轮条填充
            <rect x="-1" y="-3.8" width="2" height="7.6" rx="0.5" />
          )}
        </g>

        {/* 4. 沿轨迹滑动的真实 Windows 鼠标光标小箭头 (Cursor Pointer) */}
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
  );
};

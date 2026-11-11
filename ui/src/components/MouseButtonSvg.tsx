import type { FC } from 'react';
import type { ScreenEdge, TriggerButton } from './gestureModel';

export type MouseSvgVariant = TriggerButton | 'side' | 'wheel';

interface MouseButtonSvgProps {
  button: MouseSvgVariant;
  size?: number;
  className?: string;
}

/**
 * 高保真纯矢量鼠标物理按键指示器（局部精准高亮左键/右键/中键滚轮/侧键 X1/X2/滚轮滚动）
 * 严格遵循 currentColor 与 aria-hidden 无障碍矢量标准。
 */
export const MouseButtonSvg: FC<MouseButtonSvgProps> = ({ button, size = 14, className }) => (
  <svg
    width={size}
    height={size}
    viewBox="0 0 16 16"
    fill="none"
    aria-hidden="true"
    className={className}
  >
    {/* 鼠标对称机身轮廓 */}
    <rect x="3.2" y="1.4" width="9.6" height="13.2" rx="4.8" stroke="currentColor" strokeWidth="1.3" />
    {/* 顶部中轴分割线 */}
    <line x1="8" y1="1.8" x2="8" y2="7.2" stroke="currentColor" strokeWidth="1.05" strokeOpacity="0.55" />
    <line x1="3.6" y1="7.2" x2="12.4" y2="7.2" stroke="currentColor" strokeWidth="0.95" strokeOpacity="0.35" />

    {button === 'right' && (
      <path d="M 8 1.6 A 4.6 4.6 0 0 1 12.6 6.2 L 12.6 7.2 L 8 7.2 Z" fill="currentColor" />
    )}
    {button === 'left' && (
      <path d="M 8 1.6 A 4.6 4.6 0 0 0 3.4 6.2 L 3.4 7.2 L 8 7.2 Z" fill="currentColor" />
    )}
    {button === 'middle' && (
      <rect x="6.8" y="2.5" width="2.4" height="4.4" rx="1.2" fill="currentColor" />
    )}
    {button === 'x1' && (
      <rect x="1.3" y="8.2" width="2.4" height="3.2" rx="0.9" fill="currentColor" />
    )}
    {button === 'x2' && (
      <rect x="1.3" y="4.5" width="2.4" height="3.2" rx="0.9" fill="currentColor" />
    )}
    {button === 'side' && (
      <>
        <rect x="1.3" y="4.4" width="2.4" height="2.8" rx="0.85" fill="currentColor" />
        <rect x="1.3" y="8.1" width="2.4" height="2.8" rx="0.85" fill="currentColor" fillOpacity="0.75" />
      </>
    )}
    {button === 'wheel' && (
      <>
        <rect x="6.9" y="3.2" width="2.2" height="3.6" rx="1.1" fill="currentColor" />
        <path d="M 6.5 2.4 L 8 1.0 L 9.5 2.4" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round" />
        <path d="M 6.5 8.6 L 8 10.0 L 9.5 8.6" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round" />
      </>
    )}
  </svg>
);

interface ScreenEdgeSvgProps {
  edge: Exclude<ScreenEdge, 'none'> | 'all';
  withWheel?: boolean;
  size?: number;
  className?: string;
}

/**
 * 高保真纯矢量屏幕边缘感应区指示器（顶边/底边/左边/右边/全四边 + 可选滚轮修饰）
 */
export const ScreenEdgeSvg: FC<ScreenEdgeSvgProps> = ({
  edge,
  withWheel = false,
  size = 14,
  className,
}) => (
  <svg
    width={size}
    height={size}
    viewBox="0 0 16 16"
    fill="none"
    aria-hidden="true"
    className={className}
  >
    <rect
      x="1.8"
      y="2.6"
      width="12.4"
      height="10.8"
      rx="2.1"
      stroke="currentColor"
      strokeWidth="1.2"
      strokeOpacity="0.4"
    />
    {(edge === 'top' || edge === 'all') && (
      <line x1="3" y1="2.6" x2="13" y2="2.6" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" />
    )}
    {(edge === 'bottom' || edge === 'all') && (
      <line x1="3" y1="13.4" x2="13" y2="13.4" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" />
    )}
    {(edge === 'left' || edge === 'all') && (
      <line x1="1.8" y1="4" x2="1.8" y2="12" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" />
    )}
    {(edge === 'right' || edge === 'all') && (
      <line x1="14.2" y1="4" x2="14.2" y2="12" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" />
    )}
    {withWheel && (
      <>
        <path d="M 6.6 7.0 L 8 5.5 L 9.4 7.0" stroke="currentColor" strokeWidth="1.25" strokeLinecap="round" strokeLinejoin="round" />
        <path d="M 6.6 9.0 L 8 10.5 L 9.4 9.0" stroke="currentColor" strokeWidth="1.25" strokeLinecap="round" strokeLinejoin="round" />
      </>
    )}
  </svg>
);

export const TriggerItemSvg: FC<{ triggerKey: string; size?: number; className?: string }> = ({
  triggerKey,
  size = 13,
  className,
}) => {
  switch (triggerKey) {
    case 'right':
      return <MouseButtonSvg button="right" size={size} className={className} />;
    case 'middle':
      return <MouseButtonSvg button="middle" size={size} className={className} />;
    case 'xbutton1':
      return <MouseButtonSvg button="x1" size={size} className={className} />;
    case 'xbutton2':
      return <MouseButtonSvg button="x2" size={size} className={className} />;
    case 'left':
      return <MouseButtonSvg button="left" size={size} className={className} />;
    case 'edge_top_slide':
      return <ScreenEdgeSvg edge="top" size={size} className={className} />;
    case 'edge_bottom_slide':
      return <ScreenEdgeSvg edge="bottom" size={size} className={className} />;
    case 'edge_left_slide':
      return <ScreenEdgeSvg edge="left" size={size} className={className} />;
    case 'edge_right_slide':
      return <ScreenEdgeSvg edge="right" size={size} className={className} />;
    case 'edge_top_wheel':
      return <ScreenEdgeSvg edge="top" withWheel size={size} className={className} />;
    case 'edge_bottom_wheel':
      return <ScreenEdgeSvg edge="bottom" withWheel size={size} className={className} />;
    case 'edge_left_wheel':
      return <ScreenEdgeSvg edge="left" withWheel size={size} className={className} />;
    case 'edge_right_wheel':
      return <ScreenEdgeSvg edge="right" withWheel size={size} className={className} />;
    default:
      return <MouseButtonSvg button="right" size={size} className={className} />;
  }
};

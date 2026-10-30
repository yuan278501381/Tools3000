/* ─────────────────────────────────────────────────────────────────────────────
 * WindowResizeHandles.tsx — 沉浸式无边框窗口 8 方向边缘拖拽拉伸控制器
 * ───────────────────────────────────────────────────────────────────────────── */

import { type FC, type MouseEvent, useCallback, useEffect, useState } from 'react';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import './WindowResizeHandles.css';

export interface WindowResizeHandlesProps {
  method?: string;
  showMaximizedCheck?: boolean;
}

export const WindowResizeHandles: FC<WindowResizeHandlesProps> = ({
  method = 'window.startResize',
  showMaximizedCheck = true,
}) => {
  const [isMaximized, setIsMaximized] = useState(false);

  const checkMaximized = useCallback(() => {
    if (!showMaximizedCheck) return;
    bridgeRequest<{ isMaximized: boolean }>('window.isMaximized')
      .then((res) => {
        if (res && typeof res.isMaximized === 'boolean') {
          setIsMaximized(res.isMaximized);
        }
      })
      .catch(() => {});
  }, [showMaximizedCheck]);

  useEffect(() => {
    if (!showMaximizedCheck) return;
    checkMaximized();
    window.addEventListener('resize', checkMaximized);
    return () => window.removeEventListener('resize', checkMaximized);
  }, [checkMaximized, showMaximizedCheck]);

  useBridgeEvent('window:maximizedChanged', (data: unknown) => {
    if (showMaximizedCheck && data && typeof data === 'object' && 'isMaximized' in data) {
      setIsMaximized(Boolean((data as { isMaximized: boolean }).isMaximized));
    }
  });

  const handleMouseDown = useCallback(
    (edge: string) => (e: MouseEvent<HTMLDivElement>) => {
      if (e.button !== 0 || isMaximized) return;
      e.preventDefault();
      e.stopPropagation();
      bridgeRequest(method, { edge, direction: edge }).catch(console.error);
    },
    [isMaximized, method]
  );

  if (isMaximized) return null;

  return (
    <div className="window-resize-handles" aria-hidden="true">
      {/* 4 条直线边框 */}
      <div className="resize-handle resize-handle--top" onMouseDown={handleMouseDown('top')} />
      <div className="resize-handle resize-handle--bottom" onMouseDown={handleMouseDown('bottom')} />
      <div className="resize-handle resize-handle--left" onMouseDown={handleMouseDown('left')} />
      <div className="resize-handle resize-handle--right" onMouseDown={handleMouseDown('right')} />

      {/* 4 个对角拐角 */}
      <div className="resize-handle resize-handle--top-left" onMouseDown={handleMouseDown('top_left')} />
      <div className="resize-handle resize-handle--top-right" onMouseDown={handleMouseDown('top_right')} />
      <div className="resize-handle resize-handle--bottom-left" onMouseDown={handleMouseDown('bottom_left')} />
      <div className="resize-handle resize-handle--bottom-right" onMouseDown={handleMouseDown('bottom_right')} />
    </div>
  );
};

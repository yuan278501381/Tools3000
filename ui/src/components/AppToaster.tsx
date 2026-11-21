/* ─────────────────────────────────────────────────────────────────────────────
 * AppToaster.tsx — 全站统一 Warm Precision 温润精工 Toast 宿主 (SSOT)
 *
 * 设计准则 (Calm Technology × Studio Alabaster / Obsidian Atelier):
 * 1. 彻底关闭高饱和整块染色 (richColors)，采用一致的雕塑白瓷/曜石毛玻璃承载；
 * 2. 语义色彩收敛于左侧 22×22px 微晶图标圆托与异常态极轻微边框，正文永远清晰可读；
 * 3. 内容自适应紧凑宽度 (告别 356px 空旷大白板)，支持单行紧致与多行描述平滑延展；
 * 4. 统一接管主设置窗 (App) 与搜文件浮窗 (SearchApp) 的所有 sonner 通知渲染。
 * ───────────────────────────────────────────────────────────────────────────── */

import type { FC } from 'react';
import { Toaster, type ToasterProps } from 'sonner';
import { Check, Info, AlertTriangle, AlertCircle, Loader2 } from 'lucide-react';
import './AppToaster.css';

export interface AppToasterProps {
  theme: 'light' | 'dark' | 'system';
  /** 表面类型：用于精准计算避让底栏的偏移量 */
  surface?: 'settings' | 'search';
  position?: ToasterProps['position'];
}

export const AppToaster: FC<AppToasterProps> = ({
  theme,
  surface = 'settings',
  position = 'bottom-right',
}) => {
  const offset = surface === 'search'
    ? { bottom: 44, right: 16, left: 16 }
    : { bottom: 20, right: 20, left: 20 };

  return (
    <Toaster
      className="app-toaster"
      position={position}
      theme={theme}
      richColors={false}
      expand={false}
      visibleToasts={3}
      gap={8}
      offset={offset}
      closeButton
      icons={{
        success: (
          <span className="app-toast__badge app-toast__badge--success" aria-hidden="true">
            <Check size={12.5} strokeWidth={2.75} />
          </span>
        ),
        info: (
          <span className="app-toast__badge app-toast__badge--info" aria-hidden="true">
            <Info size={12.5} strokeWidth={2.35} />
          </span>
        ),
        warning: (
          <span className="app-toast__badge app-toast__badge--warning" aria-hidden="true">
            <AlertTriangle size={12.5} strokeWidth={2.35} />
          </span>
        ),
        error: (
          <span className="app-toast__badge app-toast__badge--error" aria-hidden="true">
            <AlertCircle size={12.5} strokeWidth={2.35} />
          </span>
        ),
        loading: (
          <span className="app-toast__badge app-toast__badge--loading" aria-hidden="true">
            <Loader2 size={12.5} strokeWidth={2.35} className="app-toast__spinner" />
          </span>
        ),
      }}
      toastOptions={{
        className: 'app-toast',
        duration: 3200,
      }}
    />
  );
};

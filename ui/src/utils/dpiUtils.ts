/**
 * dpiUtils.ts — 全局高分屏与 Windows DPI 缩放物理坐标换算工具
 * 
 * 解决 Chromium / WebView2 逻辑 CSS 像素 (window.screenX/Y, e.screenX/Y)
 * 与 Windows Win32 原生物理屏幕像素 (TrackPopupMenu, SetWindowPos 等) 之间的缩放断层。
 */

/**
 * 将前端事件的 screenX, screenY 逻辑坐标转换为 Windows 物理屏幕像素坐标
 * @param logicalScreenX 逻辑屏幕 X (例如 MouseEvent.screenX)
 * @param logicalScreenY 逻辑屏幕 Y (例如 MouseEvent.screenY)
 * @returns 包含物理坐标 x, y 的对象
 */
export function toPhysicalScreenPoint(
  logicalScreenX: number,
  logicalScreenY: number
): { x: number; y: number } {
  const dpr = window.devicePixelRatio || 1.0;
  return {
    x: Math.round(logicalScreenX * dpr),
    y: Math.round(logicalScreenY * dpr),
  };
}

/**
 * 将物理像素转换为前端 CSS 逻辑像素
 */
export function toLogicalPixel(physicalPx: number): number {
  const dpr = window.devicePixelRatio || 1.0;
  return dpr > 0 ? physicalPx / dpr : physicalPx;
}

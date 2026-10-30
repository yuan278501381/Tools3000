/* ─────────────────────────────────────────────────────────────────────────────
 * savedToastModel.ts — 全局设置保存 Toast 类型与命令式分发器
 * ───────────────────────────────────────────────────────────────────────────── */

export type SavedToastType = 'success' | 'error' | 'warning' | 'info';

export interface SavedToastDetail {
  message?: string;
  type?: SavedToastType;
  duration?: number;
}

/**
 * 触发全局「已保存」提示
 */
export function showSavedToast(options?: string | SavedToastDetail) {
  if (typeof window === 'undefined') return;
  const detail: SavedToastDetail = typeof options === 'string' ? { message: options } : (options || {});
  window.dispatchEvent(new CustomEvent<SavedToastDetail>('tools3000:settings-saved', { detail }));
}

/**
 * 触发全局「保存失败」提示
 */
export function showSaveFailedToast(error?: unknown, customMessage?: string) {
  if (typeof window === 'undefined') return;
  let errText = '';
  if (typeof error === 'string') {
    errText = error;
  } else if (error && typeof error === 'object' && 'message' in error && typeof error.message === 'string') {
    errText = error.message;
  }
  const detail: SavedToastDetail = {
    message: customMessage || errText || undefined,
    type: 'error',
    duration: 3500,
  };
  window.dispatchEvent(new CustomEvent<SavedToastDetail>('tools3000:settings-save-failed', { detail }));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * hotkeyPauseCoordinator — 全局快捷键录制静音锁引用计数协调器
 *
 * 设计目标:
 *   - 解决用户从快捷键录入框 A 直接点击录入框 B 时，A 的 blur / mousedown-outside
 *     异步发送 `hotkey.setPaused({ paused: false })` 覆盖 B 的静音锁的问题。
 *   - 计数从 0 -> 1 且后端尚未静音时立即发送 `hotkey.setPaused({ paused: true })`；
 *   - 计数归 0 时通过微任务队列确认同一事件周期内无新录入框接管且仍为 0，才发送
 *     `hotkey.setPaused({ paused: false })`。
 * ───────────────────────────────────────────────────────────────────────────── */

import { bridgeRequest } from '../hooks/useBridge';

let activePauseCount = 0;
let backendPaused = false;

export function acquireHotkeyPause(): void {
  activePauseCount += 1;
  if (!backendPaused) {
    backendPaused = true;
    void bridgeRequest('hotkey.setPaused', { paused: true }).catch(() => {});
  }
}

export function releaseHotkeyPause(): void {
  if (activePauseCount <= 0) {
    activePauseCount = 0;
    return;
  }
  activePauseCount -= 1;
  if (activePauseCount === 0) {
    queueMicrotask(() => {
      if (activePauseCount === 0 && backendPaused) {
        backendPaused = false;
        void bridgeRequest('hotkey.setPaused', { paused: false }).catch(() => {});
      }
    });
  }
}

export function getHotkeyPauseRefCount(): number {
  return activePauseCount;
}

/** 仅供单元测试隔离重置协调器内部状态 */
export function resetHotkeyPauseCoordinatorForTest(): void {
  activePauseCount = 0;
  backendPaused = false;
}

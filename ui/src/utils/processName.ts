/**
 * 进程名规范化工具（单一事实源，与 C++ HotkeyManager::normalizeProcessName 保持严格一致）
 */
export function normalizeProcessInput(raw: string): string {
  const trimmed = raw.trim();
  if (!trimmed) return '';
  const base = trimmed.split(/[\\/]/).pop()?.trim().toLowerCase() ?? '';
  if (!base) return '';
  return base.includes('.') ? base : `${base}.exe`;
}

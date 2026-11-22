/**
 * 搜索模块安全 localStorage 读写工具：
 * 统一用 try/catch 包裹所有持久化访问，并对数字、布尔、枚举与数组/对象执行运行时类型校验，
 * 杜绝隐私模式、配额满、损坏 JSON 或非法值（NaN、非数组对象等）引发渲染崩溃。
 */

export function safeStorageGetItem(key: string): string | null {
  try {
    return window.localStorage.getItem(key);
  } catch {
    return null;
  }
}

export function safeStorageSetItem(key: string, value: string): void {
  try {
    window.localStorage.setItem(key, value);
  } catch {
    // 忽略存储配额溢出或受限上下文异常
  }
}

export function safeStorageRemoveItem(key: string): void {
  try {
    window.localStorage.removeItem(key);
  } catch {
    // 忽略受限上下文异常
  }
}

export function safeStorageGetString(key: string, fallback: string): string {
  const raw = safeStorageGetItem(key);
  return typeof raw === 'string' && raw.length > 0 ? raw : fallback;
}

export function safeStorageGetBoolean(key: string, fallback: boolean): boolean {
  const raw = safeStorageGetItem(key);
  if (raw === 'true') return true;
  if (raw === 'false') return false;
  return fallback;
}

export function safeStorageGetEnum<T extends string>(
  key: string,
  allowed: readonly T[],
  fallback: T,
): T {
  const raw = safeStorageGetItem(key);
  if (raw !== null && (allowed as readonly string[]).includes(raw)) {
    return raw as T;
  }
  return fallback;
}

export function safeStorageGetNumber(
  key: string,
  fallback: number,
  options?: {
    min?: number;
    max?: number;
    integer?: boolean;
    allowedValues?: readonly number[];
  },
): number {
  const raw = safeStorageGetItem(key);
  if (raw === null || raw.trim() === '') return fallback;
  const parsed = options?.integer ? Number.parseInt(raw, 10) : Number.parseFloat(raw);
  if (!Number.isFinite(parsed)) return fallback;
  if (options?.allowedValues && !options.allowedValues.includes(parsed)) {
    return fallback;
  }
  if (options?.min !== undefined && parsed < options.min) return fallback;
  if (options?.max !== undefined && parsed > options.max) return fallback;
  return parsed;
}

export function safeStorageGetJsonArray<T>(
  key: string,
  guard: (value: unknown) => value is T,
  fallback: T[],
): T[] {
  const raw = safeStorageGetItem(key);
  if (!raw) return fallback;
  try {
    const parsed: unknown = JSON.parse(raw);
    if (!Array.isArray(parsed)) return fallback;
    return parsed.filter(guard);
  } catch {
    return fallback;
  }
}

export function safeStorageGetJsonRecord(key: string): Record<string, unknown> | null {
  const raw = safeStorageGetItem(key);
  if (!raw) return null;
  try {
    const parsed: unknown = JSON.parse(raw);
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
      return null;
    }
    return parsed as Record<string, unknown>;
  } catch {
    return null;
  }
}

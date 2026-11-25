/* ─────────────────────────────────────────────────────────────────────────────
 * excludedPathsUtils.ts — 自定义排除目录路径归一化、批量解析与合并纯函数 (SSOT)
 * ───────────────────────────────────────────────────────────────────────────── */

export const CUSTOM_EXCLUDED_PATHS_STORAGE_KEY = 'tools3000_search_custom_excluded_paths';
export const MAX_CUSTOM_EXCLUDED_PATHS = 48;

export interface MergeExcludedPathsResult {
  paths: string[];
  added: string[];
  duplicateCount: number;
  capped: boolean;
}

/**
 * 归一化单条排除目录路径：去除首尾空白与成对引号、统一反斜杠、合并冗余分隔符、保留盘符根目录尾斜杠。
 */
export function normalizeExcludedPath(raw: string): string {
  let text = raw.trim();
  while (
    text.length >= 2 &&
    ((text.startsWith('"') && text.endsWith('"')) ||
      (text.startsWith("'") && text.endsWith("'")))
  ) {
    text = text.slice(1, -1).trim();
  }
  if (!text) return '';

  text = text.replace(/\//g, '\\');
  const isUnc = text.startsWith('\\\\');
  if (isUnc) {
    text = '\\\\' + text.slice(2).replace(/\\{2,}/g, '\\');
  } else {
    text = text.replace(/\\{2,}/g, '\\');
  }

  // 裸盘符 "D:" -> "D:\"
  if (/^[a-zA-Z]:$/.test(text)) {
    return `${text[0].toUpperCase()}:\\`;
  }

  // 保留盘符根 "D:\"，其余路径剥离尾斜杠
  if (/^[a-zA-Z]:\\+$/.test(text)) {
    return `${text[0].toUpperCase()}:\\`;
  }

  while (text.endsWith('\\') && text.length > 1) {
    text = text.slice(0, -1);
  }
  if (!text || text === '\\' || text === '\\\\') {
    return '';
  }

  if (/^[a-z]:\\/.test(text)) {
    text = `${text[0].toUpperCase()}${text.slice(1)}`;
  }
  return text;
}

/**
 * 解析用户输入的多条目录路径：支持换行或中英文分号分隔，完整保留路径内空格与逗号。
 */
export function parseExcludedPathInput(raw: string): string[] {
  const seen = new Set<string>();
  const result: string[] = [];
  for (const part of raw.split(/[\r\n;\uff1b]+/)) {
    const normalized = normalizeExcludedPath(part);
    if (!normalized) continue;
    const key = normalized.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    result.push(normalized);
  }
  return result;
}

/**
 * 合并现有与新增排除目录，大小写不敏感去重并遵守最大容量上限。
 */
export function mergeExcludedPaths(
  existing: readonly string[],
  incoming: readonly string[],
  maxItems = MAX_CUSTOM_EXCLUDED_PATHS,
): MergeExcludedPathsResult {
  const seen = new Set<string>();
  const next: string[] = [];
  for (const raw of existing) {
    const clean = normalizeExcludedPath(raw);
    if (!clean) continue;
    const key = clean.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    next.push(clean);
    if (next.length >= maxItems) break;
  }

  const added: string[] = [];
  let duplicateCount = 0;
  let capped = false;

  for (const raw of incoming) {
    const clean = normalizeExcludedPath(raw);
    if (!clean) continue;
    const key = clean.toLowerCase();
    if (seen.has(key)) {
      duplicateCount += 1;
      continue;
    }
    if (next.length >= maxItems) {
      capped = true;
      continue;
    }
    seen.add(key);
    next.push(clean);
    added.push(clean);
  }

  return { paths: next, added, duplicateCount, capped };
}

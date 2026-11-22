import {
  DEFAULT_VISIBLE_TABLE_COLUMNS,
  extractDriveLabel,
  extractFileExtension,
  extractFileStem,
  extractImmediateParentName,
  formatFileAttributesBitmask,
  isKnownTableColumnId,
  resolveColumnDefinition,
} from './columnCatalog';
import type {
  ColumnLayout,
  PropertyCacheMap,
  SearchResult,
  SortDirection,
  SortField,
  TableColumnId,
} from './searchTypes';

export interface TableColumnSpec {
  id: TableColumnId;
  minWidth: number;
  defaultWidth: number;
  maxWidth: number;
  isFlex: boolean;
  sortField: SortField | null;
  align: 'left' | 'right';
}

export const DEFAULT_TABLE_COLUMN_ORDER: readonly TableColumnId[] = [
  ...DEFAULT_VISIBLE_TABLE_COLUMNS,
];

export const TABLE_COLUMN_GAP_PX = 14;

/** 兼容旧引用：保留核心 5 列静态 spec，同时通过 `getTableColumnSpec(id)` 支持全系统任意列。 */
export const TABLE_COLUMN_SPECS: Readonly<Record<string, TableColumnSpec>> = {
  name: {
    id: 'name',
    minWidth: 120,
    defaultWidth: 320,
    maxWidth: 960,
    isFlex: true,
    sortField: 'name',
    align: 'left',
  },
  path: {
    id: 'path',
    minWidth: 140,
    defaultWidth: 460,
    maxWidth: 1600,
    isFlex: true,
    sortField: 'path',
    align: 'left',
  },
  size: {
    id: 'size',
    minWidth: 72,
    defaultWidth: 96,
    maxWidth: 280,
    isFlex: false,
    sortField: 'size',
    align: 'right',
  },
  modified: {
    id: 'modified',
    minWidth: 118,
    defaultWidth: 152,
    maxWidth: 320,
    isFlex: false,
    sortField: 'modified',
    align: 'left',
  },
  created: {
    id: 'created',
    minWidth: 118,
    defaultWidth: 152,
    maxWidth: 320,
    isFlex: false,
    sortField: 'created',
    align: 'left',
  },
};

export function getTableColumnSpec(id: TableColumnId): TableColumnSpec {
  const def = resolveColumnDefinition(id);
  return {
    id: def.id,
    minWidth: def.minWidth,
    defaultWidth: def.defaultWidth,
    maxWidth: def.maxWidth,
    isFlex: def.isFlex,
    sortField: def.id,
    align: def.align,
  };
}

export function clampTableColumnWidth(id: TableColumnId, width: number): number {
  const spec = getTableColumnSpec(id);
  const rounded = Math.round(width);
  return Math.min(spec.maxWidth, Math.max(spec.minWidth, rounded));
}

export function normalizeTableColumnOrder(rawOrder?: readonly string[] | null): TableColumnId[] {
  const result: TableColumnId[] = [];
  if (Array.isArray(rawOrder)) {
    for (const item of rawOrder) {
      if (typeof item === 'string' && isKnownTableColumnId(item) && !result.includes(item)) {
        result.push(item);
      }
    }
  }
  for (const fallback of DEFAULT_TABLE_COLUMN_ORDER) {
    if (!result.includes(fallback)) {
      result.push(fallback);
    }
  }
  return result;
}

export function normalizeVisibleTableColumns(rawVisible?: readonly string[] | null): TableColumnId[] {
  const result: TableColumnId[] = [];
  if (Array.isArray(rawVisible)) {
    for (const item of rawVisible) {
      if (typeof item === 'string' && isKnownTableColumnId(item) && !result.includes(item)) {
        result.push(item);
      }
    }
  }
  if (!result.includes('name')) {
    result.unshift('name');
  }
  return result;
}

export function sanitizeTableColumnWidths(
  rawWidths?: Record<string, unknown> | null,
): Partial<Record<TableColumnId, number>> {
  const clean: Partial<Record<TableColumnId, number>> = {};
  if (!rawWidths || typeof rawWidths !== 'object') {
    return clean;
  }
  for (const [id, val] of Object.entries(rawWidths)) {
    if (!isKnownTableColumnId(id)) continue;
    if (typeof val === 'number' && Number.isFinite(val) && val > 0) {
      clean[id] = clampTableColumnWidth(id, val);
    }
  }
  return clean;
}

export function getVisibleTableColumns(columns: ColumnLayout): TableColumnId[] {
  if (Array.isArray(columns.visibleTableColumns) && columns.visibleTableColumns.length > 0) {
    const visibleSet = new Set(normalizeVisibleTableColumns(columns.visibleTableColumns));
    const ordered = normalizeTableColumnOrder([
      ...(columns.tableOrder ?? []),
      ...Array.from(visibleSet),
    ]);
    return ordered.filter((id) => visibleSet.has(id));
  }

  const ordered = normalizeTableColumnOrder(columns.tableOrder);
  return ordered.filter((id) => {
    if (id === 'name') return true;
    if (id === 'path') return columns.path;
    if (id === 'size') return columns.size;
    if (id === 'modified') return columns.modified;
    if (id === 'created') return columns.created;
    return false;
  });
}

export function reorderTableColumns(
  currentOrder: readonly TableColumnId[],
  sourceId: TableColumnId,
  targetId: TableColumnId,
  side: 'before' | 'after',
): TableColumnId[] {
  const normalized = normalizeTableColumnOrder([
    ...currentOrder,
    sourceId,
    targetId,
  ]);
  if (sourceId === targetId) return normalized;

  const withoutSource = normalized.filter((id) => id !== sourceId);
  const targetIndex = withoutSource.indexOf(targetId);
  if (targetIndex < 0) return normalized;

  const insertIndex = side === 'after' ? targetIndex + 1 : targetIndex;
  const next = [...withoutSource];
  next.splice(insertIndex, 0, sourceId);

  const unchanged = next.every((id, idx) => id === normalized[idx]);
  return unchanged ? normalized : next;
}

export interface TableLayoutMetrics {
  visibleCols: TableColumnId[];
  gridTemplateColumns: string;
  /** 所有可见列在不挤扁时的最小总像素宽（含列间距），用于多列启用时触发平滑横向滚动。 */
  minTableWidthPx: number;
}

/**
 * 表格视图（Table View）列轨道单一事实源 (SSOT)：
 * 表头与虚拟列表每一行的主信息层严格共享此 CSS Grid 列定义，保障 1px 不差的垂直对齐。
 */
export function buildTableLayoutMetrics(columns: ColumnLayout): TableLayoutMetrics {
  const visibleCols = getVisibleTableColumns(columns);
  const widths = columns.tableWidths ?? {};

  const flexCandidates = visibleCols.filter(
    (id) => getTableColumnSpec(id).isFlex && widths[id] === undefined,
  );

  let minWidthSum = 0;
  const tracks = visibleCols.map((id) => {
    const spec = getTableColumnSpec(id);
    const customWidth = widths[id];

    if (spec.isFlex) {
      if (customWidth !== undefined) {
        // 用户手动调整（或自适应拟合）列宽后必须严格遵守精确像素值，
        // 严禁再套用 minmax(px, 1fr) 强行撑宽导致在宽窗口下向左完全缩不动。
        const clamped = clampTableColumnWidth(id, customWidth);
        minWidthSum += clamped;
        return `${clamped}px`;
      }
      // 当可见列超过 5 列（开启了扩展属性列）时，保证名称与路径保留舒适底宽防止横向挤压
      const floorPx = visibleCols.length > 5
        ? Math.max(spec.minWidth, id === 'name' ? 200 : 220)
        : spec.minWidth;
      minWidthSum += floorPx;
      const weight = id === 'name'
        ? Math.max(10, columns.nameFlex || 35)
        : Math.max(10, columns.pathFlex || 45);
      return flexCandidates.length === 1
        ? `minmax(${floorPx}px, 1fr)`
        : `minmax(${floorPx}px, ${weight}fr)`;
    }

    const px = customWidth !== undefined
      ? clampTableColumnWidth(id, customWidth)
      : spec.defaultWidth;
    minWidthSum += px;
    return `${px}px`;
  });

  const gapTotal = Math.max(0, visibleCols.length - 1) * TABLE_COLUMN_GAP_PX;
  return {
    visibleCols,
    gridTemplateColumns: tracks.join(' '),
    minTableWidthPx: minWidthSum + gapTotal,
  };
}

export function buildTableGridTemplate(columns: ColumnLayout): string {
  return buildTableLayoutMetrics(columns).gridTemplateColumns;
}

/**
 * 估算一列内容的视觉像素宽度（用于表头右键“调整此列适应内容”与“调整全部列适应内容”）：
 * 使用 Canvas 2D `measureText`（不可用时按全角/半角字宽回退估算），兼顾中英文、等宽数字与图标留白。
 */
let cachedMeasureCtx: CanvasRenderingContext2D | null | undefined;
function measureTextVisualPx(text: string, isMono: boolean): number {
  if (!text) return 0;
  if (cachedMeasureCtx === undefined) {
    try {
      const canvas = document.createElement('canvas');
      cachedMeasureCtx = canvas.getContext('2d');
    } catch {
      cachedMeasureCtx = null;
    }
  }
  if (cachedMeasureCtx) {
    cachedMeasureCtx.font = isMono
      ? '500 12.5px "Cascadia Code", Consolas, monospace'
      : '550 13.5px "Noto Sans SC", "Segoe UI", sans-serif';
    return Math.ceil(cachedMeasureCtx.measureText(text).width);
  }
  let width = 0;
  for (let i = 0; i < text.length; i += 1) {
    width += text.charCodeAt(i) > 0x7f ? 13.5 : 7.6;
  }
  return Math.ceil(width);
}

export function estimateFittedColumnWidth(
  colId: TableColumnId,
  headerLabel: string,
  sampleValues: readonly string[],
): number {
  const def = resolveColumnDefinition(colId);
  const headerPx = measureTextVisualPx(headerLabel, false) + 36;
  const extraCellPadding = colId === 'name' ? 46 : 22;
  let maxPx = headerPx;

  const limit = Math.min(sampleValues.length, 120);
  for (let i = 0; i < limit; i += 1) {
    const val = sampleValues[i];
    if (!val) continue;
    const textPx = measureTextVisualPx(val, Boolean(def.isMono)) + extraCellPadding;
    if (textPx > maxPx) {
      maxPx = textPx;
    }
  }
  // 单列拟合上限避免超长路径一次撑破视口
  const fitCeiling = colId === 'path' ? 680 : colId === 'name' ? 520 : def.maxWidth;
  return clampTableColumnWidth(colId, Math.min(fitCeiling, maxPx));
}

/**
 * 为自适应列宽采样结果行：优先采集当前可见视口（含前后缓冲），
 * 不足上限时再从列表头部补齐，确保长列表滚动后列宽拟合精准贴合当前所见内容。
 */
export function collectColumnFitSampleRows(
  results: readonly SearchResult[],
  visibleRange: { start: number; end: number },
  maxSamples = 120,
): SearchResult[] {
  if (results.length <= maxSamples) {
    return results.slice();
  }
  const sampled: SearchResult[] = [];
  const seenIndices = new Set<number>();

  const winStart = Math.max(0, Math.min(results.length - 1, visibleRange.start - 10));
  const winEnd = Math.min(results.length, Math.max(visibleRange.end + 10, winStart + 60));

  for (let i = winStart; i < winEnd && sampled.length < maxSamples; i += 1) {
    seenIndices.add(i);
    sampled.push(results[i]);
  }
  for (let i = 0; i < results.length && sampled.length < maxSamples; i += 1) {
    if (!seenIndices.has(i)) {
      sampled.push(results[i]);
    }
  }
  return sampled;
}

export function formatBytes(bytes: number): string {
  if (!bytes || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  const index = Math.floor(Math.log(bytes) / Math.log(1024));
  return `${(bytes / Math.pow(1024, index)).toFixed(index === 0 ? 0 : 1)} ${units[index]}`;
}

export function extractParentDirectoryPath(fullPath: string): string {
  if (!fullPath) return '';
  const clean = fullPath.replace(/[/\\]+$/, '');
  const lastSlash = Math.max(clean.lastIndexOf('/'), clean.lastIndexOf('\\'));
  if (lastSlash <= 0) return '';
  const parentPath = clean.slice(0, lastSlash);
  if (/^[a-zA-Z]:$/.test(parentPath)) {
    return `${parentPath}\\`;
  }
  return parentPath;
}

export function extractParentFolder(fullPath: string): string {
  if (!fullPath) return '';
  const clean = fullPath.replace(/[/\\]+$/, '');
  const lastSlash = Math.max(clean.lastIndexOf('/'), clean.lastIndexOf('\\'));
  if (lastSlash <= 0) return '';
  const parentPath = clean.slice(0, lastSlash);
  const secondLastSlash = Math.max(parentPath.lastIndexOf('/'), parentPath.lastIndexOf('\\'));
  const immediateParent = secondLastSlash >= 0 ? parentPath.slice(secondLastSlash + 1) : parentPath;

  const normalizedParentPath = `${parentPath.replace(/\//g, '\\').toLowerCase()}\\`;
  if (
    normalizedParentPath.includes('\\xwechat_files\\') ||
    normalizedParentPath.includes('\\xwechat\\') ||
    normalizedParentPath.includes('\\wechat files\\')
  ) {
    return immediateParent ? `WeChat\u00b7${immediateParent}` : 'WeChat';
  }
  if (normalizedParentPath.includes('\\wxwork\\') || normalizedParentPath.includes('\\wecom\\')) {
    return immediateParent ? `WeCom\u00b7${immediateParent}` : 'WeCom';
  }
  if (normalizedParentPath.includes('\\tencent files\\') || normalizedParentPath.includes('\\nt_qq\\')) {
    return immediateParent ? `QQ\u00b7${immediateParent}` : 'QQ';
  }
  return immediateParent;
}

/**
 * 自带文件实例级独立内嵌图标的 Windows 扩展名集合：
 * 这类文件在 Windows 资源管理器中每个文件显示各自内嵌的程序/快捷方式/光标图标，
 * 必须以具体文件路径作为缓存键提取真实图标，而非共用单一的扩展名默认白框图标。
 */
const PER_INSTANCE_ICON_EXTENSIONS = new Set([
  '.exe',
  '.lnk',
  '.ico',
  '.cur',
  '.ani',
  '.scr',
  '.cpl',
  '.msc',
  '.appref-ms',
  '.url',
]);

export interface NativeIconRequestSpec {
  cacheKey: string;
  extension: string;
  isDirectory: boolean;
  filePath?: string;
}

export function resolveNativeIconRequest(item: SearchResult): NativeIconRequestSpec {
  const dotIndex = item.name.lastIndexOf('.');
  const extension = !item.isDirectory && dotIndex >= 0 ? item.name.slice(dotIndex).toLowerCase() : '';
  if (item.isDirectory) {
    return {
      cacheKey: '::dir::',
      extension: '',
      isDirectory: true,
    };
  }
  if (item.path && PER_INSTANCE_ICON_EXTENSIONS.has(extension)) {
    const normalizedPath = item.path.replace(/\//g, '\\').toLowerCase();
    return {
      cacheKey: `path:${normalizedPath}`,
      extension,
      isDirectory: false,
      filePath: item.path,
    };
  }
  return {
    cacheKey: extension,
    extension,
    isDirectory: false,
  };
}

export function formatFileSize(bytes?: number, isDirectory?: boolean): string {
  if (isDirectory) return '';
  if (bytes === undefined || bytes === null || bytes === 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let size = bytes;
  let unitIndex = 0;
  while (size >= 1024 && unitIndex < units.length - 1) {
    size /= 1024;
    unitIndex += 1;
  }
  return `${size.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
}

export function formatWindowsTime(fileTime?: number | string): string {
  if (!fileTime) return '';
  const numericFileTime = typeof fileTime === 'string' ? parseInt(fileTime, 10) : fileTime;
  if (!numericFileTime) return '';
  const milliseconds = Math.floor((numericFileTime - 116444736000000000) / 10000);
  if (milliseconds <= 0) return '';
  const date = new Date(milliseconds);
  if (Number.isNaN(date.getTime())) return '';

  const pad = (value: number) => (value < 10 ? `0${value}` : String(value));
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

function getPathShallowScore(fullPath: string): number {
  if (!fullPath) return 0;
  const clean = fullPath.replace(/\//g, '\\');
  let score = 0;
  if (clean.length >= 2 && clean[1] === ':' && clean[0].toUpperCase() !== 'C') {
    score += 100;
  }
  const slashCount = (clean.match(/\\/g) || []).length;
  score += Math.max(0, 20 - slashCount);
  return score;
}

type SortableCellValue =
  | { missing: true }
  | { missing: false; num: number; str?: string }
  | { missing: false; num?: undefined; str: string };

const MISSING_CELL_VALUE: SortableCellValue = { missing: true };

function extractSortableCellValue(
  item: SearchResult,
  sortField: SortField,
  propertyCache?: PropertyCacheMap,
): SortableCellValue {
  switch (sortField) {
    case 'modified': {
      const ts = Number(item.lastWriteTime);
      return Number.isFinite(ts) && ts > 0 ? { missing: false, num: ts } : MISSING_CELL_VALUE;
    }
    case 'created': {
      const ts = Number(item.creationTime);
      return Number.isFinite(ts) && ts > 0 ? { missing: false, num: ts } : MISSING_CELL_VALUE;
    }
    case 'accessed': {
      const nativeTs = Number(item.lastAccessTime);
      if (Number.isFinite(nativeTs) && nativeTs > 0) {
        return { missing: false, num: nativeTs };
      }
      const cached = propertyCache?.[item.path]?.accessed;
      if (typeof cached?.n === 'number' && Number.isFinite(cached.n) && cached.n > 0) {
        return { missing: false, num: cached.n, str: cached.d?.trim() || undefined };
      }
      const text = cached?.d?.trim() ?? '';
      return text && text !== '—' ? { missing: false, str: text } : MISSING_CELL_VALUE;
    }
    case 'size': {
      if (item.isDirectory) return MISSING_CELL_VALUE;
      if (item.size === undefined || item.size === null) return MISSING_CELL_VALUE;
      const bytes = Number(item.size);
      return Number.isFinite(bytes) && bytes >= 0 ? { missing: false, num: bytes } : MISSING_CELL_VALUE;
    }
    case 'name': {
      const name = item.name ?? '';
      return name.trim().length > 0 ? { missing: false, str: name } : MISSING_CELL_VALUE;
    }
    case 'path': {
      const dir = extractParentDirectoryPath(item.path) || item.path || '';
      return dir.trim().length > 0 ? { missing: false, str: dir } : MISSING_CELL_VALUE;
    }
    case 'ext': {
      const ext = extractFileExtension(item.name, item.isDirectory);
      return ext.length > 0 ? { missing: false, str: ext } : MISSING_CELL_VALUE;
    }
    case 'attributes': {
      const attr = (
        formatFileAttributesBitmask(item.attributes) ||
        propertyCache?.[item.path]?.attributes?.d?.trim() ||
        (item.isDirectory ? 'D' : '')
      );
      return attr.length > 0 && attr !== '—' ? { missing: false, str: attr } : MISSING_CELL_VALUE;
    }
    case 'runCount': {
      const runs = Number(item.runCount);
      return Number.isFinite(runs) && runs > 0 ? { missing: false, num: runs } : MISSING_CELL_VALUE;
    }
    case 'stem': {
      const stem = extractFileStem(item.name, item.isDirectory);
      return stem.length > 0 ? { missing: false, str: stem } : MISSING_CELL_VALUE;
    }
    case 'parentName': {
      const parent = extractImmediateParentName(item.path);
      return parent.length > 0 ? { missing: false, str: parent } : MISSING_CELL_VALUE;
    }
    case 'drive': {
      const drive = extractDriveLabel(item.path);
      return drive.length > 0 ? { missing: false, str: drive } : MISSING_CELL_VALUE;
    }
    case 'fullPathLength': {
      return item.path && item.path.length > 0 ? { missing: false, num: item.path.length } : MISSING_CELL_VALUE;
    }
    case 'nameLength': {
      return item.name && item.name.length > 0 ? { missing: false, num: item.name.length } : MISSING_CELL_VALUE;
    }
    default: {
      const cell = propertyCache?.[item.path]?.[sortField];
      if (!cell) return MISSING_CELL_VALUE;
      const hasNum = typeof cell.n === 'number' && Number.isFinite(cell.n);
      const rawText = typeof cell.d === 'string' ? cell.d.trim() : '';
      const hasStr = rawText.length > 0 && rawText !== '—';
      if (!hasNum && !hasStr) return MISSING_CELL_VALUE;
      if (hasNum) {
        return { missing: false, num: cell.n as number, str: hasStr ? rawText : undefined };
      }
      return { missing: false, str: rawText };
    }
  }
}

/**
 * 严格按指定表头列值比较两条记录：
 * - 缺失元数据（`—` / undefined / null / NaN / 空字符串）无论升序或降序一律沉底；
 * - 有效值严格按用户选定的升序/降序排列；
 * - 文本比较使用传入的 locale（回退为 'en'）+ 自然数字排序。
 */
export function compareByColumnField(
  left: SearchResult,
  right: SearchResult,
  sortField: SortField,
  sortDirection: SortDirection,
  propertyCache?: PropertyCacheMap,
  locale: string = 'en',
): number {
  const lVal = extractSortableCellValue(left, sortField, propertyCache);
  const rVal = extractSortableCellValue(right, sortField, propertyCache);

  if (lVal.missing && rVal.missing) return 0;
  if (lVal.missing) return 1;
  if (rVal.missing) return -1;

  let rawDiff: number;
  if (lVal.num !== undefined && rVal.num !== undefined) {
    rawDiff = lVal.num - rVal.num;
  } else {
    const lStr = lVal.str ?? (lVal.num !== undefined ? String(lVal.num) : '');
    const rStr = rVal.str ?? (rVal.num !== undefined ? String(rVal.num) : '');
    const useAsciiLocale = sortField === 'ext' || sortField === 'attributes' || sortField === 'drive';
    rawDiff = useAsciiLocale
      ? lStr.localeCompare(rStr, 'en', { sensitivity: 'base' })
      : lStr.localeCompare(rStr, locale, { numeric: true, sensitivity: 'base' });
  }

  if (rawDiff === 0) return 0;
  return sortDirection === 'asc' ? rawDiff : -rawDiff;
}

/**
 * 纯粹的表头列直控排序引擎：
 * 1. 100% 严格按用户选定的表头列与升降序排列（空值永远沉底）；
 * 2. 列值完全相等时，依次按 `matchRank` -> 浅路径 -> 本地化自然文件名 -> 完整路径决胜。
 */
export function getSortedResults(
  results: SearchResult[],
  sortField: SortField,
  sortDirection: SortDirection,
  propertyCache?: PropertyCacheMap,
  locale: string = 'en',
): SearchResult[] {
  if (results.length <= 1) return results;
  const resolvedLocale = locale && locale.trim() ? locale : 'en';

  return [...results].sort((left, right) => {
    const columnComparison = compareByColumnField(
      left,
      right,
      sortField,
      sortDirection,
      propertyCache,
      resolvedLocale,
    );
    if (columnComparison !== 0) return columnComparison;

    const lHasRank = typeof left.matchRank === 'number' && Number.isFinite(left.matchRank);
    const rHasRank = typeof right.matchRank === 'number' && Number.isFinite(right.matchRank);
    if (lHasRank && rHasRank && left.matchRank !== right.matchRank) {
      return (left.matchRank as number) - (right.matchRank as number);
    }
    if (lHasRank !== rHasRank) {
      return lHasRank ? -1 : 1;
    }

    const shallowDiff = getPathShallowScore(right.path) - getPathShallowScore(left.path);
    if (shallowDiff !== 0) return shallowDiff;

    const nameDiff = left.name.localeCompare(right.name, resolvedLocale, {
      numeric: true,
      sensitivity: 'base',
    });
    if (nameDiff !== 0) return nameDiff;

    return left.path.localeCompare(right.path, resolvedLocale, {
      numeric: true,
      sensitivity: 'base',
    });
  });
}

const EXPLICIT_SORT_ALIAS_MAP: Readonly<Record<string, { field: TableColumnId; defaultDir: SortDirection }>> = {
  date: { field: 'modified', defaultDir: 'desc' },
  mtime: { field: 'modified', defaultDir: 'desc' },
  time: { field: 'modified', defaultDir: 'desc' },
  modified: { field: 'modified', defaultDir: 'desc' },
  created: { field: 'created', defaultDir: 'desc' },
  accessed: { field: 'accessed', defaultDir: 'desc' },
  name: { field: 'name', defaultDir: 'asc' },
  size: { field: 'size', defaultDir: 'desc' },
  path: { field: 'path', defaultDir: 'asc' },
  ext: { field: 'ext', defaultDir: 'asc' },
};

/**
 * 精准解析独立 `sort:` 查询词元（仅匹配完整独立词元，避免粗糙子串 includes 误触发），
 * 支持可选方向后缀（如 `sort:size-asc`、`sort:modified-desc`）。
 */
export function parseExplicitSortDirective(
  query: string,
): { field: TableColumnId; direction: SortDirection } | null {
  if (!query || !query.includes(':')) return null;
  const tokens = tokenizeQueryQuoteAware(query);
  let matched: { field: TableColumnId; direction: SortDirection } | null = null;
  for (const token of tokens) {
    const match = token.match(/^sort:([a-z]+)(?:-(asc|desc))?$/i);
    if (!match) continue;
    const spec = EXPLICIT_SORT_ALIAS_MAP[match[1].toLowerCase()];
    if (!spec) continue;
    const explicitDir = match[2]?.toLowerCase() as SortDirection | undefined;
    matched = {
      field: spec.field,
      direction: explicitDir ?? spec.defaultDir,
    };
  }
  return matched;
}

const FILE_SCOPE_FILTER_PREFIXES = [
  'ext:',
  'file:',
  'folder:',
  'dir:',
  'path:',
  'parent:',
  'p:',
  'size:',
  'dm:',
  'date:',
  'datemodified:',
  'modified:',
  'dc:',
  'datecreated:',
  'created:',
  'exact:',
  'wfn:',
  'wholefilename:',
  'regex:',
  'r:',
  'case:',
  'nocase:',
  'pinyin:',
  'py:',
  'sort:',
  'orderby:',
] as const;

/**
 * 引号感知分词：使配对双引号内部的空格与竖线（如 `content:"hello world"`、`path:"C:\Program Files"`）
 * 保持在同一个词元内不被劈开，同时将未加引号的逻辑竖线 `|` 拆分为独立词元。
 */
export function tokenizeQueryQuoteAware(query: string): string[] {
  const tokens: string[] = [];
  const n = query.length;
  let i = 0;
  while (i < n) {
    while (i < n && /\s/.test(query[i])) i += 1;
    if (i >= n) break;

    if (query[i] === '|') {
      tokens.push('|');
      i += 1;
      continue;
    }

    const start = i;
    let inQuotes = false;
    while (i < n) {
      const ch = query[i];
      if (ch === '"') {
        inQuotes = !inQuotes;
        i += 1;
        continue;
      }
      if (!inQuotes && (/\s/.test(ch) || ch === '|')) {
        break;
      }
      i += 1;
    }
    if (i > start) {
      tokens.push(query.slice(start, i));
    }
  }
  return tokens;
}

export function stripOuterPairedQuotes(value: string): string {
  const trimmed = value.trim();
  if (trimmed.length >= 2 && trimmed.startsWith('"') && trimmed.endsWith('"')) {
    return trimmed.slice(1, -1);
  }
  return trimmed;
}

interface ContentPrefixMatch {
  isContent: boolean;
  negated: boolean;
  allowsSpaceValue: boolean;
  rawValue: string;
}

function matchContentPrefixToken(token: string): ContentPrefixMatch {
  const negated = token.startsWith('!');
  const body = negated ? token.slice(1) : token;
  const lower = body.toLowerCase();

  if (lower.startsWith('content:')) {
    return {
      isContent: true,
      negated,
      allowsSpaceValue: true,
      rawValue: body.slice('content:'.length),
    };
  }
  if (body.startsWith('\u5185\u5bb9:')) {
    return {
      isContent: true,
      negated,
      allowsSpaceValue: true,
      rawValue: body.slice('\u5185\u5bb9:'.length),
    };
  }
  if (lower.startsWith('c:') && body.length > 2 && body[2] !== '\\' && body[2] !== '/') {
    return {
      isContent: true,
      negated,
      allowsSpaceValue: false,
      rawValue: body.slice(2),
    };
  }
  return {
    isContent: false,
    negated,
    allowsSpaceValue: false,
    rawValue: '',
  };
}

/**
 * 判断一个词元是否属于文件范围限定器或逻辑修饰符（在切换全文搜索时应当原位保留，不冠以 `content:`）。
 */
export function isFileScopeFilterToken(token: string): boolean {
  if (!token) return false;
  if (token === '|') return true;
  if (token.startsWith('!')) return true;

  const isQuoted = token.startsWith('"') && token.endsWith('"') && token.length >= 2;
  if (!isQuoted && (token.includes('*') || token.includes('?'))) {
    return true;
  }

  // 盘符或路径词元，如 c:, D:, D:\Work, C:/repo, \src\
  if (/^[a-zA-Z]:(?:[\\/].*)?$/.test(token)) {
    return true;
  }
  if (!isQuoted && token.includes('\\')) {
    return true;
  }

  const lower = token.toLowerCase();
  if (FILE_SCOPE_FILTER_PREFIXES.some((prefix) => lower.startsWith(prefix))) {
    return true;
  }
  return matchContentPrefixToken(token).isContent;
}

/**
 * 识别查询串任意词元位置是否显式包含 `content:`、`内容:` 或非盘符路径简写 `c:xxx`。
 * 完美支持 `ext:docx content:合同`、`D:\Work 内容:预算`、`size:>10kb c:甲方` 等组合语法。
 */
export function hasContentSyntaxInQuery(query: string): boolean {
  const tokens = tokenizeQueryQuoteAware(query);
  for (const token of tokens) {
    if (matchContentPrefixToken(token).isContent) {
      return true;
    }
  }
  return false;
}

/**
 * 提取查询串中所有显式声明的全文检索词（支持 `content:"hello world"` 与 `content: 合同` 冒号后空格）。
 */
export function extractExplicitContentTerms(query: string): string[] {
  const tokens = tokenizeQueryQuoteAware(query);
  const terms: string[] = [];
  for (let i = 0; i < tokens.length; i += 1) {
    const match = matchContentPrefixToken(tokens[i]);
    if (!match.isContent) continue;

    let value = match.rawValue;
    if (!value && match.allowsSpaceValue && i + 1 < tokens.length) {
      const nextToken = tokens[i + 1];
      if (!isFileScopeFilterToken(nextToken)) {
        value = nextToken;
        i += 1;
      }
    }
    const unquoted = stripOuterPairedQuotes(value);
    if (unquoted.length > 0) {
      terms.push(unquoted);
    }
  }
  return terms;
}

/**
 * 判断查询串是否包含了显式内容搜索语法但尚未输入有效正文关键词（如 `content:` 或 `ext:docx content:`）。
 */
export function hasEmptyContentKeywordSyntax(query: string): boolean {
  const lowered = query.trim().toLowerCase();
  if (lowered === 'content:' || lowered === '\u5185\u5bb9:' || lowered === 'c:') {
    return true;
  }
  if (!hasContentSyntaxInQuery(query)) {
    return false;
  }
  return extractExplicitContentTerms(query).length === 0;
}

/**
 * 统一判断当前搜索上下文是否处于全文内容检索状态（涵盖分类胶囊、搜索模式 `content`/`both` 及任意位置显式语法）。
 */
export function isEffectiveContentSearch(input: {
  query: string;
  activeCategory?: string;
  searchMode?: string;
}): boolean {
  if (input.activeCategory === 'content') return true;
  if (input.searchMode === 'content' || input.searchMode === 'both') return true;
  return hasContentSyntaxInQuery(input.query);
}

/**
 * 智能将当前查询切换为显式全文内容检索：
 * - 若已含任意位置 `content:` / `内容:` / `c:xxx`，原样返回，绝不重复拼接损坏语法；
 * - 否则原位保留所有 `ext:`、盘符路径、大小/时间、通配符与排除项过滤词，仅对自由关键词冠以 `content:`；
 * - 若查询仅含文件过滤词而无自由词（如 `ext:docx D:\Work`），则在末尾追加 ` content:` 等待输入。
 */
export function buildSmartContentSearchQuery(query: string): string {
  const trimmed = query.trim();
  if (!trimmed) return 'content:';
  if (hasContentSyntaxInQuery(trimmed)) {
    return trimmed;
  }

  const tokens = tokenizeQueryQuoteAware(trimmed);
  let convertedFreeTokens = 0;
  const mapped = tokens.map((token) => {
    if (isFileScopeFilterToken(token)) {
      return token;
    }
    convertedFreeTokens += 1;
    return `content:${token}`;
  });

  const joined = mapped.join(' ');
  if (convertedFreeTokens === 0) {
    return `${joined} content:`;
  }
  return joined;
}

/**
 * 从查询串中无损剥离任意位置的显式 `content:` / `内容:` / `c:` 前缀，保留自由关键词与全部文件范围过滤条件。
 */
export function stripContentSyntaxFromQuery(query: string): string {
  const trimmed = query.trim();
  if (!trimmed) return '';

  const tokens = tokenizeQueryQuoteAware(trimmed);
  const output: string[] = [];
  for (let i = 0; i < tokens.length; i += 1) {
    const token = tokens[i];
    const match = matchContentPrefixToken(token);
    if (!match.isContent || match.negated) {
      output.push(token);
      continue;
    }

    if (!match.rawValue && match.allowsSpaceValue) {
      if (i + 1 < tokens.length && !isFileScopeFilterToken(tokens[i + 1])) {
        output.push(tokens[i + 1]);
        i += 1;
      }
      continue;
    }

    if (match.rawValue) {
      output.push(match.rawValue);
    }
  }
  return output.join(' ');
}

const FILTER_ONLY_KEYWORD_PREFIXES = new Set([
  'ext',
  'size',
  'len',
  'dm',
  'date',
  'datemodified',
  'modified',
  'dc',
  'datecreated',
  'created',
  'da',
  'dateaccessed',
  'accessed',
  'sort',
  'orderby',
  'attrib',
  'attributes',
  'type',
  'kind',
  'dupe',
  'parent',
  'p',
  'path',
  'infolder',
  'nosubfolders',
  'regex',
  'r',
  'pinyin',
  'py',
]);

const VALUE_BEARING_KEYWORD_PREFIXES = new Set([
  'content',
  '\u5185\u5bb9',
  'c',
  'case',
  'cs',
  'nocase',
  'exact',
  'wfn',
  'wholefilename',
  'nopy',
  'file',
  'folder',
  'dir',
  'bm',
]);

/**
 * 从原始查询文本中提取用于行高亮的核心关键词列表，剔除 `!` 排除项与已知冒号算子。
 */
export function parseQueryKeywords(query: string): string[] {
  const trimmed = query.trim();
  if (!trimmed) return [];

  const pushCleanedWords = (rawSegment: string, target: string[]) => {
    const unquoted = stripOuterPairedQuotes(rawSegment);
    for (const part of unquoted.split(/\s+/)) {
      const clean = part.replace(/[*?"]/g, '').trim();
      if (clean.length > 0 && !target.includes(clean)) {
        target.push(clean);
      }
    }
  };

  const tokens = tokenizeQueryQuoteAware(trimmed);
  const keywords: string[] = [];

  for (let i = 0; i < tokens.length; i += 1) {
    const token = tokens[i];
    if (!token || token === '|') continue;
    // 剔除所有 ! 排除项（含 !"quoted phrase"）及独立逻辑连接符
    if (token.startsWith('!')) continue;
    if (token === 'NOT' || token === 'OR' || token === 'AND') continue;
    // 跳过盘符或盘符路径限定词（如 c:, D:\Work）
    if (/^[a-zA-Z]:(?:[\\/].*)?$/.test(token)) continue;

    const colonPos = token.indexOf(':');
    if (colonPos > 0) {
      const prefix = token.slice(0, colonPos).toLowerCase();
      const rest = token.slice(colonPos + 1);
      if (FILTER_ONLY_KEYWORD_PREFIXES.has(prefix)) {
        continue;
      }
      if (VALUE_BEARING_KEYWORD_PREFIXES.has(prefix)) {
        if (rest.length > 0) {
          pushCleanedWords(rest, keywords);
        }
        continue;
      }
    }

    pushCleanedWords(token, keywords);
  }
  return keywords;
}

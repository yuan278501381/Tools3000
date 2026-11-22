import type { LucideIcon } from 'lucide-react';

export interface DriveInfo {
  letter: string;
  path: string;
  volumeLabel: string;
  fileSystem: string;
  type: 'fixed' | 'remote' | 'removable' | 'cdrom' | 'ramdisk' | 'unknown';
  totalBytes: number;
  freeBytes: number;
}

export type SearchMode = 'name' | 'both' | 'content';

export interface ContentSnippet {
  lineNumber: number;
  lineContent: string;
  matchOffset: number;
  matchLength: number;
}

export interface SearchResult {
  name: string;
  path: string;
  isDirectory: boolean;
  size?: number;
  creationTime?: number;
  lastWriteTime?: number;
  lastAccessTime?: number;
  attributes?: number;
  matchRank?: number;
  snippets?: ContentSnippet[];
  runCount?: number;
  frecencyScore?: number;
}

/** 单个视口惰性提取属性单元格的值：展示文本 `d` 与可选精确数值排序键 `n`。 */
export interface PropertyCellValue {
  d: string;
  n?: number;
}

/** `path -> { propId -> PropertyCellValue }` 映射表。 */
export type PropertyCacheMap = Record<string, Record<string, PropertyCellValue>>;

/**
 * 代际强锁步快照：将检索结果、对应查询词与高亮分词在同一帧原子提交，
 * 消除异步结果与打字高亮分词错位导致的频闪。
 */
export interface SearchSnapshot {
  generationId: number;
  query: string;
  keywords: string[];
  results: SearchResult[];
}

export interface SearchResponse {
  results: SearchResult[];
  available: boolean;
  status?: 'ready' | 'starting' | 'unavailable' | 'busy' | 'timeout';
  busy?: boolean;
  totalIndexedFiles?: number;
  elapsedMs?: number;
  error?: string;
  /** The server received a newer query while calculating this response. */
  cancelled?: boolean;
  queryId?: number;
  hasMore?: boolean;
  nextCursor?: number;
  planId?: number;
  totalCandidates?: number;
}

export type IndexScanPhase = 'idle' | 'scanning' | 'building' | 'syncing' | 'loading' | 'starting';

export interface VolumeIndexStats {
  drive: string;
  label: string;
  phase: IndexScanPhase;
  records: number;
  scanned: number;
  estimated: number;
  done: boolean;
}

export interface DbStatsSnapshot {
  dbPath: string;
  dbSize: number;
  timestamp: number;
  totalRecords: number;
  volumeCount: number;
  indexedVolumes?: number;
  totalVolumes?: number;
  exists: boolean;
  indexing?: boolean;
  phase?: IndexScanPhase;
  activeDrive?: string;
  activeVolumeLabel?: string;
  scannedRecords?: number;
  estimatedRecords?: number;
  progressPercent?: number;
  volumes?: VolumeIndexStats[];
  rebuildInProgress?: boolean;
  snapshotInProgress?: boolean;
  runHistoryCount: number;
  searchHistoryCount: number;
}

export interface SortProgressState {
  active: boolean;
  columnId: TableColumnId;
  columnLabel: string;
  completed: number;
  total: number;
}

export type SearchDensity = 'compact' | 'standard' | 'comfortable';
export type SearchIconStyle = 'native' | 'vector';
export type SortField = string;
export type SortDirection = 'asc' | 'desc';
export type ColumnId = 'name' | 'path' | 'size' | 'modified' | 'created' | 'snippets';
export type TableColumnId = string;

export interface ColumnSetting {
  id: ColumnId;
  label?: string;
  labelKey?: string;
  visible: boolean;
  flex?: number;
}

export interface WindowPreset {
  id: string;
  label: string;
  width: number;
  height: number;
}

export interface CategoryFilter {
  id: string;
  label: string;
  labelKey?: string;
  defaultLabel?: string;
  prefix: string;
  icon?: LucideIcon;
}

export interface SyntaxExampleItem {
  category: 'content' | 'path' | 'ext' | 'logic';
  syntax: string;
  descKey: string;
  defaultDesc: string;
  highlight?: boolean;
}

export interface FormatCategory {
  id: string;
  nameKey: 'search.catCode' | 'search.catDocs' | 'search.catConfig' | 'search.catDesign';
  iconName: 'code' | 'doc' | 'config' | 'design';
  extensions: string[];
}

/** 表格视图共享的列显示、顺序、宽度与正交全文摘要行配置。 */
export interface ColumnLayout {
  name: boolean;
  path: boolean;
  size: boolean;
  modified: boolean;
  created: boolean;
  snippets: boolean;
  nameFlex: number;
  pathFlex: number;
  tableOrder?: TableColumnId[];
  tableWidths?: Partial<Record<TableColumnId, number>>;
  /** 表格视图当前启用的列集合（单一事实源，缺省时回退到默认 5 列）。 */
  visibleTableColumns?: TableColumnId[];
}

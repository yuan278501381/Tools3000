import { useCallback, useEffect, useMemo, useRef, useState, startTransition, type MouseEvent as ReactMouseEvent } from 'react';
import {
  FileSpreadsheet,
} from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { AppToaster } from './components/AppToaster';
import { bridgeRequest } from './hooks/useBridge';
import { useAppearance } from './hooks/useAppearance';
import { WindowResizeHandles } from './components/WindowResizeHandles';
import {
  CUSTOM_EXCLUDED_PATHS_STORAGE_KEY,
  mergeExcludedPaths,
  normalizeExcludedPath,
} from './search/excludedPathsUtils';
import { VirtualSearchResults } from './search/VirtualSearchResults';
import { SearchTableHeader } from './search/SearchTableHeader';
import { SearchContextMenu } from './search/SearchContextMenu';
import { SearchRenameModal } from './search/SearchRenameModal';
import { SearchSyntaxModal } from './search/SearchSyntaxModal';
import { SearchViewSettingsModal } from './search/SearchViewSettingsModal';
import { SearchHistoryBar } from './search/SearchHistoryBar';
import { SearchEmptyState } from './search/SearchEmptyState';
import { SearchFooter } from './search/SearchFooter';
import { SearchHeader } from './search/SearchHeader';
import { SearchCategoryBar } from './search/SearchCategoryBar';
import { SearchColumnManagerModal } from './search/SearchColumnManagerModal';
import { SearchPropertyPickerModal } from './search/SearchPropertyPickerModal';
import { useSearchActions } from './search/useSearchActions';
import { useSearchKeyboard } from './search/useSearchKeyboard';
import { useSearchEngine } from './search/useSearchEngine';
import { useViewportProperties } from './search/useViewportProperties';
import {
  DEFAULT_VISIBLE_TABLE_COLUMNS,
  doesItemRequireBackendProperty,
  getColumnDisplayLabel,
  isKnownTableColumnId,
  resolveColumnDefinition,
  resolveTableCellText,
} from './search/columnCatalog';
import {
  CATEGORY_DEFS,
  DEFAULT_COLUMNS,
  DEFAULT_SEARCH_CATEGORY_ORDER,
  SEARCH_CATEGORY_ORDER_STORAGE_KEY,
  type SearchCategoryId,
} from './search/searchConstants';
import type {
  CategoryFilter,
  ColumnId,
  ColumnLayout,
  ColumnSetting,
  DbStatsSnapshot,
  DriveInfo,
  FormatCategory,
  PropertyCacheMap,
  SearchDensity,
  SearchIconStyle,
  SearchMode,
  SearchResult,
  SortDirection,
  SortField,
  TableColumnId,
} from './search/searchTypes';
import {
  buildSmartContentSearchQuery,
  collectColumnFitSampleRows,
  DEFAULT_TABLE_COLUMN_ORDER,
  estimateFittedColumnWidth,
  extractParentDirectoryPath,
  formatFileSize,
  formatWindowsTime,
  getSortedResults,
  getVisibleTableColumns,
  hasContentSyntaxInQuery,
  isEffectiveContentSearch,
  migrateTableColumnsToIncludeRunCount,
  normalizeSearchCategoryOrder,
  normalizeTableColumnOrder,
  normalizeVisibleTableColumns,
  parseExplicitSortDirective,
  resolveNativeIconRequest,
  sanitizeTableColumnWidths,
  stripContentSyntaxFromQuery,
  TABLE_COLUMN_SCHEMA_VERSION,
} from './search/searchUtils';
import {
  safeStorageGetBoolean,
  safeStorageGetEnum,
  safeStorageGetItem,
  safeStorageGetJsonArray,
  safeStorageGetJsonRecord,
  safeStorageGetNumber,
  safeStorageGetString,
  safeStorageRemoveItem,
  safeStorageSetItem,
} from './search/storageUtils';
import './SearchApp.css';

interface StoredSearchHistoryItem {
  search: string;
  searchCount: number;
  lastSearchDate: number;
}

const SEARCH_HISTORY_STORAGE_KEY = 'tools3000_search_history_cache';
const SEARCH_HISTORY_MAX_ITEMS = 25;
const SEARCH_HISTORY_AUTO_RECORD_DELAY_MS = 1350;
const REBUILD_POLL_INTERVAL_MS = 350;
const REBUILD_POLL_TIMEOUT_MS = 120_000;
const ALLOWED_MAX_RESULT_LIMITS = [0, 50, 100, 200] as const;
const VALID_COLUMN_IDS: readonly ColumnId[] = ['name', 'path', 'size', 'modified', 'created', 'snippets'];
const BARE_OPERATOR_PREFIX_REGEX = /^(content|\u5185\u5bb9|c|ext|path|parent|p|file|folder|dir|size|dm|date|modified|regex|r|case|cs|exact|pinyin|py|nopy|bm|b|app|cmd):$/i;
const CJK_CHAR_REGEX = /[\u4e00-\u9fff]/;

function isNonEmptyString(val: unknown): val is string {
  return typeof val === 'string' && val.trim().length > 0;
}

function isRecordableSearchQuery(raw: string): boolean {
  const trimmed = raw.trim();
  if (!trimmed) return false;
  if (BARE_OPERATOR_PREFIX_REGEX.test(trimmed)) return false;
  return trimmed.length >= 2 || CJK_CHAR_REGEX.test(trimmed);
}

function loadCachedSearchHistory(): StoredSearchHistoryItem[] {
  const rawList = safeStorageGetJsonArray<unknown>(
    SEARCH_HISTORY_STORAGE_KEY,
    (v): v is unknown => Boolean(v) && typeof v === 'object',
    [],
  );
  const out: StoredSearchHistoryItem[] = [];
  for (const item of rawList) {
    const rec = item as Partial<StoredSearchHistoryItem>;
    if (typeof rec.search !== 'string' || rec.search.trim().length === 0) continue;
    out.push({
      search: rec.search.trim(),
      searchCount: typeof rec.searchCount === 'number' && Number.isFinite(rec.searchCount) && rec.searchCount > 0 ? rec.searchCount : 1,
      lastSearchDate: typeof rec.lastSearchDate === 'number' && Number.isFinite(rec.lastSearchDate) ? rec.lastSearchDate : 0,
    });
    if (out.length >= SEARCH_HISTORY_MAX_ITEMS) break;
  }
  return out;
}

function saveCachedSearchHistory(items: StoredSearchHistoryItem[]): void {
  safeStorageSetItem(
    SEARCH_HISTORY_STORAGE_KEY,
    JSON.stringify(items.slice(0, SEARCH_HISTORY_MAX_ITEMS)),
  );
}

const CATEGORY_DEF_MAP = new Map<string, (typeof CATEGORY_DEFS)[number]>(
  CATEGORY_DEFS.map((def) => [def.id, def]),
);

function loadInitialCategoryOrder(): SearchCategoryId[] {
  const saved = safeStorageGetJsonArray<string>(
    SEARCH_CATEGORY_ORDER_STORAGE_KEY,
    (v): v is string => typeof v === 'string',
    [],
  );
  return normalizeSearchCategoryOrder(saved);
}

function loadInitialTableColumnState(): { visible: TableColumnId[]; order: TableColumnId[] } {
  const storedSchemaVer = safeStorageGetNumber('tools3000_search_col_schema_ver', 0, { integer: true });
  const parsedVisible = safeStorageGetJsonArray<string>(
    'tools3000_search_visible_table_cols',
    (v): v is string => typeof v === 'string',
    [],
  );
  const parsedOrder = safeStorageGetJsonArray<string>(
    'tools3000_search_table_order',
    (v): v is string => typeof v === 'string',
    [],
  );
  if (parsedVisible.length > 0) {
    if (storedSchemaVer < TABLE_COLUMN_SCHEMA_VERSION) {
      const migrated = migrateTableColumnsToIncludeRunCount(parsedVisible, parsedOrder);
      safeStorageSetItem('tools3000_search_visible_table_cols', JSON.stringify(migrated.visible));
      safeStorageSetItem('tools3000_search_table_order', JSON.stringify(migrated.order));
      safeStorageSetItem('tools3000_search_col_schema_ver', String(TABLE_COLUMN_SCHEMA_VERSION));
      return migrated;
    }
    return {
      visible: normalizeVisibleTableColumns(parsedVisible),
      order: parsedOrder.length > 0 ? normalizeTableColumnOrder(parsedOrder) : [...DEFAULT_TABLE_COLUMN_ORDER],
    };
  }
  safeStorageSetItem('tools3000_search_col_schema_ver', String(TABLE_COLUMN_SCHEMA_VERSION));
  return {
    visible: [...DEFAULT_VISIBLE_TABLE_COLUMNS],
    order: parsedOrder.length > 0 ? normalizeTableColumnOrder(parsedOrder) : [...DEFAULT_TABLE_COLUMN_ORDER],
  };
}

// ── 组件主入口 ──────────────────────────────────────────────────
export default function SearchApp() {
  useAppearance();
  const { t, i18n } = useTranslation();
  const [toasterTheme, setToasterTheme] = useState<'dark' | 'light'>(() =>
    document.documentElement.getAttribute('data-theme') === 'light' ? 'light' : 'dark',
  );
  useEffect(() => {
    const syncTheme = () => {
      setToasterTheme(document.documentElement.getAttribute('data-theme') === 'light' ? 'light' : 'dark');
    };
    syncTheme();
    if (typeof MutationObserver === 'undefined') return;
    const obs = new MutationObserver(syncTheme);
    obs.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => obs.disconnect();
  }, []);

  const currentLocale = i18n.resolvedLanguage || i18n.language || 'en';
  const compactCountFormatter = useMemo(() => {
    try {
      return new Intl.NumberFormat(currentLocale, {
        notation: 'compact',
        maximumFractionDigits: 1,
      });
    } catch {
      return new Intl.NumberFormat('en', {
        notation: 'compact',
        maximumFractionDigits: 1,
      });
    }
  }, [currentLocale]);

  const [query, setQuery] = useState('');
  const [actionError, setActionError] = useState('');
  const [showSyntaxHelp, setShowSyntaxHelp] = useState(false);
  const [syntaxCat, setSyntaxCat] = useState<string>('all');
  const [showViewSettings, setShowViewSettings] = useState(false);
  const [showHistoryMenu, setShowHistoryMenu] = useState(false);
  const [showColumnManager, setShowColumnManager] = useState(false);
  const [columnManagerDraft, setColumnManagerDraft] = useState<TableColumnId[] | null>(null);
  const [columnManagerResetPending, setColumnManagerResetPending] = useState(false);
  const [showPropertyPicker, setShowPropertyPicker] = useState(false);
  const [propertyPickerSource, setPropertyPickerSource] = useState<'header' | 'manager'>('header');
  const [visibleRange, setVisibleRange] = useState<{ start: number; end: number }>({ start: 0, end: 40 });
  const headerScrollRef = useRef<HTMLDivElement>(null);
  const isMountedRef = useRef(true);
  const firstPaintNotifiedRef = useRef(false);
  useEffect(() => {
    isMountedRef.current = true;
    return () => {
      isMountedRef.current = false;
    };
  }, []);

  // 双 rAF 确认首帧实际渲染完成后再通知原生宿主切换 WebView2 可见，彻底杜绝机械盘冷启纯色黑屏
  useEffect(() => {
    if (firstPaintNotifiedRef.current) return;
    let raf1 = 0;
    let raf2 = 0;
    let fallbackTimer: ReturnType<typeof setTimeout> | null = null;
    const notifyReady = () => {
      if (firstPaintNotifiedRef.current) return;
      firstPaintNotifiedRef.current = true;
      if (fallbackTimer !== null) {
        clearTimeout(fallbackTimer);
        fallbackTimer = null;
      }
      void bridgeRequest('search.firstPaintReady', undefined, { silent: true }).catch(() => undefined);
    };
    const scheduleVisiblePaintNotify = () => {
      if (firstPaintNotifiedRef.current || document.hidden) return;
      if (fallbackTimer === null) {
        fallbackTimer = setTimeout(notifyReady, 120);
      }
      if (!raf1) {
        raf1 = requestAnimationFrame(() => {
          raf2 = requestAnimationFrame(notifyReady);
        });
      }
    };
    const onVisibility = () => {
      if (!document.hidden) {
        scheduleVisiblePaintNotify();
      }
    };
    scheduleVisiblePaintNotify();
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      document.removeEventListener('visibilitychange', onVisibility);
      if (raf1) cancelAnimationFrame(raf1);
      if (raf2) cancelAnimationFrame(raf2);
      if (fallbackTimer !== null) clearTimeout(fallbackTimer);
    };
  }, []);

  const [contextMenu, setContextMenu] = useState<{
    visible: boolean;
    x: number;
    y: number;
    result?: SearchResult;
  }>({ visible: false, x: 0, y: 0 });
  const [renameTarget, setRenameTarget] = useState<{
    visible: boolean;
    result?: SearchResult;
    newName: string;
  }>({ visible: false, newName: '' });
  const renameInputRef = useRef<HTMLInputElement | null>(null);
  const [isPinned, setIsPinned] = useState(false);
  const [isMaximized, setIsMaximized] = useState(false);
  const [hideOnOpen, setHideOnOpen] = useState<boolean>(() =>
    safeStorageGetBoolean('tools3000_search_hide_on_open', false),
  );
  const [clearQueryOnShow, setClearQueryOnShow] = useState<boolean>(() =>
    safeStorageGetBoolean('tools3000_search_clear_query_on_show', false),
  );
  const clearQueryOnShowRef = useRef(clearQueryOnShow);
  const wasHiddenSinceLastShowRef = useRef(false);
  const clearQueryForFreshShowRef = useRef<() => void>(() => {});
  useEffect(() => {
    clearQueryOnShowRef.current = clearQueryOnShow;
  }, [clearQueryOnShow]);

  const inputRef = useRef<HTMLInputElement>(null);
  const [isComposing, setIsComposing] = useState(false);

  const [maxResultLimit, setMaxResultLimit] = useState<number>(() =>
    safeStorageGetNumber('tools3000_search_max_results', 0, {
      integer: true,
      allowedValues: ALLOWED_MAX_RESULT_LIMITS,
    }),
  );

  const [defaultCategory, setDefaultCategory] = useState<string>(() => {
    const saved = safeStorageGetString('tools3000_search_default_category', 'all');
    return saved === 'content' ? 'all' : saved;
  });

  const [excludeGitAndModules, setExcludeGitAndModules] = useState<boolean>(() =>
    safeStorageGetBoolean('tools3000_search_exclude_dev', true),
  );
  const [excludeHidden, setExcludeHidden] = useState<boolean>(() =>
    safeStorageGetBoolean('tools3000_search_exclude_hidden', false),
  );
  const [customExcludedPaths, setCustomExcludedPaths] = useState<string[]>(() =>
    mergeExcludedPaths(
      [],
      safeStorageGetJsonArray<string>(CUSTOM_EXCLUDED_PATHS_STORAGE_KEY, isNonEmptyString, []),
    ).paths,
  );
  const [isRebuilding, setIsRebuilding] = useState(false);
  const rebuildingRef = useRef(false);

  const [searchMode, setSearchMode] = useState<SearchMode>(() =>
    safeStorageGetEnum('tools3000_search_default_mode', ['name', 'both', 'content'] as const, 'name'),
  );

  const persistUiPreferencesPatch = useCallback((patch: Record<string, unknown>) => {
    void bridgeRequest('search.saveSettings', { uiPreferences: patch }).catch(() => undefined);
  }, []);

  const changeMaxResultLimit = (limit: number) => {
    setMaxResultLimit(limit);
    safeStorageSetItem('tools3000_search_max_results', String(limit));
    void bridgeRequest('search.saveSettings', { maxResults: limit }).catch(() => undefined);
    persistUiPreferencesPatch({ maxResultLimit: limit });
  };

  const changeSearchMode = (mode: SearchMode) => {
    setSearchMode(mode);
    safeStorageSetItem('tools3000_search_default_mode', mode);
    persistUiPreferencesPatch({ searchMode: mode });
  };

  const [disabledContentFormats, setDisabledContentFormats] = useState<string[]>(() => {
    const primary = safeStorageGetItem('tools3000_search_disabled_formats');
    const storageKey = primary !== null
      ? 'tools3000_search_disabled_formats'
      : 'tools3000_search_disabled_content_formats';
    return safeStorageGetJsonArray<string>(storageKey, isNonEmptyString, []);
  });

  const [customContentFormats, setCustomContentFormats] = useState<string[]>(() =>
    safeStorageGetJsonArray<string>('tools3000_search_custom_formats', isNonEmptyString, []),
  );

  const [newFormatInput, setNewFormatInput] = useState('');

  const [systemDrives, setSystemDrives] = useState<DriveInfo[]>([]);
  const [enabledDrives, setEnabledDrives] = useState<string[]>(() =>
    safeStorageGetJsonArray<string>('tools3000_search_enabled_drives', isNonEmptyString, []),
  );

  const activeCategory = useMemo(() => {
    if (hasContentSyntaxInQuery(query)) {
      return 'content';
    }
    const trimmed = query.trimStart().toLowerCase();
    if (!trimmed) {
      if (searchMode === 'content') {
        return 'content';
      }
      return defaultCategory && defaultCategory !== 'content' ? defaultCategory : 'all';
    }
    if (trimmed.startsWith('bm:') || (trimmed.startsWith('b:') && trimmed[2] !== '\\' && trimmed[2] !== '/')) {
      return 'bookmark';
    }
    if (trimmed.startsWith('app:') || trimmed.startsWith('cmd:')) {
      return 'app';
    }
    if (trimmed.startsWith('folder:') || trimmed.startsWith('dir:')) {
      return 'folder';
    }
    if (trimmed.startsWith('ext:doc') || trimmed.startsWith('ext:pdf') || trimmed.startsWith('ext:txt')) {
      return 'doc';
    }
    if (trimmed.startsWith('ext:jpg') || trimmed.startsWith('ext:png')) {
      return 'image';
    }
    if (trimmed.startsWith('ext:mp4') || trimmed.startsWith('ext:mkv')) {
      return 'video';
    }
    if (trimmed.startsWith('ext:mp3') || trimmed.startsWith('ext:wav')) {
      return 'audio';
    }
    if (trimmed.startsWith('ext:zip') || trimmed.startsWith('ext:rar')) {
      return 'archive';
    }
    if (trimmed.startsWith('ext:cpp') || trimmed.startsWith('ext:ts') || trimmed.startsWith('ext:js')) {
      return 'code';
    }
    return 'all';
  }, [defaultCategory, query, searchMode]);

  const {
    snapshot,
    results,
    setResults,
    selectedIndex,
    setSelectedIndex,
    loading,
    setLoading,
    serviceAvailable,
    setServiceAvailable,
    isServiceStarting,
    setIsServiceStarting,
    totalIndexedFiles,
    setTotalIndexedFiles,
    searchElapsedMs,
    scanElapsedSeconds,
    lastColdScanDurationSec,
    scanProgress,
    contentScanPaused,
    stopSearch,
    resumeContentScan,
    clearResultCache,
    refreshQuery,
    silentRefreshQuery,
    refreshOnFocus,
  } = useSearchEngine({
    query,
    isComposing,
    activeCategory,
    searchMode,
    enabledDrives,
    maxResultLimit,
    excludeGitAndModules,
    excludeHidden,
    customExcludedPaths,
    customContentFormats,
    disabledContentFormats,
  });

  const updateCustomExcludedPaths = useCallback(
    (nextPaths: string[]) => {
      const clean = mergeExcludedPaths([], nextPaths).paths;
      setCustomExcludedPaths(clean);
      safeStorageSetItem(CUSTOM_EXCLUDED_PATHS_STORAGE_KEY, JSON.stringify(clean));
      void bridgeRequest('search.saveSettings', { customExcludedPaths: clean }).catch(() => undefined);
      clearResultCache();
    },
    [clearResultCache],
  );

  const handleExcludeFolderFromResult = useCallback(
    (res: SearchResult) => {
      const rawDir = res.isDirectory ? res.path : extractParentDirectoryPath(res.path);
      const folderPath = normalizeExcludedPath(rawDir);
      if (!folderPath) return;

      const prevPaths = customExcludedPaths;
      const merged = mergeExcludedPaths(prevPaths, [folderPath]);
      if (merged.added.length === 0) {
        if (merged.capped) {
          toast.warning(
            t('search.customExcludedLimitReached', 'Maximum of {{max}} excluded folders reached', {
              max: 48,
            }),
          );
        } else {
          toast.info(t('search.customExcludedDuplicate', 'This folder is already in the exclusion list'));
        }
        return;
      }

      updateCustomExcludedPaths(merged.paths);
      const lowerPrefix = folderPath.toLowerCase();
      const isDriveRoot = /^[a-z]:\\$/.test(lowerPrefix);
      startTransition(() => {
        setResults((current) =>
          current.filter((item) => {
            const normItem = (item.path || '').replace(/\//g, '\\').toLowerCase();
            if (isDriveRoot) return !normItem.startsWith(lowerPrefix);
            if (normItem === lowerPrefix) return false;
            return !normItem.startsWith(`${lowerPrefix}\\`);
          }),
        );
      });

      toast.success(
        t('search.excludedFolderToast', 'Excluded folder: {{path}}', { path: folderPath }),
        {
          action: {
            label: t('search.undoAction', 'Undo'),
            onClick: () => {
              updateCustomExcludedPaths(prevPaths);
              refreshQuery();
            },
          },
        },
      );
    },
    [customExcludedPaths, refreshQuery, setResults, t, updateCustomExcludedPaths],
  );

  useEffect(() => {
    bridgeRequest<{ pinned: boolean }>('search.isPinned')
      .then(res => {
        if (res && typeof res.pinned === 'boolean') {
          setIsPinned(res.pinned);
        }
      })
      .catch(() => {});
    bridgeRequest<{ maximized?: boolean }>('search.getMaximized')
      .then(res => {
        if (res && typeof res.maximized === 'boolean') {
          setIsMaximized(res.maximized);
        }
      })
      .catch(() => {});

    const onMaximizedChanged = (event: Event) => {
      const customEvt = event as CustomEvent<boolean>;
      if (typeof customEvt.detail === 'boolean') {
        setIsMaximized(customEvt.detail);
      }
    };
    window.addEventListener('tools3000:searchMaximizedChanged', onMaximizedChanged);
    return () => {
      window.removeEventListener('tools3000:searchMaximizedChanged', onMaximizedChanged);
    };
  }, []);

  const togglePin = useCallback(async () => {
    const next = !isPinned;
    setIsPinned(next);
    try {
      await bridgeRequest('search.setPinned', { pinned: next });
      if (next) {
        toast.success(t('search.pinnedToast', 'Search window pinned on top'));
      } else {
        toast.info(t('search.unpinnedToast', 'Search window unpinned from top'));
      }
    } catch {
      setIsPinned(!next);
    }
  }, [isPinned, t]);

  const toggleMaximized = useCallback(async () => {
    const next = !isMaximized;
    setIsMaximized(next);
    try {
      const res = await bridgeRequest<{ maximized?: boolean }>('search.toggleMaximized');
      if (res && typeof res.maximized === 'boolean') {
        setIsMaximized(res.maximized);
      }
      inputRef.current?.focus();
    } catch {
      setIsMaximized(!next);
    }
  }, [isMaximized]);

  const [categoryOrder, setCategoryOrder] = useState<SearchCategoryId[]>(loadInitialCategoryOrder);

  const handleReorderCategories = useCallback(
    (nextOrder: SearchCategoryId[]) => {
      const clean = normalizeSearchCategoryOrder(nextOrder);
      setCategoryOrder(clean);
      safeStorageSetItem(SEARCH_CATEGORY_ORDER_STORAGE_KEY, JSON.stringify(clean));
      persistUiPreferencesPatch({ categoryOrder: clean });
    },
    [persistUiPreferencesPatch],
  );

  const handleResetCategoryOrder = useCallback(() => {
    const clean = [...DEFAULT_SEARCH_CATEGORY_ORDER];
    setCategoryOrder(clean);
    safeStorageRemoveItem(SEARCH_CATEGORY_ORDER_STORAGE_KEY);
    persistUiPreferencesPatch({ categoryOrder: clean });
    toast.success(t('search.toastCategoryOrderReset', 'Category order reset to default'));
  }, [persistUiPreferencesPatch, t]);

  const categories: CategoryFilter[] = useMemo(() => {
    return categoryOrder
      .map((id) => CATEGORY_DEF_MAP.get(id))
      .filter((def): def is (typeof CATEGORY_DEFS)[number] => Boolean(def))
      .map((def) => ({
        id: def.id,
        label: t(def.labelKey as unknown as 'search.catAll', def.defaultLabel),
        labelKey: def.labelKey,
        defaultLabel: def.defaultLabel,
        prefix: def.prefix,
        icon: def.icon,
      }));
  }, [categoryOrder, t]);

  const [searchHistory, setSearchHistory] = useState<StoredSearchHistoryItem[]>(() => loadCachedSearchHistory());
  const lastRecordedQueryRef = useRef<string>('');
  const latestQueryRef = useRef<string>(query);
  const isComposingRef = useRef<boolean>(isComposing);
  useEffect(() => {
    latestQueryRef.current = query;
    isComposingRef.current = isComposing;
  }, [isComposing, query]);
  const [isInitialIndexing, setIsInitialIndexing] = useState(false);
  const isInitialIndexingRef = useRef(false);
  const [dbStats, setDbStats] = useState<DbStatsSnapshot | null>(null);

  const refreshHistory = useCallback(async () => {
    try {
      const res = await bridgeRequest<{ success: boolean; history: StoredSearchHistoryItem[] }>('search.getSearchHistory', { limit: SEARCH_HISTORY_MAX_ITEMS });
      if (res && res.success && Array.isArray(res.history)) {
        const sanitized = res.history
          .filter((item) => typeof item?.search === 'string' && item.search.trim().length > 0)
          .slice(0, SEARCH_HISTORY_MAX_ITEMS);
        setSearchHistory(sanitized);
        saveCachedSearchHistory(sanitized);
      }
    } catch {
      // 忽略瞬时通信错误
    }
  }, []);

  const recordSearchHistory = useCallback((rawText: string, options?: { force?: boolean }) => {
    const trimmed = rawText.trim();
    if (!isRecordableSearchQuery(trimmed)) return;
    if (!options?.force && lastRecordedQueryRef.current.toLowerCase() === trimmed.toLowerCase()) {
      return;
    }
    lastRecordedQueryRef.current = trimmed;
    const now = Date.now();
    setSearchHistory((prev) => {
      const existing = prev.find((item) => item.search.toLowerCase() === trimmed.toLowerCase());
      const nextItem: StoredSearchHistoryItem = {
        search: trimmed,
        searchCount: (existing?.searchCount ?? 0) + 1,
        lastSearchDate: now,
      };
      const next = [
        nextItem,
        ...prev.filter((item) => item.search.toLowerCase() !== trimmed.toLowerCase()),
      ].slice(0, SEARCH_HISTORY_MAX_ITEMS);
      saveCachedSearchHistory(next);
      return next;
    });
    void bridgeRequest('search.recordSearch', { search: trimmed }).catch(() => undefined);
  }, []);

  // 搜索词在非组字态稳定停留 1.35s 后自动沉淀至历史搜索记录
  useEffect(() => {
    if (isComposing || loading) return;
    const trimmed = query.trim();
    if (!isRecordableSearchQuery(trimmed)) return;
    const timer = window.setTimeout(() => {
      recordSearchHistory(trimmed);
    }, SEARCH_HISTORY_AUTO_RECORD_DELAY_MS);
    return () => window.clearTimeout(timer);
  }, [isComposing, loading, query, recordSearchHistory]);

  const checkInitialIndex = useCallback(async (): Promise<boolean> => {
    try {
      const res = await bridgeRequest<DbStatsSnapshot & { success: boolean }>('search.getDbStats');
      if (res && res.success) {
        setDbStats(res);
        // 只要后端回报了有效索引记录数即立即刷新，绝不被旧值锁死
        if (typeof res.totalRecords === 'number' && res.totalRecords > 0) {
          setTotalIndexedFiles(res.totalRecords);
        }
        const isBusy =
          res.indexing === true ||
          res.rebuildInProgress === true ||
          (res.phase !== undefined && res.phase !== 'idle') ||
          (res.totalRecords === 0 && res.exists === false);

        // 严格区分「首次空库索引」与「已有数据的后台重建」：后台重建期间绝不白屏遮罩搜索列表
        const isEmptyInitial = res.totalRecords === 0 && (!res.exists || isBusy);

        if (isBusy) {
          setIsInitialIndexing(isEmptyInitial);
          isInitialIndexingRef.current = isEmptyInitial;
          return true;
        }

        if (isInitialIndexingRef.current) {
          isInitialIndexingRef.current = false;
          setIsInitialIndexing(false);
          setServiceAvailable(true);
          refreshQuery();
          toast.success(t('search.indexReadyToast', 'Full disk index built and ready!'));
        } else {
          setIsInitialIndexing(false);
          setServiceAvailable((wasAvailable) => {
            if (!wasAvailable) {
              window.setTimeout(() => refreshQuery(), 0);
            }
            return true;
          });
        }
        return false;
      }
    } catch {
      // 忽略瞬时轮询错误
    }
    return false;
  }, [refreshQuery, setServiceAvailable, setTotalIndexedFiles, t]);

  useEffect(() => {
    let active = true;
    let pollTimer: number | undefined;

    void bridgeRequest<{ success: boolean; history: StoredSearchHistoryItem[] }>('search.getSearchHistory', { limit: SEARCH_HISTORY_MAX_ITEMS })
      .then((res) => {
        if (active && res && res.success && Array.isArray(res.history)) {
          const sanitized = res.history
            .filter((item) => typeof item?.search === 'string' && item.search.trim().length > 0)
            .slice(0, SEARCH_HISTORY_MAX_ITEMS);
          setSearchHistory(sanitized);
          saveCachedSearchHistory(sanitized);
        }
      })
      .catch(() => undefined);

    const runPoll = async () => {
      if (document.hidden) return;
      const isBusy = await checkInitialIndex();
      if (!active || document.hidden) return;
      if (isBusy) {
        pollTimer = window.setTimeout(runPoll, REBUILD_POLL_INTERVAL_MS);
      }
    };

    const handlePollVisibility = () => {
      if (document.hidden) {
        if (pollTimer) {
          window.clearTimeout(pollTimer);
          pollTimer = undefined;
        }
      } else if (active) {
        if (pollTimer) window.clearTimeout(pollTimer);
        pollTimer = window.setTimeout(runPoll, 120);
      }
    };

    let lastFocusSyncTick = 0;
    const onFocusEvt = () => {
      const now = Date.now();
      if (now - lastFocusSyncTick > 3000) {
        lastFocusSyncTick = now;
        // Native SearchWindow::show() 为唯一按需拉起索引服务信号，focus 事件绝不自行拉起服务
        void bridgeRequest('search.sync').catch(() => {});
      }
      if (pollTimer) window.clearTimeout(pollTimer);
      pollTimer = window.setTimeout(runPoll, 250);
      refreshOnFocus();
    };
    window.addEventListener('tools3000:focusSearch', onFocusEvt);
    document.addEventListener('visibilitychange', handlePollVisibility);

    return () => {
      active = false;
      if (pollTimer) window.clearTimeout(pollTimer);
      window.removeEventListener('tools3000:focusSearch', onFocusEvt);
      document.removeEventListener('visibilitychange', handlePollVisibility);
    };
  }, [checkInitialIndex, refreshOnFocus]);

  const removeHistoryItem = async (e: React.MouseEvent, s: string) => {
    e.stopPropagation();
    await bridgeRequest('search.removeSearchHistory', { search: s }).catch(() => undefined);
    setSearchHistory((prev) => {
      const next = prev.filter((item) => item.search !== s);
      saveCachedSearchHistory(next);
      return next;
    });
    if (lastRecordedQueryRef.current.toLowerCase() === s.trim().toLowerCase()) {
      lastRecordedQueryRef.current = '';
    }
  };

  const clearAllHistory = async () => {
    await bridgeRequest('search.clearSearchHistory').catch(() => undefined);
    lastRecordedQueryRef.current = '';
    setSearchHistory([]);
    saveCachedSearchHistory([]);
    toast.success(t('search.historyCleared', 'Search history cleared'));
    void checkInitialIndex();
    void refreshHistory();
  };

  const [sortField, setSortField] = useState<SortField>(() => {
    const saved = safeStorageGetItem('tools3000_search_sort_field');
    if (saved === 'relevance') {
      return 'relevance';
    }
    if (saved && isKnownTableColumnId(saved)) {
      return saved;
    }
    return 'relevance';
  });

  const [sortDirection, setSortDirection] = useState<SortDirection>(() =>
    safeStorageGetEnum('tools3000_search_sort_dir', ['asc', 'desc'] as const, 'desc'),
  );

  const viewSettingsRef = useRef<HTMLDivElement>(null);

  const [windowSize, setWindowSize] = useState<{ width: number; height: number }>({ width: 760, height: 520 });
  const [initialTableColumnState] = useState(loadInitialTableColumnState);
  const [tableColumnOrder, setTableColumnOrder] = useState<TableColumnId[]>(initialTableColumnState.order);
  const [tableColumnWidths, setTableColumnWidths] = useState<Partial<Record<TableColumnId, number>>>(() => {
    const raw = safeStorageGetJsonRecord('tools3000_search_table_widths');
    return raw ? sanitizeTableColumnWidths(raw) : {};
  });
  const [visibleTableColumns, setVisibleTableColumns] = useState<TableColumnId[]>(initialTableColumnState.visible);
  const [recentTableColumns, setRecentTableColumns] = useState<TableColumnId[]>(() => {
    return safeStorageGetJsonArray<string>(
      'tools3000_search_recent_table_cols',
      (v): v is string => typeof v === 'string' && isKnownTableColumnId(v),
      [],
    ).slice(0, 12);
  });
  const [density, setDensity] = useState<SearchDensity>(() =>
    safeStorageGetEnum('tools3000_search_density', ['compact', 'standard', 'comfortable'] as const, 'standard'),
  );

  const [iconStyle, setIconStyle] = useState<SearchIconStyle>(() =>
    safeStorageGetEnum('tools3000_search_icon_style', ['native', 'vector'] as const, 'native'),
  );
  const [liveRefreshEnabled, setLiveRefreshEnabled] = useState<boolean>(() => {
    const raw = safeStorageGetItem('tools3000_search_live_refresh');
    return raw === null ? true : raw !== 'false';
  });
  const lastSeenIndexGenRef = useRef<number | null>(null);
  const liveWatchWindowVisibleRef = useRef<boolean>(!document.hidden);
  const NATIVE_ICON_CACHE_MAX_ENTRIES = 512;
  const [nativeIconCache, setNativeIconCache] = useState<Record<string, string>>({});
  const nativeIconCacheRef = useRef<Map<string, string>>(new Map());
  const inFlightIconsRef = useRef<Set<string>>(new Set());
  const searchSettingsSigRef = useRef<string | null>(null);
  const lastRemoteSyncTickRef = useRef(0);

  const touchAndEvictNativeIcons = useCallback((entries: Array<[string, string]>): Record<string, string> | null => {
    const map = nativeIconCacheRef.current;
    let changed = false;
    for (const [k, v] of entries) {
      if (map.has(k)) {
        const prevVal = map.get(k);
        map.delete(k);
        map.set(k, v);
        if (prevVal !== v) changed = true;
      } else {
        map.set(k, v);
        changed = true;
      }
    }
    while (map.size > NATIVE_ICON_CACHE_MAX_ENTRIES) {
      const oldestKey = map.keys().next().value;
      if (oldestKey === undefined) break;
      map.delete(oldestKey);
      changed = true;
    }
    if (!changed) return null;
    const nextRecord: Record<string, string> = {};
    map.forEach((v, k) => {
      nextRecord[k] = v;
    });
    return nextRecord;
  }, []);

  useEffect(() => {
    let active = true;
    const syncRemoteSettings = (triggerRefreshOnDiff: boolean) => {
      const now = Date.now();
      if (triggerRefreshOnDiff && now - lastRemoteSyncTickRef.current < 500) {
        return;
      }
      lastRemoteSyncTickRef.current = now;

      bridgeRequest<{
        iconStyle?: SearchIconStyle;
        pinyinEnabled?: boolean;
        matchPath?: boolean;
        caseSensitive?: boolean;
        includeBookmarks?: boolean;
        enableAppsAndCommands?: boolean;
        liveRefreshEnabled?: boolean;
        hideOnOpen?: boolean;
        clearQueryOnShow?: boolean;
        rankingProfile?: string;
        defaultCategory?: string;
        maxResults?: number;
        excludeHidden?: boolean;
        customExcludedPaths?: string[];
        enabledDrives?: string;
        uiPreferences?: Record<string, unknown>;
      }>('search.getSettings')
        .then((res) => {
          if (!active || !res) return;
          if (res.iconStyle === 'native' || res.iconStyle === 'vector') {
            setIconStyle(res.iconStyle);
            safeStorageSetItem('tools3000_search_icon_style', res.iconStyle);
          }
          if (typeof res.liveRefreshEnabled === 'boolean') {
            setLiveRefreshEnabled(res.liveRefreshEnabled);
            safeStorageSetItem('tools3000_search_live_refresh', String(res.liveRefreshEnabled));
          }
          if (typeof res.hideOnOpen === 'boolean') {
            setHideOnOpen(res.hideOnOpen);
            safeStorageSetItem('tools3000_search_hide_on_open', String(res.hideOnOpen));
          }
          if (typeof res.clearQueryOnShow === 'boolean') {
            setClearQueryOnShow(res.clearQueryOnShow);
            safeStorageSetItem('tools3000_search_clear_query_on_show', String(res.clearQueryOnShow));
          }
          if (typeof res.defaultCategory === 'string' && res.defaultCategory) {
            const normalizedCat = res.defaultCategory === 'content' ? 'all' : res.defaultCategory;
            setDefaultCategory(normalizedCat);
            safeStorageSetItem('tools3000_search_default_category', normalizedCat);
          }
          if (typeof res.excludeHidden === 'boolean') {
            setExcludeHidden(res.excludeHidden);
            safeStorageSetItem('tools3000_search_exclude_hidden', String(res.excludeHidden));
          }
          const remoteExcludedRaw = Array.isArray(res.customExcludedPaths)
            ? res.customExcludedPaths.filter(isNonEmptyString)
            : (Array.isArray(res.uiPreferences?.customExcludedPaths)
                ? (res.uiPreferences.customExcludedPaths as unknown[]).filter(isNonEmptyString)
                : null);
          const remoteExcludedClean = remoteExcludedRaw !== null
            ? mergeExcludedPaths([], remoteExcludedRaw).paths
            : null;
          if (remoteExcludedClean !== null) {
            setCustomExcludedPaths(remoteExcludedClean);
            safeStorageSetItem(CUSTOM_EXCLUDED_PATHS_STORAGE_KEY, JSON.stringify(remoteExcludedClean));
          }
          if (typeof res.enabledDrives === 'string' && res.enabledDrives.trim()) {
            const parsedDrives = res.enabledDrives.split(',').map(s => s.trim()).filter(Boolean);
            if (parsedDrives.length > 0) {
              setEnabledDrives(parsedDrives);
              safeStorageSetItem('tools3000_search_enabled_drives', JSON.stringify(parsedDrives));
            }
          }
          if (res.uiPreferences && typeof res.uiPreferences === 'object') {
            const prefs = res.uiPreferences;
            if (prefs.searchMode === 'name' || prefs.searchMode === 'both' || prefs.searchMode === 'content') {
              setSearchMode(prefs.searchMode);
              safeStorageSetItem('tools3000_search_default_mode', prefs.searchMode);
            }
            if (
              typeof prefs.maxResultLimit === 'number' &&
              Number.isFinite(prefs.maxResultLimit) &&
              (ALLOWED_MAX_RESULT_LIMITS as readonly number[]).includes(prefs.maxResultLimit)
            ) {
              setMaxResultLimit(prefs.maxResultLimit);
              safeStorageSetItem('tools3000_search_max_results', String(prefs.maxResultLimit));
            }
            if (typeof prefs.excludeGitAndModules === 'boolean') {
              setExcludeGitAndModules(prefs.excludeGitAndModules);
              safeStorageSetItem('tools3000_search_exclude_dev', String(prefs.excludeGitAndModules));
            }
            if (Array.isArray(prefs.disabledContentFormats)) {
              const clean = prefs.disabledContentFormats.filter(isNonEmptyString);
              setDisabledContentFormats(clean);
              safeStorageSetItem('tools3000_search_disabled_formats', JSON.stringify(clean));
            }
            if (Array.isArray(prefs.customContentFormats)) {
              const clean = prefs.customContentFormats.filter(isNonEmptyString);
              setCustomContentFormats(clean);
              safeStorageSetItem('tools3000_search_custom_formats', JSON.stringify(clean));
            }
            if (prefs.density === 'compact' || prefs.density === 'standard' || prefs.density === 'comfortable') {
              setDensity(prefs.density);
              safeStorageSetItem('tools3000_search_density', prefs.density);
            }
            const remoteSchemaVer = typeof prefs.columnSchemaVersion === 'number' ? prefs.columnSchemaVersion : 0;
            const rawRemoteOrder = Array.isArray(prefs.tableColumnOrder)
              ? prefs.tableColumnOrder.filter((x): x is string => typeof x === 'string')
              : null;
            const rawRemoteVisible = Array.isArray(prefs.visibleTableColumns) && prefs.visibleTableColumns.length > 0
              ? prefs.visibleTableColumns.filter((x): x is string => typeof x === 'string')
              : null;

            if (rawRemoteVisible && remoteSchemaVer < TABLE_COLUMN_SCHEMA_VERSION) {
              const migrated = migrateTableColumnsToIncludeRunCount(rawRemoteVisible, rawRemoteOrder);
              setTableColumnOrder(migrated.order);
              setVisibleTableColumns(migrated.visible);
              safeStorageSetItem('tools3000_search_table_order', JSON.stringify(migrated.order));
              safeStorageSetItem('tools3000_search_visible_table_cols', JSON.stringify(migrated.visible));
              safeStorageSetItem('tools3000_search_col_schema_ver', String(TABLE_COLUMN_SCHEMA_VERSION));
              persistUiPreferencesPatch({
                columnSchemaVersion: TABLE_COLUMN_SCHEMA_VERSION,
                tableColumnOrder: migrated.order,
                visibleTableColumns: migrated.visible,
              });
            } else {
              if (rawRemoteOrder) {
                const cleanOrder = normalizeTableColumnOrder(rawRemoteOrder);
                setTableColumnOrder(cleanOrder);
                safeStorageSetItem('tools3000_search_table_order', JSON.stringify(cleanOrder));
              }
              if (rawRemoteVisible) {
                const cleanVisible = normalizeVisibleTableColumns(rawRemoteVisible);
                setVisibleTableColumns(cleanVisible);
                safeStorageSetItem('tools3000_search_visible_table_cols', JSON.stringify(cleanVisible));
              }
              if (remoteSchemaVer >= TABLE_COLUMN_SCHEMA_VERSION) {
                safeStorageSetItem('tools3000_search_col_schema_ver', String(remoteSchemaVer));
              }
            }
            if (prefs.tableColumnWidths && typeof prefs.tableColumnWidths === 'object' && !Array.isArray(prefs.tableColumnWidths)) {
              const cleanWidths = sanitizeTableColumnWidths(prefs.tableColumnWidths as Record<string, unknown>);
              setTableColumnWidths(cleanWidths);
              safeStorageSetItem('tools3000_search_table_widths', JSON.stringify(cleanWidths));
            }
            if (Array.isArray(prefs.recentTableColumns)) {
              const cleanRecent = (prefs.recentTableColumns as unknown[])
                .filter((x): x is string => typeof x === 'string' && isKnownTableColumnId(x))
                .slice(0, 12);
              setRecentTableColumns(cleanRecent);
              safeStorageSetItem('tools3000_search_recent_table_cols', JSON.stringify(cleanRecent));
            }
            if (Array.isArray(prefs.categoryOrder)) {
              const rawCatOrder = prefs.categoryOrder.filter((x): x is string => typeof x === 'string');
              if (rawCatOrder.length > 0) {
                const cleanCatOrder = normalizeSearchCategoryOrder(rawCatOrder);
                setCategoryOrder(cleanCatOrder);
                safeStorageSetItem(SEARCH_CATEGORY_ORDER_STORAGE_KEY, JSON.stringify(cleanCatOrder));
              }
            }
          }
          const defaultPyByLang = (i18n.resolvedLanguage ?? i18n.language ?? '').toLowerCase().startsWith('zh');
          const excludedSig = remoteExcludedClean ? remoteExcludedClean.map((p) => p.toLowerCase()).join('|') : '';
          const nextSig = `${Boolean(res.pinyinEnabled ?? defaultPyByLang)}:${Boolean(res.matchPath ?? false)}:${Boolean(res.caseSensitive ?? false)}:${Boolean(res.includeBookmarks ?? true)}:${Boolean(res.enableAppsAndCommands ?? true)}:${res.rankingProfile ?? 'balanced_frecency'}:${excludedSig}`;
          const prevSig = searchSettingsSigRef.current;
          searchSettingsSigRef.current = nextSig;
          if (triggerRefreshOnDiff && prevSig !== null && prevSig !== nextSig) {
            refreshQuery();
          }
        })
        .catch(() => undefined);
    };
    syncRemoteSettings(false);
    const onSyncEvt = () => {
      syncRemoteSettings(true);
    };
    const onVisibilitySync = () => {
      if (!document.hidden) {
        syncRemoteSettings(true);
      }
    };
    const onStorageSync = (e: StorageEvent) => {
      if (e.key === 'tools3000_search_default_mode' && (e.newValue === 'name' || e.newValue === 'both' || e.newValue === 'content')) {
        setSearchMode(e.newValue);
      } else if (e.key === 'tools3000_search_default_category' && e.newValue) {
        setDefaultCategory(e.newValue === 'content' ? 'all' : e.newValue);
      } else if (e.key === 'tools3000_search_icon_style' && (e.newValue === 'native' || e.newValue === 'vector')) {
        setIconStyle(e.newValue);
      } else if (e.key === 'tools3000_search_live_refresh' && (e.newValue === 'true' || e.newValue === 'false')) {
        setLiveRefreshEnabled(e.newValue === 'true');
      } else if (e.key === 'tools3000_search_hide_on_open' && (e.newValue === 'true' || e.newValue === 'false')) {
        setHideOnOpen(e.newValue === 'true');
      } else if (e.key === 'tools3000_search_clear_query_on_show' && (e.newValue === 'true' || e.newValue === 'false')) {
        setClearQueryOnShow(e.newValue === 'true');
      } else if (e.key === CUSTOM_EXCLUDED_PATHS_STORAGE_KEY) {
        const synced = safeStorageGetJsonArray<string>(CUSTOM_EXCLUDED_PATHS_STORAGE_KEY, isNonEmptyString, []);
        setCustomExcludedPaths(mergeExcludedPaths([], synced).paths);
        clearResultCache();
      }
    };
    window.addEventListener('tools3000:focusSearch', onSyncEvt);
    window.addEventListener('focus', onSyncEvt);
    window.addEventListener('storage', onStorageSync);
    document.addEventListener('visibilitychange', onVisibilitySync);
    return () => {
      active = false;
      window.removeEventListener('tools3000:focusSearch', onSyncEvt);
      window.removeEventListener('focus', onSyncEvt);
      window.removeEventListener('storage', onStorageSync);
      document.removeEventListener('visibilitychange', onVisibilitySync);
    };
  }, [clearResultCache, refreshQuery, persistUiPreferencesPatch, i18n.resolvedLanguage, i18n.language]);

  const fetchNativeIcon = useCallback(async (
    ext: string,
    isDirectory: boolean,
    filePath?: string,
    meta?: { cacheKey?: string; resultType?: SearchResult['resultType']; browserSource?: string },
  ) => {
    const normalizedPath = filePath ? filePath.replace(/\//g, '\\').toLowerCase() : '';
    const key = meta?.cacheKey || (isDirectory ? '::dir::' : (normalizedPath ? `path:${normalizedPath}` : ext.toLowerCase()));
    if (nativeIconCacheRef.current.has(key)) {
      const cached = nativeIconCacheRef.current.get(key) || '';
      nativeIconCacheRef.current.delete(key);
      nativeIconCacheRef.current.set(key, cached);
      return cached || undefined;
    }
    if (inFlightIconsRef.current.has(key)) return undefined;
    inFlightIconsRef.current.add(key);
    try {
      const resp = await bridgeRequest<{ success: boolean; iconBase64?: string }>('search.getFileIcon', {
        ext: isDirectory ? '' : ext,
        isDirectory,
        ...(filePath ? { path: filePath } : {}),
        ...(meta?.resultType ? { resultType: meta.resultType } : {}),
        ...(meta?.browserSource ? { browserSource: meta.browserSource } : {}),
      });
      if (!resp?.success) {
        return undefined;
      }
      const iconVal = resp.iconBase64 || '';
      const nextSnapshot = touchAndEvictNativeIcons([[key, iconVal]]);
      if (nextSnapshot) {
        setNativeIconCache(nextSnapshot);
      }
      return iconVal || undefined;
    } catch {
      // IPC 异常时不写入永久负缓存，允许下次重绘重试
    } finally {
      inFlightIconsRef.current.delete(key);
    }
    return undefined;
  }, [touchAndEvictNativeIcons]);

  const prefetchIcons = useCallback(async (list: SearchResult[]) => {
    if (iconStyle !== 'native' || list.length === 0) return;
    const missing = new Set<string>();
    const items: Array<{
      key: string;
      ext: string;
      isDirectory: boolean;
      path?: string;
      resultType?: SearchResult['resultType'];
      browserSource?: string;
    }> = [];
    for (const item of list.slice(0, 100)) {
      const spec = resolveNativeIconRequest(item);
      const key = spec.cacheKey;
      if (
        !nativeIconCacheRef.current.has(key) &&
        !inFlightIconsRef.current.has(key) &&
        !missing.has(key)
      ) {
        missing.add(key);
        items.push({
          key,
          ext: spec.extension,
          isDirectory: spec.isDirectory,
          ...(spec.filePath ? { path: spec.filePath } : {}),
          ...(spec.resultType ? { resultType: spec.resultType } : {}),
          ...(spec.browserSource ? { browserSource: spec.browserSource } : {}),
        });
      }
    }
    if (items.length === 0) return;
    missing.forEach(k => inFlightIconsRef.current.add(k));
    try {
      const resp = await bridgeRequest<{ success: boolean; icons?: Record<string, string> }>('search.batchGetIcons', { items });
      if (!resp?.success || !resp.icons) {
        return;
      }
      const returned = resp.icons;
      const entries: Array<[string, string]> = [];
      for (const key of missing) {
        if (Object.prototype.hasOwnProperty.call(returned, key)) {
          entries.push([key, returned[key] || '']);
        }
      }
      if (entries.length > 0) {
        const nextSnapshot = touchAndEvictNativeIcons(entries);
        if (nextSnapshot) {
          setNativeIconCache(nextSnapshot);
        }
      }
    } catch {
      // 瞬时 IPC 异常允许后续重试
    } finally {
      missing.forEach(k => inFlightIconsRef.current.delete(k));
    }
  }, [iconStyle, touchAndEvictNativeIcons]);

  const changeIconStyle = (newStyle: SearchIconStyle) => {
    setIconStyle(newStyle);
    safeStorageSetItem('tools3000_search_icon_style', newStyle);
    void bridgeRequest('search.saveSettings', { iconStyle: newStyle });
  };

  const toggleLiveRefresh = useCallback(() => {
    setLiveRefreshEnabled((prev) => {
      const next = !prev;
      safeStorageSetItem('tools3000_search_live_refresh', String(next));
      void bridgeRequest('search.saveSettings', { liveRefreshEnabled: next }, { silent: true }).catch(() => undefined);
      if (next) {
        lastSeenIndexGenRef.current = null;
      }
      return next;
    });
  }, []);

  // 实时显示新增与变动文件（Live Watch）：仅在窗口可见、已输入文件名查询且非交互阻塞态时，
  // 通过零开销只读代次探测 (autoStart=false) 感知 USN 变动并原地无闪刷新结果列表。
  useEffect(() => {
    const trimmed = query.trim();
    const isContent = Boolean(
      trimmed &&
      isEffectiveContentSearch({
        query: trimmed,
        activeCategory,
        searchMode: activeCategory === 'content' ? 'content' : searchMode,
      }),
    );
    const isLocalOnlyPrefix =
      /^b(?:m)?:\s*/i.test(trimmed) ||
      /^cmd:\s*/i.test(trimmed) ||
      /^app:\s*/i.test(trimmed) ||
      activeCategory === 'bookmarks';

    if (
      !liveRefreshEnabled ||
      !trimmed ||
      isContent ||
      isLocalOnlyPrefix ||
      isComposing ||
      contextMenu.visible ||
      loading ||
      isRebuilding ||
      isServiceStarting ||
      isInitialIndexing
    ) {
      return;
    }

    let cancelled = false;
    let timerId: number | undefined;
    let inFlight = false;
    const LIVE_WATCH_INTERVAL_MS = 850;

    const scheduleNext = (delayMs: number) => {
      if (cancelled || !liveWatchWindowVisibleRef.current || document.hidden) return;
      if (timerId !== undefined) window.clearTimeout(timerId);
      timerId = window.setTimeout(() => {
        void pollOnce();
      }, delayMs);
    };

    const pollOnce = async () => {
      if (cancelled || inFlight || !liveWatchWindowVisibleRef.current || document.hidden) {
        return;
      }
      inFlight = true;
      try {
        const resp = await bridgeRequest<{
          success?: boolean;
          running?: boolean;
          ready?: boolean;
          indexGeneration?: number;
          totalIndexedFiles?: number;
        }>('search.getIndexGeneration', {}, { silent: true });
        if (cancelled || !liveWatchWindowVisibleRef.current || document.hidden) return;
        if (resp?.running && resp.ready && typeof resp.indexGeneration === 'number') {
          if (typeof resp.totalIndexedFiles === 'number' && resp.totalIndexedFiles > 0) {
            setTotalIndexedFiles(resp.totalIndexedFiles);
          }
          const prevGen = lastSeenIndexGenRef.current;
          lastSeenIndexGenRef.current = resp.indexGeneration;
          if (prevGen !== null && resp.indexGeneration !== prevGen) {
            silentRefreshQuery();
          }
        }
      } catch {
        // 忽略瞬时探测异常，绝不干扰正常搜索体验
      } finally {
        inFlight = false;
        scheduleNext(LIVE_WATCH_INTERVAL_MS);
      }
    };

    const onWindowHidden = () => {
      liveWatchWindowVisibleRef.current = false;
      if (timerId !== undefined) {
        window.clearTimeout(timerId);
        timerId = undefined;
      }
    };
    const onWindowShown = () => {
      liveWatchWindowVisibleRef.current = true;
      scheduleNext(350);
    };
    const onVisibility = () => {
      if (document.hidden) {
        onWindowHidden();
      } else {
        onWindowShown();
      }
    };

    window.addEventListener('tools3000:searchWindowHidden', onWindowHidden);
    window.addEventListener('tools3000:focusSearch', onWindowShown);
    document.addEventListener('visibilitychange', onVisibility);
    if (!document.hidden && liveWatchWindowVisibleRef.current) {
      scheduleNext(LIVE_WATCH_INTERVAL_MS);
    }

    return () => {
      cancelled = true;
      if (timerId !== undefined) window.clearTimeout(timerId);
      window.removeEventListener('tools3000:searchWindowHidden', onWindowHidden);
      window.removeEventListener('tools3000:focusSearch', onWindowShown);
      document.removeEventListener('visibilitychange', onVisibility);
    };
  }, [
    activeCategory,
    contextMenu.visible,
    isComposing,
    isInitialIndexing,
    isRebuilding,
    isServiceStarting,
    liveRefreshEnabled,
    loading,
    query,
    searchMode,
    setTotalIndexedFiles,
    silentRefreshQuery,
  ]);

  // 列配置（仅保留表头核心列 + 正交全文摘要开关，彻底清理卡片专属项）
  const [columns, setColumns] = useState<ColumnSetting[]>(() => {
    const parsed = safeStorageGetJsonArray<unknown>(
      'tools3000_search_columns_v2',
      (v): v is unknown => Boolean(v) && typeof v === 'object',
      [],
    );
    if (parsed.length > 0) {
      const byId = new Map<ColumnId, Partial<ColumnSetting>>();
      for (const item of parsed) {
        const rec = item as Partial<ColumnSetting>;
        if (typeof rec.id === 'string' && (VALID_COLUMN_IDS as readonly string[]).includes(rec.id)) {
          byId.set(rec.id as ColumnId, rec);
        }
      }
      return DEFAULT_COLUMNS.map((def) => {
        const saved = byId.get(def.id);
        if (!saved) return def;
        return {
          ...def,
          visible: typeof saved.visible === 'boolean' ? (def.id === 'name' ? true : saved.visible) : def.visible,
          flex: typeof saved.flex === 'number' && Number.isFinite(saved.flex) && saved.flex >= 10 && saved.flex <= 80
            ? saved.flex
            : def.flex,
        };
      });
    }
    return DEFAULT_COLUMNS;
  });

  useEffect(() => {
    void bridgeRequest<DriveInfo[]>('search.getDrives')
      .then((drives) => {
        if (Array.isArray(drives) && drives.length > 0) {
          setSystemDrives(drives);
          setEnabledDrives((prev) => {
            if (prev.length === 0) {
              const allLetters = drives.map((d) => d.letter);
              safeStorageSetItem('tools3000_search_enabled_drives', JSON.stringify(allLetters));
              return allLetters;
            }
            return prev;
          });
        }
      })
      .catch(() => undefined);
  }, []);

  const toggleDrive = (letter: string) => {
    setEnabledDrives((prev) => {
      const next = prev.includes(letter) ? prev.filter((l) => l !== letter) : [...prev, letter];
      safeStorageSetItem('tools3000_search_enabled_drives', JSON.stringify(next));
      void bridgeRequest('search.saveSettings', { enabledDrives: next.join(',') }).catch(() => undefined);
      return next;
    });
  };

  const selectAllDrives = () => {
    const all = systemDrives.map((d) => d.letter);
    setEnabledDrives(all);
    safeStorageSetItem('tools3000_search_enabled_drives', JSON.stringify(all));
    void bridgeRequest('search.saveSettings', { enabledDrives: all.join(',') }).catch(() => undefined);
  };

  const deselectAllDrives = () => {
    setEnabledDrives([]);
    safeStorageSetItem('tools3000_search_enabled_drives', JSON.stringify([]));
    void bridgeRequest('search.saveSettings', { enabledDrives: '' }).catch(() => undefined);
  };

  const toggleContentFormat = (ext: string) => {
    const cleanExt = ext.toLowerCase().replace(/^\./, '').trim();
    setDisabledContentFormats(prev => {
      const next = prev.includes(cleanExt)
        ? prev.filter(e => e !== cleanExt)
        : [...prev, cleanExt].filter(Boolean);
      safeStorageSetItem('tools3000_search_disabled_formats', JSON.stringify(next));
      persistUiPreferencesPatch({ disabledContentFormats: next });
      clearResultCache();
      if (next.includes(cleanExt)) {
        startTransition(() => {
          setResults(currentResults => currentResults.filter(item => {
            if (!item.path) return true;
            const itemExt = item.path.split('.').pop()?.toLowerCase() || '';
            return itemExt !== cleanExt;
          }));
        });
      }
      return next;
    });
  };

  const toggleCategoryFormats = (cat: FormatCategory, enableAll: boolean) => {
    setDisabledContentFormats(prev => {
      let next = [...prev];
      if (enableAll) {
        next = next.filter(e => !cat.extensions.includes(e));
      } else {
        for (const ext of cat.extensions) {
          if (!next.includes(ext)) next.push(ext);
        }
      }
      safeStorageSetItem('tools3000_search_disabled_formats', JSON.stringify(next));
      persistUiPreferencesPatch({ disabledContentFormats: next });
      clearResultCache();
      if (!enableAll) {
        const catExtSet = new Set(cat.extensions.map(e => e.toLowerCase()));
        startTransition(() => {
          setResults(currentResults => currentResults.filter(item => {
            if (!item.path) return true;
            const itemExt = item.path.split('.').pop()?.toLowerCase() || '';
            return !catExtSet.has(itemExt);
          }));
        });
      }
      return next;
    });
  };

  const addCustomContentFormat = (rawInput: string) => {
    const parts = rawInput
      .split(/[,;\s\uff0c\uff1b\u3001|]+/)
      .map(p => p.toLowerCase().replace(/^\./, '').trim())
      .filter(Boolean);
    if (parts.length === 0) return;
    setCustomContentFormats(prev => {
      const next = [...prev];
      for (const p of parts) {
        if (!next.includes(p)) next.push(p);
      }
      safeStorageSetItem('tools3000_search_custom_formats', JSON.stringify(next));
      persistUiPreferencesPatch({ customContentFormats: next });
      return next;
    });
    setDisabledContentFormats(prev => {
      const next = prev.filter(e => !parts.includes(e));
      safeStorageSetItem('tools3000_search_disabled_formats', JSON.stringify(next));
      persistUiPreferencesPatch({ disabledContentFormats: next });
      return next;
    });
    clearResultCache();
    setNewFormatInput('');
    toast.success(t('search.customExtAddedToast', 'Added {{exts}} to content search support list', { exts: parts.map(p => '.' + p).join(', ') }));
  };

  const removeCustomContentFormat = (ext: string) => {
    const cleanExt = ext.toLowerCase().replace(/^\./, '').trim();
    setCustomContentFormats(prev => {
      const next = prev.filter(e => e !== cleanExt);
      safeStorageSetItem('tools3000_search_custom_formats', JSON.stringify(next));
      persistUiPreferencesPatch({ customContentFormats: next });
      return next;
    });
    setDisabledContentFormats(prev => {
      const next = prev.filter(e => e !== cleanExt);
      safeStorageSetItem('tools3000_search_disabled_formats', JSON.stringify(next));
      persistUiPreferencesPatch({ disabledContentFormats: next });
      return next;
    });
    clearResultCache();
  };

  const resetContentFormats = () => {
    setDisabledContentFormats([]);
    setCustomContentFormats([]);
    safeStorageRemoveItem('tools3000_search_disabled_formats');
    safeStorageRemoveItem('tools3000_search_disabled_content_formats');
    safeStorageRemoveItem('tools3000_search_custom_formats');
    persistUiPreferencesPatch({ disabledContentFormats: [], customContentFormats: [] });
    toast.success(t('search.customExtResetToast', 'Content search format configuration reset to default'));
  };

  // F5 重建索引闭环：等待后台轮询确认 stillBusy === false 后再弹出成功提示并刷新列表（120s 防死锁上限）
  const rebuildIndex = useCallback(async () => {
    if (rebuildingRef.current) return;
    rebuildingRef.current = true;
    setIsRebuilding(true);
    toast.loading(t('search.rebuildingToast', 'Rescanning NTFS index and saving snapshot...'), { id: 'rebuild-idx' });

    const deadline = Date.now() + REBUILD_POLL_TIMEOUT_MS;
    try {
      await bridgeRequest('search.rebuildIndex');

      let completed = false;
      let sawBusyOnce = false;
      let initialGracePolls = 0;

      while (isMountedRef.current && Date.now() < deadline) {
        await new Promise<void>((resolve) => window.setTimeout(resolve, REBUILD_POLL_INTERVAL_MS));
        if (!isMountedRef.current) return;
        const stillBusy = await checkInitialIndex();
        if (stillBusy) {
          sawBusyOnce = true;
          continue;
        }
        // 给服务端状态位切换留出 1 次最小缓冲确认，随后判定真正完成
        if (!sawBusyOnce && initialGracePolls < 1) {
          initialGracePolls += 1;
          continue;
        }
        completed = true;
        break;
      }

      if (!isMountedRef.current) return;

      if (completed) {
        toast.success(t('search.rebuildSuccessToast', 'Full-disk index rescanned and saved to snapshot!'), { id: 'rebuild-idx' });
        clearResultCache();
        refreshQuery();
        void refreshHistory();
      } else {
        toast.info(
          t(
            'search.rebuildBackgroundToast' as never,
            'Index rebuild is still running in the background and will refresh automatically when finished' as never,
          ) as unknown as string,
          { id: 'rebuild-idx' },
        );
      }
    } catch {
      if (isMountedRef.current) {
        toast.error(t('search.rebuildError', 'Index rebuild and snapshot save failed'), { id: 'rebuild-idx' });
      }
    } finally {
      rebuildingRef.current = false;
      if (isMountedRef.current) {
        setIsRebuilding(false);
      }
    }
  }, [checkInitialIndex, clearResultCache, refreshHistory, refreshQuery, t]);

  useEffect(() => {
    void bridgeRequest<{ width?: number; height?: number; maximized?: boolean }>('search.getWindowSize')
      .then((res) => {
        if (res?.width && res?.height) {
          setWindowSize({ width: res.width, height: res.height });
        }
        if (res && typeof res.maximized === 'boolean') {
          setIsMaximized(res.maximized);
        }
      })
      .catch(() => undefined);
  }, []);

  const handleResizeMouseDown = (e: React.MouseEvent) => {
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation();
    void bridgeRequest('search.startResize', { edge: 'bottom_right', direction: 'bottom_right' }).catch(() => undefined);
  };

  const columnLayout = useMemo<ColumnLayout>(() => {
    const visibleSet = new Set(visibleTableColumns);
    return {
      name: true,
      path: visibleSet.has('path'),
      size: visibleSet.has('size'),
      modified: visibleSet.has('modified'),
      created: visibleSet.has('created'),
      snippets: columns.find(c => c.id === 'snippets')?.visible ?? true,
      nameFlex: columns.find(c => c.id === 'name')?.flex ?? 35,
      pathFlex: columns.find(c => c.id === 'path')?.flex ?? 45,
      tableOrder: tableColumnOrder,
      tableWidths: tableColumnWidths,
      visibleTableColumns,
    };
  }, [columns, tableColumnOrder, tableColumnWidths, visibleTableColumns]);

  const activeVisibleTableCols = useMemo(
    () => getVisibleTableColumns(columnLayout),
    [columnLayout],
  );

  // 设置抽屉列清单严格同步表头列单一事实源
  const settingsDrawerColumns = useMemo<ColumnSetting[]>(() => {
    const visibleSet = new Set(activeVisibleTableCols);
    return columns.map((col) => {
      if (col.id === 'snippets') return col;
      return {
        ...col,
        visible: col.id === 'name' ? true : visibleSet.has(col.id),
      };
    });
  }, [activeVisibleTableCols, columns]);

  // 仅当排序列确实依赖后端扩展属性（或 accessed/attributes 存在原生缺失行）时才将 propertyCacheForSort 纳入重排依赖
  const [propertyCacheForSort, setPropertyCacheForSort] = useState<PropertyCacheMap>({});

  const sortFieldNeedsBackendProp = useMemo(() => {
    if (sortField === 'relevance') return false;
    const def = resolveColumnDefinition(sortField);
    if (def.requiresBackendProp) return true;
    if (sortField === 'accessed') {
      return snapshot.results.some((item) => doesItemRequireBackendProperty(item, 'accessed'));
    }
    if (sortField === 'attributes') {
      return snapshot.results.some((item) => doesItemRequireBackendProperty(item, 'attributes'));
    }
    return false;
  }, [snapshot.results, sortField]);

  const activePropertyCacheForSort = sortFieldNeedsBackendProp ? propertyCacheForSort : undefined;

  const sortedResults = useMemo(() => {
    return getSortedResults(
      snapshot.results,
      sortField,
      sortDirection,
      activePropertyCacheForSort,
      currentLocale,
    );
  }, [snapshot.results, sortField, sortDirection, activePropertyCacheForSort, currentLocale]);

  const resultsHidden = showSyntaxHelp || sortedResults.length === 0;

  const {
    propertyCache,
    getLatestPropertyCache,
    sortProgress,
    cancelSortExtraction,
    ensureColumnLoadedForSort,
  } = useViewportProperties({
    sourceResults: snapshot.results,
    results: sortedResults,
    visibleColumns: activeVisibleTableCols,
    visibleRange,
    enabled: !resultsHidden,
    onPropertyCacheChange: setPropertyCacheForSort,
  });

  // 记录用户在当前结果代际中主动取消的扩展列排序，避免后台 effect 立即重拉
  const userCancelledSortKeyRef = useRef<string>('');
  const handleCancelSortExtraction = useCallback(() => {
    userCancelledSortKeyRef.current = `${snapshot.generationId}:${sortField}`;
    cancelSortExtraction();
  }, [cancelSortExtraction, snapshot.generationId, sortField]);

  // 切词或从本地存储恢复扩展列 sortField 时，自动拉齐该列全量属性完成排序
  useEffect(() => {
    if (resultsHidden || snapshot.results.length === 0 || !sortFieldNeedsBackendProp) {
      return;
    }
    const cancelKey = `${snapshot.generationId}:${sortField}`;
    if (userCancelledSortKeyRef.current === cancelKey) {
      return;
    }
    const label = getColumnDisplayLabel(
      sortField,
      (k, d) => t(k as never, d as never) as unknown as string,
    );
    void ensureColumnLoadedForSort(sortField, label, snapshot.results);
  }, [
    ensureColumnLoadedForSort,
    resultsHidden,
    snapshot.generationId,
    snapshot.results,
    sortField,
    sortFieldNeedsBackendProp,
    t,
  ]);

  const recordRecentColumn = useCallback((colId: TableColumnId) => {
    if (!colId || colId === 'name' || !isKnownTableColumnId(colId)) return;
    setRecentTableColumns((prev) => {
      const next = [colId, ...prev.filter((x) => x !== colId)].slice(0, 12);
      safeStorageSetItem('tools3000_search_recent_table_cols', JSON.stringify(next));
      persistUiPreferencesPatch({ recentTableColumns: next });
      return next;
    });
  }, [persistUiPreferencesPatch]);

  const fallbackSortIfColumnRemoved = useCallback((remainingCols: readonly TableColumnId[]) => {
    if (sortField !== 'relevance' && !remainingCols.includes(sortField)) {
      cancelSortExtraction();
      setSortField('relevance');
      setSortDirection('desc');
      safeStorageSetItem('tools3000_search_sort_field', 'relevance');
      safeStorageSetItem('tools3000_search_sort_dir', 'desc');
    }
  }, [cancelSortExtraction, sortField]);

  const commitVisibleTableColumns = useCallback(
    (nextVisible: TableColumnId[], nextOrderOverride?: TableColumnId[]) => {
      const cleanVisible = normalizeVisibleTableColumns(nextVisible);
      setVisibleTableColumns(cleanVisible);
      safeStorageSetItem('tools3000_search_visible_table_cols', JSON.stringify(cleanVisible));

      setColumns((prev) => {
        const updated = prev.map((col) => {
          if (col.id === 'path' || col.id === 'size' || col.id === 'modified' || col.id === 'created') {
            return { ...col, visible: cleanVisible.includes(col.id) };
          }
          return col;
        });
        safeStorageSetItem('tools3000_search_columns_v2', JSON.stringify(updated));
        return updated;
      });

      const mergedOrder = normalizeTableColumnOrder(
        nextOrderOverride ? [...nextOrderOverride, ...cleanVisible] : [...tableColumnOrder, ...cleanVisible],
      );
      setTableColumnOrder(mergedOrder);
      safeStorageSetItem('tools3000_search_table_order', JSON.stringify(mergedOrder));
      safeStorageSetItem('tools3000_search_col_schema_ver', String(TABLE_COLUMN_SCHEMA_VERSION));
      persistUiPreferencesPatch({
        columnSchemaVersion: TABLE_COLUMN_SCHEMA_VERSION,
        visibleTableColumns: cleanVisible,
        tableColumnOrder: mergedOrder,
      });
      fallbackSortIfColumnRemoved(cleanVisible);
    },
    [fallbackSortIfColumnRemoved, persistUiPreferencesPatch, tableColumnOrder],
  );

  const toggleColumnVisibility = useCallback((id: ColumnId) => {
    if (id === 'name') return;
    if (id === 'snippets') {
      setColumns((prev) => {
        const updated = prev.map((col) => (col.id === 'snippets' ? { ...col, visible: !col.visible } : col));
        safeStorageSetItem('tools3000_search_columns_v2', JSON.stringify(updated));
        return updated;
      });
      return;
    }
    const isCurrentlyVisible = activeVisibleTableCols.includes(id);
    const nextVisible = isCurrentlyVisible
      ? activeVisibleTableCols.filter((c) => c !== id)
      : [...activeVisibleTableCols, id];
    recordRecentColumn(id);
    commitVisibleTableColumns(nextVisible);
  }, [activeVisibleTableCols, commitVisibleTableColumns, recordRecentColumn]);

  const toggleTableColumn = useCallback(
    (colId: TableColumnId) => {
      const def = resolveColumnDefinition(colId);
      if (def.locked) return;
      const isCurrentlyVisible = activeVisibleTableCols.includes(colId);
      const nextVisible = isCurrentlyVisible
        ? activeVisibleTableCols.filter((id) => id !== colId)
        : [...activeVisibleTableCols, colId];
      recordRecentColumn(colId);
      commitVisibleTableColumns(nextVisible);
    },
    [activeVisibleTableCols, commitVisibleTableColumns, recordRecentColumn],
  );

  const removeTableColumn = useCallback(
    (colId: TableColumnId) => {
      const def = resolveColumnDefinition(colId);
      if (def.locked) return;
      if (!activeVisibleTableCols.includes(colId)) return;
      recordRecentColumn(colId);
      commitVisibleTableColumns(activeVisibleTableCols.filter((id) => id !== colId));
    },
    [activeVisibleTableCols, commitVisibleTableColumns, recordRecentColumn],
  );

  const addTableColumns = useCallback(
    (colIds: TableColumnId[]) => {
      if (colIds.length === 0) return;
      const nextVisible = [...activeVisibleTableCols];
      for (const id of colIds) {
        if (isKnownTableColumnId(id) && !nextVisible.includes(id)) {
          nextVisible.push(id);
          recordRecentColumn(id);
        }
      }
      commitVisibleTableColumns(nextVisible, nextVisible);
    },
    [activeVisibleTableCols, commitVisibleTableColumns, recordRecentColumn],
  );

  const updateNameAndPathFlex = useCallback((nameVal: number) => {
    const pathVal = Math.max(15, 80 - nameVal);
    setColumns((prev) => {
      const updated = prev.map((col) => {
        if (col.id === 'name') return { ...col, flex: nameVal };
        if (col.id === 'path') return { ...col, flex: pathVal };
        return col;
      });
      safeStorageSetItem('tools3000_search_columns_v2', JSON.stringify(updated));
      return updated;
    });
    // 调节名称/目录弹性比例时释放两列固定像素锁定，确保窗口缩放自适应铺满
    setTableColumnWidths((prev) => {
      if (prev.name === undefined && prev.path === undefined) return prev;
      const next = { ...prev };
      delete next.name;
      delete next.path;
      safeStorageSetItem('tools3000_search_table_widths', JSON.stringify(next));
      persistUiPreferencesPatch({ tableColumnWidths: next });
      return next;
    });
  }, [persistUiPreferencesPatch]);

  const updateTableColumnWidth = useCallback(
    (colId: TableColumnId, widthPx: number | undefined) => {
      setTableColumnWidths((prev) => {
        const next = { ...prev };
        if (widthPx === undefined) {
          delete next[colId];
        } else {
          next[colId] = Math.round(widthPx);
        }
        safeStorageSetItem('tools3000_search_table_widths', JSON.stringify(next));
        persistUiPreferencesPatch({ tableColumnWidths: next });
        return next;
      });
    },
    [persistUiPreferencesPatch],
  );

  const batchUpdateTableColumnWidths = useCallback(
    (patch: Partial<Record<TableColumnId, number | undefined>>) => {
      setTableColumnWidths((prev) => {
        const next = { ...prev };
        for (const [colId, widthPx] of Object.entries(patch)) {
          if (widthPx === undefined) {
            delete next[colId];
          } else {
            next[colId] = Math.round(widthPx);
          }
        }
        safeStorageSetItem('tools3000_search_table_widths', JSON.stringify(next));
        persistUiPreferencesPatch({ tableColumnWidths: next });
        return next;
      });
    },
    [persistUiPreferencesPatch],
  );

  const reorderTableColumnsOrder = useCallback(
    (nextOrder: TableColumnId[]) => {
      const cleanOrder = normalizeTableColumnOrder(nextOrder);
      setTableColumnOrder(cleanOrder);
      safeStorageSetItem('tools3000_search_table_order', JSON.stringify(cleanOrder));
      persistUiPreferencesPatch({ tableColumnOrder: cleanOrder });
    },
    [persistUiPreferencesPatch],
  );

  // 统一以 DEFAULT_VISIBLE_TABLE_COLUMNS 为默认列 SSOT，同时清理最近列并回退失效排序列
  const resetColumns = useCallback(() => {
    const defaultCols = [...DEFAULT_VISIBLE_TABLE_COLUMNS];
    const defaultOrder = [...DEFAULT_TABLE_COLUMN_ORDER];
    setColumns(DEFAULT_COLUMNS);
    safeStorageSetItem('tools3000_search_columns_v2', JSON.stringify(DEFAULT_COLUMNS));
    setTableColumnOrder(defaultOrder);
    setTableColumnWidths({});
    setVisibleTableColumns(defaultCols);
    setRecentTableColumns([]);
    safeStorageRemoveItem('tools3000_search_table_order');
    safeStorageRemoveItem('tools3000_search_table_widths');
    safeStorageRemoveItem('tools3000_search_visible_table_cols');
    safeStorageRemoveItem('tools3000_search_recent_table_cols');
    safeStorageSetItem('tools3000_search_col_schema_ver', String(TABLE_COLUMN_SCHEMA_VERSION));
    fallbackSortIfColumnRemoved(defaultCols);
    persistUiPreferencesPatch({
      columnSchemaVersion: TABLE_COLUMN_SCHEMA_VERSION,
      tableColumnOrder: defaultOrder,
      tableColumnWidths: {},
      visibleTableColumns: defaultCols,
      recentTableColumns: [],
    });
    toast.success(t('search.toastResetLayout', 'Default columns and layout restored'));
  }, [fallbackSortIfColumnRemoved, persistUiPreferencesPatch, t]);

  // 严格列管理弹窗草稿事务入口：打开初始化独立草稿，取消完整回滚，确定统一落盘
  const openColumnManager = useCallback(() => {
    setColumnManagerDraft([...activeVisibleTableCols]);
    setColumnManagerResetPending(false);
    setShowColumnManager(true);
  }, [activeVisibleTableCols]);

  const closeColumnManager = useCallback(() => {
    setShowColumnManager(false);
    setColumnManagerDraft(null);
    setColumnManagerResetPending(false);
    window.setTimeout(() => inputRef.current?.focus(), 0);
  }, []);

  const applyColumnManagerDraft = useCallback(
    (finalDraft: TableColumnId[], meta?: { resetRequested?: boolean }) => {
      const shouldFullReset =
        Boolean(meta?.resetRequested || columnManagerResetPending) &&
        finalDraft.length === DEFAULT_VISIBLE_TABLE_COLUMNS.length &&
        finalDraft.every((id, idx) => id === DEFAULT_VISIBLE_TABLE_COLUMNS[idx]);

      if (shouldFullReset) {
        resetColumns();
      } else {
        const prevSet = new Set(activeVisibleTableCols);
        for (const id of finalDraft) {
          if (!prevSet.has(id) && id !== 'name') {
            recordRecentColumn(id);
          }
        }
        commitVisibleTableColumns(finalDraft, finalDraft);
      }
      setColumnManagerDraft(null);
      setColumnManagerResetPending(false);
    },
    [activeVisibleTableCols, columnManagerResetPending, commitVisibleTableColumns, recordRecentColumn, resetColumns],
  );

  const changeDensity = (newDensity: SearchDensity) => {
    setDensity(newDensity);
    safeStorageSetItem('tools3000_search_density', newDensity);
    persistUiPreferencesPatch({ density: newDensity });
    inputRef.current?.focus();
  };

  const userTypedSinceShowRef = useRef(false);
  const pendingSelectTimersRef = useRef<number[]>([]);

  const cancelPendingSelectTimers = useCallback(() => {
    userTypedSinceShowRef.current = true;
    for (const tid of pendingSelectTimersRef.current) {
      window.clearTimeout(tid);
    }
    pendingSelectTimersRef.current = [];
  }, []);

  useEffect(() => {
    const scheduleInitialFocusAndSelect = () => {
      for (const tid of pendingSelectTimersRef.current) {
        window.clearTimeout(tid);
      }
      pendingSelectTimersRef.current = [];
      userTypedSinceShowRef.current = false;

      if (inputRef.current) {
        inputRef.current.focus({ preventScroll: true });
        inputRef.current.select();
      }

      const delays = [30, 100, 250, 500];
      for (const delay of delays) {
        const tid = window.setTimeout(() => {
          if (userTypedSinceShowRef.current) return;
          if (inputRef.current) {
            inputRef.current.focus({ preventScroll: true });
            inputRef.current.select();
          }
        }, delay);
        pendingSelectTimersRef.current.push(tid);
      }
    };

    scheduleInitialFocusAndSelect();

    const onHiddenEvt = () => {
      wasHiddenSinceLastShowRef.current = true;
    };
    const onFocusEvt = () => {
      if (wasHiddenSinceLastShowRef.current) {
        wasHiddenSinceLastShowRef.current = false;
        if (clearQueryOnShowRef.current) {
          clearQueryForFreshShowRef.current();
        }
      }
      scheduleInitialFocusAndSelect();
    };
    window.addEventListener('tools3000:searchWindowHidden', onHiddenEvt);
    window.addEventListener('tools3000:focusSearch', onFocusEvt);

    const onEarlyKeyDown = (e: KeyboardEvent) => {
      if (e.key !== 'Alt' && e.key !== 'Control' && e.key !== 'Shift' && e.key !== 'Meta') {
        cancelPendingSelectTimers();
      }
    };
    window.addEventListener('keydown', onEarlyKeyDown, true);

    const handleGlobalClick = (e: MouseEvent) => {
      const target = e.target as HTMLElement | null;
      if (!target?.closest('.search-context-menu')) {
        setContextMenu({ visible: false, x: 0, y: 0 });
      }
    };
    window.addEventListener('click', handleGlobalClick);
    window.addEventListener('pointerdown', handleGlobalClick);

    return () => {
      for (const tid of pendingSelectTimersRef.current) {
        window.clearTimeout(tid);
      }
      pendingSelectTimersRef.current = [];
      window.removeEventListener('tools3000:searchWindowHidden', onHiddenEvt);
      window.removeEventListener('tools3000:focusSearch', onFocusEvt);
      window.removeEventListener('keydown', onEarlyKeyDown, true);
      window.removeEventListener('click', handleGlobalClick);
      window.removeEventListener('pointerdown', handleGlobalClick);
    };
  }, [cancelPendingSelectTimers]);

  useEffect(() => {
    const handleClickOutside = (e: MouseEvent) => {
      const target = e.target as Node;
      if (viewSettingsRef.current && !viewSettingsRef.current.contains(target)) {
        const helpBtn = document.querySelector('.search-help-btn');
        if (!helpBtn || !helpBtn.contains(target)) {
          setShowViewSettings(false);
        }
      }
    };
    if (showViewSettings) {
      window.addEventListener('mousedown', handleClickOutside);
      return () => window.removeEventListener('mousedown', handleClickOutside);
    }
  }, [showViewSettings]);

  // 自适应列宽优先采样当前 visibleRange 可见行
  const fitColumnToContent = useCallback(
    (colId: TableColumnId) => {
      const translateLabel = (key: string, defaultVal?: string) =>
        t(key as never, defaultVal as never) as unknown as string;
      const headerLabel = getColumnDisplayLabel(colId, translateLabel);
      const formatters = { formatFileSize, formatWindowsTime, extractParentDirectoryPath };
      const sampleRows = collectColumnFitSampleRows(sortedResults, visibleRange, 120);
      const sample = sampleRows.map((item) =>
        resolveTableCellText(colId, item, propertyCache[item.path], formatters),
      );
      const fitted = estimateFittedColumnWidth(colId, headerLabel, sample);
      updateTableColumnWidth(colId, fitted);
    },
    [propertyCache, sortedResults, t, updateTableColumnWidth, visibleRange],
  );

  const fitAllColumnsToContent = useCallback(() => {
    const translateLabel = (key: string, defaultVal?: string) =>
      t(key as never, defaultVal as never) as unknown as string;
    const formatters = { formatFileSize, formatWindowsTime, extractParentDirectoryPath };
    const sampleRows = collectColumnFitSampleRows(sortedResults, visibleRange, 120);
    setTableColumnWidths((prev) => {
      const next = { ...prev };
      for (const colId of activeVisibleTableCols) {
        const headerLabel = getColumnDisplayLabel(colId, translateLabel);
        const sample = sampleRows.map((item) =>
          resolveTableCellText(colId, item, propertyCache[item.path], formatters),
        );
        next[colId] = estimateFittedColumnWidth(colId, headerLabel, sample);
      }
      safeStorageSetItem('tools3000_search_table_widths', JSON.stringify(next));
      persistUiPreferencesPatch({ tableColumnWidths: next });
      return next;
    });
  }, [activeVisibleTableCols, persistUiPreferencesPatch, propertyCache, sortedResults, t, visibleRange]);

  const handleTableHorizontalScroll = useCallback((scrollLeft: number) => {
    if (headerScrollRef.current && headerScrollRef.current.scrollLeft !== scrollLeft) {
      headerScrollRef.current.scrollLeft = scrollLeft;
    }
  }, []);

  useEffect(() => {
    if (sortedResults.length === 0) return;
    const timer = window.setTimeout(() => {
      const headSlice = sortedResults.slice(0, 60);
      if (selectedIndex >= 50) {
        const winStart = Math.max(60, selectedIndex - 20);
        const winSlice = sortedResults.slice(winStart, winStart + 40);
        void prefetchIcons([...headSlice, ...winSlice]);
      } else {
        void prefetchIcons(sortedResults.slice(0, 100));
      }
    }, 0);
    return () => window.clearTimeout(timer);
  }, [prefetchIcons, sortedResults, selectedIndex]);

  const totalResultSize = useMemo(() => {
    return sortedResults.reduce((acc, item) => acc + (item.isDirectory ? 0 : (Number(item.size) || 0)), 0);
  }, [sortedResults]);

  const handleSelectSort = useCallback((field: SortField) => {
    if (field === 'relevance') {
      cancelSortExtraction();
      setSortField('relevance');
      setSortDirection('desc');
      safeStorageSetItem('tools3000_search_sort_field', 'relevance');
      safeStorageSetItem('tools3000_search_sort_dir', 'desc');
      setSelectedIndex(0);
      inputRef.current?.focus();
      return;
    }
    if (!isKnownTableColumnId(field)) return;
    userCancelledSortKeyRef.current = '';

    const def = resolveColumnDefinition(field);
    if (sortField === field) {
      if (sortDirection === def.defaultSortDir) {
        const nextDir = sortDirection === 'desc' ? 'asc' : 'desc';
        setSortDirection(nextDir);
        safeStorageSetItem('tools3000_search_sort_dir', nextDir);
      } else {
        // 再次点击当前列：三态循环恢复智能相关性（保留后端 Frecency 与 3 档融合优先级）
        cancelSortExtraction();
        setSortField('relevance');
        setSortDirection('desc');
        safeStorageSetItem('tools3000_search_sort_field', 'relevance');
        safeStorageSetItem('tools3000_search_sort_dir', 'desc');
        setSelectedIndex(0);
        inputRef.current?.focus();
        return;
      }
    } else {
      setSortField(field);
      const defaultDir = def.defaultSortDir;
      setSortDirection(defaultDir);
      safeStorageSetItem('tools3000_search_sort_field', field);
      safeStorageSetItem('tools3000_search_sort_dir', defaultDir);
    }

    const needsBackend =
      def.requiresBackendProp ||
      (field === 'accessed' && snapshot.results.some((item) => doesItemRequireBackendProperty(item, 'accessed'))) ||
      (field === 'attributes' && snapshot.results.some((item) => doesItemRequireBackendProperty(item, 'attributes')));

    if (needsBackend && snapshot.results.length > 0) {
      const label = getColumnDisplayLabel(
        field,
        (k, d) => t(k as never, d as never) as unknown as string,
      );
      void ensureColumnLoadedForSort(field, label, snapshot.results);
    } else {
      // 切换回普通原生列排序时主动终止残留的扩展列提取任务
      cancelSortExtraction();
    }

    setSelectedIndex(0);
    inputRef.current?.focus();
  }, [cancelSortExtraction, ensureColumnLoadedForSort, setSelectedIndex, snapshot.results, sortDirection, sortField, t]);

  const updateQuery = useCallback((next: string) => {
    setQuery(next);
    setActionError('');
    if (!next.trim()) {
      setIsServiceStarting(false);
    }

    // 精准解析独立 sort:<col> 词元，避免粗糙子串 includes 误判，且绝不篡改已移除的组合排序开关
    const explicitSort = parseExplicitSortDirective(next);
    if (explicitSort) {
      setSortField(explicitSort.field);
      setSortDirection(explicitSort.direction);
      safeStorageSetItem('tools3000_search_sort_field', explicitSort.field);
      safeStorageSetItem('tools3000_search_sort_dir', explicitSort.direction);
    }

    if (!next.trim()) {
      setResults([]);
      setSelectedIndex(0);
      setLoading(false);
    }
  }, [setIsServiceStarting, setLoading, setResults, setSelectedIndex]);

  useEffect(() => {
    clearQueryForFreshShowRef.current = () => {
      updateQuery('');
    };
  }, [updateQuery]);

  const selectCategory = useCallback((cat: CategoryFilter) => {
    if (cat.id === 'content') {
      startTransition(() => {
        setResults([]);
        setSelectedIndex(0);
      });
      updateQuery(buildSmartContentSearchQuery(query));
      inputRef.current?.focus();
      return;
    }

    let cleaned = stripContentSyntaxFromQuery(query).trimStart();
    // 剥离书签/应用/命令前缀（严格保留 b:\ 与 b:/ 盘符路径）
    if (/^(bm|app|cmd):/i.test(cleaned)) {
      cleaned = cleaned.replace(/^(bm|app|cmd):\s*/i, '');
    } else if (/^b:(?![\\/])/i.test(cleaned)) {
      cleaned = cleaned.replace(/^b:\s*/i, '');
    }
    CATEGORY_DEFS.forEach(c => {
      if (c.prefix && c.id !== 'content' && cleaned.toLowerCase().startsWith(c.prefix.trim().toLowerCase())) {
        const rest = cleaned.slice(c.prefix.trim().length);
        if (!rest || /^\s/.test(rest)) {
          cleaned = rest.trimStart();
        }
      }
    });

    if (!cat.prefix) {
      updateQuery(cleaned.trimStart());
    } else {
      const trimmedClean = cleaned.trimStart();
      updateQuery(trimmedClean ? cat.prefix + trimmedClean : cat.prefix);
    }
    inputRef.current?.focus();
  }, [query, updateQuery, setResults, setSelectedIndex]);

  const applySyntaxExample = (syntax: string) => {
    updateQuery(syntax);
    setShowSyntaxHelp(false);
    inputRef.current?.focus();
  };

  const isAuxiliaryDragSurface = useCallback((target: EventTarget | null): boolean => {
    const el = target as HTMLElement | null;
    if (!el) return false;
    return !el.closest(
      'button, input, textarea, select, a, [role="tab"], [role="button"], .category-pill, [data-clickable="true"]',
    );
  }, []);

  const handleAuxiliaryDragMouseDown = useCallback(
    (e: ReactMouseEvent<HTMLElement>) => {
      if (e.button !== 0 || e.detail >= 2 || !isAuxiliaryDragSurface(e.target)) {
        return;
      }
      const startX = e.screenX;
      const startY = e.screenY;
      const cleanup = () => {
        window.removeEventListener('mousemove', onMove);
        window.removeEventListener('mouseup', cleanup);
        window.removeEventListener('blur', cleanup);
      };
      const onMove = (moveEvt: MouseEvent) => {
        if ((moveEvt.buttons & 1) === 0) {
          cleanup();
          return;
        }
        if (Math.hypot(moveEvt.screenX - startX, moveEvt.screenY - startY) >= 4) {
          cleanup();
          void bridgeRequest('search.startDrag');
        }
      };
      window.addEventListener('mousemove', onMove);
      window.addEventListener('mouseup', cleanup);
      window.addEventListener('blur', cleanup);
    },
    [isAuxiliaryDragSurface],
  );

  const handleAuxiliaryDoubleClick = useCallback(
    (e: ReactMouseEvent<HTMLElement>) => {
      if (isAuxiliaryDragSurface(e.target)) {
        void toggleMaximized();
      }
    },
    [isAuxiliaryDragSurface, toggleMaximized],
  );

  const minimizeWindow = useCallback(() => {
    setShowHistoryMenu(false);
    if (!isComposingRef.current) {
      recordSearchHistory(latestQueryRef.current);
    }
    void bridgeRequest('search.minimize').catch(() => undefined);
  }, [recordSearchHistory]);

  const hide = useCallback(() => {
    setShowHistoryMenu(false);
    if (!isComposingRef.current) {
      recordSearchHistory(latestQueryRef.current);
    }
    void bridgeRequest('search.hide').catch(() => undefined);
  }, [recordSearchHistory]);

  const handleSelectHistoryItem = useCallback((text: string) => {
    updateQuery(text);
    recordSearchHistory(text, { force: true });
    setShowHistoryMenu(false);
    inputRef.current?.focus();
  }, [recordSearchHistory, updateQuery]);

  const {
    openResult,
    openFolderResult,
    copyPathResult,
    copyFileResult,
    quickLookResult,
    pinResult,
    openResultAsAdmin,
    showFileProperties,
    copyText,
    openWithNotepad,
    startRename,
    confirmRename,
    exportResultsToCsv,
  } = useSearchActions({
    isPinned,
    hideOnOpen,
    hide,
    setActionError,
    setContextMenu,
    renameTarget,
    setRenameTarget,
    renameInputRef,
    searchInputRef: inputRef,
    setResults,
    query,
    sortedResults,
    visibleTableColumns: activeVisibleTableCols,
    propertyCache,
    getLatestPropertyCache,
    ensureColumnLoadedForSort,
    onRecordSearch: recordSearchHistory,
  });

  useSearchKeyboard({
    inputRef,
    isComposing,
    sortedResults,
    selectedIndex,
    setSelectedIndex,
    query,
    activeCategory,
    searchMode,
    renameTarget,
    setRenameTarget,
    contextMenu,
    setContextMenu,
    showHistoryMenu,
    setShowHistoryMenu,
    showSyntaxHelp,
    setShowSyntaxHelp,
    showViewSettings,
    setShowViewSettings,
    showColumnManager,
    setShowColumnManager,
    showPropertyPicker,
    setShowPropertyPicker,
    hide,
    rebuildIndex,
    togglePin,
    toggleMaximized,
    startRename,
    handleSelectSort,
    openResult,
    openFolderResult,
    pinResult,
    openResultAsAdmin,
    showFileProperties,
    copyPathResult,
    copyFileResult,
    quickLookResult,
    exportResultsToCsv,
    selectCategory,
    categories,
  });

  const nameFlex = columns.find(c => c.id === 'name')?.flex ?? 35;
  const pathFlex = columns.find(c => c.id === 'path')?.flex ?? 45;

  const handleRowHover = useCallback((index: number) => {
    setSelectedIndex(index);
  }, [setSelectedIndex]);

  const handleRowSelect = useCallback((index: number) => {
    setSelectedIndex(index);
    setContextMenu((prev) => (prev.visible ? { visible: false, x: 0, y: 0 } : prev));
  }, [setSelectedIndex]);

  const handleRowOpen = useCallback((result: SearchResult) => {
    void openResult(result);
  }, [openResult]);

  const handleRowContextMenu = useCallback((event: ReactMouseEvent, index: number, result: SearchResult) => {
    event.preventDefault();
    event.stopPropagation();
    setSelectedIndex(index);
    if (event.shiftKey) {
      setTimeout(() => {
        void bridgeRequest('search.showShellContextMenu', {
          filepath: result.path,
          path: result.path,
          x: -1,
          y: -1,
        });
      }, 20);
      return;
    }
    const menuWidth = 248;
    const menuHeight = 356;
    const x = Math.min(event.clientX, window.innerWidth - menuWidth - 10);
    const y = Math.min(event.clientY, window.innerHeight - menuHeight - 10);
    setContextMenu({
      visible: true,
      x: Math.max(10, x),
      y: Math.max(10, y),
      result,
    });
  }, [setSelectedIndex]);

  const formattedTotalIndexed = totalIndexedFiles > 0
    ? compactCountFormatter.format(totalIndexedFiles)
    : '--';

  const propertyPickerExistingCols =
    propertyPickerSource === 'manager' && columnManagerDraft
      ? columnManagerDraft
      : activeVisibleTableCols;

  return (
    <main className={`search-app ${showViewSettings ? 'search-app--view-settings-open' : ''} ${isMaximized ? 'search-app--maximized' : ''}`}>
      <AppToaster theme={toasterTheme} surface="search" />
      <section className={`search-container ${isMaximized ? 'search-container--maximized' : ''}`} aria-label={t('search.title', 'Quick file search')}>
        <SearchHeader
          inputRef={inputRef}
          query={query}
          onUpdateQuery={updateQuery}
          activeCategory={activeCategory}
          searchMode={searchMode}
          loading={loading}
          isInitialIndexing={isInitialIndexing}
          isServiceStarting={isServiceStarting}
          isPinned={isPinned}
          onTogglePin={() => void togglePin()}
          isMaximized={isMaximized}
          onToggleMaximized={() => void toggleMaximized()}
          onMinimizeWindow={minimizeWindow}
          onCloseWindow={hide}
          showViewSettings={showViewSettings}
          onToggleViewSettings={() => {
            setShowViewSettings(prev => !prev);
            setShowSyntaxHelp(false);
            setShowHistoryMenu(false);
          }}
          showSyntaxHelp={showSyntaxHelp}
          onToggleSyntaxHelp={() => {
            setShowSyntaxHelp(prev => !prev);
            setShowViewSettings(false);
            setShowHistoryMenu(false);
          }}
          onResetPlacement={() => {
            void bridgeRequest('search.resetPlacement');
            setIsMaximized(false);
            setWindowSize({ width: 760, height: 520 });
            toast.success(t('search.resetPlacementToast', 'Reset to default center and 760×520 size'));
          }}
          searchHistory={searchHistory}
          showHistoryMenu={showHistoryMenu}
          onToggleHistoryMenu={() => {
            setShowHistoryMenu((prev) => {
              const next = !prev;
              if (next) {
                setShowViewSettings(false);
                setShowSyntaxHelp(false);
                void refreshHistory();
              }
              return next;
            });
          }}
          onCloseHistoryMenu={() => setShowHistoryMenu(false)}
          onSelectHistoryItem={handleSelectHistoryItem}
          onRemoveHistoryItem={(e, text) => void removeHistoryItem(e, text)}
          onClearAllHistory={() => void clearAllHistory()}
          resultsCount={results.length}
          hasSelectedResult={!!sortedResults[selectedIndex]}
          selectedIndex={selectedIndex}
          setIsComposing={setIsComposing}
          onUserTypingActivity={cancelPendingSelectTimers}
        />

        <div
          className="search-categories-bar"
          onMouseDown={handleAuxiliaryDragMouseDown}
          onDoubleClick={handleAuxiliaryDoubleClick}
        >
          <SearchCategoryBar
            categories={categories}
            activeCategory={activeCategory}
            onSelectCategory={selectCategory}
            onReorderCategories={handleReorderCategories}
            onResetCategoryOrder={handleResetCategoryOrder}
          />

          <div className="search-bar-right-group">
            {/* 全盘已索引总数与耗时统计胶囊 */}
            <div
              className="search-stats-pill"
              data-clickable={loading ? 'true' : undefined}
              title={
                loading
                  ? t('search.stopScanTitle', 'Stop active background content scan')
                  : t('search.statsPillTip', 'Indexed {{total}} files across all disks, latest search elapsed {{ms}}ms', { total: totalIndexedFiles ? totalIndexedFiles.toLocaleString() : '1,000,000+', ms: searchElapsedMs })
              }
              onClick={loading ? stopSearch : undefined}
              style={loading ? { cursor: 'pointer' } : undefined}
            >
              <span className={`search-stats-dot ${loading ? 'search-stats-dot--scanning' : ''}`} />
              {loading && sortedResults.length > 0 ? (
                <span>
                  <strong>{sortedResults.length}+</strong> · {t('search.searchingElapsed', 'Searching... {{elapsed}}s', { elapsed: scanElapsedSeconds.toFixed(1) })}
                  {scanProgress && scanProgress.total > 0 ? ` (${Math.min(99, Math.max(1, Math.round((scanProgress.scanned / scanProgress.total) * 100)))}%)` : ''}
                </span>
              ) : loading && scanElapsedSeconds > 0 ? (
                <span>
                  <strong>
                    {t('search.searchingElapsed', 'Searching... {{elapsed}}s', { elapsed: scanElapsedSeconds.toFixed(1) })}
                    {scanProgress && scanProgress.total > 0 ? ` (${Math.min(99, Math.max(1, Math.round((scanProgress.scanned / scanProgress.total) * 100)))}%)` : ''}
                  </strong>
                </span>
              ) : sortedResults.length > 0 ? (
                <span><strong>{sortedResults.length}</strong> / {t('search.statsFullDisk', 'Total {{total}} · {{ms}}ms', { total: formattedTotalIndexed, ms: searchElapsedMs })}</span>
              ) : (
                <span>{t('search.totalFilesCount', 'Total {{count}} files', { count: totalIndexedFiles || 0 })}</span>
              )}
            </div>

            {/* 导出当前搜索结果为 CSV */}
            {sortedResults.length > 0 && (
              <button
                type="button"
                className="search-action-pill-btn"
                onClick={() => void exportResultsToCsv()}
                title={t('search.exportCsvTip', 'Export search results to CSV / Excel (Ctrl+E) - {{count}} items', { count: sortedResults.length })}
              >
                <FileSpreadsheet size={13} aria-hidden="true" />
                <span>{t('search.exportCsv', 'Export CSV')} ({sortedResults.length})</span>
              </button>
            )}
          </div>
        </div>

        {/* ── 历史搜索词极客气泡条 ── */}
        <SearchHistoryBar
          query={query}
          searchHistory={searchHistory}
          onClearAllHistory={clearAllHistory}
          onSelectHistoryItem={handleSelectHistoryItem}
          onRemoveHistoryItem={(e, text) => void removeHistoryItem(e, text)}
        />

        {/* ── 视图与搜索偏好设置弹窗 ── */}
        <SearchViewSettingsModal
          visible={showViewSettings}
          onClose={() => {
            setShowViewSettings(false);
            window.setTimeout(() => inputRef.current?.focus(), 0);
          }}
          popoverRef={viewSettingsRef}
          isRebuilding={isRebuilding}
          onRebuildIndex={() => void rebuildIndex()}
          searchMode={searchMode}
          onChangeSearchMode={changeSearchMode}
          density={density}
          onChangeDensity={changeDensity}
          iconStyle={iconStyle}
          onChangeIconStyle={changeIconStyle}
          maxResultLimit={maxResultLimit}
          onChangeMaxResultLimit={changeMaxResultLimit}
          windowSize={windowSize}
          onSetWindowSize={(nextSize) => {
            setIsMaximized(false);
            setWindowSize(nextSize);
          }}
          isMaximized={isMaximized}
          onToggleMaximized={() => void toggleMaximized()}
          columns={settingsDrawerColumns}
          onToggleColumn={toggleColumnVisibility}
          onOpenColumnManager={openColumnManager}
          nameFlex={nameFlex}
          pathFlex={pathFlex}
          onUpdateNameAndPathFlex={updateNameAndPathFlex}
          systemDrives={systemDrives}
          enabledDrives={enabledDrives}
          onToggleDrive={toggleDrive}
          onSelectAllDrives={selectAllDrives}
          onDeselectAllDrives={deselectAllDrives}
          excludeGitAndModules={excludeGitAndModules}
          onSetExcludeGitAndModules={(val) => {
            setExcludeGitAndModules(val);
            safeStorageSetItem('tools3000_search_exclude_dev', String(val));
            const patterns = val ? '$Recycle.Bin,System Volume Information,node_modules,.git,__pycache__' : '$Recycle.Bin,System Volume Information';
            void bridgeRequest('search.saveSettings', { excludePatterns: patterns }).catch(() => undefined);
            persistUiPreferencesPatch({ excludeGitAndModules: val });
          }}
          excludeHidden={excludeHidden}
          onSetExcludeHidden={(val) => {
            setExcludeHidden(val);
            safeStorageSetItem('tools3000_search_exclude_hidden', String(val));
            void bridgeRequest('search.saveSettings', { excludeHidden: val, excludeSystem: val }).catch(() => undefined);
            persistUiPreferencesPatch({ excludeHidden: val });
          }}
          customExcludedPaths={customExcludedPaths}
          onChangeCustomExcludedPaths={updateCustomExcludedPaths}
          disabledContentFormats={disabledContentFormats}
          onToggleContentFormat={toggleContentFormat}
          onToggleCategoryFormats={toggleCategoryFormats}
          onResetContentFormats={resetContentFormats}
          customContentFormats={customContentFormats}
          newFormatInput={newFormatInput}
          onSetNewFormatInput={setNewFormatInput}
          onAddCustomContentFormat={addCustomContentFormat}
          onRemoveCustomContentFormat={removeCustomContentFormat}
          onSetDisabledContentFormats={setDisabledContentFormats}
          dbStats={dbStats ? { exists: dbStats.exists, dbSize: dbStats.dbSize, totalRecords: dbStats.totalRecords } : undefined}
          totalIndexedFiles={totalIndexedFiles}
          onResetAllViewPreferences={() => {
            resetColumns();
            resetContentFormats();
            setSearchMode('name');
            setDefaultCategory('all');
            setDensity('standard');
            setIconStyle('native');
            setMaxResultLimit(0);
            setExcludeGitAndModules(true);
            setExcludeHidden(false);
            setSortField('relevance');
            setSortDirection('desc');
            const allLetters = systemDrives.map((d) => d.letter);
            if (allLetters.length > 0) {
              setEnabledDrives(allLetters);
              safeStorageSetItem('tools3000_search_enabled_drives', JSON.stringify(allLetters));
            }
            safeStorageRemoveItem('tools3000_search_default_mode');
            safeStorageRemoveItem('tools3000_search_default_category');
            safeStorageRemoveItem('tools3000_search_view_layout');
            safeStorageRemoveItem('tools3000_search_density');
            safeStorageRemoveItem('tools3000_search_icon_style');
            safeStorageRemoveItem('tools3000_search_max_results');
            safeStorageRemoveItem('tools3000_search_exclude_dev');
            safeStorageRemoveItem('tools3000_search_exclude_hidden');
            safeStorageRemoveItem('tools3000_search_sort_field');
            safeStorageRemoveItem('tools3000_search_sort_dir');
            safeStorageRemoveItem('tools3000_search_folder_priority');
            safeStorageRemoveItem('tools3000_search_sort_folders_first');
            safeStorageRemoveItem('tools3000_search_sort_group_type');
            safeStorageRemoveItem('tools3000_search_smart_path_priority');
            safeStorageRemoveItem(SEARCH_CATEGORY_ORDER_STORAGE_KEY);
            setCategoryOrder([...DEFAULT_SEARCH_CATEGORY_ORDER]);
            void bridgeRequest('search.saveSettings', {
              iconStyle: 'native',
              defaultCategory: 'all',
              maxResults: 0,
              excludeHidden: false,
              excludePatterns: '$Recycle.Bin,System Volume Information,node_modules,.git,__pycache__',
              enabledDrives: allLetters.join(','),
            }).catch(() => {});
            persistUiPreferencesPatch({
              searchMode: 'name',
              defaultCategory: 'all',
              categoryOrder: [...DEFAULT_SEARCH_CATEGORY_ORDER],
              tableColumnOrder: [...DEFAULT_TABLE_COLUMN_ORDER],
              tableColumnWidths: {},
              visibleTableColumns: [...DEFAULT_VISIBLE_TABLE_COLUMNS],
              recentTableColumns: [],
              density: 'standard',
              maxResultLimit: 0,
              excludeGitAndModules: true,
              excludeHidden: false,
              disabledContentFormats: [],
              customContentFormats: [],
            });
            toast.success(t('search.toastResetAllPref', 'All view and search preferences reset to defaults'));
          }}
        />

        {/* ── 语法帮助快速指南抽屉 (独占主内容槽，杜绝与空态/加载态重叠穿模) ── */}
        <SearchSyntaxModal
          visible={showSyntaxHelp}
          onClose={() => setShowSyntaxHelp(false)}
          syntaxCat={syntaxCat}
          onSelectCategory={setSyntaxCat}
          onApplyExample={applySyntaxExample}
        />

        {/* ── 状态与空结果显示 ── */}
        {!showSyntaxHelp && (
          <SearchEmptyState
            isInitialIndexing={isInitialIndexing}
            isServiceStarting={isServiceStarting}
            serviceAvailable={serviceAvailable}
            dbStats={dbStats}
            loading={loading}
            activeCategory={activeCategory}
            searchMode={searchMode}
            query={query}
            snapshotQuery={snapshot.query}
            sortedResults={sortedResults}
            searchHistoryLength={searchHistory.length}
            scanElapsedSeconds={scanElapsedSeconds}
            lastColdScanDurationSec={lastColdScanDurationSec}
            actionError={actionError}
            categories={categories}
            onSelectCategory={selectCategory}
            onUpdateQuery={(tag) => {
              updateQuery(tag);
              inputRef.current?.focus();
            }}
            inputRef={inputRef}
          />
        )}

        {/* ── 单一表格视图容器（外层语义角色 + 吸顶保活表头 + 虚拟列表） ── */}
        <div
          className="search-table-shell"
          role="grid"
          aria-label={t('search.title', 'Quick file search')}
          style={{ display: resultsHidden ? 'none' : undefined }}
        >
          <SearchTableHeader
            columns={columnLayout}
            sortField={sortField}
            sortDirection={sortDirection}
            onSelectSort={handleSelectSort}
            onUpdateNameAndPathFlex={updateNameAndPathFlex}
            onUpdateTableColumnWidth={updateTableColumnWidth}
            onBatchUpdateTableColumnWidths={batchUpdateTableColumnWidths}
            onReorderTableColumns={reorderTableColumnsOrder}
            onToggleColumnVisibility={toggleColumnVisibility}
            onToggleTableColumn={toggleTableColumn}
            onRemoveTableColumn={removeTableColumn}
            onFitColumnToContent={fitColumnToContent}
            onFitAllColumnsToContent={fitAllColumnsToContent}
            onOpenAddColumns={() => {
              setPropertyPickerSource('header');
              setShowPropertyPicker(true);
            }}
            onOpenOrganizeColumns={openColumnManager}
            recentTableColumns={recentTableColumns}
            onResetColumns={resetColumns}
            headerScrollRef={headerScrollRef}
            hidden={resultsHidden}
          />

          <VirtualSearchResults
            results={sortedResults}
            selectedIndex={selectedIndex}
            density={density}
            iconStyle={iconStyle}
            iconCache={nativeIconCache}
            onRequestIcon={fetchNativeIcon}
            columns={columnLayout}
            propertyCache={propertyCache}
            queryKeywords={snapshot.keywords}
            onHover={handleRowHover}
            onSelect={handleRowSelect}
            onOpen={handleRowOpen}
            onContextMenu={handleRowContextMenu}
            onVisibleRangeChange={setVisibleRange}
            onHorizontalScroll={handleTableHorizontalScroll}
            hidden={resultsHidden}
          />
        </div>

        <SearchFooter
          loading={loading}
          activeCategory={activeCategory}
          searchMode={searchMode}
          query={query}
          scanElapsedSeconds={scanElapsedSeconds}
          scanProgress={scanProgress}
          contentScanPaused={contentScanPaused}
          onStopSearch={stopSearch}
          onResumeContentScan={resumeContentScan}
          isServiceStarting={isServiceStarting}
          isInitialIndexing={isInitialIndexing}
          dbStats={dbStats}
          sortProgress={sortProgress}
          onCancelSort={handleCancelSortExtraction}
          sortedResults={sortedResults}
          totalIndexedFiles={totalIndexedFiles}
          totalResultSize={totalResultSize}
          selectedIndex={selectedIndex}
          searchElapsedMs={searchElapsedMs}
          isRebuilding={isRebuilding}
          liveRefreshEnabled={liveRefreshEnabled}
          onToggleLiveRefresh={toggleLiveRefresh}
          onRebuildIndex={() => void rebuildIndex()}
          onOpenResult={(res) => void openResult(res)}
          onOpenFolderResult={(res) => void openFolderResult(res)}
          onCopyPathResult={(res) => copyPathResult(res)}
          onExportResultsToCsv={() => void exportResultsToCsv()}
          onToggleSyntaxHelp={() => {
            setShowSyntaxHelp(prev => !prev);
            setShowViewSettings(false);
          }}
          onHide={hide}
          onTitlebarMouseDown={handleAuxiliaryDragMouseDown}
          onTitlebarDoubleClick={handleAuxiliaryDoubleClick}
        />

        <SearchColumnManagerModal
          visible={showColumnManager}
          activeColumns={activeVisibleTableCols}
          draftColumns={columnManagerDraft ?? activeVisibleTableCols}
          onChangeDraft={(nextDraft) => {
            setColumnManagerDraft(nextDraft);
            setColumnManagerResetPending(false);
          }}
          onMarkResetDraft={() => {
            setColumnManagerDraft([...DEFAULT_VISIBLE_TABLE_COLUMNS]);
            setColumnManagerResetPending(true);
          }}
          onClose={closeColumnManager}
          onApply={applyColumnManagerDraft}
          onOpenAddPicker={(draftCols) => {
            // 仅保存草稿态，严禁提前持久化；由属性选择器返回后再汇入草稿
            setColumnManagerDraft([...draftCols]);
            setPropertyPickerSource('manager');
            setShowPropertyPicker(true);
          }}
        />

        <SearchPropertyPickerModal
          visible={showPropertyPicker}
          existingColumns={propertyPickerExistingCols}
          onClose={() => {
            setShowPropertyPicker(false);
            if (propertyPickerSource !== 'manager') {
              window.setTimeout(() => inputRef.current?.focus(), 0);
            }
          }}
          onAddColumns={(newCols) => {
            if (propertyPickerSource === 'manager') {
              setColumnManagerResetPending(false);
              setColumnManagerDraft((prev) => {
                const base = prev ? [...prev] : [...activeVisibleTableCols];
                for (const id of newCols) {
                  if (isKnownTableColumnId(id) && !base.includes(id)) {
                    base.push(id);
                  }
                }
                return base;
              });
              return;
            }
            addTableColumns(newCols);
            toast.success(
              t('search.columnsAddedToast', 'Added {{count}} column(s) to table', { count: newCols.length }),
            );
          }}
        />

        <SearchContextMenu
          contextMenu={contextMenu}
          onClose={() => {
            setContextMenu({ visible: false, x: 0, y: 0 });
            window.setTimeout(() => inputRef.current?.focus(), 0);
          }}
          onOpen={(res) => void openResult(res)}
          onOpenFolder={(res) => void openFolderResult(res)}
          onEditWithNotepad={(res) => void openWithNotepad(res)}
          onStartRename={(res) => startRename(res)}
          onPinResult={(res) => void pinResult(res)}
          onRunAsAdmin={(res) => void openResultAsAdmin(res)}
          onCopyPath={(res) => copyPathResult(res)}
          onCopyFile={(res) => void copyFileResult(res)}
          onQuickLook={(res) => void quickLookResult(res)}
          onExcludeFolder={handleExcludeFolderFromResult}
          onCopyText={(text) => copyText(text)}
          onShowProperties={(res) => void showFileProperties(res)}
        />

        <SearchRenameModal
          renameTarget={renameTarget}
          onClose={() => {
            setRenameTarget({ visible: false, newName: '' });
            window.setTimeout(() => inputRef.current?.focus(), 0);
          }}
          inputRef={renameInputRef}
          onChangeName={(name) => setRenameTarget(prev => ({ ...prev, newName: name }))}
          onConfirm={() => void confirmRename()}
        />

        {!isMaximized && (
          <>
            <div
              className="search-resize-handle"
              onMouseDown={handleResizeMouseDown}
              title={t('search.dragResizeTooltip', 'Drag to resize window')}
              aria-label={t('search.resizeAria', 'Drag to resize window')}
            >
              <svg width="10" height="10" viewBox="0 0 10 10" fill="none" aria-hidden="true" xmlns="http://www.w3.org/2000/svg">
                <line x1="8" y1="2" x2="2" y2="8" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
                <line x1="8" y1="5.5" x2="5.5" y2="8" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
              </svg>
            </div>

            <WindowResizeHandles method="search.startResize" showMaximizedCheck={false} />
          </>
        )}
      </section>
    </main>
  );
}

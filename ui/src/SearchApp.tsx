import { useCallback, useEffect, useMemo, useRef, useState, startTransition, type MouseEvent as ReactMouseEvent } from 'react';
import {
  FileSpreadsheet,
} from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { Toaster, toast } from 'sonner';
import { bridgeRequest } from './hooks/useBridge';
import { useAppearance } from './hooks/useAppearance';
import { WindowResizeHandles } from './components/WindowResizeHandles';
import { VirtualSearchResults } from './search/VirtualSearchResults';
import { SearchContextMenu } from './search/SearchContextMenu';
import { SearchRenameModal } from './search/SearchRenameModal';
import { SearchSyntaxModal } from './search/SearchSyntaxModal';
import { SearchViewSettingsModal } from './search/SearchViewSettingsModal';
import { SearchSortDropdown } from './search/SearchSortDropdown';
import { SearchHistoryBar } from './search/SearchHistoryBar';
import { SearchEmptyState } from './search/SearchEmptyState';
import { SearchFooter } from './search/SearchFooter';
import { SearchHeader } from './search/SearchHeader';
import { useSearchActions } from './search/useSearchActions';
import { useSearchKeyboard } from './search/useSearchKeyboard';
import { useSearchEngine } from './search/useSearchEngine';
import {
  CATEGORY_DEFS,
  DEFAULT_COLUMNS,
} from './search/searchConstants';
import type {
  CategoryFilter,
  ColumnId,
  ColumnLayout,
  ColumnSetting,
  DriveInfo,
  FormatCategory,
  SearchDensity,
  SearchIconStyle,
  SearchMode,
  SearchResult,
  FileFolderPriority,
  SortDirection,
  SortField,
} from './search/searchTypes';
import {
  buildSmartContentSearchQuery,
  getSortedResults,
  hasContentSyntaxInQuery,
  stripContentSyntaxFromQuery,
} from './search/searchUtils';
import './SearchApp.css';

// ── 组件主入口 ──────────────────────────────────────────────────
export default function SearchApp() {
  useAppearance();
  const { t } = useTranslation();
  const [toasterTheme, setToasterTheme] = useState<'dark' | 'light'>(() =>
    document.documentElement.getAttribute('data-theme') === 'light' ? 'light' : 'dark'
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
  const [query, setQuery] = useState('');
  const [actionError, setActionError] = useState('');
  const [showSyntaxHelp, setShowSyntaxHelp] = useState(false);
  const [syntaxCat, setSyntaxCat] = useState<string>('all');
  const [showViewSettings, setShowViewSettings] = useState(false);
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

  const inputRef = useRef<HTMLInputElement>(null);
  const [isComposing, setIsComposing] = useState(false);

  const [maxResultLimit, setMaxResultLimit] = useState<number>(() => {
    const saved = localStorage.getItem('tools3000_search_max_results');
    return saved !== null ? parseInt(saved, 10) : 0;
  });

  const changeMaxResultLimit = (limit: number) => {
    setMaxResultLimit(limit);
    localStorage.setItem('tools3000_search_max_results', String(limit));
  };

  const [excludeGitAndModules, setExcludeGitAndModules] = useState<boolean>(() => {
    return localStorage.getItem('tools3000_search_exclude_dev') !== 'false';
  });
  const [excludeHidden, setExcludeHidden] = useState<boolean>(() => {
    return localStorage.getItem('tools3000_search_exclude_hidden') === 'true';
  });
  const [isRebuilding, setIsRebuilding] = useState(false);

  const [searchMode, setSearchMode] = useState<SearchMode>(() => {
    return (localStorage.getItem('tools3000_search_default_mode') as SearchMode) || 'name';
  });
  const changeSearchMode = (mode: SearchMode) => {
    setSearchMode(mode);
    localStorage.setItem('tools3000_search_default_mode', mode);
  };

  const [disabledContentFormats, setDisabledContentFormats] = useState<string[]>(() => {
    try {
      const saved =
        localStorage.getItem('tools3000_search_disabled_formats') ??
        localStorage.getItem('tools3000_search_disabled_content_formats');
      if (saved) return JSON.parse(saved);
    } catch {
      /* ignore */
    }
    return [];
  });

  const [customContentFormats, setCustomContentFormats] = useState<string[]>(() => {
    try {
      const saved = localStorage.getItem('tools3000_search_custom_formats');
      if (saved) return JSON.parse(saved);
    } catch {
      /* ignore */
    }
    return [];
  });

  const [newFormatInput, setNewFormatInput] = useState('');

  const [systemDrives, setSystemDrives] = useState<DriveInfo[]>([]);
  const [enabledDrives, setEnabledDrives] = useState<string[]>(() => {
    try {
      const saved = localStorage.getItem('tools3000_search_enabled_drives');
      if (saved) {
        const parsed = JSON.parse(saved);
        if (Array.isArray(parsed)) return parsed;
      }
    } catch {
      /* ignore */
    }
    return [];
  });

  const activeCategory = useMemo(() => {
    if (hasContentSyntaxInQuery(query)) {
      return 'content';
    }
    const trimmed = query.trimStart().toLowerCase();
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
  }, [query]);

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
    clearResultCache,
    refreshQuery,
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
    customContentFormats,
    disabledContentFormats,
  });

  useEffect(() => {
    bridgeRequest<{ pinned: boolean }>('search.isPinned')
      .then(res => {
        if (res && typeof res.pinned === 'boolean') {
          setIsPinned(res.pinned);
        }
      })
      .catch(() => {});
  }, []);

  const togglePin = useCallback(async () => {
    const next = !isPinned;
    setIsPinned(next);
    try {
      await bridgeRequest('search.setPinned', { pinned: next });
      if (next) {
        toast.success(t('search.pinnedToast', 'Search window pinned (Keeps open when clicking outside)'));
      } else {
        toast.info(t('search.unpinnedToast', 'Search window unpinned (Auto-hides on blur)'));
      }
    } catch {
      setIsPinned(!next);
    }
  }, [isPinned, t]);

  const categories: CategoryFilter[] = useMemo(() => {
    return CATEGORY_DEFS.map(def => ({
      id: def.id,
      label: t(def.labelKey as unknown as 'search.catAll', def.defaultLabel),
      labelKey: def.labelKey,
      defaultLabel: def.defaultLabel,
      prefix: def.prefix,
      icon: def.icon
    }));
  }, [t]);

  const [searchHistory, setSearchHistory] = useState<{ search: string; searchCount: number; lastSearchDate: number }[]>([]);
  const [isInitialIndexing, setIsInitialIndexing] = useState(false);
  const isInitialIndexingRef = useRef(false);
  const [dbStats, setDbStats] = useState<{
    dbPath: string;
    dbSize: number;
    timestamp: number;
    totalRecords: number;
    volumeCount: number;
    exists: boolean;
    indexing?: boolean;
    runHistoryCount: number;
    searchHistoryCount: number;
  } | null>(null);
  
  const refreshHistory = useCallback(async () => {
    try {
      const res = await bridgeRequest<{ success: boolean; history: { search: string; searchCount: number; lastSearchDate: number }[] }>('search.getSearchHistory', { limit: 20 });
      if (res && res.success && Array.isArray(res.history)) {
        setSearchHistory(res.history);
      }
    } catch {
      // ignore
    }
  }, []);

  const checkInitialIndex = useCallback(async (): Promise<boolean> => {
    try {
      const res = await bridgeRequest<{
        dbPath: string;
        dbSize: number;
        timestamp: number;
        totalRecords: number;
        volumeCount: number;
        exists: boolean;
        indexing?: boolean;
        runHistoryCount: number;
        searchHistoryCount: number;
        success: boolean;
      }>('search.getDbStats');
      if (res && res.success) {
        setDbStats(res);
        const isIndexing = res.indexing === true || (res.totalRecords === 0 && res.exists === false);
        if (isIndexing) {
          setIsInitialIndexing(true);
          isInitialIndexingRef.current = true;
          return true;
        } else {
          if (isInitialIndexingRef.current) {
            isInitialIndexingRef.current = false;
            setIsInitialIndexing(false);
            setServiceAvailable(true);
            toast.success(t('search.indexReadyToast', 'Full disk index built and ready!'));
          } else {
            setIsInitialIndexing(false);
            setServiceAvailable(true);
          }
          return false;
        }
      }
    } catch {
      // ignore
    }
    return false;
  }, [setServiceAvailable, t]);

  const refreshDbStats = useCallback(async () => {
    await checkInitialIndex();
  }, [checkInitialIndex]);

  useEffect(() => {
    let active = true;
    let pollTimer: number | undefined;

    void bridgeRequest<{ success: boolean; history: { search: string; searchCount: number; lastSearchDate: number }[] }>('search.getSearchHistory', { limit: 20 })
      .then((res) => {
        if (active && res && res.success && Array.isArray(res.history)) {
          setSearchHistory(res.history);
        }
      })
      .catch(() => undefined);

    const runPoll = async () => {
      const isBusy = await checkInitialIndex();
      if (!active) return;
      if (isBusy) {
        pollTimer = window.setTimeout(runPoll, 500);
      }
    };

    let lastFocusSyncTick = 0;
    const onFocusEvt = () => {
      const now = Date.now();
      if (now - lastFocusSyncTick > 3000) {
        lastFocusSyncTick = now;
        // Native SearchWindow::show() is the sole service-start signal.  A
        // focus event may also occur during WebView lifecycle work, so it must
        // never start the index service on its own.
        void bridgeRequest('search.sync').catch(() => {});
      }
      if (pollTimer) window.clearTimeout(pollTimer);
      pollTimer = window.setTimeout(runPoll, 250);
      refreshOnFocus();
    };
    window.addEventListener('tools3000:focusSearch', onFocusEvt);

    return () => {
      active = false;
      if (pollTimer) window.clearTimeout(pollTimer);
      window.removeEventListener('tools3000:focusSearch', onFocusEvt);
    };
  }, [checkInitialIndex, refreshOnFocus, setTotalIndexedFiles]);

  const removeHistoryItem = async (e: React.MouseEvent, s: string) => {
    e.stopPropagation();
    await bridgeRequest('search.removeSearchHistory', { search: s });
    setSearchHistory(prev => prev.filter(item => item.search !== s));
  };

  const clearAllHistory = async () => {
    await bridgeRequest('search.clearSearchHistory');
    setSearchHistory([]);
    toast.success(t('search.historyCleared', 'Search history cleared'));
    void refreshDbStats();
    void refreshHistory();
  };

  const [sortField, setSortField] = useState<SortField>(() => {
    const saved = localStorage.getItem('tools3000_search_sort_field');
    if (saved === 'modified' || saved === 'name' || saved === 'size' || saved === 'created' || saved === 'relevance') {
      return saved;
    }
    return 'modified';
  });

  const [sortDirection, setSortDirection] = useState<SortDirection>(() => {
    const saved = localStorage.getItem('tools3000_search_sort_dir');
    if (saved === 'asc' || saved === 'desc') {
      return saved;
    }
    return 'desc';
  });

  const [fileFolderPriority, setFileFolderPriority] = useState<FileFolderPriority>(() => {
    const saved = localStorage.getItem('tools3000_search_folder_priority');
    if (saved === 'filesFirst' || saved === 'foldersFirst' || saved === 'mixed') {
      return saved;
    }
    const legacy = localStorage.getItem('tools3000_search_sort_folders_first');
    if (legacy === 'false') return 'mixed';
    return 'filesFirst'; // 默认：世界级文件优先 (优先按文件排序，然后按文件夹)
  });

  const [groupByType, setGroupByType] = useState<boolean>(() => {
    return localStorage.getItem('tools3000_search_sort_group_type') === 'true';
  });

  const [smartPathPriority, setSmartPathPriority] = useState<boolean>(() => {
    return localStorage.getItem('tools3000_search_smart_path_priority') !== 'false';
  });

  const [showSortMenu, setShowSortMenu] = useState(false);
  const sortDropdownRef = useRef<HTMLDivElement>(null);
  const viewSettingsRef = useRef<HTMLDivElement>(null);

  const [windowSize, setWindowSize] = useState<{ width: number; height: number }>({ width: 760, height: 520 });
  const [density, setDensity] = useState<SearchDensity>(() => {
    const saved = localStorage.getItem('tools3000_search_density');
    if (saved === 'compact' || saved === 'standard' || saved === 'comfortable') {
      return saved;
    }
    return 'standard';
  });

  const [iconStyle, setIconStyle] = useState<SearchIconStyle>(() => {
    const saved = localStorage.getItem('tools3000_search_icon_style');
    if (saved === 'native' || saved === 'vector') return saved;
    return 'native';
  });
  const [nativeIconCache, setNativeIconCache] = useState<Record<string, string>>({});
  const nativeIconCacheRef = useRef<Record<string, string>>({});
  const inFlightIconsRef = useRef<Set<string>>(new Set());
  const searchSettingsSigRef = useRef<string | null>(null);

  useEffect(() => {
    let active = true;
    const syncRemoteSettings = (triggerRefreshOnDiff: boolean) => {
      bridgeRequest<{
        iconStyle?: SearchIconStyle;
        pinyinEnabled?: boolean;
        matchPath?: boolean;
        caseSensitive?: boolean;
      }>('search.getSettings')
        .then((res) => {
          if (!active || !res) return;
          if (res.iconStyle === 'native' || res.iconStyle === 'vector') {
            setIconStyle(res.iconStyle);
            localStorage.setItem('tools3000_search_icon_style', res.iconStyle);
          }
          const nextSig = `${Boolean(res.pinyinEnabled ?? true)}:${Boolean(res.matchPath ?? false)}:${Boolean(res.caseSensitive ?? false)}`;
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
    window.addEventListener('tools3000:focusSearch', onSyncEvt);
    window.addEventListener('focus', onSyncEvt);
    return () => {
      active = false;
      window.removeEventListener('tools3000:focusSearch', onSyncEvt);
      window.removeEventListener('focus', onSyncEvt);
    };
  }, [refreshQuery]);

  const fetchNativeIcon = useCallback(async (ext: string, isDirectory: boolean) => {
    const key = isDirectory ? '::dir::' : ext.toLowerCase();
    if (Object.prototype.hasOwnProperty.call(nativeIconCacheRef.current, key)) {
      return nativeIconCacheRef.current[key] || undefined;
    }
    if (inFlightIconsRef.current.has(key)) return undefined;
    inFlightIconsRef.current.add(key);
    try {
      const resp = await bridgeRequest<{ success: boolean; iconBase64?: string }>('search.getFileIcon', {
        ext: isDirectory ? '' : ext,
        isDirectory,
      });
      const iconVal = resp?.success && resp.iconBase64 ? resp.iconBase64 : '';
      nativeIconCacheRef.current[key] = iconVal;
      setNativeIconCache(prev => (prev[key] === iconVal ? prev : { ...prev, [key]: iconVal }));
      return iconVal || undefined;
    } catch {
      nativeIconCacheRef.current[key] = '';
      setNativeIconCache(prev => (Object.prototype.hasOwnProperty.call(prev, key) ? prev : { ...prev, [key]: '' }));
    } finally {
      inFlightIconsRef.current.delete(key);
    }
    return undefined;
  }, []);

  const prefetchIcons = useCallback(async (list: SearchResult[]) => {
    if (iconStyle !== 'native') return;
    const missing = new Set<string>();
    const items: Array<{ ext: string; isDirectory: boolean }> = [];
    for (const item of list.slice(0, 100)) {
      const dotIdx = item.name.lastIndexOf('.');
      const ext = !item.isDirectory && dotIdx >= 0 ? item.name.slice(dotIdx).toLowerCase() : '';
      const key = item.isDirectory ? '::dir::' : ext;
      if (
        !Object.prototype.hasOwnProperty.call(nativeIconCacheRef.current, key) &&
        !inFlightIconsRef.current.has(key) &&
        !missing.has(key)
      ) {
        missing.add(key);
        items.push({ ext, isDirectory: item.isDirectory });
      }
    }
    if (items.length === 0) return;
    missing.forEach(k => inFlightIconsRef.current.add(k));
    try {
      const resp = await bridgeRequest<{ success: boolean; icons: Record<string, string> }>('search.batchGetIcons', { items });
      const returned = resp?.success && resp.icons ? resp.icons : {};
      const patch: Record<string, string> = {};
      for (const key of missing) {
        const val = returned[key] || '';
        nativeIconCacheRef.current[key] = val;
        patch[key] = val;
      }
      setNativeIconCache(prev => ({ ...prev, ...patch }));
    } catch {
      // Allow retry on transient IPC error
    } finally {
      missing.forEach(k => inFlightIconsRef.current.delete(k));
    }
  }, [iconStyle]);

  useEffect(() => {
    if (results.length > 0) {
      const timer = window.setTimeout(() => {
        void prefetchIcons(results);
      }, 0);
      return () => window.clearTimeout(timer);
    }
  }, [prefetchIcons, results]);

  const changeIconStyle = (newStyle: SearchIconStyle) => {
    setIconStyle(newStyle);
    localStorage.setItem('tools3000_search_icon_style', newStyle);
    void bridgeRequest('search.saveSettings', { iconStyle: newStyle });
  };

  const [columns, setColumns] = useState<ColumnSetting[]>(() => {
    try {
      const saved = localStorage.getItem('tools3000_search_columns_v2');
      if (saved) {
        const parsed = JSON.parse(saved) as ColumnSetting[];
        if (Array.isArray(parsed) && parsed.length > 0) {
          const merged = parsed.map(c => {
            const def = DEFAULT_COLUMNS.find(d => d.id === c.id);
            return def ? { ...def, ...c } : c;
          });
          DEFAULT_COLUMNS.forEach(def => {
            if (!merged.some(m => m.id === def.id)) {
              merged.push(def);
            }
          });
          return merged;
        }
      }
    } catch {
      // fallback
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
              localStorage.setItem('tools3000_search_enabled_drives', JSON.stringify(allLetters));
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
      let next: string[];
      if (prev.includes(letter)) {
        next = prev.filter((l) => l !== letter);
      } else {
        next = [...prev, letter];
      }
      localStorage.setItem('tools3000_search_enabled_drives', JSON.stringify(next));
      void bridgeRequest('search.saveSettings', { enabledDrives: next.join(',') }).catch(() => undefined);
      return next;
    });
  };

  const selectAllDrives = () => {
    const all = systemDrives.map((d) => d.letter);
    setEnabledDrives(all);
    localStorage.setItem('tools3000_search_enabled_drives', JSON.stringify(all));
    void bridgeRequest('search.saveSettings', { enabledDrives: all.join(',') }).catch(() => undefined);
  };

  const deselectAllDrives = () => {
    setEnabledDrives([]);
    localStorage.setItem('tools3000_search_enabled_drives', JSON.stringify([]));
    void bridgeRequest('search.saveSettings', { enabledDrives: '' }).catch(() => undefined);
  };

  const toggleContentFormat = (ext: string) => {
    const cleanExt = ext.toLowerCase().replace(/^\./, '').trim();
    setDisabledContentFormats(prev => {
      let next: string[];
      if (prev.includes(cleanExt)) {
        next = prev.filter(e => e !== cleanExt);
      } else {
        next = [...prev, cleanExt].filter(Boolean);
      }
      localStorage.setItem('tools3000_search_disabled_formats', JSON.stringify(next));
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
      localStorage.setItem('tools3000_search_disabled_formats', JSON.stringify(next));
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
      .split(/[,;\s，；、|]+/)
      .map(p => p.toLowerCase().replace(/^\./, '').trim())
      .filter(Boolean);
    if (parts.length === 0) return;
    setCustomContentFormats(prev => {
      const next = [...prev];
      for (const p of parts) {
        if (!next.includes(p)) next.push(p);
      }
      localStorage.setItem('tools3000_search_custom_formats', JSON.stringify(next));
      return next;
    });
    setDisabledContentFormats(prev => {
      const next = prev.filter(e => !parts.includes(e));
      localStorage.setItem('tools3000_search_disabled_formats', JSON.stringify(next));
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
      localStorage.setItem('tools3000_search_custom_formats', JSON.stringify(next));
      return next;
    });
    setDisabledContentFormats(prev => {
      const next = prev.filter(e => e !== cleanExt);
      localStorage.setItem('tools3000_search_disabled_formats', JSON.stringify(next));
      return next;
    });
    clearResultCache();
  };

  const resetContentFormats = () => {
    setDisabledContentFormats([]);
    setCustomContentFormats([]);
    localStorage.removeItem('tools3000_search_disabled_formats');
    localStorage.removeItem('tools3000_search_disabled_content_formats');
    localStorage.removeItem('tools3000_search_custom_formats');
    toast.success(t('search.customExtResetToast', 'Content search format configuration reset to default'));
  };

  const rebuildIndex = useCallback(async () => {
    setIsRebuilding(true);
    toast.loading(t('search.rebuildingToast', 'Rescanning NTFS index and saving snapshot...'), { id: 'rebuild-idx' });
    try {
      await bridgeRequest('search.rebuildIndex');
      toast.success(t('search.rebuildSuccessToast', 'Full-disk index rescanned and saved to snapshot!'), { id: 'rebuild-idx' });
      clearResultCache();
      refreshQuery();
      void refreshDbStats();
      void refreshHistory();
    } catch {
      toast.error(t('search.rebuildError', 'Index rebuild and snapshot save failed'), { id: 'rebuild-idx' });
    } finally {
      setTimeout(() => setIsRebuilding(false), 1500);
    }
  }, [clearResultCache, refreshDbStats, refreshHistory, refreshQuery, t]);

  useEffect(() => {
    void bridgeRequest<{ width?: number; height?: number }>('search.getWindowSize')
      .then((res) => {
        if (res?.width && res?.height) {
          setWindowSize({ width: res.width, height: res.height });
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

  const toggleColumnVisibility = (id: ColumnId) => {
    const updated = columns.map(col => col.id === id ? { ...col, visible: !col.visible } : col);
    setColumns(updated);
    localStorage.setItem('tools3000_search_columns_v2', JSON.stringify(updated));
  };

  const updateNameAndPathFlex = (nameVal: number) => {
    const pathVal = Math.max(15, 80 - nameVal);
    const updated = columns.map(col => {
      if (col.id === 'name') return { ...col, flex: nameVal };
      if (col.id === 'path') return { ...col, flex: pathVal };
      return col;
    });
    setColumns(updated);
    localStorage.setItem('tools3000_search_columns_v2', JSON.stringify(updated));
  };

  const resetColumns = () => {
    setColumns(DEFAULT_COLUMNS);
    localStorage.setItem('tools3000_search_columns_v2', JSON.stringify(DEFAULT_COLUMNS));
    toast.success(t('search.toastResetLayout', 'Default columns and layout restored'));
  };

  const changeDensity = (newDensity: SearchDensity) => {
    setDensity(newDensity);
    localStorage.setItem('tools3000_search_density', newDensity);
    inputRef.current?.focus();
  };

  useEffect(() => {
    const doFocus = () => {
      if (inputRef.current) {
        inputRef.current.focus({ preventScroll: true });
        inputRef.current.select();
      }
    };
    doFocus();
    const t1 = setTimeout(doFocus, 30);
    const t2 = setTimeout(doFocus, 100);
    const t3 = setTimeout(doFocus, 250);
    const t4 = setTimeout(doFocus, 500);

    const onFocusEvt = () => {
      doFocus();
    };
    window.addEventListener('tools3000:focusSearch', onFocusEvt);

    const handleGlobalClick = (e: MouseEvent) => {
      const target = e.target as HTMLElement | null;
      if (!target?.closest('.search-context-menu')) {
        setContextMenu({ visible: false, x: 0, y: 0 });
      }
    };
    window.addEventListener('click', handleGlobalClick);
    window.addEventListener('pointerdown', handleGlobalClick);

    return () => {
      clearTimeout(t1);
      clearTimeout(t2);
      clearTimeout(t3);
      clearTimeout(t4);
      window.removeEventListener('tools3000:focusSearch', onFocusEvt);
      window.removeEventListener('click', handleGlobalClick);
      window.removeEventListener('pointerdown', handleGlobalClick);
    };
  }, []);

  const handleSetFileFolderPriority = (priority: FileFolderPriority) => {
    setFileFolderPriority(priority);
    localStorage.setItem('tools3000_search_folder_priority', priority);
    localStorage.setItem('tools3000_search_sort_folders_first', String(priority === 'foldersFirst'));
  };

  const toggleFoldersFirst = () => {
    handleSetFileFolderPriority(fileFolderPriority === 'foldersFirst' ? 'filesFirst' : 'foldersFirst');
  };

  const toggleGroupByType = () => {
    setGroupByType(prev => {
      const next = !prev;
      localStorage.setItem('tools3000_search_sort_group_type', String(next));
      return next;
    });
  };

  const toggleSmartPathPriority = () => {
    setSmartPathPriority(prev => {
      const next = !prev;
      localStorage.setItem('tools3000_search_smart_path_priority', String(next));
      return next;
    });
  };

  useEffect(() => {
    const handleClickOutside = (e: MouseEvent) => {
      const target = e.target as Node;
      if (sortDropdownRef.current && !sortDropdownRef.current.contains(target)) {
        setShowSortMenu(false);
      }
      if (viewSettingsRef.current && !viewSettingsRef.current.contains(target)) {
        const helpBtn = document.querySelector('.search-help-btn');
        if (!helpBtn || !helpBtn.contains(target)) {
          setShowViewSettings(false);
        }
      }
    };
    if (showSortMenu || showViewSettings) {
      window.addEventListener('mousedown', handleClickOutside);
      return () => window.removeEventListener('mousedown', handleClickOutside);
    }
  }, [showSortMenu, showViewSettings]);

  const sortedResults = useMemo(() => {
    return getSortedResults(snapshot.results, sortField, sortDirection, fileFolderPriority, groupByType, smartPathPriority);
  }, [snapshot.results, sortField, sortDirection, fileFolderPriority, groupByType, smartPathPriority]);

  const totalResultSize = useMemo(() => {
    return sortedResults.reduce((acc, item) => acc + (item.isDirectory ? 0 : (Number(item.size) || 0)), 0);
  }, [sortedResults]);

  const handleSetSortDirect = (field: SortField, dir: SortDirection) => {
    setSortField(field);
    setSortDirection(dir);
    localStorage.setItem('tools3000_search_sort_field', field);
    localStorage.setItem('tools3000_search_sort_dir', dir);
    setSelectedIndex(0);
    setShowSortMenu(false);
    inputRef.current?.focus();
  };

  const toggleSortDirection = (e?: React.MouseEvent) => {
    if (e) e.stopPropagation();
    if (sortField === 'relevance') {
      setSortField('modified');
      setSortDirection('desc');
      localStorage.setItem('tools3000_search_sort_field', 'modified');
      localStorage.setItem('tools3000_search_sort_dir', 'desc');
    } else {
      const nextDir = sortDirection === 'desc' ? 'asc' : 'desc';
      setSortDirection(nextDir);
      localStorage.setItem('tools3000_search_sort_dir', nextDir);
    }
    setSelectedIndex(0);
    inputRef.current?.focus();
  };

  const handleSelectSort = useCallback((field: SortField) => {
    if (field === 'relevance') {
      setSortField('relevance');
      localStorage.setItem('tools3000_search_sort_field', 'relevance');
    } else if (sortField === field) {
      const nextDir = sortDirection === 'desc' ? 'asc' : 'desc';
      setSortDirection(nextDir);
      localStorage.setItem('tools3000_search_sort_dir', nextDir);
    } else {
      setSortField(field);
      const defaultDir = (field === 'name' ? 'asc' : 'desc');
      setSortDirection(defaultDir);
      localStorage.setItem('tools3000_search_sort_field', field);
      localStorage.setItem('tools3000_search_sort_dir', defaultDir);
    }
    setSelectedIndex(0);
    setShowSortMenu(false);
    inputRef.current?.focus();
  }, [sortField, sortDirection, setSelectedIndex]);

  const updateQuery = useCallback((next: string) => {
    setQuery(next);
    setActionError('');
    if (!next.trim()) {
      setIsServiceStarting(false);
    }

    const lower = next.toLowerCase();
    if (lower.includes('sort:date') || lower.includes('sort:mtime') || lower.includes('sort:time')) {
      setSortField('modified');
      setSortDirection('desc');
    } else if (lower.includes('sort:name')) {
      setSortField('name');
      setSortDirection('asc');
    } else if (lower.includes('sort:size')) {
      setSortField('size');
      setSortDirection('desc');
    } else if (lower.includes('sort:created')) {
      setSortField('created');
      setSortDirection('desc');
    } else if (lower.includes('sort:file')) {
      handleSetFileFolderPriority('filesFirst');
    } else if (lower.includes('sort:folder') || lower.includes('sort:dir')) {
      handleSetFileFolderPriority('foldersFirst');
    } else if (lower.includes('sort:ext') || lower.includes('sort:type')) {
      setGroupByType(true);
    }

    if (!next.trim()) {
      setResults([]);
      setSelectedIndex(0);
      setLoading(false);
    }
  }, [setIsServiceStarting, setLoading, setResults, setSelectedIndex]);

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

    let cleaned = stripContentSyntaxFromQuery(query);
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

  const hide = useCallback(() => {
    void bridgeRequest('search.hide').catch(() => undefined);
  }, []);

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
    hide,
    setActionError,
    setContextMenu,
    renameTarget,
    setRenameTarget,
    renameInputRef,
    setResults,
    query,
    sortedResults,
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
    showSortMenu,
    setShowSortMenu,
    showSyntaxHelp,
    setShowSyntaxHelp,
    showViewSettings,
    setShowViewSettings,
    hide,
    rebuildIndex,
    togglePin,
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

  const columnLayout = useMemo<ColumnLayout>(() => {
    const visible = (id: ColumnId) => columns.find(c => c.id === id)?.visible ?? true;
    return {
      name: visible('name'),
      ext: visible('ext'),
      parent: visible('parent'),
      path: visible('path'),
      size: visible('size'),
      modified: visible('modified'),
      created: visible('created'),
      snippets: visible('snippets'),
      nameFlex: columns.find(c => c.id === 'name')?.flex ?? 35,
      pathFlex: columns.find(c => c.id === 'path')?.flex ?? 45,
    };
  }, [columns]);

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
      // 延迟 20ms 确保 Chromium 彻底完成 preventDefault 并释放底层鼠标捕获
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
    const menuWidth = 240;
    const menuHeight = 310;
    const x = Math.min(event.clientX, window.innerWidth - menuWidth - 10);
    const y = Math.min(event.clientY, window.innerHeight - menuHeight - 10);
    setContextMenu({
      visible: true,
      x: Math.max(10, x),
      y: Math.max(10, y),
      result,
    });
  }, [setSelectedIndex]);

  return (
    <main className={`search-app ${showViewSettings ? 'search-app--view-settings-open' : ''}`}>
      <Toaster position="bottom-right" theme={toasterTheme} richColors />
      <section className="search-container" aria-label={t('search.title', 'Quick file search')}>
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
          showViewSettings={showViewSettings}
          onToggleViewSettings={() => {
            setShowViewSettings(prev => !prev);
            setShowSyntaxHelp(false);
            setShowSortMenu(false);
          }}
          showSyntaxHelp={showSyntaxHelp}
          onToggleSyntaxHelp={() => {
            setShowSyntaxHelp(prev => !prev);
            setShowViewSettings(false);
            setShowSortMenu(false);
          }}
          onResetPlacement={() => {
            void bridgeRequest('search.resetPlacement');
            setWindowSize({ width: 760, height: 520 });
            toast.success(t('search.resetPlacementToast', 'Reset to default center and 760×520 size'));
          }}
          resultsCount={results.length}
          hasSelectedResult={!!sortedResults[selectedIndex]}
          selectedIndex={selectedIndex}
          setIsComposing={setIsComposing}
        />

        <div className="search-categories-bar">
          <div className="search-categories">
            {categories.map(cat => (
              <button
                key={cat.id}
                className={`category-pill ${activeCategory === cat.id ? 'category-pill--active' : ''}`}
                onClick={() => selectCategory(cat)}
                type="button"
              >
                {cat.icon && <cat.icon size={13} className="category-pill-icon" />}
                <span>{cat.label}</span>
              </button>
            ))}
          </div>

          <div className="search-bar-right-group">
            {/* 全盘已索引总数与耗时统计胶囊 */}
            <div 
              className="search-stats-pill" 
              title={t('search.statsPillTip', 'Indexed {{total}} files across all disks, latest search elapsed {{ms}}ms', { total: totalIndexedFiles ? totalIndexedFiles.toLocaleString() : '1,000,000+', ms: searchElapsedMs })}
            >
              <span className={`search-stats-dot ${loading ? 'search-stats-dot--scanning' : ''}`} />
              {loading && sortedResults.length > 0 ? (
                <span>
                  <strong>{sortedResults.length}+</strong> · {t('search.searchingElapsed', 'Searching... {{elapsed}}s', { elapsed: scanElapsedSeconds.toFixed(1) })}
                </span>
              ) : loading && scanElapsedSeconds > 0 ? (
                <span><strong>{t('search.searchingElapsed', 'Searching... {{elapsed}}s', { elapsed: scanElapsedSeconds.toFixed(1) })}</strong></span>
              ) : sortedResults.length > 0 ? (
                <span><strong>{sortedResults.length}</strong> / {t('search.statsFullDisk', 'Total {{total}} · {{ms}}ms', { total: totalIndexedFiles ? (totalIndexedFiles > 10000 ? (totalIndexedFiles / 10000).toFixed(1) + 'w' : totalIndexedFiles.toString()) : '--', ms: searchElapsedMs })}</span>
              ) : (
                <span>{t('search.totalFilesCount', 'Total {{count}} files', { count: totalIndexedFiles || 0 })}</span>
              )}
            </div>

            {/* 导出当前搜索结果为 CSV */}
            {sortedResults.length > 0 && (
              <button
                type="button"
                className="search-action-pill-btn"
                onClick={exportResultsToCsv}
                title={t('search.exportCsvTip', 'Export search results to CSV / Excel (Ctrl+E) - {{count}} items', { count: sortedResults.length })}
              >
                <FileSpreadsheet size={13} />
                <span>{t('search.exportCsv', 'Export CSV')} ({sortedResults.length})</span>
              </button>
            )}

            {/* ── 极客排序双区域分裂微胶囊 (Split Pill) ── */}
            <SearchSortDropdown
              sortDropdownRef={sortDropdownRef}
              showSortMenu={showSortMenu}
              onToggleSortMenu={() => {
                setShowSortMenu(prev => !prev);
                setShowViewSettings(false);
                setShowSyntaxHelp(false);
              }}
              sortField={sortField}
              sortDirection={sortDirection}
              onToggleSortDirection={toggleSortDirection}
              onSetSortDirect={handleSetSortDirect}
              folderPriority={fileFolderPriority}
              onSetFolderPriority={handleSetFileFolderPriority}
              foldersFirst={fileFolderPriority === 'foldersFirst'}
              onToggleFoldersFirst={toggleFoldersFirst}
              groupByType={groupByType}
              onToggleGroupByType={toggleGroupByType}
              smartPathPriority={smartPathPriority}
              onToggleSmartPathPriority={toggleSmartPathPriority}
            />
          </div>
        </div>

        {/* ── 历史搜索词极客气泡条 ── */}
        <SearchHistoryBar
          query={query}
          searchHistory={searchHistory}
          onClearAllHistory={clearAllHistory}
          onSelectHistoryItem={(text) => {
            updateQuery(text);
            inputRef.current?.focus();
          }}
          onRemoveHistoryItem={(e, text) => void removeHistoryItem(e, text)}
        />

        {/* ── 视图与搜索偏好设置弹窗 ── */}
        <SearchViewSettingsModal
          visible={showViewSettings}
          onClose={() => setShowViewSettings(false)}
          popoverRef={viewSettingsRef}
          isRebuilding={isRebuilding}
          onRebuildIndex={rebuildIndex}
          searchMode={searchMode}
          onChangeSearchMode={changeSearchMode}
          density={density}
          onChangeDensity={changeDensity}
          iconStyle={iconStyle}
          onChangeIconStyle={changeIconStyle}
          maxResultLimit={maxResultLimit}
          onChangeMaxResultLimit={changeMaxResultLimit}
          windowSize={windowSize}
          onSetWindowSize={setWindowSize}
          columns={columns}
          onToggleColumn={toggleColumnVisibility}
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
            localStorage.setItem('tools3000_search_exclude_dev', String(val));
          }}
          excludeHidden={excludeHidden}
          onSetExcludeHidden={(val) => {
            setExcludeHidden(val);
            localStorage.setItem('tools3000_search_exclude_hidden', String(val));
          }}
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
            setDensity('standard');
            setIconStyle('native');
            setMaxResultLimit(0);
            localStorage.removeItem('tools3000_search_density');
            localStorage.removeItem('tools3000_search_icon_style');
            void bridgeRequest('search.saveSettings', { iconStyle: 'native' }).catch(() => {});
            localStorage.removeItem('tools3000_search_max_results');
            toast.success(t('search.toastResetAllPref', 'All view and search preferences reset to defaults'));
          }}
        />

        {/* ── 语法帮助快速指南弹窗 ── */}
        <SearchSyntaxModal
          visible={showSyntaxHelp}
          onClose={() => setShowSyntaxHelp(false)}
          syntaxCat={syntaxCat}
          onSelectCategory={setSyntaxCat}
          onApplyExample={applySyntaxExample}
        />

        {/* ── 状态与空结果显示 ── */}
        <SearchEmptyState
          isInitialIndexing={isInitialIndexing}
          isServiceStarting={isServiceStarting}
          serviceAvailable={serviceAvailable}
          loading={loading}
          activeCategory={activeCategory}
          searchMode={searchMode}
          query={query}
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

        {/* ── 虚拟列表结果展示 (常驻单例稳定挂载) ── */}
        <VirtualSearchResults
          results={sortedResults}
          selectedIndex={selectedIndex}
          density={density}
          iconStyle={iconStyle}
          iconCache={nativeIconCache}
          onRequestIcon={fetchNativeIcon}
          columns={columnLayout}
          queryKeywords={snapshot.keywords}
          onHover={handleRowHover}
          onSelect={handleRowSelect}
          onOpen={handleRowOpen}
          onContextMenu={handleRowContextMenu}
          hidden={
            isInitialIndexing ||
            isServiceStarting ||
            sortedResults.length === 0
          }
        />

        <SearchFooter
          loading={loading}
          activeCategory={activeCategory}
          searchMode={searchMode}
          query={query}
          scanElapsedSeconds={scanElapsedSeconds}
          isServiceStarting={isServiceStarting}
          isInitialIndexing={isInitialIndexing}
          sortedResults={sortedResults}
          totalIndexedFiles={totalIndexedFiles}
          totalResultSize={totalResultSize}
          selectedIndex={selectedIndex}
          searchElapsedMs={searchElapsedMs}
          isRebuilding={isRebuilding}
          onRebuildIndex={rebuildIndex}
          onOpenResult={(res) => void openResult(res)}
          onOpenFolderResult={(res) => void openFolderResult(res)}
          onCopyPathResult={(res) => copyPathResult(res)}
          onExportResultsToCsv={exportResultsToCsv}
          onToggleSyntaxHelp={() => {
            setShowSyntaxHelp(prev => !prev);
            setShowViewSettings(false);
          }}
          onHide={hide}
        />

        <SearchContextMenu
          contextMenu={contextMenu}
          onClose={() => setContextMenu({ visible: false, x: 0, y: 0 })}
          onOpen={(res) => void openResult(res)}
          onOpenFolder={(res) => void openFolderResult(res)}
          onEditWithNotepad={(res) => void openWithNotepad(res)}
          onStartRename={(res) => startRename(res)}
          onPinResult={(res) => void pinResult(res)}
          onRunAsAdmin={(res) => void openResultAsAdmin(res)}
          onCopyPath={(res) => copyPathResult(res)}
          onCopyFile={(res) => void copyFileResult(res)}
          onQuickLook={(res) => void quickLookResult(res)}
          onCopyText={(text) => copyText(text)}
          onShowProperties={(res) => void showFileProperties(res)}
        />

        <SearchRenameModal
          renameTarget={renameTarget}
          onClose={() => setRenameTarget({ visible: false, newName: '' })}
          inputRef={renameInputRef}
          onChangeName={(name) => setRenameTarget(prev => ({ ...prev, newName: name }))}
          onConfirm={() => void confirmRename()}
        />

        <div
          className="search-resize-handle"
          onMouseDown={handleResizeMouseDown}
          title={t('search.dragResizeTooltip', 'Drag to resize window')}
          aria-label={t('search.resizeAria', 'Drag to resize window')}
        >
          <svg width="10" height="10" viewBox="0 0 10 10" fill="none" xmlns="http://www.w3.org/2000/svg">
            <line x1="8" y1="2" x2="2" y2="8" stroke="rgba(255,255,255,0.4)" strokeWidth="1.5" strokeLinecap="round" />
            <line x1="8" y1="5.5" x2="5.5" y2="8" stroke="rgba(255,255,255,0.4)" strokeWidth="1.5" strokeLinecap="round" />
          </svg>
        </div>

        <WindowResizeHandles method="search.startResize" showMaximizedCheck={false} />
      </section>
    </main>
  );
}

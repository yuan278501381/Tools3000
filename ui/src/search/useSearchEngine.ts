import {
  startTransition,
  useCallback,
  useEffect,
  useRef,
  useState,
  type Dispatch,
  type SetStateAction,
} from 'react';
import { bridgeRequest } from '../hooks/useBridge';
import { isEmptyContentSyntax, nextQueryId, resolveDebounceMs } from '../searchScheduling';
import type { SearchMode, SearchResponse, SearchResult, SearchSnapshot } from './searchTypes';
import { isEffectiveContentSearch, parseQueryKeywords } from './searchUtils';

interface UseSearchEngineOptions {
  query: string;
  isComposing: boolean;
  activeCategory: string;
  searchMode: SearchMode;
  enabledDrives: string[];
  maxResultLimit: number;
  excludeGitAndModules: boolean;
  excludeHidden: boolean;
  customContentFormats: string[];
  disabledContentFormats: string[];
}

interface UseSearchEngineResult {
  snapshot: SearchSnapshot;
  results: SearchResult[];
  setResults: Dispatch<SetStateAction<SearchResult[]>>;
  selectedIndex: number;
  setSelectedIndex: Dispatch<SetStateAction<number>>;
  loading: boolean;
  setLoading: Dispatch<SetStateAction<boolean>>;
  serviceAvailable: boolean;
  setServiceAvailable: Dispatch<SetStateAction<boolean>>;
  isServiceStarting: boolean;
  setIsServiceStarting: Dispatch<SetStateAction<boolean>>;
  totalIndexedFiles: number;
  setTotalIndexedFiles: Dispatch<SetStateAction<number>>;
  searchElapsedMs: number;
  scanElapsedSeconds: number;
  lastColdScanDurationSec: number;
  clearResultCache: () => void;
  refreshQuery: () => void;
  refreshOnFocus: () => void;
}

interface CachedQuery {
  results: SearchResult[];
  elapsedMs: number;
  totalIndexedFiles?: number;
  timestamp: number;
}

const CONTENT_CACHE_TTL_MS = 45_000;
const DEFAULT_CONTENT_RESULT_LIMIT = 500;
const NAME_FOCUS_REFRESH_COOLDOWN_MS = 2_500;
const MAX_RESULT_CACHE_ENTRIES = 32;

function normalizeResultDedupKey(rawPath: string): string {
  return rawPath.replace(/\//g, '\\').toLowerCase();
}

/**
 * Owns the asynchronous search lifecycle: debounce, progressive time-sliced
 * content streaming, stale-response rejection, focus-safe soft refresh,
 * service-start retries, short-lived heavy-query cache and scan timing.
 */
export function useSearchEngine({
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
}: UseSearchEngineOptions): UseSearchEngineResult {
  const [snapshot, setSnapshot] = useState<SearchSnapshot>({
    generationId: 0,
    query: '',
    keywords: [],
    results: [],
  });

  const setResults: Dispatch<SetStateAction<SearchResult[]>> = useCallback((action) => {
    setSnapshot((prev) => {
      const nextResults = typeof action === 'function' ? action(prev.results) : action;
      return {
        ...prev,
        results: nextResults,
      };
    });
  }, []);

  const [selectedIndex, setSelectedIndex] = useState(0);
  const [loading, setLoading] = useState(false);
  const [serviceAvailable, setServiceAvailable] = useState(true);
  const [isServiceStarting, setIsServiceStarting] = useState(false);
  const [totalIndexedFiles, setTotalIndexedFiles] = useState(0);
  const [searchElapsedMs, setSearchElapsedMs] = useState(0);
  const [scanElapsedSeconds, setScanElapsedSeconds] = useState(0);
  const [lastColdScanDurationSec, setLastColdScanDurationSec] = useState(() => {
    const saved = Number.parseFloat(localStorage.getItem('tools3000_last_cold_content_scan_sec') ?? '');
    return Number.isFinite(saved) ? Math.max(3, saved) : 14.5;
  });

  const requestSequenceRef = useRef(0);
  const retryCountRef = useRef(0);
  const retryTimerRef = useRef<number | null>(null);
  const scanTimerRef = useRef<{ sequence: number; intervalId: number; startTime: number } | null>(null);
  const resultCacheRef = useRef<Map<string, CachedQuery>>(new Map());
  const isRunningRef = useRef(false);
  const isContentModeRef = useRef(false);
  const lastCompletedAtRef = useRef(0);
  const hasResultsRef = useRef(false);
  const [refreshCounter, setRefreshCounter] = useState(0);
  const refreshRequestedRef = useRef(false);

  useEffect(() => {
    hasResultsRef.current = snapshot.results.length > 0;
  }, [snapshot.results.length]);

  const clearResultCache = useCallback(() => {
    resultCacheRef.current.clear();
  }, []);

  const refreshQuery = useCallback(() => {
    clearResultCache();
    refreshRequestedRef.current = true;
    setRefreshCounter((c) => c + 1);
  }, [clearResultCache]);

  const refreshOnFocus = useCallback(() => {
    // 正在执行扫描（尤其是渐进式全文扫描）时，窗口获焦绝不打断在途任务
    if (isRunningRef.current) return;
    const now = Date.now();
    if (isContentModeRef.current) {
      if (hasResultsRef.current || now - lastCompletedAtRef.current < CONTENT_CACHE_TTL_MS) {
        return;
      }
    } else if (now - lastCompletedAtRef.current < NAME_FOCUS_REFRESH_COOLDOWN_MS) {
      return;
    }
    refreshRequestedRef.current = true;
    setRefreshCounter((c) => c + 1);
  }, []);

  const stopScanStopwatch = useCallback((targetSequence?: number) => {
    const timer = scanTimerRef.current;
    if (!timer || (targetSequence !== undefined && timer.sequence !== targetSequence)) return;
    window.clearInterval(timer.intervalId);
    scanTimerRef.current = null;
  }, []);

  const startScanStopwatch = useCallback((sequence: number) => {
    stopScanStopwatch();
    setScanElapsedSeconds(0);
    const startTime = Date.now();
    const intervalId = window.setInterval(() => {
      if (scanTimerRef.current?.sequence === sequence) {
        setScanElapsedSeconds(Number(((Date.now() - startTime) / 1000).toFixed(1)));
      }
    }, 100);
    scanTimerRef.current = { sequence, intervalId, startTime };
  }, [stopScanStopwatch]);

  useEffect(() => () => {
    stopScanStopwatch();
    if (retryTimerRef.current !== null) window.clearTimeout(retryTimerRef.current);
    isRunningRef.current = false;
    requestSequenceRef.current += 1;
  }, [stopScanStopwatch]);

  useEffect(() => {
    const trimmedQuery = query.trim();
    const effectiveModeForCheck: SearchMode = activeCategory === 'content' ? 'content' : searchMode;
    isContentModeRef.current = Boolean(
      trimmedQuery &&
      isEffectiveContentSearch({
        query: trimmedQuery,
        activeCategory,
        searchMode: effectiveModeForCheck,
      }),
    );

    if (!trimmedQuery) {
      isRunningRef.current = false;
      stopScanStopwatch();
      if (retryTimerRef.current !== null) {
        window.clearTimeout(retryTimerRef.current);
        retryTimerRef.current = null;
      }
      startTransition(() => {
        setSnapshot({
          generationId: ++requestSequenceRef.current,
          query: '',
          keywords: [],
          results: [],
        });
        setLoading(false);
      });
      return;
    }
    if (isComposing) {
      if (retryTimerRef.current !== null) {
        window.clearTimeout(retryTimerRef.current);
        retryTimerRef.current = null;
      }
      return;
    }

    const sequence = ++requestSequenceRef.current;
    const isRefresh = refreshRequestedRef.current;
    refreshRequestedRef.current = false;
    const debounceMs = isRefresh ? 0 : resolveDebounceMs({ query: trimmedQuery, activeCategory, searchMode });

    const runQuery = async () => {
      if (sequence !== requestSequenceRef.current) return;
      if (isEmptyContentSyntax(trimmedQuery)) {
        isRunningRef.current = false;
        stopScanStopwatch(sequence);
        startTransition(() => {
          setSnapshot({
            generationId: sequence,
            query: trimmedQuery,
            keywords: [],
            results: [],
          });
        });
        setLoading(false);
        return;
      }

      const effectiveMode: SearchMode = activeCategory === 'content' ? 'content' : searchMode;
      const isContentMode = isEffectiveContentSearch({
        query: trimmedQuery,
        activeCategory,
        searchMode: effectiveMode,
      });
      const cacheKey = isContentMode
        ? [
            effectiveMode,
            trimmedQuery,
            enabledDrives.slice().sort().join(','),
            maxResultLimit,
            excludeGitAndModules ? 'exDev' : 'all',
            excludeHidden ? 'exHidden' : 'showHidden',
            disabledContentFormats.slice().sort().join(','),
            customContentFormats.slice().sort().join(','),
          ].join(':')
        : '';

      const cached = isContentMode && cacheKey ? resultCacheRef.current.get(cacheKey) : undefined;
      if (cached && Date.now() - cached.timestamp < CONTENT_CACHE_TTL_MS) {
        isRunningRef.current = false;
        lastCompletedAtRef.current = Date.now();
        startTransition(() => {
          setSnapshot({
            generationId: sequence,
            query: trimmedQuery,
            keywords: parseQueryKeywords(trimmedQuery),
            results: cached.results,
          });
          setSelectedIndex(0);
          setSearchElapsedMs(cached.elapsedMs);
          if (cached.totalIndexedFiles !== undefined) setTotalIndexedFiles(cached.totalIndexedFiles);
          setLoading(false);
        });
        return;
      }
      if (cached && cacheKey) resultCacheRef.current.delete(cacheKey);

      isRunningRef.current = true;
      const loadingTimer = window.setTimeout(() => {
        if (sequence === requestSequenceRef.current) setLoading(true);
      }, 80);
      startScanStopwatch(sequence);

      const excludes = excludeGitAndModules
        ? ['$Recycle.Bin', 'System Volume Information', 'node_modules', '.git', '__pycache__', 'npm-cache', 'go-build', '.gradle', 'pip\\cache']
        : [];

      const parsedKeywords = parseQueryKeywords(trimmedQuery);
      let keepRetrying = false;

      try {
        if (!isContentMode) {
          const response = await bridgeRequest<SearchResponse>('search.query', {
            query: trimmedQuery,
            queryId: nextQueryId(),
            searchMode: effectiveMode,
            limit: maxResultLimit,
            drives: enabledDrives.length > 0 ? enabledDrives : undefined,
            excludes: excludes.length > 0 ? excludes : undefined,
            excludeHidden,
            contentCustomExts: customContentFormats,
            contentDisabledExts: disabledContentFormats,
          });

          if (response?.cancelled || sequence !== requestSequenceRef.current) return;
          window.clearTimeout(loadingTimer);

          if (response.status === 'busy' || response.busy) {
            startTransition(() => {
              setIsServiceStarting(false);
              setServiceAvailable(true);
            });
            if (retryCountRef.current < 10) {
              retryCountRef.current += 1;
              keepRetrying = true;
              const retryDelay = Math.min(180 + retryCountRef.current * 80, 800);
              retryTimerRef.current = window.setTimeout(() => void runQuery(), retryDelay);
            }
            return;
          }

          if (response.status === 'starting' || (!response.available && response.status !== 'unavailable')) {
            startTransition(() => {
              setIsServiceStarting(true);
              setServiceAvailable(true);
            });
            if (retryCountRef.current < 25) {
              retryCountRef.current += 1;
              keepRetrying = true;
              const retryDelay = Math.min(250 + retryCountRef.current * 100, 1000);
              retryTimerRef.current = window.setTimeout(() => void runQuery(), retryDelay);
            } else {
              startTransition(() => {
                setIsServiceStarting(false);
                setServiceAvailable(false);
              });
            }
            return;
          }

          retryCountRef.current = 0;
          lastCompletedAtRef.current = Date.now();
          startTransition(() => {
            setIsServiceStarting(false);
            setSnapshot({
              generationId: sequence,
              query: trimmedQuery,
              keywords: parsedKeywords,
              results: Array.isArray(response.results) ? response.results : [],
            });
            setSelectedIndex((prev) => {
              const len = Array.isArray(response.results) ? response.results.length : 0;
              return len === 0 ? 0 : Math.min(prev, 0);
            });
            if (response.totalIndexedFiles !== undefined) setTotalIndexedFiles(response.totalIndexedFiles);
            if (response.elapsedMs !== undefined) setSearchElapsedMs(response.elapsedMs);
            if (response.available !== undefined) setServiceAvailable(response.available);
          });
          return;
        }

        // 渐进式内容检索管线：首拍 ~160ms 即刻上屏活跃工作区命中，后台时间片流式续扫补全
        const targetLimit = maxResultLimit > 0 ? maxResultLimit : DEFAULT_CONTENT_RESULT_LIMIT;
        const accumulated: SearchResult[] = [];
        const seenPaths = new Set<string>();
        let cursor = 0;
        let planId = 0;
        let totalElapsedMs = 0;
        let latestIndexedFiles: number | undefined;
        let firstPainted = false;
        let sliceRetries = 0;

        while (sequence === requestSequenceRef.current) {
          const remaining = Math.max(1, targetLimit - accumulated.length);
          const response = await bridgeRequest<SearchResponse>('search.query', {
            query: trimmedQuery,
            queryId: nextQueryId(),
            searchMode: effectiveMode,
            limit: remaining,
            progressive: true,
            cursor,
            planId: planId > 0 ? planId : undefined,
            drives: enabledDrives.length > 0 ? enabledDrives : undefined,
            excludes: excludes.length > 0 ? excludes : undefined,
            excludeHidden,
            contentCustomExts: customContentFormats,
            contentDisabledExts: disabledContentFormats,
          });

          if (response?.cancelled || sequence !== requestSequenceRef.current) return;

          if (response.status === 'busy' || response.busy) {
            if (cursor === 0 && accumulated.length === 0) {
              window.clearTimeout(loadingTimer);
              startTransition(() => {
                setIsServiceStarting(false);
                setServiceAvailable(true);
              });
              if (retryCountRef.current < 10) {
                retryCountRef.current += 1;
                keepRetrying = true;
                const retryDelay = Math.min(180 + retryCountRef.current * 80, 800);
                retryTimerRef.current = window.setTimeout(() => void runQuery(), retryDelay);
              }
              return;
            }
            if (sliceRetries < 5) {
              sliceRetries += 1;
              await new Promise<void>((resolve) => {
                window.setTimeout(resolve, 140 * sliceRetries);
              });
              continue;
            }
            break;
          }

          if (response.status === 'starting' || (!response.available && response.status !== 'unavailable')) {
            if (cursor === 0 && accumulated.length === 0) {
              window.clearTimeout(loadingTimer);
              startTransition(() => {
                setIsServiceStarting(true);
                setServiceAvailable(true);
              });
              if (retryCountRef.current < 25) {
                retryCountRef.current += 1;
                keepRetrying = true;
                const retryDelay = Math.min(250 + retryCountRef.current * 100, 1000);
                retryTimerRef.current = window.setTimeout(() => void runQuery(), retryDelay);
              } else {
                startTransition(() => {
                  setIsServiceStarting(false);
                  setServiceAvailable(false);
                });
              }
              return;
            }
            break;
          }

          retryCountRef.current = 0;
          sliceRetries = 0;

          let addedInSlice = 0;
          if (Array.isArray(response.results)) {
            for (const item of response.results) {
              const key = normalizeResultDedupKey(item.path);
              if (!seenPaths.has(key)) {
                seenPaths.add(key);
                accumulated.push(item);
                addedInSlice += 1;
              }
            }
          }

          totalElapsedMs += response.elapsedMs ?? 0;
          if (response.totalIndexedFiles !== undefined) {
            latestIndexedFiles = response.totalIndexedFiles;
          }
          if (typeof response.planId === 'number' && response.planId > 0) {
            planId = response.planId;
          }

          const nextCursor = typeof response.nextCursor === 'number' ? response.nextCursor : 0;
          const hasMore =
            response.hasMore === true &&
            nextCursor > cursor &&
            accumulated.length < targetLimit;

          const shouldCommitResults =
            addedInSlice > 0 ||
            (!firstPainted && accumulated.length > 0) ||
            !hasMore;

          if (shouldCommitResults) {
            window.clearTimeout(loadingTimer);
            const snapshotItems = accumulated.slice();
            const shouldResetSelection = !firstPainted;
            firstPainted = true;
            startTransition(() => {
              setIsServiceStarting(false);
              setSnapshot({
                generationId: sequence,
                query: trimmedQuery,
                keywords: parsedKeywords,
                results: snapshotItems,
              });
              if (shouldResetSelection) {
                setSelectedIndex(0);
              }
              if (latestIndexedFiles !== undefined) setTotalIndexedFiles(latestIndexedFiles);
              setSearchElapsedMs(totalElapsedMs);
              if (response.available !== undefined) setServiceAvailable(response.available);
              if (hasMore) {
                setLoading(true);
              }
            });
          } else if (hasMore) {
            setLoading(true);
          }

          if (!hasMore) {
            break;
          }

          cursor = nextCursor;
          // 让出事件循环一帧，确保首屏列表渲染、滚动、键盘选行与新输入打断零延迟响应
          await new Promise<void>((resolve) => {
            window.setTimeout(resolve, 0);
          });
        }

        if (sequence !== requestSequenceRef.current) return;

        lastCompletedAtRef.current = Date.now();
        if (cacheKey && accumulated.length > 0) {
          if (resultCacheRef.current.size >= MAX_RESULT_CACHE_ENTRIES) {
            const oldestKey = resultCacheRef.current.keys().next().value as string | undefined;
            if (oldestKey !== undefined) resultCacheRef.current.delete(oldestKey);
          }
          resultCacheRef.current.set(cacheKey, {
            results: accumulated.slice(),
            elapsedMs: totalElapsedMs,
            totalIndexedFiles: latestIndexedFiles,
            timestamp: Date.now(),
          });
        }

        if (totalElapsedMs >= 3500) {
          const actualColdSeconds = Number((totalElapsedMs / 1000).toFixed(1));
          setLastColdScanDurationSec(actualColdSeconds);
          localStorage.setItem('tools3000_last_cold_content_scan_sec', String(actualColdSeconds));
        }
      } catch {
        if (sequence === requestSequenceRef.current) {
          startTransition(() => {
            setSnapshot({
              generationId: sequence,
              query: trimmedQuery,
              keywords: parsedKeywords,
              results: [],
            });
          });
        }
      } finally {
        window.clearTimeout(loadingTimer);
        if (!keepRetrying && sequence === requestSequenceRef.current) {
          isRunningRef.current = false;
          stopScanStopwatch(sequence);
          setLoading(false);
        }
      }
    };

    retryCountRef.current = 0;
    if (retryTimerRef.current !== null) {
      window.clearTimeout(retryTimerRef.current);
      retryTimerRef.current = null;
    }
    const debounceTimer = window.setTimeout(() => void runQuery(), debounceMs);

    return () => {
      window.clearTimeout(debounceTimer);
      if (retryTimerRef.current !== null) {
        window.clearTimeout(retryTimerRef.current);
        retryTimerRef.current = null;
      }
      if (requestSequenceRef.current === sequence) {
        isRunningRef.current = false;
        requestSequenceRef.current += 1;
      }
      stopScanStopwatch(sequence);
    };
  }, [
    activeCategory,
    customContentFormats,
    disabledContentFormats,
    enabledDrives,
    excludeGitAndModules,
    excludeHidden,
    isComposing,
    maxResultLimit,
    query,
    refreshCounter,
    searchMode,
    startScanStopwatch,
    stopScanStopwatch,
  ]);

  return {
    snapshot,
    results: snapshot.results,
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
  };
}

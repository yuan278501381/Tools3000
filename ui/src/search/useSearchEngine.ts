import {
  startTransition,
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type Dispatch,
  type SetStateAction,
} from 'react';
import { bridgeRequest } from '../hooks/useBridge';
import { isEmptyContentSyntax, nextQueryId, resolveDebounceMs } from '../searchScheduling';
import type { SearchMode, SearchResponse, SearchResult, SearchSnapshot } from './searchTypes';
import { isEffectiveContentSearch, parseQueryKeywords } from './searchUtils';
import { safeStorageGetNumber, safeStorageSetItem } from './storageUtils';

interface UseSearchEngineOptions {
  query: string;
  isComposing: boolean;
  activeCategory: string;
  searchMode: SearchMode;
  enabledDrives: string[];
  maxResultLimit: number;
  excludeGitAndModules: boolean;
  excludeHidden: boolean;
  customExcludedPaths?: string[];
  customContentFormats: string[];
  disabledContentFormats: string[];
}

export interface ContentScanPauseState {
  reason: 'budget' | 'background';
  scanned: number;
  total: number;
  elapsedMs: number;
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
  scanProgress: { scanned: number; total: number } | null;
  contentScanPaused: ContentScanPauseState | null;
  stopSearch: () => void;
  resumeContentScan: () => void;
  clearResultCache: () => void;
  refreshQuery: () => void;
  silentRefreshQuery: () => void;
  refreshOnFocus: () => void;
}

interface CachedQuery {
  results: SearchResult[];
  elapsedMs: number;
  totalIndexedFiles?: number;
  timestamp: number;
}

interface ContentScanCheckpoint {
  trimmedQuery: string;
  parsedKeywords: string[];
  effectiveMode: SearchMode;
  enabledDrives: string[];
  excludes: string[];
  excludeHidden: boolean;
  customContentFormats: string[];
  disabledContentFormats: string[];
  maxResultLimit: number;
  cacheKey: string;
  planId: number;
  cursor: number;
  totalCandidates: number;
  accumulated: SearchResult[];
  seenPaths: Set<string>;
  totalElapsedMs: number;
  latestIndexedFiles?: number;
}

const CONTENT_CACHE_TTL_MS = 45_000;
const DEFAULT_CONTENT_RESULT_LIMIT = 500;
const NAME_FOCUS_REFRESH_COOLDOWN_MS = 2_500;
const MAX_RESULT_CACHE_ENTRIES = 32;
const UNBOUNDED_CONTENT_SOFT_TIMEOUT_MS = 20_000;
const UNBOUNDED_CONTENT_SOFT_CANDIDATE_CAP = 30_000;
const BACKGROUND_PAUSE_DELAY_MS = 3_000;
const PATH_SCOPE_TOKEN_REGEX = /^(?:path|parent|p|in|folder|dir):|^[a-z]:[\\/]|\\/i;
const FORMAT_SCOPE_TOKEN_REGEX = /^(?:ext|extension):|^\*\.[a-z0-9]+$/i;

function normalizeResultDedupKey(rawPath: string): string {
  return rawPath.replace(/\//g, '\\').toLowerCase();
}

export function isUnboundedFullDiskContentQuery(rawQuery: string): boolean {
  const tokens = rawQuery.match(/"[^"]*"|\S+/g) ?? [];
  for (const rawToken of tokens) {
    const token = rawToken.replace(/^"+|"+$/g, '').trim();
    if (!token) continue;
    if (PATH_SCOPE_TOKEN_REGEX.test(token) || FORMAT_SCOPE_TOKEN_REGEX.test(token)) {
      return false;
    }
  }
  return true;
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
  customExcludedPaths = [],
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
  const [scanProgress, setScanProgress] = useState<{ scanned: number; total: number } | null>(null);
  const [contentScanPaused, setContentScanPaused] = useState<ContentScanPauseState | null>(null);
  const [lastColdScanDurationSec, setLastColdScanDurationSec] = useState(() => {
    return safeStorageGetNumber('tools3000_last_cold_content_scan_sec', 14.5, { min: 3 });
  });

  const requestSequenceRef = useRef(0);
  const retryCountRef = useRef(0);
  const retryTimerRef = useRef<number | null>(null);
  const backgroundPauseTimerRef = useRef<number | null>(null);
  const scanTimerRef = useRef<{ sequence: number; intervalId: number; startTime: number } | null>(null);
  const resultCacheRef = useRef<Map<string, CachedQuery>>(new Map());
  const isRunningRef = useRef(false);
  const isContentModeRef = useRef(false);
  const lastCompletedAtRef = useRef(0);
  const hasResultsRef = useRef(false);
  const activeContentProgressRef = useRef<ContentScanCheckpoint | null>(null);
  const resumeCheckpointRef = useRef<ContentScanCheckpoint | null>(null);
  const runContentLoopRef = useRef<((checkpoint: ContentScanCheckpoint, sequence: number, bypassBudgetGuard: boolean) => Promise<void>) | null>(null);
  const [refreshCounter, setRefreshCounter] = useState(0);
  const refreshRequestedRef = useRef(false);
  const silentRefreshRequestedRef = useRef(false);

  useEffect(() => {
    hasResultsRef.current = snapshot.results.length > 0;
  }, [snapshot.results.length]);

  const clearBackgroundPauseTimer = useCallback(() => {
    if (backgroundPauseTimerRef.current !== null) {
      window.clearTimeout(backgroundPauseTimerRef.current);
      backgroundPauseTimerRef.current = null;
    }
  }, []);

  const clearResultCache = useCallback(() => {
    resultCacheRef.current.clear();
  }, []);

  const refreshQuery = useCallback(() => {
    clearResultCache();
    resumeCheckpointRef.current = null;
    setContentScanPaused(null);
    silentRefreshRequestedRef.current = false;
    refreshRequestedRef.current = true;
    setRefreshCounter((c) => c + 1);
  }, [clearResultCache]);

  const silentRefreshQuery = useCallback(() => {
    // 实时文件变动无闪原地刷新：绝不打断正在执行的检索或渐进式全文扫描
    if (isRunningRef.current || isContentModeRef.current || resumeCheckpointRef.current !== null) {
      return;
    }
    silentRefreshRequestedRef.current = true;
    refreshRequestedRef.current = true;
    setRefreshCounter((c) => c + 1);
  }, []);

  const refreshOnFocus = useCallback(() => {
    // 正在执行扫描（尤其是渐进式全文扫描）或已软暂停等待用户决定续扫时，窗口获焦绝不打断或偷偷重扫
    if (isRunningRef.current || resumeCheckpointRef.current !== null) return;
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

  const cancelBackendSearch = useCallback(() => {
    void bridgeRequest('search.cancel', {}, { silent: true }).catch(() => undefined);
  }, []);

  const stopSearch = useCallback(() => {
    const wasRunning = isRunningRef.current;
    requestSequenceRef.current += 1;
    isRunningRef.current = false;
    clearBackgroundPauseTimer();
    activeContentProgressRef.current = null;
    resumeCheckpointRef.current = null;
    if (retryTimerRef.current !== null) {
      window.clearTimeout(retryTimerRef.current);
      retryTimerRef.current = null;
    }
    stopScanStopwatch();
    setLoading(false);
    setScanProgress(null);
    setContentScanPaused(null);
    if (wasRunning || isContentModeRef.current) {
      cancelBackendSearch();
    }
  }, [cancelBackendSearch, clearBackgroundPauseTimer, stopScanStopwatch]);

  const startScanStopwatch = useCallback((sequence: number, initialElapsedMs = 0) => {
    if (scanTimerRef.current?.sequence === sequence) {
      return;
    }
    stopScanStopwatch();
    const safeInitialMs = Math.max(0, initialElapsedMs);
    setScanElapsedSeconds(Number((safeInitialMs / 1000).toFixed(1)));
    const startTime = Date.now() - safeInitialMs;
    const intervalId = window.setInterval(() => {
      if (document.hidden) return;
      if (scanTimerRef.current?.sequence === sequence) {
        setScanElapsedSeconds(Number(((Date.now() - startTime) / 1000).toFixed(1)));
      }
    }, 100);
    scanTimerRef.current = { sequence, intervalId, startTime };
  }, [stopScanStopwatch]);

  const resumeContentScan = useCallback(() => {
    const checkpoint = resumeCheckpointRef.current;
    const runner = runContentLoopRef.current;
    if (!checkpoint || !runner) return;
    clearBackgroundPauseTimer();
    resumeCheckpointRef.current = null;
    startTransition(() => {
      setContentScanPaused(null);
    });
    const sequence = ++requestSequenceRef.current;
    void runner(checkpoint, sequence, true);
  }, [clearBackgroundPauseTimer]);

  // 原生宿主窗口隐藏/最小化立即中止在途内容扫描；切至后台失焦 3 秒后自动软暂停释放 CPU/磁盘 I/O
  useEffect(() => {
    const onWindowHidden = () => {
      clearBackgroundPauseTimer();
      if (isRunningRef.current && isContentModeRef.current) {
        stopSearch();
      }
    };

    const onWindowActiveChanged = (event: Event) => {
      const active = (event as CustomEvent<boolean>).detail;
      if (active === true) {
        clearBackgroundPauseTimer();
        return;
      }
      if (active === false && isRunningRef.current && isContentModeRef.current) {
        clearBackgroundPauseTimer();
        const trackedSeq = requestSequenceRef.current;
        backgroundPauseTimerRef.current = window.setTimeout(() => {
          backgroundPauseTimerRef.current = null;
          if (!isRunningRef.current || !isContentModeRef.current || requestSequenceRef.current !== trackedSeq) {
            return;
          }
          const progress = activeContentProgressRef.current;
          requestSequenceRef.current += 1;
          isRunningRef.current = false;
          if (retryTimerRef.current !== null) {
            window.clearTimeout(retryTimerRef.current);
            retryTimerRef.current = null;
          }
          stopScanStopwatch();
          setLoading(false);
          setScanProgress(null);
          if (progress) {
            const snapshotCheckpoint: ContentScanCheckpoint = {
              ...progress,
              accumulated: progress.accumulated.slice(),
              seenPaths: new Set(progress.seenPaths),
            };
            resumeCheckpointRef.current = snapshotCheckpoint;
            setContentScanPaused({
              reason: 'background',
              scanned: progress.totalCandidates > 0
                ? Math.min(progress.cursor, progress.totalCandidates)
                : progress.cursor,
              total: progress.totalCandidates,
              elapsedMs: progress.totalElapsedMs,
            });
          }
          activeContentProgressRef.current = null;
          cancelBackendSearch();
        }, BACKGROUND_PAUSE_DELAY_MS);
      }
    };

    window.addEventListener('tools3000:searchWindowHidden', onWindowHidden);
    window.addEventListener('tools3000:searchWindowActiveChanged', onWindowActiveChanged);
    return () => {
      clearBackgroundPauseTimer();
      window.removeEventListener('tools3000:searchWindowHidden', onWindowHidden);
      window.removeEventListener('tools3000:searchWindowActiveChanged', onWindowActiveChanged);
    };
  }, [cancelBackendSearch, clearBackgroundPauseTimer, stopScanStopwatch, stopSearch]);

  useEffect(() => () => {
    clearBackgroundPauseTimer();
    stopScanStopwatch();
    if (retryTimerRef.current !== null) window.clearTimeout(retryTimerRef.current);
    const shouldCancel = isRunningRef.current && isContentModeRef.current;
    isRunningRef.current = false;
    activeContentProgressRef.current = null;
    resumeCheckpointRef.current = null;
    requestSequenceRef.current += 1;
    if (shouldCancel) {
      cancelBackendSearch();
    }
  }, [cancelBackendSearch, clearBackgroundPauseTimer, stopScanStopwatch]);

  const runContentProgressiveLoop = useCallback(
    async (
      initialCheckpoint: ContentScanCheckpoint,
      sequence: number,
      bypassBudgetGuard: boolean,
    ) => {
      const {
        trimmedQuery,
        parsedKeywords,
        effectiveMode,
        enabledDrives: cpDrives,
        excludes,
        excludeHidden: cpExcludeHidden,
        customContentFormats: cpCustomFormats,
        disabledContentFormats: cpDisabledFormats,
        maxResultLimit: cpMaxResultLimit,
        cacheKey,
      } = initialCheckpoint;

      const isUnbounded = isUnboundedFullDiskContentQuery(trimmedQuery);
      const targetLimit = cpMaxResultLimit > 0 ? cpMaxResultLimit : DEFAULT_CONTENT_RESULT_LIMIT;
      const accumulated = initialCheckpoint.accumulated.slice();
      const seenPaths = new Set<string>(initialCheckpoint.seenPaths);
      let cursor = initialCheckpoint.cursor;
      let planId = initialCheckpoint.planId;
      let totalElapsedMs = initialCheckpoint.totalElapsedMs;
      let latestIndexedFiles = initialCheckpoint.latestIndexedFiles;
      let totalCandidates = initialCheckpoint.totalCandidates;
      let firstPainted = accumulated.length > 0;
      let sliceRetries = 0;
      let keepRetrying = false;
      let pausedByBudget = false;

      isRunningRef.current = true;
      setContentScanPaused(null);
      if (cursor > 0 && totalCandidates > 0) {
        setScanProgress({ scanned: Math.min(cursor, totalCandidates), total: totalCandidates });
      } else {
        setScanProgress(null);
      }
      if (retryCountRef.current === 0) {
        setIsServiceStarting(false);
      }

      const loadingTimer = window.setTimeout(() => {
        if (sequence === requestSequenceRef.current) setLoading(true);
      }, firstPainted ? 0 : 80);
      startScanStopwatch(sequence, totalElapsedMs);

      const syncActiveProgress = () => {
        activeContentProgressRef.current = {
          trimmedQuery,
          parsedKeywords,
          effectiveMode,
          enabledDrives: cpDrives,
          excludes,
          excludeHidden: cpExcludeHidden,
          customContentFormats: cpCustomFormats,
          disabledContentFormats: cpDisabledFormats,
          maxResultLimit: cpMaxResultLimit,
          cacheKey,
          planId,
          cursor,
          totalCandidates,
          accumulated: accumulated.slice(),
          seenPaths: new Set(seenPaths),
          totalElapsedMs,
          latestIndexedFiles,
        };
      };
      syncActiveProgress();

      try {
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
            drives: cpDrives.length > 0 ? cpDrives : undefined,
            excludes: excludes.length > 0 ? excludes : undefined,
            excludeHidden: cpExcludeHidden,
            excludeSystem: cpExcludeHidden,
            contentCustomExts: cpCustomFormats,
            contentDisabledExts: cpDisabledFormats,
          });

          if (response?.cancelled || sequence !== requestSequenceRef.current) return;

          if (response.status === 'timeout') {
            window.clearTimeout(loadingTimer);
            startTransition(() => {
              setIsServiceStarting(false);
              setServiceAvailable(true);
            });
            break;
          }

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
                retryTimerRef.current = window.setTimeout(
                  () => void runContentLoopRef.current?.(initialCheckpoint, sequence, bypassBudgetGuard),
                  retryDelay,
                );
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
              setLoading(true);
              startTransition(() => {
                setIsServiceStarting(true);
                setServiceAvailable(true);
              });
              if (retryCountRef.current < 90) {
                retryCountRef.current += 1;
                keepRetrying = true;
                const retryDelay = Math.min(250 + retryCountRef.current * 80, 1000);
                retryTimerRef.current = window.setTimeout(
                  () => void runContentLoopRef.current?.(initialCheckpoint, sequence, bypassBudgetGuard),
                  retryDelay,
                );
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

          const nextCursor = typeof response.nextCursor === 'number' ? response.nextCursor : cursor;
          totalCandidates = typeof response.totalCandidates === 'number' ? response.totalCandidates : totalCandidates;
          const hasMore =
            response.hasMore === true &&
            nextCursor > cursor &&
            accumulated.length < targetLimit;

          const hitSoftBudget =
            !bypassBudgetGuard &&
            isUnbounded &&
            hasMore &&
            (totalElapsedMs >= UNBOUNDED_CONTENT_SOFT_TIMEOUT_MS ||
              nextCursor >= UNBOUNDED_CONTENT_SOFT_CANDIDATE_CAP);

          const effectiveHasMore = hasMore && !hitSoftBudget;

          if (effectiveHasMore && totalCandidates > 0) {
            setScanProgress({ scanned: Math.min(nextCursor, totalCandidates), total: totalCandidates });
          } else {
            setScanProgress(null);
          }

          const shouldCommitResults =
            addedInSlice > 0 ||
            (!firstPainted && accumulated.length > 0) ||
            !effectiveHasMore;

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
              if (effectiveHasMore) {
                setLoading(true);
              }
            });
          } else if (effectiveHasMore) {
            setLoading(true);
          }

          cursor = nextCursor;
          syncActiveProgress();

          if (hitSoftBudget) {
            pausedByBudget = true;
            resumeCheckpointRef.current = activeContentProgressRef.current;
            setContentScanPaused({
              reason: 'budget',
              scanned: totalCandidates > 0 ? Math.min(nextCursor, totalCandidates) : nextCursor,
              total: totalCandidates,
              elapsedMs: totalElapsedMs,
            });
            cancelBackendSearch();
            break;
          }

          if (!hasMore) {
            break;
          }

          // 让出事件循环一帧，确保首屏列表渲染、滚动、键盘选行与新输入打断零延迟响应
          await new Promise<void>((resolve) => {
            window.setTimeout(resolve, 0);
          });
        }

        if (sequence !== requestSequenceRef.current) return;

        lastCompletedAtRef.current = Date.now();
        if (!pausedByBudget) {
          resumeCheckpointRef.current = null;
          setContentScanPaused(null);
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
            safeStorageSetItem('tools3000_last_cold_content_scan_sec', String(actualColdSeconds));
          }
        }
      } catch (err) {
        if (sequence === requestSequenceRef.current) {
          const errMsg = err instanceof Error ? err.message : String(err ?? '');
          const isTimeoutErr = /timeout|timed\s*out/i.test(errMsg);
          if (!isTimeoutErr && retryCountRef.current < 3 && accumulated.length === 0) {
            setLoading(true);
            startTransition(() => {
              setIsServiceStarting(false);
              setServiceAvailable(true);
            });
            retryCountRef.current += 1;
            keepRetrying = true;
            const retryDelay = Math.min(350 + retryCountRef.current * 200, 1000);
            retryTimerRef.current = window.setTimeout(
              () => void runContentLoopRef.current?.(initialCheckpoint, sequence, bypassBudgetGuard),
              retryDelay,
            );
          } else {
            startTransition(() => {
              setIsServiceStarting(false);
              setServiceAvailable(isTimeoutErr);
              if (accumulated.length === 0) {
                setSnapshot({
                  generationId: sequence,
                  query: trimmedQuery,
                  keywords: parsedKeywords,
                  results: [],
                });
              }
            });
          }
        }
      } finally {
        window.clearTimeout(loadingTimer);
        if (!keepRetrying && sequence === requestSequenceRef.current) {
          isRunningRef.current = false;
          activeContentProgressRef.current = null;
          clearBackgroundPauseTimer();
          stopScanStopwatch(sequence);
          setLoading(false);
          setScanProgress(null);
        }
      }
    },
    [cancelBackendSearch, clearBackgroundPauseTimer, startScanStopwatch, stopScanStopwatch],
  );

  useEffect(() => {
    runContentLoopRef.current = runContentProgressiveLoop;
  }, [runContentProgressiveLoop]);

  const enabledDrivesKey = enabledDrives.join('\u0000');
  const customExcludedPathsKey = customExcludedPaths.join('\u0000');
  const customFormatsKey = customContentFormats.join('\u0000');
  const disabledFormatsKey = disabledContentFormats.join('\u0000');
  const stableEnabledDrives = useMemo(
    () => (enabledDrivesKey ? enabledDrivesKey.split('\u0000') : []),
    [enabledDrivesKey],
  );
  const stableCustomExcludedPaths = useMemo(
    () => (customExcludedPathsKey ? customExcludedPathsKey.split('\u0000') : []),
    [customExcludedPathsKey],
  );
  const stableCustomFormats = useMemo(
    () => (customFormatsKey ? customFormatsKey.split('\u0000') : []),
    [customFormatsKey],
  );
  const stableDisabledFormats = useMemo(
    () => (disabledFormatsKey ? disabledFormatsKey.split('\u0000') : []),
    [disabledFormatsKey],
  );

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
      const wasActiveContent = isRunningRef.current && activeContentProgressRef.current !== null;
      isRunningRef.current = false;
      activeContentProgressRef.current = null;
      resumeCheckpointRef.current = null;
      clearBackgroundPauseTimer();
      stopScanStopwatch();
      if (retryTimerRef.current !== null) {
        window.clearTimeout(retryTimerRef.current);
        retryTimerRef.current = null;
      }
      if (wasActiveContent) {
        cancelBackendSearch();
      }
      startTransition(() => {
        setSnapshot({
          generationId: ++requestSequenceRef.current,
          query: '',
          keywords: [],
          results: [],
        });
        setLoading(false);
        setScanProgress(null);
        setContentScanPaused(null);
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

    resumeCheckpointRef.current = null;
    startTransition(() => {
      setContentScanPaused(null);
    });
    const sequence = ++requestSequenceRef.current;
    const isRefresh = refreshRequestedRef.current;
    const isSilentRefresh = silentRefreshRequestedRef.current;
    refreshRequestedRef.current = false;
    silentRefreshRequestedRef.current = false;
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
        setScanProgress(null);
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
            stableEnabledDrives.slice().sort().join(','),
            maxResultLimit,
            excludeGitAndModules ? 'exDev' : 'all',
            excludeHidden ? 'exHidden' : 'showHidden',
            stableCustomExcludedPaths.slice().map((p) => p.toLowerCase()).sort().join('|'),
            stableDisabledFormats.slice().sort().join(','),
            stableCustomFormats.slice().sort().join(','),
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

      const baseExcludes = excludeGitAndModules
        ? ['$Recycle.Bin', 'System Volume Information', 'node_modules', '.git', '__pycache__', 'npm-cache', 'go-build', '.gradle', 'pip\\cache']
        : [];
      const excludesSeen = new Set<string>(baseExcludes.map((item) => item.toLowerCase()));
      const excludes = [...baseExcludes];
      for (const rawPath of stableCustomExcludedPaths) {
        const clean = rawPath.trim();
        if (!clean) continue;
        const lower = clean.toLowerCase();
        if (excludesSeen.has(lower)) continue;
        excludesSeen.add(lower);
        excludes.push(clean);
      }

      const parsedKeywords = parseQueryKeywords(trimmedQuery);

      if (isContentMode) {
        await runContentProgressiveLoop(
          {
            trimmedQuery,
            parsedKeywords,
            effectiveMode,
            enabledDrives: stableEnabledDrives,
            excludes,
            excludeHidden,
            customContentFormats: stableCustomFormats,
            disabledContentFormats: stableDisabledFormats,
            maxResultLimit,
            cacheKey,
            planId: 0,
            cursor: 0,
            totalCandidates: 0,
            accumulated: [],
            seenPaths: new Set<string>(),
            totalElapsedMs: 0,
          },
          sequence,
          false,
        );
        return;
      }

      isRunningRef.current = true;
      setScanProgress(null);
      // 发起全新查询时立即清除上一轮遗留的“服务启动中”提示，避免切查询后错误残留
      if (retryCountRef.current === 0 && !isSilentRefresh) {
        setIsServiceStarting(false);
      }
      const loadingTimer = isSilentRefresh
        ? 0
        : window.setTimeout(() => {
            if (sequence === requestSequenceRef.current) setLoading(true);
          }, 80);
      if (!isSilentRefresh) {
        startScanStopwatch(sequence);
      }

      let keepRetrying = false;

      try {
        const response = await bridgeRequest<SearchResponse>('search.query', {
          query: trimmedQuery,
          queryId: nextQueryId(),
          searchMode: effectiveMode,
          limit: maxResultLimit,
          drives: stableEnabledDrives.length > 0 ? stableEnabledDrives : undefined,
          excludes: excludes.length > 0 ? excludes : undefined,
          excludeHidden,
          excludeSystem: excludeHidden,
          contentCustomExts: stableCustomFormats,
          contentDisabledExts: stableDisabledFormats,
        });

        if (response?.cancelled || sequence !== requestSequenceRef.current) return;
        window.clearTimeout(loadingTimer);

        if (response.status === 'timeout') {
          startTransition(() => {
            setIsServiceStarting(false);
            setServiceAvailable(true);
          });
          return;
        }

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
          setLoading(true);
          startTransition(() => {
            setIsServiceStarting(true);
            setServiceAvailable(true);
          });
          if (retryCountRef.current < 90) {
            retryCountRef.current += 1;
            keepRetrying = true;
            const retryDelay = Math.min(250 + retryCountRef.current * 80, 1000);
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
            if (len === 0) return 0;
            return isSilentRefresh ? Math.min(prev, len - 1) : 0;
          });
          if (response.totalIndexedFiles !== undefined) setTotalIndexedFiles(response.totalIndexedFiles);
          if (response.elapsedMs !== undefined) setSearchElapsedMs(response.elapsedMs);
          if (response.available !== undefined) setServiceAvailable(response.available);
        });
      } catch (err) {
        if (sequence === requestSequenceRef.current) {
          const errMsg = err instanceof Error ? err.message : String(err ?? '');
          const isTimeoutErr = /timeout|timed\s*out/i.test(errMsg);
          // 已运行服务发生查询超时绝不伪装为“正在连接索引服务”并触发 90 次重试雪崩
          if (!isTimeoutErr && retryCountRef.current < 3) {
            setLoading(true);
            startTransition(() => {
              setIsServiceStarting(false);
              setServiceAvailable(true);
            });
            retryCountRef.current += 1;
            keepRetrying = true;
            const retryDelay = Math.min(350 + retryCountRef.current * 200, 1000);
            retryTimerRef.current = window.setTimeout(() => void runQuery(), retryDelay);
          } else {
            startTransition(() => {
              setIsServiceStarting(false);
              setServiceAvailable(isTimeoutErr);
              setSnapshot({
                generationId: sequence,
                query: trimmedQuery,
                keywords: parsedKeywords,
                results: [],
              });
            });
          }
        }
      } finally {
        window.clearTimeout(loadingTimer);
        if (!keepRetrying && sequence === requestSequenceRef.current) {
          isRunningRef.current = false;
          stopScanStopwatch(sequence);
          setLoading(false);
          setScanProgress(null);
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
      clearBackgroundPauseTimer();
      if (retryTimerRef.current !== null) {
        window.clearTimeout(retryTimerRef.current);
        retryTimerRef.current = null;
      }
      const wasActiveContent = isRunningRef.current && activeContentProgressRef.current !== null;
      if (requestSequenceRef.current === sequence) {
        isRunningRef.current = false;
        activeContentProgressRef.current = null;
        requestSequenceRef.current += 1;
        if (wasActiveContent) {
          cancelBackendSearch();
        }
      }
      stopScanStopwatch(sequence);
    };
  }, [
    activeCategory,
    cancelBackendSearch,
    clearBackgroundPauseTimer,
    excludeGitAndModules,
    excludeHidden,
    isComposing,
    maxResultLimit,
    query,
    refreshCounter,
    runContentProgressiveLoop,
    searchMode,
    stableCustomExcludedPaths,
    stableCustomFormats,
    stableDisabledFormats,
    stableEnabledDrives,
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
    scanProgress,
    contentScanPaused,
    stopSearch,
    resumeContentScan,
    clearResultCache,
    refreshQuery,
    silentRefreshQuery,
    refreshOnFocus,
  };
}

import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { bridgeRequest } from '../hooks/useBridge';
import {
  doesItemRequireBackendProperty,
  isBackendExtractableColumn,
} from './columnCatalog';
import type {
  PropertyCacheMap,
  PropertyCellValue,
  SearchResult,
  SortProgressState,
  TableColumnId,
} from './searchTypes';

const VIEWPORT_PREFETCH_BUFFER = 12;
const VIEWPORT_DEBOUNCE_MS = 35;
const VIEWPORT_BATCH_SIZE = 48;
const CLIENT_PROPERTY_LRU_MAX = 4096;
const SORT_PROPERTY_LRU_FLOOR = 16384;
const SORT_PROPERTY_LRU_CEILING = 65536;

interface BatchPropertiesResponse {
  success?: boolean;
  values?: Record<string, Record<string, PropertyCellValue>>;
  budgetExceeded?: boolean;
}

interface CacheBucket {
  signature: string;
  props: Record<string, PropertyCellValue>;
}

function makeItemVersionSignature(item: SearchResult): string {
  return `${item.size ?? 0}:${item.lastWriteTime ?? 0}`;
}

export interface UseViewportPropertiesOptions {
  /** 稳定原始结果集（如 snapshot.results），仅在检索结果变更时变更，避免排序刷新误杀在途提取。 */
  sourceResults?: SearchResult[];
  /** 当前展示顺序结果集（sortedResults），用于视口可见行精确切片预取。 */
  results: SearchResult[];
  visibleColumns: readonly TableColumnId[];
  visibleRange: { start: number; end: number };
  enabled: boolean;
  onPropertyCacheChange?: (next: PropertyCacheMap) => void;
}

export interface UseViewportPropertiesResult {
  propertyCache: PropertyCacheMap;
  getLatestPropertyCache: () => PropertyCacheMap;
  sortProgress: SortProgressState | null;
  cancelSortExtraction: () => void;
  ensureColumnLoadedForSort: (
    colId: TableColumnId,
    colLabel: string,
    items: SearchResult[],
  ) => Promise<boolean>;
}

/**
 * 视口惰性属性提取与可取消扩展列排序引擎：
 * - 彻底解耦稳定 `sourceResults` 与展示顺序 `results`，彻底杜绝扩展列排序边拉边自杀；
 * - 不可变更新每行属性字典对象，保障 `React.memo` 精确感知并立即刷新单元格；
 * - 精准处理后端 `budgetExceeded` 时间预算截断与满 48 条视口自动续拉，杜绝静默漏行；
 * - 排序期间动态扩容 LRU 上限，避免大结果集边拉后面边淘汰前面缓存；
 * - 支持 `accessed`/`attributes` 在缺失原生元数据时按需请求后端提取。
 */
export function useViewportProperties({
  sourceResults,
  results,
  visibleColumns,
  visibleRange,
  enabled,
  onPropertyCacheChange,
}: UseViewportPropertiesOptions): UseViewportPropertiesResult {
  const stableSourceResults = sourceResults ?? results;
  const [propertyCache, setPropertyCache] = useState<PropertyCacheMap>({});
  const [viewportDrainTick, setViewportDrainTick] = useState(0);
  const [sortProgressRecord, setSortProgressRecord] = useState<{
    sourceResults: SearchResult[];
    state: SortProgressState;
  } | null>(null);

  const lruRef = useRef<Map<string, CacheBucket>>(new Map());
  const lruLimitRef = useRef<number>(CLIENT_PROPERTY_LRU_MAX);
  const latestSnapshotRef = useRef<PropertyCacheMap>({});
  const inFlightRef = useRef<Set<string>>(new Set());
  const viewportGenRef = useRef(0);
  const sortGenRef = useRef(0);
  const sourceResultsRef = useRef<SearchResult[]>(stableSourceResults);
  useLayoutEffect(() => {
    sourceResultsRef.current = stableSourceResults;
  }, [stableSourceResults]);

  const onCacheChangeRef = useRef(onPropertyCacheChange);
  useEffect(() => {
    onCacheChangeRef.current = onPropertyCacheChange;
  }, [onPropertyCacheChange]);

  const backendPropIds = useMemo(() => {
    const out: TableColumnId[] = [];
    for (const colId of visibleColumns) {
      if (isBackendExtractableColumn(colId)) {
        out.push(colId);
      }
    }
    return out;
  }, [visibleColumns]);

  const backendPropsKey = useMemo(() => backendPropIds.join('\u0000'), [backendPropIds]);

  const commitExtractedValues = useCallback(
    (
      requestedItems: readonly SearchResult[],
      returned: Record<string, Record<string, PropertyCellValue>>,
    ) => {
      const lru = lruRef.current;
      let changed = false;

      for (const item of requestedItems) {
        const incoming = returned[item.path];
        if (!incoming) continue;
        const sig = makeItemVersionSignature(item);
        const existing = lru.get(item.path);
        if (existing && existing.signature === sig) {
          lru.delete(item.path);
          let changedEntries: Record<string, PropertyCellValue> | null = null;
          for (const [k, v] of Object.entries(incoming)) {
            const prev = existing.props[k];
            if (!prev || prev.d !== v.d || prev.n !== v.n) {
              if (!changedEntries) changedEntries = {};
              changedEntries[k] = v;
            }
          }
          if (changedEntries) {
            // 不可变生成新的 props 对象，保障 React.memo 浅比较感知变更并重绘行
            lru.set(item.path, {
              signature: sig,
              props: { ...existing.props, ...changedEntries },
            });
            changed = true;
          } else {
            lru.set(item.path, existing);
          }
        } else {
          if (existing) lru.delete(item.path);
          lru.set(item.path, { signature: sig, props: { ...incoming } });
          changed = true;
        }
      }

      const maxEntries = lruLimitRef.current;
      while (lru.size > maxEntries) {
        const oldestKey = lru.keys().next().value;
        if (oldestKey === undefined) break;
        lru.delete(oldestKey);
        changed = true;
      }

      if (changed) {
        const snapshot: PropertyCacheMap = {};
        lru.forEach((bucket, pathKey) => {
          snapshot[pathKey] = bucket.props;
        });
        latestSnapshotRef.current = snapshot;
        setPropertyCache(snapshot);
        onCacheChangeRef.current?.(snapshot);
      }
    },
    [],
  );

  const hasCachedProp = useCallback((item: SearchResult, propId: string): boolean => {
    const bucket = lruRef.current.get(item.path);
    if (!bucket || bucket.signature !== makeItemVersionSignature(item)) return false;
    return Object.prototype.hasOwnProperty.call(bucket.props, propId);
  }, []);

  const getLatestPropertyCache = useCallback((): PropertyCacheMap => {
    return latestSnapshotRef.current;
  }, []);

  // 视口惰性提取：防抖合并当前可见行（含前后缓冲）未命中的扩展列，满 48 条或预算截断自动排程续拉
  useEffect(() => {
    if (!enabled || backendPropIds.length === 0 || results.length === 0) {
      return;
    }

    const gen = ++viewportGenRef.current;
    const timer = window.setTimeout(() => {
      if (gen !== viewportGenRef.current) return;

      const winStart = Math.max(0, visibleRange.start - VIEWPORT_PREFETCH_BUFFER);
      const winEnd = Math.min(
        results.length,
        Math.max(visibleRange.end, visibleRange.start + 32) + VIEWPORT_PREFETCH_BUFFER,
      );

      const pendingItems: SearchResult[] = [];
      const missingPropsUnion = new Set<string>();
      const flightTokens: string[] = [];
      let hasMoreMissingInWindow = false;

      for (let idx = winStart; idx < winEnd; idx += 1) {
        const item = results[idx];
        if (!item || !item.path) continue;
        const itemMissingProps: string[] = [];
        for (const propId of backendPropIds) {
          if (!doesItemRequireBackendProperty(item, propId)) continue;
          if (hasCachedProp(item, propId)) continue;
          const token = `${item.path}\u0000${propId}`;
          if (inFlightRef.current.has(token)) continue;
          itemMissingProps.push(propId);
        }
        if (itemMissingProps.length > 0) {
          if (pendingItems.length >= VIEWPORT_BATCH_SIZE) {
            hasMoreMissingInWindow = true;
            break;
          }
          pendingItems.push(item);
          for (const propId of itemMissingProps) {
            missingPropsUnion.add(propId);
            flightTokens.push(`${item.path}\u0000${propId}`);
          }
        }
      }

      if (pendingItems.length === 0 || missingPropsUnion.size === 0) {
        return;
      }

      for (const token of flightTokens) {
        inFlightRef.current.add(token);
      }

      let tokensReleased = false;
      const releaseFlightTokens = () => {
        if (tokensReleased) return;
        tokensReleased = true;
        for (const token of flightTokens) {
          inFlightRef.current.delete(token);
        }
      };

      const propsList = Array.from(missingPropsUnion);
      void bridgeRequest<BatchPropertiesResponse>('search.batchGetProperties', {
        items: pendingItems.map((it) => ({
          path: it.path,
          isDirectory: it.isDirectory,
          size: it.size ?? 0,
          lastWriteTime: it.lastWriteTime ?? 0,
        })),
        properties: propsList,
      })
        .then((resp) => {
          // 必须在排程下一批续拉前立即释放 inFlight 锁，避免预算截断项被旧锁跳过
          releaseFlightTokens();
          if (gen !== viewportGenRef.current) return;
          if (!resp?.success) return;

          const returnedValues = resp.values ?? {};
          const returnedPaths = Object.keys(returnedValues);
          const mergedToCommit: Record<string, Record<string, PropertyCellValue>> = {
            ...returnedValues,
          };

          // 非预算截断时，对未返回的条目补写空占位防重复请求；预算截断时保留未返回项待下一批补齐
          if (!resp.budgetExceeded) {
            for (const item of pendingItems) {
              const existingRow = mergedToCommit[item.path] ? { ...mergedToCommit[item.path] } : {};
              let touched = false;
              for (const propId of propsList) {
                if (
                  doesItemRequireBackendProperty(item, propId) &&
                  !Object.prototype.hasOwnProperty.call(existingRow, propId)
                ) {
                  existingRow[propId] = { d: '' };
                  touched = true;
                }
              }
              if (touched) {
                mergedToCommit[item.path] = existingRow;
              }
            }
          }

          if (Object.keys(mergedToCommit).length > 0) {
            commitExtractedValues(pendingItems, mergedToCommit);
          }

          // 若本批触达 48 条上限或遭遇预算截断且本轮有实际推进，自动排程拉取可见窗口内下一批
          const madeProgress = !resp.budgetExceeded || returnedPaths.length > 0;
          const needsNextPass =
            madeProgress &&
            (hasMoreMissingInWindow ||
              (resp.budgetExceeded === true && returnedPaths.length < pendingItems.length));
          if (needsNextPass && gen === viewportGenRef.current) {
            setViewportDrainTick((tick) => tick + 1);
          }
        })
        .catch(() => undefined)
        .finally(() => {
          releaseFlightTokens();
        });
    }, VIEWPORT_DEBOUNCE_MS);

    return () => {
      window.clearTimeout(timer);
    };
  }, [
    backendPropIds,
    backendPropsKey,
    commitExtractedValues,
    enabled,
    hasCachedProp,
    results,
    viewportDrainTick,
    visibleRange.end,
    visibleRange.start,
  ]);

  const cancelSortExtraction = useCallback(() => {
    sortGenRef.current += 1;
    setSortProgressRecord(null);
  }, []);

  // 仅在原始结果集 sourceResults 真正切换时终止上一轮扩展列排序提取，绝不因排序结果刷新自杀
  useEffect(() => {
    sortGenRef.current += 1;
    if (stableSourceResults.length <= CLIENT_PROPERTY_LRU_MAX) {
      lruLimitRef.current = CLIENT_PROPERTY_LRU_MAX;
    }
  }, [stableSourceResults]);

  const ensureColumnLoadedForSort = useCallback(
    async (colId: TableColumnId, colLabel: string, items: SearchResult[]): Promise<boolean> => {
      if (!isBackendExtractableColumn(colId) || items.length === 0) {
        return true;
      }

      const missingItems = items.filter(
        (item) => item.path && doesItemRequireBackendProperty(item, colId) && !hasCachedProp(item, colId),
      );
      if (missingItems.length === 0) {
        return true;
      }

      // 排序期间临时扩容 LRU 上限，防止大结果集边拉取尾部边淘汰头部缓存
      lruLimitRef.current = Math.min(
        SORT_PROPERTY_LRU_CEILING,
        Math.max(lruLimitRef.current, SORT_PROPERTY_LRU_FLOOR, items.length + 256),
      );

      const ownerSource = sourceResultsRef.current;
      const myGen = ++sortGenRef.current;
      const total = missingItems.length;
      let completed = 0;
      let pendingQueue = missingItems.slice();
      let consecutiveStalled = 0;

      setSortProgressRecord({
        sourceResults: ownerSource,
        state: {
          active: true,
          columnId: colId,
          columnLabel: colLabel,
          completed: 0,
          total,
        },
      });

      try {
        while (pendingQueue.length > 0) {
          if (sortGenRef.current !== myGen) {
            return false;
          }

          const batch = pendingQueue.slice(0, VIEWPORT_BATCH_SIZE);
          const resp = await bridgeRequest<BatchPropertiesResponse>('search.batchGetProperties', {
            items: batch.map((it) => ({
              path: it.path,
              isDirectory: it.isDirectory,
              size: it.size ?? 0,
              lastWriteTime: it.lastWriteTime ?? 0,
            })),
            properties: [colId],
          });

          if (sortGenRef.current !== myGen) {
            return false;
          }

          if (!resp?.success) {
            return false;
          }

          const returned = resp.values ?? {};
          const returnedItems: SearchResult[] = [];
          const unreturnedItems: SearchResult[] = [];
          for (const item of batch) {
            if (Object.prototype.hasOwnProperty.call(returned, item.path)) {
              returnedItems.push(item);
            } else {
              unreturnedItems.push(item);
            }
          }

          if (returnedItems.length > 0) {
            commitExtractedValues(returnedItems, returned);
          }

          if (resp.budgetExceeded === true && unreturnedItems.length > 0) {
            if (returnedItems.length > 0) {
              consecutiveStalled = 0;
              completed += returnedItems.length;
              // 仅移除实际返回的键，将预算截断遗留项保留在队列首位继续拉取
              pendingQueue = [...unreturnedItems, ...pendingQueue.slice(batch.length)];
            } else {
              consecutiveStalled += 1;
              if (consecutiveStalled >= 2) {
                // 连续两次 0 推进时跳过队首单条并写空占位，防止单文件超时造成死循环
                const skipped = batch[0];
                commitExtractedValues([skipped], { [skipped.path]: { [colId]: { d: '' } } });
                completed += 1;
                consecutiveStalled = 0;
                pendingQueue = pendingQueue.slice(1);
              }
            }
          } else {
            consecutiveStalled = 0;
            if (unreturnedItems.length > 0) {
              const fallbackEmpty: Record<string, Record<string, PropertyCellValue>> = {};
              for (const item of unreturnedItems) {
                fallbackEmpty[item.path] = { [colId]: { d: '' } };
              }
              commitExtractedValues(unreturnedItems, fallbackEmpty);
            }
            completed += batch.length;
            pendingQueue = pendingQueue.slice(batch.length);
          }

          setSortProgressRecord({
            sourceResults: ownerSource,
            state: {
              active: true,
              columnId: colId,
              columnLabel: colLabel,
              completed: Math.min(total, completed),
              total,
            },
          });
        }
        return true;
      } catch {
        return false;
      } finally {
        if (sortGenRef.current === myGen) {
          setSortProgressRecord(null);
        }
      }
    },
    [commitExtractedValues, hasCachedProp],
  );

  // 切结果集瞬间立即归零对外暴露的进度状态，彻底杜绝残留旧结果集进度条
  const sortProgress =
    sortProgressRecord && sortProgressRecord.sourceResults === stableSourceResults
      ? sortProgressRecord.state
      : null;

  return {
    propertyCache,
    getLatestPropertyCache,
    sortProgress,
    cancelSortExtraction,
    ensureColumnLoadedForSort,
  };
}

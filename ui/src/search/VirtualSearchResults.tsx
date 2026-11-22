import {
  memo,
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type MouseEvent as ReactMouseEvent,
  type UIEvent,
} from 'react';
import { DynamicRowLayout, isSelectedOutsideVirtualRange } from '../searchVirtualization';
import { SearchResultRow } from './SearchResultRow';
import { buildTableLayoutMetrics, resolveNativeIconRequest } from './searchUtils';
import type {
  ColumnLayout,
  PropertyCacheMap,
  SearchDensity,
  SearchIconStyle,
  SearchResult,
} from './searchTypes';

const MAX_MEASURED_HEIGHTS_CACHE = 2048;

export interface VirtualSearchResultsProps {
  results: SearchResult[];
  selectedIndex: number;
  density: SearchDensity;
  iconStyle?: SearchIconStyle;
  iconCache?: Record<string, string>;
  onRequestIcon?: (ext: string, isDirectory: boolean, filePath?: string) => void;
  columns: ColumnLayout;
  propertyCache?: PropertyCacheMap;
  queryKeywords: string[];
  onHover: (index: number) => void;
  onSelect: (index: number) => void;
  onOpen: (result: SearchResult) => void;
  onContextMenu: (event: ReactMouseEvent, index: number, result: SearchResult) => void;
  onVisibleRangeChange?: (range: { start: number; end: number }) => void;
  onHorizontalScroll?: (scrollLeft: number) => void;
  hidden?: boolean;
}

/** 单一表格视图（支持正交全文摘要行展开）的高性能动态行高虚拟列表。 */
export const VirtualSearchResults = memo(function VirtualSearchResults({
  results,
  selectedIndex,
  density,
  iconStyle = 'native',
  iconCache = {},
  onRequestIcon,
  columns,
  propertyCache,
  queryKeywords,
  onHover,
  onSelect,
  onOpen,
  onContextMenu,
  onVisibleRangeChange,
  onHorizontalScroll,
  hidden = false,
}: VirtualSearchResultsProps) {
  const listRef = useRef<HTMLUListElement>(null);
  const observerRef = useRef<ResizeObserver | null>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewportHeight, setViewportHeight] = useState(480);
  const [layoutRevision, setLayoutRevision] = useState(0);

  const prevSelectedIndexRef = useRef(selectedIndex);
  const prevResultsRef = useRef(results);
  const prevKeywordsKeyRef = useRef('');
  const isScrollingRef = useRef(false);
  const scrollEndTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // 带 LRU 容量上限的已测真实 DOM 行高缓存（跨流式分片保活，防止含多行摘要的结果坍缩闪烁）
  const measuredHeightsRef = useRef<Map<string, number>>(new Map());
  const prevDensityRef = useRef<SearchDensity>(density);
  if (prevDensityRef.current !== density) {
    prevDensityRef.current = density;
    measuredHeightsRef.current.clear();
  }

  useEffect(() => {
    return () => {
      if (scrollEndTimerRef.current) {
        clearTimeout(scrollEndTimerRef.current);
      }
    };
  }, []);

  const keywordsKey = useMemo(() => queryKeywords.join('\u0001'), [queryKeywords]);
  const tableMetrics = useMemo(() => buildTableLayoutMetrics(columns), [columns]);
  const tableGridTemplate = tableMetrics.gridTemplateColumns;
  const tableMinWidthPx = tableMetrics.minTableWidthPx;

  // 渲染帧同步重置：当查询词或显示状态变更且首项选中时，立即将滚动偏移同步归零，同时保持流式分片追加期间视口零跳动
  const [prevKeywordsState, setPrevKeywordsState] = useState(keywordsKey);
  const [prevHiddenState, setPrevHiddenState] = useState(hidden);
  if (prevKeywordsState !== keywordsKey || prevHiddenState !== hidden) {
    setPrevKeywordsState(keywordsKey);
    setPrevHiddenState(hidden);
    if (selectedIndex === 0 && scrollTop !== 0) {
      setScrollTop(0);
    }
  }

  // 表格视图三档密度基线行高：紧凑 32px / 标准 36px / 宽松 42px
  const estimatedHeight = density === 'compact' ? 32 : density === 'comfortable' ? 42 : 36;
  const layout = useMemo(() => {
    const nextLayout = new DynamicRowLayout(results.length, estimatedHeight);
    const cache = measuredHeightsRef.current;
    if (cache.size > 0) {
      for (let i = 0; i < results.length; i += 1) {
        const cachedHeight = cache.get(results[i].path);
        if (cachedHeight !== undefined) {
          nextLayout.updateHeight(i, cachedHeight);
        }
      }
    }
    return nextLayout;
  }, [results, estimatedHeight]);

  const layoutRef = useRef(layout);
  layoutRef.current = layout;
  const resultsRef = useRef(results);
  resultsRef.current = results;

  const findIndexAtOffset = useCallback((offset: number, layoutGeneration: number) => {
    void layoutGeneration;
    return layout.indexAtOffset(offset);
  }, [layout]);

  const range = useMemo(() => {
    const overscan = Math.max(360, viewportHeight * 0.75);
    return {
      start: Math.max(0, findIndexAtOffset(Math.max(0, scrollTop - overscan), layoutRevision)),
      end: Math.min(results.length, findIndexAtOffset(scrollTop + viewportHeight + overscan, layoutRevision) + 1),
    };
  }, [findIndexAtOffset, layoutRevision, results.length, scrollTop, viewportHeight]);
  const selectedOutsideRange = isSelectedOutsideVirtualRange(results.length, selectedIndex, range);

  useEffect(() => {
    if (!hidden && onVisibleRangeChange) {
      onVisibleRangeChange({ start: range.start, end: range.end });
    }
  }, [hidden, onVisibleRangeChange, range.end, range.start]);

  // 取消隐藏或表头列布局变更时，主动同步表头水平滚动偏移，防止列错位
  useLayoutEffect(() => {
    if (!hidden && listRef.current && onHorizontalScroll) {
      onHorizontalScroll(listRef.current.scrollLeft);
    }
  }, [hidden, onHorizontalScroll, tableGridTemplate, tableMinWidthPx]);

  const updateViewport = useCallback(() => {
    if (listRef.current) setViewportHeight(listRef.current.clientHeight);
  }, []);

  useLayoutEffect(() => {
    if (hidden) return;
    updateViewport();
    const observer = new ResizeObserver(updateViewport);
    if (listRef.current) observer.observe(listRef.current);
    return () => observer.disconnect();
  }, [hidden, updateViewport]);

  useLayoutEffect(() => {
    if (hidden) return;
    let active = true;
    const observer = new ResizeObserver((entries) => {
      if (!active) return;
      let changed = false;
      const currentLayout = layoutRef.current;
      const currentResults = resultsRef.current;
      const heightCache = measuredHeightsRef.current;
      for (const entry of entries) {
        const targetEl = entry.target as HTMLElement;
        const index = Number(targetEl.dataset.virtualIndex);
        const targetPath = targetEl.dataset.virtualPath;
        const height = Math.ceil(entry.borderBoxSize[0]?.blockSize ?? entry.contentRect.height);
        if (!Number.isInteger(index) || index < 0 || index >= currentResults.length || height <= 0) {
          continue;
        }
        const rowItem = currentResults[index];
        // 严格校验 DOM 绑定路径与当前结果集同槽位路径一致，防止异步测量污染新结果
        if (targetPath && rowItem.path !== targetPath) {
          continue;
        }
        if (heightCache.has(rowItem.path)) {
          heightCache.delete(rowItem.path);
        }
        heightCache.set(rowItem.path, height);
        while (heightCache.size > MAX_MEASURED_HEIGHTS_CACHE) {
          const oldestKey = heightCache.keys().next().value;
          if (oldestKey === undefined) break;
          heightCache.delete(oldestKey);
        }
        changed = currentLayout.updateHeight(index, height) || changed;
      }
      if (changed) setLayoutRevision((previous) => previous + 1);
    });
    observerRef.current = observer;
    listRef.current?.querySelectorAll<HTMLLIElement>('[data-virtual-index]').forEach((element) => observer.observe(element));
    return () => {
      active = false;
      observer.disconnect();
      observerRef.current = null;
    };
  }, [hidden]);

  const measureRef = useCallback((element: HTMLLIElement | null) => {
    const observer = observerRef.current;
    if (!element || !observer) return;
    observer.observe(element);
    return () => observer.unobserve(element);
  }, []);

  useLayoutEffect(() => {
    if (hidden) return;
    const list = listRef.current;
    if (!list || selectedIndex < 0 || selectedIndex >= results.length) return;

    const selectionChanged = prevSelectedIndexRef.current !== selectedIndex;
    const resultsChanged = prevResultsRef.current !== results;
    const keywordsChanged = prevKeywordsKeyRef.current !== keywordsKey;
    prevSelectedIndexRef.current = selectedIndex;
    prevResultsRef.current = results;
    prevKeywordsKeyRef.current = keywordsKey;

    if (isScrollingRef.current) return;
    if (!selectionChanged && !resultsChanged) return;
    if (!selectionChanged && !keywordsChanged) return;

    const itemTop = layout.offsetOf(selectedIndex);
    const itemBottom = layout.offsetOf(selectedIndex + 1);
    const visibleBottom = list.scrollTop + list.clientHeight;
    let nextScrollTop = list.scrollTop;
    if (selectedIndex === 0 && keywordsChanged) {
      nextScrollTop = 0;
    } else if (itemTop < list.scrollTop) {
      nextScrollTop = itemTop;
    } else if (list.clientHeight > 0 && itemBottom > visibleBottom) {
      nextScrollTop = itemBottom - list.clientHeight;
    }
    if (nextScrollTop !== list.scrollTop) {
      list.scrollTop = nextScrollTop;
      setScrollTop(nextScrollTop);
    }
  }, [hidden, keywordsKey, layout, results, selectedIndex]);

  const handleScroll = useCallback((event: UIEvent<HTMLUListElement>) => {
    isScrollingRef.current = true;
    if (scrollEndTimerRef.current) {
      clearTimeout(scrollEndTimerRef.current);
    }
    scrollEndTimerRef.current = setTimeout(() => {
      isScrollingRef.current = false;
    }, 150);
    setScrollTop(event.currentTarget.scrollTop);
    if (onHorizontalScroll) {
      onHorizontalScroll(event.currentTarget.scrollLeft);
    }
  }, [onHorizontalScroll]);

  const getIconBase64 = useCallback((item: SearchResult) => {
    const spec = resolveNativeIconRequest(item);
    return iconCache[spec.cacheKey] || (spec.extension ? iconCache[spec.extension] : undefined) || undefined;
  }, [iconCache]);

  useEffect(() => {
    if (hidden || iconStyle !== 'native' || !onRequestIcon) return;
    const requestedInPass = new Set<string>();
    const requestIfMissing = (item: SearchResult | undefined) => {
      if (!item) return;
      const spec = resolveNativeIconRequest(item);
      if (requestedInPass.has(spec.cacheKey)) return;
      if (Object.prototype.hasOwnProperty.call(iconCache, spec.cacheKey)) return;
      requestedInPass.add(spec.cacheKey);
      if (spec.filePath) {
        onRequestIcon(spec.extension, item.isDirectory, spec.filePath);
      } else {
        onRequestIcon(spec.extension, item.isDirectory);
      }
    };

    for (let i = range.start; i < range.end && i < results.length; i += 1) {
      requestIfMissing(results[i]);
    }
    if (selectedOutsideRange) {
      requestIfMissing(results[selectedIndex]);
    }
  }, [hidden, iconCache, iconStyle, onRequestIcon, range.end, range.start, results, selectedIndex, selectedOutsideRange]);

  // 精确对齐像素：数据行内含 12px*2 padding + 1px*2 border = 26px；外层加 left/right 各 8px = 42px
  const tableSpacerMinWidth = tableMinWidthPx ? `${tableMinWidthPx + 42}px` : undefined;
  const rowWidthStyle = tableMinWidthPx
    ? { minWidth: `${tableMinWidthPx + 26}px` }
    : undefined;

  return (
    <ul
      id="search-results"
      ref={listRef}
      className={`search-results search-results--virtualized density-${density} view-table`}
      role="listbox"
      style={{ display: hidden ? 'none' : undefined }}
      onScroll={handleScroll}
    >
      <li
        aria-hidden="true"
        role="presentation"
        style={{ height: layout.totalHeight(), minWidth: tableSpacerMinWidth }}
      />
      {results.slice(range.start, range.end).map((result, relativeIndex) => {
        const index = range.start + relativeIndex;
        return (
          <SearchResultRow
            key={result.path}
            result={result}
            index={index}
            selected={index === selectedIndex}
            density={density}
            tableGridTemplate={tableGridTemplate}
            tableMinWidthPx={tableMinWidthPx}
            iconStyle={iconStyle}
            iconBase64={getIconBase64(result)}
            columns={columns}
            rowProperties={propertyCache?.[result.path]}
            queryKeywords={queryKeywords}
            onHover={onHover}
            onSelect={onSelect}
            onOpen={onOpen}
            onContextMenu={onContextMenu}
            measureRef={measureRef}
            setSize={results.length}
            style={{ position: 'absolute', top: layout.offsetOf(index), left: 0, right: 0, ...rowWidthStyle }}
          />
        );
      })}
      {selectedOutsideRange && (
        <SearchResultRow
          key={`selected-${results[selectedIndex].path}`}
          result={results[selectedIndex]}
          index={selectedIndex}
          selected
          density={density}
          tableGridTemplate={tableGridTemplate}
          tableMinWidthPx={tableMinWidthPx}
          iconStyle={iconStyle}
          iconBase64={getIconBase64(results[selectedIndex])}
          columns={columns}
          rowProperties={propertyCache?.[results[selectedIndex].path]}
          queryKeywords={queryKeywords}
          onHover={onHover}
          onSelect={onSelect}
          onOpen={onOpen}
          onContextMenu={onContextMenu}
          measureRef={measureRef}
          setSize={results.length}
          style={{ position: 'absolute', top: layout.offsetOf(selectedIndex), left: 0, right: 0, ...rowWidthStyle }}
        />
      )}
    </ul>
  );
});

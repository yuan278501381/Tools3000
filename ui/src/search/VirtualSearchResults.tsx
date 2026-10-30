import {
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
import type {
  ColumnLayout,
  SearchDensity,
  SearchIconStyle,
  SearchResult,
} from './searchTypes';

export interface VirtualSearchResultsProps {
  results: SearchResult[];
  selectedIndex: number;
  density: SearchDensity;
  iconStyle?: SearchIconStyle;
  iconCache?: Record<string, string>;
  onRequestIcon?: (ext: string, isDirectory: boolean) => void;
  columns: ColumnLayout;
  queryKeywords: string[];
  onHover: (index: number) => void;
  onSelect: (index: number) => void;
  onOpen: (result: SearchResult) => void;
  onContextMenu: (event: ReactMouseEvent, index: number, result: SearchResult) => void;
  hidden?: boolean;
}

/** A dynamic-height virtual list that measures only rows near the viewport. */
export function VirtualSearchResults({
  results,
  selectedIndex,
  density,
  iconStyle = 'native',
  iconCache = {},
  onRequestIcon,
  columns,
  queryKeywords,
  onHover,
  onSelect,
  onOpen,
  onContextMenu,
  hidden = false,
}: VirtualSearchResultsProps) {
  const listRef = useRef<HTMLUListElement>(null);
  const observerRef = useRef<ResizeObserver | null>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewportHeight, setViewportHeight] = useState(480);
  const [layoutRevision, setLayoutRevision] = useState(0);

  const prevSelectedIndexRef = useRef(selectedIndex);
  const prevResultsRef = useRef(results);
  const isScrollingRef = useRef(false);
  const scrollEndTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    return () => {
      if (scrollEndTimerRef.current) {
        clearTimeout(scrollEndTimerRef.current);
      }
    };
  }, []);

  // 渲染帧同步重置：当搜索结果集或显示状态变更且首项选中时，立即将滚动偏移同步归零，彻底消灭首帧越界空白与闪跳
  const [prevResultsState, setPrevResultsState] = useState(results);
  const [prevHiddenState, setPrevHiddenState] = useState(hidden);
  if (prevResultsState !== results || prevHiddenState !== hidden) {
    setPrevResultsState(results);
    setPrevHiddenState(hidden);
    if (selectedIndex === 0 && scrollTop !== 0) {
      setScrollTop(0);
    }
  }

  // 精准校准不同密度模式的行高估算基线 (紧凑 32px / 标准 56px / 宽松 70px)
  // 与 CSS 中盒模型基准 1:1 绝对对齐，消除普通搜索行测高时反复抖动 Fenwick 树及重算 totalHeight
  const estimatedHeight = density === 'compact' ? 32 : density === 'comfortable' ? 70 : 56;
  const layout = useMemo(
    () => new DynamicRowLayout(results.length, estimatedHeight),
    [results, estimatedHeight],
  );

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
      for (const entry of entries) {
        const index = Number((entry.target as HTMLElement).dataset.virtualIndex);
        const height = Math.ceil(entry.borderBoxSize[0]?.blockSize ?? entry.contentRect.height);
        changed = layout.updateHeight(index, height) || changed;
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
  }, [hidden, layout]);

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
    prevSelectedIndexRef.current = selectedIndex;
    prevResultsRef.current = results;

    // 当用户正在主动拖拽滚动条或滚轮滚动时，绝不强行将视口扯回选区
    if (isScrollingRef.current) return;
    // 选区与结果集均未变更时，绝不因 DOM 行高测量与 layoutRevision 触发视口跳跃
    if (!selectionChanged && !resultsChanged) return;

    const itemTop = layout.offsetOf(selectedIndex);
    const itemBottom = layout.offsetOf(selectedIndex + 1);
    const visibleBottom = list.scrollTop + list.clientHeight;
    let nextScrollTop = list.scrollTop;
    if (selectedIndex === 0 && resultsChanged) {
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
  }, [hidden, layout, results, selectedIndex]);

  const handleScroll = useCallback((event: UIEvent<HTMLUListElement>) => {
    isScrollingRef.current = true;
    if (scrollEndTimerRef.current) {
      clearTimeout(scrollEndTimerRef.current);
    }
    scrollEndTimerRef.current = setTimeout(() => {
      isScrollingRef.current = false;
    }, 150);
    setScrollTop(event.currentTarget.scrollTop);
  }, []);

  const getIconBase64 = useCallback((item: SearchResult) => {
    const dotIndex = item.name.lastIndexOf('.');
    const extension = !item.isDirectory && dotIndex >= 0 ? item.name.slice(dotIndex).toLowerCase() : '';
    const cacheKey = item.isDirectory ? '::dir::' : extension;
    return iconCache[cacheKey] || undefined;
  }, [iconCache]);

  useEffect(() => {
    if (hidden || iconStyle !== 'native' || !onRequestIcon) return;
    const requestedInPass = new Set<string>();
    const requestIfMissing = (item: SearchResult | undefined) => {
      if (!item) return;
      const dotIndex = item.name.lastIndexOf('.');
      const extension = !item.isDirectory && dotIndex >= 0 ? item.name.slice(dotIndex).toLowerCase() : '';
      const cacheKey = item.isDirectory ? '::dir::' : extension;
      if (requestedInPass.has(cacheKey)) return;
      if (Object.prototype.hasOwnProperty.call(iconCache, cacheKey)) return;
      requestedInPass.add(cacheKey);
      onRequestIcon(extension, item.isDirectory);
    };

    for (let i = range.start; i < range.end && i < results.length; i += 1) {
      requestIfMissing(results[i]);
    }
    if (selectedOutsideRange) {
      requestIfMissing(results[selectedIndex]);
    }
  }, [hidden, iconCache, iconStyle, onRequestIcon, range.end, range.start, results, selectedIndex, selectedOutsideRange]);

  return (
    <ul
      id="search-results"
      ref={listRef}
      className={`search-results search-results--virtualized density-${density}`}
      role="listbox"
      style={{ display: hidden ? 'none' : undefined }}
      onScroll={handleScroll}
    >
      <li aria-hidden="true" role="presentation" style={{ height: layout.totalHeight() }} />
      {results.slice(range.start, range.end).map((result, relativeIndex) => {
        const index = range.start + relativeIndex;
        return (
          <SearchResultRow
            key={result.path}
            result={result}
            index={index}
            selected={index === selectedIndex}
            density={density}
            iconStyle={iconStyle}
            iconBase64={getIconBase64(result)}
            columns={columns}
            queryKeywords={queryKeywords}
            onHover={onHover}
            onSelect={onSelect}
            onOpen={onOpen}
            onContextMenu={onContextMenu}
            measureRef={measureRef}
            setSize={results.length}
            style={{ position: 'absolute', top: layout.offsetOf(index), left: 0, right: 0 }}
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
          iconStyle={iconStyle}
          iconBase64={getIconBase64(results[selectedIndex])}
          columns={columns}
          queryKeywords={queryKeywords}
          onHover={onHover}
          onSelect={onSelect}
          onOpen={onOpen}
          onContextMenu={onContextMenu}
          measureRef={measureRef}
          setSize={results.length}
          style={{ position: 'absolute', top: layout.offsetOf(selectedIndex), left: 0, right: 0 }}
        />
      )}
    </ul>
  );
}

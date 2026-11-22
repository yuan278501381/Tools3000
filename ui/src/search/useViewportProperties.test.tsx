/* @vitest-environment jsdom */

import { act, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const bridgeRequestMock = vi.fn();

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
}));

import { useViewportProperties } from './useViewportProperties';
import type { PropertyCellValue, SearchResult } from './searchTypes';

function makeItems(count: number, prefix = 'D:\\Photos\\img'): SearchResult[] {
  return Array.from({ length: count }, (_, i) => ({
    name: `img_${i}.png`,
    path: `${prefix}_${i}.png`,
    isDirectory: false,
    size: 1000 + i,
    lastWriteTime: 134000000000000000 + i,
  }));
}

describe('useViewportProperties reliability & anti-regression', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    bridgeRequestMock.mockReset();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('does not self-cancel sort extraction when sortedResults rerenders after batch 1 commits', async () => {
    const sourceItems = makeItems(80);
    let batchCount = 0;

    bridgeRequestMock.mockImplementation(async (method: string, params: { items: Array<{ path: string }>; properties: string[] }) => {
      if (method === 'search.batchGetProperties') {
        batchCount += 1;
        const values: Record<string, Record<string, PropertyCellValue>> = {};
        for (const it of params.items) {
          values[it.path] = { dimensions: { d: '1920 x 1080', n: 2073600 } };
        }
        return { success: true, values };
      }
      return {};
    });

    const { result, rerender } = renderHook(
      ({ sorted, source }) =>
        useViewportProperties({
          sourceResults: source,
          results: sorted,
          visibleColumns: ['name', 'dimensions'],
          visibleRange: { start: 0, end: 10 },
          enabled: false,
        }),
      { initialProps: { sorted: sourceItems, source: sourceItems } },
    );

    let finished = false;
    const promise = act(async () => {
      // 第一批提交后外部 sortedResults 产生全新数组引用，sourceResults 保持不变
      const p = result.current.ensureColumnLoadedForSort('dimensions', 'Dimensions', sourceItems);
      await Promise.resolve();
      rerender({ sorted: [...sourceItems].reverse(), source: sourceItems });
      finished = await p;
    });
    await promise;

    expect(finished).toBe(true);
    expect(batchCount).toBe(2); // 48 + 32 全部完整拉完，绝不在第一批后自杀
    expect(Object.keys(result.current.getLatestPropertyCache())).toHaveLength(80);
    expect(result.current.sortProgress).toBeNull();
  });

  it('immutably replaces per-row props object so React.memo detects newly extracted columns', async () => {
    const items = makeItems(2);
    let callIdx = 0;
    bridgeRequestMock.mockImplementation(async (_method: string, params: { items: Array<{ path: string }>; properties: string[] }) => {
      callIdx += 1;
      const prop = params.properties[0];
      const values: Record<string, Record<string, PropertyCellValue>> = {};
      for (const it of params.items) {
        values[it.path] = { [prop]: { d: `val-${callIdx}`, n: callIdx } };
      }
      return { success: true, values };
    });

    const { result } = renderHook(() =>
      useViewportProperties({
        sourceResults: items,
        results: items,
        visibleColumns: ['name', 'dimensions', 'md5'],
        visibleRange: { start: 0, end: 2 },
        enabled: false,
      }),
    );

    await act(async () => {
      await result.current.ensureColumnLoadedForSort('dimensions', 'Dimensions', items);
    });
    const firstRowRef = result.current.propertyCache[items[0].path];
    expect(firstRowRef?.dimensions?.d).toBe('val-1');

    await act(async () => {
      await result.current.ensureColumnLoadedForSort('md5', 'MD5', items);
    });
    const updatedRowRef = result.current.propertyCache[items[0].path];
    expect(updatedRowRef).not.toBe(firstRowRef); // 必须产生新对象引用以触发 React.memo 更新
    expect(updatedRowRef?.dimensions?.d).toBe('val-1');
    expect(updatedRowRef?.md5?.d).toBe('val-2');
  });

  it('advances queue strictly by returned keys when backend responds with budgetExceeded=true', async () => {
    const items = makeItems(12);
    bridgeRequestMock.mockImplementation(async (_method: string, params: { items: Array<{ path: string }> }) => {
      // 模拟单批时间预算仅处理前 5 条并置 budgetExceeded=true
      const slice = params.items.slice(0, 5);
      const values: Record<string, Record<string, PropertyCellValue>> = {};
      for (const it of slice) {
        values[it.path] = { sha256: { d: `hash:${it.path}` } };
      }
      return {
        success: true,
        values,
        budgetExceeded: slice.length < params.items.length,
      };
    });

    const { result } = renderHook(() =>
      useViewportProperties({
        sourceResults: items,
        results: items,
        visibleColumns: ['name', 'sha256'],
        visibleRange: { start: 0, end: 12 },
        enabled: false,
      }),
    );

    let ok = false;
    await act(async () => {
      ok = await result.current.ensureColumnLoadedForSort('sha256', 'SHA-256', items);
    });

    expect(ok).toBe(true);
    // 12 条每批 5 条 -> 5 + 5 + 2 = 3 次调用，零漏条
    expect(bridgeRequestMock).toHaveBeenCalledTimes(3);
    const cache = result.current.getLatestPropertyCache();
    for (const item of items) {
      expect(cache[item.path]?.sha256?.d).toBe(`hash:${item.path}`);
    }
  });

  it('automatically drains remaining visible rows when viewport exceeds 48-item batch size', async () => {
    const items = makeItems(64);
    bridgeRequestMock.mockImplementation(async (_method: string, params: { items: Array<{ path: string }> }) => {
      const values: Record<string, Record<string, PropertyCellValue>> = {};
      for (const it of params.items) {
        values[it.path] = { type: { d: 'PNG Image' } };
      }
      return { success: true, values };
    });

    const { result } = renderHook(() =>
      useViewportProperties({
        sourceResults: items,
        results: items,
        visibleColumns: ['name', 'type'],
        visibleRange: { start: 0, end: 64 },
        enabled: true,
      }),
    );

    // 第一拍 35ms 触发前 48 条并提交状态、排程下一拍
    await act(async () => {
      await vi.advanceTimersByTimeAsync(50);
    });
    // 第二拍 35ms 自动补齐可见窗口内剩余 16 条
    await act(async () => {
      await vi.advanceTimersByTimeAsync(50);
    });

    expect(bridgeRequestMock).toHaveBeenCalledTimes(2);
    expect(Object.keys(result.current.getLatestPropertyCache())).toHaveLength(64);
  });

  it('extracts accessed and attributes on demand only for items missing native metadata', async () => {
    const items: SearchResult[] = [
      { name: 'has.txt', path: 'D:\\has.txt', isDirectory: false, lastAccessTime: 134000000000000000, attributes: 0x20 },
      { name: 'missing.txt', path: 'D:\\missing.txt', isDirectory: false },
    ];
    bridgeRequestMock.mockImplementation(async (_method: string, params: { items: Array<{ path: string }> }) => {
      const values: Record<string, Record<string, PropertyCellValue>> = {};
      for (const it of params.items) {
        values[it.path] = {
          accessed: { d: '2026-10-01 12:00', n: 134300000000000000 },
          attributes: { d: 'A' },
        };
      }
      return { success: true, values };
    });

    const { result } = renderHook(() =>
      useViewportProperties({
        sourceResults: items,
        results: items,
        visibleColumns: ['name', 'accessed', 'attributes'],
        visibleRange: { start: 0, end: 2 },
        enabled: true,
      }),
    );

    await act(async () => {
      await vi.advanceTimersByTimeAsync(80);
    });

    expect(bridgeRequestMock).toHaveBeenCalledTimes(1);
    const reqItems = bridgeRequestMock.mock.calls[0][1].items as Array<{ path: string }>;
    expect(reqItems.map((x) => x.path)).toEqual(['D:\\missing.txt']);
    expect(result.current.propertyCache['D:\\missing.txt']?.attributes?.d).toBe('A');
  });
});

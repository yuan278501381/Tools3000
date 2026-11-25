/* @vitest-environment jsdom */

import { act, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: vi.fn(),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
    info: vi.fn(),
    loading: vi.fn(),
  },
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, defaultVal?: string | Record<string, unknown>, opts?: Record<string, unknown>) => {
      if (key === 'search.colDimensions') return 'Dimensions, "Px"';
      const template = typeof defaultVal === 'string' ? defaultVal : key;
      const vars = (typeof defaultVal === 'object' && defaultVal !== null ? defaultVal : opts) ?? {};
      return template.replace(/\{\{(\w+)\}\}/g, (_, k) => String(vars[k] ?? ''));
    },
  }),
}));

import { bridgeRequest } from '../hooks/useBridge';
import { useSearchActions } from './useSearchActions';
import type { PropertyCacheMap, SearchResult } from './searchTypes';

describe('useSearchActions CSV export', () => {
  let capturedCsv = '';
  const origBlob = globalThis.Blob;
  const origCreateObjectURL = URL.createObjectURL;
  const origRevokeObjectURL = URL.revokeObjectURL;

  beforeEach(() => {
    capturedCsv = '';
    class MockBlob extends origBlob {
      constructor(parts: BlobPart[] = [], options?: BlobPropertyBag) {
        super(parts, options);
        capturedCsv = parts.map((p) => String(p)).join('');
      }
    }
    vi.stubGlobal('Blob', MockBlob);
    URL.createObjectURL = vi.fn(() => 'blob:mock-csv');
    URL.revokeObjectURL = vi.fn();
    vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => undefined);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    URL.createObjectURL = origCreateObjectURL;
    URL.revokeObjectURL = origRevokeObjectURL;
    vi.restoreAllMocks();
  });

  it('exports strictly in visibleTableColumns order with escaped headers, zero duplicate type columns, and pre-fetches uncached backend columns', async () => {
    const items: SearchResult[] = [
      {
        name: 'hero, "v1".png',
        path: 'D:\\Assets\\hero, "v1".png',
        isDirectory: false,
        size: 2048,
      },
    ];

    let liveCache: PropertyCacheMap = {};
    const ensureColumnLoadedForSort = vi.fn(async (colId: string) => {
      if (colId === 'type') {
        liveCache = {
          ...liveCache,
          'D:\\Assets\\hero, "v1".png': {
            ...(liveCache['D:\\Assets\\hero, "v1".png'] ?? {}),
            type: { d: 'PNG Image' },
          },
        };
      } else if (colId === 'dimensions') {
        liveCache = {
          ...liveCache,
          'D:\\Assets\\hero, "v1".png': {
            ...(liveCache['D:\\Assets\\hero, "v1".png'] ?? {}),
            dimensions: { d: '1920 x 1080', n: 2073600 },
          },
        };
      }
      return true;
    });

    const { result } = renderHook(() =>
      useSearchActions({
        isPinned: false,
        hide: vi.fn(),
        setActionError: vi.fn(),
        setContextMenu: vi.fn(),
        renameTarget: { visible: false, newName: '' },
        setRenameTarget: vi.fn(),
        renameInputRef: { current: null },
        setResults: vi.fn(),
        query: 'hero',
        sortedResults: items,
        visibleTableColumns: ['name', 'type', 'dimensions', 'size'],
        propertyCache: {},
        getLatestPropertyCache: () => liveCache,
        ensureColumnLoadedForSort,
      }),
    );

    await act(async () => {
      await result.current.exportResultsToCsv();
    });

    expect(ensureColumnLoadedForSort).toHaveBeenCalledTimes(2);
    expect(ensureColumnLoadedForSort).toHaveBeenNthCalledWith(1, 'type', 'Type', items);
    expect(ensureColumnLoadedForSort).toHaveBeenNthCalledWith(2, 'dimensions', 'Dimensions, "Px"', items);

    // 校验 BOM、严格列序、表头逗号与引号转义、以及零重复 Type 列
    expect(capturedCsv.startsWith('\uFEFF')).toBe(true);
    const lines = capturedCsv.slice(1).split('\r\n');
    expect(lines).toHaveLength(2);
    expect(lines[0]).toBe('"File Name","Type","Dimensions, ""Px""","Size"');
    expect(lines[1]).toBe('"hero, ""v1"".png","PNG Image","1920 x 1080","2.0 KB"');
  });
});

describe('useSearchActions hideOnOpen behavior', () => {
  const sampleItem: SearchResult = {
    name: 'readme.md',
    path: 'C:\\repo\\readme.md',
    isDirectory: false,
    size: 512,
  };

  beforeEach(() => {
    vi.mocked(bridgeRequest).mockReset();
  });

  it('hides after opening a result when hideOnOpen is enabled and window is not pinned', async () => {
    vi.mocked(bridgeRequest).mockResolvedValue({ success: true });
    const hide = vi.fn();

    const { result } = renderHook(() =>
      useSearchActions({
        isPinned: false,
        hideOnOpen: true,
        hide,
        setActionError: vi.fn(),
        setContextMenu: vi.fn(),
        renameTarget: { visible: false, newName: '' },
        setRenameTarget: vi.fn(),
        renameInputRef: { current: null },
        setResults: vi.fn(),
        query: 'readme',
        sortedResults: [sampleItem],
      }),
    );

    await act(async () => {
      await result.current.openResult(sampleItem);
    });

    expect(hide).toHaveBeenCalledTimes(1);
  });

  it('keeps window open when pinned, when hideOnOpen is false, or when open fails', async () => {
    vi.mocked(bridgeRequest).mockResolvedValue({ success: true });
    const hidePinned = vi.fn();
    const { result: pinnedHook } = renderHook(() =>
      useSearchActions({
        isPinned: true,
        hideOnOpen: true,
        hide: hidePinned,
        setActionError: vi.fn(),
        setContextMenu: vi.fn(),
        renameTarget: { visible: false, newName: '' },
        setRenameTarget: vi.fn(),
        renameInputRef: { current: null },
        setResults: vi.fn(),
        query: 'readme',
        sortedResults: [sampleItem],
      }),
    );
    await act(async () => {
      await pinnedHook.current.openResult(sampleItem);
    });
    expect(hidePinned).not.toHaveBeenCalled();

    vi.mocked(bridgeRequest).mockResolvedValue({ success: false, error: 'not found' });
    const hideOnFail = vi.fn();
    const setActionError = vi.fn();
    const { result: failHook } = renderHook(() =>
      useSearchActions({
        isPinned: false,
        hideOnOpen: true,
        hide: hideOnFail,
        setActionError,
        setContextMenu: vi.fn(),
        renameTarget: { visible: false, newName: '' },
        setRenameTarget: vi.fn(),
        renameInputRef: { current: null },
        setResults: vi.fn(),
        query: 'readme',
        sortedResults: [sampleItem],
      }),
    );
    await act(async () => {
      await failHook.current.openResult(sampleItem);
    });
    expect(hideOnFail).not.toHaveBeenCalled();
    expect(setActionError).toHaveBeenCalled();
  });
});

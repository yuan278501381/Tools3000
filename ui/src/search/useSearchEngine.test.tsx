/* @vitest-environment jsdom */

import { act, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const bridgeRequestMock = vi.fn();

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
}));

import { isUnboundedFullDiskContentQuery, useSearchEngine } from './useSearchEngine';

describe('useSearchEngine service wakeup stopwatch & recovery', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    bridgeRequestMock.mockReset();
    localStorage.clear();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('keeps scanElapsedSeconds monotonically increasing across service-starting retries without resetting at 0.9s', async () => {
    let callCount = 0;
    bridgeRequestMock.mockImplementation(async (method: string) => {
      if (method === 'search.query') {
        callCount += 1;
        if (callCount <= 5) {
          return {
            results: [],
            available: false,
            status: 'starting',
            error: 'search service starting',
          };
        }
        return {
          results: [
            {
              name: '纳百川-采购阶梯价V10001.ch',
              path: 'D:\\Chosen\\201-温州纳百川\\111-采购阶梯价\\纳百川-采购阶梯价V10001.ch',
              isDirectory: false,
              size: 4096,
              lastWriteTime: 133500000000000000,
            },
          ],
          available: true,
          status: 'ready',
          totalIndexedFiles: 3024296,
          elapsedMs: 52,
        };
      }
      return {};
    });

    const { result } = renderHook(() =>
      useSearchEngine({
        query: '纳百川 阶梯价',
        isComposing: false,
        activeCategory: 'all',
        searchMode: 'name',
        enabledDrives: [],
        maxResultLimit: 100,
        excludeGitAndModules: true,
        excludeHidden: false,
        customContentFormats: [],
        disabledContentFormats: [],
      }),
    );

    const observedElapsed: number[] = [];
    for (let step = 0; step < 20; step += 1) {
      await act(async () => {
        await vi.advanceTimersByTimeAsync(100);
      });
      if (result.current.loading) {
        observedElapsed.push(result.current.scanElapsedSeconds);
      }
    }

    // Must have retried multiple times across ~2.0s AND never reset back to 0.0/0.1 mid-retry
    expect(callCount).toBeGreaterThanOrEqual(4);
    expect(observedElapsed.length).toBeGreaterThanOrEqual(15);
    for (let i = 1; i < observedElapsed.length; i += 1) {
      expect(observedElapsed[i]).toBeGreaterThanOrEqual(observedElapsed[i - 1]);
    }
    expect(observedElapsed[observedElapsed.length - 1]).toBeGreaterThanOrEqual(1.8);

    // Advance past final retry until service returns ready results
    await act(async () => {
      await vi.advanceTimersByTimeAsync(1200);
    });

    expect(result.current.isServiceStarting).toBe(false);
    expect(result.current.serviceAvailable).toBe(true);
    expect(result.current.loading).toBe(false);
    expect(result.current.results).toHaveLength(1);
    expect(result.current.results[0].name).toBe('纳百川-采购阶梯价V10001.ch');
  });

  it('does not disguise query timeout as service starting or leak stale starting state across new queries', async () => {
    bridgeRequestMock.mockRejectedValueOnce(new Error('IPC timeout: search.query (20000ms)'));

    const { result, rerender } = renderHook(
      ({ q }) =>
        useSearchEngine({
          query: q,
          isComposing: false,
          activeCategory: 'all',
          searchMode: 'name',
          enabledDrives: [],
          maxResultLimit: 100,
          excludeGitAndModules: true,
          excludeHidden: false,
          customContentFormats: [],
          disabledContentFormats: [],
        }),
      { initialProps: { q: '建邦 账号密码' } },
    );

    await act(async () => {
      await vi.advanceTimersByTimeAsync(200);
    });

    // Must not mislabel a timeout as "connecting to index service" or trigger 90-retry storm
    expect(result.current.isServiceStarting).toBe(false);
    expect(result.current.loading).toBe(false);
    expect(bridgeRequestMock).toHaveBeenCalledTimes(1);

    // Switching to a fresh query must immediately start cleanly without stale starting banner
    bridgeRequestMock.mockResolvedValueOnce({
      results: [{ name: '建邦账号密码.txt', path: 'D:\\docs\\建邦账号密码.txt', isDirectory: false }],
      available: true,
      status: 'ready',
      totalIndexedFiles: 2880000,
      elapsedMs: 12,
    });
    rerender({ q: '建邦' });
    await act(async () => {
      await vi.advanceTimersByTimeAsync(200);
    });
    expect(result.current.isServiceStarting).toBe(false);
    expect(result.current.results).toHaveLength(1);
  });

  it('classifies unbounded full-disk content queries vs scoped queries accurately', () => {
    expect(isUnboundedFullDiskContentQuery('content:建邦 content:账号密码')).toBe(true);
    expect(isUnboundedFullDiskContentQuery('content:合同 path:D:\\Chosen')).toBe(false);
    expect(isUnboundedFullDiskContentQuery('content:合同 ext:docx')).toBe(false);
    expect(isUnboundedFullDiskContentQuery('content:合同 D:\\docs\\')).toBe(false);
    expect(isUnboundedFullDiskContentQuery('content:合同 *.md')).toBe(false);
  });

  it('soft-pauses unbounded full-disk content scan after 20s and resumes seamlessly on user request', async () => {
    let sliceIndex = 0;
    bridgeRequestMock.mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'search.cancel') return { cancelled: true };
      if (method === 'search.query') {
        sliceIndex += 1;
        if (sliceIndex === 1) {
          return {
            results: [{ name: '流程1.docx', path: 'D:\\docs\\流程1.docx', isDirectory: false }],
            available: true,
            status: 'ready',
            hasMore: true,
            planId: 77,
            nextCursor: 12000,
            totalCandidates: 120000,
            elapsedMs: 11000,
          };
        }
        if (sliceIndex === 2) {
          return {
            results: [{ name: '流程2.docx', path: 'D:\\docs\\流程2.docx', isDirectory: false }],
            available: true,
            status: 'ready',
            hasMore: true,
            planId: 77,
            nextCursor: 24000,
            totalCandidates: 120000,
            elapsedMs: 10000,
          };
        }
        // Resume slice should carry planId=77 and cursor=24000
        expect(params?.planId).toBe(77);
        expect(params?.cursor).toBe(24000);
        return {
          results: [{ name: '流程3.docx', path: 'D:\\docs\\流程3.docx', isDirectory: false }],
          available: true,
          status: 'ready',
          hasMore: false,
          planId: 77,
          nextCursor: 120000,
          totalCandidates: 120000,
          elapsedMs: 9000,
        };
      }
      return {};
    });

    const { result } = renderHook(() =>
      useSearchEngine({
        query: 'content:建邦 content:账号密码',
        isComposing: false,
        activeCategory: 'content',
        searchMode: 'content',
        enabledDrives: [],
        maxResultLimit: 100,
        excludeGitAndModules: true,
        excludeHidden: false,
        customContentFormats: [],
        disabledContentFormats: [],
      }),
    );

    await act(async () => {
      await vi.advanceTimersByTimeAsync(1200);
    });

    // Stopped automatically at 21s (soft budget = 20s), preserving found results and signalling backend cancel
    expect(result.current.loading).toBe(false);
    expect(result.current.results).toHaveLength(2);
    expect(result.current.contentScanPaused).toEqual({
      reason: 'budget',
      scanned: 24000,
      total: 120000,
      elapsedMs: 21000,
    });
    expect(bridgeRequestMock).toHaveBeenCalledWith('search.cancel', {}, { silent: true });

    // User clicks "Continue" -> resumes from checkpoint cursor=24000 and finishes remaining slices
    await act(async () => {
      result.current.resumeContentScan();
      await vi.advanceTimersByTimeAsync(200);
    });

    expect(result.current.loading).toBe(false);
    expect(result.current.contentScanPaused).toBeNull();
    expect(result.current.results).toHaveLength(3);
  });

  it('pauses active content scan 3s after losing window focus and cancels immediately when window hides', async () => {
    let resolveSlice2: ((val: unknown) => void) | undefined;
    let queryCalls = 0;

    bridgeRequestMock.mockImplementation((method: string) => {
      if (method === 'search.cancel') return Promise.resolve({ cancelled: true });
      if (method === 'search.query') {
        queryCalls += 1;
        if (queryCalls === 1) {
          return Promise.resolve({
            results: [{ name: 'a.txt', path: 'D:\\a.txt', isDirectory: false }],
            available: true,
            status: 'ready',
            hasMore: true,
            planId: 88,
            nextCursor: 500,
            totalCandidates: 5000,
            elapsedMs: 300,
          });
        }
        return new Promise((resolve) => {
          resolveSlice2 = resolve;
        });
      }
      return Promise.resolve({});
    });

    const { result } = renderHook(() =>
      useSearchEngine({
        query: 'content:合同',
        isComposing: false,
        activeCategory: 'content',
        searchMode: 'content',
        enabledDrives: [],
        maxResultLimit: 100,
        excludeGitAndModules: true,
        excludeHidden: false,
        customContentFormats: [],
        disabledContentFormats: [],
      }),
    );

    await act(async () => {
      await vi.advanceTimersByTimeAsync(1000);
    });
    expect(result.current.loading).toBe(true);
    expect(result.current.results).toHaveLength(1);

    // Window loses focus -> after 3s automatically soft-pauses and sends search.cancel
    await act(async () => {
      window.dispatchEvent(new CustomEvent('tools3000:searchWindowActiveChanged', { detail: false }));
      await vi.advanceTimersByTimeAsync(3100);
    });

    expect(result.current.loading).toBe(false);
    expect(result.current.contentScanPaused?.reason).toBe('background');
    expect(result.current.results).toHaveLength(1);
    expect(bridgeRequestMock).toHaveBeenCalledWith('search.cancel', {}, { silent: true });

    // Resume then immediately hide window -> stops and sends search.cancel without clearing results
    bridgeRequestMock.mockClear();
    await act(async () => {
      result.current.resumeContentScan();
      await vi.advanceTimersByTimeAsync(50);
    });
    expect(result.current.loading).toBe(true);

    await act(async () => {
      window.dispatchEvent(new Event('tools3000:searchWindowHidden'));
    });

    expect(result.current.loading).toBe(false);
    expect(result.current.results).toHaveLength(1);
    expect(bridgeRequestMock).toHaveBeenCalledWith('search.cancel', {}, { silent: true });

    // Clean up pending promise
    const pendingResolve = resolveSlice2 as ((val: unknown) => void) | undefined;
    pendingResolve?.({ cancelled: true });
  });
});

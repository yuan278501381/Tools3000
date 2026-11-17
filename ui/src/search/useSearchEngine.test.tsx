/* @vitest-environment jsdom */

import { act, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const bridgeRequestMock = vi.fn();

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
}));

import { useSearchEngine } from './useSearchEngine';

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
});

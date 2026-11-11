/* @vitest-environment jsdom */

import { act, renderHook } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { bridgeRequest, resolveBridgeTimeoutMs, useBridge } from './useBridge';

describe('useBridge Single-Flight Deduplication', () => {
  it('deduplicates concurrent in-flight requests for general.getSettings', async () => {
    // 模拟并发调用：在同一个微任务周期内同时发起两次 general.getSettings
    const p1 = bridgeRequest('general.getSettings');
    const p2 = bridgeRequest('general.getSettings');

    // 关键断言：两次调用必须返回同一个在途 Promise 单飞锁实例
    expect(p1).toBe(p2);

    const [res1, res2] = await Promise.all([p1, p2]);
    expect(res1).toBeDefined();
    expect(res2).toBeDefined();
    expect(res1).toEqual(res2);

    // 请求完成后单飞锁重置，随后的新调用产生新的独立 Promise 实例
    const p3 = bridgeRequest('general.getSettings');
    expect(p3).not.toBe(p1);
    const res3 = await p3;
    expect(res3).toBeDefined();
  });

  it('does not deduplicate different methods', async () => {
    const p1 = bridgeRequest('capture.getSettings');
    const p2 = bridgeRequest('capture.getSettings');
    // capture.getSettings 未走 single-flight 锁，生成独立请求
    expect(p1).not.toBe(p2);
    await Promise.all([p1, p2]);
  });

  it('forwards BridgeRequestOptions (e.g. silent) through useBridge().request', async () => {
    const savedEvents: CustomEvent[] = [];
    const onSaved = (e: Event) => savedEvents.push(e as CustomEvent);
    window.addEventListener('tools3000:settings-saved', onSaved);

    try {
      const { result } = renderHook(() => useBridge());

      await act(async () => {
        await result.current.request('general.updateSettings', { theme: 'dark' }, { silent: true });
      });
      expect(savedEvents).toHaveLength(0);

      await act(async () => {
        await result.current.request(
          'general.updateSettings',
          { theme: 'light' },
          { toastMessage: 'Custom Saved' },
        );
      });
      expect(savedEvents).toHaveLength(1);
      expect(savedEvents[0].detail?.message).toBe('Custom Saved');
    } finally {
      window.removeEventListener('tools3000:settings-saved', onSaved);
    }
  });

  it('grants 5-minute timeout budget to dialog/network long-running methods', async () => {
    vi.useFakeTimers();
    const postMessage = vi.fn();
    window.chrome = {
      webview: {
        postMessage,
        addEventListener: vi.fn(),
      },
    };

    try {
      let settled = false;
      const promise = bridgeRequest('app.exportLogs').finally(() => {
        settled = true;
      });
      // Prevent unhandled rejection warning when fake timer triggers timeout
      const caught = promise.catch((err: unknown) => err);

      await vi.advanceTimersByTimeAsync(15_000);
      expect(settled).toBe(false);

      await vi.advanceTimersByTimeAsync(5 * 60_000);
      const err = await caught;
      expect(settled).toBe(true);
      expect(err).toBeInstanceOf(Error);
      expect((err as Error).message).toContain('Bridge request timeout: app.exportLogs');
    } finally {
      delete window.chrome;
      vi.useRealTimers();
    }
  });

  it('uses bounded 20s timeout for routine search.* calls while keeping long timeout for heavy index tasks', async () => {
    expect(resolveBridgeTimeoutMs('search.query', { query: 'foo.txt' })).toBe(20_000);
    expect(resolveBridgeTimeoutMs('search.getStatus')).toBe(20_000);
    expect(resolveBridgeTimeoutMs('search.getHistory')).toBe(20_000);
    expect(resolveBridgeTimeoutMs('search.query', { query: 'content:needle' })).toBe(5 * 60_000);
    expect(resolveBridgeTimeoutMs('search.rebuildIndex')).toBe(30 * 60_000);
    expect(resolveBridgeTimeoutMs('search.exportCsv')).toBe(30 * 60_000);

    vi.useFakeTimers();
    window.chrome = {
      webview: {
        postMessage: vi.fn(),
        addEventListener: vi.fn(),
      },
    };

    try {
      let settled = false;
      const promise = bridgeRequest('search.query', { query: 'readme' }).finally(() => {
        settled = true;
      });
      const caught = promise.catch((err: unknown) => err);

      await vi.advanceTimersByTimeAsync(19_000);
      expect(settled).toBe(false);

      await vi.advanceTimersByTimeAsync(2_000);
      const err = await caught;
      expect(settled).toBe(true);
      expect((err as Error).message).toContain('Bridge request timeout: search.query');
    } finally {
      delete window.chrome;
      vi.useRealTimers();
    }
  });
});


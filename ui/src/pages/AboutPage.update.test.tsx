/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { toast } from 'sonner';
import { AboutPage } from './AboutPage';
import * as bridge from '../hooks/useBridge';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, options?: { defaultValue?: string; version?: string } | string) => {
      if (typeof options === 'string') return options;
      if (options && typeof options === 'object' && options.version) {
        return `${key}:${options.version}`;
      }
      return key;
    },
    i18n: {
      language: 'zh-CN',
      changeLanguage: vi.fn(),
    },
  }),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
    info: vi.fn(),
    loading: vi.fn(),
  },
}));

describe('AboutPage update check error handling & toast deduplication', () => {
  let bridgeSpy: ReturnType<typeof vi.spyOn>;
  const eventHandlers = new Map<string, (data: unknown) => void>();

  beforeEach(() => {
    vi.clearAllMocks();
    eventHandlers.clear();
    vi.spyOn(bridge, 'useBridgeEvent').mockImplementation((eventName, handler) => {
      eventHandlers.set(eventName, handler);
    });
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      if (method === 'app.getSystemInfo') {
        return { version: '1.5.6', buildDate: '2026-11-28' };
      }
      if (method === 'app.getUpdateState') {
        return { status: 'idle' };
      }
      if (method === 'app.checkForUpdates') {
        return { success: true, started: true };
      }
      return { success: true };
    });
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('shows rateLimited copy with fixed toast id and distinguishes generic errors', async () => {
    await act(async () => {
      render(<AboutPage />);
    });

    const onUpdateResult = eventHandlers.get('update.result');
    expect(onUpdateResult).toBeDefined();

    await act(async () => {
      onUpdateResult?.({ status: 'error', error: 'rateLimited' });
    });
    expect(toast.error).toHaveBeenLastCalledWith('about.updateRateLimited', {
      id: 'about-update-check',
    });

    await act(async () => {
      onUpdateResult?.({ status: 'error', error: 'network' });
    });
    expect(toast.error).toHaveBeenLastCalledWith('about.updateFailed', {
      id: 'about-update-check',
    });

    await act(async () => {
      onUpdateResult?.({ status: 'error' });
    });
    expect(toast.error).toHaveBeenLastCalledWith('about.updateFailed', {
      id: 'about-update-check',
    });
  });

  it('uses fixed toast id across repeated clicks and all update check outcomes', async () => {
    await act(async () => {
      render(<AboutPage />);
    });

    const onUpdateResult = eventHandlers.get('update.result');
    expect(onUpdateResult).toBeDefined();

    await act(async () => {
      onUpdateResult?.({ status: 'available', hasUpdate: true, latestVersion: 'v1.5.7' });
    });
    expect(toast.success).toHaveBeenLastCalledWith('about.updateAvailable:v1.5.7', {
      id: 'about-update-check',
    });

    await act(async () => {
      onUpdateResult?.({ status: 'upToDate' });
    });
    expect(toast.success).toHaveBeenLastCalledWith('about.upToDate', {
      id: 'about-update-check',
    });

    await act(async () => {
      onUpdateResult?.({ status: 'unavailable' });
    });
    expect(toast.info).toHaveBeenLastCalledWith('about.noPublishedRelease', {
      id: 'about-update-check',
    });

    // Repeated busy/failed clicks reuse the same toast id so duplicate error toasts never stack
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'app.checkForUpdates') {
        return { success: true, started: false };
      }
      return { success: true };
    });

    const checkBtn = screen.getByRole('button', { name: 'about.checkUpdate' });
    await act(async () => {
      fireEvent.click(checkBtn);
    });
    expect(toast.info).toHaveBeenLastCalledWith('about.updateCheckBusy', {
      id: 'about-update-check',
    });

    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'app.checkForUpdates') {
        return { success: false };
      }
      return { success: true };
    });

    await act(async () => {
      fireEvent.click(checkBtn);
      fireEvent.click(checkBtn);
    });
    expect(toast.error).toHaveBeenLastCalledWith('about.updateFailed', {
      id: 'about-update-check',
    });
    for (const call of vi.mocked(toast.error).mock.calls) {
      expect(call[1]).toEqual({ id: 'about-update-check' });
    }
  });
});

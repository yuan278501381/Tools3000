/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { PerformancePage } from './PerformancePage';
import * as bridge from '../hooks/useBridge';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, opts?: { minutes?: number; defaultValue?: string } | string) => {
      if (typeof opts === 'object' && opts !== null && typeof opts.minutes === 'number') {
        return `${key}:${opts.minutes}`;
      }
      if (typeof opts === 'object' && opts?.defaultValue) return opts.defaultValue;
      if (typeof opts === 'string') return opts;
      return key;
    },
    i18n: { language: 'zh-CN' },
  }),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
  },
}));

describe('PerformancePage (Live Telemetry & Read-Only Memory Lifecycle Matrix)', () => {
  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      if (method === 'perf.getMetrics') {
        return {
          privateMemoryMB: 6.14,
          memoryMB: 12.94,
          cpuPercent: 0.3,
          screenshotLatencyMs: 0,
          gestureLatencyMs: 1.2,
          uiRenderLatencyMs: 2.8,
        };
      }
      if (method === 'general.getSettings') {
        return { autoReleaseSettingsMemory: false };
      }
      if (method === 'search.getSettings') {
        return { memoryMode: 'idle_stop', idleTrimMinutes: 5, idleStopMinutes: 45 };
      }
      if (method === 'perf.trimMemory') {
        return { success: true };
      }
      return {};
    });
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('renders live telemetry tiles, reflects linked settings in read-only schedule, marks kernel-locked guards, and triggers quiet trim', async () => {
    const onNavigate = vi.fn();

    await act(async () => {
      render(<PerformancePage onNavigate={onNavigate} />);
    });

    // Live metrics rendered
    expect(screen.getByText('performancePage.metricPrivateWs')).toBeDefined();
    expect(screen.getByText('performancePage.notTriggeredYet')).toBeDefined();

    // Linked settings dynamically reflected in read-only badges
    expect(screen.getByText('performancePage.itemSettingsValDisabled')).toBeDefined();
    expect(screen.getByText('performancePage.itemSearchSvcValStop:45')).toBeDefined();

    // Kernel-locked anti-thrash guard badge displayed
    expect(screen.getByText('performancePage.badgeKernelLocked')).toBeDefined();
    expect(screen.getByText('performancePage.itemHeapGuardVal')).toBeDefined();

    // Jump to linked settings
    const jumpBtns = screen.getAllByText('performancePage.jumpToSetting');
    expect(jumpBtns.length).toBe(2);
    await act(async () => {
      fireEvent.click(jumpBtns[0]);
    });
    expect(onNavigate).toHaveBeenCalledWith('general');

    await act(async () => {
      fireEvent.click(jumpBtns[1]);
    });
    expect(onNavigate).toHaveBeenCalledWith('search');

    // Trigger quiet trim IPC
    const trimBtn = screen.getByText('performancePage.trimNowBtn');
    await act(async () => {
      fireEvent.click(trimBtn);
    });
    expect(bridgeSpy).toHaveBeenCalledWith('perf.trimMemory');
  });
});

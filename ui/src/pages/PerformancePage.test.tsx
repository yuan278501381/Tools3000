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

describe('PerformancePage (Full-Transparency Memory Governance Matrix)', () => {
  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      if (method === 'general.getSettings') {
        return { autoReleaseSettingsMemory: false };
      }
      if (method === 'search.getSettings') {
        return { memoryMode: 'idle_stop', idleTrimMinutes: 5, idleStopMinutes: 45 };
      }
      if (method === 'windowManager.getSettings') {
        return {
          enabled: true,
          running: true,
          settings: { features: { snapZones: true } },
        };
      }
      return {};
    });
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('renders full read-only memory governance matrix across all 6 layers without telemetry polling', async () => {
    const onNavigate = vi.fn();

    await act(async () => {
      render(<PerformancePage onNavigate={onNavigate} />);
    });

    // Live telemetry switches & manual trim controls remain absent
    expect(screen.queryByRole('switch')).toBeNull();
    expect(bridgeSpy).not.toHaveBeenCalledWith('perf.getMetrics', expect.anything(), expect.anything());
    expect(bridgeSpy).not.toHaveBeenCalledWith('perf.trimMemory');

    // 3 core mental-model expectation pillars rendered
    expect(screen.getByText('performancePage.pillarZeroIdleTitle')).toBeDefined();
    expect(screen.getByText('performancePage.pillarBoundedPeakTitle')).toBeDefined();
    expect(screen.getByText('performancePage.pillarColdPathTitle')).toBeDefined();

    // All 6 architectural layers rendered
    expect(screen.getByText('performancePage.layerIsolation')).toBeDefined();
    expect(screen.getByText('performancePage.layerMediaBudget')).toBeDefined();
    expect(screen.getByText('performancePage.layerWebView')).toBeDefined();
    expect(screen.getByText('performancePage.layerAiModels')).toBeDefined();
    expect(screen.getByText('performancePage.layerGpuOverlays')).toBeDefined();
    expect(screen.getByText('performancePage.layerKernelGuard')).toBeDefined();

    // Non-timer policies (caps, isolation, instant release, watermark) are explicitly exposed
    expect(screen.getByText('performancePage.itemShortWorkersVal')).toBeDefined();
    expect(screen.getByText('performancePage.itemPinPixelPoolVal')).toBeDefined();
    expect(screen.getByText('performancePage.itemCaptureHistoryVal')).toBeDefined();
    expect(screen.getByText('performancePage.itemInstantOverlaysVal')).toBeDefined();
    expect(screen.getByText('performancePage.itemWatermarkGuardVal')).toBeDefined();

    // Linked settings dynamically reflected in read-only badges
    expect(screen.getByText('performancePage.itemSettingsValDisabled')).toBeDefined();
    expect(screen.getByText('performancePage.itemSearchSvcValStop:45')).toBeDefined();
    expect(screen.getByText('performancePage.itemWindowKitValActive')).toBeDefined();

    // Kernel-locked anti-thrash guard badges displayed (watermark + heap guard)
    expect(screen.getAllByText('performancePage.badgeKernelLocked').length).toBe(2);
    expect(screen.getByText('performancePage.itemHeapGuardVal')).toBeDefined();

    // Jump to linked settings (search, window_manager, general, etc.)
    const jumpBtns = screen.getAllByText('performancePage.jumpToSetting');
    expect(jumpBtns.length).toBeGreaterThanOrEqual(4);

    await act(async () => {
      fireEvent.click(jumpBtns[0]);
    });
    expect(onNavigate).toHaveBeenCalledWith('search');

    await act(async () => {
      fireEvent.click(jumpBtns[1]);
    });
    expect(onNavigate).toHaveBeenCalledWith('window_manager');
  });

  it('filters policies accurately by governance mechanism tabs', async () => {
    await act(async () => {
      render(<PerformancePage />);
    });

    const tabs = screen.getAllByRole('tab');
    expect(tabs.length).toBe(6);

    // Filter to budget_cap policies
    await act(async () => {
      fireEvent.click(tabs[2]);
    });
    expect(screen.getByText('performancePage.itemPinPixelPoolTitle')).toBeDefined();
    expect(screen.getByText('performancePage.itemCaptureHistoryTitle')).toBeDefined();
    expect(screen.queryByText('performancePage.itemSettingsTitle')).toBeNull();
    expect(screen.queryByText('performancePage.itemHeapGuardTitle')).toBeNull();

    // Filter to instant_release policies
    await act(async () => {
      fireEvent.click(tabs[3]);
    });
    expect(screen.getByText('performancePage.itemShortWorkersTitle')).toBeDefined();
    expect(screen.getByText('performancePage.itemInstantOverlaysTitle')).toBeDefined();
    expect(screen.queryByText('performancePage.itemPinPixelPoolTitle')).toBeNull();

    // Filter back to all
    await act(async () => {
      fireEvent.click(tabs[0]);
    });
    expect(screen.getByText('performancePage.itemSettingsTitle')).toBeDefined();
    expect(screen.getByText('performancePage.itemWatermarkGuardTitle')).toBeDefined();
  });

  it('reflects WindowKit disabled zero-process state when disabled in settings', async () => {
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'windowManager.getSettings') {
        return { enabled: false, running: false, settings: { features: {} } };
      }
      return {};
    });

    await act(async () => {
      render(<PerformancePage />);
    });

    expect(screen.getByText('performancePage.itemWindowKitValOff')).toBeDefined();
  });
});

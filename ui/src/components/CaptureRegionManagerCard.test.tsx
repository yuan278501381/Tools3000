/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { CaptureRegionManagerCard, type SavedCaptureRegion } from './CaptureRegionManagerCard';
import * as bridge from '../hooks/useBridge';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, opts?: Record<string, unknown> | string) => {
      if (typeof opts === 'string') return opts;
      if (key === 'capture.regionSummary' && opts) {
        return `${String(opts.total)} saved · ${String(opts.pinned)} pinned`;
      }
      if (key === 'capture.regionLimitCount' && opts) {
        return `Last ${String(opts.count)} regions`;
      }
      if (key === 'capture.regionUnnamedLabel' && opts) {
        return `Recent Region #${String(opts.index)}`;
      }
      return key;
    },
    i18n: { language: 'en' },
  }),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
    info: vi.fn(),
  },
}));

describe('CaptureRegionManagerCard', () => {
  const initialItems: SavedCaptureRegion[] = [
    {
      id: 'reg_1',
      l: 100,
      t: 120,
      r: 1380,
      b: 840,
      name: 'Tutorial 720p',
      pinned: true,
      updatedAt: 1000,
    },
    {
      id: 'reg_2',
      l: 20,
      t: 30,
      r: 420,
      b: 330,
      name: '',
      pinned: false,
      updatedAt: 900,
    },
  ];

  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    vi.spyOn(bridge, 'useBridgeEvent').mockImplementation(() => {});
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'capture.getRegionHistory') {
        return {
          success: true,
          items: initialItems,
          maxEntries: 10,
          pinnedCount: 1,
          primaryWidth: 1920,
          primaryHeight: 1080,
        };
      }
      if (method === 'capture.upsertRegionHistory') {
        return {
          success: true,
          id: 'reg_new',
          items: [
            {
              id: 'reg_new',
              l: Number(params?.l ?? 0),
              t: Number(params?.t ?? 0),
              r: Number(params?.r ?? 1280),
              b: Number(params?.b ?? 720),
              name: String(params?.name ?? ''),
              pinned: Boolean(params?.pinned ?? true),
              updatedAt: 2000,
            },
            ...initialItems,
          ],
          maxEntries: 10,
          pinnedCount: 2,
        };
      }
      if (method === 'capture.setRegionPinned') {
        return {
          success: true,
          items: initialItems.map((it) =>
            it.id === params?.id ? { ...it, pinned: Boolean(params?.pinned) } : it
          ),
        };
      }
      if (method === 'capture.clearRegionHistory') {
        return {
          success: true,
          items: initialItems.filter((it) => it.pinned),
          regionHistoryCount: 1,
        };
      }
      if (method === 'capture.triggerWithRegion') {
        return { success: true };
      }
      return { success: true };
    });
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('renders saved/pinned regions, supports creating custom region, direct capture, pin toggle, and clear unpinned', async () => {
    const onLimit = vi.fn();
    const onKey = vi.fn();
    const onRebind = vi.fn();

    await act(async () => {
      render(
        <CaptureRegionManagerCard
          repeatCaptureHotkey="Shift+F1"
          onRebindRepeatCaptureHotkey={onRebind}
          regionHistoryLimit={10}
          regionHistoryKey="R"
          onUpdateRegionHistoryLimit={onLimit}
          onUpdateRegionHistoryKey={onKey}
        />
      );
    });

    expect(screen.getByText('2 saved · 1 pinned')).toBeDefined();
    expect(screen.getByText('Tutorial 720p')).toBeDefined();
    expect(screen.getByText('1280 x 720')).toBeDefined();
    expect(screen.getByText('Recent Region #2')).toBeDefined();

    // 触发按选区立即截图
    const captureBtns = screen.getAllByRole('button', { name: 'capture.regionCaptureNow' });
    await act(async () => {
      fireEvent.click(captureBtns[0]);
    });
    expect(bridgeSpy).toHaveBeenCalledWith('capture.triggerWithRegion', { id: 'reg_1' }, { silent: true });

    // 切换新建固定选区表单并保存
    await act(async () => {
      fireEvent.click(document.getElementById('capture-region-add-btn')!);
    });
    const nameInput = document.getElementById('capture-region-name-input') as HTMLInputElement;
    await act(async () => {
      fireEvent.change(nameInput, { target: { value: 'Cover Box' } });
      fireEvent.click(document.getElementById('capture-region-save-btn')!);
    });
    expect(bridgeSpy).toHaveBeenCalledWith(
      'capture.upsertRegionHistory',
      expect.objectContaining({ name: 'Cover Box', pinned: true, r: 1600, b: 900 }),
      { silent: true }
    );
    expect(screen.getByText('Cover Box')).toBeDefined();

    // 清空未置顶条目
    await act(async () => {
      fireEvent.click(document.getElementById('capture-region-clear-btn')!);
    });
    expect(bridgeSpy).toHaveBeenCalledWith(
      'capture.clearRegionHistory',
      { includePinned: false },
      { silent: true }
    );
  });
});

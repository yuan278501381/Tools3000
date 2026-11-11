/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import * as bridge from '../hooks/useBridge';
import HistoryPage from '../pages/HistoryPage';
import { Sidebar } from './Sidebar';
import { OnboardingModal } from './OnboardingModal';
import { KeyboardHeatmap } from './KeyboardHeatmap';
import { TargetAppPickerModal } from './TargetAppPickerModal';
import { consumePendingDeepNavigation } from '../config/settingsSearchIndex';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, fallbackOrOpts?: string | Record<string, unknown>) => {
      if (typeof fallbackOrOpts === 'string') return fallbackOrOpts;
      if (fallbackOrOpts && typeof fallbackOrOpts.defaultValue === 'string') {
        return fallbackOrOpts.defaultValue;
      }
      return key;
    },
  }),
}));

describe('Batch 3 Regression Suite (P-10 / U-12 / V-08 / C-05)', () => {
  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    consumePendingDeepNavigation('search');
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockResolvedValue({ success: true });
  });

  afterEach(() => {
    cleanup();
    vi.useRealTimers();
    vi.restoreAllMocks();
  });

  it('[P-10] HistoryPage skips redundant base64 reload when metadata signature is unchanged and fetches only newly added thumbnails', async () => {
    let currentServerList = [
      {
        id: 101,
        index: 0,
        timestamp: '2026-11-12 01:00:00',
        width: 1920,
        height: 1080,
        filePath: 'C:\\Shots\\first.png',
        base64: 'AAA_FIRST_BASE64',
      },
    ];

    bridgeSpy.mockImplementation(async (method: string, params?: unknown) => {
      if (method === 'history.getAll') {
        const opts = params as { includeThumbnails?: boolean } | undefined;
        if (opts?.includeThumbnails) {
          return currentServerList;
        }
        return currentServerList.map(({ base64: _omit, ...meta }) => {
          void _omit;
          return meta;
        });
      }
      if (method === 'history.getThumbnail') {
        return { success: true, base64: 'BBB_NEW_THUMB_BASE64' };
      }
      return { success: true };
    });

    let nowMs = 10_000;
    vi.spyOn(Date, 'now').mockImplementation(() => nowMs);

    const { container } = render(<HistoryPage />);
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(container.querySelectorAll('.history-card-image img')).toHaveLength(1);

    // Advance past throttle window and trigger focus with identical history metadata
    nowMs += 3_000;
    await act(async () => {
      window.dispatchEvent(new Event('focus'));
      await Promise.resolve();
      await Promise.resolve();
    });

    const thumbCallsWhenUnchanged = bridgeSpy.mock.calls.filter(
      (call: unknown[]) => call[0] === 'history.getThumbnail',
    );
    expect(thumbCallsWhenUnchanged).toHaveLength(0);

    // Prepend a new screenshot entry on backend and trigger focus again
    currentServerList = [
      {
        id: 102,
        index: 0,
        timestamp: '2026-11-12 01:05:00',
        width: 1280,
        height: 720,
        filePath: 'C:\\Shots\\second.png',
        base64: '',
      },
      {
        ...currentServerList[0],
        index: 1,
      },
    ];

    nowMs += 3_000;
    await act(async () => {
      window.dispatchEvent(new Event('focus'));
      await Promise.resolve();
      await Promise.resolve();
    });

    const thumbCallsAfterAdd = bridgeSpy.mock.calls.filter(
      (call: unknown[]) => call[0] === 'history.getThumbnail',
    );
    expect(thumbCallsAfterAdd).toHaveLength(1);
    expect(thumbCallsAfterAdd[0]?.[1]).toEqual({ id: 102, index: 0 });
    expect(container.querySelectorAll('.history-card-image img')).toHaveLength(2);
  });

  it('[U-12] Sidebar supports fine-grained deep setting jump and 1-click enable for disabled plugins', async () => {
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'plugins.setEnabled') {
        return { success: true, restartRequired: false };
      }
      if (method === 'plugins.getAll') {
        return [
          { id: 'search', name: 'Search', version: '1.0', enabled: true, loaded: true, restartRequired: false },
          { id: 'keycast', name: 'Keycast', version: '1.0', enabled: true, loaded: true, restartRequired: false },
        ];
      }
      return { success: true };
    });

    const onNavigate = vi.fn();
    const deepEvents: Array<{ navId: string; tabId?: string }> = [];
    const listener = (evt: Event) => {
      const custom = evt as CustomEvent<{ navId: string; tabId?: string }>;
      deepEvents.push(custom.detail);
    };
    window.addEventListener('tools3000:navigate-deep', listener);

    const { container } = render(
      <Sidebar
        activeNav="general"
        onNavigate={onNavigate}
        activePlugins={new Set(['search'])}
      />,
    );

    const searchInput = container.querySelector<HTMLInputElement>('.sidebar__search-input');
    expect(searchInput).not.toBeNull();

    // 1) Search for fine-grained 'regex' sub-tab inside enabled Search module
    await act(async () => {
      fireEvent.change(searchInput!, { target: { value: 'regex' } });
    });

    const regexDeepBtn = container.querySelector<HTMLButtonElement>('#deep-nav-srch-regex');
    expect(regexDeepBtn).not.toBeNull();

    await act(async () => {
      fireEvent.click(regexDeepBtn!);
    });

    expect(onNavigate).toHaveBeenCalledWith('search');
    expect(deepEvents.some((d) => d.navId === 'search' && d.tabId === 'regex')).toBe(true);

    // 2) Search for disabled 'keycast' plugin and click 1-click Enable & Open
    await act(async () => {
      fireEvent.change(searchInput!, { target: { value: 'keycast' } });
    });

    const enableBtn = container.querySelector<HTMLButtonElement>('#inactive-enable-keycast');
    expect(enableBtn).not.toBeNull();

    await act(async () => {
      fireEvent.click(enableBtn!);
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(bridgeSpy).toHaveBeenCalledWith(
      'plugins.setEnabled',
      { id: 'keycast', enabled: true },
      { silent: true },
    );
    expect(onNavigate).toHaveBeenCalledWith('keycast');
    window.removeEventListener('tools3000:navigate-deep', listener);
  });

  it('[U-12] OnboardingModal dynamically hydrates live user hotkeys from hotkey.getAll and remote.getSettings', async () => {
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'hotkey.getAll') {
        return [
          { name: 'Toggle Search', shortcut: 'Ctrl+Space' },
          { name: 'Screenshot', shortcut: 'Ctrl+Alt+A' },
          { name: 'Record', shortcut: 'Alt+R' },
          { name: 'OCR', shortcut: 'Alt+O' },
        ];
      }
      if (method === 'remote.getSettings') {
        return { emergencyShortcut: 'Ctrl+Alt+End' };
      }
      return {};
    });

    const { container } = render(<OnboardingModal onComplete={vi.fn()} />);
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });

    // Advance from Welcome (step 0) to Core Shortcuts (step 1) using ArrowRight
    await act(async () => {
      fireEvent.keyDown(window, { key: 'ArrowRight' });
    });

    const text = container.textContent ?? '';
    expect(text).toContain('Ctrl + Space');
    expect(text).toContain('Ctrl + Alt + A');
    expect(text).toContain('Alt + R');
    expect(text).toContain('Alt + O');
    expect(text).toContain('Ctrl + Alt + End');
  });

  it('[V-08] KeyboardHeatmap isolates Main Enter (0x0D) from Numpad Enter (0x10D) and removes <12px count pills', () => {
    const { container } = render(
      <KeyboardHeatmap keyMap={{ 0x0d: 42, 0x10d: 7 }} />,
    );

    const mainEnter = container.querySelector<HTMLElement>('[data-key-identity="Enter"]');
    const numpadEnter = container.querySelector<HTMLElement>('[data-key-identity="Numpad Enter"]');

    expect(mainEnter).not.toBeNull();
    expect(numpadEnter).not.toBeNull();
    expect(mainEnter?.getAttribute('data-key-count')).toBe('42');
    expect(numpadEnter?.getAttribute('data-key-count')).toBe('7');
    expect(container.querySelector('.keyboard-keycap__count-pill')).toBeNull();
  });

  it('[C-05] Shared RunningWindowPicker populates TargetAppPickerModal from open windows list', async () => {
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'window.getOpenWindows') {
        return {
          windows: [
            {
              title: 'index.tsx - Visual Studio Code',
              processName: 'Code.exe',
              windowClass: 'Chrome_WidgetWin_1',
              pid: 4321,
            },
          ],
        };
      }
      return { success: true };
    });

    const onAdd = vi.fn();
    render(<TargetAppPickerModal onAdd={onAdd} onClose={vi.fn()} />);

    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });

    const card = document.body.querySelector<HTMLElement>('.app-picker-item');
    expect(card).not.toBeNull();

    await act(async () => {
      fireEvent.click(card!);
    });

    const buttons = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button'));
    const confirmBtn = buttons.find((b) => b.textContent?.includes('Add & Configure'));
    expect(confirmBtn).toBeDefined();
    expect(confirmBtn?.disabled).toBe(false);

    await act(async () => {
      fireEvent.click(confirmBtn!);
    });

    expect(onAdd).toHaveBeenCalledWith({
      name: 'Visual Studio Code',
      processName: 'Code.exe',
      windowClass: '',
      effect: 2,
    });
  });
});

/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import HistoryPage from './HistoryPage';
import { ExtensionPage } from './ExtensionPage';
import { KeyStatsPage } from './KeyStatsPage';
import { DialogEnhancerPage } from './DialogEnhancerPage';
import { CapturePage } from './CapturePage';
import { GeneralPage } from './GeneralPage';
import * as bridge from '../hooks/useBridge';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, options?: { defaultValue?: string } | string) => {
      if (typeof options === 'string') return options;
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

describe('Pages Usability & Regression Suite', () => {
  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    vi.spyOn(bridge, 'useBridgeEvent').mockImplementation(() => {});
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async () => ({}));
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('HistoryPage renders CodeBadge file name, locates file without triggering history.open, and supports manual refresh', async () => {
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'history.getAll') {
        return [
          {
            index: 0,
            timestamp: '2026-10-14T18:30:00',
            width: 1920,
            height: 1080,
            filePath: 'C:\\Captures\\Tools3000_20261014_183000.png',
          },
        ];
      }
      if (method === 'history.open') {
        return { success: true };
      }
      if (method === 'system.openFolder') {
        return { success: true };
      }
      return {};
    });

    await act(async () => {
      render(<HistoryPage />);
    });

    expect(screen.getByText('Tools3000_20261014_183000.png')).toBeDefined();

    // Clicking the locate file button should only call system.openFolder, not history.open
    const locateBtn = screen.getByLabelText('history.openFolder');
    await act(async () => {
      fireEvent.click(locateBtn);
    });

    expect(bridgeSpy).toHaveBeenCalledWith('system.openFolder', {
      path: 'C:\\Captures\\Tools3000_20261014_183000.png',
    });
    expect(bridgeSpy).not.toHaveBeenCalledWith('history.open', expect.anything());

    // Clicking the refresh button calls history.getAll again
    const refreshBtn = screen.getByTitle('common.refresh');
    await act(async () => {
      fireEvent.click(refreshBtn);
    });
    expect(bridgeSpy).toHaveBeenCalledTimes(3);

    // Keyboard activation (Enter) on card calls history.open
    const card = screen.getByTitle('history.pinImage');
    await act(async () => {
      fireEvent.keyDown(card, { key: 'Enter' });
    });
    expect(bridgeSpy).toHaveBeenCalledWith('history.open', { index: 0 });
  });

  it('ExtensionPage resolves titleKey, descKey, conditionKey, and supportedFormatKeys for clipboard_manager and custom plugins', async () => {
    const { rerender } = render(
      <ExtensionPage
        pluginId="clipboard_manager"
        plugin={{
          id: 'clipboard_manager',
          name: '灵感剪贴板',
          version: '1.0.0',
          fileName: 'clipboard.lua',
          abiVersion: 1,
          capabilities: ['clipboard'],
          permissions: ['clipboard'],
          executionModel: 'trusted-native-in-process',
          enabled: true,
          active: true,
          restartRequired: false,
          state: 'running',
          error: '',
        }}
      />
    );

    // Verify step keys and format keys are resolved via t() instead of rendering undefined
    expect(screen.getByText('extGuide.stepClip1Title')).toBeDefined();
    expect(screen.getByText('extGuide.stepClip1Desc')).toBeDefined();
    expect(screen.getByText('extGuide.stepClip3Title:')).toBeDefined();
    expect(screen.getByText('extGuide.formatRichText')).toBeDefined();
    expect(screen.getByText('extGuide.formatCodeSnippet')).toBeDefined();

    // Rerender with a custom extension ID to verify conditionKey and enabled state sync
    rerender(
      <ExtensionPage
        pluginId="custom_lua_ext"
        plugin={{
          id: 'custom_lua_ext',
          name: 'Custom Lua',
          version: '0.1.0',
          fileName: 'custom.lua',
          abiVersion: 1,
          capabilities: ['automation'],
          permissions: ['ipc'],
          executionModel: 'trusted-native-in-process',
          enabled: false,
          active: false,
          restartRequired: false,
          state: 'disabled',
          error: '',
        }}
      />
    );

    expect(screen.getByText('extGuide.conditionGlobal')).toBeDefined();
    expect(screen.getByText('extension.statusDisabled')).toBeDefined();
  });

  it('KeyStatsPage renders Today Keystrokes card, supports manual refresh, and resets today stats on confirmation', async () => {
    const now = new Date();
    const todayKey = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')}`;

    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'stats.getHistory') {
        return {
          [todayKey]: {
            totalKeys: 1280,
            leftClicks: 300,
            rightClicks: 120,
            mouseDistance: 9600,
            keyMap: { 65: 50 },
          },
        };
      }
      if (method === 'stats.getTotal') {
        return { totalKeystrokes: 50000 };
      }
      if (method === 'stats.clearToday') {
        return { success: true };
      }
      return {};
    });

    const confirmSpy = vi.spyOn(window, 'confirm').mockReturnValue(true);

    await act(async () => {
      render(<KeyStatsPage />);
    });

    expect(screen.getByText('stats.totalKeys')).toBeDefined();
    expect(screen.getByText((1280).toLocaleString())).toBeDefined();
    expect(screen.getByText((420).toLocaleString())).toBeDefined();

    const clearBtn = screen.getByTitle('stats.clearToday');
    await act(async () => {
      fireEvent.click(clearBtn);
    });

    expect(confirmSpy).toHaveBeenCalled();
    expect(bridgeSpy).toHaveBeenCalledWith('stats.clearToday');
  });

  it('DialogEnhancerPage supports browsing folder to add favorite path', async () => {
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'dialog.getSettings') {
        return {
          enabled: true,
          autoDetectOpenSave: true,
          showFloatingButton: true,
          rememberLastFolder: true,
          enableCtrlG: true,
          enableMiddleClick: true,
          syncExplorerPaths: true,
          recentFoldersLimit: 15,
          favorites: [],
          blacklist: [],
          appMemories: [],
        };
      }
      if (method === 'capture.browseDirectory') {
        return 'D:\\Projects\\Tools3000';
      }
      if (method === 'dialog.addFavorite') {
        return { success: true };
      }
      return {};
    });

    await act(async () => {
      render(<DialogEnhancerPage />);
    });

    const browseBtn = screen.getByTitle('dialog.browseFolder');
    await act(async () => {
      fireEvent.click(browseBtn);
    });

    expect(bridgeSpy).toHaveBeenCalledWith('capture.browseDirectory');
    expect(bridgeSpy).toHaveBeenCalledWith('dialog.addFavorite', { path: 'D:\\Projects\\Tools3000' });
    expect(screen.getByText('D:\\Projects\\Tools3000')).toBeDefined();
  });

  it('CapturePage displays Record Snapshot shortcut row and GeneralPage opens data directory on button click', async () => {
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'capture.getSettings') {
        return { format: 'png', quality: 90, saveToFile: true, copyToClipboard: true };
      }
      if (method === 'recording.getSettings') {
        return { format: 'mp4_h264', fps: 30, bitrate: 8 };
      }
      if (method === 'hotkey.getAll') {
        return [
          {
            name: 'Record Snapshot',
            shortcut: 'Ctrl+Shift+S',
            registered: true,
            conflictType: 'none',
            sessionScoped: true,
          },
        ];
      }
      if (method === 'general.getSettings') {
        return {
          startWithWindows: false,
          startMinimized: true,
          checkUpdates: true,
          language: 'zh-CN',
          theme: 'dark',
          logLevel: 'info',
          showTrayIcon: true,
          closeToTray: true,
          portableMode: true,
          dataDirectory: 'D:\\Portable\\Tools3000\\data',
        };
      }
      if (method === 'system.openFolder') {
        return { success: true };
      }
      return { audioDevices: [] };
    });

    await act(async () => {
      render(<CapturePage />);
    });

    expect(screen.getByText('recording.snapshotShortcut')).toBeDefined();

    cleanup();

    await act(async () => {
      render(<GeneralPage />);
    });

    expect(screen.getByText('general.shortcutRecordSnapshot')).toBeDefined();
    expect(screen.getByText('D:\\Portable\\Tools3000\\data')).toBeDefined();

    const openDirBtn = screen.getByTitle('general.openDataDirBtn');
    await act(async () => {
      fireEvent.click(openDirBtn);
    });

    expect(bridgeSpy).toHaveBeenCalledWith('system.openFolder', {
      path: 'D:\\Portable\\Tools3000\\data',
    });
  });
});

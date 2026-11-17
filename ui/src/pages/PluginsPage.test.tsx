/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { PluginsPage, type PluginStatus } from './PluginsPage';
import * as bridge from '../hooks/useBridge';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, options?: { defaultValue?: string } | string) => {
      if (key === 'plugins.items.remote_boost.name') return '远程协助增强';
      if (key === 'plugins.items.remote_boost.description') return '远控热键直通与急救冲刷';
      if (typeof options === 'object' && options?.defaultValue) return options.defaultValue;
      if (typeof options === 'string') return options;
      return key;
    },
    i18n: {
      language: 'zh-CN',
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

describe('PluginsPage', () => {
  const mockPlugins: PluginStatus[] = [
    {
      id: 'search',
      name: '闪电搜索',
      version: '1.0.0',
      fileName: 'Tools3000.exe',
      abiVersion: 1,
      capabilities: ['instant-search', 'ntfs-usn'],
      permissions: ['read-filesystem'],
      executionModel: 'trusted-native-in-process',
      enabled: true,
      active: true,
      restartRequired: false,
      state: 'running',
      error: '',
      isExtension: false,
    },
    {
      id: 'remote_boost',
      name: '远程协助增强',
      version: '1.0.0',
      fileName: 'Tools3000.exe',
      abiVersion: 1,
      capabilities: ['remote-tunnel', 'hotkey-passthrough', 'ime-sanitizer'],
      permissions: ['low-level-keyboard-hook', 'window-event-hook', 'input-synthesis'],
      executionModel: 'trusted-native-in-process',
      enabled: true,
      active: true,
      restartRequired: false,
      state: 'running',
      error: '',
      isExtension: false,
    },
  ];

  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      if (method === 'plugins.getAll') {
        return mockPlugins;
      }
      if (method === 'plugins.setEnabled') {
        return { success: true, restartRequired: false };
      }
      return {};
    });
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('renders remote_boost module card with name and architecture metadata', async () => {
    await act(async () => {
      render(<PluginsPage initialPlugins={mockPlugins} />);
    });

    expect(screen.getByRole('heading', { level: 2, name: '远程协助增强' })).toBeDefined();
    expect(screen.getAllByText('Tools3000.exe').length).toBeGreaterThan(0);
    expect(screen.getAllByText(/1\.0\.0/).length).toBeGreaterThan(0);
  });

  it('renders capabilities tags for remote_boost', async () => {
    await act(async () => {
      render(<PluginsPage initialPlugins={mockPlugins} />);
    });

    expect(screen.getByText('remote-tunnel')).toBeDefined();
    expect(screen.getByText('hotkey-passthrough')).toBeDefined();
    expect(screen.getByText('ime-sanitizer')).toBeDefined();
  });

  it('renders permissions details for remote_boost', async () => {
    await act(async () => {
      render(<PluginsPage initialPlugins={mockPlugins} />);
    });

    expect(screen.getByText('low-level-keyboard-hook')).toBeDefined();
    expect(screen.getByText('window-event-hook')).toBeDefined();
    expect(screen.getByText('input-synthesis')).toBeDefined();
  });

  it('toggles remote_boost module enable switch and calls plugins.setEnabled IPC', async () => {
    await act(async () => {
      render(<PluginsPage initialPlugins={mockPlugins} />);
    });

    const toggle = document.getElementById('plugin-remote_boost');
    expect(toggle).toBeDefined();

    await act(async () => {
      fireEvent.click(toggle!);
    });

    expect(bridgeSpy).toHaveBeenCalledWith(
      'plugins.setEnabled',
      { id: 'remote_boost', enabled: false },
      { silent: true }
    );
  });

  it('does not render deprecated plugin marketplace tab or request marketplace catalog', async () => {
    await act(async () => {
      render(<PluginsPage initialPlugins={mockPlugins} />);
    });

    expect(screen.queryByText('plugins.tabMarketplace')).toBeNull();
    expect(screen.queryByText('plugins.installLocal')).toBeNull();
    expect(bridgeSpy).not.toHaveBeenCalledWith('plugins.getMarketplace');
  });
});

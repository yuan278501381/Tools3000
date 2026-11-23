/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { PluginsPage, type PluginStatus } from './PluginsPage';
import { Sidebar } from '../components/Sidebar';
import {
  APP_ROUTES,
  BUILTIN_PLUGIN_ORDER,
  normalizeBuiltinPluginId,
} from '../config/routes';
import { TRAY_CONTROL_REGISTRY } from '../tray/trayRegistry';
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

  it('aligns right-side module cards strictly with left-side Sidebar core_tools order (SSOT) even under shuffled input and partial disable', async () => {
    const makePlugin = (id: string, enabled = true): PluginStatus => ({
      id,
      name: id,
      version: '1.4.5',
      fileName: 'Tools3000.exe',
      abiVersion: 1,
      capabilities: [],
      permissions: [],
      executionModel: 'trusted-native-in-process',
      enabled,
      active: enabled,
      restartRequired: false,
      state: enabled ? 'running' : 'disabled',
      error: '',
      isExtension: false,
    });

    // 刻意打乱后端返回顺序，验证排序严格遵循 APP_ROUTES 单一事实源
    const shuffledIds = [
      'picasa_viewer',
      'spotlight',
      'file_locksmith',
      'capture',
      'dialogenhancer',
      'remote_boost',
      'text_expander',
      'search',
      'keycast',
      'gesture',
      'clipboard',
    ];
    const allPlugins = shuffledIds.map((id) => makePlugin(id, true));
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'plugins.getAll') return allPlugins;
      return { success: true, restartRequired: false };
    });

    // 1. 全部启用时，左侧边栏效率工具去重模块序列 === 右侧功能模块卡片序列
    const allActiveSet = new Set(shuffledIds);
    await act(async () => {
      render(
        <div>
          <Sidebar activeNav="plugins" onNavigate={vi.fn()} activePlugins={allActiveSet} />
          <PluginsPage initialPlugins={allPlugins} onNavigate={vi.fn()} />
        </div>
      );
    });

    const renderedCardIds = Array.from(document.querySelectorAll<HTMLInputElement>('input[id^="plugin-"]'))
      .map((el) => normalizeBuiltinPluginId(el.id.replace(/^plugin-/, '')));

    const coreRoutes = APP_ROUTES.filter((r) => r.category === 'core_tools');
    const renderedSidebarPluginOrder: string[] = [];
    for (const navEl of Array.from(document.querySelectorAll<HTMLElement>('[id^="nav-"]'))) {
      const routeId = navEl.id.replace(/^nav-/, '');
      const route = coreRoutes.find((r) => r.id === routeId);
      if (!route?.requiresPlugin) continue;
      const canonical = normalizeBuiltinPluginId(route.requiresPlugin);
      if (!renderedSidebarPluginOrder.includes(canonical)) {
        renderedSidebarPluginOrder.push(canonical);
      }
    }

    expect(renderedCardIds).toEqual([...BUILTIN_PLUGIN_ORDER]);
    expect(renderedSidebarPluginOrder).toEqual(renderedCardIds);

    cleanup();

    // 2. 中途关闭任意子集模块后，剩余启用项在左右两侧均保持完全一致的相对顺序
    const disabledSet = new Set(['clipboard', 'dialog_enhancer', 'keycast']);
    const partialPlugins = shuffledIds.map((id) => makePlugin(id, !disabledSet.has(normalizeBuiltinPluginId(id))));
    const partialActiveSet = new Set(partialPlugins.filter((p) => p.enabled).map((p) => p.id));
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'plugins.getAll') return partialPlugins;
      return { success: true, restartRequired: false };
    });

    await act(async () => {
      render(
        <div>
          <Sidebar activeNav="plugins" onNavigate={vi.fn()} activePlugins={partialActiveSet} />
          <PluginsPage initialPlugins={partialPlugins} onNavigate={vi.fn()} />
        </div>
      );
    });

    const partialCardIds = Array.from(document.querySelectorAll<HTMLInputElement>('input[id^="plugin-"]'))
      .map((el) => normalizeBuiltinPluginId(el.id.replace(/^plugin-/, '')));
    const partialEnabledCards = partialCardIds.filter((id) => !disabledSet.has(id));

    const partialSidebarPluginOrder: string[] = [];
    for (const navEl of Array.from(document.querySelectorAll<HTMLElement>('[id^="nav-"]'))) {
      const routeId = navEl.id.replace(/^nav-/, '');
      const route = coreRoutes.find((r) => r.id === routeId);
      if (!route?.requiresPlugin) continue;
      const canonical = normalizeBuiltinPluginId(route.requiresPlugin);
      if (!partialSidebarPluginOrder.includes(canonical)) {
        partialSidebarPluginOrder.push(canonical);
      }
    }

    // 右侧全量卡片绝对位置保持固定不变，左侧可见模块严格等于右侧已启用子序列
    expect(partialCardIds).toEqual([...BUILTIN_PLUGIN_ORDER]);
    expect(partialSidebarPluginOrder).toEqual(partialEnabledCards);

    // 3. 强锁定 MECE 推荐四域单一事实源序：侧边栏 / 功能模块 / 托盘 100% 同序
    expect([...BUILTIN_PLUGIN_ORDER]).toEqual([
      'capture',
      'clipboard',
      'gesture',
      'text_expander',
      'search',
      'dialog_enhancer',
      'file_locksmith',
      'picasa_viewer',
      'keycast',
      'spotlight',
      'remote_boost',
    ]);
    const trayPluginOrder = [...TRAY_CONTROL_REGISTRY]
      .sort((a, b) => a.order - b.order)
      .map((item) => normalizeBuiltinPluginId(item.pluginId));
    expect(trayPluginOrder).toEqual(BUILTIN_PLUGIN_ORDER.filter((id) => id !== 'picasa_viewer'));
  });
});

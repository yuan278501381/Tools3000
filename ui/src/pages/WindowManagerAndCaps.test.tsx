/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WindowManagerPage } from './WindowManagerPage';
import { CapsLayerPage } from './CapsLayerPage';
import { DEFAULT_WINDOW_MANAGER_RESPONSE, type WindowManagerResponse } from '../types/windowManager';
import { DEFAULT_CAPS_LAYER_SETTINGS, type CapsLayerSettings } from '../types/capsLayer';
import * as bridge from '../hooks/useBridge';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, defaultOrOpts?: string | Record<string, unknown>, maybeOpts?: Record<string, unknown>) => {
      const opts = typeof defaultOrOpts === 'object' ? defaultOrOpts : maybeOpts;
      const base = typeof defaultOrOpts === 'string' ? defaultOrOpts : key;
      if (opts && typeof base === 'string') {
        return base.replace(/\{\{(\w+)\}\}/g, (_, k) => String(opts[k] ?? ''));
      }
      return base;
    },
    i18n: { language: 'en' },
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

describe('WindowManagerPage & CapsLayerPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.spyOn(bridge, 'useBridgeEvent').mockImplementation(() => {});
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('renders WindowManagerPage default-OFF with 0 MB status, toggles sub-features, imports PowerToys, and interacts with layout editor', async () => {
    let currentState: WindowManagerResponse = structuredClone(DEFAULT_WINDOW_MANAGER_RESPONSE);

    const requestSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'windowManager.getSettings') {
        return currentState;
      }
      if (method === 'windowManager.getMonitors') {
        return {
          monitors: [
            { id: 'DISPLAY1', name: 'Primary Display', width: 2560, height: 1440, dpi: 120, isPrimary: true },
          ],
        };
      }
      if (method === 'windowManager.updateSettings') {
        const patch = (params ?? {}) as Partial<WindowManagerResponse>;
        currentState = {
          ...currentState,
          enabled: typeof patch.enabled === 'boolean' ? patch.enabled : currentState.enabled,
          running: typeof patch.enabled === 'boolean' ? patch.enabled : currentState.running,
          runtimeStats:
            (patch.enabled ?? currentState.enabled)
              ? {
                  running: true,
                  pid: 4321,
                  workingSetBytes: 12 * 1024 * 1024,
                  privateBytes: 8 * 1024 * 1024,
                  handleCount: 92,
                  gdiObjects: 18,
                  userObjects: 12,
                  pinnedCount: 1,
                  trayMinimizedCount: 0,
                  activeFeatures: ['snapZones'],
                }
              : undefined,
          settings: patch.settings ? patch.settings : currentState.settings,
        };
        return currentState;
      }
      if (method === 'windowManager.importPowerToysLayouts') {
        return {
          importedCount: 1,
          layouts: [
            {
              id: 'pt-imported-1',
              name: 'PowerToys Ultrawide',
              type: 'grid',
              rows: 1,
              columns: 3,
              rowPercents: [100],
              colPercents: [25, 50, 25],
              cellMap: [[0, 1, 2]],
            },
          ],
        };
      }
      if (method === 'windowManager.captureWorkspace') {
        return {
          workspace: {
            id: 'ws-dev',
            name: String(params?.name || 'Dev'),
            apps: [
              {
                exePath: 'C:\\Program Files\\Code\\Code.exe',
                processName: 'Code.exe',
                args: '',
                monitorId: 'DISPLAY1',
                zoneIndex: 0,
              },
            ],
          },
        };
      }
      if (method === 'windowManager.execAction') {
        return { success: true };
      }
      return {};
    });

    await act(async () => {
      render(<WindowManagerPage />);
    });

    // 1. Default-OFF master toggle + 0 MB zero overhead banner
    const masterToggle = document.getElementById('window-manager-master-toggle') as HTMLInputElement;
    expect(masterToggle).not.toBeNull();
    expect(masterToggle.checked).toBe(false);
    expect(screen.getByText('Stopped (0 MB · 0 Processes)')).toBeTruthy();

    // 2. Enabling SnapZones sub-feature auto-enables master and displays live PID + RAM stats
    const snapToggle = document.getElementById('wm-toggle-snapzones') as HTMLInputElement;
    expect(snapToggle.checked).toBe(false);
    await act(async () => {
      fireEvent.click(snapToggle);
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'windowManager.updateSettings',
      expect.objectContaining({
        enabled: true,
        settings: expect.objectContaining({
          features: expect.objectContaining({ snapZones: true }),
        }),
      }),
      { silent: true },
    );
    expect(await screen.findByText('PID 4321')).toBeTruthy();
    expect(screen.getByText('RAM 12.0 MB')).toBeTruthy();

    // 3. PowerToys 1-click import merges returned layout into editor strip
    await act(async () => {
      fireEvent.click(screen.getByText('Import PowerToys'));
    });
    expect(requestSpy).toHaveBeenCalledWith('windowManager.importPowerToysLayouts', undefined, { silent: true });
    expect(await screen.findByText('PowerToys Ultrawide')).toBeTruthy();

    // 4. Layout editor mode switch to Canvas & Add Zone interaction
    await act(async () => {
      fireEvent.click(screen.getByText('Canvas'));
    });
    const addZoneBtn = await screen.findByText('Add Zone');
    await act(async () => {
      fireEvent.click(addZoneBtn);
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'windowManager.updateSettings',
      expect.objectContaining({
        settings: expect.objectContaining({
          snapZones: expect.objectContaining({
            layouts: expect.arrayContaining([
              expect.objectContaining({ type: 'canvas' }),
            ]),
          }),
        }),
      }),
      { silent: true },
    );

    // 5. Workspaces tab: capture desktop & launch workspace
    await act(async () => {
      fireEvent.click(screen.getByRole('tab', { name: /Workspaces/i }));
    });
    const wsToggle = document.getElementById('wm-toggle-workspaces') as HTMLInputElement;
    await act(async () => {
      fireEvent.click(wsToggle);
    });
    const wsNameInput = screen.getByLabelText('New workspace name');
    fireEvent.change(wsNameInput, { target: { value: 'Coding Setup' } });
    await act(async () => {
      fireEvent.click(screen.getByText('Capture Current Desktop'));
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'windowManager.captureWorkspace',
      { name: 'Coding Setup' },
      { silent: true },
    );
    expect(await screen.findByText('Coding Setup')).toBeTruthy();

    await act(async () => {
      fireEvent.click(screen.getByText('Launch'));
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'windowManager.execAction',
      { action: 'launch_workspace', workspaceId: 'ws-dev' },
      { silent: true },
    );

    // 6. Utilities tab: Multi-Monitor toggle + Rescue Off-Screen action
    await act(async () => {
      fireEvent.click(screen.getByRole('tab', { name: /Window Utilities/i }));
    });
    const mmToggle = document.getElementById('wm-toggle-multi-monitor') as HTMLInputElement;
    expect(mmToggle.checked).toBe(false);
    await act(async () => {
      fireEvent.click(mmToggle);
    });
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: 'Rescue Off-Screen Windows' }));
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'windowManager.execAction',
      { action: 'rescue_offscreen' },
      { silent: true },
    );
  });

  it('supports friendly monitor names, per-virtual-desktop layout bindings, 1:1 fullscreen editor launch, and canvas bounds clamping', async () => {
    let currentState: WindowManagerResponse = structuredClone(DEFAULT_WINDOW_MANAGER_RESPONSE);
    currentState.enabled = true;
    currentState.running = true;
    currentState.settings.features.snapZones = true;
    currentState.settings.snapZones.perVirtualDesktop = true;
    currentState.settings.snapZones.layouts = [
      ...currentState.settings.snapZones.layouts,
      {
        id: 'overflow-canvas',
        name: 'Overflow Canvas',
        type: 'canvas',
        builtin: false,
        zones: [{ id: 0, x: 86, y: 80, w: 30, h: 35 }],
      },
    ];
    currentState.settings.snapZones.monitorBindings = {
      'MON-1': 'template_columns',
      'MON-1@desk-routine': 'overflow-canvas',
    };

    const requestSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'windowManager.getSettings') return currentState;
      if (method === 'windowManager.auditBindings') {
        return {
          success: true,
          totalBindings: 2,
          offlineMonitorCount: 1,
          removedDesktopCount: 0,
          offlineMonitorBindings: [{ key: 'OLD-MON', monitorId: 'OLD-MON', desktopId: '', layoutId: 'template_grid' }],
          removedDesktopBindings: [],
        };
      }
      if (method === 'windowManager.cleanStaleBindings') {
        return { success: true, removedCount: 1 };
      }
      if (method === 'windowManager.getMonitors') {
        return {
          monitors: [
            { id: 'MON-1', name: '\\\\.\\DISPLAY1', width: 3456, height: 2160, dpi: 144, isPrimary: true },
          ],
          currentDesktopId: 'desk-routine',
          virtualDesktops: [
            { id: 'desk-routine', name: 'Routine', index: 1, isCurrent: true },
            { id: 'desk-vm', name: 'VM', index: 2, isCurrent: false },
            { id: 'desk-remote', name: '', index: 3, isCurrent: false },
          ],
        };
      }
      if (method === 'windowManager.updateSettings') {
        const patch = (params ?? {}) as Partial<WindowManagerResponse>;
        if (patch.settings) currentState = { ...currentState, settings: patch.settings };
        return currentState;
      }
      if (method === 'windowManager.execAction') return { success: true };
      return {};
    });

    await act(async () => {
      render(<WindowManagerPage />);
    });

    // 1. Raw Win32 device name \\.\DISPLAY1 is humanized to Display 1
    expect(screen.getByText('Display 1')).toBeTruthy();
    expect(screen.queryByText('\\\\.\\DISPLAY1')).toBeNull();

    // 2. Overflowing canvas zone (x=86, w=30) is clamped so x+w <= 100% (x=70%, w=30%)
    const canvasBlocks = document.querySelectorAll<HTMLElement>('.zone-editor__zone--draggable');
    expect(canvasBlocks.length).toBe(1);
    expect(canvasBlocks[0].style.left).toContain('70%');
    expect(canvasBlocks[0].style.width).toContain('30%');
    expect(canvasBlocks[0].style.top).toContain('65%');
    expect(canvasBlocks[0].style.height).toContain('35%');

    // 3. Stale offline monitor binding audit notice & one-click clean
    expect(await screen.findByText(/Clean Stale Bindings/i)).toBeTruthy();
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: /Clean Stale Bindings/i }));
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'windowManager.cleanStaleBindings',
      { cleanOfflineMonitors: true, cleanRemovedDesktops: true },
      { silent: true },
    );

    // 4. Per-virtual-desktop bar lists all desktops including indexed fallback for unnamed desktop 3
    expect(screen.getByText('Routine')).toBeTruthy();
    expect(screen.getByText('VM')).toBeTruthy();
    expect(screen.getByText('Desktop 3')).toBeTruthy();

    // 5. Switch to VM desktop and bind Priority Grid -> writes composite key MON-1@desk-vm
    await act(async () => {
      fireEvent.click(screen.getByRole('tab', { name: /VM/i }));
    });
    await act(async () => {
      fireEvent.click(screen.getAllByText('Priority Grid')[0]);
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'windowManager.updateSettings',
      expect.objectContaining({
        settings: expect.objectContaining({
          snapZones: expect.objectContaining({
            monitorBindings: expect.objectContaining({
              'MON-1@desk-vm': 'template_priority_grid',
            }),
          }),
        }),
      }),
      { silent: true },
    );

    // 6. Launch 1:1 fullscreen editor forwards exact monitorId, desktopId, and layoutId
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: /1:1 Fullscreen Editor/i }));
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'windowManager.execAction',
      expect.objectContaining({
        action: 'open_layout_editor',
        monitorId: 'MON-1',
        desktopId: 'desk-vm',
        layoutId: 'template_priority_grid',
      }),
      { silent: true },
    );

    // 7. Follow Default clears the per-desktop override key
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: /Follow Default/i }));
    });
    expect(currentState.settings.snapZones.monitorBindings['MON-1@desk-vm']).toBeUndefined();
  });

  it('supports WindowKit v2 dual-track Quick Switch, red-border Workspace capture, interactive workbench stage, and desktop shortcuts', async () => {
    let currentState: WindowManagerResponse = structuredClone(DEFAULT_WINDOW_MANAGER_RESPONSE);
    currentState.enabled = true;
    currentState.running = true;
    currentState.settings.features.workspaces = true;
    currentState.settings.features.quickSwitch = true;

    let capturedCallback: ((payload: unknown) => void) | null = null;
    vi.spyOn(bridge, 'useBridgeEvent').mockImplementation((event: string, cb: (payload: unknown) => void) => {
      if (event === 'windowManager.workspaceCaptured') {
        capturedCallback = cb;
      }
    });

    const requestSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'windowManager.getSettings') return currentState;
      if (method === 'windowManager.getMonitors') {
        return {
          monitors: [{ id: 'MON-4K', name: '4K Display', width: 3456, height: 2160, dpi: 144, isPrimary: true }],
        };
      }
      if (method === 'windowManager.updateSettings') {
        const patch = (params ?? {}) as Partial<WindowManagerResponse>;
        if (patch.settings) currentState = { ...currentState, settings: patch.settings };
        return currentState;
      }
      if (method === 'windowManager.startWorkspaceCapture') return { success: true };
      if (method === 'windowManager.createWorkspaceShortcut') {
        return { success: true, shortcutPath: 'C:\\Users\\Test\\Desktop\\Coding.lnk' };
      }
      return { success: true };
    });

    await act(async () => {
      render(<WindowManagerPage />);
    });

    // 1. Quick Switch v2 independent dual-track toggle (disable global Alt+Tab, enable Same-App Alt+`)
    await act(async () => {
      fireEvent.click(screen.getByRole('tab', { name: /Quick Switch/i }));
    });
    const replaceAltTab = document.getElementById('wm-qs-replace-alttab') as HTMLInputElement;
    const sameApp = document.getElementById('wm-qs-same-app') as HTMLInputElement;
    expect(replaceAltTab.checked).toBe(true);
    expect(sameApp.checked).toBe(false);
    await act(async () => {
      fireEvent.click(sameApp);
    });
    expect(currentState.settings.quickSwitch.sameAppEnabled).toBe(true);

    // 2. Workspaces v2 red-border capture -> live event opens interactive stage workbench -> save + shortcut
    await act(async () => {
      fireEvent.click(screen.getByRole('tab', { name: /Workspaces/i }));
    });
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: /Create workspace/i }));
    });
    expect(requestSpy).toHaveBeenCalledWith('windowManager.startWorkspaceCapture', {}, { silent: true });

    expect(capturedCallback).not.toBeNull();
    await act(async () => {
      capturedCallback?.({
        captured: true,
        workspace: {
          id: 'ws-live-1',
          name: 'Coding',
          apps: [
            {
              id: 'app_1',
              appName: 'Visual Studio Code',
              processName: 'Code.exe',
              exePath: 'C:\\Code.exe',
              args: '',
              monitorId: 'MON-4K',
              monitorIndex: 0,
              runAsAdmin: true,
              maximized: false,
              minimized: false,
              relRect: { x: 0.1, y: 0.1, w: 0.6, h: 0.8 },
            },
            {
              id: 'app_2',
              appName: 'Tools3000',
              processName: 'Tools3000.exe',
              exePath: 'C:\\Tools3000.exe',
              args: '',
              monitorId: 'MON-4K',
              monitorIndex: 0,
              minimized: true,
            },
          ],
        },
      });
    });

    // Interactive workbench renders Screen 1 & Minimized apps group + Admin badge
    expect(screen.getByText('Visual Studio Code')).toBeTruthy();
    expect(screen.getByText('Administrator')).toBeTruthy();
    expect(screen.getByText('Minimized apps')).toBeTruthy();

    // Enable desktop shortcut & save
    const shortcutToggle = document.getElementById('ws-edit-create-shortcut') as HTMLInputElement;
    await act(async () => {
      fireEvent.click(shortcutToggle);
    });
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: /^Save$/i }));
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'windowManager.createWorkspaceShortcut',
      { workspaceId: 'ws-live-1', name: 'Coding' },
      { silent: true },
    );
  });

  it('renders CapsLayerPage default-OFF, applies presets, and manages per-app profiles via RunningWindowPicker', async () => {
    let capsState: CapsLayerSettings = structuredClone(DEFAULT_CAPS_LAYER_SETTINGS);

    const requestSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'capsLayer.getSettings') {
        return capsState;
      }
      if (method === 'capsLayer.updateSettings') {
        capsState = { ...capsState, ...(params as Partial<CapsLayerSettings>) };
        return capsState;
      }
      if (method === 'capsLayer.applyPreset') {
        const presetId = String(params?.presetId ?? '');
        capsState = {
          ...capsState,
          globalMappings:
            presetId === 'arrows_vim'
              ? [
                  { id: 'm1', key: 'H', targetType: 'key', targetValue: 'Left', label: 'Vim Left', enabled: true },
                  { id: 'm2', key: 'J', targetType: 'key', targetValue: 'Down', label: 'Vim Down', enabled: true },
                ]
              : [
                  { id: 'm3', key: 'I', targetType: 'key', targetValue: 'Up', label: 'Arrow Up', enabled: true },
                ],
        };
        return capsState;
      }
      if (method === 'window.getOpenWindows') {
        return {
          windows: [
            { title: 'App.tsx - Visual Studio Code', processName: 'Code.exe', windowClass: 'Chrome_WidgetWin_1', pid: 998 },
          ],
        };
      }
      return {};
    });

    await act(async () => {
      render(<CapsLayerPage />);
    });

    // 1. Default-OFF state
    const masterToggle = document.getElementById('caps-layer-master-toggle') as HTMLInputElement;
    expect(masterToggle.checked).toBe(false);
    expect(screen.getByText('Disabled (Zero Hooks)')).toBeTruthy();

    // 2. Enable master + switch tap action to Send Esc
    await act(async () => {
      fireEvent.click(masterToggle);
    });
    expect(requestSpy).toHaveBeenCalledWith('capsLayer.updateSettings', { enabled: true }, { silent: true });

    await act(async () => {
      fireEvent.click(screen.getByText('Send Esc'));
    });
    expect(requestSpy).toHaveBeenCalledWith('capsLayer.updateSettings', { tapAction: 'esc' }, { silent: true });

    // 3. Apply 1-click Vim H/J/K/L preset
    await act(async () => {
      fireEvent.click(screen.getByText('Vim H / J / K / L'));
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'capsLayer.applyPreset',
      { presetId: 'arrows_vim' },
      { silent: true },
    );
    expect(await screen.findByText('Caps + H')).toBeTruthy();
    expect(screen.getByText('Vim Left')).toBeTruthy();

    // 4. Add App Profile using RunningWindowPicker modal
    const addProfileButtons = screen.getAllByRole('button', { name: 'Add App Profile' });
    await act(async () => {
      fireEvent.click(addProfileButtons[0]);
    });
    const codeItem = await screen.findByText('Code.exe');
    await act(async () => {
      fireEvent.click(codeItem);
    });
    const modalDialog = screen.getByRole('dialog');
    await act(async () => {
      fireEvent.click(within(modalDialog).getByRole('button', { name: 'Add' }));
    });
    expect(requestSpy).toHaveBeenCalledWith(
      'capsLayer.updateSettings',
      expect.objectContaining({
        appProfiles: expect.arrayContaining([
          expect.objectContaining({ processName: 'code.exe' }),
        ]),
      }),
      { silent: true },
    );
  });

  it('mounts workspace preview stage and meta card inside a sticky dock above the scrollable app list with live hover highlight', async () => {
    const currentState: WindowManagerResponse = structuredClone(DEFAULT_WINDOW_MANAGER_RESPONSE);
    currentState.enabled = true;
    currentState.running = true;
    currentState.settings.features.workspaces = true;
    currentState.settings.workspaces.items = [
      {
        id: 'ws-sticky-1',
        name: 'Workspace 2',
        apps: [
          {
            id: 'app-1',
            appName: 'Visual Studio Code',
            exePath: 'C:\\Program Files\\Code\\Code.exe',
            processName: 'Code.exe',
            args: '',
            monitorId: 'DISPLAY1',
            monitorIndex: 0,
            rect: { x: 100, y: 100, w: 1200, h: 800 },
          },
          {
            id: 'app-2',
            appName: 'Notepad',
            exePath: 'C:\\Windows\\System32\\notepad.exe',
            processName: 'notepad.exe',
            args: '',
            monitorId: 'DISPLAY1',
            monitorIndex: 0,
            minimized: true,
          },
        ],
      },
    ];

    vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      if (method === 'windowManager.getSettings') return currentState;
      if (method === 'windowManager.getMonitors') {
        return {
          monitors: [
            { id: 'DISPLAY1', name: 'Primary Display', width: 3456, height: 2160, dpi: 192, isPrimary: true },
          ],
        };
      }
      return {};
    });

    const { container } = await act(async () => render(<WindowManagerPage />));

    await act(async () => {
      fireEvent.click(screen.getByRole('tab', { name: /Workspaces/i }));
    });
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: 'Edit' }));
    });

    const editor = container.querySelector('.ws-workbench-editor');
    const header = container.querySelector('.ws-editor-header');
    const stickyDock = container.querySelector('.ws-editor-sticky-dock');
    const stageCard = stickyDock?.querySelector('.ws-stage-card');
    const metaCard = stickyDock?.querySelector('.ws-meta-card');
    const appGroups = container.querySelectorAll('.ws-app-group');

    expect(editor).not.toBeNull();
    expect(header).not.toBeNull();
    expect(stickyDock).not.toBeNull();
    expect(stageCard).not.toBeNull();
    expect(metaCard).not.toBeNull();
    // 编辑顶栏不在吸顶区内部，应用列表也在吸顶区下方独立滚动
    expect(stickyDock?.contains(header!)).toBe(false);
    expect(appGroups.length).toBe(2);
    expect(stickyDock?.contains(appGroups[0]!)).toBe(false);

    // 悬停下方最小化应用行时，上方吸顶舞台内的微缩任务栏图标同步高亮
    const notepadRow = screen.getByText('Notepad').closest('.ws-app-row');
    expect(notepadRow).not.toBeNull();
    fireEvent.mouseEnter(notepadRow!);
    const activeTaskbarBtn = stickyDock?.querySelector('.ws-stage-taskbar__item.is-active');
    expect(activeTaskbarBtn).not.toBeNull();
  });
});

// @vitest-environment jsdom
import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, fireEvent, waitFor, cleanup } from '@testing-library/react';
import { GestureEditorModal } from './GestureEditorModal';
import { BUILTIN_COMMAND_KEYS } from './gestureModel';
import { DEFAULT_WINDOW_MANAGER_SETTINGS, type WindowManagerResponse } from '../types/windowManager';
import zhLocale from '../i18n/locales/zh.json';
import zhTwLocale from '../i18n/locales/zh-TW.json';
import enLocale from '../i18n/locales/en.json';
import jaLocale from '../i18n/locales/ja.json';
import koLocale from '../i18n/locales/ko.json';
import ruLocale from '../i18n/locales/ru.json';
import ptBrLocale from '../i18n/locales/pt-BR.json';
import esLocale from '../i18n/locales/es.json';

const mockBridgeRequest = vi.fn();

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => mockBridgeRequest(...args),
}));

vi.mock('react-i18next', () => ({
  initReactI18next: { type: '3rdParty', init: () => {} },
  useTranslation: () => ({
    t: (key: string, opts?: string | Record<string, unknown>) => {
      if (typeof opts === 'string') return opts;
      if (opts && typeof opts === 'object' && 'count' in opts) {
        return `${String(opts.count)} apps`;
      }
      return key;
    },
  }),
}));

function readPath(obj: Record<string, unknown>, keyPath: string): unknown {
  return keyPath.split('.').reduce<unknown>((acc, seg) => {
    if (acc && typeof acc === 'object' && seg in (acc as Record<string, unknown>)) {
      return (acc as Record<string, unknown>)[seg];
    }
    return undefined;
  }, obj);
}

describe('GestureEditorModal WindowKit layout & workspace commands', () => {
  afterEach(() => {
    cleanup();
  });

  beforeEach(() => {
    mockBridgeRequest.mockReset();
    const wmResp: WindowManagerResponse = {
      enabled: true,
      running: true,
      exeAvailable: true,
      settings: {
        ...DEFAULT_WINDOW_MANAGER_SETTINGS,
        snapZones: {
          ...DEFAULT_WINDOW_MANAGER_SETTINGS.snapZones,
          activeLayoutId: 'builtin-three-cols',
          layouts: [
            { id: 'builtin-two-cols', name: '双栏对半', type: 'grid', builtin: true },
            { id: 'builtin-three-cols', name: '三栏居中', type: 'grid', builtin: true },
          ],
        },
        workspaces: {
          ...DEFAULT_WINDOW_MANAGER_SETTINGS.workspaces,
          items: [
            {
              id: 'ws-dev-01',
              name: '研发工作区',
              apps: [
                {
                  exePath: 'C:\\Code.exe',
                  processName: 'Code.exe',
                  args: '',
                  monitorId: 'm1',
                },
              ],
            },
          ],
        },
      },
    };
    mockBridgeRequest.mockImplementation((method: string) => {
      if (method === 'windowManager.getSettings') return Promise.resolve(wmResp);
      return Promise.resolve({ ok: true });
    });
  });

  it('has all 6 new WindowKit builtin command keys localized across all 8 locales', () => {
    const newKeys = BUILTIN_COMMAND_KEYS.slice(38, 44);
    expect(newKeys).toEqual([
      'gestureEditor.builtin.windowKitOpenLayoutEditor',
      'gestureEditor.builtin.windowKitApplyLayout',
      'gestureEditor.builtin.windowKitLaunchWorkspace',
      'gestureEditor.builtin.windowKitStartWorkspaceCapture',
      'gestureEditor.builtin.windowKitRestoreAllTray',
      'gestureEditor.builtin.windowKitRescueOffscreen',
    ]);
    const locales = [zhLocale, zhTwLocale, enLocale, jaLocale, koLocale, ruLocale, ptBrLocale, esLocale];
    for (const loc of locales) {
      for (const k of newKeys) {
        const val = readPath(loc as unknown as Record<string, unknown>, k);
        expect(typeof val).toBe('string');
        expect((val as string).trim().length).toBeGreaterThan(0);
      }
    }
  });

  it('persists builtinParam when saving a SnapZones layout gesture', async () => {
    const onSave = vi.fn();
    render(
      <GestureEditorModal
        mapping={{
          id: 'g-layout',
          gestureCode: 'U-R',
          enabled: true,
          action: {
            type: 2,
            name: '切换三栏布局',
            builtinCmd: 39,
            builtinParam: 'builtin-three-cols',
          },
        }}
        onSave={onSave}
        onClose={vi.fn()}
      />,
    );

    await waitFor(() => {
      expect(mockBridgeRequest).toHaveBeenCalledWith('windowManager.getSettings', undefined, { silent: true });
    });

    const saveBtn = screen.getByRole('button', { name: 'common.save' }) as HTMLButtonElement;
    expect(saveBtn.disabled).toBe(false);
    fireEvent.click(saveBtn);

    expect(onSave).toHaveBeenCalledTimes(1);
    const saved = onSave.mock.calls[0][0];
    expect(saved.action.builtinCmd).toBe(39);
    expect(saved.action.builtinParam).toBe('builtin-three-cols');
  });

  it('blocks saving a launch-workspace gesture until a workspace target is chosen', async () => {
    const onSave = vi.fn();
    render(
      <GestureEditorModal
        mapping={{
          id: 'g-ws-empty',
          gestureCode: 'D-U',
          enabled: true,
          action: {
            type: 2,
            name: '启动研发工作区',
            builtinCmd: 40,
            builtinParam: '',
          },
        }}
        onSave={onSave}
        onClose={vi.fn()}
      />,
    );

    await waitFor(() => {
      expect(mockBridgeRequest).toHaveBeenCalledWith('windowManager.getSettings', undefined, { silent: true });
    });

    const saveBtn = screen.getByRole('button', { name: 'common.save' }) as HTMLButtonElement;
    expect(saveBtn.disabled).toBe(true);
  });
});

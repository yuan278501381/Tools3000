/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import TrayApp from './TrayApp';
import { formatCompactShortcut } from './tray/trayRegistry';
import * as bridge from './hooks/useBridge';

vi.mock('./hooks/useAppearance', () => ({
  useAppearance: () => {},
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, opts?: string | Record<string, unknown>) => {
      if (typeof opts === 'string') return opts;
      if (key === 'tray.modulesActive' && opts && typeof opts === 'object') {
        return `${String(opts.active)}/${String(opts.total)} Active`;
      }
      if (opts && typeof opts === 'object' && typeof opts.defaultValue === 'string') {
        return opts.defaultValue
          .replace('{{count}}', String(opts.count ?? 0))
          .replace('{{groups}}', String(opts.groups ?? 0))
          .replace('{{id}}', String(opts.id ?? 1));
      }
      return key;
    },
  }),
}));

describe('TrayApp Bento Control Center', () => {
  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    vi.spyOn(bridge, 'useBridgeEvent').mockImplementation(() => {});
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      switch (method) {
        case 'general.getSettings':
          return { elevated: true };
        case 'remote.getSettings':
          return { enabled: true, emergencyShortcut: 'Control+Alt+Backspace' };
        case 'hotkey.getAll':
          return [
            { name: 'Screenshot', shortcut: 'F1' },
            { name: 'Record', shortcut: 'Ctrl+Shift+R' },
            { name: 'Pin Paste', shortcut: 'F3' },
            { name: 'Toggle Search', shortcut: 'Alt+Space' },
          ];
        case 'capture.pinGroupControl':
          return {
            totalCount: 3,
            ungroupedCount: 1,
            activeFilter: -1,
            allHidden: false,
            maxGroupId: 32,
            nextAvailableGroupId: 8,
            groups: [
              { id: 1, count: 1, shortLabel: 'G1', fullLabel: 'Group 1 (Blue)', colorHex: '#3B82F6' },
              { id: 7, count: 1, shortLabel: 'G7', fullLabel: 'Group 7 (Indigo)', colorHex: '#6366F1' },
            ],
          };
        case 'capture.pasteAsPin':
          return { success: true };
        case 'plugins.getAll':
          return [
            { id: 'gesture', active: true, enabled: true },
            { id: 'keycast', active: true, enabled: true },
            { id: 'capture', active: true, enabled: true },
            { id: 'search', active: true, enabled: true },
            { id: 'spotlight', active: true, enabled: true },
            { id: 'dialogenhancer', active: true, enabled: true },
            { id: 'remote_boost', active: true, enabled: true },
          ];
        case 'gesture.getState':
          return { paused: false };
        case 'tray.action':
        case 'tray.hide':
        case 'tray.resize':
        case 'remote.updateSettings':
          return { success: true };
        default:
          return null;
      }
    });
  });

  afterEach(() => {
    cleanup();
  });

  it('formats verbose key names into compact desktop shortcut badges', () => {
    expect(formatCompactShortcut('Control+Alt+Backspace')).toBe('Ctrl+Alt+Bksp');
    expect(formatCompactShortcut('Ctrl + Shift + PrintScreen')).toBe('Ctrl+Shift+PrtSc');
    expect(formatCompactShortcut('')).toBe('');
  });

  it('renders brand header, active module badge, live shortcut hints, and admin capsule', async () => {
    await act(async () => {
      render(<TrayApp />);
    });

    expect(screen.getByText('Tools3000')).toBeDefined();
    expect(screen.getByText('7/7 Active')).toBeDefined();
    expect(screen.getByText('Remote Boost')).toBeDefined();
    expect(screen.getByText('Hotkey passthrough')).toBeDefined();
    expect(screen.getByText('F1')).toBeDefined();
    expect(screen.getByText('Ctrl+Shift+R')).toBeDefined();
    expect(screen.getByText('Alt+Space')).toBeDefined();
    expect(screen.getByText('Ctrl+Alt+Bksp')).toBeDefined();
    expect(screen.getByText('Admin')).toBeDefined();
  });

  it('toggles wide Remote Boost switch and dispatches tray actions cleanly', async () => {
    await act(async () => {
      render(<TrayApp />);
    });

    const remoteTile = screen.getByRole('menuitemcheckbox', { name: /Remote Boost/i });
    await act(async () => {
      fireEvent.click(remoteTile);
    });
    expect(bridgeSpy).toHaveBeenCalledWith('remote.updateSettings', { enabled: false });

    const captureItem = screen.getByRole('menuitem', { name: /Capture Area/i });
    await act(async () => {
      fireEvent.click(captureItem);
    });
    expect(bridgeSpy).toHaveBeenCalledWith('tray.hide');
    expect(bridgeSpy).toHaveBeenCalledWith('tray.action', { action: 'screenshot' });
  });

  it('expands inline Pin Groups drawer and supports groups beyond 6 plus global pin controls', async () => {
    await act(async () => {
      render(<TrayApp />);
    });

    expect(screen.getByText('F3')).toBeDefined();
    expect(screen.getByText('3 Pins · 2 Groups')).toBeDefined();

    const pinGroupsTrigger = screen.getByRole('menuitem', { name: /Pin Groups/i });
    await act(async () => {
      fireEvent.click(pinGroupsTrigger);
    });

    const newGroupBtn = screen.getByRole('button', { name: /New Group/i });
    await act(async () => {
      fireEvent.click(newGroupBtn);
    });
    expect(bridgeSpy).toHaveBeenCalledWith('capture.pinGroupControl', { createNewGroup: true });

    const group7Chip = screen.getByRole('button', { name: /G7/i });
    await act(async () => {
      fireEvent.click(group7Chip);
    });
    expect(bridgeSpy).toHaveBeenCalledWith('capture.pinGroupControl', { filterGroup: 7 });

    const arrangeAllBtn = screen.getByRole('button', { name: /Arrange All/i });
    await act(async () => {
      fireEvent.click(arrangeAllBtn);
    });
    expect(bridgeSpy).toHaveBeenCalledWith('capture.pinGroupControl', { arrangeAll: true });

    const styleBtn = screen.getByRole('button', { name: /Style/i });
    await act(async () => {
      fireEvent.click(styleBtn);
    });

    expect(screen.getByPlaceholderText(/Group 7 name/i)).toBeDefined();
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: /^G1$/ }));
    });
    const nameInput = screen.getByPlaceholderText(/Group 1 name/i);
    await act(async () => {
      fireEvent.change(nameInput, { target: { value: 'UI Spec' } });
      fireEvent.blur(nameInput);
    });
    expect(bridgeSpy).toHaveBeenCalledWith('capture.pinGroupControl', {
      updateGroupMeta: { id: 1, name: 'UI Spec' },
    });

    const emeraldSwatch = screen.getByRole('radio', { name: /Emerald #10B981/i });
    await act(async () => {
      fireEvent.click(emeraldSwatch);
    });
    expect(bridgeSpy).toHaveBeenCalledWith('capture.pinGroupControl', {
      updateGroupMeta: { id: 1, colorHex: '#10B981' },
    });
  });

  it('keeps focus inside Pin Group name input on Arrow keys and cancels draft cleanly on Escape without hiding tray', async () => {
    await act(async () => {
      render(<TrayApp />);
    });

    await act(async () => {
      fireEvent.click(screen.getByRole('menuitem', { name: /Pin Groups/i }));
    });
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: /Style/i }));
    });

    const nameInput = screen.getByPlaceholderText(/Group 1 name/i) as HTMLInputElement;
    nameInput.focus();
    expect(document.activeElement).toBe(nameInput);

    await act(async () => {
      fireEvent.change(nameInput, { target: { value: 'Draft Title' } });
      fireEvent.keyDown(nameInput, { key: 'ArrowLeft', bubbles: true, cancelable: true });
      fireEvent.keyDown(nameInput, { key: 'ArrowRight', bubbles: true, cancelable: true });
    });

    expect(document.activeElement).toBe(nameInput);
    expect(bridgeSpy).not.toHaveBeenCalledWith('capture.pinGroupControl', {
      updateGroupMeta: { id: 1, name: 'Draft Title' },
    });

    await act(async () => {
      fireEvent.keyDown(nameInput, { key: 'Escape', bubbles: true, cancelable: true });
    });
    expect(document.activeElement).not.toBe(nameInput);
    expect(nameInput.value).toBe('');
    expect(bridgeSpy).not.toHaveBeenCalledWith('tray.hide');
    expect(bridgeSpy).not.toHaveBeenCalledWith('capture.pinGroupControl', {
      updateGroupMeta: { id: 1, name: 'Draft Title' },
    });

    await act(async () => {
      fireEvent.keyDown(window, { key: 'Escape', bubbles: true, cancelable: true });
    });
    expect(bridgeSpy).toHaveBeenCalledWith('tray.hide');
  });
});

/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { RemoteBoostPage } from './RemoteBoostPage';
import * as bridge from '../hooks/useBridge';

// Mock i18next
vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (_key: string, fallback?: string) => fallback || _key,
  }),
}));

// Mock sonner toast
vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
    info: vi.fn(),
  },
}));

describe('RemoteBoostPage', () => {
  const initialSettings = {
    enabled: true,
    hotkeyTunnelEnabled: true,
    emergencyFlushEnabled: true,
    doubleRightCtrlTrigger: true,
    imeSanitizerEnabled: true,
    emergencyShortcut: 'Ctrl+Alt+Backspace',
    targetProcesses: ['ToDesk.exe', 'SunloginClient.exe', 'mstsc.exe'],
    targetClasses: ['TSSHELLWND', 'SunloginMain'],
  };

  const initialState = {
    isRemoteForeground: false,
    imeSanitized: false,
    activeProcess: '',
  };

  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      if (method === 'remote.getSettings') {
        return initialSettings;
      }
      if (method === 'remote.getState') {
        return initialState;
      }
      if (method === 'remote.updateSettings') {
        return { success: true };
      }
      if (method === 'remote.emergencyFlush') {
        return { success: true };
      }
      if (method === 'remote.resetDefaults') {
        return { success: true };
      }
      return null;
    });
  });

  afterEach(() => {
    cleanup();
  });

  it('renders the RemoteBoostPage with default sections and titles', async () => {
    await act(async () => {
      render(<RemoteBoostPage />);
    });

    expect(screen.getByText('Remote Assistant Host Boost')).toBeDefined();
    expect(screen.getByText('Immersive Remote Hotkey Tunnel')).toBeDefined();
    expect(screen.getByText('Modifier Key Emergency Flush')).toBeDefined();
    expect(screen.getByText('Smart Remote IME Sanitizer')).toBeDefined();
    expect(screen.getByText('ToDesk.exe')).toBeDefined();
    expect(screen.getByText('SunloginClient.exe')).toBeDefined();
    expect(screen.getByText('mstsc.exe')).toBeDefined();
    expect(screen.getByText('Ctrl+Alt+Backspace')).toBeDefined();
  });

  it('triggers emergency flush when Flush Now button is clicked', async () => {
    await act(async () => {
      render(<RemoteBoostPage />);
    });

    const flushBtn = screen.getByRole('button', { name: /Flush Now/i });
    await act(async () => {
      fireEvent.click(flushBtn);
    });

    expect(bridgeSpy).toHaveBeenCalledWith('remote.emergencyFlush');
  });

  it('triggers reset defaults when Reset Defaults button is clicked', async () => {
    await act(async () => {
      render(<RemoteBoostPage />);
    });

    const resetBtn = screen.getByRole('button', { name: /Reset Defaults/i });
    await act(async () => {
      fireEvent.click(resetBtn);
    });

    expect(bridgeSpy).toHaveBeenCalledWith('remote.resetDefaults');
  });

  it('can add and remove custom remote target processes', async () => {
    await act(async () => {
      render(<RemoteBoostPage />);
    });

    const input = screen.getByPlaceholderText('e.g., CustomRemote.exe');
    const addBtn = screen.getByRole('button', { name: /Add/i });

    // Add new process
    await act(async () => {
      fireEvent.change(input, { target: { value: 'CustomViewer' } });
      fireEvent.click(addBtn);
    });

    expect(bridgeSpy).toHaveBeenCalledWith('remote.updateSettings', {
      targetProcesses: [...initialSettings.targetProcesses, 'CustomViewer.exe'],
    });

    // Remove existing process
    const removeBtn = screen.getByLabelText('Delete mstsc.exe');
    await act(async () => {
      fireEvent.click(removeBtn);
    });

    expect(bridgeSpy).toHaveBeenCalledWith('remote.updateSettings', {
      targetProcesses: ['ToDesk.exe', 'SunloginClient.exe', 'CustomViewer.exe'],
    });
  });

  it('updates state dynamically when remote canvas becomes active', async () => {
    bridgeSpy.mockImplementation(async (method: string) => {
      if (method === 'remote.getSettings') return initialSettings;
      if (method === 'remote.getState') {
        return {
          isRemoteForeground: true,
          imeSanitized: true,
          activeProcess: 'ToDesk.exe',
        };
      }
      return null;
    });

    await act(async () => {
      render(<RemoteBoostPage />);
    });

    await waitFor(() => {
      expect(screen.getByText('Controlling remote canvas:')).toBeDefined();
      expect(screen.getByText('ENG 0409 Active')).toBeDefined();
    });
  });
});

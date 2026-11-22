/* @vitest-environment jsdom */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, fireEvent, waitFor, cleanup } from '@testing-library/react';
import { ClipboardPage } from './ClipboardPage';

const bridgeRequestMock = vi.fn();

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
  useBridgeEvent: vi.fn(),
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, opts?: string | { defaultValue?: string }) => {
      if (typeof opts === 'string') return opts;
      if (opts && typeof opts === 'object' && opts.defaultValue) return opts.defaultValue;
      return key;
    },
  }),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
  },
}));

describe('ClipboardPage', () => {
  afterEach(() => cleanup());

  beforeEach(() => {
    bridgeRequestMock.mockReset();
    bridgeRequestMock.mockImplementation(async (method: string) => {
      if (method === 'clipboard.getSettings') {
        return {
          enabled: true,
          hotkey: 'Alt+V',
          maxItems: 500,
          maxDays: 30,
          maxImageMb: 16,
          autoPaste: true,
          plainTextByDefault: false,
          captureImages: true,
          captureFiles: true,
          excludedApps: 'KeePass.exe,Bitwarden.exe',
        };
      }
      if (method === 'clipboard.getStats') {
        return {
          totalCount: 42,
          pinnedCount: 5,
          imageCount: 8,
          diskBytes: 1048576,
          dataDir: 'C:\\Users\\test\\AppData\\Local\\Tools3000\\clipboard',
        };
      }
      return { success: true, removed: 37 };
    });
  });

  it('loads persistent stats and invokes clearHistory keeping pinned items', async () => {
    render(<ClipboardPage />);

    await waitFor(() => {
      expect(screen.getByText('42')).toBeTruthy();
    });
    expect(screen.getByText('1.00 MB')).toBeTruthy();

    fireEvent.click(screen.getByRole('button', { name: 'Clear Unpinned' }));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.clearHistory', { keepPinned: true });
    });
  });
});

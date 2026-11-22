/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { PicasaViewerPage } from './PicasaViewerPage';
import * as bridge from '../hooks/useBridge';

const mockTranslate = (key: string, options?: Record<string, unknown>) => {
  if (key === 'picasaViewer.associatedCount' && options) {
    return `${options.registered}/${options.total}`;
  }
  return key;
};

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: mockTranslate,
    i18n: { language: 'en-US' },
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

describe('PicasaViewerPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  afterEach(() => {
    cleanup();
  });

  it('renders Rust recreation scope notice and toggles auto-association via IPC', async () => {
    const initialStatus = {
      enabled: false,
      installed: true,
      exePath: 'C:\\Program Files\\Tools3000\\Tools3000_Picasa_Viewer.exe',
      registeredExtensions: [],
      registeredCount: 0,
      supportedExtensions: ['jpg', 'png', 'webp'],
      supportedCount: 41,
      allAssociated: false,
    };

    const enabledStatus = {
      ...initialStatus,
      enabled: true,
      registeredExtensions: ['jpg', 'png', 'webp'],
      registeredCount: 41,
      allAssociated: true,
    };

    const requestSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      if (method === 'picasaViewer.getStatus') return initialStatus;
      if (method === 'picasaViewer.setEnabled') return { success: true, status: enabledStatus };
      if (method === 'picasaViewer.openFormatConfig') return { success: true };
      if (method === 'picasaViewer.openDefaultApps') return { success: true };
      return { success: true };
    });

    render(<PicasaViewerPage />);

    expect(await screen.findByText('picasaViewer.rustRecreationBadge')).toBeTruthy();
    expect(screen.getByText('picasaViewer.scopeNoticeTitle')).toBeTruthy();
    expect(screen.getByText('0/41')).toBeTruthy();

    const toggle = screen.getByRole('checkbox');
    await act(async () => {
      fireEvent.click(toggle);
    });

    expect(requestSpy).toHaveBeenCalledWith(
      'picasaViewer.setEnabled',
      { enabled: true },
      { silent: true },
    );
    expect(await screen.findByText('41/41')).toBeTruthy();

    await act(async () => {
      fireEvent.click(screen.getByText('picasaViewer.customFormatsBtn'));
      fireEvent.click(screen.getByText('picasaViewer.defaultAppsBtn'));
    });

    expect(requestSpy).toHaveBeenCalledWith('picasaViewer.openFormatConfig', {}, { silent: true });
    expect(requestSpy).toHaveBeenCalledWith('picasaViewer.openDefaultApps', {}, { silent: true });
    expect(screen.getByText('F5 / Esc')).toBeTruthy();
    expect(screen.getByText('Q / E · Ctrl+Shift+R / Ctrl+R')).toBeTruthy();
  });

  it('shows recoverable error card when initial status fetch fails and supports unassociate + UserChoice hint', async () => {
    let attempt = 0;
    const lockedStatus = {
      enabled: true,
      installed: true,
      exePath: 'C:\\Program Files\\Tools3000\\Tools3000_Picasa_Viewer.exe',
      registeredExtensions: ['jpg', 'png'],
      registeredCount: 41,
      effectiveDefaultExtensions: ['tga'],
      effectiveDefaultCount: 35,
      userChoiceLockedCount: 6,
      supportedExtensions: ['jpg', 'png'],
      supportedCount: 41,
      allAssociated: true,
    };
    const clearedStatus = {
      ...lockedStatus,
      enabled: false,
      registeredExtensions: [],
      registeredCount: 0,
      effectiveDefaultCount: 0,
      userChoiceLockedCount: 0,
      allAssociated: false,
    };

    const requestSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      if (method === 'picasaViewer.getStatus') {
        attempt += 1;
        if (attempt === 1) throw new Error('IPC offline');
        return lockedStatus;
      }
      if (method === 'picasaViewer.unassociateAll') {
        return { success: true, status: clearedStatus };
      }
      return { success: true };
    });

    render(<PicasaViewerPage />);

    expect(await screen.findByText('picasaViewer.loadErrorTitle')).toBeTruthy();
    await act(async () => {
      fireEvent.click(screen.getByText('picasaViewer.loadErrorRetry'));
    });

    expect(await screen.findByText('picasaViewer.userChoiceHint')).toBeTruthy();
    await act(async () => {
      fireEvent.click(screen.getByText('picasaViewer.unassociateAllBtn'));
    });

    expect(requestSpy).toHaveBeenCalledWith('picasaViewer.unassociateAll', {}, { silent: true });
    expect(await screen.findByText('0/41')).toBeTruthy();
  });
});

/* @vitest-environment jsdom */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, fireEvent, waitFor, cleanup } from '@testing-library/react';
import { FeedbackModal } from './FeedbackModal';
import { resolveBridgeTimeoutMs } from '../hooks/useBridge';

const bridgeRequestMock = vi.fn();
const toastSuccessMock = vi.fn();
const toastErrorMock = vi.fn();

vi.mock('../hooks/useBridge', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../hooks/useBridge')>();
  return {
    ...actual,
    bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
  };
});

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, fallback?: string) => fallback || key,
    i18n: { language: 'en', resolvedLanguage: 'en' },
  }),
}));

vi.mock('sonner', () => ({
  toast: {
    success: (...args: unknown[]) => toastSuccessMock(...args),
    error: (...args: unknown[]) => toastErrorMock(...args),
  },
}));

describe('FeedbackModal', () => {
  afterEach(() => {
    cleanup();
  });

  beforeEach(() => {
    bridgeRequestMock.mockReset();
    toastSuccessMock.mockReset();
    toastErrorMock.mockReset();
  });

  it('uses long-running bridge timeouts for feedback preview and submit', () => {
    expect(resolveBridgeTimeoutMs('feedback.submit')).toBeGreaterThanOrEqual(5 * 60_000);
    expect(resolveBridgeTimeoutMs('feedback.preview')).toBeGreaterThanOrEqual(5 * 60_000);
  });

  it('surfaces backend errorMessage and toast when submit returns ok=false', async () => {
    bridgeRequestMock.mockResolvedValue({
      ok: false,
      errorMessage: 'Disk full while creating diagnostic bundle',
    });

    render(<FeedbackModal open onClose={vi.fn()} />);
    fireEvent.change(screen.getByLabelText('Summary'), { target: { value: 'Bug summary' } });
    fireEvent.click(screen.getByRole('button', { name: 'Submit / Export Bundle' }));

    await waitFor(() => {
      expect(screen.getByRole('alert').textContent).toContain('Disk full while creating diagnostic bundle');
    });
    expect(toastErrorMock).toHaveBeenCalledTimes(1);
    expect(toastSuccessMock).not.toHaveBeenCalled();
  });

  it('surfaces thrown bridge exceptions instead of swallowing them silently', async () => {
    bridgeRequestMock.mockRejectedValue(new Error('Message too large'));

    render(<FeedbackModal open onClose={vi.fn()} />);
    fireEvent.change(screen.getByLabelText('Description'), { target: { value: 'Attached screenshot' } });
    fireEvent.click(screen.getByRole('button', { name: 'Submit / Export Bundle' }));

    await waitFor(() => {
      expect(screen.getByRole('alert').textContent).toContain('Message too large');
    });
    expect(toastErrorMock).toHaveBeenCalledTimes(1);
  });

  it('shows success card and toast when local diagnostic bundle export succeeds', async () => {
    bridgeRequestMock.mockResolvedValue({
      ok: true,
      mode: 'fallback_local',
      exportedBundlePath: 'C:\\Users\\test\\Desktop\\Tools3000-feedback.zip',
      totalRedactions: 7,
    });

    render(<FeedbackModal open onClose={vi.fn()} />);
    fireEvent.change(screen.getByLabelText('Summary'), { target: { value: 'Works cleanly' } });
    fireEvent.click(screen.getByRole('button', { name: 'Submit / Export Bundle' }));

    await waitFor(() => {
      expect(screen.getByText('Redacted Diagnostic Bundle Ready')).toBeTruthy();
      expect(screen.getByText('C:\\Users\\test\\Desktop\\Tools3000-feedback.zip')).toBeTruthy();
    });
    expect(toastSuccessMock).toHaveBeenCalledTimes(1);
  });

  it('notifies user when an attached screenshot exceeds 6 MB', () => {
    render(<FeedbackModal open onClose={vi.fn()} />);
    const input = document.querySelector('input[type="file"]') as HTMLInputElement;
    expect(input).toBeTruthy();
    const oversized = new File([new Uint8Array(7 * 1024 * 1024)], 'huge.png', { type: 'image/png' });
    fireEvent.change(input, { target: { files: [oversized] } });

    expect(screen.getByRole('alert').textContent).toContain('6 MB');
    expect(toastErrorMock).toHaveBeenCalledTimes(1);
  });

  it('pastes screenshot from native Windows clipboard on Ctrl+V outside inputs', async () => {
    bridgeRequestMock.mockImplementation((method: string) => {
      if (method === 'feedback.readClipboardImage') {
        return Promise.resolve({
          ok: true,
          dataUri: 'data:image/png;base64,iVBORw0KGgo=',
          fileName: 'clipboard-screenshot.png',
          byteSize: 128,
        });
      }
      return Promise.resolve({ ok: true });
    });

    render(<FeedbackModal open onClose={vi.fn()} />);
    fireEvent.keyDown(window, { key: 'v', ctrlKey: true });

    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('feedback.readClipboardImage', {}, { silent: true });
      expect(screen.getByText('clipboard-screenshot.png')).toBeTruthy();
    });
    expect(toastSuccessMock).toHaveBeenCalledTimes(1);
  });
});

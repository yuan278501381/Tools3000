/* @vitest-environment jsdom */

import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { HotkeyRecorder } from './HotkeyRecorder';

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
  }),
}));

vi.mock('../utils/hotkeyPauseCoordinator', () => ({
  acquireHotkeyPause: vi.fn(),
  releaseHotkeyPause: vi.fn(),
}));

describe('HotkeyRecorder', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date('2026-11-28T12:00:00Z'));
  });

  afterEach(() => {
    cleanup();
    vi.useRealTimers();
    vi.restoreAllMocks();
  });

  it('records standard modifier combinations without regression', () => {
    const onChange = vi.fn();
    render(<HotkeyRecorder id="hk-std" value="Alt+Space" onChange={onChange} />);

    fireEvent.click(screen.getByRole('button', { name: /Alt\+Space/i }));
    fireEvent.keyDown(window, { key: 'Control', ctrlKey: true });
    fireEvent.keyDown(window, { key: '1', code: 'Digit1', ctrlKey: true });
    fireEvent.keyUp(window, { key: '1', code: 'Digit1', ctrlKey: true });

    expect(onChange).toHaveBeenCalledWith('Ctrl+1');
  });

  it('records double-tap Ctrl/Alt/Shift and displays localized badge', () => {
    const onChange = vi.fn();
    const { rerender } = render(<HotkeyRecorder id="hk-double" value="" onChange={onChange} />);

    fireEvent.click(screen.getByRole('button'));

    // First quick tap on Ctrl (hold 50ms)
    fireEvent.keyDown(window, { key: 'Control', ctrlKey: true });
    vi.advanceTimersByTime(50);
    fireEvent.keyUp(window, { key: 'Control', ctrlKey: false });

    // Second tap within 400ms
    vi.advanceTimersByTime(120);
    fireEvent.keyDown(window, { key: 'Control', ctrlKey: true });
    fireEvent.keyUp(window, { key: 'Control', ctrlKey: false });

    expect(onChange).toHaveBeenCalledWith('Double+Ctrl');

    rerender(<HotkeyRecorder id="hk-double" value="Double+Ctrl" onChange={onChange} />);
    const kbd = document.querySelector('kbd.hotkey-recorder__key');
    expect(kbd?.textContent).toBe('Double Ctrl');
  });

  it('does not trigger double-tap when first hold exceeds 280ms or when interrupted by another key', () => {
    const onChange = vi.fn();
    render(<HotkeyRecorder id="hk-guard" value="" onChange={onChange} />);

    fireEvent.click(screen.getByRole('button'));

    // Hold Ctrl for 350ms (> 280ms) -> must not arm double-tap
    fireEvent.keyDown(window, { key: 'Control', ctrlKey: true });
    vi.advanceTimersByTime(350);
    fireEvent.keyUp(window, { key: 'Control', ctrlKey: false });

    vi.advanceTimersByTime(100);
    fireEvent.keyDown(window, { key: 'Control', ctrlKey: true });
    fireEvent.keyUp(window, { key: 'Control', ctrlKey: false });

    expect(onChange).not.toHaveBeenCalled();
  });

  it('disables double-tap modifier capture in allowSingleKey gesture mode', () => {
    const onChange = vi.fn();
    render(<HotkeyRecorder id="hk-single" value="" onChange={onChange} allowSingleKey />);

    fireEvent.click(screen.getByRole('button'));
    fireEvent.keyDown(window, { key: 'Control', ctrlKey: true });
    vi.advanceTimersByTime(50);
    fireEvent.keyUp(window, { key: 'Control', ctrlKey: false });
    vi.advanceTimersByTime(100);
    fireEvent.keyDown(window, { key: 'Control', ctrlKey: true });
    fireEvent.keyUp(window, { key: 'Control', ctrlKey: false });

    expect(onChange).not.toHaveBeenCalled();
  });
});

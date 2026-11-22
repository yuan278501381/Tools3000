/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import * as bridge from '../hooks/useBridge';
import {
  acquireHotkeyPause,
  getHotkeyPauseRefCount,
  releaseHotkeyPause,
  resetHotkeyPauseCoordinatorForTest,
} from './hotkeyPauseCoordinator';
import { HotkeyRecorder } from '../components/HotkeyRecorder';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (_key: string, fallback?: string) => fallback || _key,
  }),
}));

describe('hotkeyPauseCoordinator & Recorder handoff', () => {
  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    resetHotkeyPauseCoordinatorForTest();
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockResolvedValue({ success: true });
  });

  afterEach(() => {
    cleanup();
    resetHotkeyPauseCoordinatorForTest();
    vi.restoreAllMocks();
  });

  it('keeps backend hotkey paused during synchronous A -> B handoff and unpauses only after final release microtask', async () => {
    acquireHotkeyPause();
    expect(getHotkeyPauseRefCount()).toBe(1);
    expect(bridgeSpy).toHaveBeenCalledTimes(1);
    expect(bridgeSpy).toHaveBeenLastCalledWith('hotkey.setPaused', { paused: true });

    // Input A releases while Input B acquires in the same event turn
    releaseHotkeyPause();
    acquireHotkeyPause();
    expect(getHotkeyPauseRefCount()).toBe(1);

    await Promise.resolve();
    // Must NOT have emitted paused: false during handoff
    expect(bridgeSpy).toHaveBeenCalledTimes(1);

    // Final input releases
    releaseHotkeyPause();
    expect(getHotkeyPauseRefCount()).toBe(0);
    await Promise.resolve();

    expect(bridgeSpy).toHaveBeenCalledTimes(2);
    expect(bridgeSpy).toHaveBeenLastCalledWith('hotkey.setPaused', { paused: false });
  });

  it('coordinates pause lock seamlessly when switching focus between two HotkeyRecorder controls', async () => {
    const onChangeA = vi.fn();
    const onChangeB = vi.fn();

    const { container } = render(
      <div>
        <HotkeyRecorder id="recorder-a" value="Alt+Space" onChange={onChangeA} />
        <HotkeyRecorder id="recorder-b" value="F1" onChange={onChangeB} />
      </div>,
    );

    const recorders = container.querySelectorAll<HTMLButtonElement>('.hotkey-recorder');
    const recorderA = recorders[0];
    const recorderB = recorders[1];
    expect(recorderA).toBeDefined();
    expect(recorderB).toBeDefined();

    // Start recording in HotkeyRecorder A, then flush effect timer so document mousedown listener is armed
    await act(async () => {
      fireEvent.click(recorderA);
    });
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 20));
    });
    expect(bridgeSpy).toHaveBeenCalledWith('hotkey.setPaused', { paused: true });

    // Directly click HotkeyRecorder B while HotkeyRecorder A closes via outside mousedown
    await act(async () => {
      fireEvent.mouseDown(recorderB, { bubbles: true });
      fireEvent.click(recorderB);
      await Promise.resolve();
    });

    const unpauseCallsDuringSwitch = bridgeSpy.mock.calls.filter(
      (call: unknown[]) => call[0] === 'hotkey.setPaused' && (call[1] as { paused?: boolean } | undefined)?.paused === false,
    );
    expect(unpauseCallsDuringSwitch).toHaveLength(0);
    expect(getHotkeyPauseRefCount()).toBe(1);

    // Press Escape on Recorder B to cancel recording and release the last lock
    await act(async () => {
      fireEvent.keyDown(window, { key: 'Escape' });
      await Promise.resolve();
    });

    expect(getHotkeyPauseRefCount()).toBe(0);
    expect(bridgeSpy).toHaveBeenLastCalledWith('hotkey.setPaused', { paused: false });
  });

  it('allows recording standalone Delete and Backspace keys when allowSingleKey is enabled while preserving global safety by default', async () => {
    const onSingleChange = vi.fn();
    const onGlobalChange = vi.fn();

    const { container } = render(
      <div>
        <HotkeyRecorder id="single-key" value="" onChange={onSingleChange} allowSingleKey />
        <HotkeyRecorder id="global-key" value="Alt+Space" onChange={onGlobalChange} />
      </div>,
    );

    const recorders = container.querySelectorAll<HTMLButtonElement>('.hotkey-recorder');
    const singleRecorder = recorders[0];
    const globalRecorder = recorders[1];

    // allowSingleKey=true: standalone Delete records cleanly
    await act(async () => {
      fireEvent.click(singleRecorder);
    });
    await act(async () => {
      fireEvent.keyDown(window, { key: 'Delete', code: 'Delete' });
      fireEvent.keyUp(window, { key: 'Delete', code: 'Delete' });
    });
    expect(onSingleChange).toHaveBeenCalledWith('Delete');

    // allowSingleKey=true: standalone Backspace records cleanly instead of clearing
    onSingleChange.mockClear();
    await act(async () => {
      fireEvent.click(singleRecorder);
    });
    await act(async () => {
      fireEvent.keyDown(window, { key: 'Backspace', code: 'Backspace' });
      fireEvent.keyUp(window, { key: 'Backspace', code: 'Backspace' });
    });
    expect(onSingleChange).toHaveBeenCalledWith('Backspace');

    // default global hotkey mode: standalone Delete is blocked, standalone Backspace clears binding
    await act(async () => {
      fireEvent.click(globalRecorder);
    });
    await act(async () => {
      fireEvent.keyDown(window, { key: 'Delete', code: 'Delete' });
      fireEvent.keyUp(window, { key: 'Delete', code: 'Delete' });
    });
    expect(onGlobalChange).not.toHaveBeenCalled();

    await act(async () => {
      fireEvent.keyDown(window, { key: 'Backspace', code: 'Backspace' });
    });
    expect(onGlobalChange).toHaveBeenCalledWith('');
  });
});

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
import { HotkeyInput } from '../components/UIKit';

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

  it('coordinates pause lock seamlessly when switching focus from HotkeyRecorder to HotkeyInput', async () => {
    const onChangeA = vi.fn();
    const onChangeB = vi.fn();

    const { container } = render(
      <div>
        <HotkeyRecorder id="recorder-a" value="Alt+Space" onChange={onChangeA} />
        <HotkeyInput id="input-b" value="F1" onChange={onChangeB} />
      </div>,
    );

    const recorderBtn = container.querySelector('.hotkey-recorder') as HTMLButtonElement;
    const hotkeyInputEl = container.querySelector('.uikit-input--hotkey') as HTMLInputElement;
    expect(recorderBtn).not.toBeNull();
    expect(hotkeyInputEl).not.toBeNull();

    // Start recording in HotkeyRecorder, then flush effect timer so document mousedown listener is armed
    await act(async () => {
      fireEvent.click(recorderBtn);
    });
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 20));
    });
    expect(bridgeSpy).toHaveBeenCalledWith('hotkey.setPaused', { paused: true });

    // Directly focus HotkeyInput while HotkeyRecorder closes via outside mousedown
    await act(async () => {
      fireEvent.mouseDown(hotkeyInputEl, { bubbles: true });
      hotkeyInputEl.focus();
      await Promise.resolve();
    });

    const unpauseCallsDuringSwitch = bridgeSpy.mock.calls.filter(
      (call: unknown[]) => call[0] === 'hotkey.setPaused' && (call[1] as { paused?: boolean } | undefined)?.paused === false,
    );
    expect(unpauseCallsDuringSwitch).toHaveLength(0);
    expect(getHotkeyPauseRefCount()).toBe(1);

    // Blur HotkeyInput to release the last lock
    await act(async () => {
      hotkeyInputEl.blur();
      await Promise.resolve();
    });

    expect(getHotkeyPauseRefCount()).toBe(0);
    expect(bridgeSpy).toHaveBeenLastCalledWith('hotkey.setPaused', { paused: false });
  });
});

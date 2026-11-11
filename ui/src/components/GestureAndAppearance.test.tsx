/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { GestureStrokePreview } from './GestureStrokePreview';
import { GestureDrawCanvas } from './GestureDrawCanvas';
import { GeneralPage } from '../pages/GeneralPage';
import * as bridge from '../hooks/useBridge';

vi.mock('react-i18next', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-i18next')>();
  return {
    ...actual,
    useTranslation: () => ({
      t: (key: string, fallbackOrOpts?: string | { defaultValue?: string }) => {
        if (typeof fallbackOrOpts === 'string') return fallbackOrOpts;
        if (fallbackOrOpts && typeof fallbackOrOpts.defaultValue === 'string') {
          return fallbackOrOpts.defaultValue;
        }
        return key;
      },
      i18n: {
        language: 'en',
        changeLanguage: vi.fn(),
      },
    }),
  };
});

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
    info: vi.fn(),
    loading: vi.fn(),
  },
}));

describe('U-06 GestureStrokePreview & GestureDrawCanvas Regression Suite', () => {
  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('renders dedicated vector wheel badge for WheelUp / TopEdge+WheelDown without fake polyline strokes', () => {
    const { container, rerender } = render(<GestureStrokePreview code="WheelUp" />);

    const wheelRoot = container.querySelector('[data-gesture-kind="wheel"]');
    expect(wheelRoot).not.toBeNull();
    expect(wheelRoot?.getAttribute('data-wheel-dir')).toBe('up');
    expect(wheelRoot?.getAttribute('data-gesture-edge')).toBe('none');
    expect(container.querySelector('.gesture-stroke-preview__stroke')).toBeNull();
    expect(container.querySelector('.gesture-stroke-preview__edge-pill')).toBeNull();

    rerender(<GestureStrokePreview code="TopEdge+WheelDown" />);
    const edgeWheelRoot = container.querySelector('[data-gesture-kind="wheel"]');
    expect(edgeWheelRoot).not.toBeNull();
    expect(edgeWheelRoot?.getAttribute('data-wheel-dir')).toBe('down');
    expect(edgeWheelRoot?.getAttribute('data-gesture-edge')).toBe('top');
    expect(container.querySelector('.gesture-stroke-preview__edge-pill')?.textContent).toContain('Top');
    expect(container.querySelector('.gesture-stroke-preview__stroke')).toBeNull();
  });

  it('renders directional strokes with edge pill and falls back cleanly on unknown tokens', () => {
    const { container, rerender } = render(<GestureStrokePreview code="RightEdge+X1+U-R" />);
    const strokeRoot = container.querySelector('[data-gesture-kind="stroke"]');
    expect(strokeRoot).not.toBeNull();
    expect(strokeRoot?.getAttribute('data-gesture-edge')).toBe('right');
    expect(container.querySelector('.gesture-stroke-preview__stroke')).not.toBeNull();
    expect(container.querySelector('.gesture-stroke-preview__edge-pill')?.textContent).toContain('Right');

    rerender(<GestureStrokePreview code="BogusToken" />);
    expect(container.querySelector('[data-gesture-kind="fallback"]')).not.toBeNull();
    expect(container.querySelector('.gesture-stroke-preview__stroke')).toBeNull();
  });

  it('records pure global gesture even when pointer starts within left 2.5% of canvas in default Global mode, and only emits Edge prefix when explicitly selected', () => {
    const onChange = vi.fn();
    const { container, rerender } = render(<GestureDrawCanvas value="" onChange={onChange} />);

    const globalPill = container.querySelector('[data-edge-option="none"]') as HTMLButtonElement;
    const leftEdgePill = container.querySelector('[data-edge-option="left"]') as HTMLButtonElement;
    const topEdgePill = container.querySelector('[data-edge-option="top"]') as HTMLButtonElement;
    const pad = container.querySelector('.gesture-draw-pad') as HTMLDivElement;

    expect(globalPill.getAttribute('aria-checked')).toBe('true');
    expect(pad.getAttribute('data-active-edge')).toBe('none');
    expect(container.querySelector('[data-testid^="edge-sensor-"]')).toBeNull();

    vi.spyOn(pad, 'getBoundingClientRect').mockReturnValue({
      left: 0,
      top: 0,
      right: 400,
      bottom: 240,
      width: 400,
      height: 240,
      x: 0,
      y: 0,
      toJSON: () => ({}),
    });

    // 在画板最左侧 (x=10 -> 2.5%) 且靠顶部 (y=18 -> 7.5%) 按下右键向右划动
    fireEvent.pointerDown(pad, { clientX: 10, clientY: 18, button: 2, pointerId: 1 });
    fireEvent.pointerMove(pad, { clientX: 60, clientY: 19, button: 2, pointerId: 1 });
    fireEvent.pointerMove(pad, { clientX: 130, clientY: 20, button: 2, pointerId: 1 });
    fireEvent.pointerUp(pad, { clientX: 130, clientY: 20, button: 2, pointerId: 1 });

    // 默认 Global 模式下必须 100% 录为纯正 'R'，绝不静默篡改为 'LeftEdge+R' 或 'TopEdge+R'
    expect(onChange).toHaveBeenLastCalledWith('R');

    // 用户主动切换到 Left Edge 模式：渲染左侧高对比度虚线感应带并录制为 'LeftEdge+R'
    fireEvent.click(leftEdgePill);
    expect(leftEdgePill.getAttribute('aria-checked')).toBe('true');
    expect(screen.getByTestId('edge-sensor-left')).toBeDefined();

    fireEvent.pointerDown(pad, { clientX: 200, clientY: 120, button: 2, pointerId: 2 });
    fireEvent.pointerMove(pad, { clientX: 260, clientY: 120, button: 2, pointerId: 2 });
    fireEvent.pointerMove(pad, { clientX: 320, clientY: 120, button: 2, pointerId: 2 });
    fireEvent.pointerUp(pad, { clientX: 320, clientY: 120, button: 2, pointerId: 2 });

    expect(onChange).toHaveBeenLastCalledWith('LeftEdge+R');

    // 切换到 Top Edge 模式并滚动鼠标滚轮向下 -> 录制为 'TopEdge+WheelDown'
    rerender(<GestureDrawCanvas value="" onChange={onChange} />);
    fireEvent.click(topEdgePill);
    expect(screen.getByTestId('edge-sensor-top')).toBeDefined();
    fireEvent.wheel(pad, { deltaY: 100 });
    expect(onChange).toHaveBeenLastCalledWith('TopEdge+WheelDown');
  });
});

describe('V-01 GeneralPage Theme & Accent Broadcast Synchronization', () => {
  let bridgeSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    vi.clearAllMocks();
    localStorage.clear();
    document.documentElement.removeAttribute('data-theme');
    document.documentElement.removeAttribute('data-accent');
    vi.spyOn(bridge, 'useBridgeEvent').mockImplementation(() => {});
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string) => {
      if (method === 'general.getSettings') {
        return {
          autoStart: false,
          runAsAdmin: true,
          minimizeToTray: true,
          checkUpdates: true,
          language: 'en',
          logLevel: 'info',
          theme: 'dark',
          accentColor: 'cyan',
        };
      }
      if (method === 'hotkey.getAll') {
        return [];
      }
      if (method === 'general.updateSettings') {
        return { success: true };
      }
      return { success: true };
    });
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('hydrates initial theme + accentColor into DOM/localStorage and persists paired { theme, accentColor } on accent or theme switch', async () => {
    await act(async () => {
      render(<GeneralPage />);
    });

    expect(document.documentElement.getAttribute('data-theme')).toBe('dark');
    expect(document.documentElement.getAttribute('data-accent')).toBe('cyan');
    expect(localStorage.getItem('tools3000:theme')).toBe('dark');
    expect(localStorage.getItem('tools3000:accent-color')).toBe('cyan');

    // 点击切换强调色为 violet
    const violetSwatch = screen.getByTitle('general.accentViolet');
    await act(async () => {
      fireEvent.click(violetSwatch);
    });

    expect(localStorage.getItem('tools3000:accent-color')).toBe('violet');
    expect(document.documentElement.getAttribute('data-accent')).toBe('violet');
    expect(bridgeSpy).toHaveBeenCalledWith('general.updateSettings', {
      theme: 'dark',
      accentColor: 'violet',
    });

    // 切换主题为 light，验证 DOM data-theme、localStorage 与成对 { theme, accentColor } 后端提交
    const themeSelectBtn = document.getElementById('theme') as HTMLButtonElement;
    expect(themeSelectBtn).not.toBeNull();
    await act(async () => {
      fireEvent.click(themeSelectBtn);
    });
    const lightOption = screen.getByText('general.themeLight');
    await act(async () => {
      fireEvent.click(lightOption);
    });

    expect(localStorage.getItem('tools3000:theme')).toBe('light');
    expect(document.documentElement.getAttribute('data-theme')).toBe('light');
    expect(bridgeSpy).toHaveBeenCalledWith('general.updateSettings', {
      theme: 'light',
      accentColor: 'violet',
    });

    // 模拟后端拒绝下一次保存，验证本地 DOM 与 localStorage 自动回滚
    bridgeSpy.mockImplementationOnce(async () => ({ success: false, error: 'disk locked' }));
    const coralSwatch = screen.getByTitle('general.accentCoral');
    await act(async () => {
      fireEvent.click(coralSwatch);
    });

    expect(localStorage.getItem('tools3000:accent-color')).toBe('violet');
    expect(document.documentElement.getAttribute('data-accent')).toBe('violet');
  });
});

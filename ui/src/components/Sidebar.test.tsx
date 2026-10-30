/* @vitest-environment jsdom */

import { cleanup, fireEvent, render } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Sidebar } from './Sidebar';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string) => key,
  }),
}));

describe('Sidebar Component', () => {
  const onNavigateMock = vi.fn();

  beforeEach(() => {
    vi.clearAllMocks();
  });

  afterEach(() => {
    cleanup();
  });

  it('renders only enabled tools when only gesture is active (user VM test scenario)', () => {
    const activePlugins = new Set(['gesture']);
    render(
      <Sidebar
        activeNav="gesture"
        onNavigate={onNavigateMock}
        activePlugins={activePlugins}
      />
    );

    // 系统总控
    expect(document.getElementById('nav-general')).toBeTruthy();
    expect(document.getElementById('nav-plugins')).toBeTruthy();

    // 仅展示手势及其附属触发角
    expect(document.getElementById('nav-gesture')).toBeTruthy();
    expect(document.getElementById('nav-hotcorner')).toBeTruthy();

    // 未启用的核心模块坚决不展示在侧边栏
    expect(document.getElementById('nav-search')).toBeNull();
    expect(document.getElementById('nav-capture')).toBeNull();
    expect(document.getElementById('nav-history')).toBeNull();
    expect(document.getElementById('nav-ocr')).toBeNull();
    expect(document.getElementById('nav-keycast')).toBeNull();
    expect(document.getElementById('nav-spotlight')).toBeNull();
    expect(document.getElementById('nav-dialog_enhancer')).toBeNull();
    expect(document.getElementById('nav-remote_boost')).toBeNull();

    // 洞察与关于
    expect(document.getElementById('nav-stats')).toBeTruthy();
    expect(document.getElementById('nav-about')).toBeTruthy();
  });

  it('hides all core tools requiring plugins when activePlugins is undefined', () => {
    render(
      <Sidebar
        activeNav="general"
        onNavigate={onNavigateMock}
        activePlugins={undefined}
      />
    );

    expect(document.getElementById('nav-general')).toBeTruthy();
    expect(document.getElementById('nav-plugins')).toBeTruthy();
    expect(document.getElementById('nav-stats')).toBeTruthy();
    expect(document.getElementById('nav-about')).toBeTruthy();

    expect(document.getElementById('nav-search')).toBeNull();
    expect(document.getElementById('nav-gesture')).toBeNull();
    expect(document.getElementById('nav-hotcorner')).toBeNull();
    expect(document.getElementById('nav-capture')).toBeNull();
    expect(document.getElementById('nav-history')).toBeNull();
    expect(document.getElementById('nav-ocr')).toBeNull();
    expect(document.getElementById('nav-keycast')).toBeNull();
    expect(document.getElementById('nav-spotlight')).toBeNull();
    expect(document.getElementById('nav-dialog_enhancer')).toBeNull();
    expect(document.getElementById('nav-remote_boost')).toBeNull();
  });

  it('renders all core tools when all plugins are active', () => {
    const allActive = new Set([
      'gesture',
      'capture',
      'search',
      'keycast',
      'spotlight',
      'dialogenhancer',
      'remote_boost',
    ]);
    render(
      <Sidebar
        activeNav="general"
        onNavigate={onNavigateMock}
        activePlugins={allActive}
      />
    );

    expect(document.getElementById('nav-search')).toBeTruthy();
    expect(document.getElementById('nav-gesture')).toBeTruthy();
    expect(document.getElementById('nav-hotcorner')).toBeTruthy();
    expect(document.getElementById('nav-capture')).toBeTruthy();
    expect(document.getElementById('nav-history')).toBeTruthy();
    expect(document.getElementById('nav-ocr')).toBeTruthy();
    expect(document.getElementById('nav-keycast')).toBeTruthy();
    expect(document.getElementById('nav-spotlight')).toBeTruthy();
    expect(document.getElementById('nav-dialog_enhancer')).toBeTruthy();
    expect(document.getElementById('nav-remote_boost')).toBeTruthy();
  });

  it('correctly maps dialog_enhancer when activePlugins contains dialog_enhancer alias', () => {
    const activePlugins = new Set(['dialog_enhancer']);
    render(
      <Sidebar
        activeNav="general"
        onNavigate={onNavigateMock}
        activePlugins={activePlugins}
      />
    );

    expect(document.getElementById('nav-dialog_enhancer')).toBeTruthy();
    expect(document.getElementById('nav-gesture')).toBeNull();
  });

  it('invokes onNavigate callback with corresponding navId when clicked', () => {
    const activePlugins = new Set(['gesture']);
    render(
      <Sidebar
        activeNav="general"
        onNavigate={onNavigateMock}
        activePlugins={activePlugins}
      />
    );

    const gestureBtn = document.getElementById('nav-gesture');
    expect(gestureBtn).toBeTruthy();
    fireEvent.click(gestureBtn!);
    expect(onNavigateMock).toHaveBeenCalledWith('gesture');
  });

  it('sets aria-current="page" on the active nav item', () => {
    const activePlugins = new Set(['gesture']);
    render(
      <Sidebar
        activeNav="gesture"
        onNavigate={onNavigateMock}
        activePlugins={activePlugins}
      />
    );

    const gestureBtn = document.getElementById('nav-gesture');
    expect(gestureBtn?.getAttribute('aria-current')).toBe('page');

    const generalBtn = document.getElementById('nav-general');
    expect(generalBtn?.getAttribute('aria-current')).toBeNull();
  });
});

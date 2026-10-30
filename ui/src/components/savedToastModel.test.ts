/* @vitest-environment jsdom */

import { describe, expect, it, vi } from 'vitest';
import { showSavedToast, showSaveFailedToast, type SavedToastDetail } from './savedToastModel';
import { isSettingsMutationMethod } from '../hooks/useBridge';
import { getPageMetadata, PAGE_DEFINITIONS } from '../pages/registry';

describe('isSettingsMutationMethod (智能配置变更识别器)', () => {
  it('identifies explicit builtin settings methods', () => {
    expect(isSettingsMutationMethod('general.updateSettings')).toBe(true);
    expect(isSettingsMutationMethod('capture.updateSettings')).toBe(true);
    expect(isSettingsMutationMethod('recording.updateSettings')).toBe(true);
    expect(isSettingsMutationMethod('gesture.updateSettings')).toBe(true);
    expect(isSettingsMutationMethod('gesture.updateScopeRules')).toBe(true);
    expect(isSettingsMutationMethod('gesture.setTriggerState')).toBe(true);
    expect(isSettingsMutationMethod('gesture.saveMappings')).toBe(true);
    expect(isSettingsMutationMethod('search.saveSettings')).toBe(true);
    expect(isSettingsMutationMethod('dialog.updateConfig')).toBe(true);
    expect(isSettingsMutationMethod('hotcorner.updateSettings')).toBe(true);
    expect(isSettingsMutationMethod('ocr.updateSettings')).toBe(true);
    expect(isSettingsMutationMethod('config.set')).toBe(true);
    expect(isSettingsMutationMethod('plugins.setEnabled')).toBe(true);
    expect(isSettingsMutationMethod('hotkey.rebind')).toBe(true);
  });

  it('identifies future wildcard settings methods by naming convention', () => {
    expect(isSettingsMutationMethod('ai_assistant.updateSettings')).toBe(true);
    expect(isSettingsMutationMethod('clipboard.saveSettings')).toBe(true);
    expect(isSettingsMutationMethod('custom_plugin.updateConfig')).toBe(true);
    expect(isSettingsMutationMethod('workbench.saveConfig')).toBe(true);
    expect(isSettingsMutationMethod('theme.setSettings')).toBe(true);
  });

  it('rejects read-only and query methods', () => {
    expect(isSettingsMutationMethod('general.getSettings')).toBe(false);
    expect(isSettingsMutationMethod('plugins.getAll')).toBe(false);
    expect(isSettingsMutationMethod('search.query')).toBe(false);
    expect(isSettingsMutationMethod('stats.getToday')).toBe(false);
    expect(isSettingsMutationMethod('dialog.getFavorites')).toBe(false);
  });
});

describe('showSavedToast and showSaveFailedToast (事件分发机制)', () => {
  it('dispatches tools3000:settings-saved event with default payload', () => {
    const handler = vi.fn();
    window.addEventListener('tools3000:settings-saved', handler);

    showSavedToast();

    expect(handler).toHaveBeenCalledTimes(1);
    const event = handler.mock.calls[0][0] as CustomEvent<SavedToastDetail>;
    expect(event.detail).toEqual({});

    window.removeEventListener('tools3000:settings-saved', handler);
  });

  it('dispatches tools3000:settings-saved event with custom string message', () => {
    const handler = vi.fn();
    window.addEventListener('tools3000:settings-saved', handler);

    showSavedToast('快捷键已更新');

    expect(handler).toHaveBeenCalledTimes(1);
    const event = handler.mock.calls[0][0] as CustomEvent<SavedToastDetail>;
    expect(event.detail).toEqual({ message: '快捷键已更新' });

    window.removeEventListener('tools3000:settings-saved', handler);
  });

  it('dispatches tools3000:settings-save-failed event with error details', () => {
    const handler = vi.fn();
    window.addEventListener('tools3000:settings-save-failed', handler);

    showSaveFailedToast(new Error('快捷键冲突'), '注册失败');

    expect(handler).toHaveBeenCalledTimes(1);
    const event = handler.mock.calls[0][0] as CustomEvent<SavedToastDetail>;
    expect(event.detail.type).toBe('error');
    expect(event.detail.message).toBe('注册失败');
    expect(event.detail.duration).toBe(3500);

    window.removeEventListener('tools3000:settings-save-failed', handler);
  });
});

describe('getPageMetadata (页面注册表与鲁棒解析)', () => {
  it('correctly resolves metadata for all registered pages', () => {
    for (const page of PAGE_DEFINITIONS) {
      const meta = getPageMetadata(page.id);
      expect(meta.titleKey).toBe(page.titleKey);
      expect(meta.subtitleKey).toBe(page.subtitleKey);
    }
  });

  it('gracefully falls back for dynamically added future extensions', () => {
    const meta = getPageMetadata('my_dynamic_tool');
    expect(meta.titleKey).toBe('nav.my_dynamic_tool');
    expect(meta.subtitleKey).toBe('navSubtitle.my_dynamic_tool');
  });
});

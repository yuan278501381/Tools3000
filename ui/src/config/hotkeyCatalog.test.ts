import { describe, expect, it } from 'vitest';
import type { TFunction } from 'i18next';
import {
  HOTKEY_DOMAIN_GROUPS,
  HOTKEY_LABEL_KEYS,
  groupHotkeysByDomain,
  resolveHotkeyLabel,
  sortHotkeysByPriority,
} from './hotkeyCatalog';
import zhLocale from '../i18n/locales/zh.json';
import zhTwLocale from '../i18n/locales/zh-TW.json';
import enLocale from '../i18n/locales/en.json';
import jaLocale from '../i18n/locales/ja.json';
import koLocale from '../i18n/locales/ko.json';
import ruLocale from '../i18n/locales/ru.json';
import ptBrLocale from '../i18n/locales/pt-BR.json';
import esLocale from '../i18n/locales/es.json';

function readNestedKey(obj: Record<string, unknown>, keyPath: string): unknown {
  return keyPath.split('.').reduce<unknown>((acc, seg) => {
    if (acc && typeof acc === 'object' && seg in (acc as Record<string, unknown>)) {
      return (acc as Record<string, unknown>)[seg];
    }
    return undefined;
  }, obj);
}

describe('hotkeyCatalog SSOT', () => {
  it('maps Toggle Clipboard and every registered hotkey name to real localized strings in zh and en', () => {
    expect(HOTKEY_LABEL_KEYS['Toggle Clipboard']).toBe('onboarding.shortcutClipboard');

    for (const [hotkeyName, i18nKey] of Object.entries(HOTKEY_LABEL_KEYS)) {
      const zhVal = readNestedKey(zhLocale as unknown as Record<string, unknown>, i18nKey);
      const enVal = readNestedKey(enLocale as unknown as Record<string, unknown>, i18nKey);

      expect(typeof zhVal, `zh.json missing ${i18nKey} for ${hotkeyName}`).toBe('string');
      expect(typeof enVal, `en.json missing ${i18nKey} for ${hotkeyName}`).toBe('string');
      expect((zhVal as string).trim().length).toBeGreaterThan(0);
      expect((enVal as string).trim().length).toBeGreaterThan(0);
      // 中文映射结果不得退化成裸英文热键原名
      expect(zhVal).not.toBe(hotkeyName);
    }
  });

  it('orders core efficiency shortcuts ahead of recording audio mutes and groups into 4 domains across all 8 locales', () => {
    const allLocales = [
      zhLocale,
      zhTwLocale,
      enLocale,
      jaLocale,
      koLocale,
      ruLocale,
      ptBrLocale,
      esLocale,
    ] as unknown as Array<Record<string, unknown>>;

    for (const group of HOTKEY_DOMAIN_GROUPS) {
      for (const locale of allLocales) {
        const val = readNestedKey(locale, group.titleKey);
        expect(typeof val, `missing ${group.titleKey}`).toBe('string');
        expect((val as string).trim().length).toBeGreaterThan(0);
      }
    }

    // 模拟旧版按字母序返回的乱序列表（Mute Microphone 排第一、Screenshot 沉底）
    const alphabeticalInput = [
      { name: 'Dialog Inline Search', shortcut: 'Ctrl+Shift+G' },
      { name: 'Dialog Quick Jump', shortcut: 'Ctrl+G' },
      { name: 'Mute Microphone', shortcut: 'Ctrl+Alt+Shift+M' },
      { name: 'Mute System Audio', shortcut: 'Ctrl+Alt+Shift+S' },
      { name: 'OCR', shortcut: 'Ctrl+Shift+O' },
      { name: 'Pause Gestures', shortcut: 'Ctrl+Alt+Shift+W' },
      { name: 'Pin Paste', shortcut: 'F3' },
      { name: 'Record', shortcut: 'Ctrl+Shift+R' },
      { name: 'Record Pause', shortcut: 'Ctrl+Shift+P' },
      { name: 'Repeat Last Capture', shortcut: 'Shift+F1' },
      { name: 'Screenshot', shortcut: 'F1' },
      { name: 'Toggle Clipboard', shortcut: 'Alt+V' },
      { name: 'Toggle Search', shortcut: 'Alt+Space' },
      { name: 'Future Custom Action', shortcut: 'Ctrl+Alt+K' },
    ];

    const sorted = sortHotkeysByPriority(alphabeticalInput);
    expect(sorted.slice(0, 4).map((x) => x.name)).toEqual([
      'Screenshot',
      'Repeat Last Capture',
      'Toggle Clipboard',
      'Toggle Search',
    ]);

    const grouped = groupHotkeysByDomain(alphabeticalInput);
    expect(grouped.map((g) => g.id)).toEqual(['core', 'pin_ocr', 'recording', 'dialog', 'input']);
    expect(grouped[0].items.map((x) => x.name)).toEqual([
      'Screenshot',
      'Repeat Last Capture',
      'Toggle Clipboard',
      'Toggle Search',
    ]);
    expect(grouped[1].items.map((x) => x.name)).toEqual(['Pin Paste', 'OCR']);
    expect(grouped[2].items.map((x) => x.name)).toEqual([
      'Record',
      'Record Pause',
      'Mute System Audio',
      'Mute Microphone',
    ]);
    expect(grouped[3].items.map((x) => x.name)).toEqual([
      'Dialog Quick Jump',
      'Dialog Inline Search',
    ]);
    expect(grouped[4].items.map((x) => x.name)).toEqual([
      'Pause Gestures',
      'Future Custom Action',
    ]);
  });

  it('categorizes all WindowKit hotkeys and dynamic 0-9 layout hotkeys into window_manager with localized labels', () => {
    const windowKitInput = [
      { name: 'Pause Gestures', shortcut: 'Ctrl+Alt+Shift+W' },
      { name: 'WindowKit: Toggle Always on Top', shortcut: 'Ctrl+Win+T' },
      { name: 'WindowKit: Open Layout Editor', shortcut: 'Shift+Win+`' },
      { name: 'WindowKit: Open Quick Switch', shortcut: 'Alt+Tab' },
      { name: 'WindowKit: Minimize To Tray', shortcut: 'Ctrl+Win+Down' },
      { name: 'WindowKit: Restore All Tray', shortcut: 'Ctrl+Shift+Win+Down' },
      { name: 'WindowKit: Rescue Offscreen', shortcut: 'Ctrl+Win+G' },
      { name: 'WindowKit: Move Next Zone', shortcut: 'Alt+Win+Right' },
      { name: 'WindowKit: Move Prev Zone', shortcut: 'Alt+Win+Left' },
      { name: 'WindowKit: Restore From Zone', shortcut: 'Alt+Win+Down' },
      { name: 'WindowKit: Move Next Monitor', shortcut: 'Ctrl+Win+Right' },
      { name: 'WindowKit: Opacity Up', shortcut: 'Ctrl+Win+Up' },
      { name: 'WindowKit: Opacity Down', shortcut: 'Ctrl+Win+PageDown' },
      { name: 'WindowKit: Opacity Reset', shortcut: 'Ctrl+Win+0' },
      { name: 'WindowKit: Layout Hotkey 2', shortcut: 'Ctrl+Alt+Win+2' },
      { name: 'WindowKit: Layout Hotkey 1', shortcut: 'Ctrl+Alt+Win+1' },
    ];

    const grouped = groupHotkeysByDomain(windowKitInput);
    expect(grouped.map((g) => g.id)).toEqual(['window_manager', 'input']);
    expect(grouped[0].items.map((x) => x.name)).toEqual([
      'WindowKit: Open Layout Editor',
      'WindowKit: Open Quick Switch',
      'WindowKit: Move Next Zone',
      'WindowKit: Move Prev Zone',
      'WindowKit: Restore From Zone',
      'WindowKit: Move Next Monitor',
      'WindowKit: Rescue Offscreen',
      'WindowKit: Toggle Always on Top',
      'WindowKit: Minimize To Tray',
      'WindowKit: Restore All Tray',
      'WindowKit: Opacity Up',
      'WindowKit: Opacity Down',
      'WindowKit: Opacity Reset',
      'WindowKit: Layout Hotkey 1',
      'WindowKit: Layout Hotkey 2',
    ]);
    expect(grouped[1].items.map((x) => x.name)).toEqual(['Pause Gestures']);

    const mockZhT = ((key: string, opts?: Record<string, unknown>) => {
      const raw = readNestedKey(zhLocale as unknown as Record<string, unknown>, key);
      if (typeof raw !== 'string') return String(opts?.defaultValue ?? key);
      return raw.replace(/\{\{(\w+)\}\}/g, (_, k) => String(opts?.[k] ?? ''));
    }) as unknown as TFunction;

    for (const item of grouped[0].items) {
      const label = resolveHotkeyLabel(mockZhT, item.name);
      expect(label).not.toContain('WindowKit:');
      expect(label.trim().length).toBeGreaterThan(0);
    }
    expect(resolveHotkeyLabel(mockZhT, 'WindowKit: Layout Hotkey 1')).toBe('切换分屏布局 #1');
  });
});

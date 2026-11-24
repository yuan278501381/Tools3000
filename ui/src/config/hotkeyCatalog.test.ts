import { describe, expect, it } from 'vitest';
import {
  HOTKEY_DOMAIN_GROUPS,
  HOTKEY_LABEL_KEYS,
  groupHotkeysByDomain,
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
    expect(grouped.map((g) => g.id)).toEqual(['core', 'pin_ocr', 'recording', 'input']);
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
      'Pause Gestures',
      'Future Custom Action',
    ]);
  });
});

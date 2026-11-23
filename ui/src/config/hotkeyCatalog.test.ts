import { describe, expect, it } from 'vitest';
import { HOTKEY_LABEL_KEYS } from './hotkeyCatalog';
import zhLocale from '../i18n/locales/zh.json';
import enLocale from '../i18n/locales/en.json';

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
});

/* ─────────────────────────────────────────────────────────────────────────────
 * hotkeyCatalog.ts — 全局快捷键名称 <-> 多语言键单一事实源 (SSOT)
 *
 * 架构规范:
 *   1. C++ 后端通过 HotkeyManager::registerHotkey("...") 注册的每一条全局快捷键名称，
 *      必须在此处建立显式多语言键映射。
 *   2. ui/scripts/check-i18n.js 第 9 重「跨前后端契约门禁」会自动静态扫描 ../src/ 下
 *      全部 registerHotkey / rebindHotkey 字面量并强校验此表 100% 覆盖且指向真实存在的
 *      8 国语言键，从根源上杜绝“Toggle Clipboard”类漏译裸英文流入界面。
 * ───────────────────────────────────────────────────────────────────────────── */

import type { TFunction } from 'i18next';

export const HOTKEY_LABEL_KEYS: Readonly<Record<string, string>> = {
  Screenshot: 'onboarding.shortcutCapture',
  'Repeat Last Capture': 'capture.repeatLastCaptureShortcut',
  Record: 'onboarding.shortcutRecord',
  'Record Pause': 'recording.pauseShortcut',
  'Record Snapshot': 'general.shortcutRecordSnapshot',
  OCR: 'onboarding.shortcutOcr',
  'Pause Gestures': 'onboarding.shortcutGesturePause',
  'Toggle Search': 'search.title',
  'Toggle Clipboard': 'onboarding.shortcutClipboard',
  'Translate Selection': 'general.shortcutTranslateSelection',
  'Pin Toggle': 'general.shortcutPinToggle',
  'Pin Paste': 'general.shortcutPinPaste',
  'Pin Hide All': 'general.shortcutPinHideAll',
  'Pin Arrange': 'general.shortcutPinArrange',
  'Mute System Audio': 'general.shortcutMuteSystemAudio',
  'Mute Microphone': 'general.shortcutMuteMicrophone',
  // 历史配置兼容别名
  capture: 'onboarding.shortcutCapture',
  recording: 'onboarding.shortcutRecord',
  ocr: 'onboarding.shortcutOcr',
  gesturePause: 'onboarding.shortcutGesturePause',
};

export function resolveHotkeyLabel(t: TFunction, name: string): string {
  const key = HOTKEY_LABEL_KEYS[name];
  if (key) {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    return String(t(key as any, { defaultValue: name }));
  }
  return name;
}

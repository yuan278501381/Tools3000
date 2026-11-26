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
  'Dialog Quick Jump': 'dialog.quickJumpHotkeyLabel',
  'Dialog Inline Search': 'dialog.inlineSearchHotkeyLabel',
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

export type HotkeyDomainGroupId = 'core' | 'pin_ocr' | 'recording' | 'dialog' | 'input';

export interface HotkeyDomainGroupSpec {
  readonly id: HotkeyDomainGroupId;
  readonly titleKey: string;
  readonly defaultTitle: string;
  readonly members: readonly string[];
}

/**
 * 全局快捷键四域信息架构与高频优先级单一事实源（SSOT）：
 * 1. 核心效率入口（截图、复用区域、剪贴板、文件搜索）置顶；
 * 2. 贴图与识别翻译（贴图四联、离线 OCR、划词翻译）次之；
 * 3. 高清录屏与场控（全局录屏在前，录制期暂停/快照/静音在后）；
 * 4. 输入与手势控制（手势暂停及后续扩展项兜底）。
 */
export const HOTKEY_DOMAIN_GROUPS: readonly HotkeyDomainGroupSpec[] = [
  {
    id: 'core',
    titleKey: 'general.hotkeyGroupCore',
    defaultTitle: 'Core Efficiency Entry',
    members: [
      'Screenshot',
      'capture',
      'Repeat Last Capture',
      'Toggle Clipboard',
      'Toggle Search',
    ],
  },
  {
    id: 'pin_ocr',
    titleKey: 'general.hotkeyGroupPinOcr',
    defaultTitle: 'Pin, OCR & Translation',
    members: [
      'Pin Paste',
      'Pin Toggle',
      'Pin Hide All',
      'Pin Arrange',
      'OCR',
      'ocr',
      'Translate Selection',
    ],
  },
  {
    id: 'recording',
    titleKey: 'general.hotkeyGroupRecording',
    defaultTitle: 'Screen Recording & Audio Control',
    members: [
      'Record',
      'recording',
      'Record Pause',
      'Record Snapshot',
      'Mute System Audio',
      'Mute Microphone',
    ],
  },
  {
    id: 'dialog',
    titleKey: 'nav.dialog_enhancer',
    defaultTitle: 'File Dialog Enhancer',
    members: [
      'Dialog Quick Jump',
      'Dialog Inline Search',
    ],
  },
  {
    id: 'input',
    titleKey: 'general.hotkeyGroupInput',
    defaultTitle: 'Input & Gesture Control',
    members: [
      'Pause Gestures',
      'gesturePause',
    ],
  },
] as const;

const HOTKEY_META_MAP: ReadonlyMap<string, { groupId: HotkeyDomainGroupId; rank: number }> = (() => {
  const map = new Map<string, { groupId: HotkeyDomainGroupId; rank: number }>();
  let globalRank = 0;
  for (const group of HOTKEY_DOMAIN_GROUPS) {
    for (const member of group.members) {
      if (!map.has(member)) {
        map.set(member, { groupId: group.id, rank: globalRank++ });
      }
    }
  }
  return map;
})();

export function getHotkeyPriorityRank(name: string): number {
  return HOTKEY_META_MAP.get(name)?.rank ?? 10_000;
}

export function sortHotkeysByPriority<T extends { name: string }>(entries: readonly T[]): T[] {
  return [...entries].sort((a, b) => {
    const ra = getHotkeyPriorityRank(a.name);
    const rb = getHotkeyPriorityRank(b.name);
    if (ra !== rb) return ra - rb;
    return a.name.localeCompare(b.name);
  });
}

export interface GroupedHotkeySection<T extends { name: string }> {
  readonly id: HotkeyDomainGroupId;
  readonly titleKey: string;
  readonly defaultTitle: string;
  readonly items: T[];
}

export function groupHotkeysByDomain<T extends { name: string }>(
  entries: readonly T[],
): GroupedHotkeySection<T>[] {
  const buckets = new Map<HotkeyDomainGroupId, T[]>();
  for (const group of HOTKEY_DOMAIN_GROUPS) {
    buckets.set(group.id, []);
  }

  const sorted = sortHotkeysByPriority(entries);
  for (const item of sorted) {
    const groupId = HOTKEY_META_MAP.get(item.name)?.groupId ?? 'input';
    buckets.get(groupId)?.push(item);
  }

  const sections: GroupedHotkeySection<T>[] = [];
  for (const group of HOTKEY_DOMAIN_GROUPS) {
    const items = buckets.get(group.id) ?? [];
    if (items.length > 0) {
      sections.push({
        id: group.id,
        titleKey: group.titleKey,
        defaultTitle: group.defaultTitle,
        items,
      });
    }
  }
  return sections;
}

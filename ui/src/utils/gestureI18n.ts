// ─────────────────────────────────────────────────────────────────────────────
// gestureI18n.ts — 前端手势预设动作名称与描述实时国际化映射中枢 (单一事实源)
// ─────────────────────────────────────────────────────────────────────────────

import type { TFunction } from 'i18next';
import zhLocale from '../i18n/locales/zh.json';
import enLocale from '../i18n/locales/en.json';

interface ActionItem {
  name: string;
  desc?: string;
}

const zhActions = (zhLocale as { gesture?: { actions?: Record<string, ActionItem> } })?.gesture?.actions || {};
const enActions = (enLocale as { gesture?: { actions?: Record<string, ActionItem> } })?.gesture?.actions || {};

const nameToKeyMap = new Map<string, string>();
const descToKeyMap = new Map<string, string>();

for (const [key, valZh] of Object.entries(zhActions)) {
  const valEn = enActions[key];
  if (valZh.name) nameToKeyMap.set(valZh.name.trim().toLowerCase(), key);
  if (valEn?.name) nameToKeyMap.set(valEn.name.trim().toLowerCase(), key);
  if (valZh.desc) descToKeyMap.set(valZh.desc.trim().toLowerCase(), key);
  if (valEn?.desc) descToKeyMap.set(valEn.desc.trim().toLowerCase(), key);
}

/**
 * 获取手势动作国际化名称 (若为预设动作随当前语言实时跟随，用户自定义名称保持原样)
 */
export function getLocalizedGestureName(rawName: string, t: TFunction): string {
  if (!rawName) return '';
  const key = nameToKeyMap.get(rawName.trim().toLowerCase());
  if (key) {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    return t(`gesture.actions.${key}.name` as any, { defaultValue: enActions[key]?.name || rawName });
  }
  return rawName;
}

/**
 * 获取手势动作国际化描述 (若为预设描述随当前语言实时跟随，用户自定义描述保持原样)
 */
export function getLocalizedGestureDesc(rawDesc: string | undefined, t: TFunction): string | undefined {
  if (!rawDesc) return undefined;
  const key = descToKeyMap.get(rawDesc.trim().toLowerCase());
  if (key) {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    return t(`gesture.actions.${key}.desc` as any, { defaultValue: enActions[key]?.desc || rawDesc });
  }
  return rawDesc;
}

import type { NavId } from './routes';

export type SearchPageSubTab = 'settings' | 'syntax' | 'regex' | 'status';

export interface DeepSettingEntry {
  id: string;
  navId: NavId;
  labelKey: string;
  descKey?: string;
  tabId?: SearchPageSubTab;
  keywords: string;
}

export interface DeepNavigationPayload {
  navId: NavId;
  tabId?: SearchPageSubTab;
  targetText?: string;
  timestamp: number;
}

let pendingDeepNavigation: DeepNavigationPayload | null = null;

export const emitDeepNavigation = (payload: Omit<DeepNavigationPayload, 'timestamp'>): void => {
  const stamped: DeepNavigationPayload = {
    ...payload,
    timestamp: Date.now(),
  };
  pendingDeepNavigation = stamped;
  if (typeof window !== 'undefined') {
    window.dispatchEvent(new CustomEvent<DeepNavigationPayload>('tools3000:navigate-deep', { detail: stamped }));
  }
};

export const consumePendingDeepNavigation = (navId: NavId, maxAgeMs = 4000): DeepNavigationPayload | null => {
  if (!pendingDeepNavigation || pendingDeepNavigation.navId !== navId) {
    return null;
  }
  const current = pendingDeepNavigation;
  pendingDeepNavigation = null;
  if (Date.now() - current.timestamp > maxAgeMs) {
    return null;
  }
  return current;
};

/**
 * 细粒度全局设置项与子页签全文检索索引（单一事实源）
 * 所有 labelKey / descKey 严格引用字典真实键值，0 裸中文硬编码。
 */
export const DEEP_SETTINGS_INDEX: ReadonlyArray<DeepSettingEntry> = [
  // General
  { id: 'gen-autostart', navId: 'general', labelKey: 'general.autoStart', descKey: 'general.autoStartDesc', keywords: 'boot startup launch autostart kaiji' },
  { id: 'gen-admin', navId: 'general', labelKey: 'general.runAsAdmin', descKey: 'general.runAsAdminDesc', keywords: 'uac administrator privilege elevated guanliyuan' },
  { id: 'gen-tray', navId: 'general', labelKey: 'general.minimizeToTray', descKey: 'general.minimizeToTrayDesc', keywords: 'tray close background tuopan' },
  { id: 'gen-memory', navId: 'general', labelKey: 'general.autoReleaseSettingsMemory', descKey: 'general.autoReleaseSettingsMemoryDesc', keywords: 'ram memory webview trim neicun' },
  { id: 'gen-fullscreen', navId: 'general', labelKey: 'general.fullscreenMode', descKey: 'general.fullscreenModeDesc', keywords: 'game fullscreen dnd silence quanping youxi' },
  { id: 'gen-theme', navId: 'general', labelKey: 'general.theme', descKey: 'general.themeDesc', keywords: 'dark light appearance mode zhuti shense qianse' },
  { id: 'gen-accent', navId: 'general', labelKey: 'general.accentColor', descKey: 'general.accentColorDesc', keywords: 'brand color violet blue cyan mint coral zhutise' },
  { id: 'gen-lang', navId: 'general', labelKey: 'general.language', descKey: 'general.languageDesc', keywords: 'english chinese locale i18n yuyan' },
  { id: 'gen-font', navId: 'general', labelKey: 'general.fontFamily', descKey: 'general.fontFamilyDesc', keywords: 'font typography noto harmony yahei ziti' },
  { id: 'gen-hotkeys', navId: 'general', labelKey: 'general.keyboardShortcuts', descKey: 'general.keyboardShortcutsDesc', keywords: 'shortcut keybind conflict kuaijiejian rejian' },
  { id: 'gen-portable', navId: 'general', labelKey: 'general.portableMode', descKey: 'general.portableModeDesc', keywords: 'portable appdata folder bianxie' },
  { id: 'gen-logs', navId: 'general', labelKey: 'general.exportLogs', descKey: 'general.exportLogsDesc', keywords: 'log diagnostic debug export rizhi' },
  { id: 'gen-backup', navId: 'general', labelKey: 'general.exportConfig', descKey: 'general.exportConfigDesc', keywords: 'backup export import reset config beifen daochu' },

  // Plugins
  { id: 'plg-market', navId: 'plugins', labelKey: 'plugins.tabMarketplace', descKey: 'plugins.marketplaceDesc', keywords: 'marketplace store install extension chajian shichang' },

  // Search & Sub-tabs
  { id: 'srch-hotkey', navId: 'search', tabId: 'settings', labelKey: 'searchPage.toggleHotkey', descKey: 'searchPage.toggleHotkeyDesc', keywords: 'alt space summon search shortcut' },
  { id: 'srch-pinyin', navId: 'search', tabId: 'settings', labelKey: 'searchPage.pinyinTitle', descKey: 'searchPage.pinyinDesc', keywords: 'pinyin initials fuzzy chinese' },
  { id: 'srch-content', navId: 'search', tabId: 'settings', labelKey: 'searchPage.contentCategoriesGroup', descKey: 'searchPage.contentCategoriesGroupDesc', keywords: 'content fulltext office code cad' },
  { id: 'srch-syntax', navId: 'search', tabId: 'syntax', labelKey: 'searchPage.tabSyntax', descKey: 'searchPage.modifiersTitle', keywords: 'syntax manual wildcard operator ext folder' },
  { id: 'srch-regex', navId: 'search', tabId: 'regex', labelKey: 'searchPage.tabRegex', descKey: 'searchPage.regexHint', keywords: 'regex regular expression pattern zhengze' },
  { id: 'srch-status', navId: 'search', tabId: 'status', labelKey: 'searchPage.tabStatus', descKey: 'searchPage.serviceMonitor', keywords: 'ntfs mft usn pipe service index daemon suoyin' },

  // Gesture & HotCorner
  { id: 'gst-trigger', navId: 'gesture', labelKey: 'gesture.triggerButton', descKey: 'gesture.triggerButtonDesc', keywords: 'right middle button mouse trigger shoushi' },
  { id: 'gst-trail', navId: 'gesture', labelKey: 'gesture.showTrail', descKey: 'gesture.showTrailDesc', keywords: 'trail color width outline guiji' },
  { id: 'gst-scope', navId: 'gesture', labelKey: 'gesture.targetConfigTitle', descKey: 'gesture.scopeRules', keywords: 'scope app blacklist whitelist rule yingyong' },
  { id: 'gst-websearch', navId: 'gesture', labelKey: 'gesture.webSearchEngine', descKey: 'gesture.webSearchEngineDesc', keywords: 'web search engine bing google baidu duckduckgo wheel edge' },
  { id: 'hc-corners', navId: 'hotcorner', labelKey: 'hotcorner.corners', descKey: 'hotcorner.enabledDesc', keywords: 'top bottom left right corner delay chufajiao' },

  // Capture, Recording, OCR, History
  { id: 'cap-shortcut', navId: 'capture', labelKey: 'capture.shortcut', descKey: 'capture.shortcutDesc', keywords: 'f1 screenshot capture hotkey jietu' },
  { id: 'cap-savedir', navId: 'capture', labelKey: 'capture.saveDir', descKey: 'capture.saveDirDesc', keywords: 'save folder directory path png jpg webp baocun' },
  { id: 'cap-beauty', navId: 'capture', labelKey: 'capture.beautyShellTitle', descKey: 'capture.beautyShellDefaultDesc', keywords: 'beauty gradient shadow rounded padding meihua' },
  { id: 'cap-pingroup', navId: 'capture', labelKey: 'capture.pinGroupsSectionTitle', descKey: 'capture.pinGroupsCardSubtitle', keywords: 'pin group desktop color tietu fenzu' },
  { id: 'rec-video', navId: 'capture', labelKey: 'recording.title', descKey: 'recording.formatDesc', keywords: 'record mp4 gif webm fps audio microphone luping' },
  { id: 'ocr-lang', navId: 'ocr', labelKey: 'ocr.language', descKey: 'ocr.languageDesc', keywords: 'ocr text extract japanese korean english shibie' },
  { id: 'his-recent', navId: 'history', labelKey: 'history.title', descKey: 'history.subtitle', keywords: 'history recent screenshot pin lishi' },

  // Keycast, Spotlight, Dialog Enhancer, Remote Boost, Stats
  { id: 'kc-filter', navId: 'keycast', labelKey: 'keycast.filterSection', descKey: 'keycast.filterSmartShortcutsDesc', keywords: 'keycast keystroke osd modifier jianpan touying' },
  { id: 'spt-focus', navId: 'spotlight', labelKey: 'spotlight.animStyle', descKey: 'spotlight.animStyleDesc', keywords: 'spotlight double ctrl shake cursor juguangdeng' },
  { id: 'spt-ripple', navId: 'spotlight', labelKey: 'spotlight.clickRipple', descKey: 'spotlight.clickRippleDesc', keywords: 'click ripple trail sonar bowen' },
  { id: 'dlg-memory', navId: 'dialog_enhancer', labelKey: 'dialog.perAppMemory', descKey: 'dialog.perAppMemoryDesc', keywords: 'dialog open save explorer favorite folder duihuakuang' },
  { id: 'rmt-tunnel', navId: 'remote_boost', labelKey: 'remoteBoost.hotkeyTunnelTitle', descKey: 'remoteBoost.hotkeyTunnelDesc', keywords: 'remote rdp mstsc tododesk sunlogin win alt tab yuancheng' },
  { id: 'rmt-flush', navId: 'remote_boost', labelKey: 'remoteBoost.emergencyFlushTitle', descKey: 'remoteBoost.emergencyFlushDesc', keywords: 'stuck modifier keyup flush release kajian' },
  { id: 'rmt-ime', navId: 'remote_boost', labelKey: 'remoteBoost.imeSanitizerTitle', descKey: 'remoteBoost.imeSanitizerDesc', keywords: 'ime english input method shurufa' },
  { id: 'stt-heatmap', navId: 'stats', labelKey: 'stats.heatmapTitle', descKey: 'stats.dailyTitle', keywords: 'heatmap keystrokes clicks distance relitu tongji' },
];

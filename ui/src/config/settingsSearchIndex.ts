import type { NavId } from './routes';

export type SearchPageSubTab = 'settings' | 'syntax' | 'regex' | 'status' | 'external';
export type CapturePageSubTab = 'screenshot' | 'pin_region' | 'recording' | 'studio';
export type GesturePageSubTab = 'rules' | 'appearance' | 'algorithm';
export type OcrPageSubTab = 'recognize' | 'translate' | 'models';

export type AnyPageSubTab =
  | SearchPageSubTab
  | CapturePageSubTab
  | GesturePageSubTab
  | OcrPageSubTab
  | (string & {});

export interface DeepSettingEntry {
  id: string;
  navId: NavId;
  labelKey: string;
  descKey?: string;
  tabId?: AnyPageSubTab;
  keywords: string;
}

export interface DeepNavigationPayload {
  navId: NavId;
  tabId?: AnyPageSubTab;
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
  { id: 'gen-hotkeys', navId: 'general', labelKey: 'general.keyboardShortcuts', descKey: 'general.keyboardShortcutsDesc', keywords: 'shortcut keybind conflict kuaijiejian rejian' },
  { id: 'gen-portable', navId: 'general', labelKey: 'general.portableMode', descKey: 'general.portableModeDesc', keywords: 'portable appdata folder bianxie' },
  { id: 'gen-logs', navId: 'general', labelKey: 'general.exportLogs', descKey: 'general.exportLogsDesc', keywords: 'log diagnostic debug export rizhi' },
  { id: 'gen-backup', navId: 'general', labelKey: 'general.exportConfig', descKey: 'general.exportConfigDesc', keywords: 'backup export import reset config beifen daochu' },

  // Search & Sub-tabs
  { id: 'srch-hotkey', navId: 'search', tabId: 'settings', labelKey: 'searchPage.toggleHotkey', descKey: 'searchPage.toggleHotkeyDesc', keywords: 'alt space summon search shortcut' },
  { id: 'srch-hide-blur', navId: 'search', tabId: 'settings', labelKey: 'searchPage.hideOnBlurTitle', descKey: 'searchPage.hideOnBlurDesc', keywords: 'blur hide close outside focus listary utools shijiao zidong yincang guanbi' },
  { id: 'srch-hide-open', navId: 'search', tabId: 'settings', labelKey: 'searchPage.hideOnOpenTitle', descKey: 'searchPage.hideOnOpenDesc', keywords: 'open launch enter double click hide close dakai zidong yincang guanbi' },
  { id: 'srch-clear-show', navId: 'search', tabId: 'settings', labelKey: 'searchPage.clearQueryOnShowTitle', descKey: 'searchPage.clearQueryOnShowDesc', keywords: 'clear empty reset query reopen summon qingkong shangci sousuoci' },
  { id: 'srch-pinyin', navId: 'search', tabId: 'settings', labelKey: 'searchPage.pinyinTitle', descKey: 'searchPage.pinyinDesc', keywords: 'pinyin initials fuzzy chinese' },
  { id: 'srch-excluded-paths', navId: 'search', tabId: 'settings', labelKey: 'searchPage.groupExclusions', descKey: 'search.customExcludedPathsDesc', keywords: 'exclude folder directory path hide block privacy shield paichu wenjianjia pingbi yinsi' },
  { id: 'srch-syntax', navId: 'search', tabId: 'syntax', labelKey: 'searchPage.tabSyntax', descKey: 'searchPage.modifiersTitle', keywords: 'syntax manual wildcard operator ext folder' },
  { id: 'srch-regex', navId: 'search', tabId: 'regex', labelKey: 'searchPage.tabRegex', descKey: 'searchPage.regexHint', keywords: 'regex regular expression pattern zhengze' },
  { id: 'srch-status', navId: 'search', tabId: 'status', labelKey: 'searchPage.tabStatus', descKey: 'searchPage.serviceMonitor', keywords: 'ntfs mft usn pipe service index daemon suoyin' },
  { id: 'srch-external', navId: 'search', tabId: 'external', labelKey: 'searchExternal.enableTitle', descKey: 'searchExternal.enableDesc', keywords: 'cli mcp ai agent cursor claude json rpc terminal minglinhang' },

  // Clipboard
  { id: 'clip-history', navId: 'clipboard', labelKey: 'clipboardPage.enableTitle', descKey: 'clipboardPage.enableDesc', keywords: 'clipboard copy paste history pin ditto alt v rust jiantieban fuzhi zhantie' },
  { id: 'clip-window-behavior', navId: 'clipboard', labelKey: 'clipboardPage.hideOnBlurTitle', descKey: 'clipboardPage.hideOnPasteDesc', keywords: 'clipboard blur hide paste drag single click keep open copyq shijiao zhantie danji tuozhuai zidong yincang' },

  // Text Expander
  { id: 'te-enable', navId: 'text_expander', labelKey: 'textExpander.heroTitle', descKey: 'textExpander.heroDesc', keywords: 'text expander snippet espanso beeftext macro template shortcut date time cursor duanyu wenben kuozhan tihuan' },
  { id: 'te-rules', navId: 'text_expander', labelKey: 'textExpander.rulesGroup', descKey: 'textExpander.sandboxDisabledHint', keywords: 'snippet rules trigger immediate delimiter json import export guize chufaci' },
  { id: 'te-privacy', navId: 'text_expander', labelKey: 'textExpander.privacyGroup', descKey: 'textExpander.autoExcludeDesc', keywords: 'password keepass 1password bitwarden restore clipboard exclude process yinsi mima' },

  // Gesture & HotCorner
  { id: 'gst-trigger', navId: 'gesture', tabId: 'rules', labelKey: 'gesture.triggerButton', descKey: 'gesture.triggerButtonDesc', keywords: 'right middle button mouse trigger shoushi' },
  { id: 'gst-trail', navId: 'gesture', tabId: 'appearance', labelKey: 'gesture.showTrail', descKey: 'gesture.showTrailDesc', keywords: 'trail color width outline fade hint text brightness delete backspace guiji jianyin tishi liangdu shanchu' },
  { id: 'gst-scope', navId: 'gesture', tabId: 'rules', labelKey: 'gesture.targetConfigTitle', descKey: 'gesture.scopeRules', keywords: 'scope app blacklist whitelist rule yingyong' },
  { id: 'gst-websearch', navId: 'gesture', tabId: 'rules', labelKey: 'gesture.webSearchEngine', descKey: 'gesture.webSearchEngineDesc', keywords: 'web search engine bing google baidu duckduckgo wheel edge' },
  { id: 'hc-corners', navId: 'hotcorner', labelKey: 'hotcorner.corners', descKey: 'hotcorner.enabledDesc', keywords: 'top bottom left right corner delay chufajiao' },

  // Capture, Recording, OCR, History
  { id: 'cap-shortcut', navId: 'capture', tabId: 'screenshot', labelKey: 'capture.shortcut', descKey: 'capture.shortcutDesc', keywords: 'f1 screenshot capture hotkey jietu' },
  { id: 'cap-savedir', navId: 'capture', tabId: 'screenshot', labelKey: 'capture.saveDir', descKey: 'capture.saveDirDesc', keywords: 'save folder directory path png jpg webp baocun' },
  { id: 'cap-beauty', navId: 'capture', tabId: 'screenshot', labelKey: 'capture.beautyShellTitle', descKey: 'capture.beautyShellDefaultDesc', keywords: 'beauty gradient shadow rounded padding meihua' },
  { id: 'cap-pin-ocr', navId: 'capture', tabId: 'pin_region', labelKey: 'capture.pinAutoOcrTitle', descKey: 'capture.pinAutoOcrDesc', keywords: 'pin auto ocr live text memory tietu zidong shibie wenzi' },
  { id: 'cap-pin-mouse', navId: 'capture', tabId: 'pin_region', labelKey: 'capture.pinDoubleClickActionTitle', descKey: 'capture.pinMiddleClickActionDesc', keywords: 'pin double click middle click close fold edit snipaste shuangji zhongjian guanbi zhedie tietu' },
  { id: 'cap-pingroup', navId: 'capture', tabId: 'pin_region', labelKey: 'capture.pinGroupsSectionTitle', descKey: 'capture.pinGroupsCardSubtitle', keywords: 'pin group desktop color tietu fenzu' },
  { id: 'rec-video', navId: 'capture', tabId: 'recording', labelKey: 'recording.title', descKey: 'recording.formatDesc', keywords: 'record mp4 gif webm fps audio microphone luping' },
  { id: 'rec-autozoom', navId: 'capture', tabId: 'studio', labelKey: 'recording.studioAutoZoom', descKey: 'recording.studioAutoZoomDesc', keywords: 'studio auto zoom camera motion waveform yunjing zidongyunjing boxing' },
  { id: 'ocr-lang', navId: 'ocr', tabId: 'recognize', labelKey: 'ocr.language', descKey: 'ocr.languageDesc', keywords: 'ocr text extract japanese korean french german vietnamese arabic thai english shibie fawen yuenanyu alaboyu taiyu' },
  { id: 'ocr-translate', navId: 'ocr', tabId: 'translate', labelKey: 'translate.groupTitle', descKey: 'translate.providerDesc', keywords: 'translate in-place edge bing google openai deepseek ollama deepl fanyi yuanwei' },
  { id: 'ocr-models', navId: 'ocr', tabId: 'models', labelKey: 'ocr.tabModels', descKey: 'ocr.tabModelsDesc', keywords: 'ppocr ppocrv6 model offline download pack arabic thai korean cyrillic moxing xiazai' },
  { id: 'his-recent', navId: 'history', labelKey: 'history.title', descKey: 'history.subtitle', keywords: 'history recent screenshot pin lishi' },

  // Keycast, Spotlight, Dialog Enhancer, Remote Boost, Stats
  { id: 'kc-filter', navId: 'keycast', labelKey: 'keycast.filterSection', descKey: 'keycast.filterSmartShortcutsDesc', keywords: 'keycast keystroke osd modifier jianpan touying' },
  { id: 'spt-focus', navId: 'spotlight', labelKey: 'spotlight.animStyle', descKey: 'spotlight.animStyleDesc', keywords: 'spotlight double ctrl shake cursor juguangdeng' },
  { id: 'spt-ripple', navId: 'spotlight', labelKey: 'spotlight.clickRipple', descKey: 'spotlight.clickRippleDesc', keywords: 'click ripple trail sonar bowen' },
  { id: 'dlg-memory', navId: 'dialog_enhancer', labelKey: 'dialog.perAppMemory', descKey: 'dialog.perAppMemoryDesc', keywords: 'dialog open save explorer favorite folder duihuakuang' },
  { id: 'dlg-quick-jump', navId: 'dialog_enhancer', labelKey: 'dialog.quickJumpHotkeyLabel', descKey: 'dialog.quickJumpHotkeyDesc', keywords: 'ctrl+g listary quick switch jump explorer total commander opus xyplorer q-dir files qt jetbrains tiaozhuan mulu' },
  { id: 'dlg-inline-search', navId: 'dialog_enhancer', labelKey: 'dialog.inlineSearchHotkeyLabel', descKey: 'dialog.inlineSearchHotkeyDesc', keywords: 'ctrl+shift+g dialog inline search jump duihuakuang sousuo' },
  { id: 'rmt-tunnel', navId: 'remote_boost', labelKey: 'remoteBoost.hotkeyTunnelTitle', descKey: 'remoteBoost.hotkeyTunnelDesc', keywords: 'remote rdp mstsc tododesk sunlogin win alt tab yuancheng' },
  { id: 'rmt-flush', navId: 'remote_boost', labelKey: 'remoteBoost.emergencyFlushTitle', descKey: 'remoteBoost.emergencyFlushDesc', keywords: 'stuck modifier keyup flush release kajian' },
  { id: 'rmt-ime', navId: 'remote_boost', labelKey: 'remoteBoost.imeSanitizerTitle', descKey: 'remoteBoost.imeSanitizerDesc', keywords: 'ime english input method shurufa' },
  { id: 'pv-assoc', navId: 'picasa_viewer', labelKey: 'picasaViewer.associateAllTitle', descKey: 'picasaViewer.heroDesc', keywords: 'picasa photo image picture viewer rust association jpg png webp kantu tupian' },
  { id: 'pv-custom', navId: 'picasa_viewer', labelKey: 'picasaViewer.customFormatsTitle', descKey: 'picasaViewer.customFormatsDesc', keywords: 'picasa format config default apps extension raw heic avif geshi guanlian' },
  // File Locksmith
  { id: 'fl-menu', navId: 'file_locksmith', labelKey: 'general.shellMenuFileLocksmith', descKey: 'general.shellMenuFileLocksmithDesc', keywords: 'file locksmith unlock lock handle process context menu explorer shift wenjian zhanyong jiesuo' },
  { id: 'fl-inspect', navId: 'file_locksmith', labelKey: 'fileLocksmith.instantInspectGroup', descKey: 'fileLocksmith.instantInspectDesc', keywords: 'inspect locked file folder process kill unlock jiance zhanyong' },
  { id: 'stt-heatmap', navId: 'stats', labelKey: 'stats.heatmapTitle', descKey: 'stats.dailyTitle', keywords: 'heatmap keystrokes clicks distance relitu tongji' },
  { id: 'perf-lifecycle', navId: 'performance', labelKey: 'performancePage.lifecycleGroup', descKey: 'performancePage.lifecycleSubtitle', keywords: 'memory ram trim suspend destroy lifecycle idle latency xingneng neicun huishou' },
];

/**
 * Tools3000 前端 8 语种单一事实源 (SSOT)
 * 默认 "auto" 严格跟随操作系统显示语言（优先采用 C++ Win32 GetUserDefaultUILanguage
 * 探测的 systemLanguage，缺失时平滑回退至 navigator.language），并支持用户随时手动切换。
 */

export type AppLanguageCode =
  | 'zh-CN'
  | 'zh-TW'
  | 'en-US'
  | 'ja-JP'
  | 'ko-KR'
  | 'ru-RU'
  | 'pt-BR'
  | 'es-ES';

export type LanguagePreference = 'auto' | AppLanguageCode;

export type I18nBundleCode =
  | 'zh'
  | 'zh-TW'
  | 'en'
  | 'ja'
  | 'ko'
  | 'ru'
  | 'pt-BR'
  | 'es';

export interface LanguageOptionMeta {
  code: AppLanguageCode;
  bundle: I18nBundleCode;
  htmlLang: AppLanguageCode;
  badge: string;
  /** 始终展示原生母语名，确保误切外语后可一键找回母语 */
  nativeName: string;
  labelKey: string;
}

export const SUPPORTED_LANGUAGES: readonly LanguageOptionMeta[] = [
  {
    code: 'zh-CN',
    bundle: 'zh',
    htmlLang: 'zh-CN',
    badge: 'ZH',
    nativeName: '\u7B80\u4F53\u4E2D\u6587',
    labelKey: 'general.langZh',
  },
  {
    code: 'zh-TW',
    bundle: 'zh-TW',
    htmlLang: 'zh-TW',
    badge: 'TW',
    nativeName: '\u7E41\u9AD4\u4E2D\u6587',
    labelKey: 'general.langZhTw',
  },
  {
    code: 'en-US',
    bundle: 'en',
    htmlLang: 'en-US',
    badge: 'EN',
    nativeName: 'English',
    labelKey: 'general.langEn',
  },
  {
    code: 'ja-JP',
    bundle: 'ja',
    htmlLang: 'ja-JP',
    badge: 'JA',
    nativeName: '\u65E5\u672C\u8A9E',
    labelKey: 'general.langJa',
  },
  {
    code: 'ko-KR',
    bundle: 'ko',
    htmlLang: 'ko-KR',
    badge: 'KO',
    nativeName: '\uD55C\uAD6D\uC5B4',
    labelKey: 'general.langKo',
  },
  {
    code: 'ru-RU',
    bundle: 'ru',
    htmlLang: 'ru-RU',
    badge: 'RU',
    nativeName: '\u0420\u0443\u0441\u0441\u043A\u0438\u0439',
    labelKey: 'general.langRu',
  },
  {
    code: 'pt-BR',
    bundle: 'pt-BR',
    htmlLang: 'pt-BR',
    badge: 'PT',
    nativeName: 'Portugu\u00EAs (Brasil)',
    labelKey: 'general.langPtBr',
  },
  {
    code: 'es-ES',
    bundle: 'es',
    htmlLang: 'es-ES',
    badge: 'ES',
    nativeName: 'Espa\u00F1ol',
    labelKey: 'general.langEs',
  },
] as const;

let cachedOsSystemLanguage: AppLanguageCode | null = null;

export function setDetectedSystemLanguage(raw?: unknown): void {
  if (typeof raw !== 'string' || !raw.trim()) return;
  const resolved = mapTagToAppLanguage(raw);
  cachedOsSystemLanguage = resolved;
  try {
    localStorage.setItem('tools3000:system-language', resolved);
  } catch {
    // 忽略受限环境存储异常
  }
}

export function getDetectedSystemLanguage(): AppLanguageCode {
  if (cachedOsSystemLanguage) return cachedOsSystemLanguage;
  try {
    const stored = localStorage.getItem('tools3000:system-language');
    if (stored) {
      return mapTagToAppLanguage(stored);
    }
  } catch {
    // ignore
  }
  const nav = typeof navigator !== 'undefined' ? navigator.language : 'en-US';
  return mapTagToAppLanguage(nav);
}

export function mapTagToAppLanguage(raw?: string | null): AppLanguageCode {
  const lower = (raw || '').trim().toLowerCase().replace(/_/g, '-');
  if (lower.startsWith('zh')) {
    if (
      lower.includes('tw') ||
      lower.includes('hk') ||
      lower.includes('mo') ||
      lower.includes('hant')
    ) {
      return 'zh-TW';
    }
    return 'zh-CN';
  }
  if (lower.startsWith('ja')) return 'ja-JP';
  if (lower.startsWith('ko')) return 'ko-KR';
  if (lower.startsWith('ru')) return 'ru-RU';
  if (lower.startsWith('pt')) return 'pt-BR';
  if (lower.startsWith('es')) return 'es-ES';
  return 'en-US';
}

export function normalizeLanguagePreference(raw?: unknown): LanguagePreference {
  if (typeof raw !== 'string') return 'auto';
  const lower = raw.trim().toLowerCase().replace(/_/g, '-');
  if (!lower || lower === 'auto' || lower === 'system') return 'auto';
  return mapTagToAppLanguage(lower);
}

export function findLanguageMeta(codeOrBundle?: string | null): LanguageOptionMeta {
  const appCode = mapTagToAppLanguage(codeOrBundle);
  return SUPPORTED_LANGUAGES.find((item) => item.code === appCode) ?? SUPPORTED_LANGUAGES[2];
}

export function resolveI18nBundle(
  preference?: unknown,
  explicitSystemLang?: unknown,
): I18nBundleCode {
  if (typeof explicitSystemLang === 'string' && explicitSystemLang) {
    setDetectedSystemLanguage(explicitSystemLang);
  }
  const pref = normalizeLanguagePreference(preference);
  const effectiveCode = pref === 'auto' ? getDetectedSystemLanguage() : pref;
  return findLanguageMeta(effectiveCode).bundle;
}

export function applyHtmlLanguageAttribute(bundleOrCode?: string | null): void {
  if (typeof document === 'undefined') return;
  const meta = findLanguageMeta(bundleOrCode);
  document.documentElement.setAttribute('lang', meta.htmlLang);
}

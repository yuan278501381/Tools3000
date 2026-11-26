import i18n, { type BackendModule } from 'i18next';
import { initReactI18next } from 'react-i18next';
import LanguageDetector from 'i18next-browser-languagedetector';
import { applyHtmlLanguageAttribute, resolveI18nBundle, type I18nBundleCode } from './languages';

type LocaleResource = Record<string, unknown>;

// 8 语种全部拆分为独立按需加载 chunk：首屏仅读取当前有效单一语言包（砍掉 ~1.65 MB 冗余词典 I/O 与 V8 解析开销），
// 彻底加速 Win10 LTSC 机械硬盘首次冷启动，同时支持全 8 国语言随时毫秒级热切换。
const LOCALE_LOADERS: Record<I18nBundleCode, () => Promise<{ default: LocaleResource }>> = {
  en: () => import('./locales/en.json'),
  zh: () => import('./locales/zh.json'),
  'zh-TW': () => import('./locales/zh-TW.json'),
  ja: () => import('./locales/ja.json'),
  ko: () => import('./locales/ko.json'),
  ru: () => import('./locales/ru.json'),
  'pt-BR': () => import('./locales/pt-BR.json'),
  es: () => import('./locales/es.json'),
};

const localeLoadCache = new Map<I18nBundleCode, Promise<LocaleResource>>();

function loadLocaleBundle(rawLanguage: string): Promise<LocaleResource> {
  const bundle = resolveI18nBundle(rawLanguage);
  const cached = localeLoadCache.get(bundle);
  if (cached) return cached;
  const loader = LOCALE_LOADERS[bundle] ?? LOCALE_LOADERS.en;
  const promise = loader()
    .then((mod) => mod.default)
    .catch((err) => {
      localeLoadCache.delete(bundle);
      throw err;
    });
  localeLoadCache.set(bundle, promise);
  return promise;
}

const dynamicLocaleBackend: BackendModule = {
  type: 'backend',
  init() {
    // 无需外部配置，直接复用打包期静态可分析的 LOCALE_LOADERS 表
  },
  read(language, _namespace, callback) {
    loadLocaleBundle(language)
      .then((res) => callback(null, res))
      .catch((err) => callback(err as Error, false));
  },
};

const initialBundle = resolveI18nBundle(
  (() => {
    try {
      return localStorage.getItem('tools3000:language') || 'auto';
    } catch {
      return 'auto';
    }
  })(),
);

export const i18nReady: Promise<unknown> = i18n
  .use(dynamicLocaleBackend)
  .use(LanguageDetector)
  .use(initReactI18next)
  .init({
    lng: initialBundle,
    fallbackLng: 'en',
    load: 'currentOnly',
    supportedLngs: ['en', 'zh', 'zh-CN', 'zh-TW', 'zh-HK', 'ja', 'ja-JP', 'ko', 'ko-KR', 'ru', 'ru-RU', 'pt-BR', 'pt', 'es', 'es-ES'],
    interpolation: {
      escapeValue: false, // react already safes from xss
    },
    react: {
      useSuspense: false,
    },
    detection: {
      order: ['navigator', 'htmlTag', 'path', 'subdomain'],
      caches: [], // C++ ConfigManager 是多语言偏好的单一事实源
    },
  });

applyHtmlLanguageAttribute(i18n.resolvedLanguage || initialBundle);
i18n.on('languageChanged', (lng) => {
  applyHtmlLanguageAttribute(lng);
});

export default i18n;

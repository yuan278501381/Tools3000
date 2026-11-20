import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import LanguageDetector from 'i18next-browser-languagedetector';

import enTranslation from './locales/en.json';
import zhTranslation from './locales/zh.json';
import zhTwTranslation from './locales/zh-TW.json';
import jaTranslation from './locales/ja.json';
import koTranslation from './locales/ko.json';
import ruTranslation from './locales/ru.json';
import ptBrTranslation from './locales/pt-BR.json';
import esTranslation from './locales/es.json';
import { applyHtmlLanguageAttribute, resolveI18nBundle } from './languages';

const initialBundle = resolveI18nBundle(
  (() => {
    try {
      return localStorage.getItem('tools3000:language') || 'auto';
    } catch {
      return 'auto';
    }
  })(),
);

i18n
  .use(LanguageDetector)
  .use(initReactI18next)
  .init({
    resources: {
      en: { translation: enTranslation },
      zh: { translation: zhTranslation },
      'zh-TW': { translation: zhTwTranslation },
      ja: { translation: jaTranslation },
      ko: { translation: koTranslation },
      ru: { translation: ruTranslation },
      'pt-BR': { translation: ptBrTranslation },
      es: { translation: esTranslation },
    },
    lng: initialBundle,
    fallbackLng: 'en',
    supportedLngs: ['en', 'zh', 'zh-CN', 'zh-TW', 'zh-HK', 'ja', 'ja-JP', 'ko', 'ko-KR', 'ru', 'ru-RU', 'pt-BR', 'pt', 'es', 'es-ES'],
    interpolation: {
      escapeValue: false, // react already safes from xss
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

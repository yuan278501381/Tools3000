// @vitest-environment jsdom
import { describe, it, expect, beforeEach } from 'vitest';
import {
  SUPPORTED_LANGUAGES,
  applyHtmlLanguageAttribute,
  findLanguageMeta,
  getDetectedSystemLanguage,
  mapTagToAppLanguage,
  normalizeLanguagePreference,
  resolveI18nBundle,
  setDetectedSystemLanguage,
} from './languages';

describe('i18n 8-language SSOT resolution', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('registers all 8 target languages with distinct codes, bundles, badges, and native endonyms', () => {
    expect(SUPPORTED_LANGUAGES.map((l) => l.code)).toEqual([
      'zh-CN',
      'zh-TW',
      'en-US',
      'ja-JP',
      'ko-KR',
      'ru-RU',
      'pt-BR',
      'es-ES',
    ]);
    expect(new Set(SUPPORTED_LANGUAGES.map((l) => l.bundle)).size).toBe(8);
    expect(new Set(SUPPORTED_LANGUAGES.map((l) => l.badge)).size).toBe(8);
    expect(SUPPORTED_LANGUAGES.every((l) => l.nativeName.length > 0)).toBe(true);
  });

  it('maps OS locale tags accurately including regional variants and English fallback', () => {
    expect(mapTagToAppLanguage('zh-CN')).toBe('zh-CN');
    expect(mapTagToAppLanguage('zh-SG')).toBe('zh-CN');
    expect(mapTagToAppLanguage('zh-Hans-CN')).toBe('zh-CN');
    expect(mapTagToAppLanguage('zh-TW')).toBe('zh-TW');
    expect(mapTagToAppLanguage('zh-HK')).toBe('zh-TW');
    expect(mapTagToAppLanguage('zh-MO')).toBe('zh-TW');
    expect(mapTagToAppLanguage('zh-Hant')).toBe('zh-TW');
    expect(mapTagToAppLanguage('ja')).toBe('ja-JP');
    expect(mapTagToAppLanguage('ja-JP')).toBe('ja-JP');
    expect(mapTagToAppLanguage('ko')).toBe('ko-KR');
    expect(mapTagToAppLanguage('ko-KR')).toBe('ko-KR');
    expect(mapTagToAppLanguage('ru')).toBe('ru-RU');
    expect(mapTagToAppLanguage('ru-RU')).toBe('ru-RU');
    expect(mapTagToAppLanguage('pt-BR')).toBe('pt-BR');
    expect(mapTagToAppLanguage('pt-PT')).toBe('pt-BR');
    expect(mapTagToAppLanguage('es-ES')).toBe('es-ES');
    expect(mapTagToAppLanguage('es-MX')).toBe('es-ES');
    expect(mapTagToAppLanguage('de-DE')).toBe('en-US');
    expect(mapTagToAppLanguage('fr-FR')).toBe('en-US');
    expect(mapTagToAppLanguage('')).toBe('en-US');
  });

  it('defaults to follow-system ("auto") and respects manual override', () => {
    expect(normalizeLanguagePreference(undefined)).toBe('auto');
    expect(normalizeLanguagePreference('')).toBe('auto');
    expect(normalizeLanguagePreference('auto')).toBe('auto');
    expect(normalizeLanguagePreference('system')).toBe('auto');
    expect(normalizeLanguagePreference('ja-JP')).toBe('ja-JP');

    setDetectedSystemLanguage('ja-JP');
    expect(getDetectedSystemLanguage()).toBe('ja-JP');
    expect(resolveI18nBundle('auto')).toBe('ja');

    // Manual override beats system language
    expect(resolveI18nBundle('ru-RU')).toBe('ru');
    expect(resolveI18nBundle('zh-TW')).toBe('zh-TW');
    expect(resolveI18nBundle('pt-BR')).toBe('pt-BR');
    expect(resolveI18nBundle('es-ES')).toBe('es');
    expect(resolveI18nBundle('ko-KR')).toBe('ko');
    expect(resolveI18nBundle('en-US')).toBe('en');
    expect(resolveI18nBundle('zh-CN')).toBe('zh');
  });

  it('updates document.documentElement lang attribute cleanly', () => {
    applyHtmlLanguageAttribute('ja');
    expect(document.documentElement.getAttribute('lang')).toBe('ja-JP');
    applyHtmlLanguageAttribute('zh-TW');
    expect(document.documentElement.getAttribute('lang')).toBe('zh-TW');
    applyHtmlLanguageAttribute('ko');
    expect(document.documentElement.getAttribute('lang')).toBe('ko-KR');
    expect(findLanguageMeta('ru').htmlLang).toBe('ru-RU');
  });
});

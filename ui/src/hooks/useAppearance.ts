import { useCallback, useLayoutEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { bridgeRequest, useBridgeEvent } from './useBridge';

export type Theme = 'light' | 'dark';
export type ThemePreference = Theme | 'system';
export type AccentColorId = 'blue' | 'cyan' | 'amber' | 'mint' | 'coral' | 'violet';

interface AppearancePayload {
  theme?: unknown;
  accentColor?: unknown;
  language?: unknown;
}

interface AppearanceUpdateResult {
  success?: boolean;
  error?: string;
}

const APPEARANCE_CHANNEL_NAME = 'tools3000_appearance';
const VALID_ACCENT_COLORS = new Set<string>(['blue', 'cyan', 'amber', 'mint', 'coral', 'violet']);

export const isThemePreference = (val: unknown): val is ThemePreference =>
  val === 'light' || val === 'dark' || val === 'system';

export const isAccentColor = (val: unknown): val is AccentColorId =>
  typeof val === 'string' && VALID_ACCENT_COLORS.has(val);

export const normalizeThemePreference = (
  val: unknown,
  fallback: ThemePreference = 'system',
): ThemePreference => (isThemePreference(val) ? val : fallback);

export const normalizeAccentColor = (
  val: unknown,
  fallback: AccentColorId = 'blue',
): AccentColorId => (isAccentColor(val) ? val : fallback);

const systemTheme = (): Theme =>
  window.matchMedia?.('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';

const applyTheme = (preference: ThemePreference) => {
  const theme = preference === 'system' ? systemTheme() : preference;
  document.documentElement.setAttribute('data-theme', theme);
  document.documentElement.style.colorScheme = theme;
};

const applyAccent = (accent: string) => {
  const normalized = normalizeAccentColor(accent, 'blue');
  document.documentElement.setAttribute('data-accent', normalized);
};

export const readStoredTheme = (fallback: ThemePreference = 'system'): ThemePreference => {
  try {
    const raw = localStorage.getItem('tools3000:theme');
    if (isThemePreference(raw)) return raw;
  } catch (e) {
    void e;
  }
  return fallback;
};

export const readStoredAccent = (fallback: AccentColorId = 'blue'): AccentColorId => {
  try {
    const raw = localStorage.getItem('tools3000:accent-color');
    if (isAccentColor(raw)) return raw;
  } catch (e) {
    void e;
  }
  const domAccent = typeof document !== 'undefined'
    ? document.documentElement.getAttribute('data-accent')
    : null;
  return normalizeAccentColor(domAccent, fallback);
};

const persistSharedAppearance = (payload: AppearancePayload) => {
  try {
    if (isThemePreference(payload.theme)) {
      localStorage.setItem('tools3000:theme', payload.theme);
    }
    if (isAccentColor(payload.accentColor)) {
      localStorage.setItem('tools3000:accent-color', payload.accentColor);
    }
    if (typeof payload.language === 'string' && payload.language) {
      localStorage.setItem('tools3000:language', payload.language);
    }
  } catch (e) {
    void e;
  }

  try {
    if (typeof BroadcastChannel !== 'undefined') {
      const ch = new BroadcastChannel(APPEARANCE_CHANNEL_NAME);
      ch.postMessage(payload);
      ch.close();
    }
  } catch (e) {
    void e;
  }
};

/**
 * 立即同步本地存储、DOM (data-theme / colorScheme / data-accent) 与跨组件/跨窗口事件。
 */
export function applyLocalAppearance(
  patch: { theme?: unknown; accentColor?: unknown },
  options: { dispatchEvents?: boolean } = {},
): { theme: ThemePreference; accentColor: AccentColorId } {
  const dispatchEvents = options.dispatchEvents ?? true;
  const hasValidTheme = isThemePreference(patch.theme);
  const hasValidAccent = isAccentColor(patch.accentColor);

  const effectiveTheme = hasValidTheme ? (patch.theme as ThemePreference) : readStoredTheme('system');
  const effectiveAccent = hasValidAccent ? (patch.accentColor as AccentColorId) : readStoredAccent('blue');

  if (hasValidTheme) {
    applyTheme(effectiveTheme);
  }
  if (hasValidAccent) {
    applyAccent(effectiveAccent);
  }

  persistSharedAppearance({
    ...(hasValidTheme ? { theme: effectiveTheme } : {}),
    ...(hasValidAccent ? { accentColor: effectiveAccent } : {}),
  });

  if (dispatchEvents && typeof window !== 'undefined') {
    if (hasValidTheme) {
      window.dispatchEvent(new CustomEvent('tools3000:theme-changed', { detail: effectiveTheme }));
    }
    if (hasValidAccent) {
      window.dispatchEvent(new CustomEvent('tools3000:accent-changed', { detail: effectiveAccent }));
    }
  }

  return { theme: effectiveTheme, accentColor: effectiveAccent };
}

/**
 * 可靠地将最新成对的 { theme, accentColor } 提交至 C++ ConfigManager，
 * 确保后端发布的 ThemeChangedEvent 与 appearance:changed 始终同步携带最新双字段。
 */
export async function persistAppearanceToBackend(patch: {
  theme?: unknown;
  accentColor?: unknown;
}): Promise<AppearanceUpdateResult> {
  const theme = isThemePreference(patch.theme)
    ? patch.theme
    : readStoredTheme('system');
  const accentColor = isAccentColor(patch.accentColor)
    ? patch.accentColor
    : readStoredAccent('blue');

  return bridgeRequest<AppearanceUpdateResult>('general.updateSettings', {
    theme,
    accentColor,
  });
}

/** Keep auxiliary WebView surfaces aligned with the shared app appearance & typography. */
export function useAppearance() {
  const { i18n } = useTranslation();
  const preferenceRef = useRef<ThemePreference>('system');
  const langPrefRef = useRef<string>('');
  const lastAppliedSigRef = useRef<string>('');
  const lastBroadcastSigRef = useRef<string>('');
  const lastWakeFetchRef = useRef<number>(0);

  const refreshAppliedSignature = useCallback(() => {
    const currentAccent = normalizeAccentColor(
      document.documentElement.getAttribute('data-accent'),
      'blue',
    );
    lastAppliedSigRef.current = `${preferenceRef.current}|${currentAccent}|${langPrefRef.current}`;
  }, []);

  const syncLanguage = useCallback((lang?: unknown) => {
    if (typeof lang !== 'string' || !lang) return;
    langPrefRef.current = lang;
    const lower = lang.toLowerCase();
    const targetLang = lower === 'auto'
      ? (navigator.language.toLowerCase().startsWith('zh') ? 'zh' : 'en')
      : (lower.startsWith('zh') ? 'zh' : 'en');
    if (i18n.language !== targetLang) {
      void i18n.changeLanguage(targetLang);
    }
  }, [i18n]);

  const applyPayload = useCallback((payload: AppearancePayload, broadcast = false) => {
    const nextTheme = isThemePreference(payload.theme) ? payload.theme : preferenceRef.current;
    const currentAccent = normalizeAccentColor(
      document.documentElement.getAttribute('data-accent'),
      'blue',
    );
    const nextAccent = isAccentColor(payload.accentColor)
      ? payload.accentColor
      : currentAccent;
    const nextLang = (typeof payload.language === 'string' && payload.language)
      ? payload.language
      : langPrefRef.current;
    const sig = `${nextTheme}|${nextAccent}|${nextLang}`;

    const domThemeAttr = document.documentElement.getAttribute('data-theme');
    const domAccentAttr = document.documentElement.getAttribute('data-accent');
    const expectedDomTheme = nextTheme === 'system' ? systemTheme() : nextTheme;
    const domOutOfSync = domThemeAttr !== expectedDomTheme || domAccentAttr !== nextAccent;

    if (sig !== lastAppliedSigRef.current || domOutOfSync) {
      if (isThemePreference(payload.theme)) {
        preferenceRef.current = payload.theme;
      }
      applyTheme(preferenceRef.current);
      applyAccent(nextAccent);
      syncLanguage(payload.language);
      lastAppliedSigRef.current = sig;
    }

    if (broadcast && sig !== lastBroadcastSigRef.current) {
      lastBroadcastSigRef.current = sig;
      persistSharedAppearance({
        theme: nextTheme,
        accentColor: nextAccent,
        ...(nextLang ? { language: nextLang } : {}),
      });
    }
  }, [syncLanguage]);

  const refreshFromBridge = useCallback(() => {
    void bridgeRequest<AppearancePayload>('general.getSettings')
      .then((settings) => {
        if (!settings || typeof settings !== 'object') return;
        applyPayload(settings, true);
      })
      .catch(() => {
        // Cached or system appearance remains active as fallback.
      });
  }, [applyPayload]);

  useBridgeEvent('appearance:changed', (data: unknown) => {
    if (data && typeof data === 'object') {
      applyPayload(data as AppearancePayload, true);
    } else {
      refreshFromBridge();
    }
  });

  useBridgeEvent('config:changed', () => {
    refreshFromBridge();
  });

  useLayoutEffect(() => {
    const mediaQuery = window.matchMedia('(prefers-color-scheme: dark)');
    const onSystemThemeChanged = () => {
      if (preferenceRef.current === 'system') {
        applyTheme('system');
        refreshAppliedSignature();
      }
    };

    const syncFromLocalStorage = () => {
      let storedAccent: AccentColorId = 'blue';
      try {
        const storedTheme = localStorage.getItem('tools3000:theme');
        if (isThemePreference(storedTheme)) {
          preferenceRef.current = storedTheme;
        }
        const rawAccent = localStorage.getItem('tools3000:accent-color');
        if (isAccentColor(rawAccent)) {
          storedAccent = rawAccent;
        } else {
          storedAccent = readStoredAccent('blue');
        }
        const storedLang = localStorage.getItem('tools3000:language');
        if (storedLang) syncLanguage(storedLang);
      } catch (e) {
        void e;
      }
      applyAccent(storedAccent);
      applyTheme(preferenceRef.current);
      refreshAppliedSignature();
    };

    syncFromLocalStorage();
    mediaQuery.addEventListener('change', onSystemThemeChanged);

    let channel: BroadcastChannel | null = null;
    try {
      if (typeof BroadcastChannel !== 'undefined') {
        channel = new BroadcastChannel(APPEARANCE_CHANNEL_NAME);
        channel.onmessage = (ev: MessageEvent) => {
          if (ev.data && typeof ev.data === 'object') {
            applyPayload(ev.data as AppearancePayload, false);
          }
        };
      }
    } catch (e) {
      void e;
    }

    const onStorage = (e: StorageEvent) => {
      if (e.key === 'tools3000:theme' && isThemePreference(e.newValue)) {
        preferenceRef.current = e.newValue;
        applyTheme(e.newValue);
        refreshAppliedSignature();
      }
      if (e.key === 'tools3000:accent-color' && isAccentColor(e.newValue)) {
        applyAccent(e.newValue);
        refreshAppliedSignature();
      }
      if (e.key === 'tools3000:language' && e.newValue) {
        syncLanguage(e.newValue);
        refreshAppliedSignature();
      }
    };

    const onAccentChanged = (e: Event) => {
      const customEvent = e as CustomEvent<string>;
      const candidate = customEvent.detail || localStorage.getItem('tools3000:accent-color');
      if (isAccentColor(candidate)) {
        applyAccent(candidate);
        refreshAppliedSignature();
        persistSharedAppearance({ accentColor: candidate });
      }
    };

    const onThemeChanged = (e: Event) => {
      const customEvent = e as CustomEvent<ThemePreference>;
      const candidate = customEvent.detail || localStorage.getItem('tools3000:theme');
      if (isThemePreference(candidate)) {
        preferenceRef.current = candidate;
        applyTheme(candidate);
        refreshAppliedSignature();
        persistSharedAppearance({ theme: candidate });
      }
    };

    const onLanguageChanged = (e: Event) => {
      const customEvent = e as CustomEvent<string>;
      const newLang = customEvent.detail || localStorage.getItem('tools3000:language');
      if (newLang) {
        syncLanguage(newLang);
        refreshAppliedSignature();
        persistSharedAppearance({ language: newLang });
      }
    };

    const onSurfaceWake = () => {
      if (document.hidden) return;
      syncFromLocalStorage();
      const now = Date.now();
      if (now - lastWakeFetchRef.current >= 1500) {
        lastWakeFetchRef.current = now;
        refreshFromBridge();
      }
    };

    window.addEventListener('storage', onStorage);
    window.addEventListener('tools3000:accent-changed', onAccentChanged);
    window.addEventListener('tools3000:theme-changed', onThemeChanged);
    window.addEventListener('tools3000:language-changed', onLanguageChanged);
    window.addEventListener('focus', onSurfaceWake);
    document.addEventListener('visibilitychange', onSurfaceWake);

    lastWakeFetchRef.current = Date.now();
    refreshFromBridge();

    return () => {
      mediaQuery.removeEventListener('change', onSystemThemeChanged);
      window.removeEventListener('storage', onStorage);
      window.removeEventListener('tools3000:accent-changed', onAccentChanged);
      window.removeEventListener('tools3000:theme-changed', onThemeChanged);
      window.removeEventListener('tools3000:language-changed', onLanguageChanged);
      window.removeEventListener('focus', onSurfaceWake);
      document.removeEventListener('visibilitychange', onSurfaceWake);
      if (channel) {
        channel.close();
      }
    };
  }, [applyPayload, refreshAppliedSignature, refreshFromBridge, syncLanguage]);
}

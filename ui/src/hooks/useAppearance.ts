import { useCallback, useLayoutEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { bridgeRequest, useBridgeEvent } from './useBridge';

type Theme = 'light' | 'dark';
type ThemePreference = Theme | 'system';

interface AppearancePayload {
  theme?: unknown;
  accentColor?: unknown;
  language?: unknown;
}

const APPEARANCE_CHANNEL_NAME = 'tools3000_appearance';

const isThemePreference = (val: unknown): val is ThemePreference =>
  val === 'light' || val === 'dark' || val === 'system';

const systemTheme = (): Theme =>
  window.matchMedia?.('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';

const applyTheme = (preference: ThemePreference) => {
  const theme = preference === 'system' ? systemTheme() : preference;
  document.documentElement.setAttribute('data-theme', theme);
  document.documentElement.style.colorScheme = theme;
};

const applyAccent = (accent: string) => {
  document.documentElement.setAttribute('data-accent', accent || 'blue');
};

const persistSharedAppearance = (payload: AppearancePayload) => {
  try {
    if (isThemePreference(payload.theme)) {
      localStorage.setItem('tools3000:theme', payload.theme);
    }
    if (typeof payload.accentColor === 'string' && payload.accentColor) {
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

/** Keep auxiliary WebView surfaces aligned with the shared app appearance & typography. */
export function useAppearance() {
  const { i18n } = useTranslation();
  const preferenceRef = useRef<ThemePreference>('system');
  const lastAppliedSigRef = useRef<string>('');
  const lastBroadcastSigRef = useRef<string>('');
  const lastWakeFetchRef = useRef<number>(0);

  const syncLanguage = useCallback((lang?: unknown) => {
    if (typeof lang !== 'string' || !lang) return;
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
    const currentAccent = document.documentElement.getAttribute('data-accent') || 'blue';
    const nextAccent = (typeof payload.accentColor === 'string' && payload.accentColor)
      ? payload.accentColor
      : currentAccent;
    const nextLang = (typeof payload.language === 'string' && payload.language) ? payload.language : '';
    const sig = `${nextTheme}|${nextAccent}|${nextLang}`;

    if (sig !== lastAppliedSigRef.current) {
      lastAppliedSigRef.current = sig;
      if (isThemePreference(payload.theme)) {
        preferenceRef.current = payload.theme;
        applyTheme(payload.theme);
      }
      if (typeof payload.accentColor === 'string' && payload.accentColor) {
        applyAccent(payload.accentColor);
      }
      syncLanguage(payload.language);
    }

    if (broadcast && sig !== lastBroadcastSigRef.current) {
      lastBroadcastSigRef.current = sig;
      persistSharedAppearance(payload);
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
      if (preferenceRef.current === 'system') applyTheme('system');
    };

    const syncFromLocalStorage = () => {
      try {
        const storedTheme = localStorage.getItem('tools3000:theme');
        if (isThemePreference(storedTheme)) {
          preferenceRef.current = storedTheme;
        }
        const storedAccent = localStorage.getItem('tools3000:accent-color');
        if (storedAccent) applyAccent(storedAccent);
        const storedLang = localStorage.getItem('tools3000:language');
        if (storedLang) syncLanguage(storedLang);
      } catch (e) {
        void e;
      }
      applyTheme(preferenceRef.current);
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
      }
      if (e.key === 'tools3000:accent-color' && e.newValue) {
        applyAccent(e.newValue);
      }
      if (e.key === 'tools3000:language' && e.newValue) {
        syncLanguage(e.newValue);
      }
    };

    const onAccentChanged = (e: Event) => {
      const customEvent = e as CustomEvent<string>;
      const newAccent = customEvent.detail || localStorage.getItem('tools3000:accent-color');
      if (newAccent) {
        applyAccent(newAccent);
        persistSharedAppearance({ accentColor: newAccent });
      }
    };

    const onThemeChanged = (e: Event) => {
      const customEvent = e as CustomEvent<ThemePreference>;
      const newTheme = customEvent.detail || (localStorage.getItem('tools3000:theme') as ThemePreference);
      if (isThemePreference(newTheme)) {
        preferenceRef.current = newTheme;
        applyTheme(newTheme);
        persistSharedAppearance({ theme: newTheme });
      }
    };

    const onLanguageChanged = (e: Event) => {
      const customEvent = e as CustomEvent<string>;
      const newLang = customEvent.detail || localStorage.getItem('tools3000:language');
      if (newLang) {
        syncLanguage(newLang);
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
  }, [applyPayload, refreshFromBridge, syncLanguage]);
}

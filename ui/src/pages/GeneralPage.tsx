/* ─────────────────────────────────────────────────────────────────────────────
 * GeneralPage — 通用设置页
 *
 * 从 C++ ConfigManager 加载设置，修改后通过 IPC 实时保存。
 * 遵循由高频到低频再到危险收尾的 7 大正交分组信息架构。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import {
  Card,
  Toggle,
  SettingRow,
  SettingGroup,
  SubSettingGroup,
  Select,
  Button,
  CodeBadge,
  Modal,
  ConfirmModal,
  PageSkeleton,
} from '../components/UIKit';
import { CloudBackupCard } from '../components/business/CloudBackupCard';
import { HotkeyAppExclusionBar } from '../components/business/HotkeyAppExclusionBar';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import type { NavId } from '../config/routes';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import { groupHotkeysByDomain, resolveHotkeyLabel, sortHotkeysByPriority } from '../config/hotkeyCatalog';
import {
  applyLocalAppearance,
  isAccentColor,
  isThemePreference,
  persistAppearanceToBackend,
  readStoredAccent,
  readStoredTheme,
} from '../hooks/useAppearance';
import {
  SUPPORTED_LANGUAGES,
  applyHtmlLanguageAttribute,
  normalizeLanguagePreference,
  resolveI18nBundle,
  setDetectedSystemLanguage,
} from '../i18n/languages';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import {
  Settings,
  Zap,
  Globe,
  Database,
  Keyboard,
  Download,
  Upload,
  RotateCcw,
  RefreshCw,
  Check,
  CheckCircle2,
  AlertTriangle,
  AlertOctagon,
  FolderOpen,
  Layers,
  MinusCircle,
  Package,
  HardDrive,
  Sparkles,
  MemoryStick,
  ArrowUpRight,
  Shield,
} from 'lucide-react';
import './GeneralPage.css';

export interface GeneralPageProps {
  onNavigate?: (nav: NavId) => void;
  onOpenOnboarding?: () => void;
}

interface GeneralSettings {
  autoStart: boolean;
  runAsAdmin: boolean;
  elevated?: boolean;
  minimizeToTray: boolean;
  checkUpdates: boolean;
  autoDownloadUpdates?: boolean;
  showChangelogOnUpdate?: boolean;
  autoReleaseSettingsMemory?: boolean;
  fullscreenMode?: string;
  showOnboarding?: boolean;
  isPortableMode?: boolean;
  dataDirectory?: string;
  language: string;
  systemLanguage?: string;
  resolvedLanguage?: string;
  logLevel: string;
  logRetentionDays?: number;
  theme: string;
  accentColor?: string;
  trayIconTheme?: string;
}

interface OperationResult {
  success: boolean;
  cancelled?: boolean;
  error?: string;
  shortcut?: string;
  conflictType?: string;
  conflictWith?: string;
}

const ACCENT_PRESETS = [
  { id: 'blue',   labelKey: 'general.accentBlue',   color: '#3b82f6' },
  { id: 'cyan',   labelKey: 'general.accentCyan',   color: '#06b6d4' },
  { id: 'amber',  labelKey: 'general.accentAmber',  color: '#f59e0b' },
  { id: 'mint',   labelKey: 'general.accentMint',   color: '#10b981' },
  { id: 'coral',  labelKey: 'general.accentCoral',  color: '#f43f5e' },
  { id: 'violet', labelKey: 'general.accentViolet', color: '#8b5cf6' },
] as const;

export const GeneralPage: FC<GeneralPageProps> = ({ onNavigate, onOpenOnboarding }) => {
  const [settings, setSettings] = useState<GeneralSettings>({
    autoStart: false,
    runAsAdmin: true,
    elevated: false,
    minimizeToTray: true,
    checkUpdates: true,
    autoDownloadUpdates: true,
    showChangelogOnUpdate: true,
    autoReleaseSettingsMemory: true,
    fullscreenMode: 'smart',
    language: 'auto',
    // TODO(pre-release): 缺陷排查收敛后正式发版前，切回 'info'
    logLevel: 'trace',
    logRetentionDays: 7,
    theme: readStoredTheme('dark'),
    accentColor: readStoredAccent('blue'),
    trayIconTheme: 'system',
  });
  const [loading, setLoading] = useState(true);
  const [refreshingHotkeys, setRefreshingHotkeys] = useState(false);
  const [isRestartingElevated, setIsRestartingElevated] = useState(false);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [globalExcludedApps, setGlobalExcludedApps] = useState<string[]>([]);
  const [editingExclusionHotkey, setEditingExclusionHotkey] = useState<HotkeyEntry | null>(null);
  const [accent, setAccent] = useState<string>(() => readStoredAccent('blue'));

  const { t, i18n } = useTranslation();

  const handleSaveGlobalExclusions = async (nextApps: string[]) => {
    const previous = globalExcludedApps;
    setGlobalExcludedApps(nextApps);
    try {
      const res = await bridgeRequest<OperationResult & { apps?: string[] }>('hotkey.setGlobalExclusions', {
        apps: nextApps,
      });
      if (res && res.success === false) {
        throw new Error(res.error || t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'));
      }
      if (res && Array.isArray(res.apps)) {
        setGlobalExcludedApps(res.apps);
      }
    } catch (error) {
      setGlobalExcludedApps(previous);
      toast.error(t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'), {
        description: String(error),
      });
    }
  };

  const handleSavePerHotkeyExclusions = async (hotkeyName: string, nextApps: string[]) => {
    try {
      const res = await bridgeRequest<OperationResult>('hotkey.setHotkeyExclusions', {
        name: hotkeyName,
        apps: nextApps,
      });
      if (res && res.success === false) {
        throw new Error(res.error || t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'));
      }
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) {
        const sorted = sortHotkeysByPriority(refreshed);
        setHotkeys(sorted);
        const updated = sorted.find((item) => item.name === hotkeyName) ?? null;
        setEditingExclusionHotkey(updated);
      }
    } catch (error) {
      toast.error(t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'), {
        description: String(error),
      });
    }
  };

  const handleAccentChange = useCallback(async (id: string) => {
    if (!isAccentColor(id)) return;
    const previousAccent = accent;
    setAccent(id);
    setSettings(prev => ({ ...prev, accentColor: id }));
    applyLocalAppearance({ accentColor: id });

    try {
      const result = await persistAppearanceToBackend({
        theme: settings.theme,
        accentColor: id,
      });
      if (!result?.success) {
        throw new Error(result?.error || t('general.toastSaveFailed'));
      }
    } catch (error) {
      setAccent(curr => (curr === id ? previousAccent : curr));
      setSettings(prev => (prev.accentColor === id ? { ...prev, accentColor: previousAccent } : prev));
      applyLocalAppearance({ accentColor: previousAccent });
      toast.error(t('general.toastSaveFailed'), { description: String(error) });
    }
  }, [accent, settings.theme, t]);

  const applyLanguagePreference = useCallback((langValue?: string, systemLang?: string) => {
    if (systemLang) {
      setDetectedSystemLanguage(systemLang);
    }
    if (!langValue) return;
    const normalizedPref = normalizeLanguagePreference(langValue);
    try {
      localStorage.setItem('tools3000:language', normalizedPref);
    } catch (e) {
      void e;
    }
    const targetLang = resolveI18nBundle(normalizedPref, systemLang);
    applyHtmlLanguageAttribute(targetLang);
    if (i18n.language !== targetLang) {
      void i18n.changeLanguage(targetLang);
    }
    window.dispatchEvent(new CustomEvent('tools3000:language-changed', { detail: normalizedPref }));
  }, [i18n]);

  // 初始化获取设置
  useEffect(() => {
    let cancelled = false;
    Promise.all([
      bridgeRequest<GeneralSettings>('general.getSettings'),
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll'),
      bridgeRequest<{ global?: string[] }>('hotkey.getExclusions').catch(() => ({ global: [] })),
    ]).then(([res, hotkeyData, exclusionData]) => {
      if (cancelled) return;
      setSettings(prev => ({ ...prev, ...res }));
      if (isAccentColor(res.accentColor)) {
        setAccent(res.accentColor);
      }
      applyLocalAppearance(
        { theme: res.theme, accentColor: res.accentColor },
        { dispatchEvents: false },
      );
      if (res.systemLanguage) {
        setDetectedSystemLanguage(res.systemLanguage);
      }
      const normalizedLang = normalizeLanguagePreference(res.language);
      setSettings(prev => ({ ...prev, language: normalizedLang }));
      const targetLang = resolveI18nBundle(normalizedLang, res.systemLanguage);
      applyHtmlLanguageAttribute(targetLang);
      if (i18n.language !== targetLang) {
        void i18n.changeLanguage(targetLang);
      }
      setHotkeys(Array.isArray(hotkeyData) ? sortHotkeysByPriority(hotkeyData) : []);
      if (exclusionData && Array.isArray(exclusionData.global)) {
        setGlobalExcludedApps(exclusionData.global);
      }
    })
    .catch((error) => {
      if (cancelled) return;
      console.error(error);
      toast.error(t('general.loadFailed'));
    })
    .finally(() => {
      if (!cancelled) setLoading(false);
    });
    return () => { cancelled = true; };
    // i18n.changeLanguage 会换掉 t/i18n 引用；放进依赖会把 IPC 打成几百 Hz，卡死主线程钩子。
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // 监听后端 appearance:changed 事件与顶栏发出的语言/主题/强调色变更，保持页面双向数据同步
  useBridgeEvent('appearance:changed', (data: unknown) => {
    if (!data || typeof data !== 'object') return;
    const payload = data as { theme?: unknown; accentColor?: unknown };
    if (isThemePreference(payload.theme)) {
      setSettings(prev => ({ ...prev, theme: payload.theme as string }));
    }
    if (isAccentColor(payload.accentColor)) {
      setAccent(payload.accentColor);
      setSettings(prev => ({ ...prev, accentColor: payload.accentColor as string }));
    }
  });

  useEffect(() => {
    const handleThemeEvent = (e: Event) => {
      const pref = (e as CustomEvent<string>).detail;
      if (isThemePreference(pref)) setSettings(prev => ({ ...prev, theme: pref }));
    };
    const handleLangEvent = (e: Event) => {
      const lang = (e as CustomEvent<string>).detail;
      if (lang) setSettings(prev => ({ ...prev, language: lang }));
    };
    const handleAccentEvent = (e: Event) => {
      const newAccent = (e as CustomEvent<string>).detail;
      if (isAccentColor(newAccent)) {
        setAccent(newAccent);
        setSettings(prev => ({ ...prev, accentColor: newAccent }));
      }
    };
    const handleChangelogPrefEvent = (e: Event) => {
      const enabled = (e as CustomEvent<boolean>).detail;
      if (typeof enabled === 'boolean') {
        setSettings(prev => ({ ...prev, showChangelogOnUpdate: enabled }));
      }
    };
    window.addEventListener('tools3000:theme-changed', handleThemeEvent);
    window.addEventListener('tools3000:language-changed', handleLangEvent);
    window.addEventListener('tools3000:accent-changed', handleAccentEvent);
    window.addEventListener('tools3000:changelog-pref-changed', handleChangelogPrefEvent);
    return () => {
      window.removeEventListener('tools3000:theme-changed', handleThemeEvent);
      window.removeEventListener('tools3000:language-changed', handleLangEvent);
      window.removeEventListener('tools3000:accent-changed', handleAccentEvent);
      window.removeEventListener('tools3000:changelog-pref-changed', handleChangelogPrefEvent);
    };
  }, []);

  // 保存单个设置项
  const updateSetting = useCallback(async <K extends keyof GeneralSettings,>(key: K, value: GeneralSettings[K]) => {
    const previous = settings[key];
    setSettings(prev => ({ ...prev, [key]: value }));

    const applyLocalValue = (localValue: GeneralSettings[K]) => {
      if (key === 'language') {
        applyLanguagePreference(String(localValue));
      } else if (key === 'theme') {
        applyLocalAppearance({ theme: localValue });
      }
    };
    applyLocalValue(value);

    try {
      const result = key === 'theme'
        ? await persistAppearanceToBackend({ theme: value, accentColor: accent })
        : await bridgeRequest<OperationResult>('general.updateSettings', { [key]: value });
      if (!result?.success) throw new Error(result?.error || t('general.toastSaveFailed'));
    } catch (error) {
      setSettings(prev => prev[key] === value ? { ...prev, [key]: previous } : prev);
      applyLocalValue(previous);
      toast.error(t('general.toastSaveFailed'), { description: String(error) });
    }
  }, [accent, applyLanguagePreference, settings, t]);

  const handleToggleRunAsAdmin = async (checked: boolean) => {
    if (isRestartingElevated) return;
    const previous = settings.runAsAdmin;
    setSettings(prev => ({ ...prev, runAsAdmin: checked }));
    try {
      const persist = await bridgeRequest<OperationResult>('general.updateSettings', { runAsAdmin: checked });
      if (!persist.success) throw new Error(persist.error || t('general.toastSaveFailed'));

      if (checked && !settings.elevated) {
        setIsRestartingElevated(true);
        const result = await bridgeRequest<OperationResult & { alreadyElevated?: boolean }>(
          'app.restartElevated',
        );
        if (result.alreadyElevated) {
          setSettings(prev => ({ ...prev, elevated: true }));
          setIsRestartingElevated(false);
          return;
        }
        if (!result.success) {
          setIsRestartingElevated(false);
          toast.error(result.cancelled ? t('general.runAsAdminCancelled') : t('general.runAsAdminFailed'));
        }
        return;
      }

      if (!checked && settings.elevated) {
        setIsRestartingElevated(true);
        const result = await bridgeRequest<OperationResult>('app.restartDemoted');
        if (!result || !result.success) {
          setIsRestartingElevated(false);
          toast.info(result?.error || t('general.runAsAdminDemoteSaved'));
        }
        return;
      }
    } catch (error) {
      setSettings(prev => ({ ...prev, runAsAdmin: previous }));
      setIsRestartingElevated(false);
      toast.error(t('general.toastSaveFailed'), { description: String(error) });
    }
  };

  const handleReopenOnboarding = () => {
    if (onOpenOnboarding) {
      void bridgeRequest('general.updateSettings', { showOnboarding: true }, { silent: true }).catch(() => undefined);
      onOpenOnboarding();
    } else {
      void updateSetting('showOnboarding', true);
    }
  };

  // ── 数据管理操作 ─────────────────────────────────────────────────────────
  const handleExportConfig = async () => {
    try {
      const result = await bridgeRequest<OperationResult>('config.export');
      if (result.cancelled) return;
      if (!result.success) throw new Error(result.error || t('general.exportFailed'));
      toast.success(t('general.exportSuccess'));
    } catch (e) {
      toast.error(t('general.exportFailed'), { description: String(e) });
    }
  };

  const [confirmResetOpen, setConfirmResetOpen] = useState(false);

  const handleImportConfig = async () => {
    try {
      const result = await bridgeRequest<OperationResult>('config.import');
      if (result.cancelled) return;
      if (!result.success) throw new Error(result.error || t('general.importFailed'));
      toast.success(t('general.importSuccess'));
      const [refreshed, refreshedHotkeys] = await Promise.all([
        bridgeRequest<GeneralSettings>('general.getSettings'),
        bridgeRequest<HotkeyEntry[]>('hotkey.getAll').catch(() => []),
      ]);
      setSettings(prev => ({ ...prev, ...refreshed }));
      if (isAccentColor(refreshed.accentColor)) {
        setAccent(refreshed.accentColor);
      }
      if (Array.isArray(refreshedHotkeys)) setHotkeys(sortHotkeysByPriority(refreshedHotkeys));
      applyLanguagePreference(refreshed.language);
      applyLocalAppearance({ theme: refreshed.theme, accentColor: refreshed.accentColor });
      void persistAppearanceToBackend({ theme: refreshed.theme, accentColor: refreshed.accentColor });
    } catch (e) {
      toast.error(t('general.importFailed'), { description: String(e) });
    }
  };

  const handleResetConfig = async () => {
    setConfirmResetOpen(false);
    try {
      const result = await bridgeRequest<OperationResult>('config.reset');
      if (!result.success) throw new Error(result.error || t('general.resetFailed'));
      toast.success(t('general.resetSuccess'));
      const [res, refreshedHotkeys] = await Promise.all([
        bridgeRequest<GeneralSettings>('general.getSettings'),
        bridgeRequest<HotkeyEntry[]>('hotkey.getAll').catch(() => []),
      ]);
      setSettings(prev => ({ ...prev, ...res }));
      if (isAccentColor(res.accentColor)) {
        setAccent(res.accentColor);
      }
      if (Array.isArray(refreshedHotkeys)) setHotkeys(sortHotkeysByPriority(refreshedHotkeys));
      applyLanguagePreference(res.language);
      applyLocalAppearance({ theme: res.theme, accentColor: res.accentColor });
      void persistAppearanceToBackend({ theme: res.theme, accentColor: res.accentColor });
    } catch (e) {
      toast.error(t('general.resetFailed'), { description: String(e) });
    }
  };

  const refreshAllAfterRestore = useCallback(async () => {
    try {
      const [res, refreshedHotkeys] = await Promise.all([
        bridgeRequest<GeneralSettings>('general.getSettings'),
        bridgeRequest<HotkeyEntry[]>('hotkey.getAll').catch(() => []),
      ]);
      setSettings(prev => ({ ...prev, ...res }));
      if (isAccentColor(res.accentColor)) {
        setAccent(res.accentColor);
      }
      if (Array.isArray(refreshedHotkeys)) setHotkeys(sortHotkeysByPriority(refreshedHotkeys));
      applyLanguagePreference(res.language);
      applyLocalAppearance({ theme: res.theme, accentColor: res.accentColor });
      void persistAppearanceToBackend({ theme: res.theme, accentColor: res.accentColor });
    } catch {
      // 忽略静默刷新异常
    }
  }, [applyLanguagePreference]);

  // ── 刷新快捷键状态 ───────────────────────────────────────────────────────
  const refreshHotkeys = useCallback(async () => {
    setRefreshingHotkeys(true);
    try {
      const data = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      setHotkeys(Array.isArray(data) ? sortHotkeysByPriority(data) : []);
      toast.success(t('general.recheckShortcuts'));
    } catch {
      toast.error(t('general.loadFailed'));
    } finally {
      setRefreshingHotkeys(false);
    }
  }, [t]);

  const rebindHotkey = async (entry: HotkeyEntry, shortcut: string) => {
    const previous = entry.shortcut;
    const previousRegistered = entry.registered;
    setHotkeys(items => items.map(item =>
      item.name === entry.name ? { ...item, shortcut } : item));
    try {
      const result = await bridgeRequest<OperationResult>('hotkey.rebind', {
        name: entry.name,
        hotkey: shortcut,
      });
      if (!result.success) throw new Error(result.error || t('hotkey.bindFailed'));
      const applied = result.shortcut ?? shortcut;
      // 重新拉取完整状态以同步全量冲突关系
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      setHotkeys(Array.isArray(refreshed) ? sortHotkeysByPriority(refreshed) : []);
      toast.success(`${resolveHotkeyLabel(t, entry.name)}: ${applied || t('general.shortcutDisabled')}`);
    } catch (error) {
      setHotkeys(items => items.map(item =>
        item.name === entry.name && item.shortcut === shortcut
          ? { ...item, shortcut: previous, registered: previousRegistered }
          : item));
      toast.error(t('hotkey.bindFailed'), { description: String(error) });
    }
  };

  const internalConflictsCount = hotkeys.filter(h => h.conflictType === 'internal').length;
  const externalConflictsCount = hotkeys.filter(h =>
    h.conflictType === 'external' || (h.registered === false && Boolean(h.shortcut) && h.armed !== false)).length;
  const sessionOnlyCount = hotkeys.filter(h =>
    Boolean(h.shortcut) && h.armed === false && !h.conflict && h.conflictType !== 'external' && h.conflictType !== 'internal').length;
  const globalActiveCount = hotkeys.filter(h =>
    Boolean(h.shortcut) && (h.registered !== false) && h.armed !== false && !h.conflict && h.conflictType !== 'internal' && h.conflictType !== 'external').length;
  const unboundCount = hotkeys.filter(h => !h.shortcut).length;

  if (loading) {
    return <PageSkeleton cards={3} rowsPerCard={4} />;
  }

  return (
    <div className="general-page" style={{ animation: 'fadeIn 0.3s ease' }}>
      {/* ── 1. 启动与运行权限 ───────────────────────────────────────── */}
      <SettingGroup title={t('general.startupAndPrivilege')} icon={<Zap size={20} strokeWidth={2.5} />}>
        <Card>
          <Toggle
            id="autoStart"
            label={t('general.autoStart')}
            description={t('general.autoStartDesc')}
            checked={settings.autoStart}
            onChange={(v) => updateSetting('autoStart', v)}
          />
          <Toggle
            id="runAsAdmin"
            label={t('general.runAsAdmin')}
            description={settings.elevated ? t('general.runAsAdminActive') : t('general.runAsAdminDesc')}
            checked={settings.runAsAdmin}
            onChange={handleToggleRunAsAdmin}
            disabled={isRestartingElevated}
          />
          <Toggle
            id="minimizeToTray"
            label={t('general.minimizeToTray')}
            description={t('general.minimizeToTrayDesc')}
            checked={settings.minimizeToTray}
            onChange={(v) => updateSetting('minimizeToTray', v)}
          />
          <SettingRow label={t('general.fullscreenMode')} description={t('general.fullscreenModeDesc')}>
            <Select
              id="fullscreenMode"
              value={settings.fullscreenMode || 'smart'}
              onChange={(v) => updateSetting('fullscreenMode', v)}
              options={[
                { value: 'smart', label: t('general.fullscreenModeSmart') },
                { value: 'strict_all', label: t('general.fullscreenModeStrictAll') },
                { value: 'gesture_only', label: t('general.fullscreenModeGestureOnly') },
                { value: 'off', label: t('general.fullscreenModeOff') },
              ]}
            />
          </SettingRow>
        </Card>
      </SettingGroup>

      {/* ── 2. 界面与语言 ───────────────────────────────────────────── */}
      <SettingGroup title={t('general.uiAndLang')} icon={<Globe size={20} strokeWidth={2.5} />}>
        <Card>
          <SettingRow label={t('general.language')} description={t('general.languageDesc')}>
            <Select
              id="language"
              value={normalizeLanguagePreference(settings.language)}
              onChange={(v) => updateSetting('language', v)}
              options={[
                { value: 'auto', label: t('general.langAuto') },
                ...SUPPORTED_LANGUAGES.map((opt) => ({
                  value: opt.code,
                  label: opt.nativeName,
                })),
              ]}
            />
          </SettingRow>

          <SettingRow label={t('general.theme')} description={t('general.themeDesc')} layout="vertical">
            <div id="theme" className="general-theme-cards" role="radiogroup" aria-label={t('general.theme')}>
              {([
                { value: 'system', labelKey: 'general.themeSystem', previewClass: 'is-system' },
                { value: 'light',  labelKey: 'general.themeLight',  previewClass: 'is-light' },
                { value: 'dark',   labelKey: 'general.themeDark',   previewClass: 'is-dark' },
              ] as const).map((opt) => {
                const isSelected = (settings.theme || 'system') === opt.value;
                const renderWireframe = (surface: 'light' | 'dark', extraClass = '') => (
                  <span className={`general-theme-preview__scene general-theme-preview__scene--${surface} ${extraClass}`}>
                    <span className="general-theme-preview__sidebar">
                      <span className="general-theme-preview__brand-bar" />
                      <span className="general-theme-preview__nav-pill is-accent" />
                      <span className="general-theme-preview__nav-pill" />
                      <span className="general-theme-preview__nav-pill is-short" />
                    </span>
                    <span className="general-theme-preview__canvas">
                      <span className="general-theme-preview__heading" />
                      <span className="general-theme-preview__card">
                        <span className="general-theme-preview__line" />
                        <span className="general-theme-preview__toggle-dot" />
                      </span>
                      <span className="general-theme-preview__card is-short">
                        <span className="general-theme-preview__line is-muted" />
                      </span>
                    </span>
                  </span>
                );
                return (
                  <button
                    key={opt.value}
                    type="button"
                    role="radio"
                    aria-checked={isSelected}
                    className={`general-theme-card ${isSelected ? 'is-active' : ''}`}
                    onClick={() => updateSetting('theme', opt.value)}
                  >
                    <span className={`general-theme-preview ${opt.previewClass}`} aria-hidden="true">
                      {opt.value === 'system' ? (
                        <>
                          {renderWireframe('light')}
                          {renderWireframe('dark', 'general-theme-preview__scene--split-right')}
                          <span className="general-theme-preview__split-seam" />
                        </>
                      ) : (
                        renderWireframe(opt.value)
                      )}
                    </span>
                    <span className="general-theme-card__footer">
                      <span className="general-theme-card__label">{t(opt.labelKey)}</span>
                      <span className="general-theme-card__radio" aria-hidden="true">
                        {isSelected && <Check size={10} strokeWidth={3} />}
                      </span>
                    </span>
                  </button>
                );
              })}
            </div>
          </SettingRow>

          <SettingRow label={t('general.accentColor')} description={t('general.accentColorDesc')} layout="vertical">
            <div className="general-accent-swatches">
              {ACCENT_PRESETS.map((preset) => {
                const isSelected = accent === preset.id;
                const label = t(preset.labelKey);
                return (
                  <button
                    key={preset.id}
                    type="button"
                    aria-pressed={isSelected}
                    className={`general-accent-btn ${isSelected ? 'active' : ''}`}
                    onClick={() => handleAccentChange(preset.id)}
                    style={{ '--accent-dot-color': preset.color } as React.CSSProperties}
                    title={label}
                  >
                    <span
                      className="general-accent-dot"
                      style={{ backgroundColor: preset.color }}
                    >
                      {isSelected && <Check size={13} strokeWidth={2.9} className="general-accent-check" aria-hidden="true" />}
                    </span>
                    <span className="general-accent-label">{label}</span>
                  </button>
                );
              })}
            </div>
          </SettingRow>

          <SettingRow label={t('general.trayIconTheme')} description={t('general.trayIconThemeDesc')} layout="vertical">
            <div id="trayIconTheme" className="general-segmented" role="radiogroup" aria-label={t('general.trayIconTheme')}>
              {([
                { value: 'system', labelKey: 'general.trayIconThemeSystem' },
                { value: 'light',  labelKey: 'general.trayIconThemeLight' },
                { value: 'dark',   labelKey: 'general.trayIconThemeDark' },
                { value: 'color',  labelKey: 'general.trayIconThemeColor' },
              ] as const).map((opt) => {
                const isSelected = (settings.trayIconTheme || 'system') === opt.value;
                return (
                  <button
                    key={opt.value}
                    type="button"
                    role="radio"
                    aria-checked={isSelected}
                    className={`general-segmented__btn ${isSelected ? 'is-active' : ''}`}
                    onClick={() => updateSetting('trayIconTheme', opt.value)}
                  >
                    {t(opt.labelKey)}
                  </button>
                );
              })}
            </div>
          </SettingRow>
        </Card>
      </SettingGroup>

      {/* ── 3. 快捷键总览与冲突检测（高频前置） ──────────────────────── */}
      <SettingGroup title={t('general.keyboardShortcuts')} icon={<Keyboard size={20} strokeWidth={2.5} />}>
        <Card>
          <div className="general-page__hotkey-health-bar">
            <div className="general-page__hotkey-health-stats">
              <span className="general-page__stat-pill total">
                <Keyboard size={12} /> {t('general.hotkeysAllCount', 'All {{count}} items', { count: hotkeys.length })}
              </span>
              <span className="general-page__stat-pill ok">
                <CheckCircle2 size={12} /> {t('general.hotkeysActiveCount', 'Active {{count}} items', { count: globalActiveCount })}
              </span>
              {sessionOnlyCount > 0 && (
                <span className="general-page__stat-pill session">
                  <Layers size={12} /> {t('general.hotkeysSessionCount', 'Scoped {{count}} items', { count: sessionOnlyCount })}
                </span>
              )}
              {internalConflictsCount > 0 && (
                <span className="general-page__stat-pill warning">
                  <AlertTriangle size={12} /> {t('general.hotkeysInternalConflictsCount', 'Internal Conflicts {{count}} items', { count: internalConflictsCount })}
                </span>
              )}
              {externalConflictsCount > 0 && (
                <span className="general-page__stat-pill danger">
                  <AlertOctagon size={12} /> {t('general.hotkeysExternalConflictsCount', 'External Conflicts {{count}} items', { count: externalConflictsCount })}
                </span>
              )}
              {unboundCount > 0 && (
                <span className="general-page__stat-pill unbound">
                  <MinusCircle size={12} /> {t('general.hotkeysUnboundCount', 'Unbound {{count}} items', { count: unboundCount })}
                </span>
              )}
            </div>
            <Button
              variant="ghost"
              className="general-page__recheck-btn"
              onClick={refreshHotkeys}
              disabled={refreshingHotkeys}
              title={t('general.recheckShortcuts')}
            >
              <RefreshCw size={12} className={refreshingHotkeys ? 'general-page__spin' : ''} />
              <span>{t('general.recheckShortcuts')}</span>
            </Button>
          </div>

          {hotkeys.length === 0 ? (
            <div className="general-page__empty">{t('general.noShortcuts')}</div>
          ) : (
            <div className="general-page__hotkey-sections">
              {groupHotkeysByDomain(hotkeys).map((section) => (
                <section
                  key={section.id}
                  className="general-page__hotkey-section"
                  aria-label={String(t(section.titleKey as never, { defaultValue: section.defaultTitle }))}
                >
                  <div className="general-page__hotkey-section-header">
                    <span className="general-page__hotkey-section-title">
                      {String(t(section.titleKey as never, { defaultValue: section.defaultTitle }))}
                    </span>
                    <span className="general-page__hotkey-section-count">
                      {section.items.length}
                    </span>
                  </div>
                  <div className="general-page__hotkey-list">
                    {section.items.map((hk) => {
                      const isInternal = hk.conflictType === 'internal';
                      const isExternal = hk.conflictType === 'external' ||
                        (hk.registered === false && Boolean(hk.shortcut) && hk.armed !== false);

                      return (
                        <div
                          key={hk.name}
                          className={`general-page__hotkey-item ${isInternal ? 'is-internal' : ''} ${isExternal ? 'is-external' : ''}`}
                        >
                          <div className="general-page__hotkey-info">
                            <div className="general-page__hotkey-name-row">
                              <span className="general-page__hotkey-name">
                                {resolveHotkeyLabel(t, hk.name)}
                              </span>
                              <HotkeyStatusBadge entry={hk} />
                            </div>
                          </div>
                          <div className="general-page__hotkey-control">
                            <button
                              type="button"
                              className={`general-page__hotkey-exclude-btn ${(hk.excludedApps?.length ?? 0) > 0 ? 'has-apps' : ''}`}
                              onClick={() => setEditingExclusionHotkey(hk)}
                              title={
                                (hk.excludedApps?.length ?? 0) > 0
                                  ? t('hotkeyExclusion.badgeTooltip', 'Auto-yields in: {{apps}}', {
                                      apps: hk.excludedApps!.join(', '),
                                    })
                                  : t('hotkeyExclusion.configurePerHotkeyTip', 'Configure per-app auto-yield rules')
                              }
                            >
                              <Shield size={13} aria-hidden="true" />
                              {(hk.excludedApps?.length ?? 0) > 0 && (
                                <span>
                                  {t('hotkeyExclusion.excludedCountBadge', 'Yields {{count}}', {
                                    count: hk.excludedApps!.length,
                                  })}
                                </span>
                              )}
                            </button>
                            <HotkeyRecorder
                              id={`general-hotkey-${hk.name.replace(/\s+/g, '-').toLowerCase()}`}
                              value={hk.shortcut}
                              ariaLabel={resolveHotkeyLabel(t, hk.name)}
                              onChange={(value) => void rebindHotkey(hk, value)}
                            />
                          </div>
                        </div>
                      );
                    })}
                  </div>
                </section>
              ))}
            </div>
          )}

          <div className="general-page__hotkey-global-exclusions">
            <SettingRow
              layout="vertical"
              label={t('general.globalHotkeyExclusionsTitle', 'Global Hotkey Auto-Yield Apps')}
              description={t(
                'general.globalHotkeyExclusionsDesc',
                'Automatically yields all Tools3000 global shortcuts when any listed app is focused, and restores them immediately on focus switch',
              )}
            >
              <HotkeyAppExclusionBar
                apps={globalExcludedApps}
                compact
                emptyText={t('hotkeyExclusion.emptyGlobal', 'No global do-not-disturb apps configured')}
                pickerTitle={t('hotkeyExclusion.pickerGlobalTitle', 'Pick App to Auto-Yield All Global Shortcuts')}
                onChange={(nextApps) => void handleSaveGlobalExclusions(nextApps)}
              />
            </SettingRow>
          </div>
        </Card>
      </SettingGroup>

      {editingExclusionHotkey && (
        <Modal
          open
          title={t('hotkeyExclusion.modalTitle', 'Auto-Yield Apps — {{name}}', {
            name: resolveHotkeyLabel(t, editingExclusionHotkey.name),
          })}
          onClose={() => setEditingExclusionHotkey(null)}
          footer={
            <Button variant="primary" onClick={() => setEditingExclusionHotkey(null)}>
              {t('common.done', 'Done')}
            </Button>
          }
        >
          <p className="general-page__exclusion-modal-desc">
            {t(
              'hotkeyExclusion.modalDesc',
              'When any listed process is in the foreground, only this shortcut is released to the active app while all other Tools3000 shortcuts stay active.',
            )}
          </p>
          <HotkeyAppExclusionBar
            apps={editingExclusionHotkey.excludedApps ?? []}
            emptyText={t('hotkeyExclusion.emptyPerHotkey', 'Active in all apps; add process names to auto-yield')}
            pickerTitle={t('hotkeyExclusion.pickerPerHotkeyTitle', 'Pick App to Auto-Yield This Shortcut')}
            onChange={(nextApps) => void handleSavePerHotkeyExclusions(editingExclusionHotkey.name, nextApps)}
          />
        </Modal>
      )}

      {/* ── 4. 版本更新与新手引导 ───────────────────────────────────── */}
      <SettingGroup title={t('general.updatesAndGuide')} icon={<Sparkles size={20} strokeWidth={2.5} />}>
        <Card>
          <Toggle
            id="checkUpdates"
            label={t('general.checkUpdates')}
            description={t('general.checkUpdatesDesc')}
            checked={settings.checkUpdates}
            onChange={(v) => updateSetting('checkUpdates', v)}
          />
          <SubSettingGroup disabled={!settings.checkUpdates}>
            <Toggle
              id="autoDownloadUpdates"
              label={t('general.autoDownloadUpdates', 'Auto-download updates')}
              description={t('general.autoDownloadUpdatesDesc', 'Automatically download and verify SHA-256 checksums in the background when a new version is found')}
              checked={settings.autoDownloadUpdates ?? true}
              onChange={(v) => updateSetting('autoDownloadUpdates', v)}
              disabled={!settings.checkUpdates}
            />
          </SubSettingGroup>
          <Toggle
            id="showChangelogOnUpdate"
            label={t('general.showChangelogOnUpdate')}
            description={t('general.showChangelogOnUpdateDesc')}
            checked={settings.showChangelogOnUpdate ?? true}
            onChange={(v) => {
              void updateSetting('showChangelogOnUpdate', v);
              window.dispatchEvent(new CustomEvent<boolean>('tools3000:changelog-pref-changed', { detail: v }));
            }}
          />
          <SettingRow label={t('general.reopenOnboardingLabel')} description={t('general.reopenOnboardingDesc')}>
            <Button variant="secondary" onClick={handleReopenOnboarding}>
              <Sparkles size={15} style={{ marginRight: 6 }} />
              {t('general.reopenOnboardingBtn')}
            </Button>
          </SettingRow>
        </Card>
      </SettingGroup>

      {/* ── 5. 配置备份、同步与迁移 ─────────────────────────────────── */}
      <SettingGroup title={t('general.backupAndMigration')} icon={<Database size={20} strokeWidth={2.5} />}>
        <Card>
          <SettingRow
            label={t('general.portableMode')}
            description={t('general.portableModeDesc')}
            layout="vertical"
          >
            <div className="general-page__portable-bar">
              <span className={`general-page__mode-pill ${settings.isPortableMode ? 'is-portable' : ''}`}>
                {settings.isPortableMode ? (
                  <>
                    <Package size={13} strokeWidth={2} />
                    <span>{t('general.portableModeActive')}</span>
                  </>
                ) : (
                  <>
                    <HardDrive size={13} strokeWidth={2} />
                    <span>{t('general.portableModeStandard')}</span>
                  </>
                )}
              </span>
              {settings.dataDirectory && (
                <div className="general-page__data-dir-actions">
                  <CodeBadge title={settings.dataDirectory}>{settings.dataDirectory}</CodeBadge>
                  <Button
                    variant="ghost"
                    size="sm"
                    onClick={() => void bridgeRequest('system.openFolder', { path: settings.dataDirectory })}
                    title={t('general.openDataDirBtn')}
                  >
                    <FolderOpen size={14} />
                    <span>{t('general.openDataDirBtn')}</span>
                  </Button>
                </div>
              )}
            </div>
          </SettingRow>
        </Card>

        <CloudBackupCard onConfigRestored={() => void refreshAllAfterRestore()} />

        <Card>
          <SettingRow label={t('general.localConfigTransfer')} description={t('general.localConfigTransferDesc')}>
            <div className="general-page__inline-actions">
              <Button variant="secondary" onClick={handleExportConfig}>
                <Download size={15} />
                <span>{t('common.export')}</span>
              </Button>
              <Button variant="secondary" onClick={handleImportConfig}>
                <Upload size={15} />
                <span>{t('common.import')}</span>
              </Button>
            </div>
          </SettingRow>
        </Card>
      </SettingGroup>

      {/* ── 6. 诊断日志与排障 ───────────────────────────────────────── */}
      <SettingGroup title={t('general.diagnosticsAndLogs')} icon={<Settings size={20} strokeWidth={2.5} />}>
        <Card>
          <Toggle
            id="autoReleaseSettingsMemory"
            label={t('general.autoReleaseSettingsMemory')}
            description={t('general.autoReleaseSettingsMemoryDesc')}
            checked={settings.autoReleaseSettingsMemory ?? true}
            onChange={(v) => updateSetting('autoReleaseSettingsMemory', v)}
          />

          {onNavigate && (
            <SettingRow
              label={t('performancePage.lifecycleGroup')}
              description={t('performancePage.lifecycleSubtitle')}
            >
              <Button variant="secondary" onClick={() => onNavigate('performance')}>
                <MemoryStick size={15} />
                <span>{t('general.viewMemoryLifecycleBtn')}</span>
                <ArrowUpRight size={13} />
              </Button>
            </SettingRow>
          )}

          <SettingRow label={t('general.logLevel')} description={t('general.logLevelDesc')}>
            <Select
              id="log-level"
              value={settings.logLevel}
              onChange={(v) => updateSetting('logLevel', v)}
              options={[
                { value: 'trace', label: 'Trace' },
                { value: 'debug', label: 'Debug' },
                { value: 'info', label: 'Info' },
                { value: 'warn', label: 'Warning' },
                { value: 'error', label: 'Error' },
              ]}
            />
          </SettingRow>

          <SettingRow label={t('general.logRetentionDays')} description={t('general.logRetentionDaysDesc')}>
            <Select
              id="log-retention-days"
              value={String(settings.logRetentionDays ?? 7)}
              onChange={(v) => updateSetting('logRetentionDays', Number(v))}
              options={[
                { value: '3', label: t('general.retention3Days', '3 Days') },
                { value: '7', label: t('general.retention7Days', '7 Days (Recommended)') },
                { value: '14', label: t('general.retention14Days', '14 Days') },
                { value: '30', label: t('general.retention30Days', '30 Days') },
                { value: '0', label: t('general.retentionForever', 'Keep Forever') },
              ]}
            />
          </SettingRow>

          <SettingRow label={t('general.openLogDir', 'Log Directory')} description={t('general.openLogDirDesc', 'Open the folder containing diagnostic and troubleshooting logs')}>
            <Button
              variant="secondary"
              onClick={() => {
                void bridgeRequest('app.openLogDir');
              }}
            >
              <FolderOpen size={15} style={{ marginRight: 6 }} />
              {t('general.openLogDirBtn', 'Open Log Directory')}
            </Button>
          </SettingRow>

          <SettingRow label={t('general.exportLogs', 'Export Diagnostic Logs')} description={t('general.exportLogsDesc', 'Export all diagnostic logs and system environment reports as a file')}>
            <Button
              variant="secondary"
              onClick={async () => {
                try {
                  const res = await bridgeRequest<{ success: boolean; cancelled?: boolean; error?: string }>('app.exportLogs');
                  if (res.cancelled) return;
                  if (!res.success) throw new Error(res.error || t('general.exportFailed', 'Export failed'));
                  toast.success(t('general.exportLogsSuccess', 'Diagnostic logs exported and highlighted successfully'));
                } catch (e) {
                  toast.error(t('general.exportLogsFailed', 'Failed to export diagnostic logs'), { description: String(e) });
                }
              }}
            >
              <Download size={15} style={{ marginRight: 6 }} />
              {t('general.exportLogsBtn', 'Export Logs')}
            </Button>
          </SettingRow>
        </Card>
      </SettingGroup>

      {/* ── 7. 重置与恢复（沉底危险区防误触） ──────────────────────── */}
      <SettingGroup title={t('general.dangerZone')} icon={<AlertTriangle size={20} strokeWidth={2.5} />}>
        <Card className="general-page__danger-card">
          <SettingRow label={t('general.resetConfig')} description={t('general.resetConfigDesc')}>
            <Button variant="danger" onClick={() => setConfirmResetOpen(true)}>
              <RotateCcw size={16} />
              <span>{t('common.reset')}</span>
            </Button>
          </SettingRow>
        </Card>
      </SettingGroup>

      <ConfirmModal
        open={confirmResetOpen}
        title={t('general.resetConfig')}
        description={t('general.resetConfirmMsg')}
        confirmText={t('common.reset')}
        onConfirm={handleResetConfig}
        onClose={() => setConfirmResetOpen(false)}
      />
    </div>
  );
};

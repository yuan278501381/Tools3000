/* ─────────────────────────────────────────────────────────────────────────────
 * GeneralPage — 通用设置页
 *
 * 从 C++ ConfigManager 加载设置，修改后通过 IPC 实时保存。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { Card, Toggle, SettingRow, SettingGroup, Select, Button } from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { Settings, Zap, Globe, Database, Keyboard, Download, Upload, RotateCcw, RefreshCw, CheckCircle2, AlertTriangle, AlertOctagon, FolderOpen, Disc, MinusCircle, Package, HardDrive } from 'lucide-react';
import './GeneralPage.css';

interface GeneralSettings {
  autoStart: boolean;
  runAsAdmin: boolean;
  elevated?: boolean;
  minimizeToTray: boolean;
  checkUpdates: boolean;
  autoReleaseSettingsMemory?: boolean;
  showOnboarding?: boolean;
  isPortableMode?: boolean;
  dataDirectory?: string;
  language: string;
  logLevel: string;
  logRetentionDays?: number;
  theme: string;
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

export const GeneralPage: FC = () => {
  const [settings, setSettings] = useState<GeneralSettings>({
    autoStart: false,
    runAsAdmin: true,
    elevated: false,
    minimizeToTray: true,
    checkUpdates: true,
    autoReleaseSettingsMemory: true,
    language: 'auto',
    logLevel: 'info',
    logRetentionDays: 7,
    theme: 'dark',
    trayIconTheme: 'system',
  });
  const [loading, setLoading] = useState(true);
  const [refreshingHotkeys, setRefreshingHotkeys] = useState(false);
  const [isRestartingElevated, setIsRestartingElevated] = useState(false);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [accent, setAccent] = useState<string>(() => {
    try {
      return localStorage.getItem('tools3000:accent-color') || 'blue';
    } catch {
      return 'blue';
    }
  });

  const { t, i18n } = useTranslation();

  const handleAccentChange = (id: string) => {
    setAccent(id);
    try {
      localStorage.setItem('tools3000:accent-color', id);
    } catch (e) {
      void e;
    }
    bridgeRequest<{ success: boolean }>('general.updateSettings', { accentColor: id }).catch(console.error);
    window.dispatchEvent(new CustomEvent('tools3000:accent-changed', { detail: id }));
  };

  // 初始化获取设置
  useEffect(() => {
    let cancelled = false;
    Promise.all([
      bridgeRequest<GeneralSettings>('general.getSettings'),
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll'),
    ]).then(([res, hotkeyData]) => {
      if (cancelled) return;
      setSettings(prev => ({ ...prev, ...res }));
      const lang = res.language;
      if (lang && lang !== 'auto' && i18n.language !== lang) {
        void i18n.changeLanguage(lang);
      }
      setHotkeys(Array.isArray(hotkeyData) ? hotkeyData : []);
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

  // 监听顶栏或全局发出的语言、主题与强调色变更事件，保持页面双向数据同步
  useEffect(() => {
    const handleThemeEvent = (e: Event) => {
      const pref = (e as CustomEvent<string>).detail;
      if (pref) setSettings(prev => ({ ...prev, theme: pref }));
    };
    const handleLangEvent = (e: Event) => {
      const lang = (e as CustomEvent<string>).detail;
      if (lang) setSettings(prev => ({ ...prev, language: lang }));
    };
    const handleAccentEvent = (e: Event) => {
      const newAccent = (e as CustomEvent<string>).detail;
      if (newAccent) setAccent(newAccent);
    };
    window.addEventListener('tools3000:theme-changed', handleThemeEvent);
    window.addEventListener('tools3000:language-changed', handleLangEvent);
    window.addEventListener('tools3000:accent-changed', handleAccentEvent);
    return () => {
      window.removeEventListener('tools3000:theme-changed', handleThemeEvent);
      window.removeEventListener('tools3000:language-changed', handleLangEvent);
      window.removeEventListener('tools3000:accent-changed', handleAccentEvent);
    };
  }, []);

  // 保存单个设置项
  const updateSetting = useCallback(async <K extends keyof GeneralSettings,>(key: K, value: GeneralSettings[K]) => {
    const previous = settings[key];
    setSettings(prev => ({ ...prev, [key]: value }));

    const applyLocalValue = (localValue: GeneralSettings[K]) => {
      if (key === 'language') {
        const langValue = String(localValue);
        try {
          localStorage.setItem('tools3000:language', langValue);
        } catch (e) {
          void e;
        }
        if (langValue === 'auto') void i18n.changeLanguage(navigator.language.toLowerCase().startsWith('zh') ? 'zh' : 'en');
        else void i18n.changeLanguage(langValue.startsWith('zh') ? 'zh' : 'en');
        window.dispatchEvent(new CustomEvent('tools3000:language-changed', { detail: langValue }));
      } else if (key === 'theme') {
        window.dispatchEvent(new CustomEvent('tools3000:theme-changed', { detail: localValue }));
      }
    };
    applyLocalValue(value);

    try {
      const result = await bridgeRequest<OperationResult>('general.updateSettings', { [key]: value });
      if (!result.success) throw new Error(result.error || t('general.toastSaveFailed'));
    } catch (error) {
      setSettings(prev => prev[key] === value ? { ...prev, [key]: previous } : prev);
      applyLocalValue(previous);
      toast.error(t('general.toastSaveFailed'), { description: String(error) });
    }
  }, [i18n, settings, t]);

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
        await bridgeRequest('app.restartDemoted');
        return;
      }
    } catch (error) {
      setSettings(prev => ({ ...prev, runAsAdmin: previous }));
      setIsRestartingElevated(false);
      toast.error(t('general.toastSaveFailed'), { description: String(error) });
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

  const handleImportConfig = async () => {
    try {
      const result = await bridgeRequest<OperationResult>('config.import');
      if (result.cancelled) return;
      if (!result.success) throw new Error(result.error || t('general.importFailed'));
      toast.success(t('general.importSuccess'));
      const refreshed = await bridgeRequest<GeneralSettings>('general.getSettings');
      setSettings(prev => ({ ...prev, ...refreshed }));
      if (refreshed.language === 'auto') void i18n.changeLanguage(navigator.language);
      else void i18n.changeLanguage(refreshed.language);
      window.dispatchEvent(new CustomEvent('tools3000:theme-changed', { detail: refreshed.theme }));
    } catch (e) {
      toast.error(t('general.importFailed'), { description: String(e) });
    }
  };

  const handleResetConfig = async () => {
    if (!window.confirm(t('general.resetConfirmMsg'))) return;
    try {
      const result = await bridgeRequest<OperationResult>('config.reset');
      if (!result.success) throw new Error(result.error || t('general.resetFailed'));
      toast.success(t('general.resetSuccess'));
      // 重新加载设置
      const res = await bridgeRequest<GeneralSettings>('general.getSettings');
      setSettings(prev => ({ ...prev, ...res }));
      if (res.language === 'auto') void i18n.changeLanguage(navigator.language);
      else void i18n.changeLanguage(res.language);
      window.dispatchEvent(new CustomEvent('tools3000:theme-changed', { detail: res.theme }));
    } catch (e) {
      toast.error(t('general.resetFailed'), { description: String(e) });
    }
  };

  // ── 刷新快捷键状态 ───────────────────────────────────────────────────────
  const refreshHotkeys = useCallback(async () => {
    setRefreshingHotkeys(true);
    try {
      const data = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      setHotkeys(Array.isArray(data) ? data : []);
      toast.success(t('general.recheckShortcuts'));
    } catch {
      toast.error(t('general.loadFailed'));
    } finally {
      setRefreshingHotkeys(false);
    }
  }, [t]);

  // ── 快捷键名称映射 ──────────────────────────────────────────────────────
  const hotkeyNameMap: Record<string, string> = {
    Screenshot: t('onboarding.shortcutCapture'),
    Record: t('onboarding.shortcutRecord'),
    'Record Pause': t('recording.pauseShortcut'),
    OCR: t('onboarding.shortcutOcr'),
    'Pause Gestures': t('onboarding.shortcutGesturePause'),
    'Toggle Search': t('search.title'),
    'Translate Selection': t('general.shortcutTranslateSelection'),
    'Pin Toggle': t('general.shortcutPinToggle'),
    'Pin Paste': t('general.shortcutPinPaste'),
    'Pin Hide All': t('general.shortcutPinHideAll'),
    'Pin Arrange': t('general.shortcutPinArrange'),
    'Mute System Audio': t('general.shortcutMuteSystemAudio'),
    'Mute Microphone': t('general.shortcutMuteMicrophone'),
    capture: t('onboarding.shortcutCapture'),
    recording: t('onboarding.shortcutRecord'),
    ocr: t('onboarding.shortcutOcr'),
    gesturePause: t('onboarding.shortcutGesturePause'),
  };

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
      setHotkeys(Array.isArray(refreshed) ? refreshed : []);
      toast.success(`${hotkeyNameMap[entry.name] ?? entry.name}: ${applied || t('general.shortcutDisabled')}`);
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
    return <div style={{ padding: '2rem', opacity: 0.5 }}>{t('common.loading')}</div>;
  }

  return (
    <div className="general-page" style={{ animation: 'fadeIn 0.3s ease' }}>
      <SettingGroup title={t('general.behavior')} icon={<Zap size={20} strokeWidth={2.5} />}>
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
            checked={settings.runAsAdmin || (settings.elevated ?? false)}
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
          <Toggle
            id="checkUpdates"
            label={t('general.checkUpdates')}
            description={t('general.checkUpdatesDesc')}
            checked={settings.checkUpdates}
            onChange={(v) => updateSetting('checkUpdates', v)}
          />
          <Toggle
            id="autoReleaseSettingsMemory"
            label={t('general.autoReleaseSettingsMemory')}
            description={t('general.autoReleaseSettingsMemoryDesc')}
            checked={settings.autoReleaseSettingsMemory ?? true}
            onChange={(v) => updateSetting('autoReleaseSettingsMemory', v)}
          />
          <Toggle
            id="showOnboarding"
            label={t('general.showOnboarding')}
            description={t('general.showOnboardingDesc')}
            checked={settings.showOnboarding ?? false}
            onChange={(v) => updateSetting('showOnboarding', v)}
          />
        </Card>
      </SettingGroup>

      <SettingGroup title={t('general.uiAndLang')} icon={<Globe size={20} strokeWidth={2.5} />}>
        <Card>
          <SettingRow label={t('general.language')} description={t('general.languageDesc')}>
            <Select
              id="language"
              value={settings.language}
              onChange={(v) => updateSetting('language', v)}
              options={[
                { value: 'auto', label: t('general.langAuto') },
                { value: 'zh-CN', label: t('general.langZh') },
                { value: 'en-US', label: t('general.langEn') }
              ]}
            />
          </SettingRow>

          <SettingRow label={t('general.theme')} description={t('general.themeDesc')}>
            <Select
              id="theme"
              value={settings.theme}
              onChange={(v) => updateSetting('theme', v)}
              options={[
                { value: 'system', label: t('general.themeSystem') },
                { value: 'light', label: t('general.themeLight') },
                { value: 'dark', label: t('general.themeDark') }
              ]}
            />
          </SettingRow>

          <SettingRow label={t('general.trayIconTheme')} description={t('general.trayIconThemeDesc')}>
            <Select
              id="trayIconTheme"
              value={settings.trayIconTheme || 'system'}
              onChange={(v) => updateSetting('trayIconTheme', v)}
              options={[
                { value: 'system', label: t('general.trayIconThemeSystem') },
                { value: 'light', label: t('general.trayIconThemeLight') },
                { value: 'dark', label: t('general.trayIconThemeDark') },
                { value: 'color', label: t('general.trayIconThemeColor') }
              ]}
            />
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
                    className={`general-accent-btn ${isSelected ? 'active' : ''}`}
                    onClick={() => handleAccentChange(preset.id)}
                    style={{ '--accent-dot-color': preset.color } as React.CSSProperties}
                    title={label}
                  >
                    <span
                      className="general-accent-dot"
                      style={{ color: preset.color, backgroundColor: preset.color }}
                    />
                    <span>{label}</span>
                  </button>
                );
              })}
            </div>
          </SettingRow>
        </Card>
      </SettingGroup>

      <SettingGroup title={t('general.advanced')} icon={<Settings size={20} strokeWidth={2.5} />}>
        <Card>
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

      {/* ── 快捷键总览与冲突检测 ─────────────────────────────────── */}
      <SettingGroup title={t('general.keyboardShortcuts')} icon={<Keyboard size={20} strokeWidth={2.5} />}>
        <Card>
          {/* 快捷键健康度统计条 */}
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
                  <Disc size={12} /> {t('general.hotkeysSessionCount', 'Recording Only {{count}} items', { count: sessionOnlyCount })}
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
              title={t('general.recheckShortcuts')}
            >
              <RefreshCw size={12} className={refreshingHotkeys ? 'general-page__spin' : ''} />
              <span>{t('general.recheckShortcuts')}</span>
            </Button>
          </div>

          {hotkeys.length === 0 ? (
            <div className="general-page__empty">{t('general.noShortcuts')}</div>
          ) : (
            <div className="general-page__hotkey-list">
              {hotkeys.map((hk) => {
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
                          {hotkeyNameMap[hk.name] ?? hk.name}
                        </span>
                        <HotkeyStatusBadge entry={hk} />
                      </div>
                    </div>
                    <div className="general-page__hotkey-control">
                      <HotkeyRecorder
                        id={`general-hotkey-${hk.name.replace(/\s+/g, '-').toLowerCase()}`}
                        value={hk.shortcut}
                        ariaLabel={hotkeyNameMap[hk.name] ?? hk.name}
                        onChange={(value) => void rebindHotkey(hk, value)}
                      />
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </Card>
      </SettingGroup>

      {/* ── 数据管理 ────────────────────────────────────────────── */}
      <SettingGroup title={t('general.dataManagement')} icon={<Database size={20} strokeWidth={2.5} />}>
        <Card>
          <SettingRow
            label={t('general.portableMode')}
            description={t('general.portableModeDesc')}
            layout="vertical"
          >
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', width: '100%', gap: '12px', marginTop: '6px' }}>
              <span style={{
                display: 'inline-flex',
                alignItems: 'center',
                gap: '6px',
                padding: '4px 10px',
                borderRadius: '6px',
                fontSize: '0.83rem',
                fontWeight: 600,
                backgroundColor: settings.isPortableMode ? 'rgba(16, 185, 129, 0.15)' : 'rgba(59, 130, 246, 0.12)',
                color: settings.isPortableMode ? '#10b981' : '#3b82f6',
                border: `1px solid ${settings.isPortableMode ? 'rgba(16, 185, 129, 0.3)' : 'rgba(59, 130, 246, 0.25)'}`
              }}>
                {settings.isPortableMode ? (
                  <>
                    <Package size={14} strokeWidth={2.2} />
                    <span>{t('general.portableModeActive')}</span>
                  </>
                ) : (
                  <>
                    <HardDrive size={14} strokeWidth={2.2} />
                    <span>{t('general.portableModeStandard')}</span>
                  </>
                )}
              </span>
              {settings.dataDirectory && (
                <span style={{ fontSize: '0.83rem', color: 'var(--text-secondary)', fontFamily: 'var(--font-mono)' }} title={settings.dataDirectory}>
                  {settings.dataDirectory}
                </span>
              )}
            </div>
          </SettingRow>
          <SettingRow label={t('general.exportConfig')} description={t('general.exportConfigDesc')}>
            <Button variant="ghost" onClick={handleExportConfig}>
              <Download size={16} />
              <span>{t('common.export')}</span>
            </Button>
          </SettingRow>
          <SettingRow label={t('general.importConfig')} description={t('general.importConfigDesc')}>
            <Button variant="ghost" onClick={handleImportConfig}>
              <Upload size={16} />
              <span>{t('common.import')}</span>
            </Button>
          </SettingRow>
          <SettingRow label={t('general.resetConfig')} description={t('general.resetConfigDesc')}>
            <Button variant="danger" onClick={handleResetConfig}>
              <RotateCcw size={16} />
              <span>{t('common.reset')}</span>
            </Button>
          </SettingRow>
        </Card>
      </SettingGroup>
    </div>
  );
};

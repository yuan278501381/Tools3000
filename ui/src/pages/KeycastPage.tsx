/* ─────────────────────────────────────────────────────────────────────────────
 * KeycastPage.tsx — 按键回显与屏幕演示配置中心 (世界级 UX & 2D 时序流)
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useRef, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Card,
  Toggle,
  SettingRow,
  SettingGroup,
  SubSettingGroup,
  DisabledCascade,
  Button,
  NumberInput,
  ColorPickerInput,
  PageSkeleton,
} from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import { toast } from 'sonner';
import {
  RotateCcw,
  Play,
  Sparkles,
  Palette,
  Filter,
  Layers,
  Monitor,
} from 'lucide-react';
import './KeycastPage.css';

interface KeycastSettings {
  enabled: boolean;
  autoBypassFullscreen: boolean;
  filterMode: 'smart_shortcuts' | 'with_single_modifiers' | 'all_keys';
  includeFunctionKeys: boolean;
  position: 'bottom_left' | 'bottom_center' | 'bottom_right' | 'top_left' | 'top_center' | 'top_right';
  mergeRecentKeys: boolean;
  mergeTimeoutMs: number;
  displayDurationMs: number;
  fontSize: number;
  opacity: number;
  textColor: string;
  backgroundColor: string;
  modifierKeycapColor: string;
  modifierKeycapOpacity: number;
  modifierTextColor: string;
  firstKeyAnim: 'slide' | 'pop' | 'fade' | 'none';
  subsequentKeyAnim: 'pop' | 'slide' | 'fade' | 'none';
  rowCascadeAnim: boolean;
  exitDriftAnim: boolean;
}

const DEFAULT_SETTINGS: KeycastSettings = {
  enabled: true,
  autoBypassFullscreen: true,
  filterMode: 'smart_shortcuts',
  includeFunctionKeys: false,
  position: 'top_left',
  mergeRecentKeys: true,
  mergeTimeoutMs: 1200,
  displayDurationMs: 2500,
  fontSize: 36,
  opacity: 80,
  textColor: '#ffffff',
  backgroundColor: '#1c1c22',
  modifierKeycapColor: 'auto',
  modifierKeycapOpacity: 50,
  modifierTextColor: 'auto',
  firstKeyAnim: 'slide',
  subsequentKeyAnim: 'fade',
  rowCascadeAnim: true,
  exitDriftAnim: true,
};

const ACCENT_COLOR_MAP: Record<string, string> = {
  blue: '#3b82f6',
  cyan: '#06b6d4',
  amber: '#f59e0b',
  mint: '#10b981',
  coral: '#f43f5e',
  violet: '#8b5cf6',
};

const WindowsLogoIcon: FC<{ size?: number; className?: string }> = ({ size = 11, className = '' }) => (
  <svg
    width={size}
    height={size}
    viewBox="0 0 16 16"
    fill="currentColor"
    className={className}
    aria-hidden="true"
    style={{ display: 'inline-block', verticalAlign: 'middle' }}
  >
    <path d="M0 2.276L6.545 1.38v6.177H0V2.276zm0 5.869h6.545v6.178L0 13.447V8.145zm7.455-6.848L16 0v7.557H7.455V1.297zm8.545 7.424V16l-8.545-1.297V8.721H16z" />
  </svg>
);

interface ColorSegmentControlProps {
  label: string;
  desc?: string;
  value: string;
  defaultCustomFallback: string;
  brandAccentHex: string;
  autoMode?: 'brand' | 'contrast';
  onChange: (val: string) => void;
}

const ColorSegmentControl: FC<ColorSegmentControlProps> = ({
  label,
  desc,
  value,
  defaultCustomFallback,
  brandAccentHex,
  autoMode = 'brand',
  onChange,
}) => {
  const { t } = useTranslation();
  const [liveHex, setLiveHex] = useState<string | null>(null);
  const isAuto = value === 'auto';
  const effectiveCustomHex = liveHex ?? value;
  const displayHex = isAuto
    ? (autoMode === 'contrast' ? t('keycast.autoContrastBadge', 'AUTO') : brandAccentHex)
    : effectiveCustomHex;
  const swatchStyle = isAuto && autoMode === 'contrast'
    ? { background: 'linear-gradient(135deg, #ffffff 0%, #ffffff 50%, #111827 50%, #111827 100%)' }
    : { backgroundColor: isAuto ? brandAccentHex : effectiveCustomHex };
  const autoLabelText = autoMode === 'contrast'
    ? t('keycast.autoContrastLabel', 'Smart High Contrast')
    : t('keycast.followBrandAccent', 'Follow Theme Accent');
  const restoreTitleText = autoMode === 'contrast'
    ? t('keycast.restoreAutoContrastDesc', 'Automatically pick crisp white or black text based on keycap background luminance')
    : t('keycast.restoreFollowBrandDesc', 'Switch back and link dynamically to Tools3000 theme accent color');
  const restoreBtnText = autoMode === 'contrast'
    ? t('keycast.restoreAutoContrast', 'Smart Contrast')
    : t('keycast.restoreFollowBrand', 'Follow Theme Accent');

  return (
    <div className="keycast-page__prop-card keycast-page__color-card">
      <div className="keycast-page__prop-header">
        <span className="keycast-page__prop-title">{label}</span>
        {desc && <span className="keycast-page__prop-desc">{desc}</span>}
      </div>

      {/* 双态胶囊选择器 */}
      <div className="keycast-page__capsule-wrap">
        <button
          type="button"
          className={`keycast-page__capsule-btn ${isAuto ? 'active' : ''}`}
          onClick={() => {
            setLiveHex(null);
            onChange('auto');
          }}
        >
          <Sparkles size={13} aria-hidden="true" />
          <span>{autoLabelText}</span>
          <span
            className="keycast-page__capsule-dot"
            style={
              autoMode === 'contrast'
                ? { background: 'linear-gradient(135deg, #ffffff 50%, #111827 50%)', border: '1px solid var(--card-border)' }
                : { backgroundColor: brandAccentHex }
            }
          />
        </button>

        <button
          type="button"
          className={`keycast-page__capsule-btn ${!isAuto ? 'active' : ''}`}
          onClick={() => {
            if (isAuto) {
              setLiveHex(null);
              onChange(defaultCustomFallback);
            }
          }}
        >
          <Palette size={13} aria-hidden="true" />
          <span>{t('keycast.customColor', 'Custom Color')}</span>
        </button>
      </div>

      {/* 色彩展示与调色盘操作 */}
      <div className="keycast-page__prop-body">
        <div className="keycast-page__color-row">
          <div className="keycast-page__color-info">
            <span className="keycast-page__color-hex">{displayHex.toUpperCase()}</span>
            {!isAuto && (
              <button
                type="button"
                className="keycast-page__restore-capsule"
                onClick={() => {
                  setLiveHex(null);
                  onChange('auto');
                }}
                title={restoreTitleText}
              >
                <RotateCcw size={11} aria-hidden="true" />
                <span>{restoreBtnText}</span>
              </button>
            )}
          </div>

          <div className="keycast-page__color-picker-wrap">
            <div
              className="keycast-page__color-swatch"
              style={swatchStyle}
              title={isAuto ? autoLabelText : effectiveCustomHex}
            />
            {!isAuto && (
              <ColorPickerInput
                className="keycast-page__color-input"
                value={effectiveCustomHex}
                ariaLabel={label}
                onPreview={(hex) => setLiveHex(hex)}
                onChange={(hex) => {
                  setLiveHex(null);
                  onChange(hex);
                }}
              />
            )}
          </div>
        </div>
      </div>
    </div>
  );
};

export const KeycastPage: FC = () => {
  const { t } = useTranslation();
  const [settings, setSettings] = useState<KeycastSettings>(DEFAULT_SETTINGS);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [loading, setLoading] = useState(true);
  const [accent, setAccent] = useState<string>(() => {
    try {
      return localStorage.getItem('tools3000:accent-color') || 'blue';
    } catch {
      return 'blue';
    }
  });

  useEffect(() => {
    const handleAccent = (e: Event) => {
      const detail = (e as CustomEvent<string>).detail;
      if (detail) setAccent(detail);
    };
    window.addEventListener('tools3000:accent-changed', handleAccent);
    return () => window.removeEventListener('tools3000:accent-changed', handleAccent);
  }, []);

  useEffect(() => {
    let cancelled = false;
    void bridgeRequest<HotkeyEntry[]>('hotkey.getAll', undefined, { silent: true })
      .then((list) => {
        if (!cancelled && Array.isArray(list)) {
          setHotkeys(list);
        }
      })
      .catch(() => undefined);

    bridgeRequest<KeycastSettings>('keycast.getSettings')
      .then((res) => {
        if (!cancelled && res) {
          setSettings((prev) => ({ ...prev, ...res }));
        }
      })
      .catch((err) => {
        console.error('Failed to load keycast settings:', err);
        void Promise.allSettled([
          bridgeRequest<Array<{ id: string; enabled: boolean }>>('plugins.getAll'),
          bridgeRequest<boolean>('config.get', { key: '/plugins/keycast/enabled' }),
          bridgeRequest<boolean>('config.get', { key: '/keycast/enabled' }),
        ]).then(([pluginsRes, pluginCfgRes, legacyRes]) => {
          if (cancelled) return;
          let effectiveEnabled = true;
          if (pluginsRes.status === 'fulfilled' && Array.isArray(pluginsRes.value)) {
            const item = pluginsRes.value.find((p) => p.id === 'keycast');
            if (item && typeof item.enabled === 'boolean') effectiveEnabled = item.enabled;
          } else if (pluginCfgRes.status === 'fulfilled' && typeof pluginCfgRes.value === 'boolean') {
            effectiveEnabled = pluginCfgRes.value;
          } else if (legacyRes.status === 'fulfilled' && typeof legacyRes.value === 'boolean') {
            effectiveEnabled = legacyRes.value;
          }
          setSettings((prev) => ({ ...prev, enabled: effectiveEnabled }));
        });
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });

    return () => {
      cancelled = true;
    };
  }, []);

  useBridgeEvent('keycast:configChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      const nextEnabled = payload.enabled;
      setSettings((prev) => ({ ...prev, enabled: nextEnabled }));
    }
  });

  useBridgeEvent('plugins:changed', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { id?: string; enabled?: boolean }) : null;
    if (payload && payload.id === 'keycast' && typeof payload.enabled === 'boolean') {
      const nextEnabled = payload.enabled;
      setSettings((prev) => ({ ...prev, enabled: nextEnabled }));
    }
  });

  const saveSeqRef = useRef<Record<string, number>>({});

  const saveSetting = async <K extends keyof KeycastSettings>(key: K, value: KeycastSettings[K]) => {
    const seq = (saveSeqRef.current[String(key)] ?? 0) + 1;
    saveSeqRef.current[String(key)] = seq;
    const previous = settings[key];
    setSettings((prev) => ({ ...prev, [key]: value }));
    try {
      if (key === 'enabled') {
        const nextEnabled = Boolean(value);
        if (nextEnabled) {
          await bridgeRequest('plugins.setEnabled', { id: 'keycast', enabled: true });
          await bridgeRequest('keycast.updateSettings', { enabled: true });
        } else {
          await bridgeRequest('keycast.updateSettings', { enabled: false }).catch(() => {});
          await bridgeRequest('plugins.setEnabled', { id: 'keycast', enabled: false });
        }
      } else {
        await bridgeRequest('keycast.updateSettings', { [key]: value });
      }
    } catch (e) {
      if (saveSeqRef.current[String(key)] === seq) {
        setSettings((prev) => ({ ...prev, [key]: previous }));
        toast.error(t('keycast.saveFailed', 'Failed to save keycast settings'), { description: String(e) });
      }
    }
  };

  const handleRebindToggleShortcut = async (nextShortcut: string) => {
    try {
      const res = await bridgeRequest<{ success?: boolean; error?: string }>('hotkey.rebind', {
        name: 'Toggle Keycast',
        hotkey: nextShortcut,
      });
      if (!res?.success) {
        throw new Error(res?.error || t('hotkey.bindFailed'));
      }
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll', undefined, { silent: true });
      if (Array.isArray(refreshed)) setHotkeys(refreshed);
    } catch (error) {
      toast.error(t('hotkey.bindFailed'), { description: String(error) });
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll', undefined, { silent: true }).catch(() => null);
      if (Array.isArray(refreshed)) setHotkeys(refreshed);
    }
  };

  const handleResetDefaults = async () => {
    try {
      await bridgeRequest('keycast.resetDefaults', {});
      setSettings(DEFAULT_SETTINGS);
      toast.success(t('keycast.resetSuccess', 'Restored default keycast settings'));
    } catch (e) {
      toast.error(t('keycast.saveFailed', 'Failed to save keycast settings'), { description: String(e) });
    }
  };

  const handleTestKeycast = async () => {
    try {
      await bridgeRequest('keycast.trigger', {});
    } catch (e) {
      console.error('Failed to trigger keycast:', e);
    }
  };

  const currentBrandAccentHex = ACCENT_COLOR_MAP[accent] || '#3b82f6';
  const toggleKeycastEntry = hotkeys.find((h) => h.name === 'Toggle Keycast');

  if (loading) {
    return <PageSkeleton cards={3} rowsPerCard={3} />;
  }

  const POSITIONS: Array<{ id: KeycastSettings['position']; labelKey: string }> = [
    { id: 'top_left',      labelKey: 'keycast.posTopLeft' },
    { id: 'bottom_left',   labelKey: 'keycast.posBottomLeft' },
    { id: 'top_center',    labelKey: 'keycast.posTopCenter' },
    { id: 'bottom_center', labelKey: 'keycast.posBottomCenter' },
    { id: 'top_right',     labelKey: 'keycast.posTopRight' },
    { id: 'bottom_right',  labelKey: 'keycast.posBottomRight' },
  ];

  const FILTER_MODES: Array<{ id: KeycastSettings['filterMode']; titleKey: string; descKey: string }> = [
    {
      id: 'smart_shortcuts',
      titleKey: 'keycast.filterSmartShortcuts',
      descKey: 'keycast.filterSmartShortcutsDesc',
    },
    {
      id: 'with_single_modifiers',
      titleKey: 'keycast.filterWithModifiers',
      descKey: 'keycast.filterWithModifiersDesc',
    },
    {
      id: 'all_keys',
      titleKey: 'keycast.filterAllKeys',
      descKey: 'keycast.filterAllKeysDesc',
    },
  ];

  return (
    <div className="keycast-page">
      {/* ── 1. 主控与快速启停（首屏直达体验按钮与热键） ──────────────────── */}
      <Card
        title={t('keycast.title', 'Keycast')}
        headerAction={
          <Button variant="primary" size="sm" disabled={!settings.enabled} onClick={() => void handleTestKeycast()}>
            <Play size={14} aria-hidden="true" />
            <span>{t('keycast.testKeycast', 'Try Keycast Now')}</span>
          </Button>
        }
      >
        <Toggle
          id="keycast-main-enabled"
          label={t('keycast.mainToggle', 'Keycast')}
          description={t('keycast.mainToggleDesc', 'Render crystal keycap capsules on screen in real time.')}
          checked={settings.enabled}
          onChange={(v) => void saveSetting('enabled', v)}
        />

        <DisabledCascade disabled={!settings.enabled}>
          {toggleKeycastEntry && (
            <SettingRow
              label={
                <div className="keycast-page__row-label">
                  <span>{t('keycast.quickToggleShortcut', 'Toggle Keycast Overlay Shortcut')}</span>
                  <HotkeyStatusBadge entry={toggleKeycastEntry} />
                </div>
              }
              description={t(
                'keycast.quickToggleShortcutDesc',
                'Quickly enable or hide on-screen keystrokes during live demos or recordings',
              )}
            >
              <HotkeyRecorder
                id="keycast-toggle-shortcut"
                value={toggleKeycastEntry.shortcut}
                onChange={(v) => void handleRebindToggleShortcut(v)}
              />
            </SettingRow>
          )}

          <Toggle
            id="keycast-auto-bypass"
            label={t('keycast.autoBypassFullscreen', 'Auto Bypass on Fullscreen')}
            description={t('keycast.autoBypassFullscreenDesc', 'Automatically suppress keycast overlay when exclusive fullscreen apps (e.g. 3D games or videos) are active')}
            checked={settings.autoBypassFullscreen}
            onChange={(v) => void saveSetting('autoBypassFullscreen', v)}
          />
        </DisabledCascade>
      </Card>

      <DisabledCascade disabled={!settings.enabled} className="keycast-page__cascade">
        {/* ── 2. 按键过滤与回显策略 ──────────────────────────────────────── */}
        <SettingGroup title={t('keycast.filterSection', 'Filtering & Keystroke Policy')} icon={<Filter size={18} />}>
          <div className="keycast-page__filter-grid">
            {FILTER_MODES.map((mode) => {
              const isSelected = settings.filterMode === mode.id;
              return (
                <div
                  key={mode.id}
                  className={`keycast-page__filter-card ${isSelected ? 'active' : ''}`}
                  onClick={() => void saveSetting('filterMode', mode.id)}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter' || e.key === ' ') {
                      e.preventDefault();
                      void saveSetting('filterMode', mode.id);
                    }
                  }}
                  role="button"
                  tabIndex={0}
                  aria-pressed={isSelected}
                >
                  <div className="keycast-page__filter-header">
                    {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                    <span className="keycast-page__filter-title">{t(mode.titleKey as any)}</span>
                    <div className={`keycast-page__filter-radio ${isSelected ? 'active' : ''}`}>
                      {isSelected && <div className="keycast-page__filter-radio-inner" />}
                    </div>
                  </div>
                  {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                  <p className="keycast-page__filter-desc">{t(mode.descKey as any)}</p>
                </div>
              );
            })}
          </div>
          <Card>
            <Toggle
              id="keycast-include-func-keys"
              label={t('keycast.includeFunctionKeys', 'Include Standalone Functional Keys')}
              description={t('keycast.includeFunctionKeysDesc', 'Show standalone Space, Backspace, Delete, Enter, Tab, Arrow keys, and F1~F12; when disabled, only display them when combined with modifiers.')}
              checked={settings.includeFunctionKeys}
              onChange={(v) => void saveSetting('includeFunctionKeys', v)}
            />
          </Card>
        </SettingGroup>

        {/* ── 3. 屏幕出现位置 (纯保留交互式显示器沙盘，去除下方冗余重复药丸) ─ */}
        <SettingGroup title={t('keycast.positionSection', 'Screen Position')} icon={<Monitor size={18} />}>
          <div className="keycast-page__monitor-wrapper">
            <div className="keycast-page__monitor-screen">
              <div className="keycast-page__monitor-cam" />
              <div className="keycast-page__monitor-desktop">
                {POSITIONS.map((pos) => {
                  const isSelected = settings.position === pos.id;
                  return (
                    <button
                      key={pos.id}
                      type="button"
                      className={`keycast-page__monitor-hotspot pos-${pos.id} ${isSelected ? 'active' : ''}`}
                      onClick={() => void saveSetting('position', pos.id)}
                      // eslint-disable-next-line @typescript-eslint/no-explicit-any
                      title={t(pos.labelKey as any)}
                    >
                      {isSelected ? (
                        <div className="keycast-page__monitor-capsule">
                          <span className="keycast-page__monitor-capsule-key key-win">
                            <WindowsLogoIcon size={11} />
                            <span>Win</span>
                          </span>
                          <span className="keycast-page__monitor-capsule-plus">+</span>
                          <span className="keycast-page__monitor-capsule-key">E</span>
                        </div>
                      ) : (
                        // eslint-disable-next-line @typescript-eslint/no-explicit-any
                        <span className="keycast-page__monitor-hotspot-hint">{t(pos.labelKey as any)}</span>
                      )}
                    </button>
                  );
                })}

                <div className="keycast-page__monitor-taskbar">
                  <div className="keycast-page__monitor-taskbar-start" />
                  <div className="keycast-page__monitor-taskbar-icons">
                    <div className="keycast-page__monitor-taskbar-app" />
                    <div className="keycast-page__monitor-taskbar-app active" />
                    <div className="keycast-page__monitor-taskbar-app" />
                  </div>
                  <div className="keycast-page__monitor-taskbar-tray" />
                </div>
              </div>
            </div>

            <div className="keycast-page__monitor-stand" />
            <div className="keycast-page__monitor-base" />
          </div>
        </SettingGroup>

        {/* ── 4. 键帽视觉外观与配色 ──────────────────────────────────────── */}
        <SettingGroup
          title={t('keycast.appearanceSection', 'Keycap Visual Style & Colors')}
          icon={<Palette size={18} />}
        >
          <div className="keycast-page__grid">
            {/* 键帽字号 */}
            <div className="keycast-page__prop-card">
              <div className="keycast-page__prop-header">
                <span className="keycast-page__prop-title">{t('keycast.fontSize', 'Keycap Font Size')}</span>
                <span className="keycast-page__prop-desc">{t('keycast.fontSizeDesc', 'Base font size for keycast (px). Capsule trays, modifier keycaps, Windows logo and paddings scale dynamically in golden ratio.')}</span>
              </div>
              <div className="keycast-page__prop-body">
                <NumberInput
                  min={12}
                  max={64}
                  step={2}
                  unit="px"
                  value={settings.fontSize}
                  onChange={(v) => void saveSetting('fontSize', v)}
                  ariaLabel={t('keycast.fontSize', 'Keycap Font Size')}
                />
              </div>
            </div>

            {/* 胶囊背景透明度 */}
            <div className="keycast-page__prop-card">
              <div className="keycast-page__prop-header">
                <span className="keycast-page__prop-title">{t('keycast.opacity', 'Background Opacity')}</span>
                <span className="keycast-page__prop-desc">{t('keycast.opacityDesc', 'Opacity of the keycast capsule background (20%~100%). Lower values create a translucent ambient glass effect while keycap text remains 100% crisp and clear.')}</span>
              </div>
              <div className="keycast-page__prop-body">
                <NumberInput
                  min={20}
                  max={100}
                  step={5}
                  unit="%"
                  value={settings.opacity ?? 80}
                  onChange={(v) => void saveSetting('opacity', v)}
                  ariaLabel={t('keycast.opacity', 'Background Opacity')}
                />
              </div>
            </div>

            {/* 文字颜色 */}
            <ColorSegmentControl
              label={t('keycast.textColor', 'Text Color')}
              desc={t('keycast.textColorDesc', 'Color of the key text.')}
              value={settings.textColor}
              defaultCustomFallback="#F8FAFC"
              brandAccentHex={currentBrandAccentHex}
              onChange={(val) => void saveSetting('textColor', val)}
            />

            {/* 胶囊背景色 */}
            <ColorSegmentControl
              label={t('keycast.backgroundColor', 'Capsule Background')}
              desc={t('keycast.backgroundColorDesc', 'Color of the keycap capsule background.')}
              value={settings.backgroundColor}
              defaultCustomFallback="#1c1c22"
              brandAccentHex={currentBrandAccentHex}
              onChange={(val) => void saveSetting('backgroundColor', val)}
            />

            {/* 修饰键底色 */}
            <ColorSegmentControl
              label={t('keycast.modifierKeycapColor', 'Modifier Key Background')}
              desc={t('keycast.modifierKeycapColorDesc', 'Base background color for Ctrl / Alt / Win keys (follows theme accent by default).')}
              value={settings.modifierKeycapColor || 'auto'}
              defaultCustomFallback="#3b82f6"
              brandAccentHex={currentBrandAccentHex}
              onChange={(val) => void saveSetting('modifierKeycapColor', val)}
            />

            {/* 修饰键底色不透明度 */}
            <div className="keycast-page__prop-card">
              <div className="keycast-page__prop-header">
                <span className="keycast-page__prop-title">{t('keycast.modifierKeycapOpacity', 'Modifier Base Opacity')}</span>
                <span className="keycast-page__prop-desc">{t('keycast.modifierKeycapOpacityDesc', 'Background base opacity (0%~100%, only affects background, text remains crisp and clear).')}</span>
              </div>
              <div className="keycast-page__prop-body">
                <NumberInput
                  min={0}
                  max={100}
                  step={2}
                  unit="%"
                  value={settings.modifierKeycapOpacity ?? 50}
                  onChange={(v) => void saveSetting('modifierKeycapOpacity', v)}
                  ariaLabel={t('keycast.modifierKeycapOpacity', 'Modifier Base Opacity')}
                />
              </div>
            </div>

            {/* 修饰键文字颜色 */}
            <ColorSegmentControl
              label={t('keycast.modifierTextColor', 'Modifier Key Text Color')}
              desc={t('keycast.modifierTextColorDesc', 'Text and icon color for Ctrl / Alt / Win keys (smart adaptive contrast by default, white on dark, black on light).')}
              value={settings.modifierTextColor || 'auto'}
              defaultCustomFallback="#ffffff"
              brandAccentHex={currentBrandAccentHex}
              autoMode="contrast"
              onChange={(val) => void saveSetting('modifierTextColor', val)}
            />
          </div>
        </SettingGroup>

        {/* ── 5. 时序流与物理动效 ────────────────────────────────────────── */}
        <SettingGroup title={t('keycast.motionSection', 'Timeline Flow & Physics Easing')} icon={<Layers size={18} />}>
          <Card>
            <Toggle
              id="keycast-merge-recent"
              label={t('keycast.mergeRecentKeys', 'Horizontal In-Line Push')}
              description={t('keycast.mergeRecentKeysDesc', 'Push recent keystrokes in the same row from right to left with spring damping; push row upward on timeout')}
              checked={settings.mergeRecentKeys}
              onChange={(v) => void saveSetting('mergeRecentKeys', v)}
            />

            <SubSettingGroup disabled={!settings.mergeRecentKeys}>
              <SettingRow
                label={t('keycast.mergeTimeout', 'In-Line Push Timeout')}
                description={t('keycast.mergeTimeoutDesc', 'Time window (ms) to merge consecutive keystrokes into the same horizontal line.')}
              >
                <NumberInput
                  min={300}
                  max={5000}
                  step={100}
                  unit="ms"
                  disabled={!settings.mergeRecentKeys}
                  value={settings.mergeTimeoutMs || 1200}
                  onChange={(v) => void saveSetting('mergeTimeoutMs', v)}
                  ariaLabel={t('keycast.mergeTimeout', 'In-Line Push Timeout')}
                />
              </SettingRow>
            </SubSettingGroup>

            <Toggle
              id="keycast-row-cascade"
              label={t('keycast.rowCascadeAnim')}
              description={t('keycast.rowCascadeAnimDesc')}
              checked={settings.rowCascadeAnim}
              onChange={(v) => void saveSetting('rowCascadeAnim', v)}
            />
            <Toggle
              id="keycast-exit-drift"
              label={t('keycast.exitDriftAnim')}
              description={t('keycast.exitDriftAnimDesc')}
              checked={settings.exitDriftAnim}
              onChange={(v) => void saveSetting('exitDriftAnim', v)}
            />
          </Card>

          <div className="keycast-page__grid keycast-page__grid--three">
            {/* 首键进场动效 */}
            <div className="keycast-page__prop-card">
              <div className="keycast-page__prop-header">
                <span className="keycast-page__prop-title">{t('keycast.firstKeyAnim')}</span>
                <span className="keycast-page__prop-desc">{t('keycast.firstKeyAnimDesc')}</span>
              </div>
              <div className="keycast-page__capsule-wrap">
                {([
                  { id: 'slide', labelKey: 'keycast.animSlide', defaultLabel: 'Slide' },
                  { id: 'pop', labelKey: 'keycast.animPop', defaultLabel: 'Pop' },
                  { id: 'fade', labelKey: 'keycast.animFade', defaultLabel: 'Fade' },
                  { id: 'none', labelKey: 'keycast.animNone', defaultLabel: 'None' },
                ] as const).map((opt) => (
                  <button
                    key={opt.id}
                    type="button"
                    className={`keycast-page__capsule-btn ${settings.firstKeyAnim === opt.id ? 'active' : ''}`}
                    onClick={() => void saveSetting('firstKeyAnim', opt.id)}
                  >
                    {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                    {t(opt.labelKey as any, opt.defaultLabel)}
                  </button>
                ))}
              </div>
            </div>

            {/* 同排后续按键动效 */}
            <div className="keycast-page__prop-card">
              <div className="keycast-page__prop-header">
                <span className="keycast-page__prop-title">{t('keycast.subsequentKeyAnim')}</span>
                <span className="keycast-page__prop-desc">{t('keycast.subsequentKeyAnimDesc')}</span>
              </div>
              <div className="keycast-page__capsule-wrap">
                {([
                  { id: 'pop', labelKey: 'keycast.animPop', defaultLabel: 'Pop' },
                  { id: 'slide', labelKey: 'keycast.animSlide', defaultLabel: 'Slide' },
                  { id: 'fade', labelKey: 'keycast.animFade', defaultLabel: 'Fade' },
                  { id: 'none', labelKey: 'keycast.animNone', defaultLabel: 'None' },
                ] as const).map((opt) => (
                  <button
                    key={opt.id}
                    type="button"
                    className={`keycast-page__capsule-btn ${settings.subsequentKeyAnim === opt.id ? 'active' : ''}`}
                    onClick={() => void saveSetting('subsequentKeyAnim', opt.id)}
                  >
                    {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                    {t(opt.labelKey as any, opt.defaultLabel)}
                  </button>
                ))}
              </div>
            </div>

            {/* 停留时长 */}
            <div className="keycast-page__prop-card">
              <div className="keycast-page__prop-header">
                <span className="keycast-page__prop-title">{t('keycast.displayDuration', 'Hold Duration')}</span>
                <span className="keycast-page__prop-desc">{t('keycast.displayDurationDesc', 'Duration each keystroke capsule stays on screen (ms).')}</span>
              </div>
              <div className="keycast-page__prop-body">
                <NumberInput
                  min={500}
                  max={10000}
                  step={500}
                  unit="ms"
                  value={settings.displayDurationMs}
                  onChange={(v) => void saveSetting('displayDurationMs', v)}
                  ariaLabel={t('keycast.displayDuration', 'Hold Duration')}
                />
              </div>
            </div>
          </div>
        </SettingGroup>
      </DisabledCascade>

      {/* ── 6. 底部恢复默认栏 ────────────────────────────────────────────── */}
      <div className="keycast-page__action-footer">
        <Button variant="ghost" size="sm" onClick={() => void handleResetDefaults()} title={t('keycast.resetDefaults')}>
          <RotateCcw size={14} aria-hidden="true" />
          <span>{t('keycast.resetDefaults')}</span>
        </Button>
      </div>
    </div>
  );
};

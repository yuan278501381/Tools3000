/* ─────────────────────────────────────────────────────────────────────────────
 * CapsLayerPage.tsx — Caps 引导改键（Hyper 导航层）设置工作台
 *
 * 架构设计:
 *   1. 默认关闭（零底层键盘钩子），开启后将 CapsLock 升级为极客引导层
 *   2. 短按行为三选一（保留原生 CapsLock / 发送 Esc / 禁用短按），配合 Shift+CapsLock 保底切换大写
 *   3. 一键极客预设（I/J/K/L 方向键、Vim H/J/K/L、E/S/D/F 左手导航）
 *   4. 双栏 @container 作用域工作台：左侧「全局层 + 应用专属规则（复用 RunningWindowPicker）」，右侧键位映射表
 * ───────────────────────────────────────────────────────────────────────────── */

import { useCallback, useEffect, useMemo, useState, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  AppWindow,
  ArrowRight,
  Globe,
  Info,
  KeyRound,
  Keyboard,
  Loader2,
  Pencil,
  Plus,
  ShieldAlert,
  Sparkles,
  Trash2,
} from 'lucide-react';
import {
  Badge,
  Button,
  Card,
  CodeBadge,
  DisabledCascade,
  Field,
  Modal,
  PageSkeleton,
  SettingGroup,
  SettingRow,
  TextInput,
  Toggle,
} from '../components/UIKit';
import { RunningWindowPicker, type PickedWindowResult } from '../components/RunningWindowPicker';
import { HotkeyAppExclusionBar } from '../components/business/HotkeyAppExclusionBar';
import { normalizeProcessInput } from '../utils/processName';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import {
  DEFAULT_CAPS_LAYER_SETTINGS,
  type CapsAppProfile,
  type CapsKeyMapping,
  type CapsLayerSettings,
  type CapsPresetId,
  type CapsTapAction,
  type CapsTargetType,
} from '../types/capsLayer';
import './CapsLayerPage.css';

interface PresetSpec {
  id: CapsPresetId;
  labelKey: string;
  fallbackLabel: string;
  hintKey: string;
  fallbackHint: string;
  keysBadge: string;
}

const CAPS_PRESETS: readonly PresetSpec[] = [
  {
    id: 'arrows_ijkl',
    labelKey: 'capsLayer.presetIjklTitle',
    fallbackLabel: 'I / J / K / L Arrows',
    hintKey: 'capsLayer.presetIjklDesc',
    fallbackHint: 'Right-hand inverted-T navigation with Home/End & Backspace',
    keysBadge: 'I / J / K / L',
  },
  {
    id: 'arrows_vim',
    labelKey: 'capsLayer.presetVimTitle',
    fallbackLabel: 'Vim H / J / K / L',
    hintKey: 'capsLayer.presetVimDesc',
    fallbackHint: 'Classic Vim home-row cursor movement everywhere',
    keysBadge: 'H / J / K / L',
  },
  {
    id: 'arrows_esdf',
    labelKey: 'capsLayer.presetEsdfTitle',
    fallbackLabel: 'E / S / D / F Arrows',
    hintKey: 'capsLayer.presetEsdfDesc',
    fallbackHint: 'Left-hand navigation while keeping right hand on the mouse',
    keysBadge: 'E / S / D / F',
  },
];

const QUICK_TRIGGER_KEYS = ['I', 'J', 'K', 'L', 'H', 'U', 'O', 'E', 'S', 'D', 'F', 'Space', 'Backspace'] as const;
const QUICK_TARGET_KEYS = ['Up', 'Down', 'Left', 'Right', 'Home', 'End', 'PgUp', 'PgDn', 'Backspace', 'Delete', 'Esc', 'Enter'] as const;
const QUICK_TARGET_COMBOS = ['Ctrl+C', 'Ctrl+V', 'Ctrl+X', 'Ctrl+Z', 'Ctrl+A', 'Ctrl+W', 'Alt+Left', 'Alt+Right'] as const;

function normalizeCapsSettings(raw: unknown): CapsLayerSettings {
  if (!raw || typeof raw !== 'object') {
    return DEFAULT_CAPS_LAYER_SETTINGS;
  }
  const obj = raw as Partial<CapsLayerSettings>;
  const tapAction: CapsTapAction =
    obj.tapAction === 'esc' || obj.tapAction === 'none' || obj.tapAction === 'toggle_caps'
      ? obj.tapAction
      : 'toggle_caps';

  return {
    enabled: Boolean(obj.enabled),
    tapAction,
    shiftCapsTogglesNative: obj.shiftCapsTogglesNative ?? true,
    excludedApps: Array.isArray(obj.excludedApps)
      ? obj.excludedApps.filter((v): v is string => typeof v === 'string')
      : [],
    globalMappings: Array.isArray(obj.globalMappings) ? obj.globalMappings : [],
    appProfiles: Array.isArray(obj.appProfiles) ? obj.appProfiles : [],
  };
}

interface MappingDraft {
  id: string;
  key: string;
  targetType: CapsTargetType;
  targetValue: string;
  label: string;
  enabled: boolean;
}

function createEmptyMappingDraft(): MappingDraft {
  return {
    id: '',
    key: '',
    targetType: 'key',
    targetValue: '',
    label: '',
    enabled: true,
  };
}

export const CapsLayerPage: FC = () => {
  const { t } = useTranslation();
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [applyingPreset, setApplyingPreset] = useState<CapsPresetId | null>(null);
  const [settings, setSettings] = useState<CapsLayerSettings>(DEFAULT_CAPS_LAYER_SETTINGS);
  const [selectedScopeId, setSelectedScopeId] = useState<string>('global');

  // 添加应用规则弹窗状态
  const [addProfileOpen, setAddProfileOpen] = useState(false);
  const [profileNameDraft, setProfileNameDraft] = useState('');
  const [profileProcDraft, setProfileProcDraft] = useState('');

  // 添加 / 编辑键位映射弹窗状态
  const [mappingModalOpen, setMappingModalOpen] = useState(false);
  const [mappingDraft, setMappingDraft] = useState<MappingDraft>(createEmptyMappingDraft);
  const [mappingError, setMappingError] = useState('');

  const loadSettings = useCallback(async () => {
    try {
      const res = await bridgeRequest<CapsLayerSettings>('capsLayer.getSettings', undefined, { silent: true });
      if (res && typeof res === 'object') {
        setSettings(normalizeCapsSettings(res));
      }
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void loadSettings();
  }, [loadSettings]);

  useBridgeEvent('capsLayer:stateChanged', (raw: unknown) => {
    if (raw && typeof raw === 'object' && 'enabled' in raw) {
      setSettings(normalizeCapsSettings(raw));
    } else {
      void loadSettings();
    }
  });

  useBridgeEvent('caps_layer:stateChanged', (raw: unknown) => {
    if (raw && typeof raw === 'object' && 'enabled' in raw) {
      setSettings(normalizeCapsSettings(raw));
    } else {
      void loadSettings();
    }
  });

  const patchSettings = useCallback(
    async (patch: Partial<CapsLayerSettings>, showToast = false) => {
      const prev = settings;
      const optimistic: CapsLayerSettings = { ...settings, ...patch };
      setSettings(optimistic);
      setSaving(true);
      try {
        const res = await bridgeRequest<CapsLayerSettings>(
          'capsLayer.updateSettings',
          patch as Record<string, unknown>,
          { silent: true },
        );
        if (res && typeof res === 'object' && 'enabled' in res) {
          setSettings(normalizeCapsSettings(res));
        }
        if (showToast) {
          toast.success(t('general.toastSaved', 'Settings saved'));
        }
      } catch (err) {
        setSettings(prev);
        toast.error(t('capsLayer.saveFailed', 'Failed to save Caps Layer settings'), {
          description: String(err),
        });
      } finally {
        setSaving(false);
      }
    },
    [settings, t],
  );

  const handleApplyPreset = async (presetId: CapsPresetId) => {
    setApplyingPreset(presetId);
    try {
      const res = await bridgeRequest<CapsLayerSettings>(
        'capsLayer.applyPreset',
        { presetId },
        { silent: true },
      );
      if (res && typeof res === 'object' && 'globalMappings' in res) {
        setSettings(normalizeCapsSettings(res));
      }
      setSelectedScopeId('global');
      toast.success(t('capsLayer.presetAppliedToast', 'Preset applied to Global Layer'));
    } catch (err) {
      toast.error(t('capsLayer.presetApplyFailed', 'Failed to apply preset'), {
        description: String(err),
      });
    } finally {
      setApplyingPreset(null);
    }
  };

  const activeProfile: CapsAppProfile | null = useMemo(() => {
    if (selectedScopeId === 'global') return null;
    return settings.appProfiles.find((p) => p.id === selectedScopeId) ?? null;
  }, [selectedScopeId, settings.appProfiles]);

  const effectiveScopeId = activeProfile ? activeProfile.id : 'global';
  const currentMappings: CapsKeyMapping[] = activeProfile
    ? activeProfile.mappings
    : settings.globalMappings;

  const updateCurrentMappings = (nextMappings: CapsKeyMapping[]) => {
    if (activeProfile) {
      const nextProfiles = settings.appProfiles.map((p) =>
        p.id === activeProfile.id ? { ...p, mappings: nextMappings } : p,
      );
      void patchSettings({ appProfiles: nextProfiles });
    } else {
      void patchSettings({ globalMappings: nextMappings });
    }
  };

  // ── 应用规则管理 ──────────────────────────────────────────────────────────
  const handleWindowPick = (picked: PickedWindowResult) => {
    setProfileNameDraft(picked.name);
    setProfileProcDraft(picked.processName);
  };

  const handleConfirmAddProfile = () => {
    const normalizedProc = normalizeProcessInput(profileProcDraft);
    if (!normalizedProc) return;
    const existing = settings.appProfiles.find(
      (p) => p.processName.toLowerCase() === normalizedProc,
    );
    if (existing) {
      setSelectedScopeId(existing.id);
      setAddProfileOpen(false);
      return;
    }
    const created: CapsAppProfile = {
      id: `caps-app-${Date.now()}`,
      name: profileNameDraft.trim() || normalizedProc,
      processName: normalizedProc,
      enabled: true,
      mappings: [],
    };
    void patchSettings({ appProfiles: [...settings.appProfiles, created] }, true);
    setSelectedScopeId(created.id);
    setProfileNameDraft('');
    setProfileProcDraft('');
    setAddProfileOpen(false);
  };

  const handleDeleteProfile = (profileId: string) => {
    const nextProfiles = settings.appProfiles.filter((p) => p.id !== profileId);
    void patchSettings({ appProfiles: nextProfiles });
    if (selectedScopeId === profileId) {
      setSelectedScopeId('global');
    }
  };

  const handleToggleProfileEnabled = (profile: CapsAppProfile, enabled: boolean) => {
    const nextProfiles = settings.appProfiles.map((p) =>
      p.id === profile.id ? { ...p, enabled } : p,
    );
    void patchSettings({ appProfiles: nextProfiles });
  };

  // ── 键位映射 CRUD ─────────────────────────────────────────────────────────
  const openAddMapping = () => {
    setMappingDraft(createEmptyMappingDraft());
    setMappingError('');
    setMappingModalOpen(true);
  };

  const openEditMapping = (item: CapsKeyMapping) => {
    setMappingDraft({
      id: item.id,
      key: item.key,
      targetType: item.targetType,
      targetValue: item.targetValue,
      label: item.label ?? '',
      enabled: item.enabled,
    });
    setMappingError('');
    setMappingModalOpen(true);
  };

  const handleSaveMapping = () => {
    const cleanKey = mappingDraft.key.trim();
    const cleanTarget = mappingDraft.targetValue.trim();
    if (!cleanKey) {
      setMappingError(t('capsLayer.errKeyRequired', 'Trigger key cannot be empty'));
      return;
    }
    if (!cleanTarget) {
      setMappingError(t('capsLayer.errTargetRequired', 'Target output cannot be empty'));
      return;
    }
    const dup = currentMappings.some(
      (m) => m.id !== mappingDraft.id && m.key.toLowerCase() === cleanKey.toLowerCase(),
    );
    if (dup) {
      setMappingError(t('capsLayer.errDuplicateKey', 'This trigger key is already mapped in the current scope'));
      return;
    }

    const entry: CapsKeyMapping = {
      id: mappingDraft.id || `map-${Date.now()}`,
      key: cleanKey.length === 1 ? cleanKey.toUpperCase() : cleanKey,
      targetType: mappingDraft.targetType,
      targetValue: cleanTarget,
      label: mappingDraft.label.trim() || undefined,
      enabled: mappingDraft.enabled,
    };

    const nextMappings = mappingDraft.id
      ? currentMappings.map((m) => (m.id === mappingDraft.id ? entry : m))
      : [...currentMappings, entry];

    updateCurrentMappings(nextMappings);
    setMappingModalOpen(false);
    toast.success(t('capsLayer.mappingSavedToast', 'Key mapping saved'));
  };

  const handleToggleMapping = (item: CapsKeyMapping, enabled: boolean) => {
    updateCurrentMappings(
      currentMappings.map((m) => (m.id === item.id ? { ...m, enabled } : m)),
    );
  };

  const handleDeleteMapping = (id: string) => {
    updateCurrentMappings(currentMappings.filter((m) => m.id !== id));
  };

  if (loading) {
    return <PageSkeleton cards={3} rowsPerCard={3} />;
  }

  return (
    <div className="caps-page">
      {/* ── 1. 顶部总开关与 CapsLock 触发行为卡片 ────────────────────────────── */}
      <Card className="caps-hero-card">
        <div className="caps-hero">
          <div className="caps-hero__info">
            <div className="caps-hero__title-row">
              <KeyRound size={20} className="caps-hero__icon" aria-hidden="true" />
              <h2 className="caps-hero__title">
                {t('capsLayer.heroTitle', 'Caps Lock Hyper Layer')}
              </h2>
              <Badge
                variant={settings.enabled ? 'success' : 'muted'}
                text={
                  settings.enabled
                    ? t('capsLayer.statusActive', 'Active')
                    : t('capsLayer.statusDisabled', 'Disabled (Zero Hooks)')
                }
              />
            </div>
            <p className="caps-hero__desc">
              {t(
                'capsLayer.heroDesc',
                'Hold CapsLock as a home-row modifier to trigger arrow keys, shortcuts, or text snippets without moving your hands off the home row.',
              )}
            </p>
          </div>
          <div className="caps-hero__switch">
            <Toggle
              id="caps-layer-master-toggle"
              checked={settings.enabled}
              disabled={saving}
              onChange={(checked) => void patchSettings({ enabled: checked }, true)}
            />
          </div>
        </div>

        <div className="caps-input-notice" role="note">
          <ShieldAlert size={15} className="caps-input-notice__icon" aria-hidden="true" />
          <span>
            {t(
              'capsLayer.inputCautionNotice',
              'Usage tip: Normal typing without holding CapsLock is completely unaffected, but CapsLock will switch to trigger on release. This may affect IMEs that use CapsLock to toggle Chinese/English, remote desktops, or game keybindings. Press Shift + CapsLock anytime for native CapsLock, or add apps to the exclusion list below.',
            )}
          </span>
        </div>

        <DisabledCascade disabled={!settings.enabled} className="caps-hero-controls">
          <SettingRow
            label={t('capsLayer.tapActionLabel', 'Quick Tap CapsLock Action')}
            description={t('capsLayer.tapActionDesc', 'Action triggered when CapsLock is tapped and released alone without pressing another key')}
          >
            <div className="caps-segmented" role="group" aria-label={t('capsLayer.tapActionLabel', 'Quick Tap CapsLock Action')}>
              {(
                [
                  { id: 'toggle_caps', label: t('capsLayer.tapKeepCaps', 'Keep CapsLock') },
                  { id: 'esc', label: t('capsLayer.tapEsc', 'Send Esc') },
                  { id: 'none', label: t('capsLayer.tapDisable', 'Disable') },
                ] as const
              ).map((opt) => (
                <button
                  key={opt.id}
                  type="button"
                  className={`caps-segmented__btn ${settings.tapAction === opt.id ? 'is-active' : ''}`}
                  onClick={() => void patchSettings({ tapAction: opt.id })}
                >
                  {opt.label}
                </button>
              ))}
            </div>
          </SettingRow>

          <SettingRow
            label={t('capsLayer.shiftCapsLabel', 'Shift + CapsLock Toggles Native Uppercase')}
            description={t('capsLayer.shiftCapsDesc', 'Always retain a reliable fallback to toggle native CapsLock even when quick tap is remapped to Esc or disabled')}
          >
            <Toggle
              id="caps-shift-native"
              checked={settings.shiftCapsTogglesNative}
              onChange={(checked) => void patchSettings({ shiftCapsTogglesNative: checked })}
            />
          </SettingRow>

          <div className="caps-shift-hint" role="note">
            <Info size={15} className="caps-shift-hint__icon" aria-hidden="true" />
            <span>
              {t('capsLayer.shiftCapsInfoPrefix', 'Fallback tip: press')}{' '}
              <CodeBadge>Shift + CapsLock</CodeBadge>{' '}
              {t('capsLayer.shiftCapsInfoSuffix', 'anytime to toggle uppercase lock natively.')}
            </span>
          </div>
        </DisabledCascade>
      </Card>

      {/* ── 2. 一键极客预设胶囊区 ──────────────────────────────────────────── */}
      <DisabledCascade disabled={!settings.enabled} className="caps-cascade-stack">
        <SettingGroup
          title={t('capsLayer.presetsTitle', 'One-Click Navigation Presets')}
          icon={<Sparkles size={16} aria-hidden="true" />}
        >
          <p className="caps-section-desc">
            {t('capsLayer.presetsDesc', 'Load a curated home-row navigation layout into Global Layer in one click, then customize any binding below.')}
          </p>
          <div className="caps-presets-grid">
            {CAPS_PRESETS.map((preset) => {
              const isBusy = applyingPreset === preset.id;
              return (
                <button
                  key={preset.id}
                  type="button"
                  className="caps-preset-card"
                  disabled={applyingPreset !== null}
                  onClick={() => void handleApplyPreset(preset.id)}
                >
                  <div className="caps-preset-card__top">
                    <span className="caps-preset-card__title">
                      {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                      {t(preset.labelKey as any, preset.fallbackLabel)}
                    </span>
                    <CodeBadge variant="primary">
                      {isBusy ? (
                        <Loader2 size={12} className="caps-spin" aria-hidden="true" />
                      ) : (
                        `Caps + ${preset.keysBadge}`
                      )}
                    </CodeBadge>
                  </div>
                  <span className="caps-preset-card__desc">
                    {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                    {t(preset.hintKey as any, preset.fallbackHint)}
                  </span>
                </button>
              );
            })}
          </div>
        </SettingGroup>

        {/* ── 3. 双栏作用域与键位映射工作台 ────────────────────────────────── */}
        <SettingGroup
          title={t('capsLayer.appProfilesTitle', 'Scopes & Key Mappings')}
          icon={<Keyboard size={16} aria-hidden="true" />}
        >
          <p className="caps-section-desc">
            {t('capsLayer.appProfilesDesc', 'Per-application rules override Global Layer mappings whenever the target process is in focus.')}
          </p>

          <div className="caps-workspace">
            {/* 左侧作用域列表 */}
            <aside className="caps-scope-sidebar" aria-label={t('capsLayer.scopesSidebarAria', 'Caps Layer Scopes')}>
              <div className="caps-scope-section">
                <div className="caps-scope-section__header">
                  <span>{t('capsLayer.globalScopeSection', 'Default Layer')}</span>
                </div>
                <button
                  type="button"
                  className={`caps-scope-item ${effectiveScopeId === 'global' ? 'is-active' : ''}`}
                  onClick={() => setSelectedScopeId('global')}
                >
                  <span className="caps-scope-item__icon" aria-hidden="true">
                    <Globe size={14} />
                  </span>
                  <span className="caps-scope-item__text">
                    <span className="caps-scope-item__title">
                      {t('capsLayer.globalLayerTitle', 'Global Layer')}
                    </span>
                    <span className="caps-scope-item__sub">
                      {t('capsLayer.mappingCountLabel', '{{count}} mapping(s)', {
                        count: settings.globalMappings.length,
                      })}
                    </span>
                  </span>
                </button>
              </div>

              <div className="caps-scope-section">
                <div className="caps-scope-section__header">
                  <span>{t('capsLayer.perAppSection', 'App Profiles')}</span>
                  <button
                    type="button"
                    className="caps-scope-add-btn"
                    title={t('capsLayer.addAppProfileBtn', 'Add App Profile')}
                    aria-label={t('capsLayer.addAppProfileBtn', 'Add App Profile')}
                    onClick={() => {
                      setProfileNameDraft('');
                      setProfileProcDraft('');
                      setAddProfileOpen(true);
                    }}
                  >
                    <Plus size={14} aria-hidden="true" />
                  </button>
                </div>

                {settings.appProfiles.map((profile) => {
                  const isActive = effectiveScopeId === profile.id;
                  return (
                    <div
                      key={profile.id}
                      role="button"
                      tabIndex={0}
                      className={`caps-scope-item ${isActive ? 'is-active' : ''}`}
                      onClick={() => setSelectedScopeId(profile.id)}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter' || e.key === ' ') {
                          e.preventDefault();
                          setSelectedScopeId(profile.id);
                        }
                      }}
                    >
                      <span className="caps-scope-item__icon" aria-hidden="true">
                        <AppWindow size={14} />
                      </span>
                      <span className="caps-scope-item__text">
                        <span className="caps-scope-item__title">{profile.name}</span>
                        <CodeBadge variant="muted" className="caps-scope-item__proc">
                          {profile.processName}
                        </CodeBadge>
                      </span>
                      <span className="caps-scope-item__count">{profile.mappings.length}</span>
                      <button
                        type="button"
                        className="caps-scope-item__del"
                        aria-label={t('capsLayer.deleteProfileAria', 'Delete {{name}}', { name: profile.name })}
                        onClick={(e) => {
                          e.stopPropagation();
                          handleDeleteProfile(profile.id);
                        }}
                      >
                        <Trash2 size={13} aria-hidden="true" />
                      </button>
                    </div>
                  );
                })}

                <Button
                  variant="secondary"
                  size="sm"
                  className="caps-scope-add-full-btn"
                  onClick={() => {
                    setProfileNameDraft('');
                    setProfileProcDraft('');
                    setAddProfileOpen(true);
                  }}
                >
                  <Plus size={14} aria-hidden="true" />
                  <span>{t('capsLayer.addAppProfileBtn', 'Add App Profile')}</span>
                </Button>
              </div>
            </aside>

            {/* 右侧键位映射表 */}
            <div className="caps-detail-pane">
              <Card className="caps-detail-header-card">
                <div className="caps-detail-header">
                  <div className="caps-detail-header__info">
                    <div className="caps-detail-header__title-row">
                      {activeProfile ? <AppWindow size={16} aria-hidden="true" /> : <Globe size={16} aria-hidden="true" />}
                      <h3 className="caps-detail-header__title">
                        {activeProfile
                          ? activeProfile.name
                          : t('capsLayer.globalLayerTitle', 'Global Layer')}
                      </h3>
                      {activeProfile && <CodeBadge>{activeProfile.processName}</CodeBadge>}
                    </div>
                    <p className="caps-detail-header__desc">
                      {activeProfile
                        ? t('capsLayer.appScopeHint', 'Overrides Global Layer when this application is active.')
                        : t('capsLayer.globalScopeHint', 'Applies across all apps unless overridden by an App Profile or excluded below.')}
                    </p>
                  </div>
                  <div className="caps-detail-header__actions">
                    {activeProfile && (
                      <Toggle
                        id={`caps-profile-toggle-${activeProfile.id}`}
                        checked={activeProfile.enabled}
                        onChange={(checked) => handleToggleProfileEnabled(activeProfile, checked)}
                      />
                    )}
                    <Button variant="primary" size="sm" onClick={openAddMapping}>
                      <Plus size={14} aria-hidden="true" />
                      <span>{t('capsLayer.addMappingBtn', 'Add Mapping')}</span>
                    </Button>
                  </div>
                </div>
              </Card>

              <Card className="caps-mappings-card">
                {currentMappings.length === 0 ? (
                  <div className="caps-empty-state">
                    <Keyboard size={26} aria-hidden="true" />
                    <div className="caps-empty-state__title">
                      {t('capsLayer.noMappingsTitle', 'No Key Mappings Configured')}
                    </div>
                    <p className="caps-empty-state__desc">
                      {activeProfile
                        ? t('capsLayer.noAppMappingsDesc', 'Add app-specific Caps+Key bindings above to override Global Layer inside this app.')
                        : t('capsLayer.noGlobalMappingsDesc', 'Click a preset pill above (such as I/J/K/L or Vim H/J/K/L) or add a custom Caps+Key binding.')}
                    </p>
                  </div>
                ) : (
                  <div className="caps-mappings-list" role="list">
                    {currentMappings.map((item) => (
                      <div
                        key={item.id}
                        role="listitem"
                        className={`caps-mapping-row ${item.enabled ? '' : 'is-muted'}`}
                      >
                        <div className="caps-mapping-row__keys">
                          <CodeBadge variant="primary">Caps + {item.key}</CodeBadge>
                          <ArrowRight size={14} className="caps-mapping-row__arrow" aria-hidden="true" />
                          <Badge
                            variant="muted"
                            text={
                              item.targetType === 'combo'
                                ? t('capsLayer.typeCombo', 'Combo')
                                : item.targetType === 'text'
                                  ? t('capsLayer.typeText', 'Text')
                                  : t('capsLayer.typeKey', 'Key')
                            }
                          />
                          <CodeBadge>{item.targetValue}</CodeBadge>
                        </div>

                        <div className="caps-mapping-row__label">
                          {item.label || ''}
                        </div>

                        <div className="caps-mapping-row__actions">
                          <Toggle
                            id={`caps-map-${item.id}`}
                            size="sm"
                            checked={item.enabled}
                            onChange={(checked) => handleToggleMapping(item, checked)}
                          />
                          <Button
                            variant="ghost"
                            size="sm"
                            ariaLabel={t('common.edit', 'Edit')}
                            title={t('common.edit', 'Edit')}
                            onClick={() => openEditMapping(item)}
                          >
                            <Pencil size={13} aria-hidden="true" />
                          </Button>
                          <Button
                            variant="ghost"
                            size="sm"
                            ariaLabel={t('common.delete', 'Delete')}
                            title={t('common.delete', 'Delete')}
                            onClick={() => handleDeleteMapping(item.id)}
                          >
                            <Trash2 size={13} aria-hidden="true" />
                          </Button>
                        </div>
                      </div>
                    ))}
                  </div>
                )}
              </Card>
            </div>
          </div>
        </SettingGroup>

        {/* ── 4. 排除避让应用 ──────────────────────────────────────────────── */}
        <SettingGroup
          title={t('capsLayer.excludedAppsTitle', 'Excluded Applications')}
          icon={<ShieldAlert size={16} aria-hidden="true" />}
        >
          <Card>
            <SettingRow
              label={t('capsLayer.excludedAppsRowLabel', 'Bypass Caps Layer in Specific Apps')}
              description={t('capsLayer.excludedAppsRowDesc', 'Pass native CapsLock directly through in full-screen games, remote desktops, or virtual machines')}
              layout="vertical"
            >
              <HotkeyAppExclusionBar
                apps={settings.excludedApps}
                onChange={(nextApps) => void patchSettings({ excludedApps: nextApps })}
                emptyText={t('capsLayer.noExcludedApps', 'No excluded processes configured')}
                pickerTitle={t('capsLayer.pickExcludedTitle', 'Pick Application to Bypass Caps Layer')}
              />
            </SettingRow>
          </Card>
        </SettingGroup>
      </DisabledCascade>

      {/* ── 添加应用规则弹窗（复用共享 RunningWindowPicker） ──────────────────── */}
      {addProfileOpen && (
        <Modal
          open
          title={t('capsLayer.addAppModalTitle', 'Add Application Profile')}
          onClose={() => setAddProfileOpen(false)}
          footer={
            <>
              <Button variant="ghost" onClick={() => setAddProfileOpen(false)}>
                {t('common.cancel', 'Cancel')}
              </Button>
              <Button
                variant="primary"
                disabled={!profileProcDraft.trim()}
                onClick={handleConfirmAddProfile}
              >
                {t('common.add', 'Add')}
              </Button>
            </>
          }
        >
          <div className="caps-modal-stack">
            <RunningWindowPicker
              targetKind="process"
              selectedProcess={profileProcDraft}
              selectedClass=""
              fallbackPickedLabel={profileNameDraft}
              onPick={handleWindowPick}
            />
            <Field label={t('capsLayer.profileNameLabel', 'Profile Name')}>
              <TextInput
                value={profileNameDraft}
                onChange={setProfileNameDraft}
                placeholder={t('capsLayer.profileNamePlaceholder', 'e.g. VS Code / Chrome / Terminal')}
              />
            </Field>
            <Field label={t('capsLayer.profileProcessLabel', 'Process Name')}>
              <TextInput
                value={profileProcDraft}
                onChange={setProfileProcDraft}
                placeholder="code.exe"
              />
            </Field>
          </div>
        </Modal>
      )}

      {/* ── 添加 / 编辑键位映射弹窗 ────────────────────────────────────────── */}
      {mappingModalOpen && (
        <Modal
          open
          title={
            mappingDraft.id
              ? t('capsLayer.editMappingModalTitle', 'Edit Key Mapping')
              : t('capsLayer.addMappingModalTitle', 'Add Key Mapping')
          }
          onClose={() => setMappingModalOpen(false)}
          footer={
            <>
              <Button variant="ghost" onClick={() => setMappingModalOpen(false)}>
                {t('common.cancel', 'Cancel')}
              </Button>
              <Button variant="primary" onClick={handleSaveMapping}>
                {t('common.save', 'Save')}
              </Button>
            </>
          }
        >
          <div className="caps-modal-stack">
            <Field
              label={t('capsLayer.triggerKeyLabel', 'Trigger Key (Caps + Key)')}
              error={mappingError && !mappingDraft.key.trim() ? mappingError : undefined}
            >
              <TextInput
                value={mappingDraft.key}
                onChange={(val) => {
                  setMappingDraft({ ...mappingDraft, key: val });
                  setMappingError('');
                }}
                placeholder={t('capsLayer.triggerKeyPlaceholder', 'e.g. I, J, K, L, Space, Backspace')}
              />
              <div className="caps-quick-pills">
                {QUICK_TRIGGER_KEYS.map((k) => (
                  <button
                    key={k}
                    type="button"
                    className={`caps-quick-pill ${mappingDraft.key.toLowerCase() === k.toLowerCase() ? 'is-active' : ''}`}
                    onClick={() => {
                      setMappingDraft({ ...mappingDraft, key: k });
                      setMappingError('');
                    }}
                  >
                    {k}
                  </button>
                ))}
              </div>
            </Field>

            <Field label={t('capsLayer.targetTypeLabel', 'Output Type')}>
              <div className="caps-segmented" role="group" aria-label={t('capsLayer.targetTypeLabel', 'Output Type')}>
                {(
                  [
                    { id: 'key', label: t('capsLayer.typeKey', 'Key') },
                    { id: 'combo', label: t('capsLayer.typeCombo', 'Combo') },
                    { id: 'text', label: t('capsLayer.typeText', 'Text') },
                  ] as const
                ).map((opt) => (
                  <button
                    key={opt.id}
                    type="button"
                    className={`caps-segmented__btn ${mappingDraft.targetType === opt.id ? 'is-active' : ''}`}
                    onClick={() => setMappingDraft({ ...mappingDraft, targetType: opt.id })}
                  >
                    {opt.label}
                  </button>
                ))}
              </div>
            </Field>

            <Field
              label={t('capsLayer.targetValueLabel', 'Target Output')}
              error={mappingError && mappingDraft.key.trim() ? mappingError : undefined}
            >
              <TextInput
                value={mappingDraft.targetValue}
                onChange={(val) => {
                  setMappingDraft({ ...mappingDraft, targetValue: val });
                  setMappingError('');
                }}
                placeholder={
                  mappingDraft.targetType === 'combo'
                    ? 'Ctrl+C'
                    : mappingDraft.targetType === 'text'
                      ? t('capsLayer.targetTextPlaceholder', 'Enter text to type...')
                      : 'Up / Down / Left / Right'
                }
              />
              {mappingDraft.targetType !== 'text' && (
                <div className="caps-quick-pills">
                  {(mappingDraft.targetType === 'combo' ? QUICK_TARGET_COMBOS : QUICK_TARGET_KEYS).map(
                    (val) => (
                      <button
                        key={val}
                        type="button"
                        className={`caps-quick-pill ${mappingDraft.targetValue === val ? 'is-active' : ''}`}
                        onClick={() => {
                          setMappingDraft({ ...mappingDraft, targetValue: val });
                          setMappingError('');
                        }}
                      >
                        {val}
                      </button>
                    ),
                  )}
                </div>
              )}
            </Field>

            <Field label={t('capsLayer.mappingLabelField', 'Optional Description')}>
              <TextInput
                value={mappingDraft.label}
                onChange={(val) => setMappingDraft({ ...mappingDraft, label: val })}
                placeholder={t('capsLayer.mappingLabelPlaceholder', 'e.g. Cursor Up, Delete Word')}
              />
            </Field>
          </div>
        </Modal>
      )}
    </div>
  );
};

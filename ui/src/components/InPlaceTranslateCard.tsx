/* ─────────────────────────────────────────────────────────────────────────────
 * InPlaceTranslateCard — 划词与截图原位翻译统一配置工作台 (3 场景渐进式披露)
 *
 * 核心体验架构:
 *   1. 互译语言区块：主目标语言 + 双向互译副语言，直白举例说明；
 *   2. 3 选 1 场景卡片：
 *      - 免费 AI 翻译 (默认 · 免配置)：仅露 3 档速度/质量切换，零技术字段干扰；
 *      - 免费极速双源 (Bing / Google)：仅露优先线路切换，开箱即用；
 *      - 自有 API / 本地模型：仅主动点选时展开 OpenAI / DeepSeek / Gemini / DeepL / Ollama 全部极客配置与 DPAPI 加密秘钥管理；
 *   3. 底部一屏直达「测试翻译效果」（按需联网、实时回显延迟与译文样例）。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { Card, SettingRow, SettingGroup, Badge, Select, Button, TextInput, NumberInput, CodeBadge } from './UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import {
  Languages,
  PlugZap,
  Loader2,
  ShieldCheck,
  Check,
  Trash2,
  Sparkles,
  Zap,
  KeyRound,
} from 'lucide-react';
import './InPlaceTranslateCard.css';

interface TranslateSettingsPayload {
  provider: 'free_auto' | 'openai' | 'deepl';
  targetLang: string;
  secondaryTargetLang: string;
  freePrimary: 'edge' | 'google';
  openaiBaseUrl: string;
  openaiModel: string;
  hasOpenaiApiKey: boolean;
  openaiApiKeyMasked: string;
  deeplEndpoint: string;
  hasDeeplApiKey: boolean;
  deeplApiKeyMasked: string;
  timeoutSec: number;
}

interface TranslateUpdateResponse {
  success: boolean;
  error?: string;
  settings?: TranslateSettingsPayload;
}

interface TranslateProbeResponse {
  success: boolean;
  providerUsed?: string;
  resolvedTargetLang?: string;
  elapsedMs?: number;
  sampleOutput?: string;
  errorCode?: string;
  errorMessage?: string;
}

type TranslateChannelMode = 'free_ai' | 'free_dual' | 'custom';

type CustomPresetLabelKey =
  | 'translate.presetOpenAi'
  | 'translate.presetDeepSeek'
  | 'translate.presetGemini'
  | 'translate.presetDeepL'
  | 'translate.presetOllama';

type IndexTranslateModelLabelKey =
  | 'translate.indexModelFast'
  | 'translate.indexModelBalanced'
  | 'translate.indexModelFlagship';

interface CustomPreset {
  id: 'openai' | 'deepseek' | 'gemini' | 'deepl' | 'ollama';
  kind: 'openai' | 'deepl';
  labelKey: CustomPresetLabelKey;
  baseUrl: string;
  model: string;
}

interface IndexTranslateModelOption {
  id: string;
  labelKey: IndexTranslateModelLabelKey;
}

const INDEX_TRANSLATE_BASE_URL = 'https://index-translate.bilibili.com/v1';

const INDEX_TRANSLATE_MODELS: readonly IndexTranslateModelOption[] = [
  { id: 'Index-Translate-2B', labelKey: 'translate.indexModelFast' },
  { id: 'Index-Translate-9B', labelKey: 'translate.indexModelBalanced' },
  { id: 'Index-Translate-35B-A3B', labelKey: 'translate.indexModelFlagship' },
];

const CUSTOM_PRESETS: readonly CustomPreset[] = [
  { id: 'openai', kind: 'openai', labelKey: 'translate.presetOpenAi', baseUrl: 'https://api.openai.com/v1', model: 'gpt-4o-mini' },
  { id: 'deepseek', kind: 'openai', labelKey: 'translate.presetDeepSeek', baseUrl: 'https://api.deepseek.com/v1', model: 'deepseek-chat' },
  { id: 'gemini', kind: 'openai', labelKey: 'translate.presetGemini', baseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai', model: 'gemini-2.0-flash' },
  { id: 'deepl', kind: 'deepl', labelKey: 'translate.presetDeepL', baseUrl: '', model: '' },
  { id: 'ollama', kind: 'openai', labelKey: 'translate.presetOllama', baseUrl: 'http://127.0.0.1:11434/v1', model: 'qwen2.5:7b' },
];

const DEFAULT_TRANSLATE_SETTINGS: TranslateSettingsPayload = {
  provider: 'openai',
  targetLang: 'zh-Hans',
  secondaryTargetLang: 'en',
  freePrimary: 'edge',
  openaiBaseUrl: INDEX_TRANSLATE_BASE_URL,
  openaiModel: 'Index-Translate-9B',
  hasOpenaiApiKey: false,
  openaiApiKeyMasked: '',
  deeplEndpoint: '',
  hasDeeplApiKey: false,
  deeplApiKeyMasked: '',
  timeoutSec: 15,
};

function isBilibiliIndexUrl(url: string): boolean {
  return /^https:\/\/index-translate\.bilibili\.com(?:\/|$)/i.test(url.trim());
}

function isLocalOllamaUrl(url: string): boolean {
  return /^https?:\/\/(127\.0\.0\.1|localhost|\[::1\])(?::\d+)?(\/|$)/i.test(url.trim());
}

function deriveChannelMode(provider: TranslateSettingsPayload['provider'], baseUrl: string): TranslateChannelMode {
  if (provider === 'free_auto') return 'free_dual';
  if (provider === 'openai' && isBilibiliIndexUrl(baseUrl)) return 'free_ai';
  return 'custom';
}

export const InPlaceTranslateCard: FC = () => {
  const { t } = useTranslation();
  const [settings, setSettings] = useState<TranslateSettingsPayload>(DEFAULT_TRANSLATE_SETTINGS);
  const [loaded, setLoaded] = useState(false);

  const [uiMode, setUiMode] = useState<TranslateChannelMode>('free_ai');
  const [customSubProvider, setCustomSubProvider] = useState<'openai' | 'deepl'>('openai');
  const [openaiBaseUrlDraft, setOpenaiBaseUrlDraft] = useState(DEFAULT_TRANSLATE_SETTINGS.openaiBaseUrl);
  const [openaiModelDraft, setOpenaiModelDraft] = useState(DEFAULT_TRANSLATE_SETTINGS.openaiModel);
  const [openaiKeyDraft, setOpenaiKeyDraft] = useState('');
  const [deeplEndpointDraft, setDeeplEndpointDraft] = useState('');
  const [deeplKeyDraft, setDeeplKeyDraft] = useState('');

  const [savingCredentials, setSavingCredentials] = useState(false);
  const [testing, setTesting] = useState(false);
  const [probeResult, setProbeResult] = useState<TranslateProbeResponse | null>(null);

  const applyServerSnapshot = useCallback((data: Partial<TranslateSettingsPayload> | null | undefined) => {
    if (!data) return;
    const merged: TranslateSettingsPayload = {
      ...DEFAULT_TRANSLATE_SETTINGS,
      ...data,
      provider: (data.provider === 'free_auto' || data.provider === 'deepl') ? data.provider : 'openai',
      freePrimary: data.freePrimary === 'google' ? 'google' : 'edge',
      timeoutSec: typeof data.timeoutSec === 'number' ? data.timeoutSec : 15,
    };
    setSettings(merged);
    const derived = deriveChannelMode(merged.provider, merged.openaiBaseUrl);
    setUiMode(derived);
    if (merged.provider === 'deepl') {
      setCustomSubProvider('deepl');
    } else if (derived === 'custom') {
      setCustomSubProvider('openai');
    }
    setOpenaiBaseUrlDraft(merged.openaiBaseUrl);
    setOpenaiModelDraft(merged.openaiModel);
    setDeeplEndpointDraft(merged.deeplEndpoint);
  }, []);

  useEffect(() => {
    bridgeRequest<Partial<TranslateSettingsPayload>>('translate.getSettings', undefined, { silent: true })
      .then((data) => {
        applyServerSnapshot(data);
      })
      .catch(() => {
        // Keep defaults if backend plugin has not initialized yet
      })
      .finally(() => setLoaded(true));
  }, [applyServerSnapshot]);

  const patchSettings = useCallback(async (patch: Record<string, unknown>, showSavedToast = false) => {
    try {
      const res = await bridgeRequest<TranslateUpdateResponse>('translate.updateSettings', patch);
      if (!res?.success) {
        throw new Error(res?.error || 'update failed');
      }
      if (res.settings) {
        applyServerSnapshot(res.settings);
      }
      if (showSavedToast) {
        toast.success(t('translate.savedToast'));
      }
      return true;
    } catch (error) {
      toast.error(t('translate.saveFailed'), { description: String(error) });
      return false;
    }
  }, [applyServerSnapshot, t]);

  const handleSelectChannelMode = (nextMode: TranslateChannelMode) => {
    setProbeResult(null);
    if (nextMode === 'free_ai') {
      const preferredModel = INDEX_TRANSLATE_MODELS.some((m) => m.id === openaiModelDraft.trim())
        ? openaiModelDraft.trim()
        : 'Index-Translate-9B';
      setUiMode('free_ai');
      setOpenaiBaseUrlDraft(INDEX_TRANSLATE_BASE_URL);
      setOpenaiModelDraft(preferredModel);
      void patchSettings({
        provider: 'openai',
        openaiBaseUrl: INDEX_TRANSLATE_BASE_URL,
        openaiModel: preferredModel,
      });
      return;
    }

    if (nextMode === 'free_dual') {
      setUiMode('free_dual');
      void patchSettings({ provider: 'free_auto' });
      return;
    }

    // 切换至「自有 API / 本地模型」：若草稿仍是免费公网地址，预填 OpenAI 模板等待用户确认/切换预设
    setUiMode('custom');
    if (settings.provider === 'deepl') {
      setCustomSubProvider('deepl');
    } else {
      setCustomSubProvider('openai');
      if (isBilibiliIndexUrl(openaiBaseUrlDraft) || !openaiBaseUrlDraft.trim()) {
        setOpenaiBaseUrlDraft('https://api.openai.com/v1');
        setOpenaiModelDraft('gpt-4o-mini');
      }
    }
  };

  const handleApplyCustomPreset = (preset: CustomPreset) => {
    setProbeResult(null);
    setUiMode('custom');
    if (preset.kind === 'deepl') {
      setCustomSubProvider('deepl');
      void patchSettings({ provider: 'deepl' }, true);
      return;
    }
    setCustomSubProvider('openai');
    setOpenaiBaseUrlDraft(preset.baseUrl);
    setOpenaiModelDraft(preset.model);
    void patchSettings({
      provider: 'openai',
      openaiBaseUrl: preset.baseUrl,
      openaiModel: preset.model,
    }, true);
  };

  const handleSelectIndexModel = (modelId: string) => {
    setOpenaiModelDraft(modelId);
    setProbeResult(null);
    void patchSettings({
      provider: 'openai',
      openaiBaseUrl: INDEX_TRANSLATE_BASE_URL,
      openaiModel: modelId,
    }, true);
  };

  const handleSaveByok = async () => {
    if (savingCredentials) return;
    setSavingCredentials(true);
    try {
      const payload: Record<string, unknown> = customSubProvider === 'openai'
        ? {
            provider: 'openai',
            openaiBaseUrl: openaiBaseUrlDraft.trim(),
            openaiModel: openaiModelDraft.trim(),
            ...(openaiKeyDraft.trim() ? { openaiApiKey: openaiKeyDraft.trim() } : {}),
          }
        : {
            provider: 'deepl',
            deeplEndpoint: deeplEndpointDraft.trim(),
            ...(deeplKeyDraft.trim() ? { deeplApiKey: deeplKeyDraft.trim() } : {}),
          };
      const ok = await patchSettings(payload, true);
      if (ok) {
        setOpenaiKeyDraft('');
        setDeeplKeyDraft('');
      }
    } finally {
      setSavingCredentials(false);
    }
  };

  const handleClearKey = async (kind: 'openai' | 'deepl') => {
    if (savingCredentials) return;
    setSavingCredentials(true);
    try {
      const patch = kind === 'openai' ? { clearOpenaiApiKey: true } : { clearDeeplApiKey: true };
      const ok = await patchSettings(patch);
      if (ok) {
        if (kind === 'openai') setOpenaiKeyDraft('');
        else setDeeplKeyDraft('');
        toast.info(t('translate.keyClearedToast'));
      }
    } finally {
      setSavingCredentials(false);
    }
  };

  const handleTestConnection = async () => {
    if (testing) return;
    setTesting(true);
    setProbeResult(null);
    try {
      const effectiveProvider: TranslateSettingsPayload['provider'] =
        uiMode === 'free_dual'
          ? 'free_auto'
          : uiMode === 'free_ai'
            ? 'openai'
            : customSubProvider;

      const candidate: Record<string, unknown> = {
        provider: effectiveProvider,
        targetLang: settings.targetLang,
        secondaryTargetLang: settings.secondaryTargetLang,
        freePrimary: settings.freePrimary,
        timeoutSec: settings.timeoutSec,
      };
      if (effectiveProvider === 'openai') {
        candidate.openaiBaseUrl = uiMode === 'free_ai' ? INDEX_TRANSLATE_BASE_URL : openaiBaseUrlDraft.trim();
        candidate.openaiModel = openaiModelDraft.trim();
        if (uiMode === 'custom' && openaiKeyDraft.trim()) {
          candidate.openaiApiKey = openaiKeyDraft.trim();
        }
      } else if (effectiveProvider === 'deepl') {
        candidate.deeplEndpoint = deeplEndpointDraft.trim();
        if (deeplKeyDraft.trim()) candidate.deeplApiKey = deeplKeyDraft.trim();
      }

      const res = await bridgeRequest<TranslateProbeResponse>('translate.testConnection', candidate);
      setProbeResult(res);
      if (res?.success) {
        toast.success(t('translate.testSuccessToast', { ms: res.elapsedMs ?? 0 }));
      } else {
        toast.error(t('translate.testFailedToast'), {
          description: res?.errorMessage || res?.errorCode || '',
        });
      }
    } catch (error) {
      const fallback: TranslateProbeResponse = {
        success: false,
        errorCode: 'bridge_error',
        errorMessage: String(error),
      };
      setProbeResult(fallback);
      toast.error(t('translate.testFailedToast'), { description: String(error) });
    } finally {
      setTesting(false);
    }
  };

  const languageOptions = [
    { value: 'zh-Hans', label: t('translate.langZhHans') },
    { value: 'zh-Hant', label: t('translate.langZhHant') },
    { value: 'en', label: t('translate.langEn') },
    { value: 'ja', label: t('translate.langJa') },
    { value: 'ko', label: t('translate.langKo') },
    { value: 'fr', label: t('translate.langFr') },
    { value: 'de', label: t('translate.langDe') },
    { value: 'es', label: t('translate.langEs') },
    { value: 'ru', label: t('translate.langRu') },
    { value: 'pt', label: t('translate.langPt') },
  ];

  const isLocalOllama = uiMode === 'custom' && customSubProvider === 'openai' && isLocalOllamaUrl(openaiBaseUrlDraft);

  const byokDirty = uiMode === 'custom' && (
    customSubProvider === 'openai'
      ? (settings.provider !== 'openai' ||
         openaiBaseUrlDraft.trim() !== settings.openaiBaseUrl ||
         openaiModelDraft.trim() !== settings.openaiModel ||
         openaiKeyDraft.trim().length > 0)
      : (settings.provider !== 'deepl' ||
         deeplEndpointDraft.trim() !== settings.deeplEndpoint ||
         deeplKeyDraft.trim().length > 0)
  );

  if (!loaded) {
    return null;
  }

  return (
    <div className="translate-workbench">
      {/* ── 1. 互译语言 ─────────────────────────────────────────────────── */}
      <SettingGroup title={t('translate.languagesGroupTitle', 'Translation Languages')} icon={<Languages size={18} strokeWidth={2.2} aria-hidden="true" />}>
        <Card>
          <SettingRow
            label={t('translate.targetLangTitle')}
            description={t('translate.targetLangDesc')}
          >
            <Select
              id="translate-target-lang"
              value={settings.targetLang}
              onChange={(val) => void patchSettings({ targetLang: val })}
              options={languageOptions}
            />
          </SettingRow>

          <SettingRow
            label={t('translate.secondaryLangTitle')}
            description={t('translate.secondaryLangDesc')}
          >
            <Select
              id="translate-secondary-lang"
              value={settings.secondaryTargetLang}
              onChange={(val) => void patchSettings({ secondaryTargetLang: val })}
              options={languageOptions}
            />
          </SettingRow>
        </Card>
      </SettingGroup>

      {/* ── 2. 翻译通道 (3 选 1 场景化卡片 + 渐进式披露) ────────────────── */}
      <SettingGroup
        title={t('translate.groupTitle')}
        icon={<Sparkles size={18} strokeWidth={2.2} aria-hidden="true" />}
        actions={
          <Badge
            text={
              isLocalOllama
                ? t('translate.badgeLocalOffline')
                : uiMode === 'free_ai'
                  ? t('translate.badgePublicFreeAi')
                  : uiMode === 'free_dual'
                    ? t('translate.badgeZeroConfig')
                    : t('translate.badgeByokEncrypted')
            }
            variant={isLocalOllama || uiMode !== 'custom' ? 'success' : 'primary'}
          />
        }
      >
        <Card>
          <div className="translate-card">
            <div className="translate-card__mode-grid" role="radiogroup" aria-label={t('translate.providerTitle')}>
              <button
                type="button"
                role="radio"
                aria-checked={uiMode === 'free_ai'}
                className={`translate-card__mode-card ${uiMode === 'free_ai' ? 'is-active' : ''}`}
                onClick={() => handleSelectChannelMode('free_ai')}
              >
                <div className="translate-card__mode-head">
                  <Sparkles size={15} className="translate-card__mode-icon" aria-hidden="true" />
                  <span className="translate-card__mode-title">
                    {t('translate.modeFreeAiTitle', 'Free AI Translation')}
                  </span>
                </div>
                <span className="translate-card__mode-desc">
                  {t('translate.modeFreeAiDesc', 'Default & no key needed. Natural phrasing with automatic fallback on peak load.')}
                </span>
              </button>

              <button
                type="button"
                role="radio"
                aria-checked={uiMode === 'free_dual'}
                className={`translate-card__mode-card ${uiMode === 'free_dual' ? 'is-active' : ''}`}
                onClick={() => handleSelectChannelMode('free_dual')}
              >
                <div className="translate-card__mode-head">
                  <Zap size={15} className="translate-card__mode-icon" aria-hidden="true" />
                  <span className="translate-card__mode-title">
                    {t('translate.modeFreeDualTitle', 'Fast Dual Engine')}
                  </span>
                </div>
                <span className="translate-card__mode-desc">
                  {t('translate.modeFreeDualDesc', 'Free Bing / Google with automatic failover. Fast and steady for quick lookups.')}
                </span>
              </button>

              <button
                type="button"
                role="radio"
                aria-checked={uiMode === 'custom'}
                className={`translate-card__mode-card ${uiMode === 'custom' ? 'is-active' : ''}`}
                onClick={() => handleSelectChannelMode('custom')}
              >
                <div className="translate-card__mode-head">
                  <KeyRound size={15} className="translate-card__mode-icon" aria-hidden="true" />
                  <span className="translate-card__mode-title">
                    {t('translate.modeCustomTitle', 'Custom API / Local Model')}
                  </span>
                </div>
                <span className="translate-card__mode-desc">
                  {t('translate.modeCustomDesc', 'Connect OpenAI, DeepSeek, Gemini, DeepL, or offline Ollama with encrypted keys.')}
                </span>
              </button>
            </div>

            {/* 场景 1：免费 AI 翻译（仅保留极简 3 档速度/质量切换，零技术字段） */}
            {uiMode === 'free_ai' && (
              <div className="translate-card__mode-panel">
                <div className="translate-card__index-header">
                  <span className="translate-card__field-label">{t('translate.indexTranslateModelsLabel')}</span>
                  <div className="translate-card__presets">
                    {INDEX_TRANSLATE_MODELS.map((item) => {
                      const isActiveModel = openaiModelDraft.trim().toLowerCase() === item.id.toLowerCase();
                      return (
                        <button
                          key={item.id}
                          type="button"
                          className={`translate-card__preset-btn${isActiveModel ? ' translate-card__preset-btn--active' : ''}`}
                          onClick={() => handleSelectIndexModel(item.id)}
                        >
                          <span>{t(item.labelKey)}</span>
                        </button>
                      );
                    })}
                  </div>
                </div>
                <p className="translate-card__index-notice">{t('translate.indexTranslateNotice')}</p>
              </div>
            )}

            {/* 场景 2：免费极速双源（仅优先线路选择） */}
            {uiMode === 'free_dual' && (
              <SettingRow
                label={t('translate.freePrimaryTitle')}
                description={t('translate.freePrimaryDesc')}
              >
                <Select
                  id="translate-free-primary"
                  value={settings.freePrimary}
                  onChange={(val) => void patchSettings({ freePrimary: val })}
                  options={[
                    { value: 'edge', label: t('translate.freePrimaryEdge') },
                    { value: 'google', label: t('translate.freePrimaryGoogle') },
                  ]}
                />
              </SettingRow>
            )}

            {/* 场景 3：自有 API / 本地模型（仅主动点选时展开） */}
            {uiMode === 'custom' && (
              <div className="translate-card__custom-panel">
                <div className="translate-card__presets">
                  {CUSTOM_PRESETS.map((preset) => {
                    const isActive =
                      preset.kind === 'deepl'
                        ? customSubProvider === 'deepl'
                        : customSubProvider === 'openai' &&
                          openaiBaseUrlDraft.trim().toLowerCase() === preset.baseUrl.toLowerCase();
                    return (
                      <button
                        key={preset.id}
                        type="button"
                        className={`translate-card__preset-btn${isActive ? ' translate-card__preset-btn--active' : ''}`}
                        onClick={() => handleApplyCustomPreset(preset)}
                      >
                        <span>{t(preset.labelKey)}</span>
                      </button>
                    );
                  })}
                </div>

                {customSubProvider === 'openai' ? (
                  <div className="translate-card__fields-grid">
                    <div className="translate-card__field">
                      <label htmlFor="translate-openai-base-url" className="translate-card__field-label">
                        <span>{t('translate.openaiBaseUrlLabel')}</span>
                      </label>
                      <TextInput
                        id="translate-openai-base-url"
                        value={openaiBaseUrlDraft}
                        onChange={setOpenaiBaseUrlDraft}
                        placeholder="https://api.openai.com/v1"
                      />
                    </div>

                    <div className="translate-card__field">
                      <label htmlFor="translate-openai-model" className="translate-card__field-label">
                        <span>{t('translate.openaiModelLabel')}</span>
                      </label>
                      <TextInput
                        id="translate-openai-model"
                        value={openaiModelDraft}
                        onChange={setOpenaiModelDraft}
                        placeholder="gpt-4o-mini"
                      />
                    </div>

                    <div className="translate-card__field translate-card__field--full">
                      <label htmlFor="translate-openai-api-key" className="translate-card__field-label">
                        <span>{t('translate.apiKeyLabel')}</span>
                        {settings.hasOpenaiApiKey && (
                          <span className="translate-card__key-saved">
                            <ShieldCheck size={14} strokeWidth={2.2} aria-hidden="true" />
                            <span>{t('translate.keySavedHint')}</span>
                            <CodeBadge>{settings.openaiApiKeyMasked}</CodeBadge>
                          </span>
                        )}
                      </label>
                      <div className="translate-card__key-row">
                        <div className="translate-card__key-input">
                          <TextInput
                            id="translate-openai-api-key"
                            type="password"
                            value={openaiKeyDraft}
                            onChange={setOpenaiKeyDraft}
                            placeholder={
                              isLocalOllama
                                ? t('translate.ollamaKeyOptionalPlaceholder')
                                : settings.hasOpenaiApiKey
                                  ? t('translate.keyKeepExistingPlaceholder')
                                  : t('translate.apiKeyPlaceholder')
                            }
                          />
                        </div>
                        {settings.hasOpenaiApiKey && (
                          <Button
                            variant="secondary"
                            size="sm"
                            disabled={savingCredentials}
                            onClick={() => void handleClearKey('openai')}
                          >
                            <Trash2 size={14} strokeWidth={2.2} aria-hidden="true" />
                            <span>{t('translate.clearKeyBtn')}</span>
                          </Button>
                        )}
                      </div>
                    </div>
                  </div>
                ) : (
                  <div className="translate-card__fields-grid">
                    <div className="translate-card__field translate-card__field--full">
                      <label htmlFor="translate-deepl-api-key" className="translate-card__field-label">
                        <span>{t('translate.deeplKeyLabel')}</span>
                        {settings.hasDeeplApiKey && (
                          <span className="translate-card__key-saved">
                            <ShieldCheck size={14} strokeWidth={2.2} aria-hidden="true" />
                            <span>{t('translate.keySavedHint')}</span>
                            <CodeBadge>{settings.deeplApiKeyMasked}</CodeBadge>
                          </span>
                        )}
                      </label>
                      <div className="translate-card__key-row">
                        <div className="translate-card__key-input">
                          <TextInput
                            id="translate-deepl-api-key"
                            type="password"
                            value={deeplKeyDraft}
                            onChange={setDeeplKeyDraft}
                            placeholder={
                              settings.hasDeeplApiKey
                                ? t('translate.keyKeepExistingPlaceholder')
                                : t('translate.deeplKeyPlaceholder')
                            }
                          />
                        </div>
                        {settings.hasDeeplApiKey && (
                          <Button
                            variant="secondary"
                            size="sm"
                            disabled={savingCredentials}
                            onClick={() => void handleClearKey('deepl')}
                          >
                            <Trash2 size={14} strokeWidth={2.2} aria-hidden="true" />
                            <span>{t('translate.clearKeyBtn')}</span>
                          </Button>
                        )}
                      </div>
                    </div>

                    <div className="translate-card__field translate-card__field--full">
                      <label htmlFor="translate-deepl-endpoint" className="translate-card__field-label">
                        <span>{t('translate.deeplEndpointLabel')}</span>
                      </label>
                      <TextInput
                        id="translate-deepl-endpoint"
                        value={deeplEndpointDraft}
                        onChange={setDeeplEndpointDraft}
                        placeholder={t('translate.deeplEndpointPlaceholder')}
                      />
                    </div>
                  </div>
                )}

                <SettingRow
                  label={t('translate.timeoutTitle')}
                  description={t('translate.timeoutDesc')}
                >
                  <NumberInput
                    id="translate-timeout-sec"
                    value={settings.timeoutSec}
                    min={3}
                    max={60}
                    step={1}
                    unit={t('translate.unitSec')}
                    onChange={(val) => void patchSettings({ timeoutSec: val })}
                  />
                </SettingRow>
              </div>
            )}

            {/* 底部固定连通性测试栏（一屏直接可见） */}
            <div className="translate-card__verify-bar">
              <div className="translate-card__actions">
                {uiMode === 'custom' && (
                  <Button
                    variant="primary"
                    size="sm"
                    disabled={savingCredentials || !byokDirty}
                    onClick={() => void handleSaveByok()}
                  >
                    {savingCredentials ? (
                      <Loader2 size={15} strokeWidth={2.2} aria-hidden="true" className="uikit-spin-icon" />
                    ) : (
                      <Check size={15} strokeWidth={2.2} aria-hidden="true" />
                    )}
                    <span>{t('translate.saveCredentialsBtn')}</span>
                  </Button>
                )}

                <Button
                  variant={uiMode === 'custom' ? 'secondary' : 'primary'}
                  size="sm"
                  disabled={testing}
                  onClick={() => void handleTestConnection()}
                >
                  {testing ? (
                    <Loader2 size={15} strokeWidth={2.2} aria-hidden="true" className="uikit-spin-icon" />
                  ) : (
                    <PlugZap size={15} strokeWidth={2.2} aria-hidden="true" />
                  )}
                  <span>{testing ? t('translate.testingBtn') : t('translate.testConnectionBtn')}</span>
                </Button>
              </div>

              {probeResult && (
                <div className="translate-card__probe-result">
                  <div className="translate-card__probe-meta">
                    <Badge
                      text={probeResult.success ? t('translate.probeStatusOk') : t('translate.probeStatusError')}
                      variant={probeResult.success ? 'success' : 'danger'}
                    />
                    {probeResult.providerUsed && <CodeBadge>{probeResult.providerUsed}</CodeBadge>}
                    {typeof probeResult.elapsedMs === 'number' && probeResult.elapsedMs >= 0 && (
                      <span>{`${probeResult.elapsedMs} ms`}</span>
                    )}
                  </div>
                  {probeResult.success && probeResult.sampleOutput ? (
                    <div className="translate-card__probe-sample">
                      {`Hello, Tools3000! -> ${probeResult.sampleOutput}`}
                    </div>
                  ) : (
                    <div>{probeResult.errorMessage || probeResult.errorCode}</div>
                  )}
                </div>
              )}
            </div>
          </div>
        </Card>
      </SettingGroup>
    </div>
  );
};

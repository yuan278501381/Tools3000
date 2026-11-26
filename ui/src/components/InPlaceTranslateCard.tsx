/* ─────────────────────────────────────────────────────────────────────────────
 * InPlaceTranslateCard — 微信级截图识别原位翻译配置卡片
 *
 * 核心能力:
 *   1. 免 Key 免费双源 (Edge 主通道 + Google 自动备援) 零配置开箱即用；
 *   2. OpenAI 兼容接口 (OpenAI / DeepSeek / Gemini / 中转站 / 本地离线 Ollama) 一键预设；
 *   3. DeepL 官方 API (Free / Pro 自动路由) 与 DPAPI 加密秘钥管理（仅显示脱敏摘要）；
 *   4. 按需联网连通性自测（loading 防重入旋转态 + 耗时/通道/译文样例反馈）。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { Card, SettingRow, SettingGroup, Badge, Select, Button, TextInput, NumberInput, CodeBadge } from './UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { Languages, PlugZap, Loader2, ShieldCheck, Check, Trash2 } from 'lucide-react';
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

type OpenAiPresetLabelKey =
  | 'translate.presetIndexTranslate'
  | 'translate.presetOpenAi'
  | 'translate.presetDeepSeek'
  | 'translate.presetOllama'
  | 'translate.presetGemini';

type IndexTranslateModelLabelKey =
  | 'translate.indexModelFast'
  | 'translate.indexModelBalanced'
  | 'translate.indexModelFlagship';

interface OpenAiPreset {
  id: string;
  labelKey: OpenAiPresetLabelKey;
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

const OPENAI_PRESETS: readonly OpenAiPreset[] = [
  { id: 'index_translate', labelKey: 'translate.presetIndexTranslate', baseUrl: INDEX_TRANSLATE_BASE_URL, model: 'Index-Translate-9B' },
  { id: 'openai', labelKey: 'translate.presetOpenAi', baseUrl: 'https://api.openai.com/v1', model: 'gpt-4o-mini' },
  { id: 'deepseek', labelKey: 'translate.presetDeepSeek', baseUrl: 'https://api.deepseek.com/v1', model: 'deepseek-chat' },
  { id: 'ollama', labelKey: 'translate.presetOllama', baseUrl: 'http://127.0.0.1:11434/v1', model: 'qwen2.5:7b' },
  { id: 'gemini', labelKey: 'translate.presetGemini', baseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai', model: 'gemini-2.0-flash' },
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

export const InPlaceTranslateCard: FC = () => {
  const { t } = useTranslation();
  const [settings, setSettings] = useState<TranslateSettingsPayload>(DEFAULT_TRANSLATE_SETTINGS);
  const [loaded, setLoaded] = useState(false);

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

  const handleApplyPreset = (preset: OpenAiPreset) => {
    setOpenaiBaseUrlDraft(preset.baseUrl);
    setOpenaiModelDraft(preset.model);
    setProbeResult(null);
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
      openaiBaseUrl: openaiBaseUrlDraft.trim() || INDEX_TRANSLATE_BASE_URL,
      openaiModel: modelId,
    }, true);
  };

  const handleSaveByok = async () => {
    if (savingCredentials) return;
    setSavingCredentials(true);
    try {
      const payload: Record<string, unknown> = settings.provider === 'openai'
        ? {
            openaiBaseUrl: openaiBaseUrlDraft.trim(),
            openaiModel: openaiModelDraft.trim(),
            ...(openaiKeyDraft.trim() ? { openaiApiKey: openaiKeyDraft.trim() } : {}),
          }
        : {
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
      const candidate: Record<string, unknown> = {
        provider: settings.provider,
        targetLang: settings.targetLang,
        secondaryTargetLang: settings.secondaryTargetLang,
        freePrimary: settings.freePrimary,
        timeoutSec: settings.timeoutSec,
      };
      if (settings.provider === 'openai') {
        candidate.openaiBaseUrl = openaiBaseUrlDraft.trim();
        candidate.openaiModel = openaiModelDraft.trim();
        if (openaiKeyDraft.trim()) candidate.openaiApiKey = openaiKeyDraft.trim();
      } else if (settings.provider === 'deepl') {
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

  const isLocalOllama =
    settings.provider === 'openai' &&
    /^https?:\/\/(127\.0\.0\.1|localhost|\[::1\])(?::\d+)?(\/|$)/i.test(openaiBaseUrlDraft.trim());

  const isIndexTranslate =
    settings.provider === 'openai' &&
    /^https:\/\/index-translate\.bilibili\.com(?:\/|$)/i.test(openaiBaseUrlDraft.trim());

  const byokDirty = settings.provider === 'openai'
    ? (openaiBaseUrlDraft.trim() !== settings.openaiBaseUrl ||
       openaiModelDraft.trim() !== settings.openaiModel ||
       (!isIndexTranslate && openaiKeyDraft.trim().length > 0))
    : (deeplEndpointDraft.trim() !== settings.deeplEndpoint ||
       deeplKeyDraft.trim().length > 0);

  if (!loaded) {
    return null;
  }

  return (
    <SettingGroup title={t('translate.groupTitle')} icon={<Languages size={20} strokeWidth={2.5} aria-hidden="true" />}>
      <Card>
        <div className="translate-card">
          <SettingRow
            label={
              <div style={{ display: 'flex', alignItems: 'center', gap: 'var(--spacing-sm)', flexWrap: 'wrap' }}>
                <span>{t('translate.providerTitle')}</span>
                <Badge
                  text={
                    isLocalOllama
                      ? t('translate.badgeLocalOffline')
                      : isIndexTranslate
                        ? t('translate.badgePublicFreeAi')
                        : settings.provider === 'free_auto'
                          ? t('translate.badgeZeroConfig')
                          : t('translate.badgeByokEncrypted')
                  }
                  variant={isLocalOllama || isIndexTranslate || settings.provider === 'free_auto' ? 'success' : 'primary'}
                />
              </div>
            }
            description={t('translate.providerDesc')}
          >
            <Select
              id="translate-provider"
              value={settings.provider}
              onChange={(val) => {
                setProbeResult(null);
                void patchSettings({ provider: val });
              }}
              options={[
                { value: 'openai', label: t('translate.providerOpenAi') },
                { value: 'free_auto', label: t('translate.providerFreeAuto') },
                { value: 'deepl', label: t('translate.providerDeepL') },
              ]}
            />
          </SettingRow>

          {settings.provider === 'free_auto' && (
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

          {settings.provider === 'openai' && (
            <SettingRow
              layout="vertical"
              label={t('translate.openaiConfigTitle')}
              description={t('translate.openaiConfigDesc')}
            >
              <div style={{ display: 'flex', flexDirection: 'column', gap: '12px', width: '100%' }}>
                <div className="translate-card__presets">
                  {OPENAI_PRESETS.map((preset) => {
                    const isActive = openaiBaseUrlDraft.trim().toLowerCase() === preset.baseUrl.toLowerCase();
                    return (
                      <button
                        key={preset.id}
                        type="button"
                        className={`translate-card__preset-btn${isActive ? ' translate-card__preset-btn--active' : ''}`}
                        onClick={() => handleApplyPreset(preset)}
                      >
                        <span>{t(preset.labelKey)}</span>
                      </button>
                    );
                  })}
                </div>

                <div className="translate-card__fields-grid">
                  <div className="translate-card__field">
                    <label htmlFor="translate-openai-base-url" className="translate-card__field-label">
                      <span>{t('translate.openaiBaseUrlLabel')}</span>
                    </label>
                    <TextInput
                      id="translate-openai-base-url"
                      value={openaiBaseUrlDraft}
                      onChange={setOpenaiBaseUrlDraft}
                      placeholder={INDEX_TRANSLATE_BASE_URL}
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
                      placeholder={isIndexTranslate ? 'Index-Translate-9B' : 'gpt-4o-mini'}
                    />
                  </div>

                  {isIndexTranslate && (
                    <div className="translate-card__field translate-card__field--full">
                      <div className="translate-card__index-box">
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
                    </div>
                  )}

                  <div className="translate-card__field translate-card__field--full">
                    <label htmlFor="translate-openai-api-key" className="translate-card__field-label">
                      <span>{t('translate.apiKeyLabel')}</span>
                      {settings.hasOpenaiApiKey && (
                        <span style={{ display: 'inline-flex', alignItems: 'center', gap: '6px' }}>
                          <ShieldCheck size={14} strokeWidth={2.2} aria-hidden="true" />
                          <span>{t('translate.keySavedHint')}</span>
                          <CodeBadge>{settings.openaiApiKeyMasked}</CodeBadge>
                        </span>
                      )}
                    </label>
                    <div className="translate-card__key-row">
                      {isIndexTranslate ? (
                        <div className="translate-card__keyless-note">
                          <ShieldCheck size={15} strokeWidth={2.2} aria-hidden="true" />
                          <span>{t('translate.indexTranslateKeylessPlaceholder')}</span>
                        </div>
                      ) : (
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
                      )}
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
              </div>
            </SettingRow>
          )}

          {settings.provider === 'deepl' && (
            <SettingRow
              layout="vertical"
              label={t('translate.deeplConfigTitle')}
              description={t('translate.deeplConfigDesc')}
            >
              <div className="translate-card__fields-grid">
                <div className="translate-card__field translate-card__field--full">
                  <label htmlFor="translate-deepl-api-key" className="translate-card__field-label">
                    <span>{t('translate.deeplKeyLabel')}</span>
                    {settings.hasDeeplApiKey && (
                      <span style={{ display: 'inline-flex', alignItems: 'center', gap: '6px' }}>
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
            </SettingRow>
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

          <SettingRow
            layout="vertical"
            label={t('translate.verifyTitle')}
            description={t('translate.verifyDesc')}
          >
            <div className="translate-card__footer">
              <div className="translate-card__actions">
                {settings.provider !== 'free_auto' && (
                  <Button
                    variant="primary"
                    size="sm"
                    disabled={savingCredentials || !byokDirty}
                    onClick={() => void handleSaveByok()}
                  >
                    {savingCredentials ? (
                      <Loader2 size={15} strokeWidth={2.2} aria-hidden="true" style={{ animation: 'spin 1s linear infinite' }} />
                    ) : (
                      <Check size={15} strokeWidth={2.2} aria-hidden="true" />
                    )}
                    <span>{t('translate.saveCredentialsBtn')}</span>
                  </Button>
                )}

                <Button
                  variant={settings.provider === 'free_auto' || isIndexTranslate ? 'primary' : 'secondary'}
                  size="sm"
                  disabled={testing}
                  onClick={() => void handleTestConnection()}
                >
                  {testing ? (
                    <Loader2 size={15} strokeWidth={2.2} aria-hidden="true" style={{ animation: 'spin 1s linear infinite' }} />
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
          </SettingRow>
        </div>
      </Card>
    </SettingGroup>
  );
};

/* ─────────────────────────────────────────────────────────────────────────────
 * OcrPage — 双引擎高精度离线文字识别 (PP-OCRv4 + 增强版 Windows OCR) 与原图实景选字设置页
 *
 * 顶栏 3 页签信息架构:
 *   1. recognize: 识文与交互（热键、引擎选择、紧邻语种策略、输出与贴图实景选字）
 *   2. translate: 原位与划词翻译（全局划词翻译热键 + 截图原位翻译通道配置）
 *   3. models:    离线引擎与模型包（PP-OCRv4 核心包、+90MB 增强包、4 国扩展包矩阵、MI-GAN 修复包）
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, useRef, type FC } from 'react';
import {
  Card,
  Toggle,
  SettingRow,
  SettingGroup,
  SubSettingGroup,
  Tabs,
  Badge,
  Select,
  Button,
  CodeBadge,
  PageSkeleton,
  type TabItem,
} from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { InPlaceTranslateCard } from '../components/InPlaceTranslateCard';
import {
  consumePendingDeepNavigation,
  type DeepNavigationPayload,
  type OcrPageSubTab,
} from '../config/settingsSearchIndex';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { FileText, Download, X, Sparkles, Loader2, Trash2, Languages, Package } from 'lucide-react';
import './OcrPage.css';

interface OcrSettings {
  engine: string;
  modelTier: string;
  scriptMode: string;
  language: string;
  copyResult: boolean;
  showResultWindow: boolean;
  liveTextOnPin: boolean;
  pinLiveTextOnCapture: boolean;
}

interface DeepOcrScriptPackStatus {
  id: string;
  installed: boolean;
  downloading: boolean;
  fileBytes: number;
  estimatedBytes: number;
}

interface DeepOcrModelStatus {
  installed: boolean;
  serverInstalled?: boolean;
  downloading: boolean;
  downloadingServer?: boolean;
  downloadingScriptPack?: string;
  progressPercent: number;
  downloadedBytes: number;
  totalBytes: number;
  serverTotalBytes?: number;
  modelTier?: string;
  activeModelTier?: string;
  scriptMode?: string;
  scriptPacks?: DeepOcrScriptPackStatus[];
  state: 'installed' | 'downloading' | 'not_installed' | 'error' | string;
  currentFile: string;
  modelDir: string;
  error: string;
}

interface AiInpaintModelStatus {
  installed: boolean;
  downloading: boolean;
  progressPercent: number;
  downloadedBytes: number;
  totalBytes: number;
  state: 'installed' | 'downloading' | 'not_installed' | 'error' | string;
  modelDir: string;
  modelFile: string;
  error: string;
  usingGpu: boolean;
}

interface OcrStatusPayload {
  available: boolean;
  windowsOcrAvailable?: boolean;
  deepOcrInstalled?: boolean;
  effectiveEngine?: string;
  modelStatus?: DeepOcrModelStatus;
}

interface OperationResult {
  success: boolean;
  error?: string;
  shortcut?: string;
  conflictType?: string;
  conflictWith?: string;
}

const DEFAULT_SCRIPT_PACKS: DeepOcrScriptPackStatus[] = [
  { id: 'japan', installed: false, downloading: false, fileBytes: 0, estimatedBytes: 9752000 },
  { id: 'korean', installed: false, downloading: false, fileBytes: 0, estimatedBytes: 13485000 },
  { id: 'cyrillic', installed: false, downloading: false, fileBytes: 0, estimatedBytes: 8075000 },
  { id: 'latin', installed: false, downloading: false, fileBytes: 0, estimatedBytes: 7907000 },
];

const SCRIPT_PACK_CATALOG = [
  { id: 'japan', nameKey: 'ocr.scriptPackJapanName', descKey: 'ocr.scriptPackJapanDesc' },
  { id: 'korean', nameKey: 'ocr.scriptPackKoreanName', descKey: 'ocr.scriptPackKoreanDesc' },
  { id: 'cyrillic', nameKey: 'ocr.scriptPackCyrillicName', descKey: 'ocr.scriptPackCyrillicDesc' },
  { id: 'latin', nameKey: 'ocr.scriptPackLatinName', descKey: 'ocr.scriptPackLatinDesc' },
] as const;

const formatPackSizeMb = (bytes: number): string => {
  const mb = Math.max(0.1, bytes / (1024 * 1024));
  return `${mb.toFixed(1)} MB`;
};

const normalizeOcrTab = (raw?: string): OcrPageSubTab => {
  if (raw === 'translate' || raw === 'models') return raw;
  return 'recognize';
};

const DEFAULT_MODEL_STATUS: DeepOcrModelStatus = {
  installed: false,
  serverInstalled: false,
  downloading: false,
  downloadingServer: false,
  downloadingScriptPack: '',
  progressPercent: 0,
  downloadedBytes: 0,
  totalBytes: 16600000,
  serverTotalBytes: 90530732,
  modelTier: 'mobile',
  activeModelTier: 'mobile',
  scriptMode: 'auto',
  scriptPacks: DEFAULT_SCRIPT_PACKS,
  state: 'not_installed',
  currentFile: '',
  modelDir: '',
  error: '',
};

const DEFAULT_INPAINT_STATUS: AiInpaintModelStatus = {
  installed: false,
  downloading: false,
  progressPercent: 0,
  downloadedBytes: 0,
  totalBytes: 28079181,
  state: 'not_installed',
  modelDir: '',
  modelFile: 'migan_pipeline_v2.onnx',
  error: '',
  usingGpu: false,
};

export const OcrPage: FC = () => {
  const { t } = useTranslation();
  const [activeTab, setActiveTab] = useState<OcrPageSubTab>(
    () => normalizeOcrTab(consumePendingDeepNavigation('ocr')?.tabId),
  );
  const [settings, setSettings] = useState<OcrSettings>({
    engine: 'auto',
    modelTier: 'mobile',
    scriptMode: 'auto',
    language: 'auto',
    copyResult: true,
    showResultWindow: true,
    liveTextOnPin: true,
    pinLiveTextOnCapture: true,
  });
  const [loading, setLoading] = useState(true);
  const [available, setAvailable] = useState(false);
  const [modelStatus, setModelStatus] = useState<DeepOcrModelStatus>(DEFAULT_MODEL_STATUS);
  const [actionBusy, setActionBusy] = useState(false);
  const [inpaintStatus, setInpaintStatus] = useState<AiInpaintModelStatus>(DEFAULT_INPAINT_STATUS);
  const [inpaintBusy, setInpaintBusy] = useState(false);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [shortcut, setShortcut] = useState('Ctrl+Shift+O');
  const wasDownloadingRef = useRef(false);
  const wasDownloadingServerRef = useRef(false);
  const wasDownloadingScriptPackRef = useRef('');
  const wasInpaintDownloadingRef = useRef(false);

  const getHotkey = (name: string) => hotkeys.find((h) => h.name === name);

  useEffect(() => {
    const onDeepNavigate = (e: Event) => {
      const detail = (e as CustomEvent<DeepNavigationPayload>).detail;
      if (detail?.navId === 'ocr' && detail.tabId) {
        consumePendingDeepNavigation('ocr');
        setActiveTab(normalizeOcrTab(detail.tabId));
      }
    };
    window.addEventListener('tools3000:navigate-deep', onDeepNavigate);
    return () => window.removeEventListener('tools3000:navigate-deep', onDeepNavigate);
  }, []);

  const syncStatusFromBackend = useCallback(async () => {
    try {
      const status = await bridgeRequest<OcrStatusPayload>('ocr.getStatus', undefined, { silent: true });
      setAvailable(Boolean(status?.available));
      if (status?.modelStatus) {
        setModelStatus(status.modelStatus);
      }
    } catch {
      // Keep current status snapshot on transient polling failure
    }
  }, []);

  useEffect(() => {
    Promise.all([
      bridgeRequest<Partial<OcrSettings>>('ocr.getSettings'),
      bridgeRequest<OcrStatusPayload>('ocr.getStatus'),
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll'),
      bridgeRequest<AiInpaintModelStatus>('capture.getInpaintModelStatus', undefined, { silent: true }).catch(() => null),
    ])
      .then(([data, status, hotkeyList, inpaintPayload]) => {
        const resolvedLanguage =
          typeof data?.language === 'string' && data.language.trim() !== '' ? data.language : 'auto';
        const resolvedEngine =
          typeof data?.engine === 'string' && ['auto', 'ppocr_v4', 'windows'].includes(data.engine)
            ? data.engine
            : 'auto';
        const resolvedModelTier =
          typeof data?.modelTier === 'string' && ['mobile', 'server'].includes(data.modelTier)
            ? data.modelTier
            : 'mobile';
        const resolvedScriptMode =
          typeof data?.scriptMode === 'string' &&
          ['auto', 'cjk', 'japan', 'korean', 'cyrillic', 'latin'].includes(data.scriptMode)
            ? data.scriptMode
            : 'auto';
        setSettings((prev) => ({
          ...prev,
          ...data,
          engine: resolvedEngine,
          modelTier: resolvedModelTier,
          scriptMode: resolvedScriptMode,
          language: resolvedLanguage,
        }));
        setAvailable(Boolean(status?.available));
        if (status?.modelStatus) {
          setModelStatus(status.modelStatus);
          wasDownloadingRef.current = Boolean(status.modelStatus.downloading);
          wasDownloadingServerRef.current = Boolean(status.modelStatus.downloadingServer);
          wasDownloadingScriptPackRef.current = status.modelStatus.downloadingScriptPack ?? '';
        }
        if (inpaintPayload) {
          setInpaintStatus(inpaintPayload);
          wasInpaintDownloadingRef.current = Boolean(inpaintPayload.downloading);
        }
        const list = Array.isArray(hotkeyList) ? hotkeyList : [];
        setHotkeys(list);
        setShortcut(list.find((item) => item.name === 'OCR')?.shortcut ?? 'Ctrl+Shift+O');
      })
      .catch((error) => {
        console.error(error);
        toast.error(t('ocr.loadFailed'));
      })
      .finally(() => setLoading(false));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    if (!inpaintStatus.downloading) {
      return undefined;
    }
    wasInpaintDownloadingRef.current = true;
    const timer = window.setInterval(() => {
      bridgeRequest<AiInpaintModelStatus>('capture.getInpaintModelStatus', undefined, { silent: true })
        .then((next) => {
          if (!next) return;
          setInpaintStatus(next);
          if (!next.downloading && wasInpaintDownloadingRef.current) {
            wasInpaintDownloadingRef.current = false;
            if (next.installed) {
              toast.success(t('ocr.inpaintDownloadCompleted'));
            } else if (next.state === 'error' && next.error) {
              toast.error(t('ocr.inpaintDownloadFailed'), { description: next.error });
            }
          }
        })
        .catch(() => {
          /* ignore transient poll error */
        });
    }, 380);
    return () => window.clearInterval(timer);
  }, [inpaintStatus.downloading, t]);

  useEffect(() => {
    if (!modelStatus.downloading) {
      return undefined;
    }
    wasDownloadingRef.current = true;
    wasDownloadingServerRef.current = Boolean(modelStatus.downloadingServer);
    wasDownloadingScriptPackRef.current = modelStatus.downloadingScriptPack ?? '';
    const timer = window.setInterval(() => {
      bridgeRequest<DeepOcrModelStatus>('ocr.getModelStatus', undefined, { silent: true })
        .then((next) => {
          if (!next) return;
          setModelStatus(next);
          if (!next.downloading && wasDownloadingRef.current) {
            const finishedServer = wasDownloadingServerRef.current;
            const finishedScriptPack = wasDownloadingScriptPackRef.current;
            wasDownloadingRef.current = false;
            wasDownloadingServerRef.current = false;
            wasDownloadingScriptPackRef.current = '';
            void syncStatusFromBackend();
            if (finishedScriptPack) {
              if (!next.error) {
                toast.success(t('ocr.scriptPackDownloadCompleted'));
              } else {
                toast.error(t('ocr.scriptPackDownloadFailed'), { description: next.error });
              }
            } else if (finishedServer) {
              if (next.serverInstalled && !next.error) {
                setSettings((prev) => ({ ...prev, modelTier: 'server' }));
                toast.success(t('ocr.serverDownloadCompleted'));
              } else if (next.error) {
                toast.error(t('ocr.serverDownloadFailed'), { description: next.error });
              }
            } else if (next.installed && !next.error) {
              toast.success(t('ocr.downloadCompleted'));
            } else if (next.state === 'error' && next.error) {
              toast.error(t('ocr.downloadFailedToast'), { description: next.error });
            }
          }
        })
        .catch(() => {
          /* ignore transient poll error */
        });
    }, 380);
    return () => window.clearInterval(timer);
  }, [modelStatus.downloading, modelStatus.downloadingServer, modelStatus.downloadingScriptPack, syncStatusFromBackend, t]);

  const updateSetting = useCallback(
    (key: keyof OcrSettings, value: unknown) => {
      setSettings((prev) => ({ ...prev, [key]: value }));
      bridgeRequest<OperationResult>('ocr.updateSettings', { [key]: value })
        .then((result) => {
          if (!result.success) throw new Error(result.error || 'update failed');
          void syncStatusFromBackend();
        })
        .catch(async (error) => {
          toast.error(t('ocr.saveFailed'), { description: String(error) });
          try {
            const restored = await bridgeRequest<Partial<OcrSettings>>('ocr.getSettings');
            setSettings((prev) => ({ ...prev, ...restored }));
          } catch {
            /* keep usable */
          }
        });
    },
    [syncStatusFromBackend, t],
  );

  const handleStartDownload = async (tier: 'mobile' | 'server' = 'mobile') => {
    if (actionBusy || modelStatus.downloading) return;
    setActionBusy(true);
    try {
      const res = await bridgeRequest<{ success: boolean; modelStatus?: DeepOcrModelStatus }>(
        'ocr.downloadModel',
        { tier },
      );
      if (res?.modelStatus) {
        setModelStatus(res.modelStatus);
        wasDownloadingRef.current = Boolean(res.modelStatus.downloading);
        wasDownloadingServerRef.current = Boolean(res.modelStatus.downloadingServer);
      }
      if (res?.success) {
        toast.info(tier === 'server' ? t('ocr.serverDownloadStarted') : t('ocr.downloadStarted'));
      }
    } catch (error) {
      toast.error(
        tier === 'server' ? t('ocr.serverDownloadFailed') : t('ocr.downloadFailedToast'),
        { description: String(error) },
      );
    } finally {
      setActionBusy(false);
    }
  };

  const handleCancelDownload = async () => {
    if (actionBusy) return;
    setActionBusy(true);
    try {
      wasDownloadingRef.current = false;
      wasDownloadingServerRef.current = false;
      wasDownloadingScriptPackRef.current = '';
      const res = await bridgeRequest<{ success: boolean; modelStatus?: DeepOcrModelStatus }>('ocr.cancelModelDownload');
      if (res?.modelStatus) {
        setModelStatus(res.modelStatus);
      }
      toast.info(t('ocr.downloadCancelled'));
    } catch (error) {
      console.error(error);
    } finally {
      setActionBusy(false);
    }
  };

  const handleDeleteServerModel = async () => {
    if (actionBusy || modelStatus.downloading) return;
    setActionBusy(true);
    try {
      const res = await bridgeRequest<{ success: boolean; modelStatus?: DeepOcrModelStatus }>('ocr.deleteServerModel');
      if (res?.modelStatus) {
        setModelStatus(res.modelStatus);
        if (res.modelStatus.modelTier) {
          setSettings((prev) => ({ ...prev, modelTier: res.modelStatus?.modelTier ?? 'mobile' }));
        }
      }
      if (res?.success) {
        toast.success(t('ocr.serverDeleteSuccess'));
      } else {
        toast.error(t('ocr.serverDeleteFailed'));
      }
    } catch (error) {
      toast.error(t('ocr.serverDeleteFailed'), { description: String(error) });
    } finally {
      setActionBusy(false);
    }
  };

  const handleDownloadScriptPack = async (packId: string) => {
    if (actionBusy || modelStatus.downloading) return;
    setActionBusy(true);
    try {
      const res = await bridgeRequest<{ success: boolean; modelStatus?: DeepOcrModelStatus }>(
        'ocr.downloadScriptPack',
        { packId },
      );
      if (res?.modelStatus) {
        setModelStatus(res.modelStatus);
        wasDownloadingRef.current = Boolean(res.modelStatus.downloading);
        wasDownloadingServerRef.current = Boolean(res.modelStatus.downloadingServer);
        wasDownloadingScriptPackRef.current = res.modelStatus.downloadingScriptPack ?? packId;
      }
      if (res?.success) {
        toast.info(t('ocr.scriptPackDownloadStarted'));
      }
    } catch (error) {
      toast.error(t('ocr.scriptPackDownloadFailed'), { description: String(error) });
    } finally {
      setActionBusy(false);
    }
  };

  const handleDeleteScriptPack = async (packId: string) => {
    if (actionBusy || modelStatus.downloading) return;
    setActionBusy(true);
    try {
      const res = await bridgeRequest<{ success: boolean; modelStatus?: DeepOcrModelStatus }>(
        'ocr.deleteScriptPack',
        { packId },
      );
      if (res?.modelStatus) {
        setModelStatus(res.modelStatus);
      }
      if (res?.success) {
        toast.success(t('ocr.scriptPackDeleteSuccess'));
      } else {
        toast.error(t('ocr.scriptPackDeleteFailed'));
      }
    } catch (error) {
      toast.error(t('ocr.scriptPackDeleteFailed'), { description: String(error) });
    } finally {
      setActionBusy(false);
    }
  };

  const handleStartInpaintDownload = async () => {
    if (inpaintBusy || inpaintStatus.downloading) return;
    setInpaintBusy(true);
    try {
      const res = await bridgeRequest<{ success: boolean; modelStatus?: AiInpaintModelStatus }>('capture.downloadInpaintModel');
      if (res?.modelStatus) {
        setInpaintStatus(res.modelStatus);
        wasInpaintDownloadingRef.current = Boolean(res.modelStatus.downloading);
      }
      if (res?.success) {
        toast.info(t('ocr.inpaintDownloadStarted'));
      }
    } catch (error) {
      toast.error(t('ocr.inpaintDownloadFailed'), { description: String(error) });
    } finally {
      setInpaintBusy(false);
    }
  };

  const handleCancelInpaintDownload = async () => {
    if (inpaintBusy) return;
    setInpaintBusy(true);
    try {
      wasInpaintDownloadingRef.current = false;
      const res = await bridgeRequest<{ success: boolean; modelStatus?: AiInpaintModelStatus }>('capture.cancelInpaintModelDownload');
      if (res?.modelStatus) {
        setInpaintStatus(res.modelStatus);
      }
      toast.info(t('ocr.inpaintDownloadCancelled'));
    } catch (error) {
      console.error(error);
    } finally {
      setInpaintBusy(false);
    }
  };

  const handleDeleteInpaintModel = async () => {
    if (inpaintBusy || inpaintStatus.downloading) return;
    setInpaintBusy(true);
    try {
      const res = await bridgeRequest<{ success: boolean; modelStatus?: AiInpaintModelStatus }>('capture.deleteInpaintModel');
      if (res?.modelStatus) {
        setInpaintStatus(res.modelStatus);
      }
      if (res?.success) {
        toast.success(t('ocr.inpaintDeleteSuccess'));
      } else {
        toast.error(t('ocr.inpaintDeleteFailed'));
      }
    } catch (error) {
      toast.error(t('ocr.inpaintDeleteFailed'), { description: String(error) });
    } finally {
      setInpaintBusy(false);
    }
  };

  const rebindNamedShortcut = async (hotkeyName: string, value: string) => {
    try {
      const result = await bridgeRequest<OperationResult>('hotkey.rebind', { name: hotkeyName, hotkey: value });
      if (!result.success) throw new Error(result.error || t('hotkey.bindFailed'));
      if (hotkeyName === 'OCR') {
        setShortcut(result.shortcut ?? value);
      }
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) {
        setHotkeys(refreshed);
        const ocrEntry = refreshed.find((item) => item.name === 'OCR');
        if (ocrEntry) setShortcut(ocrEntry.shortcut);
      }
    } catch (error) {
      toast.error(t('hotkey.bindFailed'), { description: String(error) });
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) setHotkeys(refreshed);
    }
  };

  if (loading) {
    return <PageSkeleton cards={2} rowsPerCard={4} />;
  }

  const renderModelStatusBadge = () => {
    if (modelStatus.downloading) {
      return (
        <Badge
          text={t('ocr.deepStatusDownloading', { percent: modelStatus.progressPercent })}
          variant="primary"
        />
      );
    }
    if (modelStatus.installed) {
      return <Badge text={t('ocr.deepStatusInstalled')} variant="success" />;
    }
    if (modelStatus.state === 'error') {
      return <Badge text={t('ocr.deepStatusError')} variant="danger" />;
    }
    return <Badge text={t('ocr.deepStatusNotInstalled')} variant="muted" />;
  };

  const renderServerTierBadge = () => {
    if (modelStatus.downloading && modelStatus.downloadingServer) {
      return (
        <Badge
          text={t('ocr.deepStatusDownloading', { percent: modelStatus.progressPercent })}
          variant="primary"
        />
      );
    }
    if (modelStatus.serverInstalled) {
      return (
        <Badge
          text={modelStatus.activeModelTier === 'server' ? t('ocr.tierServerActive') : t('ocr.tierServerReady')}
          variant={modelStatus.activeModelTier === 'server' ? 'success' : 'muted'}
        />
      );
    }
    return <Badge text={t('ocr.tierServerNotInstalled')} variant="muted" />;
  };

  const scriptPacksList: DeepOcrScriptPackStatus[] =
    Array.isArray(modelStatus.scriptPacks) && modelStatus.scriptPacks.length > 0
      ? modelStatus.scriptPacks
      : DEFAULT_SCRIPT_PACKS;
  const scriptPackMap = new Map(scriptPacksList.map((p) => [p.id, p]));
  const installedScriptPackCount = scriptPacksList.filter((p) => p.installed).length;
  const missingScriptPackBytes = scriptPacksList
    .filter((p) => !p.installed)
    .reduce((sum, p) => sum + (p.estimatedBytes || 0), 0);
  const isDownloadingScriptPack = Boolean(modelStatus.downloading && modelStatus.downloadingScriptPack);
  const selectedScriptMissingPack =
    ['japan', 'korean', 'cyrillic', 'latin'].includes(settings.scriptMode) &&
    !scriptPackMap.get(settings.scriptMode)?.installed;

  const renderScriptPacksBadge = () => {
    if (isDownloadingScriptPack) {
      return (
        <Badge
          text={t('ocr.deepStatusDownloading', { percent: modelStatus.progressPercent })}
          variant="primary"
        />
      );
    }
    if (installedScriptPackCount === scriptPacksList.length && scriptPacksList.length > 0) {
      return <Badge text={t('ocr.scriptPackBadgeAllReady')} variant="success" />;
    }
    if (installedScriptPackCount > 0) {
      return (
        <Badge
          text={t('ocr.scriptPackBadgeCount', {
            count: installedScriptPackCount,
            total: scriptPacksList.length,
          })}
          variant="primary"
        />
      );
    }
    return <Badge text={t('ocr.scriptPackBadgeBaseOnly')} variant="muted" />;
  };

  const renderInpaintStatusBadge = () => {
    if (inpaintStatus.downloading) {
      return (
        <Badge
          text={t('ocr.inpaintStatusDownloading', { percent: inpaintStatus.progressPercent })}
          variant="primary"
        />
      );
    }
    if (inpaintStatus.installed) {
      return (
        <Badge
          text={inpaintStatus.usingGpu ? t('ocr.inpaintStatusInstalledGpu') : t('ocr.inpaintStatusInstalled')}
          variant="success"
        />
      );
    }
    if (inpaintStatus.state === 'error') {
      return <Badge text={t('ocr.inpaintStatusError')} variant="danger" />;
    }
    return <Badge text={t('ocr.inpaintStatusNotInstalled')} variant="muted" />;
  };

  const scriptModeOptions = [
    { value: 'auto', label: t('ocr.scriptModeAuto') },
    { value: 'cjk', label: t('ocr.scriptModeCjk') },
    { value: 'japan', label: t('ocr.scriptModeJapan') },
    { value: 'korean', label: t('ocr.scriptModeKorean') },
    { value: 'cyrillic', label: t('ocr.scriptModeCyrillic') },
    { value: 'latin', label: t('ocr.scriptModeLatin') },
  ];

  const windowsLangOptions = [
    { value: 'auto', label: t('ocr.langAuto') },
    { value: 'zh-Hans-CN', label: t('ocr.langZhHans') },
    { value: 'zh-Hant-TW', label: t('ocr.langZhHant') },
    { value: 'en-US', label: t('ocr.langEnUs') },
    { value: 'ja-JP', label: t('ocr.langJaJp') },
    { value: 'ko-KR', label: t('ocr.langKoKr') },
    { value: 'ru-RU', label: t('ocr.langRuRu') },
    { value: 'pt-BR', label: t('ocr.langPtBr') },
    { value: 'es-ES', label: t('ocr.langEsEs') },
  ];

  const isUsingWindowsOcr =
    settings.engine === 'windows' || (settings.engine === 'auto' && !modelStatus.installed);

  const translateHotkeyEntry =
    hotkeys.find((h) => h.name === 'Quick Translate') ??
    hotkeys.find((h) => h.name === 'Translate Selection');

  const tabs: TabItem<OcrPageSubTab>[] = [
    { id: 'recognize', label: t('ocr.tabRecognize', 'Recognition & Live Text'), icon: <FileText size={16} /> },
    { id: 'translate', label: t('ocr.tabTranslate', 'In-Place & Quick Translate'), icon: <Languages size={16} /> },
    { id: 'models', label: t('ocr.tabModels', 'Offline Engines & Packs'), icon: <Package size={16} /> },
  ];

  return (
    <div className="ocr-page">
      <Tabs
        tabs={tabs}
        activeId={activeTab}
        onChange={(id) => setActiveTab(id)}
        ariaLabel={t('ocr.title')}
      />

      {/* ── Tab 1: 识文与交互 (默认清爽首屏) ───────────────────────────────── */}
      {activeTab === 'recognize' && (
        <div className="ocr-page__tab-panel">
          <SettingGroup title={t('ocr.title')} icon={<FileText size={18} strokeWidth={2.2} />}>
            <Card>
              <SettingRow
                label={
                  <div className="ocr-page__row-label">
                    <span>{t('ocr.shortcut')}</span>
                    <HotkeyStatusBadge entry={getHotkey('OCR')} />
                  </div>
                }
                description={t('ocr.shortcutDesc')}
              >
                <HotkeyRecorder
                  id="ocr-shortcut"
                  value={shortcut}
                  onChange={(v) => void rebindNamedShortcut('OCR', v)}
                />
              </SettingRow>

              <SettingRow
                label={
                  <div className="ocr-page__row-label">
                    <span>{t('ocr.engine')}</span>
                    <Badge
                      text={available ? t('ocr.available') : t('ocr.unavailable')}
                      variant={available ? 'success' : 'muted'}
                    />
                  </div>
                }
                description={t('ocr.engineDesc')}
              >
                <div className="ocr-page__engine-controls">
                  <Select
                    id="ocr-engine-mode"
                    value={settings.engine || 'auto'}
                    onChange={(val) => updateSetting('engine', val)}
                    options={[
                      { value: 'auto', label: t('ocr.engineAuto') },
                      { value: 'ppocr_v4', label: t('ocr.enginePpOcrV4') },
                      { value: 'windows', label: t('ocr.engineWindowsEnhanced') },
                    ]}
                  />
                  <Button
                    variant="secondary"
                    size="sm"
                    onClick={() => setActiveTab('models')}
                  >
                    <Package size={14} aria-hidden="true" />
                    <span>{t('ocr.manageModelsBtn', 'Manage Offline Models')}</span>
                  </Button>
                </div>
              </SettingRow>

              <SubSettingGroup>
                {isUsingWindowsOcr ? (
                  <SettingRow label={t('ocr.language')} description={t('ocr.languageDesc')}>
                    <Select
                      id="ocr-language"
                      value={settings.language || 'auto'}
                      onChange={(val) => updateSetting('language', val)}
                      options={windowsLangOptions}
                    />
                  </SettingRow>
                ) : (
                  <SettingRow
                    label={
                      <div className="ocr-page__row-label">
                        <Languages size={15} strokeWidth={2.2} aria-hidden="true" className="ocr-page__label-icon" />
                        <span>{t('ocr.scriptPackTitle')}</span>
                        {renderScriptPacksBadge()}
                      </div>
                    }
                    description={
                      selectedScriptMissingPack && !modelStatus.downloading
                        ? t('ocr.scriptPackNotInstalledHint')
                        : t('ocr.scriptPackDesc')
                    }
                  >
                    <Select
                      id="ocr-script-mode"
                      value={settings.scriptMode || 'auto'}
                      onChange={(val) => updateSetting('scriptMode', val)}
                      options={scriptModeOptions}
                    />
                  </SettingRow>
                )}
              </SubSettingGroup>
            </Card>
          </SettingGroup>

          <SettingGroup
            title={t('ocr.outputAndLiveTextGroup', 'Output Behavior & Pin Live Text')}
            icon={<Sparkles size={18} strokeWidth={2.2} />}
          >
            <Card>
              <Toggle
                id="copyResult"
                label={t('ocr.copyToClipboard')}
                description={t('ocr.copyToClipboardDesc')}
                checked={settings.copyResult}
                onChange={(v) => updateSetting('copyResult', v)}
              />

              <Toggle
                id="showResultWindow"
                label={t('ocr.showResultWindow')}
                description={t('ocr.showResultWindowDesc')}
                checked={settings.showResultWindow}
                onChange={(v) => updateSetting('showResultWindow', v)}
              />

              <Toggle
                id="liveTextOnPin"
                label={t('ocr.liveTextOnPin')}
                description={t('ocr.liveTextOnPinDesc')}
                checked={settings.liveTextOnPin}
                onChange={(v) => updateSetting('liveTextOnPin', v)}
              />

              <Toggle
                id="pinLiveTextOnCapture"
                label={t('ocr.pinLiveTextOnCapture')}
                description={t('ocr.pinLiveTextOnCaptureDesc')}
                checked={settings.pinLiveTextOnCapture}
                onChange={(v) => updateSetting('pinLiveTextOnCapture', v)}
              />
            </Card>
          </SettingGroup>
        </div>
      )}

      {/* ── Tab 2: 原位与划词翻译 ──────────────────────────────────────────── */}
      {activeTab === 'translate' && (
        <div className="ocr-page__tab-panel">
          {translateHotkeyEntry && (
            <Card>
              <SettingRow
                label={
                  <div className="ocr-page__row-label">
                    <span>
                      {t('ocr.globalTranslateHotkey', 'Global Selection & Clipboard Translate Shortcut')}
                    </span>
                    <HotkeyStatusBadge entry={translateHotkeyEntry} />
                  </div>
                }
                description={t(
                  'ocr.globalTranslateHotkeyDesc',
                  'Translate selected text or clipboard content instantly in a floating overlay',
                )}
              >
                <HotkeyRecorder
                  id="ocr-global-translate-shortcut"
                  value={translateHotkeyEntry.shortcut}
                  onChange={(v) => void rebindNamedShortcut(translateHotkeyEntry.name, v)}
                />
              </SettingRow>
            </Card>
          )}

          <InPlaceTranslateCard />
        </div>
      )}

      {/* ── Tab 3: 离线引擎与模型包管理 ────────────────────────────────────── */}
      {activeTab === 'models' && (
        <div className="ocr-page__tab-panel">
          <SettingGroup
            title={t('ocr.tabModels', 'Offline Engines & Packs')}
            icon={<Package size={18} strokeWidth={2.2} />}
          >
            <p className="ocr-page__models-intro">
              {t(
                'ocr.tabModelsDesc',
                'Download or manage offline PP-OCR models, multi-language packs, and AI text erase models',
              )}
            </p>

            {/* 1. PP-OCRv4 核心包 */}
            <Card>
              <SettingRow
                layout="vertical"
                label={
                  <div className="ocr-page__row-label">
                    <span>{t('ocr.deepPackTitle')}</span>
                    {renderModelStatusBadge()}
                  </div>
                }
                description={t('ocr.deepPackDesc')}
              >
                <div className="ocr-page__pack-stack">
                  <div className="ocr-page__pack-toolbar">
                    {modelStatus.modelDir && (
                      <div className="ocr-page__pack-dir">
                        <span>{t('ocr.modelStorageDir')}</span>
                        <CodeBadge>{modelStatus.modelDir}</CodeBadge>
                      </div>
                    )}

                    <div className="ocr-page__pack-actions">
                      {modelStatus.downloading ? (
                        <Button
                          variant="secondary"
                          size="sm"
                          disabled={actionBusy}
                          onClick={() => void handleCancelDownload()}
                        >
                          <X size={15} strokeWidth={2.2} aria-hidden="true" />
                          <span>{t('ocr.cancelDownload')}</span>
                        </Button>
                      ) : !modelStatus.installed ? (
                        <Button
                          variant="primary"
                          size="sm"
                          disabled={actionBusy}
                          onClick={() => void handleStartDownload('mobile')}
                        >
                          {actionBusy ? (
                            <Loader2 size={15} strokeWidth={2.2} className="uikit-spin-icon" aria-hidden="true" />
                          ) : (
                            <Download size={15} strokeWidth={2.2} aria-hidden="true" />
                          )}
                          <span>{modelStatus.state === 'error' ? t('ocr.retryDownload') : t('ocr.downloadInstall')}</span>
                        </Button>
                      ) : null}
                    </div>
                  </div>

                  {modelStatus.downloading && (
                    <div className="ocr-page__progress-wrap">
                      <div
                        role="progressbar"
                        aria-valuemin={0}
                        aria-valuemax={100}
                        aria-valuenow={modelStatus.progressPercent}
                        className="ocr-page__progress-track"
                      >
                        <div
                          className="ocr-page__progress-fill"
                          style={{ width: `${Math.max(2, Math.min(100, modelStatus.progressPercent))}%` }}
                        />
                      </div>
                      <div className="ocr-page__progress-meta">
                        <span>
                          {modelStatus.currentFile
                            ? t('ocr.downloadingFile', { file: modelStatus.currentFile })
                            : t('ocr.deepStatusDownloading', { percent: modelStatus.progressPercent })}
                        </span>
                        <span>{`${modelStatus.progressPercent}%`}</span>
                      </div>
                    </div>
                  )}
                </div>
              </SettingRow>

              {/* 2. 识别精度档位 +90MB 增强包 */}
              <SettingRow
                layout="vertical"
                label={
                  <div className="ocr-page__row-label">
                    <span>{t('ocr.modelTier')}</span>
                    {renderServerTierBadge()}
                  </div>
                }
                description={t('ocr.modelTierDesc')}
              >
                <div className="ocr-page__pack-stack">
                  <div className="ocr-page__pack-toolbar">
                    <Select
                      id="ocr-model-tier"
                      value={settings.modelTier || 'mobile'}
                      onChange={(val) => updateSetting('modelTier', val)}
                      options={[
                        { value: 'mobile', label: t('ocr.tierMobile') },
                        { value: 'server', label: t('ocr.tierServer') },
                      ]}
                    />

                    <div className="ocr-page__pack-actions">
                      {modelStatus.downloading && modelStatus.downloadingServer ? (
                        <Button
                          variant="secondary"
                          size="sm"
                          disabled={actionBusy}
                          onClick={() => void handleCancelDownload()}
                        >
                          <X size={15} strokeWidth={2.2} aria-hidden="true" />
                          <span>{t('ocr.cancelDownload')}</span>
                        </Button>
                      ) : modelStatus.serverInstalled ? (
                        <Button
                          variant="secondary"
                          size="sm"
                          disabled={actionBusy || modelStatus.downloading}
                          onClick={() => void handleDeleteServerModel()}
                        >
                          <Trash2 size={15} strokeWidth={2.2} aria-hidden="true" />
                          <span>{t('ocr.deleteServerTier')}</span>
                        </Button>
                      ) : (
                        <Button
                          variant={settings.modelTier === 'server' ? 'primary' : 'secondary'}
                          size="sm"
                          disabled={actionBusy || modelStatus.downloading}
                          onClick={() => void handleStartDownload('server')}
                        >
                          {actionBusy ? (
                            <Loader2 size={15} strokeWidth={2.2} className="uikit-spin-icon" aria-hidden="true" />
                          ) : (
                            <Download size={15} strokeWidth={2.2} aria-hidden="true" />
                          )}
                          <span>{t('ocr.downloadServerTier')}</span>
                        </Button>
                      )}
                    </div>
                  </div>

                  {settings.modelTier === 'server' && !modelStatus.serverInstalled && !modelStatus.downloading && (
                    <div className="ocr-page__hint-note">
                      {t('ocr.tierServerFallbackHint')}
                    </div>
                  )}
                </div>
              </SettingRow>

              {/* 3. 4 国扩展包矩阵 */}
              <SettingRow
                layout="vertical"
                label={
                  <div className="ocr-page__row-label">
                    <Languages size={16} strokeWidth={2.2} aria-hidden="true" className="ocr-page__label-icon" />
                    <span>{t('ocr.scriptPackTitle')}</span>
                    {renderScriptPacksBadge()}
                  </div>
                }
                description={t('ocr.scriptPackDesc')}
              >
                <div className="ocr-page__pack-stack">
                  <div className="ocr-page__pack-toolbar">
                    <Select
                      id="ocr-models-script-mode"
                      value={settings.scriptMode || 'auto'}
                      onChange={(val) => updateSetting('scriptMode', val)}
                      options={scriptModeOptions}
                    />

                    <div className="ocr-page__pack-actions">
                      {isDownloadingScriptPack ? (
                        <Button
                          variant="secondary"
                          size="sm"
                          disabled={actionBusy}
                          onClick={() => void handleCancelDownload()}
                        >
                          <X size={15} strokeWidth={2.2} aria-hidden="true" />
                          <span>{t('ocr.cancelDownload')}</span>
                        </Button>
                      ) : installedScriptPackCount < scriptPacksList.length ? (
                        <Button
                          variant="secondary"
                          size="sm"
                          disabled={actionBusy || modelStatus.downloading}
                          onClick={() => void handleDownloadScriptPack('all')}
                        >
                          {actionBusy ? (
                            <Loader2 size={15} strokeWidth={2.2} className="uikit-spin-icon" aria-hidden="true" />
                          ) : (
                            <Download size={15} strokeWidth={2.2} aria-hidden="true" />
                          )}
                          <span>
                            {t('ocr.scriptPackDownloadAll', {
                              size: formatPackSizeMb(missingScriptPackBytes),
                            })}
                          </span>
                        </Button>
                      ) : null}
                    </div>
                  </div>

                  <div className="ocr-page__script-grid">
                    {SCRIPT_PACK_CATALOG.map((meta) => {
                      const pack = scriptPackMap.get(meta.id) ?? {
                        id: meta.id,
                        installed: false,
                        downloading: false,
                        fileBytes: 0,
                        estimatedBytes: 0,
                      };
                      const packSizeLabel = formatPackSizeMb(
                        pack.installed && pack.fileBytes > 0 ? pack.fileBytes : pack.estimatedBytes,
                      );
                      const thisPackDownloading =
                        isDownloadingScriptPack &&
                        (pack.downloading || modelStatus.downloadingScriptPack === meta.id);

                      return (
                        <div key={meta.id} className="ocr-page__script-card">
                          <div className="ocr-page__script-info">
                            <div className="ocr-page__script-head">
                              <span className="ocr-page__script-name">{t(meta.nameKey)}</span>
                              <span className="ocr-page__script-size">{packSizeLabel}</span>
                            </div>
                            <span className="ocr-page__script-desc">{t(meta.descKey)}</span>
                          </div>

                          <div className="ocr-page__script-actions">
                            {thisPackDownloading ? (
                              <Badge
                                text={t('ocr.deepStatusDownloading', { percent: modelStatus.progressPercent })}
                                variant="primary"
                              />
                            ) : pack.installed ? (
                              <>
                                <Badge text={t('ocr.scriptPackInstalled')} variant="success" />
                                <Button
                                  variant="secondary"
                                  size="sm"
                                  disabled={actionBusy || modelStatus.downloading}
                                  aria-label={t('ocr.scriptPackDeleteBtn')}
                                  onClick={() => void handleDeleteScriptPack(meta.id)}
                                >
                                  <Trash2 size={14} strokeWidth={2.2} aria-hidden="true" />
                                </Button>
                              </>
                            ) : (
                              <Button
                                variant={settings.scriptMode === meta.id ? 'primary' : 'secondary'}
                                size="sm"
                                disabled={actionBusy || modelStatus.downloading}
                                onClick={() => void handleDownloadScriptPack(meta.id)}
                              >
                                <Download size={14} strokeWidth={2.2} aria-hidden="true" />
                                <span>{t('ocr.scriptPackInstallBtn')}</span>
                              </Button>
                            )}
                          </div>
                        </div>
                      );
                    })}
                  </div>

                  {selectedScriptMissingPack && !modelStatus.downloading && (
                    <div className="ocr-page__hint-note">
                      {t('ocr.scriptPackNotInstalledHint')}
                    </div>
                  )}
                </div>
              </SettingRow>

              {/* 4. MI-GAN 512 修复引擎包 */}
              <SettingRow
                layout="vertical"
                label={
                  <div className="ocr-page__row-label">
                    <span>{t('ocr.inpaintPackTitle')}</span>
                    {renderInpaintStatusBadge()}
                  </div>
                }
                description={t('ocr.inpaintPackDesc')}
              >
                <div className="ocr-page__pack-stack">
                  <div className="ocr-page__pack-toolbar">
                    {inpaintStatus.modelDir && (
                      <div className="ocr-page__pack-dir">
                        <span>{t('ocr.modelStorageDir')}</span>
                        <CodeBadge>{inpaintStatus.modelDir}</CodeBadge>
                      </div>
                    )}

                    <div className="ocr-page__pack-actions">
                      {inpaintStatus.downloading ? (
                        <Button
                          variant="secondary"
                          size="sm"
                          disabled={inpaintBusy}
                          onClick={() => void handleCancelInpaintDownload()}
                        >
                          <X size={15} strokeWidth={2.2} aria-hidden="true" />
                          <span>{t('ocr.cancelDownload')}</span>
                        </Button>
                      ) : inpaintStatus.installed ? (
                        <Button
                          variant="secondary"
                          size="sm"
                          disabled={inpaintBusy}
                          onClick={() => void handleDeleteInpaintModel()}
                        >
                          <Trash2 size={15} strokeWidth={2.2} aria-hidden="true" />
                          <span>{t('ocr.inpaintDeleteModel')}</span>
                        </Button>
                      ) : (
                        <Button
                          variant="primary"
                          size="sm"
                          disabled={inpaintBusy}
                          onClick={() => void handleStartInpaintDownload()}
                        >
                          {inpaintBusy ? (
                            <Loader2 size={15} strokeWidth={2.2} className="uikit-spin-icon" aria-hidden="true" />
                          ) : (
                            <Download size={15} strokeWidth={2.2} aria-hidden="true" />
                          )}
                          <span>{inpaintStatus.state === 'error' ? t('ocr.retryDownload') : t('ocr.downloadInstall')}</span>
                        </Button>
                      )}
                    </div>
                  </div>

                  {inpaintStatus.downloading && (
                    <div className="ocr-page__progress-wrap">
                      <div
                        role="progressbar"
                        aria-valuemin={0}
                        aria-valuemax={100}
                        aria-valuenow={inpaintStatus.progressPercent}
                        className="ocr-page__progress-track"
                      >
                        <div
                          className="ocr-page__progress-fill"
                          style={{ width: `${Math.max(2, Math.min(100, inpaintStatus.progressPercent))}%` }}
                        />
                      </div>
                      <div className="ocr-page__progress-meta">
                        <span>
                          {inpaintStatus.modelFile
                            ? t('ocr.downloadingFile', { file: inpaintStatus.modelFile })
                            : t('ocr.inpaintStatusDownloading', { percent: inpaintStatus.progressPercent })}
                        </span>
                        <span>{`${inpaintStatus.progressPercent}%`}</span>
                      </div>
                    </div>
                  )}
                </div>
              </SettingRow>
            </Card>
          </SettingGroup>
        </div>
      )}
    </div>
  );
};

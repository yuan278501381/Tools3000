/* ─────────────────────────────────────────────────────────────────────────────
 * OcrPage — 双引擎高精度离线文字识别 (PP-OCRv6 + 增强版 Windows OCR) 与翻译设置页
 *
 * 顶栏 3 页签 MECE 信息架构:
 *   1. recognize: 文字识别（快捷键、动态联动引擎策略、紧凑离线状态条、识文后行为与贴图选字）
 *   2. translate: 划词与截图翻译（全局划词热键 + 弹窗式应用避让 + 3 场景渐进式翻译通道）
 *   3. models:    离线模型管理（主力推荐 PP-OCRv6 / 按需增强包 / 额外小语种包 3 张独立卡片）
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
  Modal,
  PageSkeleton,
  type TabItem,
} from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { HotkeyAppExclusionBar } from '../components/business/HotkeyAppExclusionBar';
import { InPlaceTranslateCard } from '../components/InPlaceTranslateCard';
import {
  consumePendingDeepNavigation,
  type DeepNavigationPayload,
  type OcrPageSubTab,
} from '../config/settingsSearchIndex';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import {
  FileText,
  Download,
  X,
  Sparkles,
  Loader2,
  Trash2,
  Languages,
  Package,
  Shield,
  FolderOpen,
  Info,
  Sliders,
} from 'lucide-react';
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
  engineGeneration?: string;
  upgradeAvailable?: boolean;
  downloading: boolean;
  downloadingServer?: boolean;
  downloadingScriptPack?: string;
  progressPercent: number;
  downloadedBytes: number;
  totalBytes: number;
  serverTotalBytes?: number;
  speedBytesPerSec?: number;
  activeMirrorHost?: string;
  mirrorIndex?: number;
  mirrorTotal?: number;
  phase?: 'probing' | 'downloading' | 'switching' | 'verifying' | string;
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
  speedBytesPerSec?: number;
  activeMirrorHost?: string;
  mirrorIndex?: number;
  mirrorTotal?: number;
  phase?: 'probing' | 'downloading' | 'switching' | 'verifying' | string;
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
  { id: 'korean', installed: false, downloading: false, fileBytes: 0, estimatedBytes: 13485000 },
  { id: 'cyrillic', installed: false, downloading: false, fileBytes: 0, estimatedBytes: 8075000 },
  { id: 'arabic', installed: false, downloading: false, fileBytes: 0, estimatedBytes: 8022000 },
  { id: 'thai', installed: false, downloading: false, fileBytes: 0, estimatedBytes: 7917000 },
];

const SCRIPT_PACK_CATALOG = [
  { id: 'korean', nameKey: 'ocr.scriptPackKoreanName', descKey: 'ocr.scriptPackKoreanDesc' },
  { id: 'cyrillic', nameKey: 'ocr.scriptPackCyrillicName', descKey: 'ocr.scriptPackCyrillicDesc' },
  { id: 'arabic', nameKey: 'ocr.scriptPackArabicName', descKey: 'ocr.scriptPackArabicDesc' },
  { id: 'thai', nameKey: 'ocr.scriptPackThaiName', descKey: 'ocr.scriptPackThaiDesc' },
] as const;

const formatPackSizeMb = (bytes: number): string => {
  const mb = Math.max(0.1, bytes / (1024 * 1024));
  return `${mb.toFixed(1)} MB`;
};

const formatDownloadSpeed = (bytesPerSec?: number): string => {
  if (!bytesPerSec || bytesPerSec <= 1024) return '';
  if (bytesPerSec >= 1024 * 1024) {
    return `${(bytesPerSec / (1024 * 1024)).toFixed(1)} MB/s`;
  }
  return `${Math.round(bytesPerSec / 1024)} KB/s`;
};

const formatTransferredBytes = (downloaded: number, total: number): string => {
  const dlMb = (Math.max(0, downloaded) / (1024 * 1024)).toFixed(1);
  if (total > 0 && total >= downloaded) {
    const totalMb = (total / (1024 * 1024)).toFixed(1);
    return `${dlMb} / ${totalMb} MB`;
  }
  return `${dlMb} MB`;
};

const normalizeOcrTab = (raw?: string): OcrPageSubTab => {
  if (raw === 'translate' || raw === 'models') return raw;
  return 'recognize';
};

const DEFAULT_MODEL_STATUS: DeepOcrModelStatus = {
  installed: false,
  serverInstalled: false,
  engineGeneration: '',
  upgradeAvailable: false,
  downloading: false,
  downloadingServer: false,
  downloadingScriptPack: '',
  progressPercent: 0,
  downloadedBytes: 0,
  totalBytes: 31750000,
  serverTotalBytes: 76630000,
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
  const [translateExclusionsOpen, setTranslateExclusionsOpen] = useState(false);
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
          ['auto', 'cjk', 'japan', 'korean', 'cyrillic', 'latin', 'arabic', 'thai'].includes(data.scriptMode)
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
            } else if (next.error) {
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

  const updateHotkeyExclusions = async (hotkeyName: string, apps: string[]) => {
    try {
      const res = await bridgeRequest<OperationResult>('hotkey.setHotkeyExclusions', {
        name: hotkeyName,
        apps,
      });
      if (res && res.success === false) {
        throw new Error(res.error || t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'));
      }
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) {
        setHotkeys(refreshed);
      }
    } catch (error) {
      toast.error(t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'), {
        description: String(error),
      });
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
    if (modelStatus.installed && modelStatus.upgradeAvailable) {
      return <Badge text={t('ocr.deepStatusUpgradeAvailable')} variant="primary" />;
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
    ['korean', 'cyrillic', 'arabic', 'thai'].includes(settings.scriptMode) &&
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

  const isLegacyV4 = modelStatus.engineGeneration === 'v4';
  const scriptModeOptions = isLegacyV4
    ? [
        { value: 'auto', label: t('ocr.scriptModeAuto') },
        { value: 'cjk', label: t('ocr.scriptModeCjk') },
        { value: 'japan', label: t('ocr.scriptModeJapan') },
        { value: 'korean', label: t('ocr.scriptModeKorean') },
        { value: 'cyrillic', label: t('ocr.scriptModeCyrillic') },
        { value: 'latin', label: t('ocr.scriptModeLatin') },
        { value: 'arabic', label: t('ocr.scriptModeArabic') },
        { value: 'thai', label: t('ocr.scriptModeThai') },
      ]
    : [
        { value: 'auto', label: t('ocr.scriptModeAuto') },
        { value: 'cjk', label: t('ocr.scriptModeCjk') },
        { value: 'korean', label: t('ocr.scriptModeKorean') },
        { value: 'cyrillic', label: t('ocr.scriptModeCyrillic') },
        { value: 'arabic', label: t('ocr.scriptModeArabic') },
        { value: 'thai', label: t('ocr.scriptModeThai') },
      ];

  const windowsLangOptions = [
    { value: 'auto', label: t('ocr.langAuto') },
    { value: 'zh-Hans-CN', label: t('ocr.langZhHans') },
    { value: 'zh-Hant-TW', label: t('ocr.langZhHant') },
    { value: 'en-US', label: t('ocr.langEnUs') },
    { value: 'ja-JP', label: t('ocr.langJaJp') },
    { value: 'ko-KR', label: t('ocr.langKoKr') },
    { value: 'fr-FR', label: t('ocr.langFrFr') },
    { value: 'de-DE', label: t('ocr.langDeDe') },
    { value: 'es-ES', label: t('ocr.langEsEs') },
    { value: 'pt-BR', label: t('ocr.langPtBr') },
    { value: 'ru-RU', label: t('ocr.langRuRu') },
    { value: 'vi-VN', label: t('ocr.langViVn') },
    { value: 'ar-SA', label: t('ocr.langArSa') },
    { value: 'th-TH', label: t('ocr.langThTh') },
  ];

  const isUsingWindowsOcr =
    settings.engine === 'windows' || (settings.engine === 'auto' && !modelStatus.installed);

  const resolvedEngineDesc = (() => {
    const mode = settings.engine || 'auto';
    if (mode === 'windows') {
      return t('ocr.engineDescWindows', 'Uses built-in Windows OCR only (zero extra disk usage; suitable for clean text and standard documents)');
    }
    if (mode === 'ppocr_v4') {
      return t('ocr.engineDescPpOcr', 'Always uses the offline PP-OCRv6 engine (50 languages; high accuracy on photos, posters, tables, and code)');
    }
    if (modelStatus.installed) {
      return t('ocr.engineDescAutoDeep', 'Currently using the installed PP-OCRv6 offline engine (50 languages; runs 100% locally)');
    }
    return t('ocr.engineDescAutoWindows', 'Currently using built-in Windows OCR (no download needed); will auto-upgrade after downloading the PP-OCRv6 offline pack (~30 MB)');
  })();

  const translateHotkeyEntry =
    hotkeys.find((h) => h.name === 'Quick Translate') ??
    hotkeys.find((h) => h.name === 'Translate Selection');

  const tabs: TabItem<OcrPageSubTab>[] = [
    { id: 'recognize', label: t('ocr.tabRecognize', 'Text Recognition'), icon: <FileText size={16} /> },
    { id: 'translate', label: t('ocr.tabTranslate', 'Selection & Screenshot Translate'), icon: <Languages size={16} /> },
    { id: 'models', label: t('ocr.tabModels', 'Offline Models'), icon: <Package size={16} /> },
  ];

  const isDownloadingCoreBase =
    modelStatus.downloading && !modelStatus.downloadingServer && !modelStatus.downloadingScriptPack;
  const isDownloadingServerPack = Boolean(modelStatus.downloading && modelStatus.downloadingServer);

  const renderDownloadProgress = (
    progressPercent: number,
    downloadedBytes: number,
    totalBytes: number,
    fileLabel: string,
    phase?: string,
    speedBytesPerSec?: number,
    activeMirrorHost?: string,
    mirrorIndex?: number,
    mirrorTotal?: number,
  ) => {
    let stageLabel = fileLabel
      ? t('ocr.downloadingFile', { file: fileLabel })
      : t('ocr.deepStatusDownloading', { percent: progressPercent });
    if (phase === 'probing') {
      stageLabel = t('ocr.downloadPhaseProbing', 'Testing fastest download mirror...');
    } else if (phase === 'switching') {
      stageLabel = t('ocr.downloadPhaseSwitching', {
        index: mirrorIndex || 2,
        total: mirrorTotal || 2,
        defaultValue: 'Current mirror is slow, switching to fallback mirror ({{index}}/{{total}})...',
      });
    } else if (phase === 'verifying') {
      stageLabel = t('ocr.downloadPhaseVerifying', 'Verifying offline model integrity...');
    }

    const speedText = phase === 'downloading' || !phase ? formatDownloadSpeed(speedBytesPerSec) : '';
    const transferText = downloadedBytes > 0 ? formatTransferredBytes(downloadedBytes, totalBytes) : '';
    const rightParts = [transferText, speedText, `${progressPercent}%`].filter(Boolean);

    return (
      <div className="ocr-page__progress-wrap">
        <div
          role="progressbar"
          aria-valuemin={0}
          aria-valuemax={100}
          aria-valuenow={progressPercent}
          className="ocr-page__progress-track"
        >
          <div
            className="ocr-page__progress-fill"
            style={{ width: `${Math.max(2, Math.min(100, progressPercent))}%` }}
          />
        </div>
        <div className="ocr-page__progress-meta">
          <span className="ocr-page__progress-stage">
            {stageLabel}
            {activeMirrorHost && phase !== 'probing' && phase !== 'verifying'
              ? ` · ${t('ocr.downloadMirrorTag', { host: activeMirrorHost, defaultValue: 'Mirror: {{host}}' })}`
              : ''}
          </span>
          <span className="ocr-page__progress-stats">{rightParts.join(' · ')}</span>
        </div>
      </div>
    );
  };

  return (
    <div className="ocr-page">
      <Tabs
        tabs={tabs}
        activeId={activeTab}
        onChange={(id) => setActiveTab(id)}
        ariaLabel={t('ocr.title')}
      />

      {/* ── Tab 1: 文字识别 (清爽直白首屏，零重复信息) ────────────────────── */}
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
                description={resolvedEngineDesc}
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
                </div>
              </SettingRow>

              <SubSettingGroup>
                {isUsingWindowsOcr ? (
                  <>
                    <SettingRow label={t('ocr.language')} description={t('ocr.languageDesc')}>
                      <Select
                        id="ocr-language"
                        value={settings.language || 'auto'}
                        onChange={(val) => updateSetting('language', val)}
                        options={windowsLangOptions}
                      />
                    </SettingRow>

                    {!modelStatus.installed && (
                      <div className="ocr-page__engine-banner">
                        <div className="ocr-page__engine-banner-text">
                          <span className="ocr-page__engine-banner-title">
                            {t('ocr.bannerNotInstalledTitle', 'Need higher accuracy for photos, tables, code, or 50+ languages?')}
                          </span>
                          <span className="ocr-page__engine-banner-desc">
                            {t('ocr.bannerNotInstalledDesc', 'Download the PP-OCRv6 offline pack (~30 MB, recommended). Runs 100% locally with no extra runtime.')}
                          </span>
                        </div>
                        <Button
                          variant="primary"
                          size="sm"
                          disabled={actionBusy || modelStatus.downloading}
                          onClick={() => {
                            setActiveTab('models');
                            if (!modelStatus.downloading) {
                              void handleStartDownload('mobile');
                            }
                          }}
                        >
                          <Download size={14} aria-hidden="true" />
                          <span>{t('ocr.bannerDownloadV6Btn', 'Download PP-OCRv6 (~30 MB)')}</span>
                        </Button>
                      </div>
                    )}
                  </>
                ) : (
                  <div className="ocr-page__engine-banner">
                    <div className="ocr-page__engine-banner-text">
                      <div className="ocr-page__row-label">
                        <span className="ocr-page__engine-banner-title">
                          {modelStatus.activeModelTier === 'server'
                            ? t('ocr.bannerActiveServerTitle', 'PP-OCRv6 + High-Accuracy Refinement Active')
                            : t('ocr.bannerActiveStandardTitle', 'PP-OCRv6 Offline Pack Active (50 Languages Built-In)')}
                        </span>
                        {renderModelStatusBadge()}
                      </div>
                      <span className="ocr-page__engine-banner-desc">
                        {installedScriptPackCount > 0
                          ? t('ocr.bannerWithPacksDesc', 'Includes {{count}} extra language pack(s). Manage refinement and language packs in Offline Models.', {
                              count: installedScriptPackCount,
                            })
                          : t('ocr.bannerStandardDesc', 'Covers Simplified/Traditional Chinese, English, Japanese, and European languages locally. Manage optional packs in Offline Models.')}
                      </span>
                    </div>
                    <div className="ocr-page__engine-banner-actions">
                      {installedScriptPackCount > 0 && (
                        <Select
                          id="ocr-script-mode"
                          value={settings.scriptMode || 'auto'}
                          onChange={(val) => updateSetting('scriptMode', val)}
                          options={scriptModeOptions}
                        />
                      )}
                      <Button
                        variant="secondary"
                        size="sm"
                        onClick={() => setActiveTab('models')}
                      >
                        <Package size={14} aria-hidden="true" />
                        <span>{t('ocr.manageModelsBtn', 'Manage Offline Models')}</span>
                      </Button>
                    </div>
                  </div>
                )}
              </SubSettingGroup>
            </Card>
          </SettingGroup>

          <SettingGroup
            title={t('ocr.outputAndLiveTextGroup', 'After Recognition & Pin Live Text')}
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

      {/* ── Tab 2: 划词与截图翻译 (统一作用域声明 + 紧凑快捷键 + 渐进式通道) ──── */}
      {activeTab === 'translate' && (
        <div className="ocr-page__tab-panel">
          <div className="ocr-page__scope-banner" role="note">
            <Info size={15} className="ocr-page__scope-banner-icon" aria-hidden="true" />
            <span>
              {t(
                'ocr.translateSharedScopeHint',
                'The target languages and translation channel below apply to both Global Selection Translation and Screenshot In-Place Translation.',
              )}
            </span>
          </div>

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
                  'Press this shortcut after selecting text or copying to open a quick translation popup',
                )}
              >
                <div className="ocr-page__hotkey-actions">
                  <button
                    type="button"
                    className={`ocr-page__exclude-btn ${(translateHotkeyEntry.excludedApps?.length ?? 0) > 0 ? 'has-apps' : ''}`}
                    disabled={!translateHotkeyEntry.shortcut}
                    onClick={() => setTranslateExclusionsOpen(true)}
                    title={t('ocr.translateExcludedAppsTitle', 'Auto-Yield in Specific Apps')}
                  >
                    <Shield size={13} aria-hidden="true" />
                    <span>
                      {(translateHotkeyEntry.excludedApps?.length ?? 0) > 0
                        ? t('ocr.yieldAppsCountBtn', 'Yield Apps ({{count}})', {
                            count: translateHotkeyEntry.excludedApps!.length,
                          })
                        : t('ocr.yieldAppsBtn', 'Yield Apps')}
                    </span>
                  </button>
                  <HotkeyRecorder
                    id="ocr-global-translate-shortcut"
                    value={translateHotkeyEntry.shortcut}
                    onChange={(v) => void rebindNamedShortcut(translateHotkeyEntry.name, v)}
                  />
                </div>
              </SettingRow>
            </Card>
          )}

          {translateExclusionsOpen && translateHotkeyEntry && (
            <Modal
              open
              title={t('ocr.translateExcludedAppsTitle', 'Auto-Yield in Specific Apps')}
              onClose={() => setTranslateExclusionsOpen(false)}
              footer={
                <Button variant="primary" onClick={() => setTranslateExclusionsOpen(false)}>
                  {t('common.done', 'Done')}
                </Button>
              }
            >
              <p className="ocr-page__modal-desc">
                {t(
                  'ocr.translateExcludedAppsDesc',
                  'Temporarily releases this shortcut when these apps are focused (e.g. ssms.exe for Execute SQL, emacs.exe for M-x) while keeping all other Tools3000 shortcuts active.',
                )}
              </p>
              <HotkeyAppExclusionBar
                apps={translateHotkeyEntry.excludedApps ?? []}
                disabled={!translateHotkeyEntry.shortcut}
                emptyText={t('hotkeyExclusion.emptyPerHotkey', 'Active in all apps; add process names to auto-yield')}
                pickerTitle={t('hotkeyExclusion.pickerPerHotkeyTitle', 'Pick App to Auto-Yield This Shortcut')}
                onChange={(nextApps) => void updateHotkeyExclusions(translateHotkeyEntry.name, nextApps)}
              />
            </Modal>
          )}

          <InPlaceTranslateCard />
        </div>
      )}

      {/* ── Tab 3: 离线模型管理 (按「推荐主力 / 按需增强 / 额外语种」3 张独立卡) ── */}
      {activeTab === 'models' && (
        <div className="ocr-page__tab-panel">
          {/* 卡 1: 主力离线识字引擎 (PP-OCRv6 · 约 30 MB · 推荐) */}
          <SettingGroup
            title={t('ocr.modelsCoreGroupTitle', 'Core Offline Recognition Engine')}
            icon={<Package size={18} strokeWidth={2.2} />}
            actions={
              modelStatus.modelDir ? (
                <Button
                  variant="ghost"
                  size="sm"
                  onClick={() => void bridgeRequest('system.openFolder', { path: modelStatus.modelDir })}
                  title={modelStatus.modelDir}
                >
                  <FolderOpen size={14} aria-hidden="true" />
                  <span>{t('ocr.openModelDirBtn', 'Open Model Folder')}</span>
                </Button>
              ) : undefined
            }
          >
            <Card>
              <SettingRow
                layout="vertical"
                label={
                  <div className="ocr-page__row-label">
                    <span>{t('ocr.deepPackTitle')}</span>
                    <Badge text={t('ocr.recommendedBadge', 'Recommended')} variant="primary" />
                    {renderModelStatusBadge()}
                  </div>
                }
                description={t('ocr.deepPackDesc')}
              >
                <div className="ocr-page__pack-stack">
                  <div className="ocr-page__pack-toolbar">
                    <span className="ocr-page__pack-summary">
                      {t(
                        'ocr.deepPackImpactNote',
                        'Without it: uses built-in Windows OCR (clean text). Installed (~30 MB): 50 languages locally with high accuracy across screenshots, photos, posters, tables, and code.',
                      )}
                    </span>

                    <div className="ocr-page__pack-actions">
                      {modelStatus.downloading && !modelStatus.downloadingServer && !modelStatus.downloadingScriptPack ? (
                        <Button
                          variant="secondary"
                          size="sm"
                          disabled={actionBusy}
                          onClick={() => void handleCancelDownload()}
                        >
                          <X size={15} strokeWidth={2.2} aria-hidden="true" />
                          <span>{t('ocr.cancelDownload')}</span>
                        </Button>
                      ) : !modelStatus.installed || modelStatus.upgradeAvailable ? (
                        <Button
                          variant="primary"
                          size="sm"
                          disabled={actionBusy || modelStatus.downloading}
                          onClick={() => void handleStartDownload('mobile')}
                        >
                          {actionBusy ? (
                            <Loader2 size={15} strokeWidth={2.2} className="uikit-spin-icon" aria-hidden="true" />
                          ) : (
                            <Download size={15} strokeWidth={2.2} aria-hidden="true" />
                          )}
                          <span>
                            {modelStatus.upgradeAvailable
                              ? t('ocr.upgradeToV6Btn')
                              : modelStatus.state === 'error'
                                ? t('ocr.retryDownload')
                                : t('ocr.downloadInstall')}
                          </span>
                        </Button>
                      ) : null}
                    </div>
                  </div>

                  {isDownloadingCoreBase &&
                    renderDownloadProgress(
                      modelStatus.progressPercent,
                      modelStatus.downloadedBytes,
                      modelStatus.totalBytes,
                      modelStatus.currentFile,
                      modelStatus.phase,
                      modelStatus.speedBytesPerSec,
                      modelStatus.activeMirrorHost,
                      modelStatus.mirrorIndex,
                      modelStatus.mirrorTotal,
                    )}
                </div>
              </SettingRow>
            </Card>
          </SettingGroup>

          {/* 卡 2: 按需增强包（高精度精修 +73 MB + 贴图改字背景补全 ~27 MB） */}
          <SettingGroup
            title={t('ocr.modelsOptionalGroupTitle', 'Optional Enhancement Packs')}
            icon={<Sliders size={18} strokeWidth={2.2} />}
          >
            <Card>
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
                      {isDownloadingServerPack ? (
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

                  {isDownloadingServerPack &&
                    renderDownloadProgress(
                      modelStatus.progressPercent,
                      modelStatus.downloadedBytes,
                      modelStatus.totalBytes,
                      modelStatus.currentFile,
                      modelStatus.phase,
                      modelStatus.speedBytesPerSec,
                      modelStatus.activeMirrorHost,
                      modelStatus.mirrorIndex,
                      modelStatus.mirrorTotal,
                    )}

                  {settings.modelTier === 'server' && !modelStatus.serverInstalled && !modelStatus.downloading && (
                    <div className="ocr-page__hint-note">
                      {t('ocr.tierServerFallbackHint')}
                    </div>
                  )}
                </div>
              </SettingRow>

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
                    <span className="ocr-page__pack-summary">
                      {t(
                        'ocr.inpaintPackImpactNote',
                        'Solid backgrounds, documents, and tables work without downloading. Only install (~27 MB) if you edit text over complex photos or posters.',
                      )}
                    </span>

                    <div className="ocr-page__pack-actions">
                      {inpaintStatus.modelDir && inpaintStatus.installed && (
                        <Button
                          variant="ghost"
                          size="sm"
                          onClick={() => void bridgeRequest('system.openFolder', { path: inpaintStatus.modelDir })}
                          title={inpaintStatus.modelDir}
                        >
                          <FolderOpen size={14} aria-hidden="true" />
                          <span>{t('ocr.openModelDirBtn', 'Open Model Folder')}</span>
                        </Button>
                      )}
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
                          variant="secondary"
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

                  {inpaintStatus.downloading &&
                    renderDownloadProgress(
                      inpaintStatus.progressPercent,
                      inpaintStatus.downloadedBytes,
                      inpaintStatus.totalBytes,
                      inpaintStatus.modelFile,
                      inpaintStatus.phase,
                      inpaintStatus.speedBytesPerSec,
                      inpaintStatus.activeMirrorHost,
                      inpaintStatus.mirrorIndex,
                      inpaintStatus.mirrorTotal,
                    )}
                </div>
              </SettingRow>
            </Card>
          </SettingGroup>

          {/* 卡 3: 额外小语种识别包（韩 / 俄 / 阿 / 泰 · 按需单选） */}
          <SettingGroup
            title={t('ocr.scriptPackTitle')}
            icon={<Languages size={18} strokeWidth={2.2} />}
          >
            <Card>
              <SettingRow
                layout="vertical"
                label={
                  <div className="ocr-page__row-label">
                    <span>{t('ocr.scriptModeLabel', 'Language Recognition Mode')}</span>
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

                  {isDownloadingScriptPack &&
                    renderDownloadProgress(
                      modelStatus.progressPercent,
                      modelStatus.downloadedBytes,
                      modelStatus.totalBytes,
                      modelStatus.currentFile,
                      modelStatus.phase,
                      modelStatus.speedBytesPerSec,
                      modelStatus.activeMirrorHost,
                      modelStatus.mirrorIndex,
                      modelStatus.mirrorTotal,
                    )}

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
            </Card>
          </SettingGroup>
        </div>
      )}
    </div>
  );
};

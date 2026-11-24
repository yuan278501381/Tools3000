/* ─────────────────────────────────────────────────────────────────────────────
 * CapturePage — 截图录屏设置页 (顶栏 4 页签分流 + SubSettingGroup 父子从属缩进架构)
 *
 * 从 C++ ConfigManager 加载截图/录屏设置，修改后实时 IPC 保存。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import {
  Card,
  Toggle,
  SettingRow,
  SettingGroup,
  SubSettingGroup,
  Tabs,
  TextInput,
  Select,
  Button,
  PageSkeleton,
  type TabItem,
} from '../components/UIKit';
import {
  consumePendingDeepNavigation,
  type CapturePageSubTab,
  type DeepNavigationPayload,
} from '../config/settingsSearchIndex';
import { bridgeRequest } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import {
  Camera,
  Video,
  Palette,
  FolderOpen,
  Sparkles,
  ShieldCheck,
  Crop,
  Crosshair,
  Mic,
  MousePointerClick,
  Monitor,
  VolumeX,
  Zap,
} from 'lucide-react';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { toast } from 'sonner';
import { PinGroupSettingsCard } from '../components/PinGroupSettingsCard';
import { CaptureRegionManagerCard } from '../components/CaptureRegionManagerCard';
import './CapturePage.css';

interface CaptureSettings {
  format: string;
  quality: number;
  saveToFile: boolean;
  copyToClipboard: boolean;
  includeCursor?: boolean;
  showCrosshair: boolean;
  autoDetectWindow: boolean;
  showShortcutHints: boolean;
  autoBypassFullscreen?: boolean;
  beautyShellEnabled?: boolean;
  beautyShellTheme?: string;
  beautyShellPadding?: number;
  beautyShellRadius?: number;
  watermarkEnabled?: boolean;
  watermarkMode?: number;
  watermarkImagePath?: string;
  watermarkText?: string;
  watermarkAnchor?: number;
  watermarkScale?: number;
  watermarkOpacity?: number;
  watermarkPolicyForced?: boolean;
  shortcut?: string;
  saveDirectory?: string;
  regionHistoryLimit?: number;
  regionHistoryKey?: string;
  regionHistoryCount?: number;
}

interface RecordingSettings {
  format: string;
  fps: number;
  bitrate: number;
  saveDirectory?: string;
  includeCursor: boolean;
  showClickEffects: boolean;
  showClickZoom?: boolean;
  includeKeycast?: boolean;
  captureSystemAudio: boolean;
  captureMicrophone: boolean;
  systemAudioDeviceId: string;
  microphoneDeviceId: string;
  systemAudioVolume: number;
  microphoneVolume: number;
  countdownSeconds: number;
  voiceEnhance?: boolean;
  hideIndicatorInCapture?: boolean;
  hideDesktopIcons?: boolean;
  hideTaskbar?: boolean;
  typingAutoZoom?: boolean;
  studioMode?: boolean;
  studioAutoZoom?: boolean;
  studioZoomMode?: number;
  studioZoomFactor?: number;
  studioZoomPacing?: number;
  studioZoomSensitivity?: number;
  studioFocusSpotlightDim?: number;
  studioLockCameraHold?: boolean;
  studioMotionBlur?: boolean;
  studioSmoothCursor?: boolean;
  studioCursorScale?: number;
  studioClickStyle?: number;
  studioAutoFrame?: boolean;
  studioTheme?: string;
  studioAutoCaptions?: boolean;
  studioExportFormat?: number;
  studioExportResolution?: number;
  studioExportAutoClose?: boolean;
  studioWatermarkEnabled?: boolean;
  studioWatermarkMode?: number;
  studioWatermarkImagePath?: string;
  studioWatermarkText?: string;
  studioWatermarkAnchor?: number;
  studioWatermarkScale?: number;
  studioWatermarkOpacity?: number;
  studioWatermarkPolicyForced?: boolean;
}

interface AudioDeviceInfo {
  id: string;
  name: string;
  systemAudio: boolean;
  defaultDevice: boolean;
}

interface RecordingCapabilities { audioDevices: AudioDeviceInfo[] }

interface OperationResult {
  success: boolean;
  error?: string;
  shortcut?: string;
  conflictType?: string;
  conflictWith?: string;
}

const VALID_CAPTURE_TABS: ReadonlySet<string> = new Set<CapturePageSubTab>([
  'screenshot',
  'pin_region',
  'recording',
  'studio',
]);

function resolveCaptureTab(raw?: string): CapturePageSubTab | null {
  return raw && VALID_CAPTURE_TABS.has(raw) ? (raw as CapturePageSubTab) : null;
}

const LEGACY_BEAUTY_THEME_MAP: Record<string, string> = {
  studio_slate: 'aurora_purple',
  paper_chalk: 'sunrise_coral',
  silk_mist: 'emerald_sea',
  midnight_graphite: 'obsidian_dark',
  pure_minimal: 'minimal_light',
};

function normalizeBeautyShellTheme(raw?: string): string {
  if (!raw) return 'aurora_purple';
  return LEGACY_BEAUTY_THEME_MAP[raw] ?? raw;
}

const BEAUTY_SHELL_PADDING_PRESETS = [16, 24, 32, 48, 64, 72] as const;
const BEAUTY_SHELL_RADIUS_PRESETS = [0, 8, 10, 12, 16, 24, 32] as const;
const RECORDING_BITRATE_PRESETS = [4, 8, 16, 32] as const;
const WATERMARK_SCALE_PRESETS = [0.6, 0.8, 1.0, 1.25, 1.5, 2.0] as const;
const WATERMARK_OPACITY_PRESETS = [0.08, 0.12, 0.16, 0.25, 0.4, 0.65, 0.85, 1.0] as const;

export const CapturePage: FC = () => {
  const { t } = useTranslation();
  const [activeTab, setActiveTab] = useState<CapturePageSubTab>(
    () => resolveCaptureTab(consumePendingDeepNavigation('capture')?.tabId) ?? 'screenshot',
  );
  const [capture, setCapture] = useState<CaptureSettings>({
    format: 'png', quality: 90, saveToFile: true, copyToClipboard: true,
    includeCursor: false,
    showCrosshair: false, autoDetectWindow: true, showShortcutHints: true,
    autoBypassFullscreen: true,
    beautyShellEnabled: false, beautyShellTheme: 'aurora_purple',
    beautyShellPadding: 32, beautyShellRadius: 16,
    watermarkEnabled: false, watermarkMode: 2, watermarkImagePath: '',
    watermarkText: '{user} · {host} · {date} {time}',
    watermarkAnchor: 3, watermarkScale: 1.0, watermarkOpacity: 0.16,
    watermarkPolicyForced: false,
    regionHistoryLimit: 10, regionHistoryKey: 'R', regionHistoryCount: 0,
  });
  const [recording, setRecording] = useState<RecordingSettings>({
    format: 'mp4_h264', fps: 30, bitrate: 8, includeCursor: true, showClickEffects: false,
    showClickZoom: true, includeKeycast: true,
    captureSystemAudio: false, captureMicrophone: false,
    systemAudioDeviceId: '', microphoneDeviceId: '',
    systemAudioVolume: 100, microphoneVolume: 100,
    countdownSeconds: 3,
    voiceEnhance: true, hideIndicatorInCapture: true,
    hideDesktopIcons: false, hideTaskbar: false, typingAutoZoom: false,
    studioMode: true, studioAutoZoom: true, studioZoomMode: 1, studioZoomFactor: 1.32,
    studioZoomPacing: 0.32, studioZoomSensitivity: 0.32, studioFocusSpotlightDim: 0.18,
    studioLockCameraHold: true,
    studioMotionBlur: false,
    studioSmoothCursor: true, studioCursorScale: 1.35, studioClickStyle: 0,
    studioAutoFrame: true, studioTheme: 'blossom_silk', studioAutoCaptions: false,
    studioExportFormat: 0, studioExportResolution: 3, studioExportAutoClose: false,
    studioWatermarkEnabled: false, studioWatermarkMode: 0, studioWatermarkImagePath: '',
    studioWatermarkText: '{user} · {date} {time}', studioWatermarkAnchor: 3,
    studioWatermarkScale: 1.0, studioWatermarkOpacity: 0.88, studioWatermarkPolicyForced: false,
  });
  const [audioDevices, setAudioDevices] = useState<AudioDeviceInfo[]>([]);
  const [loading, setLoading] = useState(true);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [screenshotHotkey, setScreenshotHotkey] = useState('F1');
  const [repeatCaptureHotkey, setRepeatCaptureHotkey] = useState('Shift+F1');
  const [recordHotkey, setRecordHotkey] = useState('Ctrl+Shift+R');
  const [recordPauseHotkey, setRecordPauseHotkey] = useState('Ctrl+Shift+P');
  const [recordSnapshotHotkey, setRecordSnapshotHotkey] = useState('Ctrl+Shift+S');

  useEffect(() => {
    const onDeepNavigate = (e: Event) => {
      const detail = (e as CustomEvent<DeepNavigationPayload>).detail;
      if (detail?.navId === 'capture' && detail.tabId) {
        consumePendingDeepNavigation('capture');
        const nextTab = resolveCaptureTab(detail.tabId);
        if (nextTab) {
          setActiveTab(nextTab);
        }
      }
    };
    window.addEventListener('tools3000:navigate-deep', onDeepNavigate);
    return () => window.removeEventListener('tools3000:navigate-deep', onDeepNavigate);
  }, []);

  const getHotkey = (name: string) => hotkeys.find(h => h.name === name);

  const syncSettingsSilently = useCallback(() => {
    Promise.all([
      bridgeRequest<CaptureSettings>('capture.getSettings'),
      bridgeRequest<RecordingSettings>('recording.getSettings'),
    ]).then(([capData, recData]) => {
      setCapture(prev => ({
        ...prev,
        ...capData,
        beautyShellTheme: normalizeBeautyShellTheme(capData.beautyShellTheme ?? prev.beautyShellTheme),
      }));
      setRecording(prev => ({ ...prev, ...recData }));
    }).catch(() => { /* silent background sync */ });
  }, []);

  useEffect(() => {
    Promise.all([
      bridgeRequest<CaptureSettings>('capture.getSettings'),
      bridgeRequest<RecordingSettings>('recording.getSettings'),
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll'),
      bridgeRequest<RecordingCapabilities>('recording.getCapabilities')
        .catch(() => ({ audioDevices: [] })),
    ]).then(([capData, recData, hotkeyData, capabilities]) => {
      setCapture(prev => ({
        ...prev,
        ...capData,
        beautyShellTheme: normalizeBeautyShellTheme(capData.beautyShellTheme ?? prev.beautyShellTheme),
      }));
      setRecording(prev => ({ ...prev, ...recData }));
      const hkList = Array.isArray(hotkeyData) ? hotkeyData : [];
      setHotkeys(hkList);
      setScreenshotHotkey(hkList.find(item => item.name === 'Screenshot')?.shortcut ?? 'F1');
      setRepeatCaptureHotkey(hkList.find(item => item.name === 'Repeat Last Capture')?.shortcut ?? 'Shift+F1');
      setRecordHotkey(hkList.find(item => item.name === 'Record')?.shortcut ?? 'Ctrl+Shift+R');
      setRecordPauseHotkey(hkList.find(item => item.name === 'Record Pause')?.shortcut ?? 'Ctrl+Shift+P');
      setRecordSnapshotHotkey(hkList.find(item => item.name === 'Record Snapshot')?.shortcut ?? 'Ctrl+Shift+S');
      setAudioDevices(capabilities.audioDevices || []);
    }).catch((error) => {
      console.error(error);
      toast.error(t('capture.loadFailed'));
    }).finally(() => setLoading(false));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const onFocus = () => syncSettingsSilently();
    const onVisibility = () => {
      if (document.visibilityState === 'visible') {
        syncSettingsSilently();
      }
    };
    window.addEventListener('focus', onFocus);
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      window.removeEventListener('focus', onFocus);
      document.removeEventListener('visibilitychange', onVisibility);
    };
  }, [syncSettingsSilently]);

  const updateCapture = useCallback((key: keyof CaptureSettings, value: unknown) => {
    setCapture(prev => ({ ...prev, [key]: value }));
    bridgeRequest<OperationResult>('capture.updateSettings', { [key]: value })
      .then(result => { if (!result.success) throw new Error(result.error || 'update failed'); })
      .catch(async (error) => {
        toast.error(t('capture.saveFailed'), { description: String(error) });
        try {
          const latest = await bridgeRequest<CaptureSettings>('capture.getSettings');
          setCapture({
            ...latest,
            beautyShellTheme: normalizeBeautyShellTheme(latest.beautyShellTheme),
          });
        } catch { /* keep usable */ }
      });
  }, [t]);

  const updateRecording = useCallback((key: keyof RecordingSettings, value: unknown) => {
    setRecording(prev => ({ ...prev, [key]: value }));
    bridgeRequest<OperationResult>('recording.updateSettings', { [key]: value })
      .then(result => { if (!result.success) throw new Error(result.error || 'update failed'); })
      .catch(async (error) => {
        toast.error(t('capture.saveFailed'), { description: String(error) });
        try { setRecording(await bridgeRequest<RecordingSettings>('recording.getSettings')); } catch { /* keep usable */ }
      });
  }, [t]);

  const updateRecordingPatch = useCallback((patch: Partial<RecordingSettings>) => {
    setRecording(prev => ({ ...prev, ...patch }));
    bridgeRequest<OperationResult>('recording.updateSettings', patch)
      .then(result => { if (!result.success) throw new Error(result.error || 'update failed'); })
      .catch(async (error) => {
        toast.error(t('capture.saveFailed'), { description: String(error) });
        try { setRecording(await bridgeRequest<RecordingSettings>('recording.getSettings')); } catch { /* keep usable */ }
      });
  }, [t]);

  const rebindHotkey = async (
    name: 'Screenshot' | 'Repeat Last Capture' | 'Record' | 'Record Pause' | 'Record Snapshot',
    value: string,
  ) => {
    try {
      const result = await bridgeRequest<OperationResult>('hotkey.rebind', { name, hotkey: value });
      if (!result.success) throw new Error(result.error || t('hotkey.bindFailed'));
      if (name === 'Screenshot') setScreenshotHotkey(result.shortcut ?? value);
      else if (name === 'Repeat Last Capture') setRepeatCaptureHotkey(result.shortcut ?? value);
      else if (name === 'Record') setRecordHotkey(result.shortcut ?? value);
      else if (name === 'Record Pause') setRecordPauseHotkey(result.shortcut ?? value);
      else setRecordSnapshotHotkey(result.shortcut ?? value);

      // 同步刷新全局热键状态以更新冲突徽章
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) setHotkeys(refreshed);
    } catch (error) {
      toast.error(t('hotkey.bindFailed'), { description: String(error) });
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) setHotkeys(refreshed);
    }
  };

  const handleBrowseDir = async () => {
    try {
      const dir = await bridgeRequest<string | null>('capture.browseDirectory');
      if (dir) updateCapture('saveDirectory', dir);
    } catch (error) {
      toast.error(t('capture.browseFailed'), { description: String(error) });
    }
  };

  const handleBrowseRecordingDir = async () => {
    try {
      const dir = await bridgeRequest<string | null>('capture.browseDirectory');
      if (dir) updateRecording('saveDirectory', dir);
    } catch (error) {
      toast.error(t('capture.browseFailed'), { description: String(error) });
    }
  };

  const handleOpenSaveFolder = async (dirPath: string | undefined, kind: 'capture' | 'recording') => {
    try {
      const res = await bridgeRequest<{ success?: boolean; error?: string }>('capture.openSaveDirectory', {
        path: dirPath?.trim() || '',
        kind,
      });
      if (!res?.success) {
        throw new Error(res?.error || t('capture.openFolderFailed'));
      }
    } catch (error) {
      toast.error(t('capture.openFolderFailed'), { description: String(error) });
    }
  };

  const handleTryCapture = async () => {
    try {
      const result = await bridgeRequest<OperationResult>('capture.triggerScreenshot');
      if (!result.success) throw new Error(result.error || t('capture.startFailed'));
    } catch (error) {
      toast.error(t('capture.startFailed'), { description: String(error) });
    }
  };

  const handlePickWatermarkImage = async () => {
    try {
      const res = await bridgeRequest<{ success?: boolean; path?: string; cancelled?: boolean; error?: string }>(
        'capture.pickWatermarkImage',
      );
      if (res?.cancelled) return;
      if (!res?.success || !res.path) {
        throw new Error(res?.error || t('capture.browseFailed'));
      }
      updateCapture('watermarkImagePath', res.path);
    } catch (error) {
      toast.error(t('capture.browseFailed'), { description: String(error) });
    }
  };

  const handleWatermarkModeChange = (rawMode: string) => {
    const nextMode = parseInt(rawMode, 10);
    const curOpacity = capture.watermarkOpacity ?? 0.16;
    let nextOpacity = curOpacity;
    if (nextMode === 2 && curOpacity > 0.40) {
      nextOpacity = 0.16;
    } else if (nextMode !== 2 && curOpacity < 0.30) {
      nextOpacity = 0.85;
    }
    setCapture(prev => ({ ...prev, watermarkMode: nextMode, watermarkOpacity: nextOpacity }));
    void bridgeRequest<OperationResult>('capture.updateSettings', {
      watermarkMode: nextMode,
      watermarkOpacity: nextOpacity,
    }).catch(() => { /* synced via focus fallback */ });
  };

  const appendWatermarkToken = (current: string, token: string): string => {
    const cur = current.trimEnd();
    if (token === '\\n') {
      return cur.length > 0 ? `${cur}\\n` : '\\n';
    }
    if (cur.length === 0) return token;
    if (cur.endsWith('\\n') || cur.endsWith('\n')) {
      return `${cur}${token}`;
    }
    if (cur.endsWith('{date}') && token === '{time}') {
      return `${cur} ${token}`;
    }
    return `${cur} · ${token}`;
  };

  const handleInsertWatermarkToken = (token: string) => {
    updateCapture('watermarkText', appendWatermarkToken(capture.watermarkText ?? '', token));
  };

  const handleApplyDualLineWatermarkPreset = () => {
    updateCapture('watermarkText', '{user}\\n{date} {time}');
  };

  const handlePickStudioWatermarkImage = async () => {
    try {
      const res = await bridgeRequest<{ success?: boolean; path?: string; cancelled?: boolean; error?: string }>(
        'capture.pickWatermarkImage',
      );
      if (res?.cancelled) return;
      if (!res?.success || !res.path) {
        throw new Error(res?.error || t('capture.browseFailed'));
      }
      updateRecording('studioWatermarkImagePath', res.path);
    } catch (error) {
      toast.error(t('capture.browseFailed'), { description: String(error) });
    }
  };

  const handleStudioWatermarkModeChange = (rawMode: string) => {
    const nextMode = parseInt(rawMode, 10);
    const curOpacity = recording.studioWatermarkOpacity ?? 0.88;
    let nextOpacity = curOpacity;
    if (nextMode === 2 && curOpacity > 0.40) {
      nextOpacity = 0.16;
    } else if (nextMode !== 2 && curOpacity < 0.30) {
      nextOpacity = 0.88;
    }
    setRecording(prev => ({ ...prev, studioWatermarkMode: nextMode, studioWatermarkOpacity: nextOpacity }));
    void bridgeRequest<OperationResult>('recording.updateSettings', {
      studioWatermarkMode: nextMode,
      studioWatermarkOpacity: nextOpacity,
    }).catch(() => { /* synced via focus fallback */ });
  };

  const handleInsertStudioWatermarkToken = (token: string) => {
    updateRecording('studioWatermarkText', appendWatermarkToken(recording.studioWatermarkText ?? '', token));
  };

  const handleApplyDualLineStudioWatermarkPreset = () => {
    updateRecording('studioWatermarkText', '{user}\\n{date} {time}');
  };

  const handleSyncStudioWatermarkFromCapture = () => {
    const patch = {
      studioWatermarkEnabled: true,
      studioWatermarkMode: capture.watermarkMode ?? 2,
      studioWatermarkImagePath: capture.watermarkImagePath ?? '',
      studioWatermarkText: capture.watermarkText ?? '{user} · {host} · {date} {time}',
      studioWatermarkAnchor: Math.min(4, Math.max(0, capture.watermarkAnchor ?? 3)),
      studioWatermarkScale: capture.watermarkScale ?? 1.0,
      studioWatermarkOpacity: capture.watermarkOpacity ?? 0.16,
    };
    setRecording(prev => ({ ...prev, ...patch }));
    bridgeRequest<OperationResult>('recording.updateSettings', patch)
      .then(res => {
        if (!res.success) throw new Error(res.error || 'sync failed');
        toast.success(t('recording.studioWatermarkSyncedToast'));
      })
      .catch(err => toast.error(t('capture.saveFailed'), { description: String(err) }));
  };

  const watermarkScaleOptions = (current: number) => {
    const rounded = Math.round(current * 100) / 100;
    const values: number[] = [...WATERMARK_SCALE_PRESETS];
    if (!values.some(v => Math.abs(v - rounded) < 0.02)) {
      values.push(rounded);
      values.sort((a, b) => a - b);
    }
    return values.map(v => ({
      value: String(v),
      label: `${Math.round(v * 100)}%`,
    }));
  };

  const watermarkOpacityOptions = (current: number) => {
    const rounded = Math.round(current * 100) / 100;
    const values: number[] = [...WATERMARK_OPACITY_PRESETS];
    if (!values.some(v => Math.abs(v - rounded) < 0.02)) {
      values.push(rounded);
      values.sort((a, b) => a - b);
    }
    return values.map(v => ({
      value: String(v),
      label: `${Math.round(v * 100)}%`,
    }));
  };

  const audioDeviceOptions = (systemAudio: boolean, selectedId: string) => {
    const devices = audioDevices.filter(device => device.systemAudio === systemAudio);
    const options = [
      { value: '', label: t('recording.defaultAudioDevice') },
      ...devices.map(device => ({
        value: device.id,
        label: device.defaultDevice
          ? `${device.name} · ${t('recording.currentDefault')}` : device.name,
      })),
    ];
    if (selectedId && !devices.some(device => device.id === selectedId)) {
      options.push({ value: selectedId, label: t('recording.unavailableAudioDevice') });
    }
    return options;
  };

  const audioVolumeOptions = (current: number) => {
    const values = [0, 25, 50, 75, 100, 125, 150, 200];
    if (!values.includes(current)) values.push(current);
    return values.sort((left, right) => left - right)
      .map(value => ({ value: String(value), label: `${value}%` }));
  };

  const bitrateOptions = (current: number) => {
    const presetLabels: Record<number, string> = {
      4: t('recording.bitrate4'),
      8: t('recording.bitrate8'),
      16: t('recording.bitrate16'),
      32: t('recording.bitrate32'),
    };
    const values: number[] = [...RECORDING_BITRATE_PRESETS];
    if (current > 0 && !values.includes(current)) {
      values.push(current);
      values.sort((a, b) => a - b);
    }
    return values.map((val) => ({
      value: String(val),
      label: presetLabels[val] ?? `${val} Mbps`,
    }));
  };

  if (loading) {
    return <PageSkeleton cards={3} rowsPerCard={4} />;
  }

  const tabs: TabItem<CapturePageSubTab>[] = [
    { id: 'screenshot', label: t('capture.tabScreenshot'), icon: <Camera size={15} /> },
    { id: 'pin_region', label: t('capture.tabPinRegion'), icon: <Crop size={15} /> },
    { id: 'recording', label: t('capture.tabRecording'), icon: <Video size={15} /> },
    { id: 'studio', label: t('capture.tabStudioWatermark'), icon: <Sparkles size={15} /> },
  ];

  const watermarkTokenEntries = [
    ['{user}', t('capture.watermarkTokenUser')],
    ['{host}', t('capture.watermarkTokenHost')],
    ['{ip}', t('capture.watermarkTokenIp')],
    ['{date}', t('capture.watermarkTokenDate')],
    ['{time}', t('capture.watermarkTokenTime')],
    ['\\n', t('capture.watermarkTokenNewline')],
  ] as const;

  return (
    <div className="capture-page">
      <Tabs
        tabs={tabs}
        activeId={activeTab}
        onChange={setActiveTab}
        className="capture-page__tabs"
        ariaLabel={t('capture.title')}
      />

      {/* ── 1. 截图与标注 (默认首屏) ─────────────────────────────────── */}
      <div
        className="capture-page__panel"
        role="tabpanel"
        hidden={activeTab !== 'screenshot'}
      >
        <SettingGroup title={t('capture.groupTrigger')} icon={<Zap size={18} />}>
          <Card>
            <SettingRow
              label={
                <div className="capture-page__inline-label">
                  <span>{t('capture.shortcut')}</span>
                  <HotkeyStatusBadge entry={getHotkey('Screenshot')} />
                </div>
              }
              description={t('capture.shortcutDesc')}
            >
              <div className="capture-page__inline-actions">
                <HotkeyRecorder
                  id="capture-shortcut"
                  value={screenshotHotkey}
                  onChange={(v) => void rebindHotkey('Screenshot', v)}
                />
                <Button variant="ghost" onClick={() => void handleTryCapture()}>
                  {t('capture.tryCapture')}
                </Button>
              </div>
            </SettingRow>
            <SettingRow
              label={
                <div className="capture-page__inline-label">
                  <span>{t('capture.repeatLastCaptureShortcut')}</span>
                  <HotkeyStatusBadge entry={getHotkey('Repeat Last Capture')} />
                </div>
              }
              description={t('capture.repeatLastCaptureShortcutDesc')}
            >
              <HotkeyRecorder
                id="capture-repeat-shortcut"
                value={repeatCaptureHotkey}
                onChange={(v) => void rebindHotkey('Repeat Last Capture', v)}
              />
            </SettingRow>
            <Toggle
              id="capture-auto-bypass"
              label={t('capture.autoBypassFullscreen')}
              description={t('capture.autoBypassFullscreenDesc')}
              checked={capture.autoBypassFullscreen ?? true}
              onChange={(v) => updateCapture('autoBypassFullscreen', v)}
            />
          </Card>
        </SettingGroup>

        <SettingGroup title={t('capture.groupOutput')} icon={<FolderOpen size={18} />}>
          <Card>
            <Toggle
              id="capture-copy-clipboard"
              label={t('capture.copyToClipboard')}
              description={t('capture.copyToClipboardDesc')}
              checked={capture.copyToClipboard}
              onChange={(v) => updateCapture('copyToClipboard', v)}
            />
            <Toggle
              id="capture-auto-save"
              label={t('capture.saveToFile')}
              description={t('capture.saveToFileDesc')}
              checked={capture.saveToFile}
              onChange={(v) => updateCapture('saveToFile', v)}
            />
            <SubSettingGroup disabled={!capture.saveToFile}>
              <SettingRow label={t('capture.saveDir')} description={t('capture.saveDirDesc')}>
                <div className="capture-page__inline-actions">
                  <div className="capture-page__dir-input">
                    <TextInput
                      id="saveDir"
                      value={capture.saveDirectory || ''}
                      placeholder={t('capture.saveDirPlaceholder')}
                      readOnly
                      onChange={() => {}}
                    />
                  </div>
                  <Button variant="ghost" onClick={handleBrowseDir}>{t('capture.browse')}</Button>
                  <Button
                    id="open-capture-save-dir"
                    variant="ghost"
                    title={t('capture.openFolder')}
                    onClick={() => void handleOpenSaveFolder(capture.saveDirectory, 'capture')}
                  >
                    <FolderOpen size={14} aria-hidden="true" />
                    <span>{t('capture.openFolder')}</span>
                  </Button>
                </div>
              </SettingRow>
              <SettingRow label={t('capture.imageFormat')} description={t('capture.imageFormatDesc')}>
                <Select
                  id="capture-format"
                  value={capture.format}
                  disabled={!capture.saveToFile}
                  onChange={(v) => updateCapture('format', v)}
                  options={[
                    { value: 'png', label: t('capture.formatPng') },
                    { value: 'jpg', label: t('capture.formatJpg') },
                    { value: 'webp', label: t('capture.formatWebp') },
                    { value: 'bmp', label: t('capture.formatBmp') },
                  ]}
                />
              </SettingRow>
              {(capture.format === 'jpg' || capture.format === 'jpeg' || capture.format === 'webp') && (
                <SettingRow label={t('capture.quality')} description={t('capture.qualityDesc')}>
                  <Select
                    id="capture-quality"
                    value={String(capture.quality)}
                    disabled={!capture.saveToFile}
                    onChange={(v) => updateCapture('quality', parseInt(v, 10))}
                    options={[75, 85, 90, 95, 100].map((value) => ({
                      value: String(value), label: `${value}%`,
                    }))}
                  />
                </SettingRow>
              )}
            </SubSettingGroup>
          </Card>
        </SettingGroup>

        <SettingGroup title={t('capture.groupAssist')} icon={<Crosshair size={18} />}>
          <Card>
            <Toggle
              id="capture-detect-window"
              label={t('capture.autoDetectWindow')}
              description={t('capture.autoDetectWindowDesc')}
              checked={capture.autoDetectWindow}
              onChange={(v) => updateCapture('autoDetectWindow', v)}
            />
            <Toggle
              id="capture-crosshair"
              label={t('capture.showCrosshair')}
              description={t('capture.showCrosshairDesc')}
              checked={capture.showCrosshair}
              onChange={(v) => updateCapture('showCrosshair', v)}
            />
            <Toggle
              id="capture-include-cursor"
              label={t('capture.includeCursor')}
              description={t('capture.includeCursorDesc')}
              checked={capture.includeCursor ?? false}
              onChange={(v) => updateCapture('includeCursor', v)}
            />
            <Toggle
              id="capture-shortcut-hints"
              label={t('capture.showShortcutHints')}
              description={t('capture.showShortcutHintsDesc')}
              checked={capture.showShortcutHints}
              onChange={(v) => updateCapture('showShortcutHints', v)}
            />
          </Card>
        </SettingGroup>

        <SettingGroup title={t('capture.beautyShellTitle')} icon={<Palette size={18} />}>
          <Card>
            <Toggle
              id="capture-beauty-enabled"
              label={t('capture.beautyShellDefault')}
              description={t('capture.beautyShellDefaultDesc')}
              checked={capture.beautyShellEnabled ?? false}
              onChange={(v) => updateCapture('beautyShellEnabled', v)}
            />
            {capture.beautyShellEnabled && (
              <SubSettingGroup>
                <SettingRow label={t('capture.beautyShellTheme')} description={t('capture.beautyShellThemeDesc')}>
                  <Select
                    id="capture-beauty-theme"
                    value={normalizeBeautyShellTheme(capture.beautyShellTheme)}
                    onChange={(v) => updateCapture('beautyShellTheme', v)}
                    options={[
                      { value: 'obsidian_dark', label: t('capture.themeObsidianDark') },
                      { value: 'aurora_purple', label: t('capture.themeAuroraPurple') },
                      { value: 'emerald_sea', label: t('capture.themeEmeraldSea') },
                      { value: 'minimal_light', label: t('capture.themeMinimalLight') },
                      { value: 'sunrise_coral', label: t('capture.themeSunriseCoral') },
                      { value: 'airy_glacier', label: t('capture.themeAiryGlacier') },
                      { value: 'dusk_mountains', label: t('capture.themeDuskMountains') },
                      { value: 'misty_lake_dawn', label: t('capture.themeMistyLakeDawn') },
                    ]}
                  />
                </SettingRow>
                <SettingRow label={t('capture.beautyShellPadding')} description={t('capture.beautyShellPaddingDesc')}>
                  <Select
                    id="capture-beauty-padding"
                    value={String(capture.beautyShellPadding ?? 32)}
                    onChange={(v) => updateCapture('beautyShellPadding', parseInt(v, 10))}
                    options={BEAUTY_SHELL_PADDING_PRESETS.map((px) => ({
                      value: String(px),
                      label: `${px} px`,
                    }))}
                  />
                </SettingRow>
                <SettingRow label={t('capture.beautyShellRadius')} description={t('capture.beautyShellRadiusDesc')}>
                  <Select
                    id="capture-beauty-radius"
                    value={String(capture.beautyShellRadius ?? 16)}
                    onChange={(v) => updateCapture('beautyShellRadius', parseInt(v, 10))}
                    options={BEAUTY_SHELL_RADIUS_PRESETS.map((px) => ({
                      value: String(px),
                      label: `${px} px`,
                    }))}
                  />
                </SettingRow>
              </SubSettingGroup>
            )}
          </Card>
        </SettingGroup>
      </div>

      {/* ── 2. 贴图与固定选区 ────────────────────────────────────────── */}
      <div
        className="capture-page__panel"
        role="tabpanel"
        hidden={activeTab !== 'pin_region'}
      >
        <CaptureRegionManagerCard
          repeatCaptureHotkey={repeatCaptureHotkey}
          repeatCaptureHotkeyEntry={getHotkey('Repeat Last Capture')}
          onRebindRepeatCaptureHotkey={(v) => void rebindHotkey('Repeat Last Capture', v)}
          regionHistoryLimit={capture.regionHistoryLimit ?? 10}
          regionHistoryKey={capture.regionHistoryKey ?? 'R'}
          onUpdateRegionHistoryLimit={(limit) => updateCapture('regionHistoryLimit', limit)}
          onUpdateRegionHistoryKey={(keyName) => updateCapture('regionHistoryKey', keyName)}
        />

        <PinGroupSettingsCard />
      </div>

      {/* ── 3. 屏幕录制 ──────────────────────────────────────────────── */}
      <div
        className="capture-page__panel"
        role="tabpanel"
        hidden={activeTab !== 'recording'}
      >
        <SettingGroup title={t('recording.groupShortcutsAndFormat')} icon={<Video size={18} />}>
          <Card>
            <SettingRow
              label={
                <div className="capture-page__inline-label">
                  <span>{t('recording.shortcut')}</span>
                  <HotkeyStatusBadge entry={getHotkey('Record')} />
                </div>
              }
              description={t('recording.shortcutDesc')}
            >
              <HotkeyRecorder
                id="recording-shortcut"
                value={recordHotkey}
                onChange={(v) => void rebindHotkey('Record', v)}
              />
            </SettingRow>
            <SettingRow
              label={
                <div className="capture-page__inline-label">
                  <span>{t('recording.pauseShortcut')}</span>
                  <HotkeyStatusBadge entry={getHotkey('Record Pause')} />
                </div>
              }
              description={t('recording.pauseShortcutDesc')}
            >
              <HotkeyRecorder
                id="recording-pause-shortcut"
                value={recordPauseHotkey}
                onChange={(v) => void rebindHotkey('Record Pause', v)}
              />
            </SettingRow>
            <SettingRow
              label={
                <div className="capture-page__inline-label">
                  <span>{t('recording.snapshotShortcut')}</span>
                  <HotkeyStatusBadge entry={getHotkey('Record Snapshot')} />
                </div>
              }
              description={t('recording.snapshotShortcutDesc')}
            >
              <HotkeyRecorder
                id="recording-snapshot-shortcut"
                value={recordSnapshotHotkey}
                onChange={(v) => void rebindHotkey('Record Snapshot', v)}
              />
            </SettingRow>
            <SettingRow label={t('recording.saveDir')} description={t('recording.saveDirDesc')}>
              <div className="capture-page__inline-actions">
                <div className="capture-page__dir-input">
                  <TextInput
                    id="recordSaveDir"
                    value={recording.saveDirectory || ''}
                    placeholder={t('recording.saveDirPlaceholder')}
                    readOnly
                    onChange={() => {}}
                  />
                </div>
                <Button variant="ghost" onClick={handleBrowseRecordingDir}>{t('capture.browse')}</Button>
                <Button
                  id="open-recording-save-dir"
                  variant="ghost"
                  title={t('capture.openFolder')}
                  onClick={() => void handleOpenSaveFolder(recording.saveDirectory, 'recording')}
                >
                  <FolderOpen size={14} aria-hidden="true" />
                  <span>{t('capture.openFolder')}</span>
                </Button>
              </div>
            </SettingRow>
            <SettingRow label={t('recording.format')} description={t('recording.formatDesc')}>
              <Select
                id="record-format"
                value={recording.format}
                onChange={(v) => updateRecording('format', v)}
                options={[
                  { value: 'mp4_h264', label: t('recording.formatMp4H264') },
                  { value: 'mp4_h265', label: t('recording.formatMp4H265') },
                  { value: 'gif', label: t('recording.formatGif') },
                  { value: 'webm_vp9', label: t('recording.formatWebmVp9') },
                ]}
              />
            </SettingRow>
            <SettingRow label={t('recording.fps')} description={t('recording.fpsDesc')}>
              <Select
                id="record-fps"
                value={String(recording.fps)}
                onChange={(v) => updateRecording('fps', parseInt(v, 10))}
                options={[
                  { value: '15', label: t('recording.fps15') },
                  { value: '30', label: t('recording.fps30') },
                  { value: '60', label: t('recording.fps60') },
                ]}
              />
            </SettingRow>
            <SettingRow label={t('recording.bitrate')} description={t('recording.bitrateDesc')}>
              <Select
                id="record-bitrate"
                value={String(recording.bitrate)}
                onChange={(v) => updateRecording('bitrate', parseInt(v, 10))}
                options={bitrateOptions(recording.bitrate)}
              />
            </SettingRow>
            <SettingRow label={t('recording.countdown')} description={t('recording.countdownDesc')}>
              <Select
                id="record-countdown"
                value={String(recording.countdownSeconds)}
                onChange={(v) => updateRecording('countdownSeconds', parseInt(v, 10))}
                options={[
                  { value: '0', label: t('recording.countdownOff') },
                  { value: '3', label: t('recording.countdownSeconds', { count: 3 }) },
                  { value: '5', label: t('recording.countdownSeconds', { count: 5 }) },
                  { value: '10', label: t('recording.countdownSeconds', { count: 10 }) },
                ]}
              />
            </SettingRow>
          </Card>
        </SettingGroup>

        <SettingGroup title={t('recording.groupAudio')} icon={<Mic size={18} />}>
          <Card>
            {recording.format === 'gif' ? (
              <div className="capture-page__gif-audio-notice">
                <VolumeX size={16} className="capture-page__gif-audio-notice-icon" aria-hidden="true" />
                <span>{t('recording.gifNoAudioHint')}</span>
              </div>
            ) : (
              <>
                <Toggle
                  id="record-system-audio"
                  label={t('recording.captureSystemAudio')}
                  description={t('recording.captureSystemAudioDesc')}
                  checked={recording.captureSystemAudio}
                  onChange={(v) => updateRecording('captureSystemAudio', v)}
                />
                {recording.captureSystemAudio && (
                  <SubSettingGroup>
                    <SettingRow
                      label={t('recording.systemAudioDevice')}
                      description={t('recording.systemAudioDeviceDesc')}
                    >
                      <Select
                        id="record-system-audio-device"
                        value={recording.systemAudioDeviceId}
                        onChange={(v) => updateRecording('systemAudioDeviceId', v)}
                        options={audioDeviceOptions(true, recording.systemAudioDeviceId)}
                      />
                    </SettingRow>
                    <SettingRow
                      label={t('recording.systemAudioVolume')}
                      description={t('recording.audioVolumeDesc')}
                    >
                      <Select
                        id="record-system-audio-volume"
                        value={String(recording.systemAudioVolume)}
                        onChange={(v) => updateRecording('systemAudioVolume', parseInt(v, 10))}
                        options={audioVolumeOptions(recording.systemAudioVolume)}
                      />
                    </SettingRow>
                  </SubSettingGroup>
                )}

                <Toggle
                  id="record-microphone"
                  label={t('recording.captureMicrophone')}
                  description={t('recording.captureMicrophoneDesc')}
                  checked={recording.captureMicrophone}
                  onChange={(v) => updateRecording('captureMicrophone', v)}
                />
                {recording.captureMicrophone && (
                  <SubSettingGroup>
                    <SettingRow
                      label={t('recording.microphoneDevice')}
                      description={t('recording.microphoneDeviceDesc')}
                    >
                      <Select
                        id="record-microphone-device"
                        value={recording.microphoneDeviceId}
                        onChange={(v) => updateRecording('microphoneDeviceId', v)}
                        options={audioDeviceOptions(false, recording.microphoneDeviceId)}
                      />
                    </SettingRow>
                    <SettingRow
                      label={t('recording.microphoneVolume')}
                      description={t('recording.audioVolumeDesc')}
                    >
                      <Select
                        id="record-microphone-volume"
                        value={String(recording.microphoneVolume)}
                        onChange={(v) => updateRecording('microphoneVolume', parseInt(v, 10))}
                        options={audioVolumeOptions(recording.microphoneVolume)}
                      />
                    </SettingRow>
                    <Toggle
                      id="record-voice-enhance"
                      label={t('recording.voiceEnhance')}
                      description={t('recording.voiceEnhanceDesc')}
                      checked={recording.voiceEnhance ?? true}
                      onChange={(v) => updateRecording('voiceEnhance', v)}
                    />
                  </SubSettingGroup>
                )}
              </>
            )}
          </Card>
        </SettingGroup>

        <SettingGroup title={t('recording.groupCursorAndOsd')} icon={<MousePointerClick size={18} />}>
          <Card>
            <Toggle
              id="record-include-cursor"
              label={t('recording.includeCursor')}
              description={t('recording.includeCursorDesc')}
              checked={recording.includeCursor}
              onChange={(v) => updateRecording('includeCursor', v)}
            />
            <SubSettingGroup disabled={!recording.includeCursor}>
              <Toggle
                id="record-click-effects"
                label={t('recording.showClickEffects')}
                description={t('recording.showClickEffectsDesc')}
                checked={recording.showClickEffects}
                disabled={!recording.includeCursor}
                onChange={(v) => updateRecording('showClickEffects', v)}
              />
              {(!(recording.studioMode ?? true) || recording.format === 'gif') && (
                <Toggle
                  id="record-click-zoom"
                  label={t('recording.showClickZoom')}
                  description={t('recording.showClickZoomDesc')}
                  checked={recording.showClickZoom ?? true}
                  disabled={!recording.includeCursor}
                  onChange={(v) => updateRecording('showClickZoom', v)}
                />
              )}
            </SubSettingGroup>
            <Toggle
              id="record-include-keycast"
              label={t('recording.includeKeycast')}
              description={t('recording.includeKeycastDesc')}
              checked={recording.includeKeycast ?? true}
              onChange={(v) => updateRecording('includeKeycast', v)}
            />
          </Card>
        </SettingGroup>

        <SettingGroup title={t('recording.groupCleanDesktop')} icon={<Monitor size={18} />}>
          <Card>
            <Toggle
              id="record-hide-indicator"
              label={t('recording.hideIndicatorInCapture')}
              description={t('recording.hideIndicatorInCaptureDesc')}
              checked={recording.hideIndicatorInCapture ?? true}
              onChange={(v) => updateRecording('hideIndicatorInCapture', v)}
            />
            <Toggle
              id="record-hide-desktop-icons"
              label={t('recording.hideDesktopIcons')}
              description={t('recording.hideDesktopIconsDesc')}
              checked={recording.hideDesktopIcons ?? false}
              onChange={(v) => updateRecording('hideDesktopIcons', v)}
            />
            <Toggle
              id="record-hide-taskbar"
              label={t('recording.hideTaskbar')}
              description={t('recording.hideTaskbarDesc')}
              checked={recording.hideTaskbar ?? false}
              onChange={(v) => updateRecording('hideTaskbar', v)}
            />
          </Card>
        </SettingGroup>
      </div>

      {/* ── 4. 演播室与水印 ──────────────────────────────────────────── */}
      <div
        className="capture-page__panel"
        role="tabpanel"
        hidden={activeTab !== 'studio'}
      >
        <SettingGroup title={t('recording.studioGroupTitle')} icon={<Sparkles size={18} />}>
          <Card>
            <Toggle
              id="record-studio-mode"
              label={t('recording.studioMode')}
              description={
                recording.format === 'gif'
                  ? t('recording.studioGifHintDesc')
                  : t('recording.studioModeDesc')
              }
              checked={recording.studioMode ?? true}
              disabled={recording.format === 'gif'}
              onChange={(v) => updateRecording('studioMode', v)}
            />
            {(recording.studioMode ?? true) && recording.format !== 'gif' && (
              <SubSettingGroup>
                <Toggle
                  id="record-studio-auto-zoom"
                  label={t('recording.studioAutoZoom')}
                  description={t('recording.studioAutoZoomDesc')}
                  checked={recording.studioAutoZoom ?? true}
                  onChange={(v) => updateRecording('studioAutoZoom', v)}
                />
                {(recording.studioAutoZoom ?? true) && (
                  <SubSettingGroup>
                    <SettingRow label={t('recording.studioZoomMode')} description={t('recording.studioZoomModeDesc')}>
                      <Select
                        id="record-studio-zoom-mode"
                        value={String(recording.studioZoomMode === 2 ? 2 : 1)}
                        onChange={(v) => updateRecording('studioZoomMode', parseInt(v, 10))}
                        options={[
                          { value: '1', label: t('recording.studioZoomMode2D') },
                          { value: '2', label: t('recording.studioZoomMode3D') },
                        ]}
                      />
                    </SettingRow>
                    <SettingRow label={t('recording.studioZoomPreset')} description={t('recording.studioZoomPresetDesc')}>
                      {(() => {
                        const presets = [
                          {
                            id: 'gentle',
                            label: t('recording.studioZoomPresetGentle'),
                            patch: {
                              studioZoomFactor: 1.32,
                              studioZoomPacing: 0.32,
                              studioZoomSensitivity: 0.32,
                              studioFocusSpotlightDim: 0.18,
                              studioLockCameraHold: true,
                            },
                          },
                          {
                            id: 'balanced',
                            label: t('recording.studioZoomPresetBalanced'),
                            patch: {
                              studioZoomFactor: 1.60,
                              studioZoomPacing: 0.55,
                              studioZoomSensitivity: 0.60,
                              studioFocusSpotlightDim: 0.10,
                              studioLockCameraHold: true,
                            },
                          },
                          {
                            id: 'dynamic',
                            label: t('recording.studioZoomPresetDynamic'),
                            patch: {
                              studioZoomFactor: 1.95,
                              studioZoomPacing: 0.82,
                              studioZoomSensitivity: 0.88,
                              studioFocusSpotlightDim: 0.0,
                              studioLockCameraHold: false,
                            },
                          },
                        ];
                        const curZoom = recording.studioZoomFactor ?? 1.32;
                        const curPacing = recording.studioZoomPacing ?? 0.32;
                        const curSens = recording.studioZoomSensitivity ?? 0.32;
                        const curDim = recording.studioFocusSpotlightDim ?? 0.18;
                        const curLock = recording.studioLockCameraHold ?? true;
                        return (
                          <div className="uikit-segmented" role="group" aria-label={t('recording.studioZoomPreset')}>
                            {presets.map((p) => {
                              const active =
                                Math.abs(curZoom - p.patch.studioZoomFactor) <= 0.045 &&
                                Math.abs(curPacing - p.patch.studioZoomPacing) <= 0.045 &&
                                Math.abs(curSens - p.patch.studioZoomSensitivity) <= 0.045 &&
                                Math.abs(curDim - p.patch.studioFocusSpotlightDim) <= 0.035 &&
                                curLock === p.patch.studioLockCameraHold;
                              return (
                                <button
                                  key={p.id}
                                  type="button"
                                  className={`uikit-segmented__btn${active ? ' uikit-segmented__btn--active' : ''}`}
                                  aria-pressed={active}
                                  onClick={() => updateRecordingPatch(p.patch)}
                                >
                                  {p.label}
                                </button>
                              );
                            })}
                          </div>
                        );
                      })()}
                    </SettingRow>
                    <Toggle
                      id="record-typing-auto-zoom"
                      label={t('recording.typingAutoZoom')}
                      description={t('recording.typingAutoZoomDesc')}
                      checked={recording.typingAutoZoom ?? false}
                      onChange={(v) => updateRecording('typingAutoZoom', v)}
                    />
                  </SubSettingGroup>
                )}
                <Toggle
                  id="record-studio-smooth-cursor"
                  label={t('recording.studioSmoothCursor')}
                  description={t('recording.studioSmoothCursorDesc')}
                  checked={recording.studioSmoothCursor ?? true}
                  onChange={(v) => updateRecording('studioSmoothCursor', v)}
                />
                <Toggle
                  id="record-studio-auto-frame"
                  label={t('recording.studioAutoFrame')}
                  description={t('recording.studioAutoFrameDesc')}
                  checked={recording.studioAutoFrame ?? true}
                  onChange={(v) => updateRecording('studioAutoFrame', v)}
                />
                <Toggle
                  id="record-studio-captions"
                  label={t('recording.studioAutoCaptions')}
                  description={
                    recording.captureMicrophone
                      ? t('recording.studioAutoCaptionsDesc')
                      : t('capture.subtitleNeedsMicHint')
                  }
                  checked={Boolean(recording.captureMicrophone && (recording.studioAutoCaptions ?? false))}
                  disabled={!recording.captureMicrophone}
                  onChange={(v) => updateRecording('studioAutoCaptions', v)}
                />
              </SubSettingGroup>
            )}
          </Card>
        </SettingGroup>

        <SettingGroup title={t('capture.watermarkSectionTitle')} icon={<ShieldCheck size={18} />}>
          <Card>
            <Toggle
              id="capture-watermark-enabled"
              label={t('capture.watermarkEnable')}
              description={
                capture.watermarkPolicyForced
                  ? t('capture.watermarkPolicyLockedDesc')
                  : t('capture.watermarkEnableDesc')
              }
              checked={Boolean(capture.watermarkEnabled || capture.watermarkPolicyForced)}
              disabled={Boolean(capture.watermarkPolicyForced)}
              onChange={(v) => updateCapture('watermarkEnabled', v)}
            />
            {(capture.watermarkEnabled || capture.watermarkPolicyForced) && (
              <SubSettingGroup>
                <SettingRow label={t('capture.watermarkMode')} description={t('capture.watermarkModeDesc')}>
                  <Select
                    id="capture-watermark-mode"
                    value={String(capture.watermarkMode ?? 2)}
                    disabled={Boolean(capture.watermarkPolicyForced)}
                    onChange={handleWatermarkModeChange}
                    options={[
                      { value: '2', label: t('capture.watermarkModeTiled') },
                      { value: '0', label: t('capture.watermarkModeImage') },
                      { value: '1', label: t('capture.watermarkModeCornerText') },
                    ]}
                  />
                </SettingRow>
                {(capture.watermarkMode ?? 2) === 0 ? (
                  <SettingRow label={t('capture.watermarkImage')} description={t('capture.watermarkImageDesc')}>
                    <div className="capture-page__inline-actions">
                      <TextInput
                        id="capture-watermark-image-path"
                        value={capture.watermarkImagePath || ''}
                        placeholder={t('capture.watermarkImagePlaceholder')}
                        readOnly
                        onChange={() => {}}
                      />
                      <Button
                        variant="ghost"
                        disabled={Boolean(capture.watermarkPolicyForced)}
                        onClick={() => void handlePickWatermarkImage()}
                      >
                        {t('capture.watermarkPickImage')}
                      </Button>
                    </div>
                  </SettingRow>
                ) : (
                  <SettingRow
                    label={t('capture.watermarkText')}
                    description={t('capture.watermarkTextDesc')}
                    layout="vertical"
                  >
                    <div className="capture-page__watermark-stack">
                      <TextInput
                        id="capture-watermark-text"
                        value={capture.watermarkText ?? ''}
                        disabled={Boolean(capture.watermarkPolicyForced)}
                        onChange={(v) => updateCapture('watermarkText', v)}
                      />
                      <div className="capture-page__token-bar">
                        {watermarkTokenEntries.map(([tok, label]) => (
                          <button
                            key={tok}
                            type="button"
                            className="uikit-token-chip"
                            disabled={Boolean(capture.watermarkPolicyForced)}
                            onClick={() => handleInsertWatermarkToken(tok)}
                          >
                            <span>{label}</span>
                            <span className="uikit-token-chip__code">{tok}</span>
                          </button>
                        ))}
                        <Button
                          variant="secondary"
                          disabled={Boolean(capture.watermarkPolicyForced)}
                          onClick={handleApplyDualLineWatermarkPreset}
                        >
                          <span>{t('capture.watermarkPresetDualLine')}</span>
                        </Button>
                      </div>
                    </div>
                  </SettingRow>
                )}
                {(capture.watermarkMode ?? 2) !== 2 && (
                  <SettingRow label={t('capture.watermarkAnchor')} description={t('capture.watermarkAnchorDesc')}>
                    <Select
                      id="capture-watermark-anchor"
                      value={String(Math.min(4, Math.max(0, capture.watermarkAnchor ?? 3)))}
                      disabled={Boolean(capture.watermarkPolicyForced)}
                      onChange={(v) => updateCapture('watermarkAnchor', parseInt(v, 10))}
                      options={[
                        { value: '0', label: t('capture.watermarkAnchorTopLeft') },
                        { value: '1', label: t('capture.watermarkAnchorTopRight') },
                        { value: '2', label: t('capture.watermarkAnchorBottomLeft') },
                        { value: '3', label: t('capture.watermarkAnchorBottomRight') },
                        { value: '4', label: t('capture.watermarkAnchorCenter') },
                      ]}
                    />
                  </SettingRow>
                )}
                <SettingRow label={t('capture.watermarkScale')} description={t('capture.watermarkScaleDesc')}>
                  <Select
                    id="capture-watermark-scale"
                    value={String(Math.round((capture.watermarkScale ?? 1.0) * 100) / 100)}
                    disabled={Boolean(capture.watermarkPolicyForced)}
                    onChange={(v) => updateCapture('watermarkScale', parseFloat(v))}
                    options={watermarkScaleOptions(capture.watermarkScale ?? 1.0)}
                  />
                </SettingRow>
                <SettingRow label={t('capture.watermarkOpacity')} description={t('capture.watermarkOpacityDesc')}>
                  <Select
                    id="capture-watermark-opacity"
                    value={String(Math.round((capture.watermarkOpacity ?? 0.16) * 100) / 100)}
                    disabled={Boolean(capture.watermarkPolicyForced)}
                    onChange={(v) => updateCapture('watermarkOpacity', parseFloat(v))}
                    options={watermarkOpacityOptions(capture.watermarkOpacity ?? 0.16)}
                  />
                </SettingRow>
              </SubSettingGroup>
            )}
          </Card>

          <Card>
            <Toggle
              id="record-studio-watermark-enabled"
              label={t('recording.studioWatermarkEnable')}
              description={
                recording.studioWatermarkPolicyForced
                  ? t('capture.watermarkPolicyLockedDesc')
                  : t('recording.studioWatermarkEnableDesc')
              }
              checked={Boolean(recording.studioWatermarkEnabled || recording.studioWatermarkPolicyForced)}
              disabled={Boolean(recording.studioWatermarkPolicyForced)}
              onChange={(v) => updateRecording('studioWatermarkEnabled', v)}
            />
            {(recording.studioWatermarkEnabled || recording.studioWatermarkPolicyForced) && (
              <SubSettingGroup>
                <SettingRow label={t('capture.watermarkMode')} description={t('capture.watermarkModeDesc')}>
                  <div className="capture-page__inline-actions">
                    <Select
                      id="record-studio-watermark-mode"
                      value={String(recording.studioWatermarkMode ?? 0)}
                      disabled={Boolean(recording.studioWatermarkPolicyForced)}
                      onChange={handleStudioWatermarkModeChange}
                      options={[
                        { value: '0', label: t('capture.watermarkModeImage') },
                        { value: '1', label: t('capture.watermarkModeCornerText') },
                        { value: '2', label: t('capture.watermarkModeTiled') },
                      ]}
                    />
                    {!recording.studioWatermarkPolicyForced && (
                      <Button variant="ghost" onClick={handleSyncStudioWatermarkFromCapture}>
                        {t('recording.studioWatermarkSyncBtn')}
                      </Button>
                    )}
                  </div>
                </SettingRow>
                {(recording.studioWatermarkMode ?? 0) === 0 ? (
                  <SettingRow label={t('capture.watermarkImage')} description={t('capture.watermarkImageDesc')}>
                    <div className="capture-page__inline-actions">
                      <TextInput
                        id="record-studio-watermark-image"
                        value={recording.studioWatermarkImagePath || ''}
                        placeholder={t('capture.watermarkImagePlaceholder')}
                        readOnly
                        onChange={() => {}}
                      />
                      <Button
                        variant="ghost"
                        disabled={Boolean(recording.studioWatermarkPolicyForced)}
                        onClick={() => void handlePickStudioWatermarkImage()}
                      >
                        {t('capture.watermarkPickImage')}
                      </Button>
                    </div>
                  </SettingRow>
                ) : (
                  <SettingRow
                    label={t('capture.watermarkText')}
                    description={t('capture.watermarkTextDesc')}
                    layout="vertical"
                  >
                    <div className="capture-page__watermark-stack">
                      <TextInput
                        id="record-studio-watermark-text"
                        value={recording.studioWatermarkText ?? ''}
                        disabled={Boolean(recording.studioWatermarkPolicyForced)}
                        onChange={(v) => updateRecording('studioWatermarkText', v)}
                      />
                      <div className="capture-page__token-bar">
                        {watermarkTokenEntries.map(([tok, label]) => (
                          <button
                            key={tok}
                            type="button"
                            className="uikit-token-chip"
                            disabled={Boolean(recording.studioWatermarkPolicyForced)}
                            onClick={() => handleInsertStudioWatermarkToken(tok)}
                          >
                            <span>{label}</span>
                            <span className="uikit-token-chip__code">{tok}</span>
                          </button>
                        ))}
                        <Button
                          variant="secondary"
                          disabled={Boolean(recording.studioWatermarkPolicyForced)}
                          onClick={handleApplyDualLineStudioWatermarkPreset}
                        >
                          <span>{t('capture.watermarkPresetDualLine')}</span>
                        </Button>
                      </div>
                    </div>
                  </SettingRow>
                )}
                {(recording.studioWatermarkMode ?? 0) !== 2 && (
                  <SettingRow label={t('capture.watermarkAnchor')} description={t('capture.watermarkAnchorDesc')}>
                    <Select
                      id="record-studio-watermark-anchor"
                      value={String(Math.min(5, Math.max(0, recording.studioWatermarkAnchor ?? 3)))}
                      disabled={Boolean(recording.studioWatermarkPolicyForced)}
                      onChange={(v) => updateRecording('studioWatermarkAnchor', parseInt(v, 10))}
                      options={[
                        { value: '0', label: t('capture.watermarkAnchorTopLeft') },
                        { value: '1', label: t('capture.watermarkAnchorTopRight') },
                        { value: '2', label: t('capture.watermarkAnchorBottomLeft') },
                        { value: '3', label: t('capture.watermarkAnchorBottomRight') },
                        { value: '4', label: t('capture.watermarkAnchorCenter') },
                        { value: '5', label: t('recording.studioWatermarkAnchorCustom') },
                      ]}
                    />
                  </SettingRow>
                )}
                <SettingRow label={t('capture.watermarkScale')} description={t('capture.watermarkScaleDesc')}>
                  <Select
                    id="record-studio-watermark-scale"
                    value={String(Math.round((recording.studioWatermarkScale ?? 1.0) * 100) / 100)}
                    disabled={Boolean(recording.studioWatermarkPolicyForced)}
                    onChange={(v) => updateRecording('studioWatermarkScale', parseFloat(v))}
                    options={watermarkScaleOptions(recording.studioWatermarkScale ?? 1.0)}
                  />
                </SettingRow>
                <SettingRow label={t('capture.watermarkOpacity')} description={t('capture.watermarkOpacityDesc')}>
                  <Select
                    id="record-studio-watermark-opacity"
                    value={String(Math.round((recording.studioWatermarkOpacity ?? 0.88) * 100) / 100)}
                    disabled={Boolean(recording.studioWatermarkPolicyForced)}
                    onChange={(v) => updateRecording('studioWatermarkOpacity', parseFloat(v))}
                    options={watermarkOpacityOptions(recording.studioWatermarkOpacity ?? 0.88)}
                  />
                </SettingRow>
              </SubSettingGroup>
            )}
          </Card>
        </SettingGroup>
      </div>
    </div>
  );
};

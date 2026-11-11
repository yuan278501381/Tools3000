/* ─────────────────────────────────────────────────────────────────────────────
 * CapturePage — 截图录屏设置页
 *
 * 从 C++ ConfigManager 加载截图/录屏设置，修改后实时 IPC 保存。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { Card, Toggle, SettingRow, SettingGroup, TextInput, Select, Button } from '../components/UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { Camera, Video, Palette, FolderOpen } from 'lucide-react';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { toast } from 'sonner';
import { PinGroupSettingsCard } from '../components/PinGroupSettingsCard';

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
  shortcut?: string;
  saveDirectory?: string;
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

export const CapturePage: FC = () => {
  const { t } = useTranslation();
  const [capture, setCapture] = useState<CaptureSettings>({
    format: 'png', quality: 90, saveToFile: true, copyToClipboard: true,
    includeCursor: false,
    showCrosshair: false, autoDetectWindow: true, showShortcutHints: true,
    autoBypassFullscreen: true,
    beautyShellEnabled: false, beautyShellTheme: 'aurora_purple',
    beautyShellPadding: 32, beautyShellRadius: 16,
  });
  const [recording, setRecording] = useState<RecordingSettings>({
    format: 'mp4_h264', fps: 30, bitrate: 8, includeCursor: true, showClickEffects: false,
    showClickZoom: true, includeKeycast: true,
    captureSystemAudio: false, captureMicrophone: false,
    systemAudioDeviceId: '', microphoneDeviceId: '',
    systemAudioVolume: 100, microphoneVolume: 100,
    countdownSeconds: 3,
  });
  const [audioDevices, setAudioDevices] = useState<AudioDeviceInfo[]>([]);
  const [loading, setLoading] = useState(true);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [screenshotHotkey, setScreenshotHotkey] = useState('F1');
  const [recordHotkey, setRecordHotkey] = useState('Ctrl+Shift+R');
  const [recordPauseHotkey, setRecordPauseHotkey] = useState('Ctrl+Shift+P');
  const [recordSnapshotHotkey, setRecordSnapshotHotkey] = useState('Ctrl+Shift+S');

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

  const rebindHotkey = async (name: 'Screenshot' | 'Record' | 'Record Pause' | 'Record Snapshot', value: string) => {
    try {
      const result = await bridgeRequest<OperationResult>('hotkey.rebind', { name, hotkey: value });
      if (!result.success) throw new Error(result.error || t('hotkey.bindFailed'));
      if (name === 'Screenshot') setScreenshotHotkey(result.shortcut ?? value);
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
    return <div style={{ padding: '2rem', opacity: 0.5 }}>{t('common.loading')}</div>;
  }

  return (
    <div className="capture-page" style={{ animation: 'fadeIn 0.3s ease' }}>
      <SettingGroup title={t('capture.title')} icon={<Camera size={20} strokeWidth={2.5} />}>
        <Card>
          <SettingRow
            label={
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                <span>{t('capture.shortcut')}</span>
                <HotkeyStatusBadge entry={getHotkey('Screenshot')} />
              </div>
            }
            description={t('capture.shortcutDesc')}
          >
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
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
          <Toggle
            id="capture-auto-bypass"
            label={t('capture.autoBypassFullscreen')}
            description={t('capture.autoBypassFullscreenDesc')}
            checked={capture.autoBypassFullscreen ?? true}
            onChange={(v) => updateCapture('autoBypassFullscreen', v)}
          />
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
          {capture.saveToFile && (
            <SettingRow label={t('capture.saveDir')} description={t('capture.saveDirDesc')}>
              <div style={{ display: 'flex', gap: '8px', alignItems: 'center', flexWrap: 'wrap' }}>
                <div>
                  <TextInput
                    id="saveDir"
                    value={capture.saveDirectory || ''}
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
          )}
          <SettingRow label={t('capture.imageFormat')} description={t('capture.imageFormatDesc')}>
            <Select
              id="capture-format"
              value={capture.format}
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
                onChange={(v) => updateCapture('quality', parseInt(v))}
                options={[75, 85, 90, 95, 100].map((value) => ({
                  value: String(value), label: `${value}%`,
                }))}
              />
            </SettingRow>
          )}
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
            id="capture-detect-window"
            label={t('capture.autoDetectWindow')}
            description={t('capture.autoDetectWindowDesc')}
            checked={capture.autoDetectWindow}
            onChange={(v) => updateCapture('autoDetectWindow', v)}
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

      <SettingGroup title={t('capture.beautyShellTitle')} icon={<Palette size={20} strokeWidth={2.5} />}>
        <Card>
          <Toggle
            id="capture-beauty-enabled"
            label={t('capture.beautyShellDefault')}
            description={t('capture.beautyShellDefaultDesc')}
            checked={capture.beautyShellEnabled ?? false}
            onChange={(v) => updateCapture('beautyShellEnabled', v)}
          />
          {capture.beautyShellEnabled && (
            <>
              <SettingRow label={t('capture.beautyShellTheme')} description={t('capture.beautyShellThemeDesc')}>
                <Select
                  id="capture-beauty-theme"
                  value={normalizeBeautyShellTheme(capture.beautyShellTheme)}
                  onChange={(v) => updateCapture('beautyShellTheme', v)}
                  options={[
                    { value: 'aurora_purple', label: t('capture.themeAuroraPurple') },
                    { value: 'sunrise_coral', label: t('capture.themeSunriseCoral') },
                    { value: 'emerald_sea', label: t('capture.themeEmeraldSea') },
                    { value: 'obsidian_dark', label: t('capture.themeObsidianDark') },
                    { value: 'minimal_light', label: t('capture.themeMinimalLight') },
                  ]}
                />
              </SettingRow>
              <SettingRow label={t('capture.beautyShellPadding')} description={t('capture.beautyShellPaddingDesc')}>
                <Select
                  id="capture-beauty-padding"
                  value={String(capture.beautyShellPadding ?? 32)}
                  onChange={(v) => updateCapture('beautyShellPadding', parseInt(v))}
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
                  onChange={(v) => updateCapture('beautyShellRadius', parseInt(v))}
                  options={BEAUTY_SHELL_RADIUS_PRESETS.map((px) => ({
                    value: String(px),
                    label: `${px} px`,
                  }))}
                />
              </SettingRow>
            </>
          )}
        </Card>
      </SettingGroup>

      <PinGroupSettingsCard />

      <SettingGroup title={t('recording.title')} icon={<Video size={20} strokeWidth={2.5} />}>
        <Card>
          <SettingRow
            label={
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
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
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
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
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
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
            <div style={{ display: 'flex', gap: '8px', alignItems: 'center', flexWrap: 'wrap' }}>
              <TextInput id="recordSaveDir" value={recording.saveDirectory || ''} readOnly onChange={() => {}} />
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
              onChange={(v) => updateRecording('fps', parseInt(v))}
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
              onChange={(v) => updateRecording('bitrate', parseInt(v))}
              options={bitrateOptions(recording.bitrate)}
            />
          </SettingRow>
          <SettingRow label={t('recording.countdown')} description={t('recording.countdownDesc')}>
            <Select
              id="record-countdown"
              value={String(recording.countdownSeconds)}
              onChange={(v) => updateRecording('countdownSeconds', parseInt(v))}
              options={[
                { value: '0', label: t('recording.countdownOff') },
                { value: '3', label: t('recording.countdownSeconds', { count: 3 }) },
                { value: '5', label: t('recording.countdownSeconds', { count: 5 }) },
                { value: '10', label: t('recording.countdownSeconds', { count: 10 }) },
              ]}
            />
          </SettingRow>
          <Toggle
            id="record-include-cursor"
            label={t('recording.includeCursor')}
            description={t('recording.includeCursorDesc')}
            checked={recording.includeCursor}
            onChange={(v) => updateRecording('includeCursor', v)}
          />
          <Toggle
            id="record-click-effects"
            label={t('recording.showClickEffects')}
            description={t('recording.showClickEffectsDesc')}
            checked={recording.showClickEffects}
            onChange={(v) => updateRecording('showClickEffects', v)}
          />
          <Toggle
            id="record-click-zoom"
            label={t('recording.showClickZoom')}
            description={t('recording.showClickZoomDesc')}
            checked={recording.showClickZoom ?? true}
            onChange={(v) => updateRecording('showClickZoom', v)}
          />
          <Toggle
            id="record-include-keycast"
            label={t('recording.includeKeycast')}
            description={t('recording.includeKeycastDesc')}
            checked={recording.includeKeycast ?? true}
            onChange={(v) => updateRecording('includeKeycast', v)}
          />
          <Toggle
            id="record-system-audio"
            label={t('recording.captureSystemAudio')}
            description={t('recording.captureSystemAudioDesc')}
            checked={recording.captureSystemAudio}
            disabled={recording.format === 'gif'}
            onChange={(v) => updateRecording('captureSystemAudio', v)}
          />
          {recording.captureSystemAudio && recording.format !== 'gif' && (
            <>
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
                  onChange={(v) => updateRecording('systemAudioVolume', parseInt(v))}
                  options={audioVolumeOptions(recording.systemAudioVolume)}
                />
              </SettingRow>
            </>
          )}
          <Toggle
            id="record-microphone"
            label={t('recording.captureMicrophone')}
            description={t('recording.captureMicrophoneDesc')}
            checked={recording.captureMicrophone}
            disabled={recording.format === 'gif'}
            onChange={(v) => updateRecording('captureMicrophone', v)}
          />
          {recording.captureMicrophone && recording.format !== 'gif' && (
            <>
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
                  onChange={(v) => updateRecording('microphoneVolume', parseInt(v))}
                  options={audioVolumeOptions(recording.microphoneVolume)}
                />
              </SettingRow>
            </>
          )}
        </Card>
      </SettingGroup>
    </div>
  );
};

/* ─────────────────────────────────────────────────────────────────────────────
 * ClipboardPage — 轻量持久化剪贴板历史设置页
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import {
  ClipboardList,
  HardDrive,
  ShieldCheck,
  Sliders,
  Trash2,
  FolderOpen,
  Play,
  Save,
} from 'lucide-react';
import { toast } from 'sonner';
import {
  Card,
  SettingGroup,
  SettingRow,
  Toggle,
  NumberInput,
  TextInput,
  Button,
  Badge,
  PageSkeleton,
} from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import './ClipboardPage.css';

interface ClipboardSettings {
  enabled: boolean;
  hotkey: string;
  maxItems: number;
  maxDays: number;
  maxImageMb: number;
  autoPaste: boolean;
  plainTextByDefault: boolean;
  captureImages: boolean;
  captureFiles: boolean;
  excludedApps: string;
}

interface ClipboardStats {
  totalCount: number;
  pinnedCount: number;
  imageCount: number;
  diskBytes: number;
  dataDir: string;
}

const DEFAULT_SETTINGS: ClipboardSettings = {
  enabled: true,
  hotkey: 'Alt+V',
  maxItems: 500,
  maxDays: 30,
  maxImageMb: 16,
  autoPaste: true,
  plainTextByDefault: false,
  captureImages: true,
  captureFiles: true,
  excludedApps: 'KeePass.exe,KeePassXC.exe,1Password.exe,Bitwarden.exe,Enpass.exe,LastPass.exe,RoboForm.exe',
};

function formatDiskSize(bytes: number): string {
  if (!bytes || bytes < 1024) return `${bytes || 0} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export const ClipboardPage: FC = () => {
  const { t } = useTranslation();
  const [loading, setLoading] = useState(true);
  const [settings, setSettings] = useState<ClipboardSettings>(DEFAULT_SETTINGS);
  const [excludedOverride, setExcludedOverride] = useState<string | null>(null);
  const [stats, setStats] = useState<ClipboardStats>({
    totalCount: 0,
    pinnedCount: 0,
    imageCount: 0,
    diskBytes: 0,
    dataDir: '',
  });
  const [clearing, setClearing] = useState(false);

  const excludedDraft = excludedOverride ?? settings.excludedApps;
  const excludedDirty = excludedDraft.trim() !== settings.excludedApps.trim();

  const refreshStats = useCallback(async () => {
    try {
      const s = await bridgeRequest<Partial<ClipboardStats>>('clipboard.getStats', undefined, { silent: true });
      if (s && typeof s === 'object') {
        setStats((prev) => ({ ...prev, ...s }));
      }
    } catch {
      // ignore
    }
  }, []);

  useEffect(() => {
    let cancelled = false;
    void Promise.allSettled([
      bridgeRequest<Partial<ClipboardSettings>>('clipboard.getSettings', undefined, { silent: true }).then((res) => {
        if (!cancelled && res && typeof res === 'object') {
          setSettings((prev) => ({ ...prev, ...res }));
        }
      }),
      bridgeRequest<Partial<ClipboardStats>>('clipboard.getStats', undefined, { silent: true }).then((res) => {
        if (!cancelled && res && typeof res === 'object') {
          setStats((prev) => ({ ...prev, ...res }));
        }
      }),
    ]).then(() => {
      if (!cancelled) setLoading(false);
    });
    return () => {
      cancelled = true;
    };
  }, []);

  useBridgeEvent('clipboard:updated', () => {
    void refreshStats();
  });

  const patchSettings = async (patch: Partial<ClipboardSettings>) => {
    setSettings((prev) => ({ ...prev, ...patch }));
    try {
      const res = await bridgeRequest<{ success?: boolean }>('clipboard.updateSettings', patch);
      if (res?.success === false) {
        toast.error(t('clipboardPage.saveFailedToast', 'Failed to save clipboard settings'));
      } else {
        void refreshStats();
      }
    } catch {
      toast.error(t('clipboardPage.saveFailedToast', 'Failed to save clipboard settings'));
    }
  };

  const handleClear = async (keepPinned: boolean) => {
    if (clearing) return;
    setClearing(true);
    try {
      const res = await bridgeRequest<{ success?: boolean; removed?: number }>('clipboard.clearHistory', { keepPinned });
      if (res?.success) {
        toast.success(
          t('clipboardPage.clearedToast', {
            count: res.removed ?? 0,
            defaultValue: `Removed ${res.removed ?? 0} clipboard items`,
          }),
        );
        void refreshStats();
      }
    } finally {
      setClearing(false);
    }
  };

  if (loading) {
    return <PageSkeleton />;
  }

  return (
    <div className="clipboard-page">
      <SettingGroup
        title={t('clipboardPage.overviewGroup', 'Persistent Clipboard History')}
        icon={<ClipboardList size={18} strokeWidth={2.2} aria-hidden="true" />}
      >
        <Card>
          <SettingRow
            label={
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px', flexWrap: 'wrap' }}>
                <span>{t('clipboardPage.enableTitle', 'Enable Persistent Clipboard Manager')}</span>
                <Badge text={t('clipboardPage.rustKernelBadge', 'Rust Persistent Engine')} variant="primary" />
                <Badge
                  text={
                    settings.enabled
                      ? t('clipboardPage.badgeActive', 'Listening (0% Idle CPU)')
                      : t('clipboardPage.badgePaused', 'Disabled')
                  }
                  variant={settings.enabled ? 'success' : 'muted'}
                />
              </div>
            }
            description={t(
              'clipboardPage.enableDesc',
              'Survives Windows shutdown and reboots without losing unpinned entries. Supports instant search, hash deduplication, and 1-click image Pin / OCR / Translate.',
            )}
          >
            <Toggle
              id="clipboard-enable-toggle"
              checked={settings.enabled}
              onChange={(v) => void patchSettings({ enabled: v })}
            />
          </SettingRow>

          <SettingRow
            label={t('clipboardPage.hotkeyTitle', 'Summon Clipboard Panel Shortcut')}
            description={t('clipboardPage.hotkeyDesc', 'Default Alt + V (avoids conflicts with Windows Win + V). Press anywhere to summon.')}
          >
            <HotkeyRecorder
              id="clipboard-hotkey-recorder"
              value={settings.hotkey}
              onChange={(val) => void patchSettings({ hotkey: val })}
              placeholder="Alt+V"
            />
          </SettingRow>

          <SettingRow
            label={t('clipboardPage.quickOpenTitle', 'Open Clipboard Panel Now')}
            description={t('clipboardPage.quickOpenDesc', 'Preview your persisted history, pinned snippets, and captured images.')}
          >
            <Button
              variant="primary"
              onClick={() => void bridgeRequest('clipboard.showWindow')}
            >
              <Play size={14} aria-hidden="true" />
              <span>{t('clipboardPage.openNowBtn', 'Open History Panel')}</span>
            </Button>
          </SettingRow>
        </Card>
      </SettingGroup>

      <SettingGroup
        title={t('clipboardPage.behaviorGroup', 'Paste & Capture Behavior')}
        icon={<Sliders size={18} strokeWidth={2.2} aria-hidden="true" />}
      >
        <Card>
          <SettingRow
            label={t('clipboardPage.autoPasteTitle', 'Auto-Paste on Selection')}
            description={t('clipboardPage.autoPasteDesc', 'Automatically return focus to the previous window and paste immediately after pressing Enter or double-clicking.')}
          >
            <Toggle
              id="clipboard-autopaste-toggle"
              checked={settings.autoPaste}
              onChange={(v) => void patchSettings({ autoPaste: v })}
            />
          </SettingRow>

          <SettingRow
            label={t('clipboardPage.plainTextTitle', 'Paste as Plain Text by Default')}
            description={t('clipboardPage.plainTextDesc', 'Strip rich HTML formatting on paste. You can always press Shift + Enter for one-off plain text paste.')}
          >
            <Toggle
              id="clipboard-plaintext-toggle"
              checked={settings.plainTextByDefault}
              onChange={(v) => void patchSettings({ plainTextByDefault: v })}
            />
          </SettingRow>

          <SettingRow
            label={t('clipboardPage.captureImagesTitle', 'Record Copied Images')}
            description={t('clipboardPage.captureImagesDesc', 'Losslessly deduplicate screenshots and copied images on disk, unlocking 1-click Pin (F3), offline OCR, and in-place translation.')}
          >
            <Toggle
              id="clipboard-capture-images-toggle"
              checked={settings.captureImages}
              onChange={(v) => void patchSettings({ captureImages: v })}
            />
          </SettingRow>

          <SettingRow
            label={t('clipboardPage.captureFilesTitle', 'Record Copied File Paths')}
            description={t('clipboardPage.captureFilesDesc', 'Keep track of files copied from Explorer so you can re-paste them later.')}
          >
            <Toggle
              id="clipboard-capture-files-toggle"
              checked={settings.captureFiles}
              onChange={(v) => void patchSettings({ captureFiles: v })}
            />
          </SettingRow>
        </Card>
      </SettingGroup>

      <SettingGroup
        title={t('clipboardPage.storageGroup', 'Retention & Disk Storage')}
        icon={<HardDrive size={18} strokeWidth={2.2} aria-hidden="true" />}
      >
        <Card>
          <SettingRow layout="vertical" label={t('clipboardPage.statsTitle', 'Local Persistent Store Overview')}>
            <div className="clipboard-page__stats-grid">
              <div className="clipboard-page__stat-card">
                <span className="clipboard-page__stat-label">{t('clipboardPage.statTotal', 'Saved Entries')}</span>
                <span className="clipboard-page__stat-value">{stats.totalCount}</span>
              </div>
              <div className="clipboard-page__stat-card">
                <span className="clipboard-page__stat-label">{t('clipboardPage.statPinned', 'Pinned Forever')}</span>
                <span className="clipboard-page__stat-value">{stats.pinnedCount}</span>
              </div>
              <div className="clipboard-page__stat-card">
                <span className="clipboard-page__stat-label">{t('clipboardPage.statImages', 'Cached Images')}</span>
                <span className="clipboard-page__stat-value">{stats.imageCount}</span>
              </div>
              <div className="clipboard-page__stat-card">
                <span className="clipboard-page__stat-label">{t('clipboardPage.statDisk', 'Disk Usage')}</span>
                <span className="clipboard-page__stat-value">{formatDiskSize(stats.diskBytes)}</span>
              </div>
            </div>
          </SettingRow>

          <SettingRow
            label={t('clipboardPage.maxItemsTitle', 'Maximum Unpinned Entries')}
            description={t('clipboardPage.maxItemsDesc', 'Pinned entries are permanently preserved and never count toward automatic eviction (20 - 5000).')}
          >
            <NumberInput
              id="clipboard-max-items"
              value={settings.maxItems}
              min={20}
              max={5000}
              step={50}
              onChange={(v) => void patchSettings({ maxItems: v })}
            />
          </SettingRow>

          <SettingRow
            label={t('clipboardPage.maxDaysTitle', 'Retention Days (0 = Keep Forever)')}
            description={t('clipboardPage.maxDaysDesc', 'Automatically expire unpinned items older than this number of days.')}
          >
            <NumberInput
              id="clipboard-max-days"
              value={settings.maxDays}
              min={0}
              max={3650}
              step={5}
              onChange={(v) => void patchSettings({ maxDays: v })}
            />
          </SettingRow>

          <SettingRow
            label={t('clipboardPage.maxImageMbTitle', 'Max Single Image Size (MB)')}
            description={t('clipboardPage.maxImageMbDesc', 'Skip oversized bitmap captures exceeding this PNG size cap (1 - 64 MB).')}
          >
            <NumberInput
              id="clipboard-max-img-mb"
              value={settings.maxImageMb}
              min={1}
              max={64}
              step={1}
              onChange={(v) => void patchSettings({ maxImageMb: v })}
            />
          </SettingRow>

          <SettingRow
            label={t('clipboardPage.maintenanceTitle', 'Storage Maintenance')}
            description={t('clipboardPage.maintenanceDesc', 'Inspect local storage folder or purge history immediately.')}
          >
            <div className="clipboard-page__actions-row">
              <Button variant="secondary" onClick={() => void bridgeRequest('clipboard.openDataDir')}>
                <FolderOpen size={14} aria-hidden="true" />
                <span>{t('clipboardPage.openFolderBtn', 'Open Folder')}</span>
              </Button>
              <Button variant="secondary" disabled={clearing} onClick={() => void handleClear(true)}>
                <Trash2 size={14} aria-hidden="true" />
                <span>{t('clipboardPage.clearUnpinnedBtn', 'Clear Unpinned')}</span>
              </Button>
              <Button variant="danger" disabled={clearing} onClick={() => void handleClear(false)}>
                <Trash2 size={14} aria-hidden="true" />
                <span>{t('clipboardPage.clearAllBtn', 'Clear All')}</span>
              </Button>
            </div>
          </SettingRow>
        </Card>
      </SettingGroup>

      <SettingGroup
        title={t('clipboardPage.privacyGroup', 'Privacy & Password Manager Guard')}
        icon={<ShieldCheck size={18} strokeWidth={2.2} aria-hidden="true" />}
      >
        <Card>
          <div className="clipboard-page__privacy-banner" role="note">
            <ShieldCheck size={18} strokeWidth={2.2} className="clipboard-page__privacy-icon" aria-hidden="true" />
            <span>
              {t(
                'clipboardPage.privacyBanner',
                'Automatically honors Windows ExcludeClipboardContentFromMonitorProcessing, CanIncludeInClipboardHistory=0, and Clipboard Viewer Ignore formats. Passwords copied from compliant managers are never recorded.',
              )}
            </span>
          </div>

          <SettingRow
            layout="vertical"
            label={t('clipboardPage.excludedAppsTitle', 'Excluded Applications (Process Blacklist)')}
            description={t('clipboardPage.excludedAppsDesc', 'Comma-separated process names whose clipboard writes are always ignored.')}
          >
            <div style={{ display: 'flex', gap: '10px', width: '100%', alignItems: 'center', flexWrap: 'wrap' }}>
              <div style={{ flex: 1, minWidth: '240px' }}>
                <TextInput
                  id="clipboard-excluded-apps"
                  value={excludedDraft}
                  onChange={setExcludedOverride}
                  placeholder="KeePass.exe,1Password.exe,Bitwarden.exe"
                />
              </div>
              {excludedDirty && (
                <Button
                  variant="primary"
                  onClick={async () => {
                    await patchSettings({ excludedApps: excludedDraft.trim() });
                    setExcludedOverride(null);
                    toast.success(t('clipboardPage.excludedSavedToast', 'Excluded apps list saved'));
                  }}
                >
                  <Save size={14} aria-hidden="true" />
                  <span>{t('clipboardPage.saveBtn', 'Save')}</span>
                </Button>
              )}
            </div>
          </SettingRow>
        </Card>
      </SettingGroup>
    </div>
  );
};

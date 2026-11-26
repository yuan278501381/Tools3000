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
  Monitor,
  LocateFixed,
  RotateCcw,
} from 'lucide-react';
import { toast } from 'sonner';
import {
  Card,
  SettingGroup,
  SettingRow,
  SubSettingGroup,
  DisabledCascade,
  ConfirmModal,
  Toggle,
  NumberInput,
  TextInput,
  Button,
  Badge,
  PageSkeleton,
} from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import './ClipboardPage.css';

type ClipboardWindowPositionMode = 'follow_mouse' | 'remember_last' | 'fixed';

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
  hidePinnedInOtherCategories: boolean;
  hideOnBlur: boolean;
  hideOnPaste: boolean;
  keepOpenAfterDrag: boolean;
  singleClickPaste: boolean;
  excludedApps: string;
  windowPositionMode: ClipboardWindowPositionMode;
  windowWidthDip: number;
  windowHeightDip: number;
  hasFixedAnchor: boolean;
  fixedX: number;
  fixedY: number;
  fixedMonitorDevice: string;
  lockWindowBounds: boolean;
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
  hidePinnedInOtherCategories: false,
  hideOnBlur: true,
  hideOnPaste: true,
  keepOpenAfterDrag: true,
  singleClickPaste: false,
  excludedApps: 'KeePass.exe,KeePassXC.exe,1Password.exe,Bitwarden.exe,Enpass.exe,LastPass.exe,RoboForm.exe',
  windowPositionMode: 'follow_mouse',
  windowWidthDip: 760,
  windowHeightDip: 560,
  hasFixedAnchor: false,
  fixedX: 0,
  fixedY: 0,
  fixedMonitorDevice: '',
  lockWindowBounds: false,
};

function formatMonitorDeviceLabel(device: string): string {
  const trimmed = device.trim();
  if (!trimmed) return '';
  const match = trimmed.match(/DISPLAY(\d+)$/i);
  if (match) {
    return `Display ${match[1]}`;
  }
  return trimmed.replace(/^\\\\\.\\/, '');
}

function formatDiskSize(bytes: number): string {
  if (!bytes || bytes < 1024) return `${bytes || 0} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export const ClipboardPage: FC = () => {
  const { t } = useTranslation();
  const [loading, setLoading] = useState(true);
  const [settings, setSettings] = useState<ClipboardSettings>(DEFAULT_SETTINGS);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [excludedOverride, setExcludedOverride] = useState<string | null>(null);
  const [stats, setStats] = useState<ClipboardStats>({
    totalCount: 0,
    pinnedCount: 0,
    imageCount: 0,
    diskBytes: 0,
    dataDir: '',
  });
  const [clearing, setClearing] = useState(false);
  const [confirmClearAllOpen, setConfirmClearAllOpen] = useState(false);

  const excludedDraft = excludedOverride ?? settings.excludedApps;
  const excludedDirty = excludedDraft.trim() !== settings.excludedApps.trim();
  const clipboardHotkeyEntry = hotkeys.find((h) => h.name === 'Toggle Clipboard');

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

  const refreshHotkeys = useCallback(async () => {
    try {
      const res = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll', undefined, { silent: true });
      if (Array.isArray(res)) {
        setHotkeys(res);
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
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll', undefined, { silent: true }).then((res) => {
        if (!cancelled && Array.isArray(res)) {
          setHotkeys(res);
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

  useBridgeEvent('clipboard:settingsChanged', (rawData) => {
    const data = rawData as Partial<ClipboardSettings> | undefined;
    if (!data || typeof data !== 'object') return;
    setSettings((prev) => ({
      ...prev,
      ...(typeof data.autoPaste === 'boolean' ? { autoPaste: data.autoPaste } : {}),
      ...(typeof data.plainTextByDefault === 'boolean' ? { plainTextByDefault: data.plainTextByDefault } : {}),
      ...(typeof data.hidePinnedInOtherCategories === 'boolean'
        ? { hidePinnedInOtherCategories: data.hidePinnedInOtherCategories }
        : {}),
      ...(typeof data.hideOnBlur === 'boolean' ? { hideOnBlur: data.hideOnBlur } : {}),
      ...(typeof data.hideOnPaste === 'boolean' ? { hideOnPaste: data.hideOnPaste } : {}),
      ...(typeof data.keepOpenAfterDrag === 'boolean' ? { keepOpenAfterDrag: data.keepOpenAfterDrag } : {}),
      ...(typeof data.singleClickPaste === 'boolean' ? { singleClickPaste: data.singleClickPaste } : {}),
      ...(data.windowPositionMode === 'follow_mouse' ||
      data.windowPositionMode === 'remember_last' ||
      data.windowPositionMode === 'fixed'
        ? { windowPositionMode: data.windowPositionMode }
        : {}),
      ...(typeof data.windowWidthDip === 'number' ? { windowWidthDip: data.windowWidthDip } : {}),
      ...(typeof data.windowHeightDip === 'number' ? { windowHeightDip: data.windowHeightDip } : {}),
      ...(typeof data.hasFixedAnchor === 'boolean' ? { hasFixedAnchor: data.hasFixedAnchor } : {}),
      ...(typeof data.fixedX === 'number' ? { fixedX: data.fixedX } : {}),
      ...(typeof data.fixedY === 'number' ? { fixedY: data.fixedY } : {}),
      ...(typeof data.fixedMonitorDevice === 'string' ? { fixedMonitorDevice: data.fixedMonitorDevice } : {}),
      ...(typeof data.lockWindowBounds === 'boolean' ? { lockWindowBounds: data.lockWindowBounds } : {}),
    }));
  });

  const handleCaptureFixedPlacement = async () => {
    try {
      const res = await bridgeRequest<{ success?: boolean; captured?: boolean; settings?: Partial<ClipboardSettings> }>(
        'clipboard.captureFixedPlacement',
      );
      if (res?.settings && typeof res.settings === 'object') {
        setSettings((prev) => ({ ...prev, ...res.settings }));
      }
      if (res?.captured) {
        toast.success(t('clipboardPage.placementCapturedToast', 'Current clipboard panel position and size saved'));
      } else {
        toast.error(
          t(
            'clipboardPage.placementCaptureNeedOpenToast',
            'Open the clipboard panel and move it to the target position first, then capture again',
          ),
        );
      }
    } catch {
      toast.error(t('clipboardPage.saveFailedToast', 'Failed to save clipboard settings'));
    }
  };

  const handleResetPlacement = async () => {
    try {
      const res = await bridgeRequest<{ success?: boolean; settings?: Partial<ClipboardSettings> }>(
        'clipboard.resetPlacement',
      );
      if (res?.settings && typeof res.settings === 'object') {
        setSettings((prev) => ({ ...prev, ...res.settings }));
      } else {
        setSettings((prev) => ({
          ...prev,
          windowPositionMode: 'follow_mouse',
          windowWidthDip: 760,
          windowHeightDip: 560,
          hasFixedAnchor: false,
          fixedX: 0,
          fixedY: 0,
          fixedMonitorDevice: '',
          lockWindowBounds: false,
        }));
      }
      toast.success(t('clipboardPage.placementResetToast', 'Clipboard panel position and size reset to defaults'));
    } catch {
      toast.error(t('clipboardPage.saveFailedToast', 'Failed to save clipboard settings'));
    }
  };

  const patchSettings = async (patch: Partial<ClipboardSettings>) => {
    setSettings((prev) => ({ ...prev, ...patch }));
    try {
      const res = await bridgeRequest<{ success?: boolean }>('clipboard.updateSettings', patch);
      if (res?.success === false) {
        toast.error(t('clipboardPage.saveFailedToast', 'Failed to save clipboard settings'));
      } else {
        void refreshStats();
        if ('hotkey' in patch || 'enabled' in patch) {
          void refreshHotkeys();
        }
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
              <div className="clipboard-page__row-label">
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

          <DisabledCascade disabled={!settings.enabled}>
            <SettingRow
              label={
                <div className="clipboard-page__row-label">
                  <span>{t('clipboardPage.hotkeyTitle', 'Summon Clipboard Panel Shortcut')}</span>
                  <HotkeyStatusBadge entry={clipboardHotkeyEntry} />
                </div>
              }
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
                disabled={!settings.enabled}
                onClick={() => void bridgeRequest('clipboard.showWindow')}
              >
                <Play size={14} aria-hidden="true" />
                <span>{t('clipboardPage.openNowBtn', 'Open History Panel')}</span>
              </Button>
            </SettingRow>
          </DisabledCascade>
        </Card>
      </SettingGroup>

      <DisabledCascade disabled={!settings.enabled} className="clipboard-page__cascade">
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
              label={t('clipboardPage.hideOnBlurTitle', 'Auto-Hide When Clicking Outside')}
              description={t('clipboardPage.hideOnBlurDesc', 'Automatically hide the clipboard panel when focus moves elsewhere. Turn off to keep it open like a floating reference tool until you press Esc or close it.')}
            >
              <Toggle
                id="clipboard-hide-on-blur-toggle"
                checked={settings.hideOnBlur}
                onChange={(v) => void patchSettings({ hideOnBlur: v })}
              />
            </SettingRow>

            <SettingRow
              label={t('clipboardPage.hideOnPasteTitle', 'Auto-Hide After Pasting or Copying')}
              description={t('clipboardPage.hideOnPasteDesc', 'Automatically close the panel after activating an entry. Turn off for continuous multi-item pasting without pinning the panel first.')}
            >
              <Toggle
                id="clipboard-hide-on-paste-toggle"
                checked={settings.hideOnPaste}
                onChange={(v) => void patchSettings({ hideOnPaste: v })}
              />
            </SettingRow>

            <SettingRow
              label={t('clipboardPage.keepOpenAfterDragTitle', 'Keep Panel Open After Drag-Paste')}
              description={t('clipboardPage.keepOpenAfterDragDesc', 'Keep the panel floating after a drag-to-paste delivery so you can drop multiple items in a row; turn off to auto-hide after one drop.')}
            >
              <Toggle
                id="clipboard-keep-after-drag-toggle"
                checked={settings.keepOpenAfterDrag}
                onChange={(v) => void patchSettings({ keepOpenAfterDrag: v })}
              />
            </SettingRow>

            <SettingRow
              label={t('clipboardPage.singleClickPasteTitle', 'Single-Click Item to Paste Immediately')}
              description={t('clipboardPage.singleClickPasteDesc', 'Paste immediately on a single left-click (Win + V style) instead of double-clicking. Drag-paste, checkboxes, and Ctrl/Shift multi-select remain unaffected.')}
            >
              <Toggle
                id="clipboard-single-click-paste-toggle"
                checked={settings.singleClickPaste}
                onChange={(v) => void patchSettings({ singleClickPaste: v })}
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

            <SubSettingGroup disabled={!settings.captureImages}>
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
                  disabled={!settings.captureImages}
                  onChange={(v) => void patchSettings({ maxImageMb: v })}
                />
              </SettingRow>
            </SubSettingGroup>

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

            <SettingRow
              label={t('clipboardPage.hidePinnedInOtherTitle', 'Hide Pinned Items in Other Categories')}
              description={t(
                'clipboardPage.hidePinnedInOtherDesc',
                'Keep pinned entries exclusively in the Pinned tab so they do not occupy space in All, Text, Images, Files, or Colors.',
              )}
            >
              <Toggle
                id="clipboard-hide-pinned-toggle"
                checked={settings.hidePinnedInOtherCategories}
                onChange={(v) => void patchSettings({ hidePinnedInOtherCategories: v })}
              />
            </SettingRow>
          </Card>
        </SettingGroup>

        <SettingGroup
          title={t('clipboardPage.placementGroup', 'Window Position & Size (Multi-Monitor)')}
          icon={<Monitor size={18} strokeWidth={2.2} aria-hidden="true" />}
        >
          <Card>
            <SettingRow
              layout="vertical"
              label={t('clipboardPage.positionModeTitle', 'Panel Placement Mode')}
              description={t(
                'clipboardPage.positionModeDesc',
                'Choose whether the clipboard panel follows your active monitor, remembers its last position, or stays pinned at fixed screen coordinates with automatic DPI scaling.',
              )}
            >
              <div
                className="clipboard-page__mode-group"
                role="radiogroup"
                aria-label={t('clipboardPage.positionModeTitle', 'Panel Placement Mode')}
              >
                {(
                  [
                    {
                      id: 'follow_mouse' as const,
                      title: t('clipboardPage.modeFollowMouse', 'Follow Active Monitor'),
                      desc: t(
                        'clipboardPage.modeFollowMouseDesc',
                        'Centers on the monitor under your cursor and adapts cleanly to mixed DPI scales.',
                      ),
                    },
                    {
                      id: 'remember_last' as const,
                      title: t('clipboardPage.modeRememberLast', 'Remember Last Position'),
                      desc: t(
                        'clipboardPage.modeRememberLastDesc',
                        'Persists the last dragged location and size across restarts; falls back safely if a monitor disconnects.',
                      ),
                    },
                    {
                      id: 'fixed' as const,
                      title: t('clipboardPage.modeFixed', 'Fixed Position & Size'),
                      desc: t(
                        'clipboardPage.modeFixedDesc',
                        'Always opens at the exact pinned screen coordinates and logical DIP size.',
                      ),
                    },
                  ] as const
                ).map((mode) => {
                  const active = settings.windowPositionMode === mode.id;
                  return (
                    <button
                      key={mode.id}
                      type="button"
                      role="radio"
                      aria-checked={active}
                      className={`clipboard-page__mode-card${active ? ' clipboard-page__mode-card--active' : ''}`}
                      onClick={() => void patchSettings({ windowPositionMode: mode.id })}
                    >
                      <span className="clipboard-page__mode-title">{mode.title}</span>
                      <span className="clipboard-page__mode-desc">{mode.desc}</span>
                    </button>
                  );
                })}
              </div>
            </SettingRow>

            <SettingRow
              label={t('clipboardPage.windowSizeTitle', 'Panel Logical Size (DIP)')}
              description={t(
                'clipboardPage.windowSizeDesc',
                'Stored in DPI-independent units (560-2400 x 420-1600) so the panel keeps a consistent visual size across 100%, 150%, and 200% displays.',
              )}
            >
              <div className="clipboard-page__dims-row">
                <label className="clipboard-page__dim-field" htmlFor="clipboard-window-width-dip">
                  <span className="clipboard-page__dim-label">{t('clipboardPage.widthLabel', 'Width')}</span>
                  <NumberInput
                    id="clipboard-window-width-dip"
                    value={settings.windowWidthDip}
                    min={560}
                    max={2400}
                    step={20}
                    onChange={(v) => void patchSettings({ windowWidthDip: v })}
                  />
                </label>
                <label className="clipboard-page__dim-field" htmlFor="clipboard-window-height-dip">
                  <span className="clipboard-page__dim-label">{t('clipboardPage.heightLabel', 'Height')}</span>
                  <NumberInput
                    id="clipboard-window-height-dip"
                    value={settings.windowHeightDip}
                    min={420}
                    max={1600}
                    step={20}
                    onChange={(v) => void patchSettings({ windowHeightDip: v })}
                  />
                </label>
              </div>
            </SettingRow>

            <SettingRow
              layout="vertical"
              label={t('clipboardPage.anchorActionsTitle', 'Quick Capture & Reset')}
              description={t(
                'clipboardPage.anchorActionsDesc',
                'Drag and resize the clipboard panel to your preferred spot (or click the target icon in its title bar), then capture it as the fixed position.',
              )}
            >
              <div className="clipboard-page__anchor-bar">
                <span className="clipboard-page__anchor-status">
                  {settings.hasFixedAnchor
                    ? t('clipboardPage.anchorSavedSummary', {
                        monitor: formatMonitorDeviceLabel(settings.fixedMonitorDevice) || t('clipboardPage.anchorPrimaryMonitor', 'Primary'),
                        x: settings.fixedX,
                        y: settings.fixedY,
                        w: settings.windowWidthDip,
                        h: settings.windowHeightDip,
                        defaultValue: `Saved anchor: ${formatMonitorDeviceLabel(settings.fixedMonitorDevice) || 'Primary'} · (${settings.fixedX}, ${settings.fixedY}) · ${settings.windowWidthDip} x ${settings.windowHeightDip} DIP`,
                      })
                    : t(
                        'clipboardPage.anchorNotSetSummary',
                        'No custom anchor captured yet (centered automatically on target monitor).',
                      )}
                </span>
                <div className="clipboard-page__actions-row">
                  <Button variant="primary" onClick={() => void handleCaptureFixedPlacement()}>
                    <LocateFixed size={14} aria-hidden="true" />
                    <span>{t('clipboardPage.captureCurrentBtn', 'Capture Current Position')}</span>
                  </Button>
                  <Button variant="secondary" onClick={() => void handleResetPlacement()}>
                    <RotateCcw size={14} aria-hidden="true" />
                    <span>{t('clipboardPage.resetPlacementBtn', 'Reset to Default')}</span>
                  </Button>
                </div>
              </div>
            </SettingRow>

            {settings.windowPositionMode === 'fixed' && (
              <SubSettingGroup>
                <SettingRow
                  label={t('clipboardPage.fixedCoordsTitle', 'Fixed Screen Coordinates (px)')}
                  description={t(
                    'clipboardPage.fixedCoordsDesc',
                    'Fine-tune the top-left desktop coordinates. Negative values are supported for left or upper secondary monitors.',
                  )}
                >
                  <div className="clipboard-page__dims-row">
                    <label className="clipboard-page__dim-field" htmlFor="clipboard-fixed-x">
                      <span className="clipboard-page__dim-label">X</span>
                      <NumberInput
                        id="clipboard-fixed-x"
                        value={settings.fixedX}
                        min={-16000}
                        max={16000}
                        step={10}
                        onChange={(v) => void patchSettings({ hasFixedAnchor: true, fixedX: v })}
                      />
                    </label>
                    <label className="clipboard-page__dim-field" htmlFor="clipboard-fixed-y">
                      <span className="clipboard-page__dim-label">Y</span>
                      <NumberInput
                        id="clipboard-fixed-y"
                        value={settings.fixedY}
                        min={-16000}
                        max={16000}
                        step={10}
                        onChange={(v) => void patchSettings({ hasFixedAnchor: true, fixedY: v })}
                      />
                    </label>
                  </div>
                </SettingRow>

                <SettingRow
                  label={t('clipboardPage.lockBoundsTitle', 'Lock Panel Position & Size')}
                  description={t(
                    'clipboardPage.lockBoundsDesc',
                    'Prevent accidental dragging, edge resizing, or title-bar double-click maximize while Fixed mode is active.',
                  )}
                >
                  <Toggle
                    id="clipboard-lock-bounds-toggle"
                    checked={settings.lockWindowBounds}
                    onChange={(v) => void patchSettings({ lockWindowBounds: v })}
                  />
                </SettingRow>
              </SubSettingGroup>
            )}
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
                <Button variant="danger" disabled={clearing} onClick={() => setConfirmClearAllOpen(true)}>
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
              <div className="clipboard-page__excluded-row">
                <div className="clipboard-page__excluded-input">
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
      </DisabledCascade>

      <ConfirmModal
        open={confirmClearAllOpen}
        title={t('clipboardPage.clearAllBtn', 'Clear All')}
        description={t('history.confirmClear', 'Are you sure you want to clear all history records?')}
        confirmText={t('history.confirmClearBtn', 'Confirm Clear')}
        cancelText={t('common.cancel', 'Cancel')}
        loading={clearing}
        onConfirm={async () => {
          await handleClear(false);
          setConfirmClearAllOpen(false);
        }}
        onClose={() => setConfirmClearAllOpen(false)}
      />
    </div>
  );
};

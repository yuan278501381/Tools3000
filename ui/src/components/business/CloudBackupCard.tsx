import { useState, useEffect, useCallback, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Cloud,
  FolderSync,
  Server,
  HardDrive,
  Lock,
  ShieldCheck,
  RefreshCw,
  Upload,
  Download,
  FolderOpen,
  Check,
  Save,
} from 'lucide-react';
import {
  Card,
  SettingRow,
  Toggle,
  Button,
  CodeBadge,
  ConfirmModal,
} from '../UIKit';
import { toast } from 'sonner';
import { bridgeRequest } from '../../hooks/useBridge';
import './CloudBackupCard.css';

export type BackupProvider = 'folder' | 'webdav' | 's3';

export interface CloudBackupSettingsDto {
  enabled: boolean;
  provider: BackupProvider;
  autoBackupOnExit: boolean;
  keepCount: number;
  encryptionEnabled: boolean;
  folderPath: string;
  webdavUrl: string;
  webdavUsername: string;
  hasWebdavPassword: boolean;
  s3Endpoint: string;
  s3Region: string;
  s3Bucket: string;
  s3Prefix: string;
  s3AccessKeyId: string;
  s3ForcePathStyle: boolean;
  hasS3SecretKey: boolean;
  hasEncryptionPassphrase: boolean;
  lastBackupAtIso: string;
  lastBackupStatus: string;
  lastBackupFile: string;
}

export interface DetectedSyncFolderDto {
  providerId: string;
  label: string;
  path: string;
}

export interface BackupSnapshotEntryDto {
  id: string;
  fileName: string;
  sizeBytes: number;
  modifiedAtIso: string;
  encrypted: boolean;
}

interface BackupSettingsResponse {
  success: boolean;
  settings?: CloudBackupSettingsDto;
  detectedFolders?: DetectedSyncFolderDto[];
}

interface BackupOpResponse {
  success: boolean;
  errorCode?: string;
  message?: string;
  snapshotName?: string;
  preRestoreBackupPath?: string;
  bytesTransferred?: number;
  prunedCount?: number;
  snapshots?: BackupSnapshotEntryDto[];
  settings?: CloudBackupSettingsDto;
}

const DEFAULT_BACKUP_SETTINGS: CloudBackupSettingsDto = {
  enabled: false,
  provider: 'folder',
  autoBackupOnExit: false,
  keepCount: 10,
  encryptionEnabled: false,
  folderPath: '',
  webdavUrl: '',
  webdavUsername: '',
  hasWebdavPassword: false,
  s3Endpoint: '',
  s3Region: 'auto',
  s3Bucket: '',
  s3Prefix: 'tools3000-backups/',
  s3AccessKeyId: '',
  s3ForcePathStyle: true,
  hasS3SecretKey: false,
  hasEncryptionPassphrase: false,
  lastBackupAtIso: '',
  lastBackupStatus: '',
  lastBackupFile: '',
};

function formatSnapshotBytes(bytes: number): string {
  if (!bytes || bytes < 1024) return `${bytes || 0} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export interface CloudBackupCardProps {
  onConfigRestored?: () => void;
}

export const CloudBackupCard: FC<CloudBackupCardProps> = ({ onConfigRestored }) => {
  const { t } = useTranslation();
  const [settings, setSettings] = useState<CloudBackupSettingsDto>(DEFAULT_BACKUP_SETTINGS);
  const [detectedFolders, setDetectedFolders] = useState<DetectedSyncFolderDto[]>([]);
  const [webdavPassword, setWebdavPassword] = useState('');
  const [s3SecretKey, setS3SecretKey] = useState('');
  const [encryptionPassphrase, setEncryptionPassphrase] = useState('');

  const [saving, setSaving] = useState(false);
  const [testing, setTesting] = useState(false);
  const [backingUp, setBackingUp] = useState(false);
  const [loadingSnapshots, setLoadingSnapshots] = useState(false);
  const [restoring, setRestoring] = useState(false);

  const [snapshots, setSnapshots] = useState<BackupSnapshotEntryDto[]>([]);
  const [snapshotsLoaded, setSnapshotsLoaded] = useState(false);
  const [restoreTarget, setRestoreTarget] = useState<BackupSnapshotEntryDto | null>(null);
  const [restorePassphrase, setRestorePassphrase] = useState('');

  useEffect(() => {
    let cancelled = false;
    void (async () => {
      try {
        const res = await bridgeRequest<BackupSettingsResponse>('backup.getSettings', undefined, { silent: true });
        if (cancelled) return;
        if (res?.success && res.settings) {
          setSettings({ ...DEFAULT_BACKUP_SETTINGS, ...res.settings });
        }
        if (Array.isArray(res?.detectedFolders)) {
          setDetectedFolders(res.detectedFolders);
        }
      } catch {
        // 忽略首次静默加载异常
      }
    })();
    return () => {
      cancelled = true;
    };
  }, []);

  const buildDraftPayload = useCallback(() => ({
    enabled: settings.enabled,
    provider: settings.provider,
    autoBackupOnExit: settings.autoBackupOnExit,
    keepCount: settings.keepCount,
    encryptionEnabled: settings.encryptionEnabled,
    folderPath: settings.folderPath.trim(),
    webdavUrl: settings.webdavUrl.trim(),
    webdavUsername: settings.webdavUsername.trim(),
    webdavPassword: webdavPassword || undefined,
    s3Endpoint: settings.s3Endpoint.trim(),
    s3Region: settings.s3Region.trim() || 'auto',
    s3Bucket: settings.s3Bucket.trim(),
    s3Prefix: settings.s3Prefix.trim(),
    s3AccessKeyId: settings.s3AccessKeyId.trim(),
    s3SecretKey: s3SecretKey || undefined,
    s3ForcePathStyle: settings.s3ForcePathStyle,
    encryptionPassphrase: encryptionPassphrase || undefined,
  }), [encryptionPassphrase, s3SecretKey, settings, webdavPassword]);

  const handleSave = useCallback(async (silentSuccess = false): Promise<boolean> => {
    if (saving) return false;
    setSaving(true);
    try {
      const res = await bridgeRequest<BackupSettingsResponse>('backup.saveSettings', buildDraftPayload());
      if (res?.success && res.settings) {
        setSettings({ ...DEFAULT_BACKUP_SETTINGS, ...res.settings });
        setWebdavPassword('');
        setS3SecretKey('');
        setEncryptionPassphrase('');
        if (!silentSuccess) {
          toast.success(t('backup.savedSuccess', 'Cloud backup settings saved'));
        }
        return true;
      }
      toast.error(t('backup.saveFailed', 'Failed to save cloud backup settings'));
      return false;
    } catch (error) {
      toast.error(t('backup.saveFailed', 'Failed to save cloud backup settings'), {
        description: String(error),
      });
      return false;
    } finally {
      setSaving(false);
    }
  }, [buildDraftPayload, saving, t]);

  const handleBrowseFolder = async () => {
    try {
      const picked = await bridgeRequest<string | null>('backup.browseDirectory');
      if (picked) {
        setSettings((prev) => ({ ...prev, folderPath: picked }));
      }
    } catch (error) {
      toast.error(t('backup.browseFailed', 'Failed to pick backup folder'), {
        description: String(error),
      });
    }
  };

  const handleTestConnection = async () => {
    if (testing) return;
    setTesting(true);
    try {
      const res = await bridgeRequest<BackupOpResponse>('backup.testConnection', buildDraftPayload());
      if (res?.success) {
        toast.success(t('backup.testSuccess', 'Connection verified successfully'), {
          description: res.message || undefined,
        });
      } else {
        toast.error(t('backup.testFailed', 'Connection test failed'), {
          description: res?.errorCode || res?.message,
        });
      }
    } catch (error) {
      toast.error(t('backup.testFailed', 'Connection test failed'), {
        description: String(error),
      });
    } finally {
      setTesting(false);
    }
  };

  const handleListSnapshots = useCallback(async (showToast = false) => {
    if (loadingSnapshots) return;
    setLoadingSnapshots(true);
    try {
      const res = await bridgeRequest<BackupOpResponse>('backup.listSnapshots', buildDraftPayload());
      if (res?.success) {
        setSnapshots(Array.isArray(res.snapshots) ? res.snapshots : []);
        setSnapshotsLoaded(true);
        if (showToast) {
          toast.info(t('backup.listLoaded', 'Snapshot list refreshed'));
        }
      } else {
        toast.error(t('backup.listFailed', 'Failed to load backup snapshots'), {
          description: res?.errorCode || res?.message,
        });
      }
    } catch (error) {
      toast.error(t('backup.listFailed', 'Failed to load backup snapshots'), {
        description: String(error),
      });
    } finally {
      setLoadingSnapshots(false);
    }
  }, [buildDraftPayload, loadingSnapshots, t]);

  const handleBackupNow = async () => {
    if (backingUp) return;
    const saved = await handleSave(true);
    if (!saved) return;

    setBackingUp(true);
    try {
      const res = await bridgeRequest<BackupOpResponse>('backup.backupNow');
      if (res?.success) {
        if (res.settings) {
          setSettings({ ...DEFAULT_BACKUP_SETTINGS, ...res.settings });
        }
        toast.success(t('backup.backupSuccess', 'Configuration backup uploaded'), {
          description: res.snapshotName
            ? `${res.snapshotName} (${formatSnapshotBytes(res.bytesTransferred || 0)})`
            : undefined,
        });
        await handleListSnapshots(false);
      } else {
        toast.error(t('backup.backupFailed', 'Configuration backup failed'), {
          description: res?.errorCode || res?.message,
        });
      }
    } catch (error) {
      toast.error(t('backup.backupFailed', 'Configuration backup failed'), {
        description: String(error),
      });
    } finally {
      setBackingUp(false);
    }
  };

  const handleConfirmRestore = async () => {
    if (!restoreTarget || restoring) return;
    setRestoring(true);
    try {
      const effectivePassphrase = restorePassphrase || encryptionPassphrase;
      const res = await bridgeRequest<BackupOpResponse>('backup.restoreSnapshot', {
        snapshotId: restoreTarget.id,
        passphrase: effectivePassphrase || undefined,
      });
      if (res?.success) {
        toast.success(t('backup.restoreSuccess', 'Configuration restored safely'), {
          description: res.preRestoreBackupPath
            ? t('backup.preRestoreSaved', 'Previous config was backed up automatically')
            : undefined,
        });
        setRestoreTarget(null);
        setRestorePassphrase('');
        onConfigRestored?.();
      } else {
        toast.error(t('backup.restoreFailed', 'Failed to restore configuration'), {
          description: res?.errorCode || res?.message,
        });
      }
    } catch (error) {
      toast.error(t('backup.restoreFailed', 'Failed to restore configuration'), {
        description: String(error),
      });
    } finally {
      setRestoring(false);
    }
  };

  return (
    <Card className="cloud-backup-card">
      <div className="cloud-backup__header">
        <div className="cloud-backup__title-wrap">
          <Cloud size={18} strokeWidth={2.2} className="cloud-backup__title-icon" aria-hidden="true" />
          <div>
            <h3 className="cloud-backup__title">{t('backup.title', 'Cloud & Sync Folder Backup')}</h3>
            <p className="cloud-backup__subtitle">
              {t('backup.subtitle', 'Back up configs and custom Lua scripts to OneDrive, Google Drive, WebDAV, or S3-compatible storage (credentials encrypted via Windows DPAPI).')}
            </p>
          </div>
        </div>
        {settings.lastBackupFile && (
          <div className="cloud-backup__last-meta">
            <ShieldCheck size={14} aria-hidden="true" />
            <span>{settings.lastBackupFile}</span>
          </div>
        )}
      </div>

      {/* ── 协议多态切换 ── */}
      <div className="cloud-backup__providers" role="tablist" aria-label={t('backup.providerLabel', 'Backup provider')}>
        <button
          type="button"
          role="tab"
          aria-selected={settings.provider === 'folder'}
          className={`cloud-backup__provider-btn${settings.provider === 'folder' ? ' is-active' : ''}`}
          onClick={() => setSettings((prev) => ({ ...prev, provider: 'folder' }))}
        >
          <FolderSync size={15} aria-hidden="true" />
          <span>{t('backup.providerFolder', 'Sync Folder (OneDrive / Google Drive)')}</span>
        </button>
        <button
          type="button"
          role="tab"
          aria-selected={settings.provider === 'webdav'}
          className={`cloud-backup__provider-btn${settings.provider === 'webdav' ? ' is-active' : ''}`}
          onClick={() => setSettings((prev) => ({ ...prev, provider: 'webdav' }))}
        >
          <Server size={15} aria-hidden="true" />
          <span>{t('backup.providerWebdav', 'WebDAV')}</span>
        </button>
        <button
          type="button"
          role="tab"
          aria-selected={settings.provider === 's3'}
          className={`cloud-backup__provider-btn${settings.provider === 's3' ? ' is-active' : ''}`}
          onClick={() => setSettings((prev) => ({ ...prev, provider: 's3' }))}
        >
          <HardDrive size={15} aria-hidden="true" />
          <span>{t('backup.providerS3', 'S3 Compatible (R2 / OSS / MinIO)')}</span>
        </button>
      </div>

      {/* ── 各协议专属配置表单 ── */}
      {settings.provider === 'folder' && (
        <div className="cloud-backup__section">
          <label className="cloud-backup__label" htmlFor="cloud-backup-folder-input">
            {t('backup.folderPathLabel', 'Backup Sync Directory')}
          </label>
          <div className="cloud-backup__input-row">
            <input
              id="cloud-backup-folder-input"
              type="text"
              className="cloud-backup__input"
              value={settings.folderPath}
              onChange={(e) => setSettings((prev) => ({ ...prev, folderPath: e.target.value }))}
              placeholder={t('backup.folderPathPlaceholder', 'Select your OneDrive, Google Drive, or local sync folder')}
            />
            <Button variant="ghost" size="sm" onClick={() => void handleBrowseFolder()}>
              <FolderOpen size={14} />
              <span>{t('backup.browseBtn', 'Browse')}</span>
            </Button>
          </div>
          {detectedFolders.length > 0 && (
            <div className="cloud-backup__detected-row">
              <span className="cloud-backup__detected-label">{t('backup.detectedSyncDrives', 'Detected sync drives:')}</span>
              {detectedFolders.map((item) => (
                <button
                  key={item.path}
                  type="button"
                  className={`cloud-backup__drive-chip${settings.folderPath === item.path ? ' is-selected' : ''}`}
                  onClick={() => setSettings((prev) => ({ ...prev, folderPath: item.path }))}
                  title={item.path}
                >
                  {settings.folderPath === item.path && <Check size={12} aria-hidden="true" />}
                  <span>{item.label}</span>
                </button>
              ))}
            </div>
          )}
        </div>
      )}

      {settings.provider === 'webdav' && (
        <div className="cloud-backup__grid">
          <div className="cloud-backup__field cloud-backup__field--full">
            <label className="cloud-backup__label" htmlFor="cloud-backup-webdav-url">
              {t('backup.webdavUrlLabel', 'WebDAV Directory URL')}
            </label>
            <input
              id="cloud-backup-webdav-url"
              type="url"
              className="cloud-backup__input"
              value={settings.webdavUrl}
              onChange={(e) => setSettings((prev) => ({ ...prev, webdavUrl: e.target.value }))}
              placeholder="https://dav.jianguoyun.com/dav/Tools3000/"
            />
          </div>
          <div className="cloud-backup__field">
            <label className="cloud-backup__label" htmlFor="cloud-backup-webdav-user">
              {t('backup.webdavUserLabel', 'Username')}
            </label>
            <input
              id="cloud-backup-webdav-user"
              type="text"
              className="cloud-backup__input"
              value={settings.webdavUsername}
              onChange={(e) => setSettings((prev) => ({ ...prev, webdavUsername: e.target.value }))}
              placeholder="name@example.com"
            />
          </div>
          <div className="cloud-backup__field">
            <label className="cloud-backup__label" htmlFor="cloud-backup-webdav-pass">
              {t('backup.webdavPassLabel', 'App Password')}
            </label>
            <input
              id="cloud-backup-webdav-pass"
              type="password"
              className="cloud-backup__input"
              value={webdavPassword}
              onChange={(e) => setWebdavPassword(e.target.value)}
              placeholder={
                settings.hasWebdavPassword
                  ? t('backup.secretSavedPlaceholder', 'Encrypted & saved (leave blank to keep)')
                  : t('backup.enterPasswordPlaceholder', 'Enter WebDAV app password')
              }
            />
          </div>
        </div>
      )}

      {settings.provider === 's3' && (
        <div className="cloud-backup__grid">
          <div className="cloud-backup__field cloud-backup__field--full">
            <label className="cloud-backup__label" htmlFor="cloud-backup-s3-endpoint">
              {t('backup.s3EndpointLabel', 'Endpoint URL')}
            </label>
            <input
              id="cloud-backup-s3-endpoint"
              type="url"
              className="cloud-backup__input"
              value={settings.s3Endpoint}
              onChange={(e) => setSettings((prev) => ({ ...prev, s3Endpoint: e.target.value }))}
              placeholder="https://<account-id>.r2.cloudflarestorage.com"
            />
          </div>
          <div className="cloud-backup__field">
            <label className="cloud-backup__label" htmlFor="cloud-backup-s3-bucket">
              {t('backup.s3BucketLabel', 'Bucket Name')}
            </label>
            <input
              id="cloud-backup-s3-bucket"
              type="text"
              className="cloud-backup__input"
              value={settings.s3Bucket}
              onChange={(e) => setSettings((prev) => ({ ...prev, s3Bucket: e.target.value }))}
              placeholder="my-tools3000-backups"
            />
          </div>
          <div className="cloud-backup__field">
            <label className="cloud-backup__label" htmlFor="cloud-backup-s3-region">
              {t('backup.s3RegionLabel', 'Region')}
            </label>
            <input
              id="cloud-backup-s3-region"
              type="text"
              className="cloud-backup__input"
              value={settings.s3Region}
              onChange={(e) => setSettings((prev) => ({ ...prev, s3Region: e.target.value }))}
              placeholder="auto"
            />
          </div>
          <div className="cloud-backup__field">
            <label className="cloud-backup__label" htmlFor="cloud-backup-s3-ak">
              {t('backup.s3AccessKeyLabel', 'Access Key ID')}
            </label>
            <input
              id="cloud-backup-s3-ak"
              type="text"
              className="cloud-backup__input"
              value={settings.s3AccessKeyId}
              onChange={(e) => setSettings((prev) => ({ ...prev, s3AccessKeyId: e.target.value }))}
            />
          </div>
          <div className="cloud-backup__field">
            <label className="cloud-backup__label" htmlFor="cloud-backup-s3-sk">
              {t('backup.s3SecretKeyLabel', 'Secret Access Key')}
            </label>
            <input
              id="cloud-backup-s3-sk"
              type="password"
              className="cloud-backup__input"
              value={s3SecretKey}
              onChange={(e) => setS3SecretKey(e.target.value)}
              placeholder={
                settings.hasS3SecretKey
                  ? t('backup.secretSavedPlaceholder', 'Encrypted & saved (leave blank to keep)')
                  : t('backup.enterSecretPlaceholder', 'Enter S3 secret access key')
              }
            />
          </div>
          <div className="cloud-backup__field">
            <label className="cloud-backup__label" htmlFor="cloud-backup-s3-prefix">
              {t('backup.s3PrefixLabel', 'Object Prefix')}
            </label>
            <input
              id="cloud-backup-s3-prefix"
              type="text"
              className="cloud-backup__input"
              value={settings.s3Prefix}
              onChange={(e) => setSettings((prev) => ({ ...prev, s3Prefix: e.target.value }))}
              placeholder="tools3000-backups/"
            />
          </div>
          <div className="cloud-backup__field cloud-backup__field--toggle">
            <Toggle
              id="cloud-backup-s3-pathstyle"
              label={t('backup.s3PathStyleLabel', 'Path-Style URL')}
              description={t('backup.s3PathStyleDesc', 'Recommended for MinIO, R2, and self-hosted S3 servers')}
              checked={settings.s3ForcePathStyle}
              onChange={(v) => setSettings((prev) => ({ ...prev, s3ForcePathStyle: v }))}
            />
          </div>
        </div>
      )}

      {/* ── 安全加密与自动策略 ── */}
      <div className="cloud-backup__options">
        <Toggle
          id="cloud-backup-auto-exit"
          label={t('backup.autoOnExitLabel', 'Auto-backup on exit')}
          description={t('backup.autoOnExitDesc', 'Silently upload a fresh snapshot when Tools3000 exits normally')}
          checked={settings.enabled && settings.autoBackupOnExit}
          onChange={(v) => setSettings((prev) => ({ ...prev, enabled: v ? true : prev.enabled, autoBackupOnExit: v }))}
        />

        <SettingRow
          label={t('backup.keepCountLabel', 'Snapshot retention count')}
          description={t('backup.keepCountDesc', 'Automatically prune older snapshots beyond this count')}
        >
          <input
            type="number"
            min={1}
            max={100}
            className="cloud-backup__number-input"
            aria-label={t('backup.keepCountLabel', 'Snapshot retention count')}
            value={settings.keepCount}
            onChange={(e) =>
              setSettings((prev) => ({
                ...prev,
                keepCount: Math.max(1, Math.min(100, Number(e.target.value) || 10)),
              }))
            }
          />
        </SettingRow>

        <Toggle
          id="cloud-backup-encrypt"
          label={t('backup.encryptionLabel', 'End-to-end encryption (AES-256-GCM)')}
          description={t('backup.encryptionDesc', 'Encrypt snapshots locally before upload so cloud providers cannot read your settings')}
          checked={settings.encryptionEnabled}
          onChange={(v) => setSettings((prev) => ({ ...prev, encryptionEnabled: v }))}
        />

        {settings.encryptionEnabled && (
          <div className="cloud-backup__section">
            <label className="cloud-backup__label" htmlFor="cloud-backup-passphrase">
              {t('backup.passphraseLabel', 'Encryption Passphrase')}
            </label>
            <input
              id="cloud-backup-passphrase"
              type="password"
              className="cloud-backup__input"
              value={encryptionPassphrase}
              onChange={(e) => setEncryptionPassphrase(e.target.value)}
              placeholder={
                settings.hasEncryptionPassphrase
                  ? t('backup.secretSavedPlaceholder', 'Encrypted & saved (leave blank to keep)')
                  : t('backup.passphrasePlaceholder', 'Enter a memorable passphrase for recovery')
              }
            />
          </div>
        )}
      </div>

      {/* ── 操作栏 ── */}
      <div className="cloud-backup__actions">
        <Button variant="secondary" size="sm" onClick={() => void handleSave(false)} disabled={saving}>
          <Save size={14} />
          <span>{t('backup.saveBtn', 'Save Settings')}</span>
        </Button>
        <Button variant="ghost" size="sm" onClick={() => void handleTestConnection()} disabled={testing}>
          <RefreshCw size={14} className={testing ? 'cloud-backup__spin' : undefined} />
          <span>{t('backup.testBtn', 'Test Connection')}</span>
        </Button>
        <Button variant="primary" size="sm" onClick={() => void handleBackupNow()} disabled={backingUp || saving}>
          <Upload size={14} />
          <span>{backingUp ? t('backup.backingUp', 'Backing up...') : t('backup.backupNowBtn', 'Backup Now')}</span>
        </Button>
        <Button variant="ghost" size="sm" onClick={() => void handleListSnapshots(true)} disabled={loadingSnapshots}>
          <Download size={14} />
          <span>{t('backup.listBtn', 'Cloud Snapshots')}</span>
        </Button>
      </div>

      {/* ── 历史快照列表与一键恢复 ── */}
      {snapshotsLoaded && (
        <div className="cloud-backup__snapshots">
          {snapshots.length === 0 ? (
            <div className="cloud-backup__empty">{t('backup.noSnapshots', 'No backup snapshots found yet')}</div>
          ) : (
            snapshots.map((item) => (
              <div key={item.id} className="cloud-backup__snapshot-row">
                <div className="cloud-backup__snapshot-info">
                  <CodeBadge>{item.fileName}</CodeBadge>
                  {item.encrypted && (
                    <span className="cloud-backup__enc-pill">
                      <Lock size={12} aria-hidden="true" />
                      <span>AES-256</span>
                    </span>
                  )}
                  <span className="cloud-backup__snapshot-meta">
                    {formatSnapshotBytes(item.sizeBytes)}
                    {item.modifiedAtIso ? ` · ${item.modifiedAtIso}` : ''}
                  </span>
                </div>
                <Button
                  variant="secondary"
                  size="sm"
                  onClick={() => {
                    setRestoreTarget(item);
                    setRestorePassphrase('');
                  }}
                >
                  <Download size={13} />
                  <span>{t('backup.restoreBtn', 'Restore')}</span>
                </Button>
              </div>
            ))
          )}
        </div>
      )}

      <ConfirmModal
        open={Boolean(restoreTarget)}
        title={t('backup.restoreConfirmTitle', 'Restore Configuration Snapshot')}
        description={
          restoreTarget
            ? t('backup.restoreConfirmDesc', {
                file: restoreTarget.fileName,
                defaultValue: `Restore settings from ${restoreTarget.fileName}? Current local config will be backed up automatically before overwriting.`,
              })
            : ''
        }
        confirmText={restoring ? t('backup.restoring', 'Restoring...') : t('backup.restoreBtn', 'Restore')}
        onConfirm={() => void handleConfirmRestore()}
        onClose={() => {
          if (!restoring) {
            setRestoreTarget(null);
            setRestorePassphrase('');
          }
        }}
      />
    </Card>
  );
};

import { useCallback, useEffect, useState, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  LockOpen,
  ShieldCheck,
  FileSearch,
  FolderOpen,
  File as FileIcon,
  Play,
} from 'lucide-react';
import { Button, Card, CodeBadge, SettingGroup, Toggle } from '../components/UIKit';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import './FileLocksmithPage.css';

interface ShellMenuConfig {
  enabled: boolean;
  shiftOnly: boolean;
}

export const FileLocksmithPage: FC = () => {
  const { t } = useTranslation();
  const [config, setConfig] = useState<ShellMenuConfig>({ enabled: false, shiftOnly: true });
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [targetPath, setTargetPath] = useState('');
  const [browsing, setBrowsing] = useState<'file' | 'folder' | null>(null);
  const [inspecting, setInspecting] = useState(false);

  const loadConfig = useCallback(async () => {
    try {
      const res = await bridgeRequest<{ enabled?: boolean; shiftOnly?: boolean }>(
        'system.getShellMenuConfig',
        undefined,
        { silent: true },
      );
      if (res && typeof res.enabled === 'boolean') {
        setConfig({
          enabled: res.enabled,
          shiftOnly: res.shiftOnly ?? true,
        });
      }
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void loadConfig();
  }, [loadConfig]);

  useBridgeEvent('fileLocksmith:stateChanged', (raw: unknown) => {
    const payload = raw && typeof raw === 'object'
      ? (raw as { enabled?: boolean; shiftOnly?: boolean })
      : null;
    if (payload && typeof payload.enabled === 'boolean') {
      setConfig((prev) => ({
        enabled: payload.enabled as boolean,
        shiftOnly: typeof payload.shiftOnly === 'boolean' ? payload.shiftOnly : prev.shiftOnly,
      }));
    }
  });

  useBridgeEvent('plugins:changed', (raw: unknown) => {
    const payload = raw && typeof raw === 'object'
      ? (raw as { id?: string; enabled?: boolean })
      : null;
    if (payload?.id === 'file_locksmith' && typeof payload.enabled === 'boolean') {
      setConfig((prev) => ({ ...prev, enabled: payload.enabled as boolean }));
    }
  });

  const applyConfig = useCallback(async (next: ShellMenuConfig) => {
    const prev = config;
    setConfig(next);
    setSaving(true);
    try {
      const res = await bridgeRequest<{ success?: boolean; enabled?: boolean; shiftOnly?: boolean }>(
        'system.updateShellMenuConfig',
        next as unknown as Record<string, unknown>,
        { silent: true },
      );
      if (!res?.success) {
        setConfig(prev);
        toast.error(t('general.toastSaveFailed'));
        return;
      }
      toast.success(t('general.toastSaved'));
    } catch {
      setConfig(prev);
      toast.error(t('general.toastSaveFailed'));
    } finally {
      setSaving(false);
    }
  }, [config, t]);

  const handleBrowse = useCallback(async (folder: boolean) => {
    if (browsing) return;
    setBrowsing(folder ? 'folder' : 'file');
    try {
      const res = await bridgeRequest<{ success?: boolean; path?: string }>(
        'system.browseFileLocksmithTarget',
        { folder },
        { silent: true },
      );
      if (res?.success && res.path) {
        setTargetPath(res.path);
      }
    } finally {
      setBrowsing(null);
    }
  }, [browsing]);

  const handleInspect = useCallback(async () => {
    const clean = targetPath.trim();
    if (!clean || inspecting) return;
    setInspecting(true);
    try {
      const res = await bridgeRequest<{ success?: boolean }>(
        'system.openFileLocksmith',
        { path: clean },
        { silent: true },
      );
      if (!res?.success) {
        toast.error(t('fileLocksmith.inspectFailed', 'Failed to launch File Locksmith window'));
      }
    } finally {
      setInspecting(false);
    }
  }, [inspecting, t, targetPath]);

  if (loading) {
    return <div className="file-locksmith-page__loading">{t('common.loading')}</div>;
  }

  return (
    <div className="file-locksmith-page">
      <Card className="file-locksmith-hero">
        <div className="file-locksmith-hero__left">
          <span className="file-locksmith-hero__icon" aria-hidden="true">
            <LockOpen size={20} strokeWidth={2} />
          </span>
          <div className="file-locksmith-hero__meta">
            <div className="file-locksmith-hero__title-row">
              <h2>{t('fileLocksmith.heroTitle', 'File Locksmith (Zero-Resident)')}</h2>
              <span className={`file-locksmith-status ${config.enabled ? 'is-active' : ''}`}>
                <span className="file-locksmith-status__dot" aria-hidden="true" />
                <span>
                  {config.enabled
                    ? t('fileLocksmith.statusRegistered', 'Context Menu Active')
                    : t('fileLocksmith.statusInactive', 'Context Menu Disabled')}
                </span>
              </span>
            </div>
            <p className="file-locksmith-hero__desc">
              {t('fileLocksmith.heroDesc', 'Pure HKCU registry integration with zero Explorer DLL injection and zero background RAM usage. Launches a lightweight native Direct2D window only when needed.')}
            </p>
            <div className="file-locksmith-hero__badges">
              <CodeBadge>HKCU Only</CodeBadge>
              <CodeBadge variant="muted">0 MB Idle</CodeBadge>
              <CodeBadge variant="muted">Direct2D Native</CodeBadge>
            </div>
          </div>
        </div>
      </Card>

      <SettingGroup
        title={t('fileLocksmith.contextMenuGroup', 'Explorer Right-Click Integration')}
        icon={<ShieldCheck size={18} strokeWidth={2.2} />}
      >
        <Card>
          <Toggle
            id="fileLocksmithEnabled"
            label={t('general.shellMenuFileLocksmith', 'Explorer Context Menu: Inspect File Locks')}
            description={t('general.shellMenuFileLocksmithDesc', 'Add a zero-resident File Locksmith entry to File Explorer right-click menu to inspect and unlock busy files or folders')}
            checked={config.enabled}
            disabled={saving}
            onChange={(enabled) => void applyConfig({ ...config, enabled })}
          />
          <Toggle
            id="fileLocksmithShiftOnly"
            label={t('general.shellMenuFileLocksmithShiftOnly', 'Show Only on Shift + Right-Click')}
            description={t('general.shellMenuFileLocksmithShiftOnlyDesc', 'Keep the standard right-click menu clean; hold Shift while right-clicking (1-step direct menu on Windows 11) to reveal it')}
            checked={config.shiftOnly}
            disabled={saving || !config.enabled}
            onChange={(shiftOnly) => void applyConfig({ ...config, shiftOnly })}
          />
        </Card>
      </SettingGroup>

      <SettingGroup
        title={t('fileLocksmith.instantInspectGroup', 'Quick Inspect & Unlock')}
        icon={<FileSearch size={18} strokeWidth={2.2} />}
      >
        <Card>
          <div className="file-locksmith-inspect">
            <p className="file-locksmith-inspect__desc">
              {t('fileLocksmith.instantInspectDesc', 'Pick any locked file or folder directly here to inspect holding processes and safely release locks without leaving Tools3000.')}
            </p>
            <div className="file-locksmith-inspect__row">
              <input
                type="text"
                className="file-locksmith-inspect__input"
                value={targetPath}
                onChange={(e) => setTargetPath(e.target.value)}
                placeholder={t('fileLocksmith.pathPlaceholder', 'Enter or browse a file/folder path...')}
                aria-label={t('fileLocksmith.pathPlaceholder', 'Enter or browse a file/folder path...')}
              />
              <Button
                variant="secondary"
                size="sm"
                disabled={browsing !== null}
                onClick={() => void handleBrowse(false)}
              >
                <FileIcon size={14} aria-hidden="true" />
                <span>{t('fileLocksmith.pickFile', 'Pick File')}</span>
              </Button>
              <Button
                variant="secondary"
                size="sm"
                disabled={browsing !== null}
                onClick={() => void handleBrowse(true)}
              >
                <FolderOpen size={14} aria-hidden="true" />
                <span>{t('fileLocksmith.pickFolder', 'Pick Folder')}</span>
              </Button>
              <Button
                variant="primary"
                size="sm"
                disabled={!targetPath.trim() || inspecting}
                onClick={() => void handleInspect()}
              >
                <Play size={14} aria-hidden="true" />
                <span>{t('fileLocksmith.inspectNow', 'Inspect Locks')}</span>
              </Button>
            </div>
          </div>
        </Card>
      </SettingGroup>
    </div>
  );
};

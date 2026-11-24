/* ─────────────────────────────────────────────────────────────────────────────
 * PicasaViewerPage.tsx — Rust 复刻经典 Picasa 轻量图片查看器设置中心
 * ───────────────────────────────────────────────────────────────────────────── */

import { useCallback, useEffect, useMemo, useState, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import {
  AlertCircle,
  CheckCircle2,
  ExternalLink,
  FolderOpen,
  Image as ImageIcon,
  Info,
  Keyboard,
  Layers,
  Loader2,
  RefreshCw,
  SlidersHorizontal,
  Unlink,
} from 'lucide-react';
import { toast } from 'sonner';
import { Button, Card, CodeBadge, DisabledCascade, PageSkeleton, SettingGroup, Toggle } from '../components/UIKit';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import './PicasaViewerPage.css';

interface PicasaViewerStatus {
  enabled: boolean;
  installed: boolean;
  exePath: string;
  registeredExtensions: string[];
  registeredCount: number;
  effectiveDefaultExtensions?: string[];
  effectiveDefaultCount?: number;
  userChoiceLockedCount?: number;
  supportedExtensions: string[];
  supportedCount: number;
  allAssociated: boolean;
}

interface ActionResponse {
  success: boolean;
  error?: string;
  status?: PicasaViewerStatus;
}

interface FormatGroupDef {
  id: string;
  labelKey: string;
  extensions: readonly string[];
}

const FORMAT_GROUPS: readonly FormatGroupDef[] = [
  {
    id: 'common',
    labelKey: 'picasaViewer.groupCommon',
    extensions: ['jpg', 'jpeg', 'jpe', 'jfif', 'pjpeg', 'png', 'apng', 'webp', 'gif', 'bmp', 'dib', 'rle', 'ico', 'cur', 'tif', 'tiff'],
  },
  {
    id: 'modern',
    labelKey: 'picasaViewer.groupModern',
    extensions: ['svg', 'svgz', 'avif', 'heic', 'heif', 'hif', 'wdp', 'hdp', 'jxr'],
  },
  {
    id: 'graphics',
    labelKey: 'picasaViewer.groupGraphics',
    extensions: ['tga', 'qoi', 'ppm', 'pgm', 'pbm', 'dds'],
  },
  {
    id: 'raw',
    labelKey: 'picasaViewer.groupRaw',
    extensions: ['dng', 'cr2', 'cr3', 'nef', 'arw', 'orf', 'rw2', 'pef', 'raf', 'srw'],
  },
];

interface ShortcutDef {
  id: string;
  keys?: string;
  keysKey?: string;
  labelKey: string;
}

// 与 src/viewer/src/app.rs 真实键盘绑定 100% 对齐
const SHORTCUTS: readonly ShortcutDef[] = [
  { id: 'wheel', keysKey: 'picasaViewer.shortcutKeysWheel', labelKey: 'picasaViewer.shortcutWheel' },
  { id: 'doubleClick', keysKey: 'picasaViewer.shortcutKeysActual', labelKey: 'picasaViewer.shortcutActual' },
  { id: 'navigate', keysKey: 'picasaViewer.shortcutKeysNavigate', labelKey: 'picasaViewer.shortcutNavigate' },
  { id: 'slideshow', keys: 'F5 / Esc', labelKey: 'picasaViewer.shortcutSlideshow' },
  { id: 'rotate', keys: 'Q / E · Ctrl+Shift+R / Ctrl+R', labelKey: 'picasaViewer.shortcutRotate' },
  { id: 'close', keys: 'Esc', labelKey: 'picasaViewer.shortcutClose' },
];

type BusyAction = 'toggle' | 'associate' | 'unassociate' | 'config' | 'defaultApps' | 'launch' | 'refresh' | null;

export const PicasaViewerPage: FC = () => {
  const { t } = useTranslation();
  const [status, setStatus] = useState<PicasaViewerStatus | null>(null);
  const [loading, setLoading] = useState(true);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [busyAction, setBusyAction] = useState<BusyAction>(null);

  const loadStatus = useCallback(async (silent = true) => {
    if (!silent) {
      setBusyAction('refresh');
    }
    try {
      const res = await bridgeRequest<PicasaViewerStatus>('picasaViewer.getStatus');
      if (res && typeof res === 'object') {
        setStatus(res);
        setLoadError(null);
      }
    } catch (error) {
      const message = String(error);
      setLoadError(message);
      if (!silent) {
        toast.error(t('picasaViewer.loadFailed'), { description: message });
      }
    } finally {
      setLoading(false);
      if (!silent) {
        setBusyAction(null);
      }
    }
  }, [t]);

  useEffect(() => {
    let cancelled = false;
    void bridgeRequest<PicasaViewerStatus>('picasaViewer.getStatus')
      .then((res) => {
        if (!cancelled && res && typeof res === 'object') {
          setStatus(res);
          setLoadError(null);
        }
      })
      .catch((error) => {
        if (!cancelled) {
          setLoadError(String(error));
        }
      })
      .finally(() => {
        if (!cancelled) {
          setLoading(false);
        }
      });

    const onFocus = () => {
      void loadStatus(true);
    };
    window.addEventListener('focus', onFocus);
    return () => {
      cancelled = true;
      window.removeEventListener('focus', onFocus);
    };
  }, [loadStatus]);

  useBridgeEvent('picasa_viewer:stateChanged', (data: unknown) => {
    if (data && typeof data === 'object' && 'supportedCount' in data) {
      setStatus(data as PicasaViewerStatus);
      setLoadError(null);
    } else {
      void loadStatus(true);
    }
  });

  const handleToggleEnabled = useCallback(async (nextEnabled: boolean) => {
    if (busyAction) return;
    setBusyAction('toggle');
    try {
      const res = await bridgeRequest<ActionResponse>(
        'picasaViewer.setEnabled',
        { enabled: nextEnabled },
        { silent: true },
      );
      if (res.status) {
        setStatus(res.status);
      }
      if (!res.success) {
        toast.error(t('picasaViewer.toggleFailed'), { description: res.error || '' });
        return;
      }
      if (nextEnabled) {
        toast.success(t('picasaViewer.enabledToast', { count: res.status?.registeredCount ?? 0 }));
      } else {
        toast.success(t('picasaViewer.disabledToast'));
      }
    } catch (error) {
      toast.error(t('picasaViewer.toggleFailed'), { description: String(error) });
    } finally {
      setBusyAction(null);
    }
  }, [busyAction, t]);

  const handleAssociateAll = useCallback(async () => {
    if (busyAction) return;
    setBusyAction('associate');
    try {
      const res = await bridgeRequest<ActionResponse>('picasaViewer.associateAll', {}, { silent: true });
      if (res.status) {
        setStatus(res.status);
      }
      if (!res.success) {
        toast.error(t('picasaViewer.associateFailed'), { description: res.error || '' });
        return;
      }
      toast.success(t('picasaViewer.associateSuccess', { count: res.status?.registeredCount ?? 0 }));
    } catch (error) {
      toast.error(t('picasaViewer.associateFailed'), { description: String(error) });
    } finally {
      setBusyAction(null);
    }
  }, [busyAction, t]);

  const handleUnassociateAll = useCallback(async () => {
    if (busyAction) return;
    setBusyAction('unassociate');
    try {
      const res = await bridgeRequest<ActionResponse>('picasaViewer.unassociateAll', {}, { silent: true });
      if (res.status) {
        setStatus(res.status);
      }
      if (!res.success) {
        toast.error(t('picasaViewer.unassociateFailed'), { description: res.error || '' });
        return;
      }
      toast.success(t('picasaViewer.unassociateSuccess'));
    } catch (error) {
      toast.error(t('picasaViewer.unassociateFailed'), { description: String(error) });
    } finally {
      setBusyAction(null);
    }
  }, [busyAction, t]);

  const handleOpenFormatConfig = useCallback(async () => {
    if (busyAction) return;
    setBusyAction('config');
    try {
      const res = await bridgeRequest<ActionResponse>('picasaViewer.openFormatConfig', {}, { silent: true });
      if (!res.success) {
        toast.error(t('picasaViewer.launchFailed'), { description: res.error || '' });
      }
    } catch (error) {
      toast.error(t('picasaViewer.launchFailed'), { description: String(error) });
    } finally {
      setBusyAction(null);
    }
  }, [busyAction, t]);

  const handleOpenDefaultApps = useCallback(async () => {
    if (busyAction) return;
    setBusyAction('defaultApps');
    try {
      const res = await bridgeRequest<ActionResponse>('picasaViewer.openDefaultApps', {}, { silent: true });
      if (!res.success) {
        toast.error(t('picasaViewer.openDefaultAppsFailed'), { description: res.error || '' });
      }
    } catch (error) {
      toast.error(t('picasaViewer.openDefaultAppsFailed'), { description: String(error) });
    } finally {
      setBusyAction(null);
    }
  }, [busyAction, t]);

  const handleLaunchViewer = useCallback(async () => {
    if (busyAction) return;
    setBusyAction('launch');
    try {
      const res = await bridgeRequest<ActionResponse>('picasaViewer.launchViewer', {}, { silent: true });
      if (!res.success) {
        toast.error(t('picasaViewer.launchFailed'), { description: res.error || '' });
      }
    } catch (error) {
      toast.error(t('picasaViewer.launchFailed'), { description: String(error) });
    } finally {
      setBusyAction(null);
    }
  }, [busyAction, t]);

  const registeredSet = useMemo(
    () => new Set((status?.registeredExtensions ?? []).map((ext) => ext.toLowerCase())),
    [status],
  );

  if (loading) {
    return <PageSkeleton cards={3} rowsPerCard={3} />;
  }

  if (!status) {
    return (
      <div className="picasa-viewer-page">
        <Card className="picasa-viewer-error-card">
          <div className="picasa-viewer-error-state" role="alert">
            <AlertCircle size={22} className="picasa-viewer-error-state__icon" aria-hidden="true" />
            <div className="picasa-viewer-error-state__body">
              <div className="picasa-viewer-error-state__title">{t('picasaViewer.loadErrorTitle')}</div>
              {loadError && <p className="picasa-viewer-error-state__desc">{loadError}</p>}
            </div>
            <Button
              variant="primary"
              size="sm"
              disabled={busyAction !== null}
              onClick={() => { void loadStatus(false); }}
            >
              <RefreshCw
                size={14}
                className={busyAction === 'refresh' ? 'picasa-viewer-spin' : ''}
                aria-hidden="true"
              />
              <span>{t('picasaViewer.loadErrorRetry')}</span>
            </Button>
          </div>
        </Card>
      </div>
    );
  }

  const isRunning = status.installed && status.enabled;
  const lockedCount = status.userChoiceLockedCount ?? 0;
  const effectiveDefaultCount = status.effectiveDefaultCount ?? 0;

  return (
    <div className="picasa-viewer-page">
      {/* ── 1. 顶部核心开关与状态总览 ───────────────────────────────────────── */}
      <Card className="picasa-viewer-hero-card">
        <div className="picasa-viewer-hero">
          <div className="picasa-viewer-hero__info">
            <div className="picasa-viewer-hero__title-row">
              <ImageIcon size={20} className="picasa-viewer-hero__icon" aria-hidden="true" />
              <h2 className="picasa-viewer-hero__title">{t('picasaViewer.heroTitle')}</h2>
              <span className="picasa-viewer-badge">{t('picasaViewer.rustRecreationBadge')}</span>
            </div>
            <p className="picasa-viewer-hero__desc">{t('picasaViewer.heroDesc')}</p>
          </div>

          <div className="picasa-viewer-hero__switch">
            <Toggle
              id="picasa-viewer-master-toggle"
              checked={status.enabled}
              disabled={!status.installed || busyAction !== null}
              onChange={(val) => { void handleToggleEnabled(val); }}
            />
          </div>
        </div>

        <div className="picasa-viewer-status-bar">
          <div className="picasa-viewer-status-indicator">
            <span
              className={`picasa-viewer-dot ${
                !status.installed
                  ? 'picasa-viewer-dot--error'
                  : isRunning
                    ? 'picasa-viewer-dot--active'
                    : 'picasa-viewer-dot--idle'
              }`}
              aria-hidden="true"
            />
            <span className={`picasa-viewer-status-text ${isRunning ? 'picasa-viewer-status-text--active' : ''}`}>
              {!status.installed
                ? t('picasaViewer.statusMissingBinary')
                : isRunning
                  ? t('picasaViewer.statusEnabled')
                  : t('picasaViewer.statusDisabled')}
            </span>
          </div>

          <div className="picasa-viewer-status-right">
            <span className="picasa-viewer-count-pill">
              <CheckCircle2 size={14} aria-hidden="true" />
              <span>
                {t('picasaViewer.associatedCount', {
                  registered: status.registeredCount,
                  total: status.supportedCount,
                })}
              </span>
            </span>
            <Button
              variant="secondary"
              size="sm"
              disabled={!status.installed || busyAction !== null}
              title={t('picasaViewer.tryOpenDesc')}
              onClick={() => { void handleLaunchViewer(); }}
            >
              {busyAction === 'launch'
                ? <Loader2 size={14} className="picasa-viewer-spin" aria-hidden="true" />
                : <FolderOpen size={14} aria-hidden="true" />}
              <span>{t('picasaViewer.tryOpenBtn')}</span>
            </Button>
            <Button
              variant="ghost"
              size="sm"
              disabled={busyAction !== null}
              onClick={() => { void loadStatus(false); }}
            >
              <RefreshCw
                size={14}
                className={busyAction === 'refresh' ? 'picasa-viewer-spin' : ''}
                aria-hidden="true"
              />
              <span>{t('picasaViewer.refreshStatus')}</span>
            </Button>
          </div>
        </div>
        {isRunning && lockedCount > 0 && (
          <div className="picasa-viewer-userchoice-hint" role="note">
            {t('picasaViewer.userChoiceHint', {
              effective: effectiveDefaultCount,
              locked: lockedCount,
            })}
          </div>
        )}
      </Card>

      {/* ── 2. 诚实边界说明卡片（Rust 复刻轻量看图器，非原版全功能套件） ──────── */}
      <div className="picasa-viewer-notice" role="note">
        <Info size={18} className="picasa-viewer-notice__icon" aria-hidden="true" />
        <div className="picasa-viewer-notice__body">
          <div className="picasa-viewer-notice__title">{t('picasaViewer.scopeNoticeTitle')}</div>
          <p className="picasa-viewer-notice__desc">{t('picasaViewer.scopeNoticeDesc')}</p>
        </div>
      </div>

      {/* ── 3 & 4. 文件格式关联与支持的图片格式矩阵（未启用时视觉降权） ──── */}
      <DisabledCascade disabled={!status.enabled} className="picasa-viewer-cascade">
        <SettingGroup title={t('picasaViewer.assocSectionTitle')}>
          <p className="picasa-viewer-section-desc">{t('picasaViewer.assocSectionDesc')}</p>
          <Card className="picasa-viewer-actions-card">
            <div className="picasa-viewer-action-row uikit-setting-row">
              <div className="picasa-viewer-action-row__text">
                <div className="picasa-viewer-action-row__title">{t('picasaViewer.associateAllTitle')}</div>
                <div className="picasa-viewer-action-row__desc">
                  {t('picasaViewer.associateAllDesc', { total: status.supportedCount })}
                </div>
              </div>
              <Button
                variant="primary"
                size="sm"
                disabled={!status.installed || busyAction !== null}
                onClick={() => { void handleAssociateAll(); }}
              >
                {busyAction === 'associate'
                  ? <Loader2 size={14} className="picasa-viewer-spin" aria-hidden="true" />
                  : <Layers size={14} aria-hidden="true" />}
                <span>{t('picasaViewer.associateAllBtn')}</span>
              </Button>
            </div>

            <div className="picasa-viewer-action-row uikit-setting-row">
              <div className="picasa-viewer-action-row__text">
                <div className="picasa-viewer-action-row__title">{t('picasaViewer.customFormatsTitle')}</div>
                <div className="picasa-viewer-action-row__desc">{t('picasaViewer.customFormatsDesc')}</div>
              </div>
              <Button
                variant="secondary"
                size="sm"
                disabled={!status.installed || busyAction !== null}
                onClick={() => { void handleOpenFormatConfig(); }}
              >
                {busyAction === 'config'
                  ? <Loader2 size={14} className="picasa-viewer-spin" aria-hidden="true" />
                  : <SlidersHorizontal size={14} aria-hidden="true" />}
                <span>{t('picasaViewer.customFormatsBtn')}</span>
              </Button>
            </div>

            <div className="picasa-viewer-action-row uikit-setting-row">
              <div className="picasa-viewer-action-row__text">
                <div className="picasa-viewer-action-row__title">{t('picasaViewer.defaultAppsTitle')}</div>
                <div className="picasa-viewer-action-row__desc">{t('picasaViewer.defaultAppsDesc')}</div>
              </div>
              <Button
                variant="secondary"
                size="sm"
                disabled={busyAction !== null}
                onClick={() => { void handleOpenDefaultApps(); }}
              >
                {busyAction === 'defaultApps'
                  ? <Loader2 size={14} className="picasa-viewer-spin" aria-hidden="true" />
                  : <ExternalLink size={14} aria-hidden="true" />}
                <span>{t('picasaViewer.defaultAppsBtn')}</span>
              </Button>
            </div>

            {status.registeredCount > 0 && (
              <div className="picasa-viewer-action-row uikit-setting-row">
                <div className="picasa-viewer-action-row__text">
                  <div className="picasa-viewer-action-row__title">{t('picasaViewer.unassociateAllTitle')}</div>
                  <div className="picasa-viewer-action-row__desc">{t('picasaViewer.unassociateAllDesc')}</div>
                </div>
                <Button
                  variant="ghost"
                  size="sm"
                  disabled={!status.installed || busyAction !== null}
                  onClick={() => { void handleUnassociateAll(); }}
                >
                  {busyAction === 'unassociate'
                    ? <Loader2 size={14} className="picasa-viewer-spin" aria-hidden="true" />
                    : <Unlink size={14} aria-hidden="true" />}
                  <span>{t('picasaViewer.unassociateAllBtn')}</span>
                </Button>
              </div>
            )}
          </Card>
        </SettingGroup>

        {/* ── 4. 支持的图片格式矩阵 ────────────────────────────────────────────── */}
        <SettingGroup title={t('picasaViewer.formatsSectionTitle')}>
          <p className="picasa-viewer-section-desc">{t('picasaViewer.formatsSectionDesc')}</p>
          <Card className="picasa-viewer-formats-card">
            <div className="picasa-viewer-formats-grid">
              {FORMAT_GROUPS.map((group) => (
                <div key={group.id} className="picasa-viewer-format-group">
                  <div className="picasa-viewer-format-group__label">
                    {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                    {t(group.labelKey as any)}
                  </div>
                  <div className="picasa-viewer-format-chips">
                    {group.extensions.map((ext) => {
                      const active = registeredSet.has(ext);
                      return (
                        <span
                          key={ext}
                          className={`picasa-viewer-ext-chip ${active ? 'picasa-viewer-ext-chip--active' : ''}`}
                        >
                          .{ext.toUpperCase()}
                        </span>
                      );
                    })}
                  </div>
                </div>
              ))}
            </div>
          </Card>
        </SettingGroup>
      </DisabledCascade>

      {/* ── 5. 常用看图快捷键速查 ────────────────────────────────────────────── */}
      <SettingGroup title={t('picasaViewer.shortcutsSectionTitle')}>
        <p className="picasa-viewer-section-desc">{t('picasaViewer.shortcutsSectionDesc')}</p>
        <Card className="picasa-viewer-shortcuts-card">
          <div className="picasa-viewer-shortcuts-grid">
            {SHORTCUTS.map((item) => (
              <div key={item.id} className="picasa-viewer-shortcut-item">
                <div className="picasa-viewer-shortcut-item__left">
                  <Keyboard size={14} className="picasa-viewer-shortcut-item__icon" aria-hidden="true" />
                  <span className="picasa-viewer-shortcut-item__label">
                    {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                    {t(item.labelKey as any)}
                  </span>
                </div>
                <CodeBadge>
                  {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
                  {item.keysKey ? t(item.keysKey as any) : item.keys}
                </CodeBadge>
              </div>
            ))}
          </div>
        </Card>
      </SettingGroup>
    </div>
  );
};

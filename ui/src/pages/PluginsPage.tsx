import { useCallback, useEffect, useMemo, useState, type FC } from 'react';
import {
  Camera,
  Cast,
  ChevronDown,
  FileSearch,
  FolderSymlink,
  Image as ImageIcon,
  Keyboard,
  Mouse,
  Puzzle,
  RotateCw,
  ShieldCheck,
  SlidersHorizontal,
  Sparkles,
} from 'lucide-react';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { Button, Toggle, CodeBadge } from '../components/UIKit';
import type { NavId } from '../components/Sidebar';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import './PluginsPage.css';

export interface PluginStatus {
  id: string;
  name: string;
  version: string;
  fileName: string;
  abiVersion: number;
  capabilities: string[];
  permissions: string[];
  executionModel?: 'trusted-native-in-process' | 'standalone-rust-process';
  enabled: boolean;
  active: boolean;
  restartRequired: boolean;
  state: 'running' | 'disabled' | 'pendingRestart' | 'failed' | 'unavailable';
  error?: string;
  isExtension?: boolean;
}

interface PluginsPageProps {
  initialPlugins?: PluginStatus[];
  onNavigate?: (nav: NavId) => void;
}

interface UpdateResult {
  success: boolean;
  restartRequired: boolean;
  error?: string;
}

const ICONS = {
  gesture: Mouse,
  capture: Camera,
  search: FileSearch,
  keycast: Keyboard,
  spotlight: Sparkles,
  dialogenhancer: FolderSymlink,
  dialog_enhancer: FolderSymlink,
  remote_boost: Cast,
  remote: Cast,
  picasa_viewer: ImageIcon,
  picasaviewer: ImageIcon,
} as const;

const PLUGIN_DISPLAY_ORDER: Record<string, number> = {
  search: 1,
  gesture: 2,
  capture: 3,
  dialogenhancer: 4,
  dialog_enhancer: 4,
  keycast: 5,
  spotlight: 6,
  remote_boost: 7,
  remote: 7,
  picasa_viewer: 8,
  picasaviewer: 8,
};

const PLUGIN_NAV_MAP: Record<string, NavId> = {
  search: 'search',
  gesture: 'gesture',
  capture: 'capture',
  dialogenhancer: 'dialog_enhancer',
  dialog_enhancer: 'dialog_enhancer',
  keycast: 'keycast',
  spotlight: 'spotlight',
  remote_boost: 'remote_boost',
  remote: 'remote_boost',
  picasa_viewer: 'picasa_viewer',
  picasaviewer: 'picasa_viewer',
};

export const PluginsPage: FC<PluginsPageProps> = ({ initialPlugins = [], onNavigate }) => {
  const { t } = useTranslation();
  const [plugins, setPlugins] = useState<PluginStatus[]>(initialPlugins);
  const [loading, setLoading] = useState(initialPlugins.length === 0);
  const [savingId, setSavingId] = useState<string | null>(null);
  const [isRestarting, setIsRestarting] = useState(false);

  const handleRestart = useCallback(async () => {
    if (isRestarting) return;
    setIsRestarting(true);
    toast.loading(t('plugins.restarting'));
    try {
      await bridgeRequest('app.restart');
    } catch {
      setIsRestarting(false);
      toast.error(t('plugins.restartFailed'));
    }
  }, [isRestarting, t]);

  const refresh = useCallback(async () => {
    try {
      const installedResult = await bridgeRequest<PluginStatus[]>('plugins.getAll');
      if (Array.isArray(installedResult)) {
        setPlugins(installedResult);
        window.dispatchEvent(new CustomEvent('tools3000:plugins-changed', { detail: installedResult }));
      }
    } catch (error) {
      toast.error(t('plugins.loadFailed'), { description: String(error) });
    } finally {
      setLoading(false);
    }
  }, [t]);

  useEffect(() => {
    const frame = requestAnimationFrame(() => { void refresh(); });
    const onFocus = () => { void refresh(); };
    window.addEventListener('focus', onFocus);
    return () => {
      cancelAnimationFrame(frame);
      window.removeEventListener('focus', onFocus);
    };
  }, [refresh]);

  const updatePluginState = useCallback((id: string, enabled: boolean) => {
    setPlugins((items) => {
      const aliases = (id === 'dialogenhancer' || id === 'dialog_enhancer')
        ? ['dialogenhancer', 'dialog_enhancer']
        : (id === 'remote_boost' || id === 'remote')
          ? ['remote_boost', 'remote']
          : (id === 'picasa_viewer' || id === 'picasaviewer')
            ? ['picasa_viewer', 'picasaviewer']
            : [id];
      const next = items.map((item) => {
        if (!aliases.includes(item.id)) return item;
        // 若二进制缺失处于 unavailable 状态，绝不被外部状态事件误覆写为 disabled 并解锁开关
        if (item.state === 'unavailable') {
          return { ...item, enabled: false, active: false };
        }
        return {
          ...item,
          enabled,
          active: enabled,
          state: (enabled ? 'running' : 'disabled') as PluginStatus['state'],
        };
      });
      window.dispatchEvent(new CustomEvent('tools3000:plugins-changed', { detail: next }));
      return next;
    });
  }, []);

  useBridgeEvent('plugins:changed', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { id?: string; enabled?: boolean }) : null;
    if (payload?.id && typeof payload.enabled === 'boolean') {
      updatePluginState(payload.id, payload.enabled);
    } else {
      void refresh();
    }
  });

  useBridgeEvent('dialog:configChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      updatePluginState('dialogenhancer', payload.enabled);
    }
  });

  useBridgeEvent('keycast:configChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      updatePluginState('keycast', payload.enabled);
    }
  });

  useBridgeEvent('spotlight:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      updatePluginState('spotlight', payload.enabled);
    }
  });

  useBridgeEvent('remote:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      updatePluginState('remote_boost', payload.enabled);
    }
  });

  useBridgeEvent('picasa_viewer:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      updatePluginState('picasa_viewer', payload.enabled);
    }
  });

  useBridgeEvent('gesture:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      updatePluginState('gesture', payload.enabled);
    }
  });

  useBridgeEvent('gesture.stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      updatePluginState('gesture', payload.enabled);
    }
  });

  const setEnabled = async (plugin: PluginStatus, enabled: boolean) => {
    setSavingId(plugin.id);
    setPlugins((items) => items.map((item) => item.id === plugin.id ? { ...item, enabled } : item));
    try {
      const result = await bridgeRequest<UpdateResult>('plugins.setEnabled', { id: plugin.id, enabled }, { silent: true });
      if (!result.success) throw new Error(result.error || t('plugins.saveFailed'));
      await refresh();
      toast.success(enabled ? t('plugins.enabledSaved') : t('plugins.disabledSaved'), {
        description: result.restartRequired ? t('plugins.restartHint') : undefined,
        action: result.restartRequired ? {
          label: t('plugins.restartNow'),
          onClick: () => { void handleRestart(); },
        } : undefined,
      });
    } catch (error) {
      setPlugins((items) => items.map((item) => item.id === plugin.id ? { ...item, enabled: plugin.enabled } : item));
      toast.error(t('plugins.saveFailed'), { description: String(error) });
    } finally {
      setSavingId(null);
    }
  };

  const sortedPlugins = useMemo(() => {
    return [...plugins].sort((a, b) => {
      const orderA = PLUGIN_DISPLAY_ORDER[a.id] ?? 99;
      const orderB = PLUGIN_DISPLAY_ORDER[b.id] ?? 99;
      return orderA - orderB;
    });
  }, [plugins]);

  if (loading) return <div className="plugins-page__loading">{t('common.loading')}</div>;

  const pending = plugins.some((plugin) => plugin.restartRequired);

  return (
    <div className="plugins-page">
      {/* 待重启提示条（支持一键热重启） */}
      {pending && (
        <div className="plugins-page__restart" role="status">
          <RotateCw size={20} className={isRestarting ? 'plugins-spin' : ''} aria-hidden="true" />
          <div className="plugins-page__restart-content">
            <strong>{t('plugins.restartTitle')}</strong>
            <span>{t('plugins.restartHint')}</span>
          </div>
          <Button
            variant="primary"
            size="sm"
            onClick={() => { void handleRestart(); }}
            disabled={isRestarting}
            className="plugins-restart-now-btn"
          >
            <RotateCw size={14} className={isRestarting ? 'plugins-spin' : ''} />
            {t('plugins.restartNow')}
          </Button>
        </div>
      )}

      {/* 核心已安装模块矩阵 */}
      <div className="plugins-page__grid">
        {sortedPlugins.map((plugin) => {
          const Icon = ICONS[plugin.id as keyof typeof ICONS] ?? Puzzle;
          const failed = plugin.state === 'failed';
          const isUnavailable = plugin.state === 'unavailable';
          const isRunning = plugin.active && !failed && !isUnavailable && !plugin.restartRequired;
          const statusTone = failed
            ? 'danger'
            : isUnavailable
              ? 'muted'
              : plugin.restartRequired
                ? 'warning'
                : plugin.active
                  ? 'success'
                  : 'muted';

          return (
            <article
              className={`plugin-card ${isRunning ? 'plugin-card--active' : ''} ${failed ? 'plugin-card--failed' : ''} ${isUnavailable ? 'plugin-card--unavailable' : ''}`}
              key={plugin.id}
            >
              <div className="plugin-card__header">
                <span className="plugin-card__icon" aria-hidden="true">
                  <Icon size={17} strokeWidth={1.85} />
                </span>
                <div className="plugin-card__identity">
                  <div className="plugin-card__title-row">
                    <h2>{t(`plugins.items.${plugin.id}.name`, { defaultValue: plugin.name })}</h2>
                  </div>
                  <div className="plugin-card__status-line">
                    <span className={`plugin-card__status-dot plugin-card__status-dot--${statusTone}`} aria-hidden="true" />
                    <span className={`plugin-card__status-text plugin-card__status-text--${statusTone}`}>
                      {t(`plugins.state.${plugin.state}`)}
                    </span>
                    <span className="plugin-card__status-sep" aria-hidden="true">·</span>
                    <span className="plugin-card__version-inline">v{plugin.version || '—'}</span>
                  </div>
                </div>
                <div className="plugin-card__switch">
                  <Toggle
                    id={`plugin-${plugin.id}`}
                    checked={plugin.enabled}
                    disabled={savingId === plugin.id || (failed && !plugin.enabled) || isUnavailable}
                    onChange={(value) => void setEnabled(plugin, value)}
                  />
                </div>
              </div>

              <p className="plugin-card__description">
                {t(`plugins.items.${plugin.id}.description`, { defaultValue: plugin.name })}
              </p>

              {plugin.error && <p className="plugin-card__error" role="alert">{plugin.error}</p>}

              <div className="plugin-card__footer">
                <details className="plugin-card__details">
                  <summary className="plugin-card__details-trigger">
                    <ShieldCheck size={13} strokeWidth={1.9} aria-hidden="true" />
                    <span>{t('plugins.permissions')}</span>
                    <ChevronDown size={13} strokeWidth={2} className="plugin-card__details-chevron" aria-hidden="true" />
                  </summary>
                  <div className="plugin-card__details-body">
                    <div className="plugin-card__meta-row">
                      <CodeBadge>{plugin.fileName}</CodeBadge>
                      <CodeBadge variant="muted">{t('plugins.abi', { version: plugin.abiVersion || '—' })}</CodeBadge>
                      <span className="plugin-card__runtime-note">
                        {plugin.active ? t('plugins.resourceActive') : t('plugins.resourceInactive')}
                      </span>
                    </div>
                    {(plugin.capabilities || []).length > 0 && (
                      <div className="plugin-card__meta-group" aria-label={t('plugins.capabilities')}>
                        {(plugin.capabilities || []).map((capability) => (
                          <CodeBadge key={capability}>{capability}</CodeBadge>
                        ))}
                      </div>
                    )}
                    {(plugin.permissions || []).length > 0 && (
                      <div className="plugin-card__meta-group">
                        {plugin.executionModel === 'trusted-native-in-process' && (
                          <span className="plugin-card__trust-note">{t('plugins.fullTrustWarning')}</span>
                        )}
                        <div className="plugin-card__perm-chips">
                          {plugin.permissions.map((permission) => <CodeBadge key={permission} variant="muted">{permission}</CodeBadge>)}
                        </div>
                      </div>
                    )}
                  </div>
                </details>

                {onNavigate && plugin.enabled && plugin.active && PLUGIN_NAV_MAP[plugin.id] && (
                  <Button
                    variant="ghost"
                    size="sm"
                    className="plugin-card__config-btn"
                    onClick={() => onNavigate(PLUGIN_NAV_MAP[plugin.id])}
                    title={t('plugins.configure')}
                  >
                    <SlidersHorizontal size={13} strokeWidth={1.9} />
                    <span>{t('plugins.configure')}</span>
                  </Button>
                )}
              </div>
            </article>
          );
        })}
      </div>

      {plugins.length === 0 && (
        <div className="plugins-page__empty">{t('plugins.empty')}</div>
      )}
    </div>
  );
};

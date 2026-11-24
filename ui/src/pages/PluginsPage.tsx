import { useCallback, useEffect, useMemo, useState, type FC } from 'react';
import {
  Camera,
  Cast,
  ChevronDown,
  ClipboardList,
  FileSearch,
  FolderSymlink,
  Image as ImageIcon,
  Keyboard,
  LockOpen,
  Mouse,
  Puzzle,
  RotateCw,
  Search,
  ShieldCheck,
  SlidersHorizontal,
  Sparkles,
  TextCursorInput,
  X,
} from 'lucide-react';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { Button, Toggle, CodeBadge } from '../components/UIKit';
import type { NavId } from '../components/Sidebar';
import {
  PLUGIN_DOMAIN_GROUPS,
  getBuiltinPluginAliases,
  getBuiltinPluginDisplayOrder,
  getBuiltinPluginPrimaryRoute,
  getBuiltinPluginSecondaryRoutes,
  normalizeBuiltinPluginId,
} from '../config/routes';
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

type PluginFilterMode = 'all' | 'active' | 'disabled';

const ICONS = {
  gesture: Mouse,
  capture: Camera,
  search: FileSearch,
  keycast: Keyboard,
  spotlight: Sparkles,
  dialogenhancer: FolderSymlink,
  dialog_enhancer: FolderSymlink,
  file_locksmith: LockOpen,
  filelocksmith: LockOpen,
  remote_boost: Cast,
  remote: Cast,
  picasa_viewer: ImageIcon,
  picasaviewer: ImageIcon,
  clipboard: ClipboardList,
  text_expander: TextCursorInput,
  textexpander: TextCursorInput,
} as const;

const ALL_BUILTIN_DOMAIN_IDS = new Set<string>(
  PLUGIN_DOMAIN_GROUPS.flatMap((group) => group.pluginIds.map((id) => normalizeBuiltinPluginId(id))),
);

export const PluginsPage: FC<PluginsPageProps> = ({ initialPlugins = [], onNavigate }) => {
  const { t } = useTranslation();
  const [plugins, setPlugins] = useState<PluginStatus[]>(initialPlugins);
  const [loading, setLoading] = useState(initialPlugins.length === 0);
  const [savingId, setSavingId] = useState<string | null>(null);
  const [isRestarting, setIsRestarting] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [filterMode, setFilterMode] = useState<PluginFilterMode>('all');

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
      const aliases = getBuiltinPluginAliases(id);
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

  useBridgeEvent('fileLocksmith:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      updatePluginState('file_locksmith', payload.enabled);
    }
  });

  useBridgeEvent('textExpander:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      updatePluginState('text_expander', payload.enabled);
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
    return plugins
      .map((plugin, originalIndex) => ({
        plugin,
        originalIndex,
        displayOrder: getBuiltinPluginDisplayOrder(plugin.id),
      }))
      .sort((a, b) => (a.displayOrder - b.displayOrder) || (a.originalIndex - b.originalIndex))
      .map((entry) => entry.plugin);
  }, [plugins]);

  const counts = useMemo(() => {
    const all = sortedPlugins.length;
    const active = sortedPlugins.filter((p) => p.enabled).length;
    return {
      all,
      active,
      disabled: all - active,
    };
  }, [sortedPlugins]);

  const filteredPlugins = useMemo(() => {
    const needle = searchQuery.trim().toLowerCase();
    return sortedPlugins.filter((plugin) => {
      if (filterMode === 'active' && !plugin.enabled) return false;
      if (filterMode === 'disabled' && plugin.enabled) return false;
      if (!needle) return true;

      const canonicalId = normalizeBuiltinPluginId(plugin.id);
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const displayName = String(t(`plugins.items.${canonicalId}.name` as any, { defaultValue: plugin.name })).toLowerCase();
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const displayDesc = String(t(`plugins.items.${canonicalId}.description` as any, { defaultValue: plugin.name })).toLowerCase();
      const secondaryNames = getBuiltinPluginSecondaryRoutes(plugin.id)
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        .map((route) => String(t(route.labelKey as any)))
        .join(' ')
        .toLowerCase();

      return (
        displayName.includes(needle) ||
        displayDesc.includes(needle) ||
        plugin.id.toLowerCase().includes(needle) ||
        plugin.name.toLowerCase().includes(needle) ||
        secondaryNames.includes(needle)
      );
    });
  }, [sortedPlugins, filterMode, searchQuery, t]);

  const groupedSections = useMemo(() => {
    const sections: Array<{ id: string; titleKey: string; items: PluginStatus[] }> = [];

    for (const group of PLUGIN_DOMAIN_GROUPS) {
      const groupSet = new Set(group.pluginIds.map((id) => normalizeBuiltinPluginId(id)));
      const items = filteredPlugins.filter((p) => groupSet.has(normalizeBuiltinPluginId(p.id)));
      if (items.length > 0) {
        sections.push({
          id: group.id,
          titleKey: group.titleKey,
          items,
        });
      }
    }

    const extensionItems = filteredPlugins.filter(
      (p) => !ALL_BUILTIN_DOMAIN_IDS.has(normalizeBuiltinPluginId(p.id)),
    );
    if (extensionItems.length > 0) {
      sections.push({
        id: 'extensions',
        titleKey: 'plugins.domainExtensions',
        items: extensionItems,
      });
    }

    return sections;
  }, [filteredPlugins]);

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

      {/* 顶部轻量搜索与三态分段筛选工具栏 */}
      {plugins.length > 0 && (
        <div className="plugins-page__toolbar">
          <div className="plugins-page__search-box">
            <Search size={15} className="plugins-page__search-icon" aria-hidden="true" />
            <input
              type="search"
              className="plugins-page__search-input"
              value={searchQuery}
              onChange={(e) => setSearchQuery(e.target.value)}
              placeholder={t('plugins.searchPlaceholder')}
              aria-label={t('plugins.searchPlaceholder')}
            />
            {searchQuery && (
              <button
                type="button"
                className="plugins-page__search-clear"
                onClick={() => setSearchQuery('')}
                aria-label={t('common.close')}
              >
                <X size={13} aria-hidden="true" />
              </button>
            )}
          </div>

          <div className="plugins-page__filters" role="tablist" aria-label={t('plugins.title')}>
            {(
              [
                { id: 'all', label: t('plugins.filterAll', { count: counts.all }) },
                { id: 'active', label: t('plugins.filterActive', { count: counts.active }) },
                { id: 'disabled', label: t('plugins.filterDisabled', { count: counts.disabled }) },
              ] as const
            ).map((tab) => {
              const isSelected = filterMode === tab.id;
              return (
                <button
                  key={tab.id}
                  type="button"
                  role="tab"
                  aria-selected={isSelected}
                  className={`plugins-page__filter-pill ${isSelected ? 'is-active' : ''}`}
                  onClick={() => setFilterMode(tab.id)}
                >
                  {tab.label}
                </button>
              );
            })}
          </div>
        </div>
      )}

      {/* 四大正交业务域分组单列列表 */}
      <div className="plugins-page__domains">
        {groupedSections.map((section) => (
          <section key={section.id} className="plugins-domain-group">
            <div className="plugins-domain-group__header">
              {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
              <span className="plugins-domain-group__title">{t(section.titleKey as any)}</span>
            </div>

            <div className="plugins-domain-card">
              {section.items.map((plugin) => {
                const canonicalId = normalizeBuiltinPluginId(plugin.id);
                const Icon = ICONS[plugin.id as keyof typeof ICONS] ?? ICONS[canonicalId as keyof typeof ICONS] ?? Puzzle;
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

                const secondaryRoutes = getBuiltinPluginSecondaryRoutes(plugin.id);
                const subPageNames = secondaryRoutes
                  // eslint-disable-next-line @typescript-eslint/no-explicit-any
                  .map((route) => String(t(route.labelKey as any)))
                  .join(' / ');
                // eslint-disable-next-line @typescript-eslint/no-explicit-any
                const moduleTitle = String(t(`plugins.items.${canonicalId}.name` as any, { defaultValue: plugin.name }));
                // eslint-disable-next-line @typescript-eslint/no-explicit-any
                const moduleDescription = String(t(`plugins.items.${canonicalId}.description` as any, { defaultValue: plugin.name }));
                const targetNav = getBuiltinPluginPrimaryRoute(plugin.id);

                return (
                  <article
                    className={`plugin-row ${isRunning ? 'plugin-row--active' : ''} ${failed ? 'plugin-row--failed' : ''} ${isUnavailable ? 'plugin-row--unavailable' : ''}`}
                    key={plugin.id}
                  >
                    <div className="plugin-row__main">
                      <span className="plugin-row__icon" aria-hidden="true">
                        <Icon size={17} strokeWidth={1.85} />
                      </span>
                      <div className="plugin-row__identity">
                        <div className="plugin-row__title-line">
                          <h2>{moduleTitle}</h2>
                          <span className="plugin-row__status-pill">
                            <span
                              className={`plugin-row__status-dot plugin-row__status-dot--${statusTone}`}
                              aria-hidden="true"
                            />
                            <span className={`plugin-row__status-text plugin-row__status-text--${statusTone}`}>
                              {t(`plugins.state.${plugin.state}`)}
                            </span>
                          </span>
                          {secondaryRoutes.length > 0 && (
                            <span className="plugin-row__subpages-pill">
                              {t('plugins.includesSubPages', { pages: subPageNames })}
                            </span>
                          )}
                        </div>
                        <p className="plugin-row__description" title={moduleDescription}>
                          {moduleDescription}
                        </p>
                        {plugin.error && (
                          <p className="plugin-row__error" role="alert">
                            {plugin.error}
                          </p>
                        )}
                      </div>
                    </div>

                    <details className="plugin-row__details">
                      <summary
                        className="plugin-row__details-trigger"
                        title={t('plugins.permissions')}
                        aria-label={t('plugins.permissions')}
                      >
                        <ShieldCheck size={14} strokeWidth={1.9} aria-hidden="true" />
                        <ChevronDown size={13} strokeWidth={2} className="plugin-row__details-chevron" aria-hidden="true" />
                      </summary>
                      <div className="plugin-row__details-body">
                        <div className="plugin-row__meta-row">
                          <CodeBadge>{plugin.fileName}</CodeBadge>
                          <CodeBadge variant="muted">v{plugin.version || '1.0.0'}</CodeBadge>
                          <CodeBadge variant="muted">{t('plugins.abi', { version: plugin.abiVersion || 1 })}</CodeBadge>
                          <span className="plugin-row__runtime-note">
                            {plugin.active ? t('plugins.resourceActive') : t('plugins.resourceInactive')}
                          </span>
                        </div>
                        {(plugin.capabilities || []).length > 0 && (
                          <div className="plugin-row__meta-group" aria-label={t('plugins.capabilities')}>
                            {(plugin.capabilities || []).map((capability) => (
                              <CodeBadge key={capability}>{capability}</CodeBadge>
                            ))}
                          </div>
                        )}
                        {(plugin.permissions || []).length > 0 && (
                          <div className="plugin-row__meta-group">
                            {plugin.executionModel === 'trusted-native-in-process' && (
                              <span className="plugin-row__trust-note">{t('plugins.fullTrustWarning')}</span>
                            )}
                            <div className="plugin-row__perm-chips">
                              {plugin.permissions.map((permission) => (
                                <CodeBadge key={permission} variant="muted">{permission}</CodeBadge>
                              ))}
                            </div>
                          </div>
                        )}
                      </div>
                    </details>

                    <div className="plugin-row__actions">
                      {onNavigate && plugin.enabled && plugin.active && targetNav && (
                        <Button
                          variant="ghost"
                          size="sm"
                          className="plugin-row__config-btn"
                          onClick={() => onNavigate(targetNav)}
                          title={t('plugins.configure')}
                        >
                          <SlidersHorizontal size={13} strokeWidth={1.9} />
                          <span>{t('plugins.configure')}</span>
                        </Button>
                      )}
                      <Toggle
                        id={`plugin-${plugin.id}`}
                        checked={plugin.enabled}
                        disabled={savingId === plugin.id || (failed && !plugin.enabled) || isUnavailable}
                        onChange={(value) => void setEnabled(plugin, value)}
                      />
                    </div>
                  </article>
                );
              })}
            </div>
          </section>
        ))}
      </div>

      {plugins.length > 0 && groupedSections.length === 0 && (
        <div className="plugins-page__empty">{t('plugins.noMatchSearch')}</div>
      )}

      {plugins.length === 0 && (
        <div className="plugins-page__empty">{t('plugins.empty')}</div>
      )}
    </div>
  );
};

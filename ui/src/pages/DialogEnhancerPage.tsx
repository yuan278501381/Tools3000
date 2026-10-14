/* ─────────────────────────────────────────────────────────────────────────────
 * DialogEnhancerPage.tsx — 文件对话框智能助手设置页
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, type FC } from 'react';
import { Card, Toggle, SettingRow, SettingGroup, Select, Button, CodeBadge } from '../components/UIKit';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  FolderSymlink,
  FolderOpen,
  Sparkles,
  History,
  Star,
  ShieldAlert,
  Trash2,
  Plus,
  FolderHeart,
  Pin,
  PinOff
} from 'lucide-react';
import './DialogEnhancerPage.css';

interface DialogConfig {
  enabled: boolean;
  perAppMemory: boolean;
  quickSwitch: boolean;
  ribbonEnabled: boolean;
  ribbonPosition: string;
}

interface AppMemoryItem {
  processName: string;
  lastPath: string;
  fixedWorkspace?: string;
  isFixed?: boolean;
  lastUsedTimestamp: number;
}

export const DialogEnhancerPage: FC = () => {
  const { t } = useTranslation();
  const [config, setConfig] = useState<DialogConfig>({
    enabled: true,
    perAppMemory: true,
    quickSwitch: true,
    ribbonEnabled: true,
    ribbonPosition: 'top-right',
  });
  const [favorites, setFavorites] = useState<string[]>([]);
  const [appMemories, setAppMemories] = useState<AppMemoryItem[]>([]);
  const [blacklist, setBlacklist] = useState<string[]>([]);
  const [loading, setLoading] = useState(true);

  const [newFavoriteInput, setNewFavoriteInput] = useState('');
  const [newBlacklistInput, setNewBlacklistInput] = useState('');

  useEffect(() => {
    let cancelled = false;
    Promise.allSettled([
      bridgeRequest<DialogConfig>('dialog.getConfig'),
      bridgeRequest<Array<{ id: string; enabled: boolean; active: boolean }>>('plugins.getAll'),
      bridgeRequest<boolean>('config.get', { key: '/dialog/enabled' }),
      bridgeRequest<boolean>('config.get', { key: '/plugins/dialogenhancer/enabled' }),
      bridgeRequest<string[]>('dialog.getFavorites'),
      bridgeRequest<AppMemoryItem[]>('dialog.getAppMemories'),
      bridgeRequest<string[]>('dialog.getBlacklist'),
    ]).then(([cfgRes, pluginsRes, legacyRes, pluginCfgRes, favsRes, memsRes, blRes]) => {
      if (cancelled) return;
      let effectiveEnabled = true;
      if (cfgRes.status === 'fulfilled' && cfgRes.value) {
        effectiveEnabled = cfgRes.value.enabled;
        setConfig(cfgRes.value);
      } else {
        if (pluginsRes.status === 'fulfilled' && Array.isArray(pluginsRes.value)) {
          const item = pluginsRes.value.find((p) => p.id === 'dialogenhancer' || p.id === 'dialog_enhancer');
          if (item && typeof item.enabled === 'boolean') {
            effectiveEnabled = item.enabled;
          }
        } else if (pluginCfgRes.status === 'fulfilled' && typeof pluginCfgRes.value === 'boolean') {
          effectiveEnabled = pluginCfgRes.value;
        } else if (legacyRes.status === 'fulfilled' && typeof legacyRes.value === 'boolean') {
          effectiveEnabled = legacyRes.value;
        }
        setConfig((prev) => ({ ...prev, enabled: effectiveEnabled }));
      }
      if (favsRes.status === 'fulfilled' && Array.isArray(favsRes.value)) setFavorites(favsRes.value);
      if (memsRes.status === 'fulfilled' && Array.isArray(memsRes.value)) setAppMemories(memsRes.value);
      if (blRes.status === 'fulfilled' && Array.isArray(blRes.value)) setBlacklist(blRes.value);
      setLoading(false);
    }).catch(() => {
      if (!cancelled) setLoading(false);
    });

    return () => {
      cancelled = true;
    };
  }, []);

  useBridgeEvent('dialog:configChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      const nextEnabled = payload.enabled;
      setConfig((prev) => ({ ...prev, enabled: nextEnabled }));
    }
  });

  useBridgeEvent('plugins:changed', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { id?: string; enabled?: boolean }) : null;
    if (payload && (payload.id === 'dialogenhancer' || payload.id === 'dialog_enhancer') && typeof payload.enabled === 'boolean') {
      const nextEnabled = payload.enabled;
      setConfig((prev) => ({ ...prev, enabled: nextEnabled }));
    }
  });

  const updateConfig = async (patch: Partial<DialogConfig>) => {
    const previous = config;
    const next = { ...config, ...patch };
    setConfig(next);
    if (typeof patch.enabled === 'boolean') {
      try {
        const res = await bridgeRequest<{ success: boolean; restartRequired?: boolean }>('plugins.setEnabled', { id: 'dialogenhancer', enabled: patch.enabled });
        if (patch.enabled) {
          await bridgeRequest('dialog.updateConfig', patch as Record<string, unknown>).catch(() => {});
        }
        if (res?.restartRequired) {
          toast.info(t('plugins.restartHint'), {
            action: {
              label: t('plugins.restartNow'),
              onClick: () => { void bridgeRequest('app.restart'); },
            },
          });
        }
      } catch {
        setConfig(previous);
      }
      return;
    }
    try {
      await bridgeRequest('dialog.updateConfig', patch as Record<string, unknown>);
    } catch {
      setConfig(previous);
    }
  };

  const handleAddFavorite = async (overridePath?: string) => {
    const path = (overridePath ?? newFavoriteInput).trim();
    if (!path) return;
    try {
      await bridgeRequest('dialog.addFavorite', { path });
      setFavorites((prev) => [...prev.filter((p) => p !== path), path]);
      if (!overridePath) setNewFavoriteInput('');
    } catch {
      // 错误已由 bridgeRequest 自动拦截提示
    }
  };

  const handleBrowseFavorite = async () => {
    try {
      const dir = await bridgeRequest<string | null>('capture.browseDirectory');
      if (dir) {
        await handleAddFavorite(dir);
      }
    } catch (error) {
      toast.error(t('capture.browseFailed'), { description: String(error) });
    }
  };

  const handleRemoveFavorite = async (path: string) => {
    try {
      await bridgeRequest('dialog.removeFavorite', { path });
      setFavorites((prev) => prev.filter((p) => p !== path));
    } catch {
      // 错误已由 bridgeRequest 自动拦截提示
    }
  };

  const handleTogglePinWorkspace = async (item: AppMemoryItem) => {
    const nextFixed = !item.isFixed;
    const targetPath = item.fixedWorkspace || item.lastPath;
    try {
      await bridgeRequest('dialog.setAppFixedWorkspace', {
        processName: item.processName,
        workspacePath: targetPath,
        isFixed: nextFixed
      });
      setAppMemories((prev) => prev.map((m) =>
        m.processName === item.processName ? { ...m, isFixed: nextFixed, fixedWorkspace: targetPath } : m
      ));
    } catch {
      // 错误已由 bridgeRequest 自动拦截提示
    }
  };

  const handleRemoveAppMemory = async (proc: string) => {
    try {
      await bridgeRequest('dialog.removeAppMemory', { processName: proc });
      setAppMemories((prev) => prev.filter((m) => m.processName !== proc));
    } catch {
      // 错误已由 bridgeRequest 自动拦截提示
    }
  };

  const handleClearAppMemories = async () => {
    try {
      await bridgeRequest('dialog.clearAppMemories');
      setAppMemories([]);
    } catch {
      // 错误已由 bridgeRequest 自动拦截提示
    }
  };

  const handleAddBlacklist = async () => {
    const proc = newBlacklistInput.trim();
    if (!proc) return;
    try {
      const next = [...blacklist.filter((p) => p.toLowerCase() !== proc.toLowerCase()), proc];
      await bridgeRequest('dialog.setBlacklist', { blacklist: next });
      setBlacklist(next);
      setNewBlacklistInput('');
    } catch {
      // 错误已由 bridgeRequest 自动拦截提示
    }
  };

  const handleRemoveBlacklist = async (proc: string) => {
    try {
      const next = blacklist.filter((p) => p !== proc);
      await bridgeRequest('dialog.setBlacklist', { blacklist: next });
      setBlacklist(next);
    } catch {
      // 错误已由 bridgeRequest 自动拦截提示
    }
  };

  const formatTime = (timestamp: number) => {
    if (!timestamp) return '-';
    const d = new Date(timestamp * 1000);
    return `${d.getMonth() + 1}/${d.getDate()} ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  };

  if (loading) {
    return <div className="dialog-page-loading">{t('common.loading')}</div>;
  }

  return (
    <div className="dialog-page">
      {/* 1. 基础开关 */}
      <SettingGroup title={t('dialog.generalGroup')} icon={<FolderSymlink size={18} />}>
        <Card>
          <Toggle
            id="dialog-enable"
            label={t('dialog.enable')}
            description={t('dialog.enableDesc')}
            checked={config.enabled}
            onChange={(v) => updateConfig({ enabled: v })}
          />
          <Toggle
            id="dialog-per-app"
            label={t('dialog.perAppMemory')}
            description={t('dialog.perAppMemoryDesc')}
            checked={config.perAppMemory}
            disabled={!config.enabled}
            onChange={(v) => updateConfig({ perAppMemory: v })}
          />
        </Card>
      </SettingGroup>

      {/* 2. 文件对话框顶部快捷工具条：总开关 + 从属选项 */}
      <SettingGroup title={t('dialog.ribbonGroup')} icon={<Sparkles size={18} />}>
        <Card>
          <Toggle
            id="dialog-ribbon-enable"
            label={t('dialog.ribbonEnabled')}
            description={t('dialog.ribbonEnabledDesc')}
            checked={config.ribbonEnabled}
            disabled={!config.enabled}
            onChange={(v) => updateConfig({ ribbonEnabled: v })}
          />
          <div
            className={`dialog-ribbon-options ${!config.enabled || !config.ribbonEnabled ? 'dialog-ribbon-options--disabled' : ''}`}
            aria-label={t('dialog.ribbonOptions')}
          >
            <div className="dialog-ribbon-options__title">{t('dialog.ribbonOptions')}</div>
            <Toggle
              id="dialog-quick-switch"
              label={t('dialog.quickSwitch')}
              description={t('dialog.quickSwitchDesc')}
              checked={config.quickSwitch}
              disabled={!config.enabled || !config.ribbonEnabled}
              onChange={(v) => updateConfig({ quickSwitch: v })}
            />
            <SettingRow label={t('dialog.ribbonPosition')}>
              <Select
                id="dialog-ribbon-pos"
                value={config.ribbonPosition}
                disabled={!config.enabled || !config.ribbonEnabled}
                options={[
                  { value: 'top-right', label: t('dialog.ribbonPositionTopRight') },
                  { value: 'top-center', label: t('dialog.ribbonPositionTopCenter') },
                ]}
                onChange={(v) => updateConfig({ ribbonPosition: v })}
              />
            </SettingRow>
          </div>
        </Card>
      </SettingGroup>

      {/* 3. 常用工作区收藏 */}
      <SettingGroup title={t('dialog.favoritesGroup')} icon={<Star size={18} />}>
        <Card subtitle={t('dialog.favoritesDesc')}>
          <div className="dialog-add-row">
            <input
              type="text"
              className="dialog-input"
              placeholder={t('dialog.enterPath')}
              value={newFavoriteInput}
              onChange={(e) => setNewFavoriteInput(e.target.value)}
              onKeyDown={(e) => e.key === 'Enter' && void handleAddFavorite()}
            />
            <Button variant="ghost" onClick={() => void handleBrowseFavorite()} title={t('dialog.browseFolder')}>
              <FolderOpen size={16} /> {t('dialog.browseFolder')}
            </Button>
            <Button variant="primary" onClick={() => void handleAddFavorite()}>
              <Plus size={16} /> {t('common.add')}
            </Button>
          </div>

          <div className="dialog-list">
            {favorites.length === 0 ? (
              <div className="dialog-empty">{t('dialog.noFavorites')}</div>
            ) : (
              favorites.map((fav) => (
                <div key={fav} className="dialog-list-item">
                  <div className="dialog-item-icon">
                    <FolderHeart size={16} />
                  </div>
                  <div className="dialog-item-text" title={fav}>
                    <CodeBadge>{fav}</CodeBadge>
                  </div>
                  <Button
                    variant="ghost"
                    size="sm"
                    className="dialog-btn-danger"
                    title={t('common.delete')}
                    aria-label={t('common.delete')}
                    onClick={() => handleRemoveFavorite(fav)}
                  >
                    <Trash2 size={14} />
                  </Button>
                </div>
              ))
            )}
          </div>
        </Card>
      </SettingGroup>

      {/* 4. 已记忆的应用路径表 */}
      <SettingGroup title={t('dialog.appMemoriesGroup')} icon={<History size={18} />}>
        <Card subtitle={t('dialog.appMemoriesDesc')}>
          {appMemories.length > 0 && (
            <div style={{ marginBottom: '1rem', display: 'flex', justifyContent: 'flex-end' }}>
              <Button variant="ghost" size="sm" onClick={handleClearAppMemories}>
                <Trash2 size={14} /> {t('dialog.clearAllMemories')}
              </Button>
            </div>
          )}
          <div className="dialog-memories-table">
            {appMemories.length === 0 ? (
              <div className="dialog-empty">{t('dialog.noMemories')}</div>
            ) : (
              appMemories.map((item) => (
                <div key={item.processName} className="dialog-memory-row">
                  <div className="dialog-memory-proc">
                    <CodeBadge className="dialog-proc-badge">{item.processName}</CodeBadge>
                    {item.isFixed && (
                      <span className="dialog-fixed-badge">{t('dialog.fixedBadge')}</span>
                    )}
                  </div>
                  <div className="dialog-memory-path" title={item.isFixed ? item.fixedWorkspace : item.lastPath}>
                    <CodeBadge>{item.isFixed ? item.fixedWorkspace : item.lastPath}</CodeBadge>
                  </div>
                  <div className="dialog-memory-time">{formatTime(item.lastUsedTimestamp)}</div>
                  <div className="dialog-memory-action" style={{ display: 'flex', gap: '0.25rem' }}>
                    <Button
                      variant="ghost"
                      size="sm"
                      title={item.isFixed ? t('dialog.unpinWorkspace') : t('dialog.pinWorkspace')}
                      aria-label={item.isFixed ? t('dialog.unpinWorkspace') : t('dialog.pinWorkspace')}
                      onClick={() => handleTogglePinWorkspace(item)}
                    >
                      {item.isFixed ? <PinOff size={14} /> : <Pin size={14} />}
                    </Button>
                    <Button
                      variant="ghost"
                      size="sm"
                      className="dialog-btn-danger"
                      title={t('common.delete')}
                      aria-label={t('common.delete')}
                      onClick={() => handleRemoveAppMemory(item.processName)}
                    >
                      <Trash2 size={14} />
                    </Button>
                  </div>
                </div>
              ))
            )}
          </div>
        </Card>
      </SettingGroup>

      {/* 5. 排除黑名单 */}
      <SettingGroup title={t('dialog.blacklistGroup')} icon={<ShieldAlert size={18} />}>
        <Card subtitle={t('dialog.blacklistDesc')}>
          <div className="dialog-add-row">
            <input
              type="text"
              className="dialog-input"
              placeholder={t('dialog.enterProcessName')}
              value={newBlacklistInput}
              onChange={(e) => setNewBlacklistInput(e.target.value)}
              onKeyDown={(e) => e.key === 'Enter' && handleAddBlacklist()}
            />
            <Button variant="primary" onClick={handleAddBlacklist}>
              <Plus size={16} /> {t('common.add')}
            </Button>
          </div>

          <div className="dialog-chips">
            {blacklist.map((proc) => (
              <div key={proc} className="dialog-chip">
                <CodeBadge>{proc}</CodeBadge>
                <button
                  type="button"
                  className="dialog-chip-remove"
                  title={t('common.delete')}
                  aria-label={t('common.delete')}
                  onClick={() => handleRemoveBlacklist(proc)}
                >
                  ×
                </button>
              </div>
            ))}
          </div>
        </Card>
      </SettingGroup>
    </div>
  );
};

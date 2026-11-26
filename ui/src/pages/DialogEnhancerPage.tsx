/* ─────────────────────────────────────────────────────────────────────────────
 * DialogEnhancerPage.tsx — 文件对话框智能助手设置页
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, type FC } from 'react';
import {
  Card,
  Toggle,
  SettingRow,
  SettingGroup,
  SubSettingGroup,
  DisabledCascade,
  Select,
  Button,
  CodeBadge,
  ConfirmModal,
  PageSkeleton,
} from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  FolderSymlink,
  FolderOpen,
  Keyboard,
  Sparkles,
  History,
  Star,
  ShieldAlert,
  Trash2,
  Plus,
  FolderHeart,
  Pin,
  PinOff,
  X
} from 'lucide-react';
import './DialogEnhancerPage.css';

interface DialogConfig {
  enabled: boolean;
  perAppMemory: boolean;
  quickSwitch: boolean;
  ribbonEnabled: boolean;
  inlineSearchEnabled: boolean;
  ribbonPosition: string;
  quickJumpHotkey: string;
  inlineSearchHotkey: string;
}

interface AppMemoryItem {
  processName: string;
  lastPath: string;
  fixedWorkspace?: string;
  isFixed?: boolean;
  lastUsedTimestamp: number;
}

const DEFAULT_DIALOG_CONFIG: DialogConfig = {
  enabled: true,
  perAppMemory: true,
  quickSwitch: true,
  ribbonEnabled: true,
  inlineSearchEnabled: true,
  ribbonPosition: 'top-right',
  quickJumpHotkey: 'Ctrl+G',
  inlineSearchHotkey: 'Ctrl+Shift+G',
};

export const DialogEnhancerPage: FC = () => {
  const { t } = useTranslation();
  const [config, setConfig] = useState<DialogConfig>(DEFAULT_DIALOG_CONFIG);
  const [favorites, setFavorites] = useState<string[]>([]);
  const [appMemories, setAppMemories] = useState<AppMemoryItem[]>([]);
  const [blacklist, setBlacklist] = useState<string[]>([]);
  const [loading, setLoading] = useState(true);

  const [newFavoriteInput, setNewFavoriteInput] = useState('');
  const [newBlacklistInput, setNewBlacklistInput] = useState('');

  useEffect(() => {
    let cancelled = false;
    Promise.allSettled([
      bridgeRequest<Partial<DialogConfig>>('dialog.getConfig'),
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
        effectiveEnabled = typeof cfgRes.value.enabled === 'boolean' ? cfgRes.value.enabled : true;
        setConfig({
          ...DEFAULT_DIALOG_CONFIG,
          ...cfgRes.value,
          enabled: effectiveEnabled,
        });
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
    const payload =
      data && typeof data === 'object'
        ? (data as { enabled?: boolean; quickJumpHotkey?: string; inlineSearchHotkey?: string })
        : null;
    if (!payload) return;
    setConfig((prev) => ({
      ...prev,
      ...(typeof payload.enabled === 'boolean' ? { enabled: payload.enabled } : {}),
      ...(typeof payload.quickJumpHotkey === 'string' ? { quickJumpHotkey: payload.quickJumpHotkey } : {}),
      ...(typeof payload.inlineSearchHotkey === 'string' ? { inlineSearchHotkey: payload.inlineSearchHotkey } : {}),
    }));
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

    // P1 修复：总开关统一只通过 plugins.setEnabled 单次调度（后端内部自动同步 dialog.updateConfig 与插件热加载/卸载），
    // 彻底消除先前先停用、再 setEnabled、再启用导致 3 次重复 dialog.updateConfig 的风暴。
    if (typeof patch.enabled === 'boolean') {
      const { enabled, ...restPatch } = patch;
      try {
        const res = await bridgeRequest<{ success: boolean; restartRequired?: boolean }>(
          'plugins.setEnabled',
          { id: 'dialogenhancer', enabled }
        );
        if (enabled) {
          if (Object.keys(restPatch).length > 0) {
            await bridgeRequest('dialog.updateConfig', restPatch as Record<string, unknown>);
          }
          const [cfgRes, favsRes, memsRes, blRes] = await Promise.allSettled([
            bridgeRequest<Partial<DialogConfig>>('dialog.getConfig'),
            bridgeRequest<string[]>('dialog.getFavorites'),
            bridgeRequest<AppMemoryItem[]>('dialog.getAppMemories'),
            bridgeRequest<string[]>('dialog.getBlacklist'),
          ]);
          if (cfgRes.status === 'fulfilled' && cfgRes.value) {
            setConfig({
              ...DEFAULT_DIALOG_CONFIG,
              ...cfgRes.value,
              enabled: true,
            });
          }
          if (favsRes.status === 'fulfilled' && Array.isArray(favsRes.value)) setFavorites(favsRes.value);
          if (memsRes.status === 'fulfilled' && Array.isArray(memsRes.value)) setAppMemories(memsRes.value);
          if (blRes.status === 'fulfilled' && Array.isArray(blRes.value)) setBlacklist(blRes.value);
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

  // P1 修复：改用独立 dialog.browseDirectory IPC，彻底解除对 capture 插件的跨模块耦合
  const handleBrowseFavorite = async () => {
    try {
      const dir = await bridgeRequest<string | null>('dialog.browseDirectory');
      if (dir) {
        await handleAddFavorite(dir);
      }
    } catch (error) {
      toast.error(t('dialog.browseFailed', 'Failed to select folder'), {
        description: String(error),
      });
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

  const [confirmClearMemoriesOpen, setConfirmClearMemoriesOpen] = useState(false);

  const handleClearAppMemories = async () => {
    setConfirmClearMemoriesOpen(false);
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
    return <PageSkeleton cards={3} rowsPerCard={3} />;
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
        </Card>
      </SettingGroup>

      <DisabledCascade disabled={!config.enabled}>
        <div style={{ display: 'flex', flexDirection: 'column', gap: '1.5rem' }}>
          {/* 2. 键盘快捷键直达跳转与内嵌搜索（独立于悬浮工具条，始终可用） */}
          <SettingGroup title={t('dialog.hotkeyGroup', 'Dialog Keyboard Shortcuts & Quick Jump')} icon={<Keyboard size={18} />}>
            <Card>
              <Toggle
                id="dialog-quick-switch"
                label={t('dialog.quickSwitch')}
                description={t('dialog.quickSwitchDesc')}
                checked={config.quickSwitch}
                disabled={!config.enabled}
                onChange={(v) => updateConfig({ quickSwitch: v })}
              />
              <SubSettingGroup disabled={!config.enabled || !config.quickSwitch}>
                <SettingRow
                  label={t('dialog.quickJumpHotkeyLabel', 'Jump to Active File Manager Folder')}
                  description={t(
                    'dialog.quickJumpHotkeyDesc',
                    'Press inside a file dialog to jump straight to the active tab of the most recently used Explorer, Total Commander, Directory Opus, XYplorer, Q-Dir, OneCommander, or Files window (also supports Qt & JetBrains file dialogs). Clear to disable.'
                  )}
                >
                  <HotkeyRecorder
                    id="dialog-quick-jump-hotkey"
                    value={config.quickJumpHotkey}
                    disabled={!config.enabled || !config.quickSwitch}
                    onChange={(nextKey) => {
                      if (
                        nextKey &&
                        config.inlineSearchHotkey &&
                        nextKey.toLowerCase() === config.inlineSearchHotkey.toLowerCase()
                      ) {
                        toast.warning(
                          t(
                            'dialog.hotkeyDuplicateWarn',
                            'This shortcut is already assigned to the other dialog action; please choose a different combination'
                          )
                        );
                        return;
                      }
                      void updateConfig({ quickJumpHotkey: nextKey });
                    }}
                  />
                </SettingRow>
              </SubSettingGroup>

              <Toggle
                id="dialog-inline-search"
                label={t('dialog.inlineSearchEnabled', 'Enable inline quick search & jump')}
                description={t(
                  'dialog.inlineSearchEnabledDesc',
                  'Click the ribbon search icon or press your shortcut inside file dialogs for instant Listary-style search; shows local paths in 0ms and stays folder-only in Save As mode'
                )}
                checked={config.inlineSearchEnabled}
                disabled={!config.enabled}
                onChange={(v) => updateConfig({ inlineSearchEnabled: v })}
              />
              <SubSettingGroup disabled={!config.enabled || !config.inlineSearchEnabled}>
                <SettingRow
                  label={t('dialog.inlineSearchHotkeyLabel', 'Open Inline Quick Search')}
                  description={t(
                    'dialog.inlineSearchHotkeyDesc',
                    'Scoped exclusively to foreground file dialogs so it never interferes with other apps; works even when the floating ribbon is hidden. Clear to disable.'
                  )}
                >
                  <HotkeyRecorder
                    id="dialog-inline-search-hotkey"
                    value={config.inlineSearchHotkey}
                    disabled={!config.enabled || !config.inlineSearchEnabled}
                    onChange={(nextKey) => {
                      if (
                        nextKey &&
                        config.quickJumpHotkey &&
                        nextKey.toLowerCase() === config.quickJumpHotkey.toLowerCase()
                      ) {
                        toast.warning(
                          t(
                            'dialog.hotkeyDuplicateWarn',
                            'This shortcut is already assigned to the other dialog action; please choose a different combination'
                          )
                        );
                        return;
                      }
                      void updateConfig({ inlineSearchHotkey: nextKey });
                    }}
                  />
                </SettingRow>
              </SubSettingGroup>
            </Card>
          </SettingGroup>

          {/* 3. 文件对话框悬浮快捷工具条 */}
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
              <SubSettingGroup disabled={!config.enabled || !config.ribbonEnabled}>
                <SettingRow label={t('dialog.ribbonPosition')}>
                  <Select
                    id="dialog-ribbon-pos"
                    value={config.ribbonPosition}
                    disabled={!config.enabled || !config.ribbonEnabled}
                    options={[
                      { value: 'top-right', label: t('dialog.ribbonPositionTopRight') },
                      { value: 'top-center', label: t('dialog.ribbonPositionTopCenter') },
                      { value: 'bottom-right', label: t('dialog.ribbonPositionBottomRight', 'Bottom Right') },
                      { value: 'bottom-center', label: t('dialog.ribbonPositionBottomCenter', 'Bottom Center') },
                    ]}
                    onChange={(v) => updateConfig({ ribbonPosition: v })}
                  />
                </SettingRow>
              </SubSettingGroup>
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

          {/* 4. 已记忆的应用路径表（父子同处一卡） */}
          <SettingGroup title={t('dialog.appMemoriesGroup')} icon={<History size={18} />}>
            <Card subtitle={t('dialog.appMemoriesDesc')}>
              <Toggle
                id="dialog-per-app"
                label={t('dialog.perAppMemory')}
                description={t('dialog.perAppMemoryDesc')}
                checked={config.perAppMemory}
                disabled={!config.enabled}
                onChange={(v) => updateConfig({ perAppMemory: v })}
              />
              <SubSettingGroup disabled={!config.enabled || !config.perAppMemory}>
                {appMemories.length > 0 && (
                  <div className="dialog-memories-toolbar">
                    <Button
                      variant="ghost"
                      size="sm"
                      disabled={!config.enabled || !config.perAppMemory}
                      onClick={() => setConfirmClearMemoriesOpen(true)}
                    >
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
                        <div className="dialog-memory-action">
                          <Button
                            variant="ghost"
                            size="sm"
                            disabled={!config.enabled || !config.perAppMemory}
                            title={item.isFixed ? t('dialog.unpinWorkspace') : t('dialog.pinWorkspace')}
                            aria-label={item.isFixed ? t('dialog.unpinWorkspace') : t('dialog.pinWorkspace')}
                            onClick={() => handleTogglePinWorkspace(item)}
                          >
                            {item.isFixed ? <PinOff size={14} /> : <Pin size={14} />}
                          </Button>
                          <Button
                            variant="ghost"
                            size="sm"
                            disabled={!config.enabled || !config.perAppMemory}
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
              </SubSettingGroup>
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
                      <X size={13} aria-hidden="true" />
                    </button>
                  </div>
                ))}
              </div>
            </Card>
          </SettingGroup>
        </div>
      </DisabledCascade>

      <ConfirmModal
        open={confirmClearMemoriesOpen}
        title={t('dialog.clearAllMemories')}
        description={t('dialog.confirmClearMemories')}
        confirmText={t('common.confirm')}
        onConfirm={handleClearAppMemories}
        onClose={() => setConfirmClearMemoriesOpen(false)}
      />
    </div>
  );
};

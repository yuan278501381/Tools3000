/* ─────────────────────────────────────────────────────────────────────────────
 * RemoteBoostPage.tsx — 远程协助主控单边增强设置中心 (Remote Desktop Host Boost)
 *
 * 核心设计:
 *   1. 沉浸式系统热键直通 (Immersive Remote Hotkey Tunnel)
 *   2. 远程修饰键卡死一键急救 (Modifier Key Emergency Flush)
 *   3. 远控输入法智能脱敏 (Smart Remote IME Sanitizer)
 *   4. 世界级玻璃拟态微晶卡片与微徽章交互
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { Card, Toggle, SettingRow, SettingGroup, Button, CodeBadge, PageSkeleton } from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  Cast,
  Zap,
  Languages,
  RotateCcw,
  Plus,
  Trash2,
  CheckCircle2,
  Radio,
} from 'lucide-react';
import './RemoteBoostPage.css';

interface RemoteMasterSettings {
  enabled: boolean;
  hotkeyTunnelEnabled: boolean;
  emergencyFlushEnabled: boolean;
  doubleRightCtrlTrigger: boolean;
  imeSanitizerEnabled: boolean;
  emergencyShortcut: string;
  targetProcesses: string[];
  targetClasses: string[];
}

interface RemoteState {
  isRemoteForeground: boolean;
  imeSanitized: boolean;
  activeProcess: string;
}

const DEFAULT_SETTINGS: RemoteMasterSettings = {
  enabled: true,
  hotkeyTunnelEnabled: true,
  emergencyFlushEnabled: true,
  doubleRightCtrlTrigger: false,
  imeSanitizerEnabled: true,
  emergencyShortcut: 'Ctrl+Alt+Backspace',
  targetProcesses: [
    'ToDesk.exe',
    'SunloginClient.exe',
    'AnyDesk.exe',
    'RustDesk.exe',
    'mstsc.exe',
    'msrdc.exe',
    'msrdcw.exe',
    'TeamViewer.exe',
    'vncviewer.exe',
    'tv_w32.exe',
    'parsec.exe',
    'Splashtop.exe',
    'UltraViewer_Desktop.exe',
    'vmware.exe',
    'VirtualBoxVM.exe',
    'uuremote.exe',
    'GameViewer.exe',
    'Moonlight.exe',
  ],
  targetClasses: [
    'TSSHELLWND',
    'IHWindowClass',
    'OPContainerClass',
    'SunloginMain',
    'SunloginRemote',
    'ToDesk_Main',
    'ToDesk_Remote',
    'anydesk',
    'RustDesk',
    'TeamViewer',
  ],
};

export const RemoteBoostPage: FC = () => {
  const { t } = useTranslation();
  const [settings, setSettings] = useState<RemoteMasterSettings>(DEFAULT_SETTINGS);
  const [remoteState, setRemoteState] = useState<RemoteState>({
    isRemoteForeground: false,
    imeSanitized: false,
    activeProcess: '',
  });
  const [loading, setLoading] = useState(true);
  const [newProcessInput, setNewProcessInput] = useState('');
  const [newClassInput, setNewClassInput] = useState('');
  const [flushing, setFlushing] = useState(false);

  // 加载后台真实配置与状态
  const loadData = useCallback(() => {
    bridgeRequest<RemoteMasterSettings>('remote.getSettings')
      .then((res) => {
        if (res && typeof res.enabled === 'boolean') {
          setSettings((prev) => ({
            ...prev,
            ...res,
            targetProcesses: Array.isArray(res.targetProcesses) ? res.targetProcesses : prev.targetProcesses,
            targetClasses: Array.isArray(res.targetClasses) ? res.targetClasses : prev.targetClasses,
          }));
        }
      })
      .catch(() => {
        void Promise.allSettled([
          bridgeRequest<Array<{ id: string; enabled: boolean }>>('plugins.getAll'),
          bridgeRequest<boolean>('config.get', { key: '/plugins/remote_boost/enabled' }),
          bridgeRequest<boolean>('config.get', { key: '/remote_boost/enabled' }),
        ]).then(([pluginsRes, pluginCfgRes, legacyRes]) => {
          let effectiveEnabled = true;
          if (pluginsRes.status === 'fulfilled' && Array.isArray(pluginsRes.value)) {
            const item = pluginsRes.value.find((p) => p.id === 'remote_boost' || p.id === 'remote');
            if (item && typeof item.enabled === 'boolean') effectiveEnabled = item.enabled;
          } else if (pluginCfgRes.status === 'fulfilled' && typeof pluginCfgRes.value === 'boolean') {
            effectiveEnabled = pluginCfgRes.value;
          } else if (legacyRes.status === 'fulfilled' && typeof legacyRes.value === 'boolean') {
            effectiveEnabled = legacyRes.value;
          }
          setSettings((prev) => ({ ...prev, enabled: effectiveEnabled }));
        });
      })
      .finally(() => setLoading(false));

    bridgeRequest<RemoteState>('remote.getState')
      .then((res) => {
        if (res) setRemoteState(res);
      })
      .catch(() => {});
  }, []);

  useEffect(() => {
    loadData();
  }, [loadData]);

  useBridgeEvent('remote:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      const nextEnabled = payload.enabled;
      setSettings((prev) => ({ ...prev, enabled: nextEnabled }));
    }
  });

  useBridgeEvent('plugins:changed', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { id?: string; enabled?: boolean }) : null;
    if (payload && (payload.id === 'remote_boost' || payload.id === 'remote') && typeof payload.enabled === 'boolean') {
      const nextEnabled = payload.enabled;
      setSettings((prev) => ({ ...prev, enabled: nextEnabled }));
    }
  });

  // 更新设置
  const updateSettings = async (patch: Partial<RemoteMasterSettings>) => {
    const previous = settings;
    const next = { ...settings, ...patch };
    setSettings(next);
    try {
      if (typeof patch.enabled === 'boolean') {
        await Promise.all([
          bridgeRequest('plugins.setEnabled', { id: 'remote_boost', enabled: patch.enabled }),
          bridgeRequest('remote.updateSettings', patch as Record<string, unknown>),
        ]);
      } else {
        await bridgeRequest('remote.updateSettings', patch as Record<string, unknown>);
      }
    } catch {
      setSettings(previous);
      toast.error(t('remoteBoost.saveFailed', 'Failed to save settings'));
    }
  };

  // 触发一键急救
  const handleEmergencyFlush = async () => {
    if (flushing) return;
    setFlushing(true);
    try {
      await bridgeRequest('remote.emergencyFlush');
      toast.success(t('remoteBoost.flushSuccess', 'All modifier keys and mouse buttons released'));
      bridgeRequest<RemoteState>('remote.getState')
        .then((res) => {
          if (res) setRemoteState(res);
        })
        .catch(() => {});
    } catch {
      toast.error(t('remoteBoost.flushFailed', 'Emergency flush failed'));
    } finally {
      setTimeout(() => setFlushing(false), 300);
    }
  };

  // 重置默认
  const handleResetDefaults = async () => {
    try {
      await bridgeRequest('remote.resetDefaults');
      setSettings(DEFAULT_SETTINGS);
      toast.success(t('remoteBoost.resetSuccess', 'Settings restored to defaults'));
    } catch {
      toast.error(t('remoteBoost.resetFailed', 'Failed to reset settings'));
    }
  };

  // 添加自定义远控进程
  const handleAddProcess = () => {
    let proc = newProcessInput.trim();
    if (!proc) return;
    if (!proc.toLowerCase().endsWith('.exe')) {
      proc += '.exe';
    }
    if (settings.targetProcesses.some((p) => p.toLowerCase() === proc.toLowerCase())) {
      toast.info(t('remoteBoost.processExists', 'Process already in target list'));
      return;
    }
    const nextList = [...settings.targetProcesses, proc];
    void updateSettings({ targetProcesses: nextList });
    setNewProcessInput('');
  };

  // 移除远控进程
  const handleRemoveProcess = (target: string) => {
    const nextList = settings.targetProcesses.filter((p) => p !== target);
    void updateSettings({ targetProcesses: nextList });
  };

  // 添加自定义远控窗口类名
  const handleAddClass = () => {
    const cls = newClassInput.trim();
    if (!cls) return;
    if (settings.targetClasses.some((c) => c.toLowerCase() === cls.toLowerCase())) {
      toast.info(t('remoteBoost.classExists', 'Window class already in target list'));
      return;
    }
    const nextList = [...settings.targetClasses, cls];
    void updateSettings({ targetClasses: nextList });
    setNewClassInput('');
  };

  // 移除远控窗口类名
  const handleRemoveClass = (target: string) => {
    const nextList = settings.targetClasses.filter((c) => c !== target);
    void updateSettings({ targetClasses: nextList });
  };

  if (loading) {
    return <PageSkeleton cards={3} rowsPerCard={3} />;
  }

  return (
    <div className="remote-boost-page">
      {/* ── 顶部总控卡片与前台感知状态条 ────────────────────────────────────── */}
      <Card className="remote-boost-hero-card">
        <div className="remote-boost-hero">
          <div className="remote-boost-hero__info" style={{ flexDirection: 'row', alignItems: 'flex-start', gap: '12px' }}>
            <Cast size={20} className="remote-boost-hero__icon" style={{ marginTop: 2, flexShrink: 0 }} />
            <p className="remote-boost-hero__desc">
              {t(
                'remoteBoost.masterDesc',
                'Unilateral enhancement on host PC: Immersive system hotkey tunnel, emergency modifier flush, and intelligent IME sanitizing without installing any software on remote machines.'
              )}
            </p>
          </div>
          <div className="remote-boost-hero__toggle">
            <Toggle
              id="remote-master-toggle"
              checked={settings.enabled}
              onChange={(checked) => void updateSettings({ enabled: checked })}
              size="lg"
            />
          </div>
        </div>

        {/* 动态前台连接指示条 */}
        <div className="remote-boost-status-bar">
          <div className="remote-boost-status-indicator">
            {remoteState.isRemoteForeground ? (
              <>
                <span className="remote-boost-dot remote-boost-dot--active" />
                <span className="remote-boost-status-text remote-boost-status-text--active">
                  {t('remoteBoost.statusControlling', 'Controlling remote canvas:')}{' '}
                  <CodeBadge variant="primary">{remoteState.activeProcess || 'Remote Canvas'}</CodeBadge>
                </span>
              </>
            ) : (
              <>
                <span className="remote-boost-dot remote-boost-dot--idle" />
                <span className="remote-boost-status-text">
                  {t('remoteBoost.statusIdle', 'Idle (Operating on local desktop)')}
                </span>
              </>
            )}
          </div>
          {remoteState.imeSanitized && (
            <div className="remote-boost-ime-pill" title={t('remoteBoost.imeSanitizedTooltip', 'IME Sanitized')}>
              <Languages size={13} />
              <span>{t('remoteBoost.imePureEnglish', 'ENG 0409 Active')}</span>
            </div>
          )}
        </div>
      </Card>

      {/* ── 引擎 1: 沉浸式系统热键直通 ───────────────────────────────────────── */}
      <Card
        title={t('remoteBoost.tunnelSectionTitle', 'Immersive Remote Hotkey Tunnel')}
        subtitle={t(
          'remoteBoost.tunnelSectionSubtitle',
          'Intercepts local Windows priority consumption on system hotkeys and forwards hardware scancodes to remote canvas'
        )}
        className="remote-boost-card"
      >
        <SettingGroup title={t('remoteBoost.hotkeyTunnelTitle', 'Enable System Hotkey Tunnel')}>
          <SettingRow
            label={t('remoteBoost.hotkeyTunnelTitle', 'Enable System Hotkey Tunnel')}
            description={t(
              'remoteBoost.hotkeyTunnelDesc',
              'Forwards Win, Win+R, Win+E, Win+D, Win+X, Alt+Tab, Ctrl+Shift+Esc to remote PC without triggering local menus'
            )}
          >
            <Toggle
              id="hotkey-tunnel-toggle"
              checked={settings.hotkeyTunnelEnabled && settings.enabled}
              disabled={!settings.enabled}
              onChange={(checked) => void updateSettings({ hotkeyTunnelEnabled: checked })}
            />
          </SettingRow>

          {/* 直通按键胶囊清单 */}
          <div className="remote-boost-tags-container">
            <span className="remote-boost-tags-label">
              {t('remoteBoost.interceptedKeysLabel', 'Intercepted & Tunneled Keys:')}
            </span>
            <div className="remote-boost-key-chips">
              <CodeBadge variant="primary">Win</CodeBadge>
              <CodeBadge variant="primary">Win + R</CodeBadge>
              <CodeBadge variant="primary">Win + E</CodeBadge>
              <CodeBadge variant="primary">Win + D</CodeBadge>
              <CodeBadge variant="primary">Win + X</CodeBadge>
              <CodeBadge variant="primary">Alt + Tab</CodeBadge>
              <CodeBadge variant="primary">Ctrl + Shift + Esc</CodeBadge>
            </div>
          </div>

          {/* 支持的远控客户端进程列表 */}
          <div className="remote-boost-process-section">
            <div className="remote-boost-process-header">
              <span className="remote-boost-tags-label">
                {t('remoteBoost.targetProcessesLabel', 'Target Remote Client Processes:')}
              </span>
            </div>

            <div className="remote-boost-process-tags">
              {settings.targetProcesses.map((proc) => (
                <span key={proc} className="remote-boost-process-chip">
                  <CodeBadge variant="muted">{proc}</CodeBadge>
                  <button
                    type="button"
                    className="remote-boost-process-chip__remove"
                    title={t('common.delete', 'Delete')}
                    aria-label={`${t('common.delete', 'Delete')} ${proc}`}
                    onClick={() => handleRemoveProcess(proc)}
                  >
                    <Trash2 size={12} />
                  </button>
                </span>
              ))}
            </div>

            {/* 新增进程输入栏 */}
            <div className="remote-boost-add-row">
              <input
                type="text"
                className="remote-boost-input"
                placeholder={t('remoteBoost.addProcessPlaceholder', 'e.g., CustomRemote.exe')}
                value={newProcessInput}
                onChange={(e) => setNewProcessInput(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') handleAddProcess();
                }}
              />
              <Button variant="secondary" size="sm" onClick={handleAddProcess}>
                <Plus size={14} />
                <span>{t('common.add', 'Add')}</span>
              </Button>
            </div>
          </div>

          {/* 支持的远控窗口类名列表 (C-03 开放编辑) */}
          <div className="remote-boost-process-section">
            <div className="remote-boost-process-header">
              <span className="remote-boost-tags-label">
                {t('remoteBoost.targetClassesLabel', 'Target Remote Window Classes:')}
              </span>
            </div>

            <div className="remote-boost-process-tags">
              {settings.targetClasses.map((cls) => (
                <span key={cls} className="remote-boost-process-chip">
                  <CodeBadge variant="muted">{cls}</CodeBadge>
                  <button
                    type="button"
                    className="remote-boost-process-chip__remove"
                    title={t('common.delete', 'Delete')}
                    aria-label={`${t('common.delete', 'Delete')} ${cls}`}
                    onClick={() => handleRemoveClass(cls)}
                  >
                    <Trash2 size={12} />
                  </button>
                </span>
              ))}
            </div>

            <div className="remote-boost-add-row">
              <input
                type="text"
                className="remote-boost-input"
                placeholder={t('remoteBoost.addClassPlaceholder', 'e.g., TSSHELLWND')}
                value={newClassInput}
                onChange={(e) => setNewClassInput(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') handleAddClass();
                }}
              />
              <Button variant="secondary" size="sm" onClick={handleAddClass}>
                <Plus size={14} />
                <span>{t('common.add', 'Add')}</span>
              </Button>
            </div>
          </div>
        </SettingGroup>
      </Card>

      {/* ── 引擎 2: 远程修饰键卡死一键急救 ───────────────────────────────────── */}
      <Card
        title={t('remoteBoost.flushSectionTitle', 'Modifier Key Emergency Flush')}
        subtitle={t(
          'remoteBoost.flushSectionSubtitle',
          'Instantly releases stuck Ctrl, Alt, Shift, Win and mouse buttons caused by network latency or focus loss'
        )}
        className="remote-boost-card"
      >
        <SettingGroup title={t('remoteBoost.emergencyFlushTitle', 'Emergency Flush Engine')}>
          <SettingRow
            label={t('remoteBoost.emergencyFlushTitle', 'Emergency Flush Engine')}
            description={t(
              'remoteBoost.emergencyFlushDesc',
              'Sends atomic 11-channel KEYUP pulses across hardware scancodes to completely flush remote stuck keys'
            )}
          >
            <Toggle
              id="emergency-flush-toggle"
              checked={settings.emergencyFlushEnabled && settings.enabled}
              disabled={!settings.enabled}
              onChange={(checked) => void updateSettings({ emergencyFlushEnabled: checked })}
            />
          </SettingRow>

          <SettingRow
            label={t('remoteBoost.doubleRightCtrlTitle', 'Double-tap Right Ctrl Trigger')}
            description={t(
              'remoteBoost.doubleRightCtrlDesc',
              'Double-tap Right Ctrl within 400ms to immediately fire emergency flush without lifting hands from keyboard'
            )}
          >
            <Toggle
              id="double-ctrl-toggle"
              checked={settings.doubleRightCtrlTrigger && settings.emergencyFlushEnabled && settings.enabled}
              disabled={!settings.enabled || !settings.emergencyFlushEnabled}
              onChange={(checked) => void updateSettings({ doubleRightCtrlTrigger: checked })}
            />
          </SettingRow>

          <SettingRow
            label={t('remoteBoost.dedicatedShortcutTitle', 'Dedicated Emergency Hotkey')}
            description={t('remoteBoost.dedicatedShortcutDesc', 'Global emergency shortcut to trigger flush anytime')}
          >
            <HotkeyRecorder
              id="remote-emergency-shortcut"
              value={settings.emergencyShortcut}
              onChange={(val) => void updateSettings({ emergencyShortcut: val })}
            />
          </SettingRow>

          {/* 立即急救冲刷按钮 */}
          <div className="remote-boost-flush-action-bar">
            <div className="remote-boost-flush-desc">
              <Radio size={15} className="remote-boost-flush-icon" />
              <span>
                {t(
                  'remoteBoost.flushTestDesc',
                  'Test flush: Emits full-array KEYUP pulses to current window and cleans sticky states'
                )}
              </span>
            </div>
            <Button
              variant="primary"
              size="md"
              disabled={flushing}
              onClick={() => void handleEmergencyFlush()}
              className="remote-boost-flush-btn"
            >
              <Zap size={15} />
              <span>{flushing ? t('remoteBoost.flushing', 'Flushing...') : t('remoteBoost.flushNow', 'Flush Now')}</span>
            </Button>
          </div>
        </SettingGroup>
      </Card>

      {/* ── 引擎 3: 远控输入法智能脱敏 ───────────────────────────────────────── */}
      <Card
        title={t('remoteBoost.imeSectionTitle', 'Smart Remote IME Sanitizer')}
        subtitle={t(
          'remoteBoost.imeSectionSubtitle',
          'Prevents local Chinese IME candidate window from obscuring and corrupting remote keyboard input'
        )}
        className="remote-boost-card"
      >
        <SettingGroup title={t('remoteBoost.imeSanitizerTitle', 'Auto Switch to US English on Focus')}>
          <SettingRow
            label={t('remoteBoost.imeSanitizerTitle', 'Auto Switch to US English on Focus')}
            description={t(
              'remoteBoost.imeSanitizerDesc',
              'Automatically activates pure US English (ENG 0409) when entering remote window; seamlessly restores original IME on blur'
            )}
          >
            <Toggle
              id="ime-sanitizer-toggle"
              checked={settings.imeSanitizerEnabled && settings.enabled}
              disabled={!settings.enabled}
              onChange={(checked) => void updateSettings({ imeSanitizerEnabled: checked })}
            />
          </SettingRow>
        </SettingGroup>
      </Card>

      {/* ── 底部操作栏 ──────────────────────────────────────────────────────── */}
      <div className="remote-boost-footer">
        <Button variant="ghost" size="sm" onClick={() => void handleResetDefaults()}>
          <RotateCcw size={14} />
          <span>{t('remoteBoost.resetDefaults', 'Reset Defaults')}</span>
        </Button>
        <div className="remote-boost-footer__status">
          <CheckCircle2 size={13} className="remote-boost-footer__check" />
          <span>{t('remoteBoost.autoSaved', 'Auto saved')}</span>
        </div>
      </div>
    </div>
  );
};

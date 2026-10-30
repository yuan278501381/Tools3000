/* ─────────────────────────────────────────────────────────────────────────────
 * TargetAppPickerModal — WGesture 2 风格快速添加目标应用弹窗
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useRef, type FC } from 'react';
import { Modal, Field, Button, TextInput, Tabs } from './UIKit';
import { useTranslation } from 'react-i18next';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import {
  Crosshair,
  AppWindow,
  Sparkles,
  SlidersHorizontal,
  RefreshCw,
  Globe,
  Code2,
  Terminal,
  Paintbrush,
  Compass,
  FileText,
  Table,
  Presentation,
  MessageSquare,
  Folder,
  Activity,
  Check,
  CheckCircle2,
  Ban,
  Cpu,
  Layers,
  LayoutGrid,
} from 'lucide-react';

type TargetKind = 'process' | 'class';
type PickerTab = 'running' | 'presets' | 'manual';

interface OpenWindowItem {
  title: string;
  processName: string;
  windowClass: string;
  pid: number;
}

interface AppPreset {
  name: string;
  nameKey?: string;
  categoryKey?: string;
  category: 'browser' | 'dev' | 'office' | 'system';
  categoryLabel: string;
  processName: string;
  windowClass?: string;
}

const PRESETS: AppPreset[] = [
  // 浏览器
  { name: 'Google Chrome', category: 'browser', categoryLabel: 'Browser', categoryKey: 'components.catBrowserPreset', processName: 'chrome.exe', windowClass: 'Chrome_WidgetWin_1' },
  { name: 'Microsoft Edge', category: 'browser', categoryLabel: 'Browser', categoryKey: 'components.catBrowserPreset', processName: 'msedge.exe', windowClass: 'Chrome_WidgetWin_1' },
  { name: 'Mozilla Firefox', category: 'browser', categoryLabel: 'Browser', categoryKey: 'components.catBrowserPreset', processName: 'firefox.exe', windowClass: 'MozillaWindowClass' },
  { name: '360 Extreme Browser', nameKey: 'components.app360Browser', category: 'browser', categoryLabel: 'Browser', categoryKey: 'components.catBrowserPreset', processName: '360chrome.exe' },

  // 开发与设计
  { name: 'VS Code', category: 'dev', categoryLabel: 'Dev & Design', categoryKey: 'picker.catDevDesign', processName: 'Code.exe', windowClass: 'Chrome_WidgetWin_1' },
  { name: 'Visual Studio', category: 'dev', categoryLabel: 'Dev & Design', categoryKey: 'picker.catDevDesign', processName: 'devenv.exe' },
  { name: 'Windows Terminal', category: 'dev', categoryLabel: 'Dev & Design', categoryKey: 'picker.catDevDesign', processName: 'WindowsTerminal.exe' },
  { name: 'IntelliJ IDEA', category: 'dev', categoryLabel: 'Dev & Design', categoryKey: 'picker.catDevDesign', processName: 'idea64.exe' },
  { name: 'Adobe Photoshop', category: 'dev', categoryLabel: 'Dev & Design', categoryKey: 'picker.catDevDesign', processName: 'Photoshop.exe' },
  { name: 'AutoCAD', category: 'dev', categoryLabel: 'Dev & Design', categoryKey: 'picker.catDevDesign', processName: 'acad.exe' },

  // 办公协同
  { name: 'Microsoft Word', category: 'office', categoryLabel: 'Office & Collab', categoryKey: 'picker.catOfficeCollab', processName: 'WINWORD.EXE' },
  { name: 'Microsoft Excel', category: 'office', categoryLabel: 'Office & Collab', categoryKey: 'picker.catOfficeCollab', processName: 'EXCEL.EXE' },
  { name: 'Microsoft PowerPoint', category: 'office', categoryLabel: 'Office & Collab', categoryKey: 'picker.catOfficeCollab', processName: 'POWERPNT.EXE' },
  { name: 'WPS Office', category: 'office', categoryLabel: 'Office & Collab', categoryKey: 'picker.catOfficeCollab', processName: 'wps.exe' },
  { name: 'WeChat', nameKey: 'picker.appWeChat', category: 'office', categoryLabel: 'Office & Collab', categoryKey: 'picker.catOfficeCollab', processName: 'WeChat.exe', windowClass: 'WeChatMainWndForPC' },
  { name: 'Feishu', nameKey: 'picker.appFeishu', category: 'office', categoryLabel: 'Office & Collab', categoryKey: 'picker.catOfficeCollab', processName: 'Feishu.exe' },
  { name: 'DingTalk', nameKey: 'picker.appDingTalk', category: 'office', categoryLabel: 'Office & Collab', categoryKey: 'picker.catOfficeCollab', processName: 'DingTalk.exe' },
  { name: 'QQ', category: 'office', categoryLabel: 'Office & Collab', categoryKey: 'picker.catOfficeCollab', processName: 'QQ.exe' },

  // 系统工具
  { name: 'File Explorer', nameKey: 'picker.appExplorer', category: 'system', categoryLabel: 'System Tools', categoryKey: 'picker.catSystemTools', processName: 'explorer.exe', windowClass: 'CabinetWClass' },
  { name: 'Notepad', nameKey: 'picker.appNotepad', category: 'system', categoryLabel: 'System Tools', categoryKey: 'picker.catSystemTools', processName: 'notepad.exe', windowClass: 'Notepad' },
  { name: 'Task Manager', nameKey: 'picker.appTaskmgr', category: 'system', categoryLabel: 'System Tools', categoryKey: 'picker.catSystemTools', processName: 'Taskmgr.exe' },
];

function renderVectorAppIcon(proc: string) {
  const p = proc.toLowerCase();
  if (p.includes('chrome') || p.includes('edge') || p.includes('firefox') || p.includes('360') || p.includes('browser')) {
    return <Globe size={14} />;
  }
  if (p.includes('code') || p.includes('devenv') || p.includes('idea') || p.includes('studio')) {
    return <Code2 size={14} />;
  }
  if (p.includes('terminal') || p.includes('cmd') || p.includes('powershell')) {
    return <Terminal size={14} />;
  }
  if (p.includes('photoshop') || p.includes('illustrator') || p.includes('figma')) {
    return <Paintbrush size={14} />;
  }
  if (p.includes('cad') || p.includes('acad')) {
    return <Compass size={14} />;
  }
  if (p.includes('word') || p.includes('wps') || p.includes('notepad') || p.includes('text')) {
    return <FileText size={14} />;
  }
  if (p.includes('excel') || p.includes('sheet') || p.includes('calc')) {
    return <Table size={14} />;
  }
  if (p.includes('powerpnt') || p.includes('ppt')) {
    return <Presentation size={14} />;
  }
  if (p.includes('wechat') || p.includes('qq') || p.includes('feishu') || p.includes('dingtalk') || p.includes('slack')) {
    return <MessageSquare size={14} />;
  }
  if (p.includes('explorer')) {
    return <Folder size={14} />;
  }
  if (p.includes('taskmgr') || p.includes('monitor')) {
    return <Activity size={14} />;
  }
  return <AppWindow size={14} />;
}

export interface AddedTargetResult {
  name: string;
  processName: string;
  windowClass: string;
  effect: number; // 1 = Disable, 2 = UseProfile
}

interface Props {
  defaultDisabled?: boolean;
  onAdd: (res: AddedTargetResult) => void;
  onClose: () => void;
}

export const TargetAppPickerModal: FC<Props> = ({ defaultDisabled = false, onAdd, onClose }) => {
  const { t } = useTranslation();
  const [name, setName] = useState('');
  const [targetKind, setTargetKind] = useState<TargetKind>('process');
  const [targetValue, setTargetValue] = useState('');
  const [effect, setEffect] = useState<number>(defaultDisabled ? 1 : 2);
  const [pickerTab, setPickerTab] = useState<PickerTab>('running');
  const [selectedCategory, setSelectedCategory] = useState<string>('all');
  const [openWindows, setOpenWindows] = useState<OpenWindowItem[]>([]);
  const [loadingWindows, setLoadingWindows] = useState(false);
  const [countdown, setCountdown] = useState<number | null>(null);
  const [justPicked, setJustPicked] = useState(false);
  const [pickedName, setPickedName] = useState<string | null>(null);
  const countdownIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const justPickedTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const isMountedRef = useRef(true);

  const resolvePresetName = (preset: AppPreset) =>
    preset.nameKey ? t(preset.nameKey, preset.name) : preset.name;

  const scheduleJustPickedReset = (delayMs: number) => {
    if (justPickedTimerRef.current !== null) {
      clearTimeout(justPickedTimerRef.current);
    }
    justPickedTimerRef.current = setTimeout(() => {
      justPickedTimerRef.current = null;
      if (isMountedRef.current) {
        setJustPicked(false);
      }
    }, delayMs);
  };

  const loadOpenWindows = async () => {
    setLoadingWindows(true);
    try {
      const res = await bridgeRequest<{ windows: OpenWindowItem[] }>('window.getOpenWindows');
      if (isMountedRef.current && res && Array.isArray(res.windows)) {
        setOpenWindows(res.windows);
      }
    } catch {
      // ignore
    } finally {
      if (isMountedRef.current) {
        setLoadingWindows(false);
      }
    }
  };

  useEffect(() => {
    isMountedRef.current = true;
    void bridgeRequest<{ windows: OpenWindowItem[] }>('window.getOpenWindows')
      .then((res) => {
        if (isMountedRef.current && res && Array.isArray(res.windows)) {
          setOpenWindows(res.windows);
        }
      })
      .catch(() => {});
    return () => {
      isMountedRef.current = false;
      if (countdownIntervalRef.current !== null) {
        clearInterval(countdownIntervalRef.current);
        countdownIntervalRef.current = null;
      }
      if (justPickedTimerRef.current !== null) {
        clearTimeout(justPickedTimerRef.current);
        justPickedTimerRef.current = null;
      }
    };
  }, []);

  const handleSelectRunningApp = (win: OpenWindowItem) => {
    let friendlyName = win.title.trim();
    if (friendlyName.includes(' - ')) {
      const parts = friendlyName.split(' - ');
      friendlyName = parts[parts.length - 1].trim();
    }
    const finalName = friendlyName || win.processName;
    const targetVal = targetKind === 'process' ? win.processName : win.windowClass;
    setName(finalName);
    setTargetValue(targetVal);
    setJustPicked(true);
    setPickedName(finalName);
    toast.success(t('picker.toastAppSelected', 'Selected application: {{name}}', { name: finalName }), {
      duration: 2000,
      description: t('picker.toastProcessDesc', 'Process: {{proc}}', { proc: win.processName }),
    });
    scheduleJustPickedReset(2500);
  };

  const handleSelectPreset = (preset: AppPreset) => {
    const finalName = resolvePresetName(preset);
    const targetVal = targetKind === 'process' ? preset.processName : (preset.windowClass || preset.processName);
    setName(finalName);
    setTargetValue(targetVal);
    setJustPicked(true);
    setPickedName(finalName);
    toast.success(t('picker.toastPresetSelected', 'Selected preset: {{name}}', { name: finalName }), {
      duration: 2000,
      description: t('picker.toastProcessDesc', 'Process: {{proc}}', { proc: preset.processName }),
    });
    scheduleJustPickedReset(2500);
  };

  const startCountdownPick = () => {
    if (countdownIntervalRef.current !== null) {
      clearInterval(countdownIntervalRef.current);
      countdownIntervalRef.current = null;
    }
    setCountdown(3);
    setJustPicked(false);
    setPickedName(null);
    toast.loading(t('picker.toastCountdownStart', '3s window recognition started, please switch to target window immediately...'), {
      id: 'window-picker-toast',
      duration: 3500,
    });

    countdownIntervalRef.current = setInterval(() => {
      setCountdown((prev) => {
        if (prev === null || prev <= 1) {
          if (countdownIntervalRef.current !== null) {
            clearInterval(countdownIntervalRef.current);
            countdownIntervalRef.current = null;
          }
          if (isMountedRef.current) {
            void triggerPickForeground();
          }
          return null;
        }
        return prev - 1;
      });
    }, 1000);
  };

  const triggerPickForeground = async () => {
    try {
      const res = await bridgeRequest<{ success: boolean; title: string; processName: string; windowClass: string }>('window.getForegroundInfo');
      if (!isMountedRef.current) return;
      if (res && res.success && (res.processName || res.title)) {
        let friendlyName = res.title.trim();
        if (friendlyName.includes(' - ')) {
          const parts = friendlyName.split(' - ');
          friendlyName = parts[parts.length - 1].trim();
        }
        const finalName = friendlyName || res.processName;
        const targetVal = targetKind === 'process' ? res.processName : res.windowClass;
        setName(finalName);
        setTargetValue(targetVal);
        setJustPicked(true);
        setPickedName(finalName);

        toast.success(t('picker.toastWindowIdentified', 'Target window recognized: {{name}}', { name: finalName }), {
          id: 'window-picker-toast',
          duration: 4000,
          description: t('picker.pickToastDesc', 'Process: {{proc}} | Class: {{cls}}', {
            proc: res.processName || '-',
            cls: res.windowClass || '-',
          }),
        });

        scheduleJustPickedReset(3000);
      } else {
        toast.error(t('picker.pickToastNoWindow', 'No active foreground window detected, please retry or select from list below'), {
          id: 'window-picker-toast',
          duration: 3000,
        });
      }
    } catch (err) {
      if (!isMountedRef.current) return;
      toast.error(t('picker.pickToastFail', 'Failed to pick foreground window'), {
        id: 'window-picker-toast',
        description: String(err),
      });
    }
  };

  const nameError = name.trim() ? '' : t('scope.nameRequired', 'Enter a rule name');
  const targetError = targetValue.trim() ? '' : t('scope.targetRequired', 'Enter a process or window class to match');
  const canSave = !nameError && !targetError;

  const filteredPresets = selectedCategory === 'all'
    ? PRESETS
    : PRESETS.filter((p) => p.category === selectedCategory);

  const handleSave = () => {
    if (!canSave) return;
    onAdd({
      name: name.trim(),
      processName: targetKind === 'process' ? targetValue.trim() : '',
      windowClass: targetKind === 'class' ? targetValue.trim() : '',
      effect,
    });
  };

  return (
    <Modal
      open
      title={defaultDisabled ? t('picker.titleAddDisabled', 'Add Do Not Disturb Target (Blacklist)') : t('picker.titleAddApp', 'Add Target Application')}
      onClose={onClose}
      footer={
        <>
          <Button variant="ghost" onClick={onClose}>{t('common.cancel', 'Cancel')}</Button>
          <Button variant="primary" onClick={handleSave} disabled={!canSave}>{t('components.addAndConfigure', 'Add & Configure')}</Button>
        </>
      }
    >
      <div className={`app-picker-grabber-bar ${countdown !== null ? 'is-picking' : ''} ${justPicked ? 'is-success' : ''}`}>
        {countdown !== null && (
          <div className="picker-progress-track">
            <div className="picker-progress-fill" style={{ width: `${((4 - countdown) / 3) * 100}%` }} />
          </div>
        )}
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: '0.83rem', color: 'var(--text-primary)', zIndex: 1 }}>
          {justPicked ? (
            <CheckCircle2 size={16} style={{ color: 'var(--success, #10b981)' }} className="picker-success-icon" />
          ) : (
            <Crosshair size={16} style={{ color: 'var(--primary)' }} className={countdown !== null ? 'picker-radar-spin' : ''} />
          )}
          <span>
            {countdown !== null
              ? t('picker.countdownTip', 'Please switch to target window in {{count}}s...', { count: countdown })
              : justPicked
              ? t('picker.pickedTip', 'Recognized: {{name}}', { name: pickedName || name })
              : t('picker.pickDirectTip', 'Don\'t know the process or class name? Pick it with one click')}
          </span>
        </div>
        <Button
          size="sm"
          variant={justPicked ? 'primary' : 'secondary'}
          onClick={startCountdownPick}
          disabled={countdown !== null}
          style={{ zIndex: 1 }}
        >
          <Crosshair size={13} className={countdown !== null ? 'picker-radar-spin' : ''} />
          <span>{countdown !== null ? t('picker.pickingBtn', '{{count}}s Picking...', { count: countdown }) : justPicked ? t('picker.rePickBtn', 'Re-pick') : t('picker.pickWindowBtn', 'Pick Foreground Window')}</span>
        </Button>
      </div>

      <Tabs
        tabs={[
          { id: 'running', label: t('picker.tabRunning', 'Running Apps ({{count}})', { count: openWindows.length }), icon: <AppWindow size={14} /> },
          { id: 'presets', label: t('picker.tabPresets', 'App Presets'), icon: <Sparkles size={14} /> },
          { id: 'manual', label: t('picker.tabManual', 'Manual Advanced'), icon: <SlidersHorizontal size={14} /> },
        ]}
        activeId={pickerTab}
        onChange={(tab) => {
          setPickerTab(tab);
          if (tab === 'running') void loadOpenWindows();
        }}
        className="app-picker-tabs-uikit"
      />

      {pickerTab === 'running' && (
        <div className="app-picker-grid">
          {openWindows.length > 0 ? (
            openWindows.map((win, idx) => {
              const isSelected = targetValue.toLowerCase() === (targetKind === 'process' ? win.processName : win.windowClass).toLowerCase();
              return (
                <div
                  key={`${win.processName}-${idx}`}
                  className={`app-picker-item ${isSelected ? 'selected' : ''}`}
                  onClick={() => handleSelectRunningApp(win)}
                  title={t('picker.windowCardTip', 'Title: {{title}}\nProcess: {{proc}}\nClass: {{cls}}', { title: win.title, proc: win.processName, cls: win.windowClass })}
                >
                  <div className="app-picker-item__icon-wrap">
                    {isSelected ? <Check size={14} /> : renderVectorAppIcon(win.processName)}
                  </div>
                  <div className="app-picker-item__info">
                    <span className="app-picker-item__name">{win.title || win.processName}</span>
                    <span className="app-picker-item__proc">{targetKind === 'process' ? win.processName : win.windowClass}</span>
                  </div>
                </div>
              );
            })
          ) : (
            <div className="app-picker-empty">
              {loadingWindows ? t('picker.loadingWindows', 'Scanning running windows...') : t('picker.noWindowsFound', 'No open windows detected')}
              <button
                type="button"
                style={{ marginLeft: 8, background: 'none', border: 'none', color: 'var(--primary)', cursor: 'pointer' }}
                onClick={loadOpenWindows}
              >
                <RefreshCw size={12} /> {t('common.refresh', 'Refresh')}
              </button>
            </div>
          )}
        </div>
      )}

      {pickerTab === 'presets' && (
        <>
          <Tabs
            tabs={[
              { id: 'all', label: t('picker.catAll', 'All'), icon: <LayoutGrid size={12} /> },
              { id: 'browser', label: t('picker.catBrowser', 'Browser'), icon: <Globe size={12} /> },
              { id: 'dev', label: t('picker.catDev', 'Dev'), icon: <Code2 size={12} /> },
              { id: 'office', label: t('picker.catOffice', 'Office'), icon: <FileText size={12} /> },
              { id: 'system', label: t('picker.catSystem', 'System'), icon: <SlidersHorizontal size={12} /> },
            ]}
            activeId={selectedCategory}
            onChange={(cat) => setSelectedCategory(cat)}
            className="app-picker-subtabs-uikit"
          />
          <div className="app-picker-grid">
            {filteredPresets.map((preset) => {
              const displayName = resolvePresetName(preset);
              const presetTarget = targetKind === 'process'
                ? preset.processName
                : (preset.windowClass || preset.processName);
              const isSelected = targetValue.toLowerCase() === presetTarget.toLowerCase();
              return (
                <div
                  key={preset.name}
                  className={`app-picker-item ${isSelected ? 'selected' : ''}`}
                  onClick={() => handleSelectPreset(preset)}
                  title={`${displayName} (${preset.processName})`}
                >
                  <div className="app-picker-item__icon-wrap">
                    {isSelected ? <Check size={14} /> : renderVectorAppIcon(preset.processName)}
                  </div>
                  <div className="app-picker-item__info">
                    <span className="app-picker-item__name">{displayName}</span>
                    <span className="app-picker-item__proc">{presetTarget}</span>
                  </div>
                </div>
              );
            })}
          </div>
        </>
      )}

      <div className={`app-picker-form ${justPicked ? 'just-picked-glow' : ''}`}>
        <Field label={t('scope.ruleName', 'Rule Name')} error={nameError}>
          <TextInput
            value={name}
            onChange={setName}
            placeholder={t('components.appPlaceholder', 'e.g. Google Chrome / VS Code / Game')}
          />
        </Field>

        <Field label={t('scope.matchTarget', 'Match Target')}>
          <Tabs
            tabs={[
              { id: 'process', label: t('picker.kindProcess', 'By Process Name (Recommended, e.g. chrome.exe)'), icon: <Cpu size={14} /> },
              { id: 'class', label: t('picker.kindClass', 'By Window Class Name (e.g. Chrome_WidgetWin_1)'), icon: <Layers size={14} /> },
            ]}
            activeId={targetKind}
            onChange={(kind) => setTargetKind(kind as TargetKind)}
            className="app-picker-kind-tabs-uikit"
          />
        </Field>

        <Field
          label={targetKind === 'process' ? t('picker.fieldProcess', 'Process Name') : t('picker.fieldClass', 'Window Class Name')}
          error={targetError}
          hint={targetKind === 'process' ? t('picker.hintProcess', 'Click from list above or enter manually (e.g. chrome.exe)') : t('picker.hintClass', 'Windows ClassName of target window')}
        >
          <TextInput
            value={targetValue}
            onChange={setTargetValue}
            placeholder={targetKind === 'process' ? 'chrome.exe' : 'Chrome_WidgetWin_1'}
          />
        </Field>
      </div>

      <div className="uikit-field">
        <label className="uikit-field__label">{t('components.scopeStrategyLabel', 'Scope Strategy')}</label>
        <div className="picker-strategy-cards">
          <div
            className={`picker-strategy-card ${effect === 2 ? 'active' : ''}`}
            onClick={() => setEffect(2)}
          >
            <div className="picker-strategy-card__icon">
              <Sparkles size={16} />
            </div>
            <div className="picker-strategy-card__content">
              <div className="picker-strategy-card__title">{t('components.customIndependentTitle', 'Custom Independent Gestures')}</div>
              <div className="picker-strategy-card__desc">{t('components.customIndependentDesc', 'Override or extend global gestures')}</div>
            </div>
            {effect === 2 && (
              <div className="picker-strategy-card__check">
                <Check size={11} />
              </div>
            )}
          </div>

          <div
            className={`picker-strategy-card ${effect === 1 ? 'active' : ''}`}
            onClick={() => setEffect(1)}
          >
            <div className="picker-strategy-card__icon ban-icon">
              <Ban size={16} />
            </div>
            <div className="picker-strategy-card__content">
              <div className="picker-strategy-card__title">{t('components.disableGestureTitle', 'Disable Gestures (Do Not Disturb)')}</div>
              <div className="picker-strategy-card__desc">{t('components.disableGestureDesc', 'Completely disable gestures in this app')}</div>
            </div>
            {effect === 1 && (
              <div className="picker-strategy-card__check">
                <Check size={11} />
              </div>
            )}
          </div>
        </div>
      </div>
    </Modal>
  );
};

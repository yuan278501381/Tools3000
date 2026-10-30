/* ─────────────────────────────────────────────────────────────────────────────
 * ScopeRuleModal — 新增 / 编辑作用域规则 (世界级现代设计系统: 矢量应用微标 + 运行列表 + 预设库 + 倒计时拾取)
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useRef, type FC } from 'react';
import { Modal, Field, Button, Select, TextInput, Tabs } from './UIKit';
import {
  type ScopeRule,
  MATCH_MODE_KEYS,
  emptyRule,
} from './scopeModel';
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
  category: 'browser' | 'dev' | 'office' | 'system';
  categoryLabel: string;
  processName: string;
  windowClass?: string;
}

const PRESETS: AppPreset[] = [
  // 浏览器
  { name: 'Google Chrome', category: 'browser', categoryLabel: 'Browser', processName: 'chrome.exe', windowClass: 'Chrome_WidgetWin_1' },
  { name: 'Microsoft Edge', category: 'browser', categoryLabel: 'Browser', processName: 'msedge.exe', windowClass: 'Chrome_WidgetWin_1' },
  { name: 'Mozilla Firefox', category: 'browser', categoryLabel: 'Browser', processName: 'firefox.exe', windowClass: 'MozillaWindowClass' },
  { name: '360 Extreme Browser', nameKey: 'components.app360Browser', category: 'browser', categoryLabel: 'Browser', processName: '360chrome.exe' },

  // 开发与设计
  { name: 'VS Code', category: 'dev', categoryLabel: 'Dev & Design', processName: 'Code.exe', windowClass: 'Chrome_WidgetWin_1' },
  { name: 'Visual Studio', category: 'dev', categoryLabel: 'Dev & Design', processName: 'devenv.exe' },
  { name: 'Windows Terminal', category: 'dev', categoryLabel: 'Dev & Design', processName: 'WindowsTerminal.exe' },
  { name: 'IntelliJ IDEA', category: 'dev', categoryLabel: 'Dev & Design', processName: 'idea64.exe' },
  { name: 'Adobe Photoshop', category: 'dev', categoryLabel: 'Dev & Design', processName: 'Photoshop.exe' },
  { name: 'AutoCAD', category: 'dev', categoryLabel: 'Dev & Design', processName: 'acad.exe' },

  // 办公协同
  { name: 'Microsoft Word', category: 'office', categoryLabel: 'Office & Collab', processName: 'WINWORD.EXE' },
  { name: 'Microsoft Excel', category: 'office', categoryLabel: 'Office & Collab', processName: 'EXCEL.EXE' },
  { name: 'Microsoft PowerPoint', category: 'office', categoryLabel: 'Office & Collab', processName: 'POWERPNT.EXE' },
  { name: 'WPS Office', category: 'office', categoryLabel: 'Office & Collab', processName: 'wps.exe' },
  { name: 'WeChat', nameKey: 'picker.appWeChat', category: 'office', categoryLabel: 'Office & Collab', processName: 'WeChat.exe', windowClass: 'WeChatMainWndForPC' },
  { name: 'Feishu', nameKey: 'picker.appFeishu', category: 'office', categoryLabel: 'Office & Collab', processName: 'Feishu.exe' },
  { name: 'DingTalk', nameKey: 'picker.appDingTalk', category: 'office', categoryLabel: 'Office & Collab', processName: 'DingTalk.exe' },
  { name: 'QQ', category: 'office', categoryLabel: 'Office & Collab', processName: 'QQ.exe' },

  // 系统工具
  { name: 'File Explorer', nameKey: 'picker.appExplorer', category: 'system', categoryLabel: 'System Tools', processName: 'explorer.exe', windowClass: 'CabinetWClass' },
  { name: 'Notepad', nameKey: 'picker.appNotepad', category: 'system', categoryLabel: 'System Tools', processName: 'notepad.exe', windowClass: 'Notepad' },
  { name: 'Task Manager', nameKey: 'picker.appTaskmgr', category: 'system', categoryLabel: 'System Tools', processName: 'Taskmgr.exe' },
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

interface Props {
  initial: ScopeRule | null;
  /** 可选的配置集名称, 供 effect===使用配置集 时选择 */
  profileNames: string[];
  onSave: (rule: ScopeRule) => void;
  onClose: () => void;
}

export const ScopeRuleModal: FC<Props> = ({ initial, profileNames, onSave, onClose }) => {
  const { t } = useTranslation();
  const [draft, setDraft] = useState<ScopeRule>(() => (initial ? structuredClone(initial) : emptyRule()));
  const [targetKind, setTargetKind] = useState<TargetKind>(
    initial && initial.windowClass && !initial.processName ? 'class' : 'process',
  );
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

  const isEdit = initial !== null;
  const set = (patch: Partial<ScopeRule>) => setDraft((d) => ({ ...d, ...patch }));
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

  const targetValue = targetKind === 'process' ? draft.processName : draft.windowClass;
  const setTargetValue = (v: string) =>
    set(targetKind === 'process' ? { processName: v, windowClass: '' }
                                 : { windowClass: v, processName: '' });

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
    if (!draft.name || draft.name.startsWith('e.g.') || !isEdit) {
      set({ name: finalName });
    }
    if (targetKind === 'process') {
      set({ processName: win.processName, windowClass: '' });
    } else {
      set({ windowClass: win.windowClass, processName: '' });
    }
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
    if (!draft.name || draft.name.startsWith('e.g.') || !isEdit) {
      set({ name: finalName });
    }
    if (targetKind === 'process') {
      set({ processName: preset.processName, windowClass: '' });
    } else {
      set({ windowClass: preset.windowClass || preset.processName, processName: '' });
    }
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
        set({
          name: finalName,
          processName: targetKind === 'process' ? res.processName : '',
          windowClass: targetKind === 'class' ? res.windowClass : '',
        });
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

  const nameError = draft.name.trim() ? '' : t('scope.nameRequired', 'Enter a rule name');
  const targetError = targetValue.trim() ? '' : t('scope.targetRequired', 'Enter a process or window class to match');
  const canSave = !nameError && !targetError;

  const filteredPresets = selectedCategory === 'all'
    ? PRESETS
    : PRESETS.filter((p) => p.category === selectedCategory);

  const handleSave = () => {
    if (!canSave) return;
    const rule: ScopeRule = {
      ...draft,
      name: draft.name.trim(),
      processName: targetKind === 'process' ? targetValue.trim() : '',
      windowClass: targetKind === 'class' ? targetValue.trim() : '',
      profileName: draft.effect === 2 ? (draft.profileName || (profileNames[0] ?? '')) : '',
    };
    onSave(rule);
  };

  return (
    <Modal
      open
      title={isEdit ? t('scope.editRule', 'Edit Scope Rule') : t('scope.newRule', 'New Scope Rule')}
      onClose={onClose}
      footer={
        <>
          <Button variant="ghost" onClick={onClose}>{t('common.cancel', 'Cancel')}</Button>
          <Button variant="primary" onClick={handleSave} disabled={!canSave}>{t('common.save', 'Save')}</Button>
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
              ? t('picker.pickedTip', 'Recognized: {{name}}', { name: pickedName || draft.name })
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
              const isSelected = targetKind === 'process'
                ? draft.processName.toLowerCase() === win.processName.toLowerCase()
                : draft.windowClass === win.windowClass;
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
              const isSelected = targetKind === 'process'
                ? draft.processName.toLowerCase() === preset.processName.toLowerCase()
                : draft.windowClass.toLowerCase() === presetTarget.toLowerCase();
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
            value={draft.name}
            onChange={(v) => set({ name: v })}
            placeholder={t('scope.ruleNamePlaceholder', 'e.g. Chrome browser')}
          />
        </Field>

        <Field label={t('scope.matchTarget', 'Match Target')}>
          <Tabs
            tabs={[
              { id: 'process', label: t('scope.byProcess', 'Process name'), icon: <Cpu size={14} /> },
              { id: 'class', label: t('scope.byClass', 'Window class'), icon: <Layers size={14} /> },
            ]}
            activeId={targetKind}
            onChange={(kind) => setTargetKind(kind as TargetKind)}
            className="app-picker-kind-tabs-uikit"
          />
        </Field>

        <Field
          label={targetKind === 'process' ? t('scope.processName', 'Process Name') : t('scope.windowClass', 'Window Class')}
          error={targetError}
          hint={targetKind === 'process' ? t('scope.processHint', 'For example chrome.exe; wildcards such as *.exe are supported') : t('scope.classHint', 'For example Chrome_WidgetWin_1')}
        >
          <TextInput
            value={targetValue}
            onChange={setTargetValue}
            placeholder={targetKind === 'process' ? 'chrome.exe' : 'Chrome_WidgetWin_1'}
          />
        </Field>
      </div>

      <Field label={t('scope.matchMode', 'Match Mode')}>
        <Select
          value={String(draft.matchMode)}
          options={MATCH_MODE_KEYS.map((key, index) => ({ value: String(index), label: t(key) }))}
          onChange={(v) => set({ matchMode: Number(v) })}
          portal
        />
      </Field>

      <div className="uikit-field">
        <label className="uikit-field__label">{t('scope.effect', 'Effect')}</label>
        <div className="picker-strategy-cards">
          <div
            className={`picker-strategy-card ${draft.effect === 2 ? 'active' : ''}`}
            onClick={() => set({ effect: 2, profileName: draft.profileName || (profileNames[0] ?? '') })}
          >
            <div className="picker-strategy-card__icon">
              <Sparkles size={16} />
            </div>
            <div className="picker-strategy-card__content">
              <div className="picker-strategy-card__title">{t('scope.effectProfile', 'Use profile')}</div>
              <div className="picker-strategy-card__desc">{t('scope.profileHint', 'Switch to this profile when the window matches')}</div>
            </div>
            {draft.effect === 2 && (
              <div className="picker-strategy-card__check">
                <Check size={11} />
              </div>
            )}
          </div>

          <div
            className={`picker-strategy-card ${draft.effect === 1 ? 'active' : ''}`}
            onClick={() => set({ effect: 1 })}
          >
            <div className="picker-strategy-card__icon ban-icon">
              <Ban size={16} />
            </div>
            <div className="picker-strategy-card__content">
              <div className="picker-strategy-card__title">{t('scope.effectDisable', 'Disable gestures')}</div>
              <div className="picker-strategy-card__desc">{t('components.disableGesturesInWindow', 'Completely disable mouse gestures in this window')}</div>
            </div>
            {draft.effect === 1 && (
              <div className="picker-strategy-card__check">
                <Check size={11} />
              </div>
            )}
          </div>
        </div>
      </div>

      {draft.effect === 2 && (
        <Field label={t('scope.profile', 'Gesture Profile')} hint={t('scope.profileHint', 'Switch to this profile when the window matches')}>
          <Select
            value={draft.profileName || (profileNames[0] ?? '')}
            options={profileNames.map((n) => ({ value: n, label: n }))}
            onChange={(v) => set({ profileName: v })}
            portal
          />
        </Field>
      )}
    </Modal>
  );
};

/* ─────────────────────────────────────────────────────────────────────────────
 * GesturePage — 鼠标手势设置页 (WGestures 2 风格 Master-Detail 架构)
 *
 * 功能:
 *   - 左侧作用目标树: 全局 / 禁用免打扰组 / 应用程序 / 特殊目标(桌面/任务栏)
 *   - 右上触发管控: 允许的触发方式三态模型 (启用 / 禁用 / 默认继承) + 批量操作
 *   - 右下手势列表: 手势单项启用/禁用、上下调序、删除、即时执行/静默提示
 *   - 轮盘菜单 (RadialMenu) 配置
 *   - 全局开关 / 轨迹流光显示 / 游戏全屏免打扰
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, useMemo, type FC } from 'react';
import { Card, Toggle, SettingRow, SettingGroup, Badge, Select, Button, TextInput } from '../components/UIKit';
import { GestureEditorModal } from '../components/GestureEditorModal';
import { GestureStrokePreview } from '../components/GestureStrokePreview';
import { GestureGuide } from '../components/GestureGuide';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import {
  ScopeTargetsSidebar,
  type ScopeTargetItem,
} from '../components/ScopeTargetsSidebar';
import { TargetAppPickerModal, type AddedTargetResult } from '../components/TargetAppPickerModal';
import {
  type ScopeRule,
  makeRuleId,
} from '../components/scopeModel';
import {
  ACTION_TYPE_KEYS,
  BUILTIN_COMMAND_KEYS,
  TRIGGER_ITEM_DEFINITIONS,
  upsertGestureMapping,
  type GestureMapping,
  type GestureProfileData,
  type TriggerState,
} from '../components/gestureModel';
import { getLocalizedGestureName, getLocalizedGestureDesc } from '../utils/gestureI18n';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  Mouse,
  Hand,
  Edit3,
  Trash2,
  Compass,
  ArrowUp,
  ArrowDown,
  Globe,
  ShieldAlert,
  Monitor,
  LayoutTemplate,
  Code2,
  Plus,
  SlidersHorizontal,
  Folder,
  AppWindow,
  Sparkles,
  Palette,
  Search,
  CheckCircle2,
  Zap,
  VolumeX,
  GripVertical,
} from 'lucide-react';
import './GesturePage.css';

interface RadialMenuItem {
  label: string;
  command: string;
}

interface GestureState {
  enabled: boolean;
  paused: boolean;
  triggerButton: string;
  trailVisible: boolean;
  autoBypassFullscreen?: boolean;
  enableScribbleCancel?: boolean;
  enableInFlightCompass?: boolean;
  targetMode?: 'underPointer' | 'foreground';
  initialTimeoutMs?: number;
  minSegmentDistance?: number;
  trailColorMode?: 'auto' | 'custom';
  trailColor?: string;
  trailWidth?: number;
  trailOutlineWidth?: number;
  elevated?: boolean;
}

const TRAIL_COLOR_PRESETS = [
  { name: 'Purple', hex: '#8B5CF6' },
  { name: 'Cyan', hex: '#06B6D4' },
  { name: 'Gold', hex: '#F59E0B' },
  { name: 'Blue', hex: '#3B82F6' },
  { name: 'Green', hex: '#10B981' },
  { name: 'Sunset Coral', nameKey: 'gesture.colorSunsetCoral', hex: '#F43F5E' },
];

interface OperationResult {
  success: boolean;
  error?: string;
}

export const GesturePage: FC = () => {
  const { t } = useTranslation();
  // 类型安全的 i18n 辅助函数，避免 TS2345 严格类型与 eslint no-explicit-any 冲突
  const tr = useCallback((key: string, options?: Record<string, unknown>): string => {
    return (t as (k: string, opts?: Record<string, unknown>) => string)(key, options);
  }, [t]);

  const [enabled, setEnabled] = useState(true);
  const [trailVisible, setTrailVisible] = useState(true);
  const [autoBypassFullscreen, setAutoBypassFullscreen] = useState(true);
  const [targetMode, setTargetMode] = useState<'underPointer' | 'foreground'>('underPointer');
  const [initialTimeoutMs, setInitialTimeoutMs] = useState(500);
  const [minSegmentDistance, setMinSegmentDistance] = useState(24);
  const [scribbleCancel, setScribbleCancel] = useState(true);
  const [inFlightCompass, setInFlightCompass] = useState(true);
  const [triggerButton, setTriggerButton] = useState('right');
  const [trailColorMode, setTrailColorMode] = useState<'auto' | 'custom'>('auto');
  const [trailColor, setTrailColor] = useState('#3B82F6');
  const [trailWidth, setTrailWidth] = useState(2.5);
  const [trailOutlineWidth, setTrailOutlineWidth] = useState(1.5);
  const [elevated, setElevated] = useState(false);
  
  // Profiles & Rules
  const [profiles, setProfiles] = useState<Record<string, GestureProfileData>>({
    default: { name: 'default', mappings: [], triggerStates: {} }
  });
  const [rules, setRules] = useState<ScopeRule[]>([]);
  const [selectedTarget, setSelectedTarget] = useState<ScopeTargetItem>({
    id: 'global',
    title: t('components.globalTitle', 'Global'),
    subtitle: t('components.defaultGestureSubtitle', 'Default Gesture Config'),
    kind: 'global',
  });

  const [searchQuery, setSearchQuery] = useState('');
  const [loading, setLoading] = useState(true);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [radialItems, setRadialItems] = useState<RadialMenuItem[]>([]);
  const [radialDirty, setRadialDirty] = useState(false);
  const [radialSaving, setRadialSaving] = useState(false);
  const [pauseHotkey, setPauseHotkey] = useState('Ctrl+Alt+Shift+W');

  // Modal states
  const [appPickerOpen, setAppPickerOpen] = useState(false);
  const [appPickerDefaultDisabled, setAppPickerDefaultDisabled] = useState(false);
  const [editorOpen, setEditorOpen] = useState(false);
  const [editingMapping, setEditingMapping] = useState<GestureMapping | null>(null);
  const [editorFocusTarget, setEditorFocusTarget] = useState<'instant' | 'silent' | 'action_type' | 'action_detail' | 'hotkey' | 'lua' | 'builtin' | 'program' | null>(null);

  const getHotkey = (name: string) => hotkeys.find(h => h.name === name);

  const actionDetail = (action: GestureMapping['action']): string => {
    switch (action.type) {
      case 0: return action.keyStroke ?? '';
      case 1: return action.luaScript ? t('gesture.luaScript', 'Lua Script') : '';
      case 2: {
        const key = BUILTIN_COMMAND_KEYS[action.builtinCmd ?? 0];
        return key ? tr(key) : '';
      }
      case 3: return action.programPath ?? '';
      default: return '';
    }
  };

  useBridgeEvent('gesture.stateChanged', (data) => {
    const state = data as Partial<GestureState>;
    if (typeof state.enabled === 'boolean') setEnabled(state.enabled);
  });

  useBridgeEvent('gesture:stateChanged', (data) => {
    const state = data as Partial<GestureState>;
    if (typeof state.enabled === 'boolean') setEnabled(state.enabled);
  });

  useBridgeEvent('plugins:changed', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { id?: string; enabled?: boolean }) : null;
    if (payload?.id === 'gesture' && typeof payload.enabled === 'boolean') {
      setEnabled(payload.enabled);
    }
  });

  useEffect(() => {
    let isMounted = true;
    Promise.all([
      bridgeRequest<GestureState>('gesture.getState'),
      bridgeRequest<GestureProfileData[]>('gesture.getProfiles'),
      bridgeRequest<ScopeRule[]>('gesture.getScopeRules'),
      bridgeRequest<{ items: RadialMenuItem[] }>('radialmenu.getItems'),
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll'),
    ])
      .then(([state, profileList, ruleList, radialRes, hotkeyList]) => {
        if (!isMounted) return;
        setEnabled(state.enabled);
        setTriggerButton(state.triggerButton ?? 'right');
        setTrailVisible(state.trailVisible ?? true);
        setAutoBypassFullscreen(state.autoBypassFullscreen ?? true);
        setScribbleCancel(state.enableScribbleCancel ?? true);
        setInFlightCompass(state.enableInFlightCompass ?? true);
        setTargetMode(state.targetMode === 'foreground' ? 'foreground' : 'underPointer');
        setTrailColorMode(state.trailColorMode ?? 'auto');
        setTrailColor(state.trailColor ?? '#3B82F6');
        setTrailWidth(state.trailWidth ?? 2.5);
        setTrailOutlineWidth(state.trailOutlineWidth ?? 1.5);
        setInitialTimeoutMs(state.initialTimeoutMs ?? 500);
        setMinSegmentDistance(state.minSegmentDistance ?? 24);
        setElevated(state.elevated ?? false);

        const pMap: Record<string, GestureProfileData> = {};
        if (Array.isArray(profileList)) {
          profileList.forEach((p) => {
            pMap[p.name] = {
              name: p.name,
              mappings: Array.isArray(p.mappings) ? p.mappings : [],
              triggerStates: p.triggerStates ?? {},
            };
          });
        }
        if (!pMap.default) pMap.default = { name: 'default', mappings: [], triggerStates: {} };
        setProfiles(pMap);

        const rList = Array.isArray(ruleList) ? ruleList : [];
        setRules(rList);

        if (radialRes?.items) setRadialItems(radialRes.items);
        const list = Array.isArray(hotkeyList) ? hotkeyList : [];
        setHotkeys(list);
        const pauseBinding = list.find((entry) => entry.name === 'Pause Gestures');
        if (pauseBinding) setPauseHotkey(pauseBinding.shortcut);
        setLoading(false);
      })
      .catch((err) => {
        if (!isMounted) return;
        console.error('Failed to load gesture config:', err);
        toast.error(tr('gesture.loadFailed'));
        setLoading(false);
      });

    return () => {
      isMounted = false;
    };
  }, [tr]);

  // ── 获取当前选中 Target 对应的 Profile 名称 ────────────────────────────────
  const getCurrentProfileName = useCallback((): string => {
    if (selectedTarget.kind === 'global') return 'default';
    if (selectedTarget.kind === 'special') {
      return selectedTarget.specialType === 'desktop' ? 'special_desktop' : 'special_taskbar';
    }
    if (selectedTarget.rule?.profileName) return selectedTarget.rule.profileName;
    const sanitized = (selectedTarget.rule?.processName || 'custom').replace(/[^a-zA-Z0-9_]/g, '_').toLowerCase();
    return `app_${sanitized}`;
  }, [selectedTarget]);

  const currentProfileName = getCurrentProfileName();
  const currentProfile = profiles[currentProfileName] ?? (
    selectedTarget.kind === 'global' ? { name: 'default', mappings: [], triggerStates: {} } : (profiles.default ?? { name: 'default', mappings: [], triggerStates: {} })
  );
  const currentMappings: GestureMapping[] = useMemo(() => currentProfile.mappings ?? [], [currentProfile.mappings]);
  const currentTriggerStates: Record<string, TriggerState> = useMemo(() => currentProfile.triggerStates ?? {}, [currentProfile.triggerStates]);

  // ── 持久化当前 Profile 的手势映射 ──────────────────────────────────────────
  const persistMappings = useCallback(async (nextMappings: GestureMapping[]) => {
    const profName = getCurrentProfileName();
    const prevProf = profiles[profName] ?? { name: profName, mappings: [], triggerStates: {} };
    const updatedProf: GestureProfileData = { ...prevProf, mappings: nextMappings };
    setProfiles((prev) => ({ ...prev, [profName]: updatedProf }));

    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateProfile', {
        name: profName,
        mappings: nextMappings,
        triggerStates: updatedProf.triggerStates ?? {},
      });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      console.error('Failed to save gesture profile:', err);
      setProfiles((prev) => ({ ...prev, [profName]: prevProf }));
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  }, [getCurrentProfileName, profiles, tr]);

  // ── 触发方式状态管控 ────────────────────────────────────────────────────────
  const getTriggerState = (key: string): TriggerState => {
    return currentTriggerStates[key] || 'default';
  };

  const handleSetTriggerState = async (triggerKey: string, nextState: TriggerState) => {
    const profName = getCurrentProfileName();
    const nextStates = { ...currentTriggerStates, [triggerKey]: nextState };
    const updatedProf: GestureProfileData = {
      ...(profiles[profName] ?? { name: profName, mappings: [] }),
      triggerStates: nextStates,
    };
    setProfiles((prev) => ({ ...prev, [profName]: updatedProf }));

    try {
      const result = await bridgeRequest<OperationResult>('gesture.setTriggerState', {
        profile: profName,
        trigger: triggerKey,
        state: nextState,
      });
      if (!result.success) throw new Error(result.error || 'Failed to update trigger state');
    } catch (err) {
      console.error('Failed to set trigger state:', err);
      toast.error(t('gesture.toastChangeTriggerFailed', 'Failed to update trigger mode'), { description: String(err) });
    }
  };

  // ── 手势调序与单项开关 ──────────────────────────────────────────────────────
  const [draggedIdx, setDraggedIdx] = useState<number | null>(null);

  const handleDropMapping = async (targetIdx: number) => {
    if (draggedIdx === null || draggedIdx === targetIdx || targetIdx < 0 || targetIdx >= currentMappings.length) {
      setDraggedIdx(null);
      return;
    }
    const nextList = [...currentMappings];
    const [moved] = nextList.splice(draggedIdx, 1);
    nextList.splice(targetIdx, 0, moved);
    setDraggedIdx(null);
    await persistMappings(nextList);
  };

  const handleToggleMappingEnabled = async (index: number) => {
    const nextList = [...currentMappings];
    const item = nextList[index];
    nextList[index] = { ...item, enabled: !(item.enabled ?? true) };
    await persistMappings(nextList);
  };

  // ── 持久化 ScopeRules ───────────────────────────────────────────────────────
  const persistRules = useCallback(async (nextRules: ScopeRule[]) => {
    const prevRules = rules;
    setRules(nextRules);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateScopeRules', { rules: nextRules });
      if (!result.success) throw new Error(result.error || 'Failed to save scope rules');
    } catch (err) {
      console.error('Failed to save scope rules:', err);
      setRules(prevRules);
      toast.error(t('gesture.toastSaveRuleFailed', 'Failed to save scope rule'), { description: String(err) });
    }
  }, [rules, t]);

  // ── 添加 Target (通过 AppPickerModal) ───────────────────────────────────────
  const handleAddTarget = (res: AddedTargetResult) => {
    const profName = res.effect === 2 ? `app_${(res.processName || res.name).replace(/[^a-zA-Z0-9_]/g, '_').toLowerCase()}` : '';
    const newRule: ScopeRule = {
      id: makeRuleId(),
      name: res.name,
      enabled: true,
      processName: res.processName,
      windowClass: res.windowClass,
      matchMode: 0,
      effect: res.effect,
      profileName: profName,
    };

    const nextRules = [...rules, newRule];
    void persistRules(nextRules);

    // 如果是自定义手势，初始化该 profile
    if (res.effect === 2 && profName && !profiles[profName]) {
      const initialMappings = profiles.default?.mappings ? structuredClone(profiles.default.mappings) : [];
      setProfiles((p) => ({ ...p, [profName]: { name: profName, mappings: initialMappings, triggerStates: {} } }));
      void bridgeRequest('gesture.updateProfile', { name: profName, mappings: initialMappings, triggerStates: {} });
    }

    setAppPickerOpen(false);
    setSelectedTarget({
      id: `rule:${newRule.id}`,
      title: newRule.name || newRule.processName,
      subtitle: newRule.processName || newRule.windowClass,
      kind: res.effect === 1 ? 'disabled' : 'app',
      rule: newRule,
    });
    toast.success(t('gesture.toastTargetAdded', 'Added target: {{name}}', { name: res.name }));
  };

  // ── 删除 Target ─────────────────────────────────────────────────────────────
  const handleDeleteTarget = (ruleId: string) => {
    const r = rules.find((x) => x.id === ruleId);
    if (!r) return;
    if (!window.confirm(t('gesture.confirmRemoveTarget', 'Are you sure you want to remove the custom configuration for target 「{{name}}」?', { name: r.name || r.processName }))) return;

    const nextRules = rules.filter((x) => x.id !== ruleId);
    void persistRules(nextRules);

    if (selectedTarget.id === `rule:${ruleId}`) {
      setSelectedTarget({
        id: 'global',
        title: t('components.globalTitle', 'Global'),
        subtitle: t('components.defaultGestureSubtitle', 'Default Gesture Config'),
        kind: 'global',
      });
    }
    toast.success(t('gesture.toastTargetRemoved', 'Target configuration removed'));
  };

  // ── 切换当前 Target 的作用策略 (自定义手势 <-> 禁用手势) ────────────────────
  const handleToggleStrategy = (newEffect: number) => {
    if (!selectedTarget.rule) return;
    const ruleId = selectedTarget.rule.id;
    const profName = newEffect === 2
      ? (selectedTarget.rule.profileName || `app_${(selectedTarget.rule.processName || 'custom').replace(/[^a-zA-Z0-9_]/g, '_').toLowerCase()}`)
      : '';

    const nextRules = rules.map((r) => {
      if (r.id === ruleId) {
        return { ...r, effect: newEffect, profileName: profName };
      }
      return r;
    });

    void persistRules(nextRules);

    if (newEffect === 2 && profName && !profiles[profName]) {
      const initMaps = profiles.default?.mappings ? structuredClone(profiles.default.mappings) : [];
      setProfiles((p) => ({ ...p, [profName]: { name: profName, mappings: initMaps, triggerStates: {} } }));
      void bridgeRequest('gesture.updateProfile', { name: profName, mappings: initMaps, triggerStates: {} });
    }

    const updatedRule = nextRules.find((r) => r.id === ruleId);
    setSelectedTarget((prev) => ({
      ...prev,
      kind: newEffect === 1 ? 'disabled' : 'app',
      rule: updatedRule,
    }));
  };

  // ── 手势映射 CRUD ───────────────────────────────────────────────────────────
  const openAddMapping = () => {
    setEditingMapping(null);
    setEditorFocusTarget(null);
    setEditorOpen(true);
  };

  const openEditMapping = (
    m: GestureMapping,
    focusTarget: 'instant' | 'silent' | 'action_type' | 'action_detail' | 'hotkey' | 'lua' | 'builtin' | 'program' | null = null
  ) => {
    setEditingMapping(m);
    setEditorFocusTarget(focusTarget);
    setEditorOpen(true);
  };

  const handleSaveMapping = (saved: GestureMapping) => {
    void persistMappings(upsertGestureMapping(currentMappings, saved, editingMapping));
    setEditorOpen(false);
  };

  const handleDeleteMapping = (index: number) => {
    const m = currentMappings[index];
    if (!m) return;
    if (!window.confirm(tr('gesture.deleteConfirm', { name: getLocalizedGestureName(m.action.name, t), code: m.gestureCode }))) return;
    void persistMappings(currentMappings.filter((_, i) => i !== index));
  };

  // ── 触发与全局配置变更 ──────────────────────────────────────────────────────
  const handleToggleEnabled = async (checked: boolean) => {
    setEnabled(checked);
    try {
      const [result] = await Promise.all([
        bridgeRequest<OperationResult>('gesture.setPaused', { paused: !checked }),
        bridgeRequest('plugins.setEnabled', { id: 'gesture', enabled: checked }).catch(() => {}),
      ]);
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setEnabled(!checked);
      console.error('Failed to update gesture enabled state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleToggleTrail = async (checked: boolean) => {
    setTrailVisible(checked);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailVisible: checked });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setTrailVisible(!checked);
      console.error('Failed to update gesture trail state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleToggleTrailColorMode = async (mode: 'auto' | 'custom') => {
    setTrailColorMode(mode);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailColorMode: mode });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      console.error('Failed to update trail color mode:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleTrailColorChange = async (color: string) => {
    setTrailColor(color);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailColor: color });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      console.error('Failed to update trail color:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleTrailWidthChange = async (widthStr: string) => {
    const w = parseFloat(widthStr) || 4.0;
    setTrailWidth(w);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailWidth: w });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      console.error('Failed to update trail width:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleTrailOutlineWidthChange = async (widthStr: string) => {
    const w = parseFloat(widthStr);
    const outline = Number.isFinite(w) ? w : 2.5;
    setTrailOutlineWidth(outline);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailOutlineWidth: outline });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      console.error('Failed to update trail outline width:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleToggleAutoBypass = async (checked: boolean) => {
    setAutoBypassFullscreen(checked);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { autoBypassFullscreen: checked });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setAutoBypassFullscreen(!checked);
      console.error('Failed to update gesture autoBypassFullscreen state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleInitialTimeoutChange = async (val: number) => {
    const clamped = Math.max(100, Math.min(3000, Math.round(val)));
    setInitialTimeoutMs(clamped);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { initialTimeoutMs: clamped });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      console.error('Failed to update gesture initialTimeoutMs:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleMinSegmentDistanceChange = async (val: number) => {
    const clamped = Math.max(5, Math.min(100, Math.round(val)));
    setMinSegmentDistance(clamped);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { minSegmentDistance: clamped });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      console.error('Failed to update gesture minSegmentDistance:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleTargetModeChange = async (value: string) => {
    const next = value === 'foreground' ? 'foreground' : 'underPointer';
    const previous = targetMode;
    setTargetMode(next);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { targetMode: next });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setTargetMode(previous);
      console.error('Failed to update gesture target mode:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleToggleScribbleCancel = async (checked: boolean) => {
    setScribbleCancel(checked);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { enableScribbleCancel: checked });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setScribbleCancel(!checked);
      console.error('Failed to update scribble cancel state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleToggleInFlightCompass = async (checked: boolean) => {
    setInFlightCompass(checked);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { enableInFlightCompass: checked });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setInFlightCompass(!checked);
      console.error('Failed to update in-flight compass state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleTriggerChange = async (value: string) => {
    const previous = triggerButton;
    setTriggerButton(value);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { triggerButton: value });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setTriggerButton(previous);
      console.error('Failed to update gesture trigger button:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  // ── 轮盘菜单逻辑 ────────────────────────────────────────────────────────────
  const radialCommandIndex = (command: string): number => {
    const legacy: Record<string, number> = { capture: 10, search: 16, lock: 8, pin: 18, record: 11 };
    if (command in legacy) return legacy[command];
    const parsed = Number(command);
    return Number.isInteger(parsed) && parsed >= 0 && parsed < BUILTIN_COMMAND_KEYS.length ? parsed : 10;
  };

  const updateRadialItem = (index: number, patch: Partial<RadialMenuItem>) => {
    setRadialItems((items) =>
      items.map((item, itemIndex) => (itemIndex === index ? { ...item, ...patch } : item))
    );
    setRadialDirty(true);
  };

  const moveRadialItem = (index: number, direction: -1 | 1) => {
    const target = index + direction;
    if (target < 0 || target >= radialItems.length) return;
    setRadialItems((items) => {
      const next = [...items];
      [next[index], next[target]] = [next[target], next[index]];
      return next;
    });
    setRadialDirty(true);
  };

  const removeRadialItem = (index: number) => {
    setRadialItems((items) => items.filter((_, itemIndex) => itemIndex !== index));
    setRadialDirty(true);
  };

  const addRadialItem = () => {
    if (radialItems.length >= 8) return;
    setRadialItems((items) => [
      ...items,
      { label: tr('gesture.radialDefaultLabel', { count: items.length + 1 }), command: '10' },
    ]);
    setRadialDirty(true);
  };

  const saveRadialItems = async () => {
    const normalized = radialItems.map((item) => ({ ...item, label: item.label.trim() }));
    if (normalized.some((item) => !item.label)) {
      toast.error(tr('gesture.radialLabelRequired'));
      return;
    }
    setRadialSaving(true);
    try {
      const result = await bridgeRequest<OperationResult>('radialmenu.updateItems', { items: normalized });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
      setRadialItems(normalized);
      setRadialDirty(false);
      toast.success(tr('gesture.radialSaved'));
    } catch (error) {
      toast.error(tr('gesture.saveFailed'), { description: String(error) });
    } finally {
      setRadialSaving(false);
    }
  };

  const [triggerFilter, setTriggerFilter] = useState<'all' | 'right' | 'middle'>('all');

  const rightCount = useMemo(() => currentMappings.filter((m) => !m.gestureCode.trim().toUpperCase().startsWith('MIDDLE+')).length, [currentMappings]);
  const middleCount = useMemo(() => currentMappings.filter((m) => m.gestureCode.trim().toUpperCase().startsWith('MIDDLE+')).length, [currentMappings]);

  const filteredMappings = useMemo(() => {
    return currentMappings.filter((m) => {
      const isMiddle = m.gestureCode.trim().toUpperCase().startsWith('MIDDLE+');
      if (triggerFilter === 'right' && isMiddle) return false;
      if (triggerFilter === 'middle' && !isMiddle) return false;

      if (!searchQuery.trim()) return true;
      const q = searchQuery.toLowerCase();
      const locName = getLocalizedGestureName(m.action.name, t).toLowerCase();
      const locDesc = (getLocalizedGestureDesc(m.action.description, t) || '').toLowerCase();
      return (
        m.gestureCode.toLowerCase().includes(q) ||
        m.action.name.toLowerCase().includes(q) ||
        locName.includes(q) ||
        (m.action.keyStroke || '').toLowerCase().includes(q) ||
        (m.action.description || '').toLowerCase().includes(q) ||
        locDesc.includes(q)
      );
    });
  }, [currentMappings, triggerFilter, searchQuery, t]);

  if (loading) {
    return (
      <div className="page-loading">
        <div className="page-loading__spinner" />
        <span>{tr('common.loading')}</span>
      </div>
    );
  }

  const renderTargetHeaderIcon = () => {
    if (selectedTarget.kind === 'global') return <Globe size={18} />;
    if (selectedTarget.kind === 'disabled') return <ShieldAlert size={18} style={{ color: '#ef4444' }} />;
    if (selectedTarget.specialType === 'desktop') return <Monitor size={18} />;
    if (selectedTarget.specialType === 'taskbar') return <LayoutTemplate size={18} />;
    const p = (selectedTarget.rule?.processName || '').toLowerCase();
    if (p.includes('code') || p.includes('studio')) return <Code2 size={18} />;
    if (p.includes('explorer')) return <Folder size={18} />;
    return <AppWindow size={18} />;
  };

  return (
    <div className="gesture-page" style={{ animation: 'fadeIn 0.3s ease', paddingBottom: '2.5rem' }}>
      {/* ── 顶部全局开关与触发设置 ──────────────────────────────────── */}
      <SettingGroup title={tr('gesture.title')} icon={<Mouse size={20} strokeWidth={2.2} />}>
        <Card>
          <div className={`gesture-status ${enabled ? 'gesture-status--active' : 'gesture-status--paused'}`}>
            <span className="gesture-status__dot" />
            <span className="gesture-status__text">{enabled ? tr('gesture.statusRunning') : tr('gesture.statusPaused')}</span>
            <div style={{ display: 'inline-flex', alignItems: 'center', gap: '6px' }}>
              <kbd className="gesture-status__hotkey">{pauseHotkey}</kbd>
              <HotkeyStatusBadge entry={getHotkey('Pause Gestures')} />
            </div>
          </div>
          <Toggle
            id="gesture-enabled"
            label={tr('gesture.enabled')}
            description={tr('gesture.enabledDesc')}
            checked={enabled}
            onChange={handleToggleEnabled}
          />
          <Toggle
            id="gesture-bypass-fullscreen"
            label={tr('gesture.autoBypassFullscreen')}
            description={tr('gesture.autoBypassFullscreenDesc')}
            checked={autoBypassFullscreen}
            onChange={handleToggleAutoBypass}
          />
          <div className="gesture-admin-hint-card">
            <ShieldAlert size={18} className="gesture-admin-hint-icon" />
            <div className="gesture-admin-hint-content">
              <div className="gesture-admin-hint-title">
                {tr('gesture.adminHintTitle')}
                {elevated && <span className="gesture-admin-badge">{tr('gesture.alreadyElevated')}</span>}
              </div>
              <div className="gesture-admin-hint-desc">
                {elevated
                  ? tr('gesture.adminHintElevatedDesc')
                  : tr('gesture.adminHintDesc')}
              </div>
            </div>
          </div>
          <Toggle
            id="gesture-trail"
            label={tr('gesture.showTrail')}
            description={tr('gesture.showTrailDesc')}
            checked={trailVisible}
            onChange={handleToggleTrail}
          />
          {trailVisible && (
            <div className="gesture-trail-subgroup">
              <SettingRow label={tr('gesture.trailColorMode')} description={tr('gesture.trailColorModeDesc')}>
                <div className="gesture-trail-mode-segmented">
                  <button
                    type="button"
                    className={`gesture-trail-mode-btn ${trailColorMode === 'auto' ? 'active' : ''}`}
                    onClick={() => void handleToggleTrailColorMode('auto')}
                  >
                    <Sparkles size={14} />
                    <span>{tr('gesture.trailColorAuto')}</span>
                  </button>
                  <button
                    type="button"
                    className={`gesture-trail-mode-btn ${trailColorMode === 'custom' ? 'active' : ''}`}
                    onClick={() => void handleToggleTrailColorMode('custom')}
                  >
                    <Palette size={14} />
                    <span>{tr('gesture.trailColorCustom')}</span>
                  </button>
                </div>
              </SettingRow>

              {trailColorMode === 'custom' && (
                <SettingRow label={tr('gesture.trailCustomColor')} description={tr('gesture.trailCustomColorDesc')}>
                  <div className="gesture-color-picker-row">
                    <div className="gesture-color-swatches">
                      {TRAIL_COLOR_PRESETS.map((preset) => (
                        <button
                          key={preset.hex}
                          type="button"
                          className={`gesture-color-swatch ${trailColor.toUpperCase() === preset.hex.toUpperCase() ? 'active' : ''}`}
                          style={{ backgroundColor: preset.hex }}
                          title={preset.name}
                          onClick={() => void handleTrailColorChange(preset.hex)}
                        />
                      ))}
                    </div>
                    <div className="gesture-custom-color-input-wrapper">
                      <input
                        type="color"
                        className="gesture-color-native-input"
                        value={trailColor}
                        onChange={(e) => void handleTrailColorChange(e.target.value)}
                      />
                      <span className="gesture-color-hex-label">{trailColor.toUpperCase()}</span>
                    </div>
                  </div>
                </SettingRow>
              )}

              <SettingRow label={tr('gesture.trailWidth')} description={tr('gesture.trailWidthDesc')}>
                <Select
                  id="gesture-trail-width"
                  value={String(trailWidth)}
                  onChange={handleTrailWidthChange}
                  options={[
                    { value: '2.5', label: tr('gesture.trailWidthFine') },
                    { value: '4', label: tr('gesture.trailWidthStandard') },
                    { value: '6', label: tr('gesture.trailWidthBold') },
                  ]}
                />
              </SettingRow>

              <SettingRow label={tr('gesture.trailOutline')} description={tr('gesture.trailOutlineDesc')}>
                <Select
                  id="gesture-trail-outline"
                  value={String(trailOutlineWidth)}
                  onChange={handleTrailOutlineWidthChange}
                  options={[
                    { value: '0', label: tr('gesture.trailOutlineNone') },
                    { value: '1.5', label: tr('gesture.trailOutlineFine') },
                    { value: '2.5', label: tr('gesture.trailOutlineStandard') },
                    { value: '4', label: tr('gesture.trailOutlineBold') },
                  ]}
                />
              </SettingRow>

              {/* 实时平滑霓虹流光轨迹预览条 */}
              <div className="gesture-trail-preview-card">
                <div className="gesture-trail-preview-label">{t('gesture.trailPreview', 'Gesture Trail Glow Rendering Preview')}</div>
                <svg className="gesture-trail-preview-svg" viewBox="0 0 400 50" preserveAspectRatio="none">
                  {trailOutlineWidth > 0 && (
                    <path
                      d="M 20 25 Q 110 5, 200 25 T 370 25"
                      fill="none"
                      stroke="#FFFFFF"
                      strokeWidth={trailWidth + trailOutlineWidth * 2}
                      strokeOpacity="0.96"
                      strokeLinecap="round"
                    />
                  )}
                  {trailOutlineWidth <= 0 && (
                    <path
                      d="M 20 25 Q 110 5, 200 25 T 370 25"
                      fill="none"
                      stroke={trailColorMode === 'custom' ? trailColor : 'var(--primary)'}
                      strokeWidth={trailWidth * 2.4}
                      strokeOpacity="0.30"
                      strokeLinecap="round"
                    />
                  )}
                  <path
                    d="M 20 25 Q 110 5, 200 25 T 370 25"
                    fill="none"
                    stroke={trailColorMode === 'custom' ? trailColor : 'var(--primary)'}
                    strokeWidth={trailWidth}
                    strokeOpacity="0.95"
                    strokeLinecap="round"
                  />
                  {trailOutlineWidth > 0 && (
                    <circle
                      cx="370"
                      cy="25"
                      r={trailWidth * 0.9 + trailOutlineWidth}
                      fill="#FFFFFF"
                      fillOpacity="0.96"
                    />
                  )}
                  <circle
                    cx="370"
                    cy="25"
                    r={trailWidth * 1.8}
                    fill={trailColorMode === 'custom' ? trailColor : 'var(--primary)'}
                    fillOpacity="0.4"
                  />
                  <circle
                    cx="370"
                    cy="25"
                    r={trailWidth * 0.9}
                    fill={trailColorMode === 'custom' ? trailColor : 'var(--primary)'}
                    fillOpacity="1"
                  />
                  <circle
                    cx="370"
                    cy="25"
                    r={trailWidth * 0.4}
                    fill="#FFFFFF"
                    fillOpacity="0.9"
                  />
                </svg>
              </div>
            </div>
          )}
          <SettingRow label={tr('gesture.initialTimeout')} description={tr('gesture.initialTimeoutDesc')}>
            <Select
              id="gesture-initial-timeout"
              value={String(initialTimeoutMs)}
              onChange={(val) => void handleInitialTimeoutChange(Number(val))}
              options={[
                { value: '300', label: tr('gesture.initialTimeoutFast') },
                { value: '500', label: tr('gesture.initialTimeoutStandard') },
                { value: '800', label: tr('gesture.initialTimeoutRelaxed') },
                { value: '1200', label: tr('gesture.initialTimeoutGenerous') },
                { value: '2000', label: tr('gesture.initialTimeoutLong') },
                ...(![300, 500, 800, 1200, 2000].includes(initialTimeoutMs)
                  ? [{ value: String(initialTimeoutMs), label: t('gesture.customMsUnit', '{{ms}} ms (Custom)', { ms: initialTimeoutMs }) }]
                  : []),
              ]}
            />
          </SettingRow>
          <SettingRow label={tr('gesture.minSegmentDistance')} description={tr('gesture.minSegmentDistanceDesc')}>
            <Select
              id="gesture-min-segment-dist"
              value={String(minSegmentDistance)}
              onChange={(val) => void handleMinSegmentDistanceChange(Number(val))}
              options={[
                { value: '12', label: tr('gesture.minSegmentDistanceUltra') },
                { value: '18', label: tr('gesture.minSegmentDistanceSensitive') },
                { value: '24', label: tr('gesture.minSegmentDistanceStandard') },
                { value: '32', label: tr('gesture.minSegmentDistanceAntiShake') },
                { value: '48', label: tr('gesture.minSegmentDistanceLarge') },
                ...(![12, 18, 24, 32, 48].includes(minSegmentDistance)
                  ? [{ value: String(minSegmentDistance), label: t('gesture.customPxUnit', '{{px}} px (Custom)', { px: minSegmentDistance }) }]
                  : []),
              ]}
            />
          </SettingRow>
          <SettingRow label={tr('gesture.targetMode')} description={tr('gesture.targetModeDesc')}>
            <Select
              id="gesture-target-mode"
              value={targetMode}
              onChange={handleTargetModeChange}
              options={[
                { value: 'underPointer', label: tr('gesture.targetModeUnderPointer') },
                { value: 'foreground', label: tr('gesture.targetModeForeground') },
              ]}
            />
          </SettingRow>
          <Toggle
            id="gesture-scribble-cancel"
            label={tr('gesture.scribbleCancel')}
            description={tr('gesture.scribbleCancelDesc')}
            checked={scribbleCancel}
            onChange={handleToggleScribbleCancel}
          />
          <Toggle
            id="gesture-inflight-compass"
            label={tr('gesture.inFlightCompass')}
            description={tr('gesture.inFlightCompassDesc')}
            checked={inFlightCompass}
            onChange={handleToggleInFlightCompass}
          />
          <SettingRow label={tr('gesture.triggerButton')} description={tr('gesture.triggerButtonDesc')}>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '8px', width: '100%', maxWidth: '380px' }}>
              <Select
                id="gesture-trigger"
                value={triggerButton}
                onChange={handleTriggerChange}
                options={[
                  { value: 'both', label: tr('gesture.btnBoth') },
                  { value: 'right', label: tr('gesture.btnRight') },
                  { value: 'middle', label: tr('gesture.btnMiddle') },
                ]}
              />
              <div style={{ fontSize: 'var(--text-xs, 0.84rem)', color: 'var(--text-muted)', lineHeight: '1.4' }}>
                {tr('gesture.triggerAdaptiveHint')}
              </div>
            </div>
          </SettingRow>

          <GestureGuide />
        </Card>
      </SettingGroup>

      {/* ── WGestures 2 风格作用目标与手势管理 (Master-Detail) ─────── */}
      <SettingGroup title={t('gesture.targetConfigTitle', 'Scope Targets & Gesture Configuration')} icon={<Hand size={20} strokeWidth={2.5} />}>
        <div className="gesture-master-detail-layout">
          {/* 左侧目标导航树 */}
          <ScopeTargetsSidebar
            selectedId={selectedTarget.id}
            rules={rules}
            onSelect={setSelectedTarget}
            onAddApp={() => { setAppPickerDefaultDisabled(false); setAppPickerOpen(true); }}
            onAddDisabled={() => { setAppPickerDefaultDisabled(true); setAppPickerOpen(true); }}
            onDeleteRule={handleDeleteTarget}
          />

          {/* 右侧主配置区域 */}
          <main className="gesture-detail-pane">
            {/* 目标信息卡片 */}
            <div className="target-header-card">
              <div className="target-header-info">
                <div className="target-header-icon">
                  {renderTargetHeaderIcon()}
                </div>
                <div className="target-header-texts">
                  <span className="target-header-title">{selectedTarget.title}</span>
                  <span className="target-header-sub">
                    {selectedTarget.kind === 'global' ? t('gesture.globalScopeSubtitle', 'Global gestures and trigger methods') : (selectedTarget.subtitle || t('gesture.customScopeSubtitle', 'App-specific gesture rules'))}
                  </span>
                </div>
              </div>

              {/* 仅在应用程序 Target 下显示策略切换 */}
              {selectedTarget.kind !== 'global' && selectedTarget.kind !== 'special' && selectedTarget.rule && (
                <div className="target-strategy-segmented">
                  <button
                    type="button"
                    className={`target-strategy-btn ${selectedTarget.rule.effect !== 1 ? 'active' : ''}`}
                    onClick={() => handleToggleStrategy(2)}
                  >
                    <SlidersHorizontal size={13} />
                    <span>{t('gesture.customGestures', 'Custom Gestures')}</span>
                  </button>
                  <button
                    type="button"
                    className={`target-strategy-btn ${selectedTarget.rule.effect === 1 ? 'active' : ''}`}
                    onClick={() => handleToggleStrategy(1)}
                  >
                    <ShieldAlert size={13} />
                    <span>{t('gesture.disabledTarget', 'Disable Gestures (Do Not Disturb)')}</span>
                  </button>
                </div>
              )}
            </div>

            {/* 如果该目标被设置为“禁用手势” */}
            {selectedTarget.kind === 'disabled' || (selectedTarget.rule && selectedTarget.rule.effect === 1) ? (
              <div className="target-disabled-card">
                <div className="target-disabled-icon">
                  <ShieldAlert size={28} />
                </div>
                <span className="target-disabled-title">{t('gesture.targetDisabledNotice', 'Gestures are disabled for this target')}</span>
                <p className="target-disabled-desc">
                  {t('gesture.targetDisabledDesc', 'When this app is in the foreground, mouse events will pass through untouched without triggering gestures.')}
                </p>
                <Button size="sm" variant="secondary" onClick={() => handleToggleStrategy(2)}>
                  {t('gesture.restoreCustomGestures', 'Restore Custom Gestures')}
                </Button>
              </div>
            ) : (
              <>
                {/* ── 允许的触发方式 (极简单行药丸横向条) ── */}
                <div className="trigger-strip-container">
                  <div className="trigger-strip-label">
                    <span className="trigger-strip-title">{t('gesture.triggerModeStrip', 'Trigger Method')}</span>
                    <span className="trigger-strip-hint">
                      {selectedTarget.kind === 'global' ? t('gesture.quickTogglePill', 'Click pill to toggle') : t('gesture.overrideTarget', '(Override Target)')}
                    </span>
                  </div>

                  <div className="trigger-pills-row">
                    {TRIGGER_ITEM_DEFINITIONS.map((item) => {
                      const st = getTriggerState(item.key);
                      const isDefaultEnabled = item.key === 'right';
                      const isEffectiveEnabled = st === 'enabled' || (st === 'default' && isDefaultEnabled);
                      const itemName = item.nameKey ? tr(item.nameKey) : item.name;

                      return (
                        <button
                          key={item.key}
                          type="button"
                          className={`trigger-pill-btn trigger-pill-btn--${isEffectiveEnabled ? 'active' : 'inactive'}`}
                          title={t('gesture.triggerPillTip', '{{name}} ({{cat}}) - Click to {{action}}', { name: itemName, cat: item.category === 'mouse' ? t('gesture.catTrack', 'Mouse Track') : t('gesture.catEdge', 'Screen Edge'), action: isEffectiveEnabled ? t('common.disable', 'Disable') : t('common.enable', 'Enable') })}
                          onClick={() => void handleSetTriggerState(item.key, isEffectiveEnabled ? 'disabled' : 'enabled')}
                        >
                          {isEffectiveEnabled ? <CheckCircle2 size={12} className="trigger-pill-icon" /> : null}
                          <span>{itemName}</span>
                        </button>
                      );
                    })}
                  </div>
                </div>

                {/* ── 手势映射表 (手势列表 + 单项开关 + 调序 + 过滤 + CRUD) ── */}
                <Card>
                  <div className="gesture-toolbar">
                    <div style={{ display: 'flex', alignItems: 'center', gap: '12px', flex: 1, flexWrap: 'wrap' }}>
                      <span className="gesture-toolbar__count">
                        {selectedTarget.kind === 'global' ? t('gesture.globalTableTitle', 'Global Gesture Map') : t('gesture.appTableTitle', '「{{title}}」Gestures', { title: selectedTarget.title })}
                        {' '}({filteredMappings.length}/{currentMappings.length})
                      </span>

                      {/* 触发按键分类快速筛选 */}
                      <div className="gesture-trigger-filter-tabs">
                        <button
                          type="button"
                          className={`gesture-trigger-filter-tab ${triggerFilter === 'all' ? 'active' : ''}`}
                          onClick={() => setTriggerFilter('all')}
                        >
                          {t('common.all', 'All')} ({currentMappings.length})
                        </button>
                        <button
                          type="button"
                          className={`gesture-trigger-filter-tab ${triggerFilter === 'right' ? 'active' : ''}`}
                          onClick={() => setTriggerFilter('right')}
                        >
                          ◐ {t('gesture.rightGestures', 'Right Click')} ({rightCount})
                        </button>
                        <button
                          type="button"
                          className={`gesture-trigger-filter-tab ${triggerFilter === 'middle' ? 'active' : ''}`}
                          onClick={() => setTriggerFilter('middle')}
                        >
                          ◓ {t('gesture.middleGestures', 'Middle Click')} ({middleCount})
                        </button>
                      </div>

                      <div className="gesture-search-box">
                        <Search size={13} className="gesture-search-icon" />
                        <input
                          type="text"
                          className="gesture-search-input"
                          placeholder={t('gesture.filterPlaceholder', 'Filter gesture or action name...')}
                          value={searchQuery}
                          onChange={(e) => setSearchQuery(e.target.value)}
                        />
                      </div>
                    </div>

                    <Button size="sm" variant="primary" onClick={openAddMapping}>
                      <Plus size={14} />
                      <span>{tr('gesture.addMapping')}</span>
                    </Button>
                  </div>

                  <div className="gesture-table-container">
                    <div className="gesture-table">
                      <div className="gesture-table__header">
                        <span className="gesture-table__col gesture-table__col--gesture">{tr('gesture.colGesture')}</span>
                        <span className="gesture-table__col gesture-table__col--action">{tr('gesture.colAction')}</span>
                        <span className="gesture-table__col gesture-table__col--type">{tr('gesture.colType')}</span>
                        <span className="gesture-table__col gesture-table__col--key">{tr('gesture.colDetail')}</span>
                        <span className="gesture-table__col gesture-table__col--switch">{t('gesture.colEnable', 'Enable')}</span>
                        <span className="gesture-table__col gesture-table__col--actions" />
                      </div>

                      {filteredMappings.length === 0 && (
                        <div className="gesture-empty">
                          {searchQuery ? t('gesture.noMatchesFound', 'No matching gestures found') : tr('gesture.emptyMapping')}
                        </div>
                      )}

                      {filteredMappings.map((m: GestureMapping, i: number) => {
                        const actualIdx = currentMappings.findIndex((x) => x.gestureCode === m.gestureCode);
                        const isEnabled = m.enabled ?? true;
                        const isDragging = draggedIdx === actualIdx;
                        return (
                          <div
                            key={m.gestureCode}
                            draggable
                            onDragStart={(e) => {
                              setDraggedIdx(actualIdx);
                              e.dataTransfer.effectAllowed = 'move';
                              e.dataTransfer.setData('text/plain', String(actualIdx));
                            }}
                            onDragOver={(e) => {
                              e.preventDefault();
                              e.dataTransfer.dropEffect = 'move';
                            }}
                            onDragEnd={() => setDraggedIdx(null)}
                            onDrop={(e) => {
                              e.preventDefault();
                              void handleDropMapping(actualIdx);
                            }}
                            onDoubleClick={() => openEditMapping(m)}
                            className={`gesture-table__row ${!isEnabled ? 'gesture-table__row--disabled' : ''} ${isDragging ? 'gesture-table__row--dragging' : ''}`}
                            style={{ animationDelay: `${i * 20}ms` }}
                            title={t('gesture.editTip', 'Double-click to open editor')}
                          >
                            {/* 1. 拖拽抓手 + 动态手势画板 + 触发按键徽章 */}
                            <span className="gesture-table__col gesture-table__col--gesture">
                              <div className="gesture-handle-box">
                                <div
                                  className="gesture-drag-handle"
                                  title={t('gesture.dragOrderTip', 'Hold and drag to reorder gestures')}
                                  onDoubleClick={(e) => e.stopPropagation()}
                                >
                                  <GripVertical size={14} className="gesture-grip-icon" />
                                </div>
                                <div className="gesture-preview-with-badge">
                                  <GestureStrokePreview code={m.gestureCode} width={56} height={34} />
                                  <span
                                    className={`gesture-trigger-mini-badge ${
                                      m.gestureCode.trim().toUpperCase().startsWith('MIDDLE+')
                                        ? 'gesture-trigger-mini-badge--middle'
                                        : 'gesture-trigger-mini-badge--right'
                                    }`}
                                  >
                                    {m.gestureCode.trim().toUpperCase().startsWith('MIDDLE+') ? `◓ ${t('gesture.middleClickShort', 'Middle')}` : `◐ ${t('gesture.rightClickShort', 'Right')}`}
                                  </span>
                                </div>
                              </div>
                            </span>

                            {/* 2. 动作名称与特性徽章 */}
                            <span className="gesture-table__col gesture-table__col--action">
                              <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
                                <span className="gesture-action-name">{getLocalizedGestureName(m.action.name, t)}</span>
                                {m.instantExecute && (
                                  <button
                                    type="button"
                                    className="gesture-flag-badge gesture-flag-badge--instant gesture-flag-badge--clickable"
                                    title={t('gesture.instantTip', 'Instant Execution (click to locate config)')}
                                    onClick={(e) => {
                                      e.stopPropagation();
                                      openEditMapping(m, 'instant');
                                    }}
                                    onDoubleClick={(e) => e.stopPropagation()}
                                  >
                                    <Zap size={10} />
                                    <span>{t('gesture.instantBadge', 'Instant')}</span>
                                  </button>
                                )}
                                {m.silentToast && (
                                  <button
                                    type="button"
                                    className="gesture-flag-badge gesture-flag-badge--silent gesture-flag-badge--clickable"
                                    title={t('gesture.silentTip', 'Silent Mode (click to locate config)')}
                                    onClick={(e) => {
                                      e.stopPropagation();
                                      openEditMapping(m, 'silent');
                                    }}
                                    onDoubleClick={(e) => e.stopPropagation()}
                                  >
                                    <VolumeX size={10} />
                                    <span>{t('gesture.silentBadge', 'Silent')}</span>
                                  </button>
                                )}
                              </div>
                              {m.action.description && (
                                <span className="gesture-action-desc">{getLocalizedGestureDesc(m.action.description, t)}</span>
                              )}
                            </span>

                            {/* 3. 动作类型 */}
                            <span className="gesture-table__col gesture-table__col--type">
                              <button
                                type="button"
                                className="gesture-type-badge-btn"
                                title={t('gesture.actionParamTip', 'Click to configure action type and parameters')}
                                onClick={(e) => {
                                  e.stopPropagation();
                                  openEditMapping(m, 'action_type');
                                }}
                                onDoubleClick={(e) => e.stopPropagation()}
                              >
                                <Badge
                                  text={ACTION_TYPE_KEYS[m.action.type] ? tr(ACTION_TYPE_KEYS[m.action.type]) : tr('common.unknown')}
                                  variant={m.action.type === 0 ? 'primary' : m.action.type === 2 ? 'success' : 'muted'}
                                />
                              </button>
                            </span>

                            {/* 4. 详情按键 */}
                            <span className="gesture-table__col gesture-table__col--key">
                              {actionDetail(m.action) && (() => {
                                const detailText = actionDetail(m.action);
                                const len = detailText.length;
                                const fontSize = len >= 10 ? '0.62rem' : len >= 7 ? '0.67rem' : len >= 5 ? '0.73rem' : '0.78rem';
                                return (
                                  <button
                                    type="button"
                                    className="gesture-detail-badge-btn"
                                    title={t('gesture.editParamTip', 'Click to edit parameter: {{detail}}', { detail: detailText })}
                                    onClick={(e) => {
                                      e.stopPropagation();
                                      openEditMapping(m, 'action_detail');
                                    }}
                                    onDoubleClick={(e) => e.stopPropagation()}
                                  >
                                    <kbd className="gesture-kbd" style={{ fontSize }}>
                                      {detailText}
                                    </kbd>
                                  </button>
                                );
                              })()}
                            </span>

                            {/* 5. 世界级统一微型 3D 胶囊开关 (Unified Micro 3D Toggle) */}
                            <span
                              className="gesture-table__col gesture-table__col--switch"
                              onClick={(e) => e.stopPropagation()}
                              onDoubleClick={(e) => e.stopPropagation()}
                            >
                              <Toggle
                                id={`gesture-mapping-${actualIdx}`}
                                checked={isEnabled}
                                size="sm"
                                variant="primary"
                                onChange={() => void handleToggleMappingEnabled(actualIdx)}
                              />
                            </span>

                            {/* 6. 编辑与删除 */}
                            <span className="gesture-table__col gesture-table__col--actions">
                              <div style={{ display: 'flex', gap: '4px' }}>
                                <button
                                  className="gesture-icon-btn"
                                  title={tr('common.edit')}
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    openEditMapping(m);
                                  }}
                                  onDoubleClick={(e) => e.stopPropagation()}
                                >
                                  <Edit3 size={15} />
                                </button>
                                <button
                                  className="gesture-icon-btn gesture-icon-btn--danger"
                                  title={tr('common.delete')}
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    handleDeleteMapping(actualIdx);
                                  }}
                                  onDoubleClick={(e) => e.stopPropagation()}
                                >
                                  <Trash2 size={15} />
                                </button>
                              </div>
                            </span>
                          </div>
                        );
                      })}
                    </div>
                  </div>
                </Card>
              </>
            )}
          </main>
        </div>
      </SettingGroup>

      {/* ── 轮盘菜单 ──────────────────────────────────────────────── */}
      <SettingGroup title={tr('gesture.radialMenu')} icon={<Compass size={20} strokeWidth={2.5} />}>
        <Card>
          <div className="gesture-radial-toolbar">
            <span>{tr('gesture.radialHint')}</span>
            <div className="gesture-radial-toolbar__actions">
              <Button size="sm" variant="ghost" onClick={addRadialItem} disabled={radialItems.length >= 8}>
                {tr('common.add')}
              </Button>
              <Button
                size="sm"
                variant="primary"
                onClick={() => void saveRadialItems()}
                disabled={!radialDirty || radialSaving}
              >
                {radialSaving ? tr('common.saving') : tr('common.save')}
              </Button>
            </div>
          </div>
          <div className="gesture-radial-list">
            {radialItems.length === 0 && (
              <div className="gesture-empty">{tr('common.empty')}</div>
            )}
            {radialItems.map((item, i) => (
              <div key={i} className="gesture-radial-row">
                <span className="gesture-radial-index">{i + 1}</span>
                <TextInput
                  id={`radial-label-${i}`}
                  value={item.label}
                  onChange={(label) => updateRadialItem(i, { label })}
                  placeholder={tr('gesture.radialLabelPlaceholder')}
                />
                <Select
                  id={`radial-command-${i}`}
                  value={String(radialCommandIndex(item.command))}
                  onChange={(command) => updateRadialItem(i, { command })}
                  options={BUILTIN_COMMAND_KEYS.map((key, commandIndex) => ({
                    value: String(commandIndex),
                    label: tr(key),
                  }))}
                />
                <div className="gesture-radial-actions">
                  <button className="gesture-icon-btn" disabled={i === 0} title={tr('common.moveUp')} onClick={() => moveRadialItem(i, -1)}>
                    <ArrowUp size={15} />
                  </button>
                  <button className="gesture-icon-btn" disabled={i === radialItems.length - 1} title={tr('common.moveDown')} onClick={() => moveRadialItem(i, 1)}>
                    <ArrowDown size={15} />
                  </button>
                  <button className="gesture-icon-btn gesture-icon-btn--danger" title={tr('common.delete')} onClick={() => removeRadialItem(i)}>
                    <Trash2 size={15} />
                  </button>
                </div>
              </div>
            ))}
          </div>
        </Card>
      </SettingGroup>

      {/* ── 弹窗组 ────────────────────────────────────────────────── */}
      {appPickerOpen && (
        <TargetAppPickerModal
          defaultDisabled={appPickerDefaultDisabled}
          onAdd={handleAddTarget}
          onClose={() => setAppPickerOpen(false)}
        />
      )}

      {editorOpen && (
        <GestureEditorModal
          key={editingMapping?.id ?? editingMapping?.gestureCode ?? '__new__'}
          initial={editingMapping}
          initialFocusTarget={editorFocusTarget}
          existingMappings={currentMappings}
          existingCodes={currentMappings.map((m) => m.gestureCode)}
          onSave={handleSaveMapping}
          onClose={() => setEditorOpen(false)}
        />
      )}
    </div>
  );
};

/* ─────────────────────────────────────────────────────────────────────────────
 * GesturePage — 鼠标手势设置页 (顶栏 3 页签分流 + 首屏直达 Master-Detail 手势工作台)
 *
 * 功能:
 *   - rules 页签 (首屏): 紧凑主控卡片 + 左右双栏作用目标与手势配置工作台 + 划词默认搜索引擎
 *   - appearance 页签: 手势轨迹色彩/宽度/描边/渐隐动效与实时 SVG 预览 + 9 方向轮播教学
 *   - algorithm 页签: 防误触与中途反悔机制 + 几何识别引擎参数
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, useMemo, useRef, type FC } from 'react';
import {
  Card,
  Toggle,
  SettingRow,
  SettingGroup,
  SubSettingGroup,
  Tabs,
  Badge,
  CodeBadge,
  Select,
  Button,
  ColorPickerInput,
  ConfirmModal,
  PageSkeleton,
  type TabItem,
} from '../components/UIKit';
import {
  consumePendingDeepNavigation,
  type GesturePageSubTab,
  type DeepNavigationPayload,
} from '../config/settingsSearchIndex';
import { GestureEditorModal, type GestureSaveOptions } from '../components/GestureEditorModal';
import { GestureStrokePreview } from '../components/GestureStrokePreview';
import { MouseButtonSvg, ScreenEdgeSvg, TriggerItemSvg } from '../components/MouseButtonSvg';
import { GestureGuide } from '../components/GestureGuide';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
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
  parseGestureCode,
  matchesGestureCategory,
  resolveRequiredEdgeTriggerKey,
  simplifyDirections,
  upsertGestureMapping,
  type Direction,
  type GestureFilterCategory,
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
  ChevronDown,
} from 'lucide-react';
import './GesturePage.css';

interface ProRightClickPresetApp {
  processName: string;
  displayName: string;
  category: 'cad' | 'eda' | 'dcc3d' | string;
}

interface GestureState {
  enabled: boolean;
  paused: boolean;
  triggerButton: string;
  trailVisible: boolean;
  trailFadeEnabled?: boolean;
  showActionHint?: boolean;
  autoBypassFullscreen?: boolean;
  autoBypassProRightClickApps?: boolean;
  proRightClickPresetApps?: ProRightClickPresetApp[];
  enableScribbleCancel?: boolean;
  pauseCancelEnabled?: boolean;
  pauseCancelMs?: number;
  targetMode?: 'underPointer' | 'foreground';
  initialTimeoutMs?: number;
  minSegmentDistance?: number;
  shapeMatchThreshold?: number;
  trailColorMode?: 'auto' | 'custom';
  trailColor?: string;
  trailWidth?: number;
  trailOutlineWidth?: number;
  webSearchEngine?: string;
  elevated?: boolean;
}

const VALID_GESTURE_TABS: ReadonlySet<string> = new Set<GesturePageSubTab>([
  'rules',
  'appearance',
  'algorithm',
]);

function resolveGestureTab(raw?: string): GesturePageSubTab | null {
  return raw && VALID_GESTURE_TABS.has(raw) ? (raw as GesturePageSubTab) : null;
}

const TRAIL_COLOR_PRESETS = [
  { name: 'Purple', nameKey: 'gesture.colorVioletPurple', hex: '#8B5CF6' },
  { name: 'Cyan', nameKey: 'gesture.colorNeonCyan', hex: '#06B6D4' },
  { name: 'Gold', nameKey: 'gesture.colorAmberGold', hex: '#F59E0B' },
  { name: 'Blue', nameKey: 'gesture.colorSkyBlue', hex: '#3B82F6' },
  { name: 'Green', nameKey: 'gesture.colorEmeraldGreen', hex: '#10B981' },
  { name: 'Sunset Coral', nameKey: 'gesture.colorSunsetCoral', hex: '#F43F5E' },
];

interface OperationResult {
  success: boolean;
  error?: string;
  shortcut?: string;
}

const DIRECTION_ALIAS_MAP: Record<string, Direction> = {
  U: 'U',
  D: 'D',
  L: 'L',
  R: 'R',
  UL: 'UL',
  UR: 'UR',
  DL: 'DL',
  DR: 'DR',
  LU: 'UL',
  RU: 'UR',
  LD: 'DL',
  RD: 'DR',
};

function canonicalizeBareTrack(bareCode: string): string {
  const upper = bareCode.trim().toUpperCase();
  if (!upper || upper === 'WHEELUP' || upper === 'WHEELDOWN') {
    return upper;
  }
  const rawSegments = upper.split('-').filter(Boolean);
  const dirs: Direction[] = [];
  for (const seg of rawSegments) {
    const mapped = DIRECTION_ALIAS_MAP[seg];
    if (!mapped) {
      return upper;
    }
    dirs.push(mapped);
  }
  return simplifyDirections(dirs).join('-');
}

function areSimilarGestureCodes(codeA: string, codeB: string): boolean {
  const a = parseGestureCode(codeA);
  const b = parseGestureCode(codeB);
  if (
    a.edge !== b.edge ||
    a.triggerButton !== b.triggerButton ||
    a.hasCtrl !== b.hasCtrl ||
    a.hasShift !== b.hasShift ||
    a.hasAlt !== b.hasAlt
  ) {
    return false;
  }
  if (!a.bareCode || !b.bareCode) {
    return false;
  }
  return canonicalizeBareTrack(a.bareCode) === canonicalizeBareTrack(b.bareCode);
}

export const GesturePage: FC = () => {
  const { t } = useTranslation();
  // 类型安全的 i18n 辅助函数，避免 TS2345 严格类型与 eslint no-explicit-any 冲突
  const tr = useCallback((key: string, options?: Record<string, unknown>): string => {
    return (t as (k: string, opts?: Record<string, unknown>) => string)(key, options);
  }, [t]);

  const [activeTab, setActiveTab] = useState<GesturePageSubTab>(
    () => resolveGestureTab(consumePendingDeepNavigation('gesture')?.tabId) ?? 'rules',
  );

  const [enabled, setEnabled] = useState(true);
  const [trailVisible, setTrailVisible] = useState(true);
  const [trailFadeEnabled, setTrailFadeEnabled] = useState(true);
  const [showActionHint, setShowActionHint] = useState(true);
  const [autoBypassFullscreen, setAutoBypassFullscreen] = useState(true);
  const [autoBypassProRightClickApps, setAutoBypassProRightClickApps] = useState(true);
  const [proRightClickPresetApps, setProRightClickPresetApps] = useState<ProRightClickPresetApp[]>([]);
  const [targetMode, setTargetMode] = useState<'underPointer' | 'foreground'>('underPointer');
  const [initialTimeoutMs, setInitialTimeoutMs] = useState(0);
  const [minSegmentDistance, setMinSegmentDistance] = useState(24);
  const [shapeMatchThreshold, setShapeMatchThreshold] = useState(75);
  const [scribbleCancel, setScribbleCancel] = useState(true);
  const [pauseCancelEnabled, setPauseCancelEnabled] = useState(true);
  const [pauseCancelMs, setPauseCancelMs] = useState(700);
  const [triggerButton, setTriggerButton] = useState('right');
  const [trailColorMode, setTrailColorMode] = useState<'auto' | 'custom'>('auto');
  const [trailColor, setTrailColor] = useState('#3B82F6');
  const [trailWidth, setTrailWidth] = useState(2.5);
  const [trailOutlineWidth, setTrailOutlineWidth] = useState(1.5);
  const [webSearchEngine, setWebSearchEngine] = useState('bing');
  const [elevated, setElevated] = useState(false);
  const trailColorSeqRef = useRef(0);

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
  const [pauseHotkey, setPauseHotkey] = useState('Ctrl+Alt+Shift+W');

  // Modal & Confirm states
  const [appPickerOpen, setAppPickerOpen] = useState(false);
  const [appPickerDefaultDisabled, setAppPickerDefaultDisabled] = useState(false);
  const [editorOpen, setEditorOpen] = useState(false);
  const [editingMapping, setEditingMapping] = useState<GestureMapping | null>(null);
  const [editorFocusTarget, setEditorFocusTarget] = useState<'instant' | 'silent' | 'action_type' | 'action_detail' | 'hotkey' | 'lua' | 'builtin' | 'program' | null>(null);
  const [confirmDialog, setConfirmDialog] = useState<
    | { kind: 'target'; ruleId: string; name: string }
    | { kind: 'mapping'; index: number; name: string; code: string }
    | null
  >(null);

  useEffect(() => {
    const onDeepNavigate = (e: Event) => {
      const detail = (e as CustomEvent<DeepNavigationPayload>).detail;
      if (detail?.navId === 'gesture' && detail.tabId) {
        consumePendingDeepNavigation('gesture');
        const nextTab = resolveGestureTab(detail.tabId);
        if (nextTab) {
          setActiveTab(nextTab);
        }
      }
    };
    window.addEventListener('tools3000:navigate-deep', onDeepNavigate);
    return () => window.removeEventListener('tools3000:navigate-deep', onDeepNavigate);
  }, []);

  const pauseHotkeyEntry = useMemo(
    () =>
      hotkeys.find((h) => h.name === 'Toggle Gestures') ??
      hotkeys.find((h) => h.name === 'Pause Gestures'),
    [hotkeys],
  );

  const actionDetail = (action: GestureMapping['action']): string => {
    switch (action.type) {
      case 0: return action.keyStroke ?? '';
      case 1: return action.luaScript ? t('gesture.luaScript', 'Lua Script') : '';
      case 2: {
        const key = BUILTIN_COMMAND_KEYS[action.builtinCmd ?? 0];
        return key ? tr(key) : '';
      }
      case 3: {
        const rawPath = (action.programPath ?? '').trim();
        if (!rawPath) return '';
        const stripped = rawPath.replace(/^["']|["']$/g, '');
        const parts = stripped.split(/[\\/]/).filter(Boolean);
        const exeName = parts.length > 0 ? parts[parts.length - 1] : stripped;
        const mode = action.runMode ?? 'always_new';
        if (mode === 'activate_or_run') {
          return `[${t('gesture.runModeShortActivate', 'Activate')}] ${exeName}`;
        }
        if (mode === 'toggle_or_run') {
          return `[${t('gesture.runModeShortToggle', 'Toggle')}] ${exeName}`;
        }
        return exeName;
      }
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
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll'),
      bridgeRequest<string>('config.get', { key: '/gesture/webSearchEngine' }, { silent: true }).catch(() => null),
    ])
      .then(([state, profileList, ruleList, hotkeyList, cfgEngine]) => {
        if (!isMounted) return;
        setEnabled(state.enabled);
        const normalizedTrigger = state.triggerButton === 'all' ? 'both' : (state.triggerButton ?? 'right');
        setTriggerButton(normalizedTrigger);
        setTrailVisible(state.trailVisible ?? true);
        setTrailFadeEnabled(state.trailFadeEnabled ?? true);
        setShowActionHint(state.showActionHint ?? true);
        setAutoBypassFullscreen(state.autoBypassFullscreen ?? true);
        setAutoBypassProRightClickApps(state.autoBypassProRightClickApps ?? true);
        if (Array.isArray(state.proRightClickPresetApps)) {
          setProRightClickPresetApps(state.proRightClickPresetApps);
        }
        setScribbleCancel(state.enableScribbleCancel ?? true);
        setPauseCancelEnabled(state.pauseCancelEnabled ?? true);
        setPauseCancelMs(state.pauseCancelMs ?? 700);
        setTargetMode(state.targetMode === 'foreground' ? 'foreground' : 'underPointer');
        setTrailColorMode(state.trailColorMode ?? 'auto');
        setTrailColor(state.trailColor ?? '#3B82F6');
        setTrailWidth(state.trailWidth ?? 2.5);
        setTrailOutlineWidth(state.trailOutlineWidth ?? 1.5);
        setInitialTimeoutMs(state.initialTimeoutMs ?? 0);
        setMinSegmentDistance(state.minSegmentDistance ?? 24);
        setShapeMatchThreshold(state.shapeMatchThreshold ?? 75);
        setElevated(state.elevated ?? false);
        const resolvedEngine =
          (typeof state.webSearchEngine === 'string' && state.webSearchEngine.trim() !== '' ? state.webSearchEngine : null) ??
          (typeof cfgEngine === 'string' && cfgEngine.trim() !== '' ? cfgEngine : 'bing');
        setWebSearchEngine(resolvedEngine);

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

        const list = Array.isArray(hotkeyList) ? hotkeyList : [];
        setHotkeys(list);
        const pauseBinding =
          list.find((entry) => entry.name === 'Toggle Gestures') ??
          list.find((entry) => entry.name === 'Pause Gestures');
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

  const handleRebindPauseHotkey = async (value: string) => {
    if (!pauseHotkeyEntry) return;
    const targetName = pauseHotkeyEntry.name;
    const prev = pauseHotkey;
    try {
      const result = await bridgeRequest<OperationResult>('hotkey.rebind', {
        name: targetName,
        hotkey: value,
      });
      if (!result.success) throw new Error(result.error || tr('hotkey.bindFailed'));
      setPauseHotkey(result.shortcut ?? value);
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) setHotkeys(refreshed);
    } catch (err) {
      setPauseHotkey(prev);
      toast.error(tr('hotkey.bindFailed'), { description: String(err) });
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll').catch(() => null);
      if (Array.isArray(refreshed)) setHotkeys(refreshed);
    }
  };

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
  const persistMappings = useCallback(async (
    nextMappings: GestureMapping[],
    triggerStateOverrides?: Record<string, TriggerState>
  ) => {
    const profName = getCurrentProfileName();
    const prevProf = profiles[profName] ?? { name: profName, mappings: [], triggerStates: {} };
    const nextTriggerStates = triggerStateOverrides
      ? { ...(prevProf.triggerStates ?? {}), ...triggerStateOverrides }
      : prevProf.triggerStates;
    const updatedProf: GestureProfileData = {
      ...prevProf,
      mappings: nextMappings,
      ...(nextTriggerStates ? { triggerStates: nextTriggerStates } : {}),
    };
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
    const prevProf = profiles[profName] ?? { name: profName, mappings: [], triggerStates: {} };
    const nextStates = { ...currentTriggerStates, [triggerKey]: nextState };
    const updatedProf: GestureProfileData = {
      ...prevProf,
      triggerStates: nextStates,
    };
    setProfiles((prev) => ({ ...prev, [profName]: updatedProf }));

    try {
      const result = await bridgeRequest<OperationResult>('gesture.setTriggerState', {
        profile: profName,
        trigger: triggerKey,
        state: nextState,
      });
      if (!result || !result.success) throw new Error(result?.error || 'Failed to update trigger state');
      // 协同放行顶层触发门禁，避免在药丸条启用中键/侧键时被旧版 TriggerMode::RightOnly 静默拦截
      if (
        nextState === 'enabled' &&
        (triggerKey === 'middle' || triggerKey === 'xbutton1' || triggerKey === 'xbutton2') &&
        triggerButton !== 'both'
      ) {
        setTriggerButton('both');
        void bridgeRequest<OperationResult>('gesture.updateSettings', { triggerButton: 'both' }).catch(() => undefined);
      }
    } catch (err) {
      setProfiles((prev) => ({ ...prev, [profName]: prevProf }));
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
    const nextEnabled = !(item.enabled ?? true);
    nextList[index] = { ...item, enabled: nextEnabled };

    // 边缘手势一键拨开时自动联动点亮对应边缘触发药丸；全部关回时自动熄灭，零误触无双道锁
    let triggerOverrides: Record<string, TriggerState> | undefined;
    const requiredTrigger = resolveRequiredEdgeTriggerKey(item.gestureCode);
    if (requiredTrigger) {
      if (nextEnabled && getTriggerState(requiredTrigger) !== 'enabled') {
        triggerOverrides = { [requiredTrigger]: 'enabled' };
      } else if (!nextEnabled && getTriggerState(requiredTrigger) === 'enabled') {
        const stillHasActive = nextList.some(
          (m) => (m.enabled ?? true) && resolveRequiredEdgeTriggerKey(m.gestureCode) === requiredTrigger
        );
        if (!stillHasActive) {
          triggerOverrides = { [requiredTrigger]: 'disabled' };
        }
      }
    }

    await persistMappings(nextList, triggerOverrides);
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
    setConfirmDialog({ kind: 'target', ruleId, name: r.name || r.processName });
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

  const handleSaveMapping = (saved: GestureMapping, options?: GestureSaveOptions) => {
    const shouldReplaceSimilar = options?.replaceSimilarTracks ?? false;
    const editingId = editingMapping?.id;
    const editingCode = editingMapping ? editingMapping.gestureCode.trim().toUpperCase() : '';
    const baseMappings = shouldReplaceSimilar
      ? currentMappings.filter((m) => {
          const isSelf = editingMapping
            ? (editingId && m.id ? m.id === editingId : m.gestureCode.trim().toUpperCase() === editingCode)
            : false;
          if (isSelf) return true;
          return !areSimilarGestureCodes(m.gestureCode, saved.gestureCode);
        })
      : currentMappings;
    const nextMappings = upsertGestureMapping(baseMappings, saved, editingMapping);
    let triggerOverrides: Record<string, TriggerState> | undefined;
    if (saved.enabled !== false) {
      const requiredTrigger = resolveRequiredEdgeTriggerKey(saved.gestureCode);
      if (requiredTrigger && getTriggerState(requiredTrigger) !== 'enabled') {
        triggerOverrides = { [requiredTrigger]: 'enabled' };
      }
    }
    void persistMappings(nextMappings, triggerOverrides);
    setEditorOpen(false);
  };

  const handleDeleteMapping = (index: number) => {
    const m = currentMappings[index];
    if (!m) return;
    setConfirmDialog({
      kind: 'mapping',
      index,
      name: getLocalizedGestureName(m.action.name, t),
      code: m.gestureCode,
    });
  };

  const handleConfirmDialogExecute = async () => {
    if (!confirmDialog) return;
    const current = confirmDialog;
    setConfirmDialog(null);
    if (current.kind === 'target') {
      const nextRules = rules.filter((x) => x.id !== current.ruleId);
      await persistRules(nextRules);
      if (selectedTarget.id === `rule:${current.ruleId}`) {
        setSelectedTarget({
          id: 'global',
          title: t('components.globalTitle', 'Global'),
          subtitle: t('components.defaultGestureSubtitle', 'Default Gesture Config'),
          kind: 'global',
        });
      }
      toast.success(t('gesture.toastTargetRemoved', 'Target configuration removed'));
    } else {
      await persistMappings(currentMappings.filter((_, i) => i !== current.index));
    }
  };

  // ── 触发与全局配置变更 ──────────────────────────────────────────────────────
  const handleToggleEnabled = async (checked: boolean) => {
    setEnabled(checked);
    try {
      if (checked) {
        await bridgeRequest('plugins.setEnabled', { id: 'gesture', enabled: true });
      }
      const result = await bridgeRequest<OperationResult>('gesture.setPaused', { paused: !checked });
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

  const handleToggleTrailFade = async (checked: boolean) => {
    setTrailFadeEnabled(checked);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailFadeEnabled: checked });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setTrailFadeEnabled(!checked);
      console.error('Failed to update gesture trail fade state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleToggleShowActionHint = async (checked: boolean) => {
    setShowActionHint(checked);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { showActionHint: checked });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setShowActionHint(!checked);
      console.error('Failed to update gesture action hint state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleToggleTrailColorMode = async (mode: 'auto' | 'custom') => {
    const previous = trailColorMode;
    setTrailColorMode(mode);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailColorMode: mode });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      setTrailColorMode(previous);
      console.error('Failed to update trail color mode:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleTrailColorChange = async (color: string) => {
    const seq = ++trailColorSeqRef.current;
    const previous = trailColor;
    setTrailColor(color);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailColor: color });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      if (trailColorSeqRef.current === seq) {
        setTrailColor(previous);
        console.error('Failed to update trail color:', err);
        toast.error(tr('gesture.saveFailed'), { description: String(err) });
      }
    }
  };

  const handleTrailWidthChange = async (widthStr: string) => {
    const previous = trailWidth;
    const w = parseFloat(widthStr) || 4.0;
    setTrailWidth(w);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailWidth: w });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      setTrailWidth(previous);
      console.error('Failed to update trail width:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleTrailOutlineWidthChange = async (widthStr: string) => {
    const previous = trailOutlineWidth;
    const w = parseFloat(widthStr);
    const outline = Number.isFinite(w) ? w : 2.5;
    setTrailOutlineWidth(outline);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { trailOutlineWidth: outline });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      setTrailOutlineWidth(previous);
      console.error('Failed to update trail outline width:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleToggleAutoBypass = async (checked: boolean) => {
    setAutoBypassFullscreen(checked);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { autoBypassFullscreen: checked });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      setAutoBypassFullscreen(!checked);
      console.error('Failed to update gesture autoBypassFullscreen state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleToggleAutoBypassProRightClick = async (checked: boolean) => {
    setAutoBypassProRightClickApps(checked);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', {
        autoBypassProRightClickApps: checked,
      });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      setAutoBypassProRightClickApps(!checked);
      console.error('Failed to update gesture autoBypassProRightClickApps state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleInitialTimeoutChange = async (val: number) => {
    const previous = initialTimeoutMs;
    const clamped = val <= 0 ? 0 : Math.max(100, Math.min(3000, Math.round(val)));
    setInitialTimeoutMs(clamped);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { initialTimeoutMs: clamped });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      setInitialTimeoutMs(previous);
      console.error('Failed to update gesture initialTimeoutMs:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleMinSegmentDistanceChange = async (val: number) => {
    const previous = minSegmentDistance;
    const clamped = Math.max(5, Math.min(100, Math.round(val)));
    setMinSegmentDistance(clamped);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { minSegmentDistance: clamped });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      setMinSegmentDistance(previous);
      console.error('Failed to update gesture minSegmentDistance:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleShapeMatchThresholdChange = async (val: number) => {
    const previous = shapeMatchThreshold;
    const clamped = Math.max(50, Math.min(100, Math.round(val)));
    setShapeMatchThreshold(clamped);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { shapeMatchThreshold: clamped });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      setShapeMatchThreshold(previous);
      console.error('Failed to update gesture shapeMatchThreshold:', err);
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

  const handleTogglePauseCancel = async (checked: boolean) => {
    setPauseCancelEnabled(checked);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { pauseCancelEnabled: checked });
      if (!result.success) throw new Error(result.error || tr('gesture.saveFailed'));
    } catch (err) {
      setPauseCancelEnabled(!checked);
      console.error('Failed to update pause cancel state:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handlePauseCancelMsChange = async (val: number) => {
    const previous = pauseCancelMs;
    const clamped = Math.max(400, Math.min(2000, Math.round(val)));
    setPauseCancelMs(clamped);
    try {
      const result = await bridgeRequest<OperationResult>('gesture.updateSettings', { pauseCancelMs: clamped });
      if (!result || !result.success) throw new Error(result?.error || tr('gesture.saveFailed'));
    } catch (err) {
      setPauseCancelMs(previous);
      console.error('Failed to update gesture pauseCancelMs:', err);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const handleWebSearchEngineChange = async (engine: string) => {
    const previous = webSearchEngine;
    setWebSearchEngine(engine);
    try {
      const direct = await bridgeRequest<OperationResult>('gesture.updateSettings', { webSearchEngine: engine }, { silent: true }).catch(() => null);
      if (!direct?.success) {
        await bridgeRequest('config.set', { key: '/gesture/webSearchEngine', value: engine });
      }
    } catch (err) {
      setWebSearchEngine(previous);
      toast.error(tr('gesture.saveFailed'), { description: String(err) });
    }
  };

  const [triggerFilter, setTriggerFilter] = useState<GestureFilterCategory>('all');

  const categoryCounts = useMemo(() => {
    const counts: Record<GestureFilterCategory, number> = {
      all: currentMappings.length,
      right: 0,
      middle: 0,
      side: 0,
      edge: 0,
      wheel: 0,
    };
    for (const m of currentMappings) {
      if (matchesGestureCategory(m.gestureCode, 'right')) counts.right += 1;
      if (matchesGestureCategory(m.gestureCode, 'middle')) counts.middle += 1;
      if (matchesGestureCategory(m.gestureCode, 'side')) counts.side += 1;
      if (matchesGestureCategory(m.gestureCode, 'edge')) counts.edge += 1;
      if (matchesGestureCategory(m.gestureCode, 'wheel')) counts.wheel += 1;
    }
    return counts;
  }, [currentMappings]);

  const filteredMappings = useMemo(() => {
    return currentMappings.filter((m) => {
      if (!matchesGestureCategory(m.gestureCode, triggerFilter)) return false;

      if (!searchQuery.trim()) return true;
      const q = searchQuery.toLowerCase();
      const locName = getLocalizedGestureName(m.action.name, t).toLowerCase();
      const locDesc = (getLocalizedGestureDesc(m.action.description, t) || '').toLowerCase();
      return (
        m.gestureCode.toLowerCase().includes(q) ||
        m.action.name.toLowerCase().includes(q) ||
        locName.includes(q) ||
        (m.action.keyStroke || '').toLowerCase().includes(q) ||
        (m.action.programPath || '').toLowerCase().includes(q) ||
        (m.action.matchCmdLine || '').toLowerCase().includes(q) ||
        (m.action.matchTitle || '').toLowerCase().includes(q) ||
        (m.action.description || '').toLowerCase().includes(q) ||
        locDesc.includes(q)
      );
    });
  }, [currentMappings, triggerFilter, searchQuery, t]);

  const renderTriggerMiniBadge = (gestureCode: string) => {
    const parsed = parseGestureCode(gestureCode);
    const isWheel = parsed.wheelDirection !== null;
    if (parsed.edge !== 'none') {
      const edgeShortMap = {
        top: t('gesture.edgeShortTop', 'Top'),
        bottom: t('gesture.edgeShortBottom', 'Bottom'),
        left: t('gesture.edgeShortLeft', 'Left'),
        right: t('gesture.edgeShortRight', 'Right'),
      } as const;
      const edgeLabel = edgeShortMap[parsed.edge];
      return (
        <span className="gesture-trigger-mini-badge gesture-trigger-mini-badge--edge">
          <ScreenEdgeSvg edge={parsed.edge} withWheel={isWheel} size={11} />
          <span>{isWheel ? `${edgeLabel}·${t('gesture.wheelShort', 'Wheel')}` : edgeLabel}</span>
        </span>
      );
    }
    if (isWheel) {
      return (
        <span className="gesture-trigger-mini-badge gesture-trigger-mini-badge--wheel">
          <MouseButtonSvg button="wheel" size={11} />
          <span>{t('gesture.wheelShort', 'Wheel')}</span>
        </span>
      );
    }
    if (parsed.triggerButton === 'x1' || parsed.triggerButton === 'x2') {
      return (
        <span className="gesture-trigger-mini-badge gesture-trigger-mini-badge--side">
          <MouseButtonSvg button={parsed.triggerButton} size={11} />
          <span>{parsed.triggerButton === 'x1' ? t('gesture.side1Short', 'X1') : t('gesture.side2Short', 'X2')}</span>
        </span>
      );
    }
    if (parsed.triggerButton === 'middle') {
      return (
        <span className="gesture-trigger-mini-badge gesture-trigger-mini-badge--middle">
          <MouseButtonSvg button="middle" size={11} />
          <span>{t('gesture.middleClickShort', 'Middle')}</span>
        </span>
      );
    }
    if (parsed.triggerButton === 'left') {
      return (
        <span className="gesture-trigger-mini-badge gesture-trigger-mini-badge--side">
          <MouseButtonSvg button="left" size={11} />
          <span>{t('gesture.leftClickShort', 'Left')}</span>
        </span>
      );
    }
    return (
      <span className="gesture-trigger-mini-badge gesture-trigger-mini-badge--right">
        <MouseButtonSvg button="right" size={11} />
        <span>{t('gesture.rightClickShort', 'Right')}</span>
      </span>
    );
  };

  if (loading) {
    return <PageSkeleton cards={2} rowsPerCard={4} />;
  }

  const renderTargetHeaderIcon = () => {
    if (selectedTarget.kind === 'global') return <Globe size={18} />;
    if (selectedTarget.kind === 'disabled') return <ShieldAlert size={18} style={{ color: 'var(--danger)' }} />;
    if (selectedTarget.specialType === 'desktop') return <Monitor size={18} />;
    if (selectedTarget.specialType === 'taskbar') return <LayoutTemplate size={18} />;
    const p = (selectedTarget.rule?.processName || '').toLowerCase();
    if (p.includes('code') || p.includes('studio')) return <Code2 size={18} />;
    if (p.includes('explorer')) return <Folder size={18} />;
    return <AppWindow size={18} />;
  };

  const tabs: TabItem<GesturePageSubTab>[] = [
    { id: 'rules', label: tr('gesture.tabRules'), icon: <Hand size={15} /> },
    { id: 'appearance', label: tr('gesture.tabAppearance'), icon: <Palette size={15} /> },
    { id: 'algorithm', label: tr('gesture.tabAlgorithm'), icon: <SlidersHorizontal size={15} /> },
  ];

  return (
    <div className="gesture-page">
      <Tabs
        tabs={tabs}
        activeId={activeTab}
        onChange={setActiveTab}
        className="gesture-page__tabs"
        ariaLabel={tr('gesture.title')}
      />

      {/* ── 1. 手势与目标规则 (默认首屏：精炼主控 + Master-Detail 手势工作台) ── */}
      {activeTab === 'rules' && (
        <div className="gesture-page__panel">
          <SettingGroup title={tr('gesture.title')} icon={<Mouse size={20} strokeWidth={2.2} />}>
            <Card>
              <div className={`gesture-status ${enabled ? 'gesture-status--active' : 'gesture-status--paused'}`}>
                <span className="gesture-status__dot" />
                <span className="gesture-status__text">{enabled ? tr('gesture.statusRunning') : tr('gesture.statusPaused')}</span>
              </div>
              <Toggle
                id="gesture-enabled"
                label={tr('gesture.enabled')}
                description={tr('gesture.enabledDesc')}
                checked={enabled}
                onChange={handleToggleEnabled}
              />
              {!elevated && (
                <div className="gesture-admin-hint-card">
                  <ShieldAlert size={16} className="gesture-admin-hint-icon" aria-hidden="true" />
                  <div className="gesture-admin-hint-content">
                    <div className="gesture-admin-hint-title">{tr('gesture.adminHintTitle')}</div>
                    <div className="gesture-admin-hint-desc">{tr('gesture.adminHintDesc')}</div>
                  </div>
                </div>
              )}
              <Toggle
                id="gesture-bypass-fullscreen"
                label={tr('gesture.autoBypassFullscreen')}
                description={tr('gesture.autoBypassFullscreenDesc')}
                checked={autoBypassFullscreen}
                onChange={handleToggleAutoBypass}
              />
              <Toggle
                id="gesture-bypass-pro-right-click"
                label={tr('gesture.autoBypassProRightClickApps')}
                description={tr('gesture.autoBypassProRightClickAppsDesc')}
                checked={autoBypassProRightClickApps}
                onChange={handleToggleAutoBypassProRightClick}
              />
              {proRightClickPresetApps.length > 0 && (
                <details className="gesture-pro-presets-details">
                  <summary className="gesture-pro-presets-summary">
                    <span>
                      {t('gesture.proRightClickPresetToggle', 'View built-in bypass list ({{count}} apps)', {
                        count: proRightClickPresetApps.length,
                      })}
                    </span>
                    <ChevronDown size={14} className="gesture-pro-presets-chevron" aria-hidden="true" />
                  </summary>
                  <div className="gesture-pro-presets-body">
                    {(
                      [
                        { id: 'cad', titleKey: 'gesture.proRightClickCatCad' },
                        { id: 'eda', titleKey: 'gesture.proRightClickCatEda' },
                        { id: 'dcc3d', titleKey: 'gesture.proRightClickCatDcc3d' },
                      ] as const
                    ).map((cat) => {
                      const items = proRightClickPresetApps.filter((a) => a.category === cat.id);
                      if (items.length === 0) return null;
                      return (
                        <div key={cat.id} className="gesture-pro-presets-group">
                          <div className="gesture-pro-presets-group-title">{tr(cat.titleKey)}</div>
                          <div className="gesture-pro-presets-chips">
                            {items.map((app) => (
                              <span key={app.processName} className="gesture-pro-preset-item">
                                <span className="gesture-pro-preset-name">{app.displayName}</span>
                                <CodeBadge variant="muted">{app.processName}</CodeBadge>
                              </span>
                            ))}
                          </div>
                        </div>
                      );
                    })}
                    <p className="gesture-pro-presets-hint">{tr('gesture.proRightClickPresetOverrideHint')}</p>
                  </div>
                </details>
              )}
              <SettingRow
                label={
                  <div className="gesture-inline-label">
                    <span>{tr('gesture.pauseShortcutLabel')}</span>
                    <HotkeyStatusBadge entry={pauseHotkeyEntry} />
                  </div>
                }
              >
                {pauseHotkeyEntry ? (
                  <HotkeyRecorder
                    id="gesture-pause-shortcut"
                    value={pauseHotkey}
                    onChange={(v) => void handleRebindPauseHotkey(v)}
                  />
                ) : (
                  <kbd className="gesture-status__hotkey">{pauseHotkey || 'Ctrl+Alt+Shift+W'}</kbd>
                )}
              </SettingRow>
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
                    {/* ── 允许的触发方式 (极简单行药丸横向条，13 枚药丸单一事实源) ── */}
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
                          const isDefaultEnabled =
                            (item.key === 'right' && (triggerButton === 'right' || triggerButton === 'both')) ||
                            (item.key === 'middle' && (triggerButton === 'middle' || triggerButton === 'both')) ||
                            ((item.key === 'xbutton1' || item.key === 'xbutton2') && triggerButton === 'both');
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
                              <TriggerItemSvg triggerKey={item.key} size={13} />
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

                          {/* 触发来源五分类精准筛选 (U-11 纯矢量徽章) */}
                          <div className="gesture-trigger-filter-tabs" role="tablist" aria-label={t('gesture.triggerModeStrip', 'Trigger Method')}>
                            <button
                              type="button"
                              role="tab"
                              aria-selected={triggerFilter === 'all'}
                              className={`gesture-trigger-filter-tab ${triggerFilter === 'all' ? 'active' : ''}`}
                              onClick={() => setTriggerFilter('all')}
                            >
                              <span>{t('common.all', 'All')} ({categoryCounts.all})</span>
                            </button>
                            <button
                              type="button"
                              role="tab"
                              aria-selected={triggerFilter === 'right'}
                              className={`gesture-trigger-filter-tab ${triggerFilter === 'right' ? 'active' : ''}`}
                              onClick={() => setTriggerFilter('right')}
                            >
                              <MouseButtonSvg button="right" size={13} />
                              <span>{t('gesture.rightGestures', 'Right Click')} ({categoryCounts.right})</span>
                            </button>
                            <button
                              type="button"
                              role="tab"
                              aria-selected={triggerFilter === 'middle'}
                              className={`gesture-trigger-filter-tab ${triggerFilter === 'middle' ? 'active' : ''}`}
                              onClick={() => setTriggerFilter('middle')}
                            >
                              <MouseButtonSvg button="middle" size={13} />
                              <span>{t('gesture.middleGestures', 'Middle Click')} ({categoryCounts.middle})</span>
                            </button>
                            <button
                              type="button"
                              role="tab"
                              aria-selected={triggerFilter === 'side'}
                              className={`gesture-trigger-filter-tab ${triggerFilter === 'side' ? 'active' : ''}`}
                              onClick={() => setTriggerFilter('side')}
                            >
                              <MouseButtonSvg button="side" size={13} />
                              <span>{t('gesture.sideGestures', 'Side Buttons')} ({categoryCounts.side})</span>
                            </button>
                            <button
                              type="button"
                              role="tab"
                              aria-selected={triggerFilter === 'edge'}
                              className={`gesture-trigger-filter-tab ${triggerFilter === 'edge' ? 'active' : ''}`}
                              onClick={() => setTriggerFilter('edge')}
                            >
                              <ScreenEdgeSvg edge="all" size={13} />
                              <span>{t('gesture.edgeGestures', 'Screen Edge')} ({categoryCounts.edge})</span>
                            </button>
                            <button
                              type="button"
                              role="tab"
                              aria-selected={triggerFilter === 'wheel'}
                              className={`gesture-trigger-filter-tab ${triggerFilter === 'wheel' ? 'active' : ''}`}
                              onClick={() => setTriggerFilter('wheel')}
                            >
                              <MouseButtonSvg button="wheel" size={13} />
                              <span>{t('gesture.wheelGestures', 'Wheel')} ({categoryCounts.wheel})</span>
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
                            const actualIdx = currentMappings.findIndex((x) => (m.id ? x.id === m.id : x.gestureCode === m.gestureCode));
                            const isEnabled = m.enabled ?? true;
                            const isDragging = draggedIdx === actualIdx;
                            return (
                              <div
                                key={m.id || `${m.gestureCode}-${i}`}
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
                                      {renderTriggerMiniBadge(m.gestureCode)}
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
                                    const tooltipDetail =
                                      m.action.type === 3
                                        ? [
                                            detailText,
                                            m.action.programPath,
                                            m.action.matchCmdLine,
                                            m.action.matchTitle,
                                          ]
                                            .filter((part): part is string => Boolean(part && part.trim()))
                                            .join(' | ')
                                        : detailText;
                                    const len = detailText.length;
                                    const fontSize = len >= 10 ? '0.83rem' : len >= 7 ? '0.85rem' : len >= 5 ? '0.88rem' : '0.92rem';
                                    return (
                                      <button
                                        type="button"
                                        className="gesture-detail-badge-btn"
                                        title={t('gesture.editParamTip', 'Click to edit parameter: {{detail}}', { detail: tooltipDetail })}
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

            <Card className="gesture-search-engine-card">
              <SettingRow
                label={t('gesture.webSearchEngine', 'Web Search Engine')}
                description={t('gesture.webSearchEngineDesc', 'Default search engine opened when executing Web Search gesture action')}
              >
                <Select
                  id="gesture-web-search-engine"
                  value={webSearchEngine}
                  onChange={(val) => void handleWebSearchEngineChange(val)}
                  options={[
                    { value: 'bing', label: 'Bing' },
                    { value: 'google', label: 'Google' },
                    { value: 'baidu', label: t('gesture.engineBaidu', 'Baidu') },
                    { value: 'duckduckgo', label: 'DuckDuckGo' },
                  ]}
                />
              </SettingRow>
            </Card>
          </SettingGroup>
        </div>
      )}

      {/* ── 2. 轨迹外观与演示 ──────────────────────────────────────────── */}
      {activeTab === 'appearance' && (
        <div className="gesture-page__panel">
          <SettingGroup title={tr('gesture.tabAppearance')} icon={<Palette size={20} strokeWidth={2.2} />}>
            <Card>
              <Toggle
                id="gesture-trail"
                label={tr('gesture.showTrail')}
                description={tr('gesture.showTrailDesc')}
                checked={trailVisible}
                onChange={handleToggleTrail}
              />
              {trailVisible && (
                <SubSettingGroup>
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
                                title={t(preset.nameKey as never, preset.name)}
                                onClick={() => void handleTrailColorChange(preset.hex)}
                              />
                            ))}
                          </div>
                          <div className="gesture-custom-color-input-wrapper">
                            <ColorPickerInput
                              className="gesture-color-native-input"
                              value={trailColor}
                              ariaLabel={tr('gesture.trailCustomColor')}
                              onPreview={(hex) => setTrailColor(hex)}
                              onChange={(hex) => void handleTrailColorChange(hex)}
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
                </SubSettingGroup>
              )}
              <Toggle
                id="gesture-action-hint"
                label={t('gesture.showActionHint', 'Show gesture action hint text')}
                description={t('gesture.showActionHintDesc', 'Display the recognized action name pill while drawing and upon release; can be toggled independently of trail lines')}
                checked={showActionHint}
                onChange={handleToggleShowActionHint}
              />
              {(trailVisible || showActionHint) && (
                <Toggle
                  id="gesture-trail-fade"
                  label={t('gesture.trailFadeEnabled', 'Trail release fade-out animation')}
                  description={t('gesture.trailFadeEnabledDesc', 'Smoothly fade out the gesture trail and hint after release; disable to clear immediately at 0ms')}
                  checked={trailFadeEnabled}
                  onChange={handleToggleTrailFade}
                />
              )}
            </Card>

            <Card className="gesture-guide-card">
              <GestureGuide />
            </Card>
          </SettingGroup>
        </div>
      )}

      {/* ── 3. 识别算法与防误触 ────────────────────────────────────────── */}
      {activeTab === 'algorithm' && (
        <div className="gesture-page__panel">
          <SettingGroup title={tr('gesture.groupAntiMistouch')} icon={<ShieldAlert size={18} />}>
            <Card>
              <Toggle
                id="gesture-scribble-cancel"
                label={tr('gesture.scribbleCancel')}
                description={tr('gesture.scribbleCancelDesc')}
                checked={scribbleCancel}
                onChange={handleToggleScribbleCancel}
              />
              <Toggle
                id="gesture-pause-cancel"
                label={t('gesture.pauseCancel', 'Pause to Cancel')}
                description={t(
                  'gesture.pauseCancelDesc',
                  'Hold the cursor still briefly after starting a gesture to fade out and cancel without triggering any action or context menu',
                )}
                checked={pauseCancelEnabled}
                onChange={handleTogglePauseCancel}
              />
              {pauseCancelEnabled && (
                <SubSettingGroup>
                  <SettingRow
                    label={t('gesture.pauseCancelMs', 'Pause Cancel Delay')}
                    description={t(
                      'gesture.pauseCancelMsDesc',
                      'How long the cursor must stay stationary during a stroke before auto-canceling (400-2000 ms)',
                    )}
                  >
                    <Select
                      id="gesture-pause-cancel-ms"
                      value={String(pauseCancelMs)}
                      onChange={(val) => void handlePauseCancelMsChange(Number(val))}
                      options={[
                        { value: '400', label: t('gesture.pauseCancelMsFast', '400 ms (Fast)') },
                        { value: '550', label: t('gesture.pauseCancelMsQuick', '550 ms (Quick)') },
                        { value: '700', label: t('gesture.pauseCancelMsStandard', '700 ms (Standard)') },
                        { value: '1000', label: t('gesture.pauseCancelMsRelaxed', '1000 ms (Relaxed)') },
                        { value: '1500', label: t('gesture.pauseCancelMsLong', '1500 ms (Long)') },
                        { value: '2000', label: t('gesture.pauseCancelMsExtraLong', '2000 ms (Extra Long)') },
                        ...(![400, 550, 700, 1000, 1500, 2000].includes(pauseCancelMs)
                          ? [{ value: String(pauseCancelMs), label: t('gesture.customMsUnit', '{{ms}} ms (Custom)', { ms: pauseCancelMs }) }]
                          : []),
                      ]}
                    />
                  </SettingRow>
                </SubSettingGroup>
              )}
            </Card>
          </SettingGroup>

          <SettingGroup title={tr('gesture.groupEngineParams')} icon={<SlidersHorizontal size={18} />}>
            <Card>
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
              <SettingRow label={tr('gesture.shapeMatchThreshold')} description={tr('gesture.shapeMatchThresholdDesc')}>
                <Select
                  id="gesture-shape-match-threshold"
                  value={String(shapeMatchThreshold)}
                  onChange={(val) => void handleShapeMatchThresholdChange(Number(val))}
                  options={[
                    { value: '55', label: tr('gesture.shapeMatchThresholdVeryLoose') },
                    { value: '65', label: tr('gesture.shapeMatchThresholdLoose') },
                    { value: '75', label: tr('gesture.shapeMatchThresholdStandard') },
                    { value: '85', label: tr('gesture.shapeMatchThresholdStrict') },
                    { value: '95', label: tr('gesture.shapeMatchThresholdVeryStrict') },
                    { value: '100', label: tr('gesture.shapeMatchThresholdExactOnly') },
                    ...(![55, 65, 75, 85, 95, 100].includes(shapeMatchThreshold)
                      ? [{ value: String(shapeMatchThreshold), label: t('gesture.customPercentUnit', '{{percent}}% (Custom)', { percent: shapeMatchThreshold }) }]
                      : []),
                  ]}
                />
              </SettingRow>
              <SettingRow label={tr('gesture.initialTimeout')} description={tr('gesture.initialTimeoutDesc')}>
                <Select
                  id="gesture-initial-timeout"
                  value={String(initialTimeoutMs)}
                  onChange={(val) => void handleInitialTimeoutChange(Number(val))}
                  options={[
                    { value: '0', label: tr('gesture.initialTimeoutNone') },
                    { value: '300', label: tr('gesture.initialTimeoutFast') },
                    { value: '500', label: tr('gesture.initialTimeoutStandard') },
                    { value: '800', label: tr('gesture.initialTimeoutRelaxed') },
                    { value: '1200', label: tr('gesture.initialTimeoutGenerous') },
                    { value: '2000', label: tr('gesture.initialTimeoutLong') },
                    ...(![0, 300, 500, 800, 1200, 2000].includes(initialTimeoutMs)
                      ? [{ value: String(initialTimeoutMs), label: t('gesture.customMsUnit', '{{ms}} ms (Custom)', { ms: initialTimeoutMs }) }]
                      : []),
                  ]}
                />
              </SettingRow>
            </Card>
          </SettingGroup>
        </div>
      )}

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
          webSearchEngine={webSearchEngine}
          onWebSearchEngineChange={(eng) => void handleWebSearchEngineChange(eng)}
          onSave={handleSaveMapping}
          onClose={() => setEditorOpen(false)}
        />
      )}

      <ConfirmModal
        open={confirmDialog !== null}
        title={t('common.delete')}
        description={
          confirmDialog?.kind === 'target'
            ? t('gesture.confirmRemoveTarget', 'Are you sure you want to remove the custom configuration for target 「{{name}}」?', { name: confirmDialog.name })
            : tr('gesture.deleteConfirm', { name: confirmDialog?.name ?? '', code: confirmDialog?.code ?? '' })
        }
        confirmText={t('common.delete')}
        onConfirm={handleConfirmDialogExecute}
        onClose={() => setConfirmDialog(null)}
      />
    </div>
  );
};

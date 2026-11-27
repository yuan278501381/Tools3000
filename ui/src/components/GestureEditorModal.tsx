/* ─────────────────────────────────────────────────────────────────────────────
 * GestureEditorModal — 手势动作编辑器弹窗 (World-Class Interactive Gesture Editor)
 *
 * 核心交互架构:
 *   - 零认知负荷: 无冗长说明文字，界面高度紧凑，一屏尽览
 *   - 一体化手势控制舱: 触发按键 (右键/中键) 与键盘修饰键 (Ctrl/Shift/Alt) 实时联动
 *   - 紧凑网格动作表单: 动作名称 + 类型与参数 2 列并排，最大化垂直空间利用率
 *   - 替换相似手势设置: 智能检测冲突并提供一键覆盖
 *   - 录制免打扰保护: 挂载时通知后端进入 recordingMode，拦截所有手势动作触发
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useRef, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Sliders,
  ChevronDown,
  ChevronUp,
  Zap,
  VolumeX,
  Lightbulb,
} from 'lucide-react';
import { Modal, Field, TextInput, Select, Toggle, Button } from './UIKit';
import { HotkeyRecorder } from './HotkeyRecorder';
import { GestureDrawCanvas } from './GestureDrawCanvas';
import { GestureStrokePreview } from './GestureStrokePreview';
import {
  type GestureMapping,
  type ProgramRunMode,
  ACTION_TYPE_KEYS,
  BUILTIN_COMMAND_KEYS,
  DEFAULT_LOCAL_LUA_PERMISSIONS,
  GESTURE_CODE_PATTERN,
  codeToArrows,
  isGesturePrefixConflict,
  parseGestureCode,
  assembleGestureCode,
} from './gestureModel';
import { getLocalizedGestureName } from '../utils/gestureI18n';
import { bridgeRequest } from '../hooks/useBridge';
import {
  DEFAULT_WINDOW_MANAGER_SETTINGS,
  type SnapZoneLayout,
  type WindowManagerResponse,
  type WorkspaceItem,
} from '../types/windowManager';
import './GestureEditorModal.css';

const BUILTIN_CMD_APPLY_LAYOUT = 39;
const BUILTIN_CMD_LAUNCH_WORKSPACE = 40;

export interface GestureSaveOptions {
  replaceSimilarTracks?: boolean;
}

interface Props {
  mapping?: GestureMapping | null;
  initial?: GestureMapping | null;
  existingCodes?: string[];
  existingMappings?: GestureMapping[];
  initialFocusTarget?: 'instant' | 'silent' | 'action_type' | 'action_detail' | 'hotkey' | 'lua' | 'builtin' | 'program' | null;
  webSearchEngine?: string;
  onWebSearchEngineChange?: (engine: string) => void;
  onSave: (mapping: GestureMapping, options?: GestureSaveOptions) => void;
  onClose: () => void;
}

const DEFAULT_MAPPING: GestureMapping = {
  gestureCode: '',
  enabled: true,
  instantExecute: false,
  silentToast: false,
  action: {
    type: 0,
    name: '',
  },
};

export const GestureEditorModal: FC<Props> = ({
  mapping,
  initial,
  existingCodes,
  existingMappings,
  initialFocusTarget,
  webSearchEngine = 'bing',
  onWebSearchEngineChange,
  onSave,
  onClose,
}) => {
  const { t } = useTranslation();
  const initialMap = mapping ?? initial ?? null;

  const [draft, setDraft] = useState<GestureMapping>(() => ({
    ...(initialMap ?? DEFAULT_MAPPING),
    action: { ...(initialMap?.action ?? DEFAULT_MAPPING.action) },
  }));

  const initialCode = (initialMap?.gestureCode ?? '').trim().toUpperCase();
  const initialId = initialMap?.id;
  const isEdit = Boolean(initialMap);

  const [advancedOpen, setAdvancedOpen] = useState(
    initialFocusTarget === 'instant' || initialFocusTarget === 'silent'
  );
  const [replaceSimilarTracks, setReplaceSimilarTracks] = useState(true);
  const [wmLayouts, setWmLayouts] = useState<SnapZoneLayout[]>(DEFAULT_WINDOW_MANAGER_SETTINGS.snapZones.layouts);
  const [wmActiveLayoutId, setWmActiveLayoutId] = useState<string>(
    DEFAULT_WINDOW_MANAGER_SETTINGS.snapZones.activeLayoutId,
  );
  const [wmWorkspaces, setWmWorkspaces] = useState<WorkspaceItem[]>([]);

  const actionTypeRef = useRef<HTMLDivElement>(null);
  const actionDetailRef = useRef<HTMLDivElement>(null);
  const advancedRef = useRef<HTMLDivElement>(null);
  const instantCardRef = useRef<HTMLDivElement>(null);
  const silentCardRef = useRef<HTMLDivElement>(null);

  const code = draft.gestureCode.trim().toUpperCase();

  // 根据传入的 initialFocusTarget 精准滚动定位到目标字段
  useEffect(() => {
    if (!initialFocusTarget) return;
    const refMap: Record<string, React.RefObject<HTMLDivElement | null>> = {
      action_type: actionTypeRef,
      action_detail: actionDetailRef,
      hotkey: actionDetailRef,
      builtin: actionDetailRef,
      lua: actionDetailRef,
      program: actionDetailRef,
      instant: instantCardRef,
      silent: silentCardRef,
    };
    const targetRef = refMap[initialFocusTarget];
    const timer = window.setTimeout(() => {
      targetRef?.current?.scrollIntoView({ behavior: 'smooth', block: 'center' });
    }, 60);
    return () => window.clearTimeout(timer);
  }, [initialFocusTarget]);

  // 弹窗挂载时开启录制免打扰保护模式，关闭时自动恢复全局拦截；同时静默加载分屏布局与工作区列表
  useEffect(() => {
    let active = true;
    void bridgeRequest('gesture.setRecordingMode', { recording: true });
    void bridgeRequest<WindowManagerResponse>('windowManager.getSettings', undefined, { silent: true })
      .then((res) => {
        if (!active || !res?.settings) return;
        const layouts = res.settings.snapZones?.layouts;
        if (Array.isArray(layouts) && layouts.length > 0) {
          setWmLayouts(layouts);
        }
        if (typeof res.settings.snapZones?.activeLayoutId === 'string') {
          setWmActiveLayoutId(res.settings.snapZones.activeLayoutId);
        }
        const items = res.settings.workspaces?.items;
        if (Array.isArray(items)) {
          setWmWorkspaces(items);
        }
      })
      .catch(() => {
        // 离线或窗口管理未就绪时保留默认内置布局兜底
      });
    return () => {
      active = false;
      void bridgeRequest('gesture.setRecordingMode', { recording: false });
    };
  }, []);

  const handleToggleAdvanced = () => {
    const next = !advancedOpen;
    setAdvancedOpen(next);
    if (next) {
      requestAnimationFrame(() => {
        setTimeout(() => {
          advancedRef.current?.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
        }, 40);
      });
    }
  };

  // 校验与智能冲突识别
  let codeError = '';
  if (!code) {
    codeError = t('gestureEditor.gestureCodeRequired', 'Please draw or select a gesture on the canvas');
  } else if (!GESTURE_CODE_PATTERN.test(code)) {
    codeError = t('gestureEditor.gestureCodeInvalid', 'Invalid gesture track, please redraw on canvas');
  }

  // 检测是否与其它已有手势存在编码冲突
  const conflictingMapping = (existingMappings ?? []).find(
    (m) =>
      m.gestureCode.trim().toUpperCase() === code &&
      (m.id && initialId ? m.id !== initialId : m.gestureCode.trim().toUpperCase() !== initialCode)
  );

  const isDuplicateOfOther = Boolean(
    conflictingMapping ||
      ((existingCodes ?? []).map((c) => c.trim().toUpperCase()).includes(code) && code !== initialCode)
  );

  const conflictWarning =
    isDuplicateOfOther && !codeError
      ? conflictingMapping?.action?.name
        ? t('gestureEditor.gestureConflictOverwriteNamed', {
            name: getLocalizedGestureName(conflictingMapping.action.name, t),
            code: `「${codeToArrows(code)}」`,
            defaultValue: t('gesture.conflictReplaceWithAction', 'Gesture 「{{arrows}}」 is already bound to 「{{action}}」. Saving will replace it.', { arrows: codeToArrows(code), action: getLocalizedGestureName(conflictingMapping.action.name, t) }),
          })
        : t('gestureEditor.gestureConflictOverwrite', {
            code: `「${codeToArrows(code)}」`,
            defaultValue: t('gesture.conflictReplaceSimple', 'Gesture 「{{arrows}}」 already exists. Saving will replace it.', { arrows: codeToArrows(code) }),
          })
      : '';

  const allOtherCodes = (existingMappings ? existingMappings.map((m) => m.gestureCode) : (existingCodes ?? []))
    .map((c) => c.trim().toUpperCase())
    .filter((c) => c !== initialCode && c !== code);

  const prefixConflicts =
    code && !codeError && !conflictWarning
      ? allOtherCodes.filter((c) => isGesturePrefixConflict(c, code))
      : [];

  const nameError = draft.action.name.trim() ? '' : t('gestureEditor.actionNameRequired', 'Action name is required');
  const needsBuiltinTarget =
    draft.action.type === 2 &&
    (draft.action.builtinCmd === BUILTIN_CMD_APPLY_LAYOUT ||
      draft.action.builtinCmd === BUILTIN_CMD_LAUNCH_WORKSPACE);
  const builtinTargetError =
    needsBuiltinTarget && !(draft.action.builtinParam ?? '').trim()
      ? t('gestureEditor.targetRequiredError', 'Please select a target layout or workspace')
      : '';
  const canSave = !codeError && !nameError && !builtinTargetError;

  const setAction = (patch: Partial<GestureMapping['action']>) =>
    setDraft((d) => ({ ...d, action: { ...d.action, ...patch } }));

  const isLegacyProgramEdit = isEdit && initialMap?.action.type === 3 && !initialMap?.action.runMode;
  const effectiveRunMode: ProgramRunMode =
    draft.action.runMode ?? (isLegacyProgramEdit ? 'always_new' : 'activate_or_run');

  const handleSave = () => {
    if (!canSave) return;
    const tVal = draft.action.type;
    const action: GestureMapping['action'] = {
      type: tVal,
      name: draft.action.name.trim(),
      description: draft.action.description,
    };
    if (tVal === 0) action.keyStroke = draft.action.keyStroke ?? '';
    else if (tVal === 1) {
      action.luaScript = draft.action.luaScript ?? '';
      action.permissions =
        Array.isArray(draft.action.permissions) && draft.action.permissions.length > 0
          ? draft.action.permissions
          : [...DEFAULT_LOCAL_LUA_PERMISSIONS];
    } else if (tVal === 2) {
      action.builtinCmd = draft.action.builtinCmd ?? 0;
      const param = (draft.action.builtinParam ?? '').trim();
      if (
        param &&
        (action.builtinCmd === BUILTIN_CMD_APPLY_LAYOUT ||
          action.builtinCmd === BUILTIN_CMD_LAUNCH_WORKSPACE)
      ) {
        action.builtinParam = param;
      }
    } else if (tVal === 3) {
      action.programPath = (draft.action.programPath ?? '').trim();
      action.programArgs = draft.action.programArgs ?? '';
      action.runMode = effectiveRunMode;
      const cmdLineFilter = (draft.action.matchCmdLine ?? '').trim();
      const titleFilter = (draft.action.matchTitle ?? '').trim();
      if (cmdLineFilter) action.matchCmdLine = cmdLineFilter;
      if (titleFilter) action.matchTitle = titleFilter;
    }
    onSave(
      {
        id: draft.id || generateMappingId(code),
        enabled: draft.enabled ?? true,
        instantExecute: draft.instantExecute ?? false,
        silentToast: draft.silentToast ?? false,
        gestureCode: code,
        action,
      },
      { replaceSimilarTracks },
    );
  };

  const handleGestureCodeChange = (newCode: string) => {
    const clean = newCode.trim().toUpperCase();
    setDraft((prev) => {
      const isCleanEmpty = !prev.action.name.trim();
      const presets: Record<
        string,
        { nameKey: string; name: string; type: number; keyStroke?: string; builtinCmd?: number; descKey?: string; desc?: string }
      > = {
        L: { nameKey: 'gesture.actions.back.name', name: 'Back', type: 0, keyStroke: 'Alt+Left', descKey: 'gesture.actions.back.desc', desc: 'Browser / Explorer back' },
        R: { nameKey: 'gesture.actions.forward.name', name: 'Forward', type: 0, keyStroke: 'Alt+Right', descKey: 'gesture.actions.forward.desc', desc: 'Browser / Explorer forward' },
        'MIDDLE+L': { nameKey: 'gesture.actions.prevTrack.name', name: 'Previous Track', type: 0, keyStroke: 'MediaPrev', descKey: 'gesture.actions.prevTrack.desc', desc: 'Media previous track' },
        'MIDDLE+R': { nameKey: 'gesture.actions.nextTrack.name', name: 'Next Track', type: 0, keyStroke: 'MediaNext', descKey: 'gesture.actions.nextTrack.desc', desc: 'Media next track' },
        'X1+L': { nameKey: 'gesture.actions.back.name', name: 'Back', type: 0, keyStroke: 'Alt+Left', descKey: 'gesture.actions.back.desc', desc: 'Browser / Explorer back' },
        'X1+R': { nameKey: 'gesture.actions.forward.name', name: 'Forward', type: 0, keyStroke: 'Alt+Right', descKey: 'gesture.actions.forward.desc', desc: 'Browser / Explorer forward' },
        'X1+U': { nameKey: 'gesture.actions.nextTab.name', name: 'Next Tab', type: 0, keyStroke: 'Ctrl+Tab', descKey: 'gesture.actions.nextTab.desc', desc: 'Switch to next tab' },
        'X1+D': { nameKey: 'gesture.actions.prevTab.name', name: 'Previous Tab', type: 0, keyStroke: 'Ctrl+Shift+Tab', descKey: 'gesture.actions.prevTab.desc', desc: 'Switch to previous tab' },
        'X1+D-R': { nameKey: 'gesture.actions.closeTab.name', name: 'Close Tab', type: 0, keyStroke: 'Ctrl+W', descKey: 'gesture.actions.closeTab.desc', desc: 'Close current tab' },
        'X2+L': { nameKey: 'gesture.actions.prevTrack.name', name: 'Previous Track', type: 0, keyStroke: 'MediaPrev', descKey: 'gesture.actions.prevTrack.desc', desc: 'Media previous track' },
        'X2+R': { nameKey: 'gesture.actions.nextTrack.name', name: 'Next Track', type: 0, keyStroke: 'MediaNext', descKey: 'gesture.actions.nextTrack.desc', desc: 'Media next track' },
        'X2+U': { nameKey: 'gesture.actions.volumeUp.name', name: 'Volume Up', type: 2, builtinCmd: 22, descKey: 'gesture.actions.volumeUp.desc', desc: 'Increase master volume' },
        'X2+D': { nameKey: 'gesture.actions.volumeDown.name', name: 'Volume Down', type: 2, builtinCmd: 23, descKey: 'gesture.actions.volumeDown.desc', desc: 'Decrease master volume' },
        'TOPEDGE+D': { nameKey: 'gesture.actions.taskView.name', name: 'Task View', type: 2, builtinCmd: 7, descKey: 'gesture.actions.taskView.desc', desc: 'Open Windows task view' },
        'TOPEDGE+L': { nameKey: 'gesture.actions.prevVirtualDesktop.name', name: 'Previous Virtual Desktop', type: 2, builtinCmd: 25, descKey: 'gesture.actions.prevVirtualDesktop.desc', desc: 'Switch to previous virtual desktop' },
        'TOPEDGE+R': { nameKey: 'gesture.actions.nextVirtualDesktop.name', name: 'Next Virtual Desktop', type: 2, builtinCmd: 26, descKey: 'gesture.actions.nextVirtualDesktop.desc', desc: 'Switch to next virtual desktop' },
        'TOPEDGE+LEFT+D': { nameKey: 'gesture.actions.closeWindow.name', name: 'Close Window', type: 0, keyStroke: 'Alt+F4', descKey: 'gesture.actions.closeWindow.desc', desc: 'Close active window' },
        U: { nameKey: 'gesture.actions.maxRestore.name', name: 'Maximize/Restore', type: 2, builtinCmd: 2, descKey: 'gesture.actions.maxRestore.desc', desc: 'Maximize or restore window' },
        D: { nameKey: 'gesture.actions.minimize.name', name: 'Minimize', type: 2, builtinCmd: 3, descKey: 'gesture.actions.minimize.desc', desc: 'Minimize active window' },
        'D-R': { nameKey: 'gesture.actions.closeTab.name', name: 'Close Tab', type: 0, keyStroke: 'Ctrl+W', descKey: 'gesture.actions.closeTab.desc', desc: 'Close current tab' },
        'R-D': { nameKey: 'gesture.actions.reopenTab.name', name: 'Reopen Closed Tab', type: 0, keyStroke: 'Ctrl+Shift+T', descKey: 'gesture.actions.reopenTab.desc', desc: 'Reopen last closed tab' },
        'D-L': { nameKey: 'gesture.actions.closeWindow.name', name: 'Close Window', type: 0, keyStroke: 'Alt+F4', descKey: 'gesture.actions.closeWindow.desc', desc: 'Exit active application window' },
        'L-U-R': { nameKey: 'gesture.actions.refresh.name', name: 'Reload Page', type: 0, keyStroke: 'F5', descKey: 'gesture.actions.refresh.desc', desc: 'Reload current web page' },
        'D-U': { nameKey: 'gesture.actions.refresh.name', name: 'Reload Page', type: 0, keyStroke: 'F5', descKey: 'gesture.actions.refresh.desc', desc: 'Reload current web page' },
        'L-R-D': { nameKey: 'gesture.actions.quickTranslate.name', name: 'Quick Translate', type: 2, builtinCmd: 31, descKey: 'gesture.actions.quickTranslate.desc', desc: 'Translate selected or clipboard text' },
        'L-D': { nameKey: 'gesture.actions.showDesktop.name', name: 'Show Desktop', type: 2, builtinCmd: 5, descKey: 'gesture.actions.showDesktop.desc', desc: 'Minimize all and show desktop' },
        'D-R-D': { nameKey: 'gesture.actions.screenshot.name', name: 'Quick Screenshot', type: 2, builtinCmd: 10, descKey: 'gesture.actions.screenshot.desc', desc: 'Launch Tools3000 screenshot' },
        WHEELUP: { nameKey: 'gesture.actions.prevVirtualDesktop.name', name: 'Previous Virtual Desktop', type: 2, builtinCmd: 25, descKey: 'gesture.actions.prevVirtualDesktop.desc', desc: 'Switch to previous virtual desktop' },
        WHEELDOWN: { nameKey: 'gesture.actions.nextVirtualDesktop.name', name: 'Next Virtual Desktop', type: 2, builtinCmd: 26, descKey: 'gesture.actions.nextVirtualDesktop.desc', desc: 'Switch to next virtual desktop' },
        'TOPEDGE+WHEELUP': { nameKey: 'gesture.actions.prevVirtualDesktop.name', name: 'Previous Virtual Desktop', type: 2, builtinCmd: 25, descKey: 'gesture.actions.prevVirtualDesktop.desc', desc: 'Switch to previous virtual desktop' },
        'TOPEDGE+WHEELDOWN': { nameKey: 'gesture.actions.nextVirtualDesktop.name', name: 'Next Virtual Desktop', type: 2, builtinCmd: 26, descKey: 'gesture.actions.nextVirtualDesktop.desc', desc: 'Switch to next virtual desktop' },
        'BOTTOMEDGE+WHEELUP': { nameKey: 'gesture.actions.volumeUp.name', name: 'Volume Up', type: 2, builtinCmd: 22, descKey: 'gesture.actions.volumeUp.desc', desc: 'Increase master volume' },
        'BOTTOMEDGE+WHEELDOWN': { nameKey: 'gesture.actions.volumeDown.name', name: 'Volume Down', type: 2, builtinCmd: 23, descKey: 'gesture.actions.volumeDown.desc', desc: 'Decrease master volume' },
        'LEFTEDGE+WHEELUP': { nameKey: 'gesture.actions.brightnessUp.name', name: 'Brightness Up', type: 2, builtinCmd: 29, descKey: 'gesture.actions.brightnessUp.desc', desc: 'Increase brightness of the display under cursor' },
        'LEFTEDGE+WHEELDOWN': { nameKey: 'gesture.actions.brightnessDown.name', name: 'Brightness Down', type: 2, builtinCmd: 30, descKey: 'gesture.actions.brightnessDown.desc', desc: 'Decrease brightness of the display under cursor' },
        'RIGHTEDGE+WHEELUP': { nameKey: 'gesture.actions.prevVirtualDesktop.name', name: 'Previous Virtual Desktop', type: 2, builtinCmd: 25, descKey: 'gesture.actions.prevVirtualDesktop.desc', desc: 'Switch to previous virtual desktop' },
        'RIGHTEDGE+WHEELDOWN': { nameKey: 'gesture.actions.nextVirtualDesktop.name', name: 'Next Virtual Desktop', type: 2, builtinCmd: 26, descKey: 'gesture.actions.nextVirtualDesktop.desc', desc: 'Switch to next virtual desktop' },
      };

      const matched = presets[clean];
      if (matched && isCleanEmpty) {
        return {
          ...prev,
          gestureCode: newCode,
          action: {
            ...prev.action,
            name: matched.nameKey ? (t as (k: string, d?: string) => string)(matched.nameKey, matched.name) : matched.name,
            type: matched.type,
            keyStroke: matched.keyStroke,
            builtinCmd: matched.builtinCmd,
            description: matched.descKey ? (t as (k: string, d?: string) => string)(matched.descKey, matched.desc) : matched.desc,
          },
        };
      }
      return { ...prev, gestureCode: newCode };
    });
  };

  const handleTriggerChange = (targetTrigger: import('./gestureModel').TriggerButton) => {
    const parsed = parseGestureCode(draft.gestureCode);
    if (!parsed.bareCode) return;
    const newCode = assembleGestureCode({
      edge: parsed.edge,
      triggerButton: targetTrigger,
      hasCtrl: parsed.hasCtrl,
      hasShift: parsed.hasShift,
      hasAlt: parsed.hasAlt,
      bareCode: parsed.bareCode,
    });
    handleGestureCodeChange(newCode);
  };

  const handleEdgeChange = (targetEdge: import('./gestureModel').ScreenEdge) => {
    const parsed = parseGestureCode(draft.gestureCode);
    if (!parsed.bareCode) return;
    const newCode = assembleGestureCode({
      edge: targetEdge,
      triggerButton: parsed.triggerButton,
      hasCtrl: parsed.hasCtrl,
      hasShift: parsed.hasShift,
      hasAlt: parsed.hasAlt,
      bareCode: parsed.bareCode,
    });
    handleGestureCodeChange(newCode);
  };

  const activeAdvancedCount = (draft.instantExecute ? 1 : 0) + (draft.silentToast ? 1 : 0);
  const parsed = parseGestureCode(draft.gestureCode);

  return (
    <Modal
      open
      title={isEdit ? t('gestureEditor.titleEdit') : t('gestureEditor.titleAdd')}
      onClose={onClose}
      footer={
        <>
          <Button variant="ghost" onClick={onClose}>
            {t('common.cancel')}
          </Button>
          <Button variant="primary" onClick={handleSave} disabled={!canSave}>
            {t('common.save')}
          </Button>
        </>
      }
    >
      <Field
        label={t('gestureEditor.gestureDraw')}
        error={codeError}
        hint={
          prefixConflicts.length
            ? t('gestureEditor.prefixConflict', {
                codes: prefixConflicts.map((c) => `「${codeToArrows(c)}」`).join(', '),
              })
            : undefined
        }
      >
        <GestureDrawCanvas
          value={draft.gestureCode}
          onChange={handleGestureCodeChange}
          triggerButton={parsed.triggerButton}
          onTriggerButtonChange={handleTriggerChange}
          screenEdge={parsed.edge}
          onScreenEdgeChange={handleEdgeChange}
        />
        {code && (
          <div style={{ marginTop: '10px', display: 'flex', gap: '12px', alignItems: 'center' }}>
            <span style={{ fontSize: '0.83rem', color: 'var(--text-muted)' }}>
              {t('gestureEditor.recognizedGesture')}:
            </span>
            <GestureStrokePreview code={code} width={64} height={38} autoAnimate />
            <span style={{ fontSize: '0.88rem', fontWeight: 600, color: 'var(--primary)' }}>
              {codeToArrows(code)}
            </span>
          </div>
        )}
        {conflictWarning && (
          <div className="gesture-conflict-warning">
            <Lightbulb size={14} className="gesture-conflict-icon" />
            <span>{conflictWarning}</span>
          </div>
        )}
      </Field>

      <Field label={t('gestureEditor.actionName')} error={nameError}>
        <TextInput
          value={draft.action.name}
          onChange={(v: string) => setAction({ name: v })}
          placeholder={t('gestureEditor.actionNamePlaceholder')}
        />
      </Field>

      {/* 动作类型与参数配置 (紧凑 2 列并排网格) */}
      <div className="gesture-action-config-grid">
        <div ref={actionTypeRef}>
          <Field label={t('gestureEditor.actionType')}>
            <Select
              value={String(draft.action.type)}
              options={ACTION_TYPE_KEYS.map((key, index) => ({ value: String(index), label: t(key) }))}
              onChange={(v: string) => {
                const nextType = Number(v);
                if (nextType === 3 && !draft.action.runMode) {
                  setAction({ type: nextType, runMode: 'activate_or_run' });
                } else {
                  setAction({ type: nextType });
                }
              }}
              placement="auto"
              portal
            />
          </Field>
        </div>

        <div ref={actionDetailRef}>
          {draft.action.type === 0 && (
            <Field label={t('gestureEditor.hotkey')} hint={t('gestureEditor.hotkeyHintSingleKey', 'Supports single keys (e.g. Delete, Backspace, Enter) and key combinations')}>
              <HotkeyRecorder
                id="gesture-hotkey-recorder"
                value={draft.action.keyStroke ?? ''}
                onChange={(v: string) => setAction({ keyStroke: v })}
                allowSingleKey
              />
            </Field>
          )}

          {draft.action.type === 1 && (
            <Field label={t('gestureEditor.luaScript')} hint={t('gestureEditor.luaScriptHint')}>
              <TextInput
                multiline
                value={draft.action.luaScript ?? ''}
                onChange={(v: string) => setAction({ luaScript: v })}
                placeholder={'easy.shell.open("https://example.com")'}
              />
            </Field>
          )}

          {draft.action.type === 2 && (
            <>
              <Field label={t('gestureEditor.builtinCommand')}>
                <Select
                  value={String(draft.action.builtinCmd ?? 0)}
                  options={BUILTIN_COMMAND_KEYS
                    .map((key, i) => ({ value: String(i), label: t(key) }))
                    // C-01: 过滤下标 17 (已下线的 radialMenu) 与与 26 重复的 6 (仅老配置仍可回显)
                    .filter((opt) => opt.value !== '17' && (opt.value !== '6' || draft.action.builtinCmd === 6))}
                  onChange={(v: string) => {
                    const nextCmd = Number(v);
                    const prevCmd = draft.action.builtinCmd ?? 0;
                    const prevKey = BUILTIN_COMMAND_KEYS[prevCmd];
                    const nextKey = BUILTIN_COMMAND_KEYS[nextCmd];
                    const trimmedName = draft.action.name.trim();
                    const prevDefaultLabel = prevKey ? t(prevKey) : '';
                    const shouldAutoName =
                      !trimmedName ||
                      trimmedName === prevDefaultLabel ||
                      (prevDefaultLabel !== '' && trimmedName.startsWith(`${prevDefaultLabel}: `));

                    if (nextCmd === BUILTIN_CMD_APPLY_LAYOUT) {
                      const defaultLayout =
                        wmLayouts.find((l) => l.id === wmActiveLayoutId) ?? wmLayouts[0];
                      const nextParam = defaultLayout?.id ?? '';
                      const nextLabel = nextKey ? t(nextKey) : '';
                      setAction({
                        builtinCmd: nextCmd,
                        builtinParam: nextParam,
                        ...(shouldAutoName && defaultLayout
                          ? { name: `${nextLabel}: ${defaultLayout.name}` }
                          : {}),
                      });
                    } else if (nextCmd === BUILTIN_CMD_LAUNCH_WORKSPACE) {
                      const defaultWs = wmWorkspaces[0];
                      const nextParam = defaultWs?.id ?? '';
                      const nextLabel = nextKey ? t(nextKey) : '';
                      setAction({
                        builtinCmd: nextCmd,
                        builtinParam: nextParam,
                        ...(shouldAutoName && defaultWs
                          ? { name: `${nextLabel}: ${defaultWs.name}` }
                          : {}),
                      });
                    } else {
                      const nextLabel = nextKey ? t(nextKey) : '';
                      setAction({
                        builtinCmd: nextCmd,
                        builtinParam: undefined,
                        ...(shouldAutoName && nextLabel ? { name: nextLabel } : {}),
                      });
                    }
                  }}
                  placement="auto"
                  portal
                />
              </Field>
              {draft.action.builtinCmd === BUILTIN_CMD_APPLY_LAYOUT && (
                <div style={{ marginTop: '8px' }}>
                  <Field
                    label={t('gestureEditor.targetLayoutLabel', 'Target SnapZones Layout')}
                    hint={t('gestureEditor.targetLayoutHint', 'Immediately activates this layout on the active monitor when triggered')}
                    error={builtinTargetError}
                  >
                    <Select
                      id="gesture-target-layout-select"
                      value={draft.action.builtinParam ?? ''}
                      placeholder={t('gestureEditor.targetLayoutPlaceholder', 'Select a SnapZones layout to apply')}
                      options={wmLayouts.map((layout) => ({
                        value: layout.id,
                        label: layout.name,
                      }))}
                      onChange={(layoutId: string) => {
                        const chosen = wmLayouts.find((l) => l.id === layoutId);
                        const cmdLabel = t(BUILTIN_COMMAND_KEYS[BUILTIN_CMD_APPLY_LAYOUT]);
                        const trimmedName = draft.action.name.trim();
                        const shouldSyncName =
                          !trimmedName ||
                          trimmedName === cmdLabel ||
                          trimmedName.startsWith(`${cmdLabel}: `);
                        setAction({
                          builtinParam: layoutId,
                          ...(shouldSyncName && chosen ? { name: `${cmdLabel}: ${chosen.name}` } : {}),
                        });
                      }}
                      placement="auto"
                      portal
                    />
                  </Field>
                </div>
              )}
              {draft.action.builtinCmd === BUILTIN_CMD_LAUNCH_WORKSPACE && (
                <div style={{ marginTop: '8px' }}>
                  <Field
                    label={t('gestureEditor.targetWorkspaceLabel', 'Target Workspace')}
                    hint={
                      wmWorkspaces.length > 0
                        ? t('gestureEditor.targetWorkspaceHint', 'Launches or repositions all apps saved in this workspace snapshot')
                        : t('gestureEditor.noWorkspaceAvailableHint', 'No workspaces yet. Capture one first in Window Manager -> Workspaces')
                    }
                    error={builtinTargetError}
                  >
                    <Select
                      id="gesture-target-workspace-select"
                      value={draft.action.builtinParam ?? ''}
                      placeholder={t('gestureEditor.targetWorkspacePlaceholder', 'Select a workspace to launch')}
                      options={wmWorkspaces.map((ws) => ({
                        value: ws.id,
                        label: `${ws.name} (${t('gestureEditor.workspaceAppCountSuffix', { count: ws.apps.length })})`,
                      }))}
                      onChange={(wsId: string) => {
                        const chosen = wmWorkspaces.find((w) => w.id === wsId);
                        const cmdLabel = t(BUILTIN_COMMAND_KEYS[BUILTIN_CMD_LAUNCH_WORKSPACE]);
                        const trimmedName = draft.action.name.trim();
                        const shouldSyncName =
                          !trimmedName ||
                          trimmedName === cmdLabel ||
                          trimmedName.startsWith(`${cmdLabel}: `);
                        setAction({
                          builtinParam: wsId,
                          ...(shouldSyncName && chosen ? { name: `${cmdLabel}: ${chosen.name}` } : {}),
                        });
                      }}
                      disabled={wmWorkspaces.length === 0}
                      placement="auto"
                      portal
                    />
                  </Field>
                </div>
              )}
              {draft.action.builtinCmd === 15 && onWebSearchEngineChange && (
                <div style={{ marginTop: '8px' }}>
                  <Field label={t('gesture.webSearchEngine', 'Web Search Engine')}>
                    <Select
                      value={webSearchEngine}
                      options={[
                        { value: 'bing', label: 'Bing' },
                        { value: 'google', label: 'Google' },
                        { value: 'baidu', label: t('gesture.engineBaidu', 'Baidu') },
                        { value: 'duckduckgo', label: 'DuckDuckGo' },
                      ]}
                      onChange={onWebSearchEngineChange}
                      placement="auto"
                      portal
                    />
                  </Field>
                </div>
              )}
            </>
          )}

          {draft.action.type === 3 && (
            <>
              <Field label={t('gestureEditor.programPath')}>
                <TextInput
                  value={draft.action.programPath ?? ''}
                  onChange={(v: string) => setAction({ programPath: v })}
                  placeholder="C:\\Windows\\System32\\notepad.exe"
                />
              </Field>
              <Field label={t('gestureEditor.programArgs')} hint={t('gestureEditor.programArgsHint')}>
                <TextInput
                  value={draft.action.programArgs ?? ''}
                  onChange={(v: string) => setAction({ programArgs: v })}
                />
              </Field>
              <Field
                label={t('gestureEditor.programRunMode' as never, 'Launch Mode')}
                hint={t(
                  'gestureEditor.programRunModeHint' as never,
                  'Automatically extracts --user-data-dir / --profile-directory fingerprints from arguments for <3ms single-instance switching',
                )}
              >
                <Select
                  id="gesture-program-run-mode"
                  value={effectiveRunMode}
                  options={[
                    {
                      value: 'activate_or_run',
                      label: t(
                        'gestureEditor.runModeActivateOrRun' as never,
                        'Activate existing window or launch (Recommended)',
                      ),
                    },
                    {
                      value: 'toggle_or_run',
                      label: t(
                        'gestureEditor.runModeToggleOrRun' as never,
                        'Activate / minimize toggle or launch',
                      ),
                    },
                    {
                      value: 'always_new',
                      label: t(
                        'gestureEditor.runModeAlwaysNew' as never,
                        'Always launch a new instance',
                      ),
                    },
                  ]}
                  onChange={(v: string) => setAction({ runMode: v as ProgramRunMode })}
                  placement="auto"
                  portal
                />
              </Field>
              {effectiveRunMode !== 'always_new' && (
                <>
                  <Field
                    label={t('gestureEditor.programMatchCmdLine' as never, 'Command-line / Profile Filter')}
                    hint={t(
                      'gestureEditor.programMatchCmdLineHint' as never,
                      'Optional. Leave blank to auto-detect from Arguments (e.g. --user-data-dir)',
                    )}
                  >
                    <TextInput
                      value={draft.action.matchCmdLine ?? ''}
                      onChange={(v: string) => setAction({ matchCmdLine: v })}
                      placeholder={t(
                        'gestureEditor.programMatchCmdLinePlaceholder' as never,
                        'e.g. Profile 1 or Chrome\\Data',
                      )}
                    />
                  </Field>
                  <Field
                    label={t('gestureEditor.programMatchTitle' as never, 'Window Title Filter')}
                    hint={t(
                      'gestureEditor.programMatchTitleHint' as never,
                      'Optional case-insensitive substring match for window title',
                    )}
                  >
                    <TextInput
                      value={draft.action.matchTitle ?? ''}
                      onChange={(v: string) => setAction({ matchTitle: v })}
                      placeholder={t(
                        'gestureEditor.programMatchTitlePlaceholder' as never,
                        'e.g. Visual Studio Code',
                      )}
                    />
                  </Field>
                </>
              )}
            </>
          )}
        </div>
      </div>

      {/* 替换所有相似手势轨迹配置卡片 */}
      <div className="gesture-replace-tracks-card">
        <label className="gesture-replace-tracks-label">
          <input
            type="checkbox"
            className="gesture-replace-tracks-checkbox"
            checked={replaceSimilarTracks}
            onChange={(e: React.ChangeEvent<HTMLInputElement>) => setReplaceSimilarTracks(e.target.checked)}
          />
          <span className="gesture-replace-tracks-title">
            {t('gestureEditor.replaceSimilarTracks', 'Replace all similar gesture tracks')}
          </span>
          <span className="gesture-replace-tracks-subtitle">
            ({t('gestureEditor.replaceSimilarHintAuto', 'Automatically overwrites duplicate tracks to maintain unique mappings')})
          </span>
        </label>
      </div>

      {/* 高级执行特性折叠面板 */}
      <div ref={advancedRef} className="gesture-editor-advanced-wrapper">
        <button
          type="button"
          className="gesture-editor-advanced-toggle"
          onClick={handleToggleAdvanced}
        >
          <div className="gesture-editor-advanced-toggle__left">
            <Sliders size={16} className="gesture-editor-advanced-toggle__icon" />
            <span>{t('gestureEditor.advancedTitle')}</span>
          </div>
          <div className="gesture-editor-advanced-toggle__right">
            {activeAdvancedCount > 0 && (
              <span className="gesture-editor-advanced-badge">
                {t('gestureEditor.advancedActiveCount', { count: activeAdvancedCount })}
              </span>
            )}
            {advancedOpen ? <ChevronUp size={16} /> : <ChevronDown size={16} />}
          </div>
        </button>

        {advancedOpen && (
          <div className="gesture-editor-advanced-content">
            {/* 即时执行 */}
            <div
              ref={instantCardRef}
              className={`gesture-scenario-card ${draft.instantExecute ? 'gesture-scenario-card--active' : ''} ${initialFocusTarget === 'instant' ? 'gesture-scenario-card--focused' : ''}`}
            >
              <div
                className="gesture-scenario-card__header"
                onClick={() => setDraft((d) => ({ ...d, instantExecute: !d.instantExecute }))}
              >
                <div className="gesture-scenario-card__main">
                  <div className="gesture-scenario-card__icon-box gesture-scenario-card__icon-box--instant">
                    <Zap size={16} />
                  </div>
                  <div className="gesture-scenario-card__text-group">
                    <span className="gesture-scenario-card__title">{t('gestureEditor.instantExecuteTitle')}</span>
                    <span className="gesture-scenario-card__desc">{t('gestureEditor.instantExecuteDesc')}</span>
                  </div>
                </div>
                <div onClick={(e) => e.stopPropagation()}>
                  <Toggle
                    id="gesture-instant-execute-toggle"
                    checked={draft.instantExecute ?? false}
                    onChange={(checked: boolean) => setDraft((d) => ({ ...d, instantExecute: checked }))}
                  />
                </div>
              </div>
              <div className="gesture-scenario-card__callout">
                <Lightbulb size={13} className="gesture-scenario-card__callout-icon" />
                <span>{t('gestureEditor.instantExecuteScenario')}</span>
              </div>
            </div>

            {/* 静默模式 */}
            <div
              ref={silentCardRef}
              className={`gesture-scenario-card ${draft.silentToast ? 'gesture-scenario-card--active' : ''} ${initialFocusTarget === 'silent' ? 'gesture-scenario-card--focused' : ''}`}
            >
              <div
                className="gesture-scenario-card__header"
                onClick={() => setDraft((d) => ({ ...d, silentToast: !d.silentToast }))}
              >
                <div className="gesture-scenario-card__main">
                  <div className="gesture-scenario-card__icon-box gesture-scenario-card__icon-box--silent">
                    <VolumeX size={16} />
                  </div>
                  <div className="gesture-scenario-card__text-group">
                    <span className="gesture-scenario-card__title">{t('gestureEditor.silentToastTitle')}</span>
                    <span className="gesture-scenario-card__desc">{t('gestureEditor.silentToastDesc')}</span>
                  </div>
                </div>
                <div onClick={(e) => e.stopPropagation()}>
                  <Toggle
                    id="gesture-silent-toast-toggle"
                    checked={draft.silentToast ?? false}
                    onChange={(checked: boolean) => setDraft((d) => ({ ...d, silentToast: checked }))}
                  />
                </div>
              </div>
              <div className="gesture-scenario-card__callout">
                <Lightbulb size={13} className="gesture-scenario-card__callout-icon" />
                <span>{t('gestureEditor.silentToastScenario')}</span>
              </div>
            </div>
          </div>
        )}
      </div>
    </Modal>
  );
};

function generateMappingId(code: string): string {
  return `custom-${code.toLowerCase().replace(/[^a-z0-9]/g, '_')}-${Date.now().toString(36)}`;
}

/* -----------------------------------------------------------------------------
 * TextExpanderPage — 系统级文本扩展器配置与实时体验工作台
 * ----------------------------------------------------------------------------- */

import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ChangeEvent,
  type FC,
} from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  CornerDownLeft,
  Download,
  Lock,
  Pencil,
  Plus,
  RotateCcw,
  Save,
  Search,
  ShieldCheck,
  Sparkles,
  TextCursorInput,
  Trash2,
  Upload,
  X,
  Zap,
} from 'lucide-react';
import {
  Button,
  Card,
  CodeBadge,
  Field,
  Modal,
  PageSkeleton,
  SettingGroup,
  SettingRow,
  SubSettingGroup,
  TextInput,
  Toggle,
} from '../components/UIKit';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import './TextExpanderPage.css';

export type TextExpanderTriggerMode = 'immediate' | 'delimiter';

export interface TextExpanderRuleItem {
  id: string;
  trigger: string;
  replacement: string;
  label: string;
  enabled: boolean;
  mode: TextExpanderTriggerMode;
  caseSensitive: boolean;
  fullwidthSensitive: boolean;
  keepDelimiter: boolean;
  hitCount: number;
}

export interface TextExpanderSettingsState {
  enabled: boolean;
  autoExcludeSensitiveApps: boolean;
  restoreClipboard: boolean;
  excludedProcesses: string[];
  defaultSensitiveProcesses: string[];
}

interface RuleFormDraft {
  id: string;
  trigger: string;
  replacement: string;
  label: string;
  enabled: boolean;
  mode: TextExpanderTriggerMode;
  caseSensitive: boolean;
  fullwidthSensitive: boolean;
  keepDelimiter: boolean;
}

const DEFAULT_SENSITIVE_PROCESSES = [
  'keepass.exe',
  'keepassxc.exe',
  '1password.exe',
  'bitwarden.exe',
  'enpass.exe',
  'lastpass.exe',
  'roboform.exe',
  'dashlane.exe',
  'mstsc.exe',
];

const DEFAULT_SETTINGS: TextExpanderSettingsState = {
  enabled: true,
  autoExcludeSensitiveApps: true,
  restoreClipboard: true,
  excludedProcesses: [],
  defaultSensitiveProcesses: DEFAULT_SENSITIVE_PROCESSES,
};

const MAX_TRIGGER_LENGTH = 64;

interface MacroItemSpec {
  id: string;
  token: string;
  labelKey: string;
  fallbackLabel: string;
}

const DYNAMIC_MACRO_GROUPS: ReadonlyArray<{
  groupId: string;
  groupLabelKey: string;
  groupFallback: string;
  items: ReadonlyArray<MacroItemSpec>;
}> = [
  {
    groupId: 'datetime',
    groupLabelKey: 'textExpander.macroGroupDate',
    groupFallback: 'Date & Time',
    items: [
      { id: 'date', token: '{{date}}', labelKey: 'textExpander.macroDate', fallbackLabel: 'Date' },
      { id: 'date_compact', token: '{{date_compact}}', labelKey: 'textExpander.macroDateCompact', fallbackLabel: 'Compact Date' },
      { id: 'date_slash', token: '{{date_slash}}', labelKey: 'textExpander.macroDateSlash', fallbackLabel: 'Slash Date' },
      { id: 'time', token: '{{time}}', labelKey: 'textExpander.macroTime', fallbackLabel: 'Time' },
      { id: 'time_sec', token: '{{time_sec}}', labelKey: 'textExpander.macroTimeSec', fallbackLabel: 'Time (Sec)' },
      { id: 'datetime', token: '{{datetime}}', labelKey: 'textExpander.macroDateTime', fallbackLabel: 'Date & Time' },
      { id: 'weekday', token: '{{weekday}}', labelKey: 'textExpander.macroWeekday', fallbackLabel: 'Weekday' },
      { id: 'date_plus_1d', token: '{{date+1d}}', labelKey: 'textExpander.macroDateTomorrow', fallbackLabel: 'Tomorrow' },
      { id: 'date_minus_1d', token: '{{date-1d}}', labelKey: 'textExpander.macroDateYesterday', fallbackLabel: 'Yesterday' },
    ],
  },
  {
    groupId: 'clipboard',
    groupLabelKey: 'textExpander.macroGroupClip',
    groupFallback: 'Clipboard & Cursor',
    items: [
      { id: 'clipboard', token: '{{clipboard}}', labelKey: 'textExpander.macroClipboard', fallbackLabel: 'Clipboard' },
      { id: 'clipboard_trim', token: '{{clipboard_trim}}', labelKey: 'textExpander.macroClipboardTrim', fallbackLabel: 'Trimmed Clipboard' },
      { id: 'cursor', token: '{{cursor}}', labelKey: 'textExpander.macroCursor', fallbackLabel: 'Cursor' },
    ],
  },
  {
    groupId: 'system',
    groupLabelKey: 'textExpander.macroGroupSystem',
    groupFallback: 'Dev & System',
    items: [
      { id: 'uuid', token: '{{uuid}}', labelKey: 'textExpander.macroUuid', fallbackLabel: 'UUID v4' },
      { id: 'timestamp', token: '{{timestamp}}', labelKey: 'textExpander.macroTimestamp', fallbackLabel: 'Unix Timestamp' },
      { id: 'user', token: '{{user}}', labelKey: 'textExpander.macroUser', fallbackLabel: 'System User' },
    ],
  },
];

function createEmptyDraft(): RuleFormDraft {
  return {
    id: '',
    trigger: '',
    replacement: '',
    label: '',
    enabled: true,
    mode: 'immediate',
    caseSensitive: false,
    fullwidthSensitive: false,
    keepDelimiter: true,
  };
}

function foldHalfwidthCodePoint(ch: string): string {
  const code = ch.charCodeAt(0);
  if (code >= 0xff01 && code <= 0xff5e) {
    return String.fromCharCode(code - 0xfee0);
  }
  if (code === 0x3000) return ' ';
  switch (code) {
    case 0x3001: return '\\';
    case 0x3002: return '.';
    case 0x300a: return '<';
    case 0x300b: return '>';
    case 0x3010: return '[';
    case 0x3011: return ']';
    case 0x300c:
    case 0x300d:
    case 0x201c:
    case 0x201d: return '"';
    case 0x2018:
    case 0x2019: return "'";
    case 0xffe5: return '$';
    case 0x2014: return '-';
    case 0x2026: return '.';
    case 0x00b7: return '`';
    default: return ch;
  }
}

function normalizeTriggerForMatch(
  input: string,
  caseSensitive: boolean,
  fullwidthSensitive: boolean,
): string {
  let out = '';
  for (let i = 0; i < input.length; i += 1) {
    const ch = fullwidthSensitive ? input[i] : foldHalfwidthCodePoint(input[i]);
    out += caseSensitive ? ch : ch.toLowerCase();
  }
  return out;
}

function normalizeTriggerMode(raw: unknown): TextExpanderTriggerMode {
  if (raw === 'delimiter' || raw === 1) return 'delimiter';
  return 'immediate';
}

function normalizeRule(raw: unknown): TextExpanderRuleItem | null {
  if (!raw || typeof raw !== 'object') return null;
  const obj = raw as Record<string, unknown>;
  const trigger = typeof obj.trigger === 'string' ? obj.trigger : '';
  const replacement = typeof obj.replacement === 'string' ? obj.replacement : '';
  if (!trigger) return null;
  return {
    id: typeof obj.id === 'string' ? obj.id : '',
    trigger,
    replacement,
    label: typeof obj.label === 'string' ? obj.label : '',
    enabled: typeof obj.enabled === 'boolean' ? obj.enabled : true,
    mode: normalizeTriggerMode(obj.mode),
    caseSensitive: Boolean(obj.caseSensitive),
    fullwidthSensitive: Boolean(obj.fullwidthSensitive),
    keepDelimiter: typeof obj.keepDelimiter === 'boolean' ? obj.keepDelimiter : true,
    hitCount: typeof obj.hitCount === 'number' && obj.hitCount >= 0 ? obj.hitCount : 0,
  };
}

function formatLocalizedWeekdayPreview(date: Date, lang: string): string {
  const raw = (lang || 'en').trim();
  const normalized = raw.toLowerCase() === 'zh' ? 'zh-CN' : raw;
  try {
    return new Intl.DateTimeFormat(normalized, { weekday: 'long' }).format(date);
  } catch {
    return new Intl.DateTimeFormat('en', { weekday: 'long' }).format(date);
  }
}

function expandMacrosForPreview(
  template: string,
  locale = 'en',
): { text: string; cursorOffsetFromEnd: number; cursorIndex: number | null } {
  const now = new Date();
  const pad2 = (n: number) => String(n).padStart(2, '0');
  const formatYmd = (d: Date, sep = '-') =>
    `${d.getFullYear()}${sep}${pad2(d.getMonth() + 1)}${sep}${pad2(d.getDate())}`;

  const dateStr = formatYmd(now, '-');
  const dateCompactStr = formatYmd(now, '');
  const dateSlashStr = formatYmd(now, '/');
  const timeStr = `${pad2(now.getHours())}:${pad2(now.getMinutes())}`;
  const timeSecStr = `${timeStr}:${pad2(now.getSeconds())}`;
  const dateTimeStr = `${dateStr} ${timeStr}`;
  const weekdayStr = formatLocalizedWeekdayPreview(now, locale);
  const timestampStr = String(Math.floor(now.getTime() / 1000));

  let out = '';
  let cursorPos: number | null = null;
  let i = 0;

  while (i < template.length) {
    if (template[i] === '{' && template[i + 1] === '{') {
      const end = template.indexOf('}}', i + 2);
      if (end !== -1 && end - (i + 2) <= 28) {
        const token = template.slice(i + 2, end);
        if (token === 'date') { out += dateStr; i = end + 2; continue; }
        if (token === 'date_compact') { out += dateCompactStr; i = end + 2; continue; }
        if (token === 'date_slash') { out += dateSlashStr; i = end + 2; continue; }
        if (token === 'time') { out += timeStr; i = end + 2; continue; }
        if (token === 'time_sec') { out += timeSecStr; i = end + 2; continue; }
        if (token === 'datetime') { out += dateTimeStr; i = end + 2; continue; }
        if (token === 'weekday') { out += weekdayStr; i = end + 2; continue; }
        if (token === 'clipboard') { out += '[clipboard]'; i = end + 2; continue; }
        if (token === 'clipboard_trim') { out += '[clipboard_trim]'; i = end + 2; continue; }
        if (token === 'uuid') { out += 'f47ac10b-58cc-4372-a567-0e02b2c3d479'; i = end + 2; continue; }
        if (token === 'timestamp') { out += timestampStr; i = end + 2; continue; }
        if (token === 'user') { out += 'user'; i = end + 2; continue; }
        if (token === 'cursor') {
          if (cursorPos === null) {
            cursorPos = out.length;
          }
          i = end + 2;
          continue;
        }
        const offsetMatch = /^date([+-]\d{1,5})d$/i.exec(token);
        if (offsetMatch) {
          const delta = Number(offsetMatch[1]);
          if (Math.abs(delta) <= 36500) {
            const shifted = new Date(now.getTime());
            shifted.setDate(shifted.getDate() + delta);
            out += formatYmd(shifted, '-');
            i = end + 2;
            continue;
          }
        }
      }
    }
    out += template[i];
    i += 1;
  }

  return {
    text: out,
    cursorOffsetFromEnd: cursorPos === null ? 0 : out.length - cursorPos,
    cursorIndex: cursorPos,
  };
}

function formatPreviewLine(text: string): string {
  return text.replace(/\r?\n/g, ' / ');
}

function parseCsvProcessList(csv: string): string[] {
  const seen = new Set<string>();
  const out: string[] = [];
  for (const part of csv.split(/[,;\n\r]+/)) {
    const cleaned = part.trim().toLowerCase();
    if (!cleaned || seen.has(cleaned)) continue;
    seen.add(cleaned);
    out.push(cleaned);
  }
  return out;
}

export const TextExpanderPage: FC = () => {
  const { t, i18n } = useTranslation();
  const activeLocale = i18n?.language || 'en';
  const [loading, setLoading] = useState(true);
  const [savingSettings, setSavingSettings] = useState(false);
  const [savingRule, setSavingRule] = useState(false);
  const [settings, setSettings] = useState<TextExpanderSettingsState>(DEFAULT_SETTINGS);
  const [rules, setRules] = useState<TextExpanderRuleItem[]>([]);
  const [searchQuery, setSearchQuery] = useState('');
  const [sandboxText, setSandboxText] = useState('');
  const [excludedOverride, setExcludedOverride] = useState<string | null>(null);

  // 新增/编辑短语表单状态
  const [editorOpen, setEditorOpen] = useState(false);
  const [draft, setDraft] = useState<RuleFormDraft>(createEmptyDraft);
  const [formError, setFormError] = useState('');
  const [sandboxCollapsed, setSandboxCollapsed] = useState(false);

  const replacementRef = useRef<HTMLTextAreaElement>(null);
  const sandboxRef = useRef<HTMLTextAreaElement>(null);
  const sandboxSectionRef = useRef<HTMLDivElement>(null);
  const importInputRef = useRef<HTMLInputElement>(null);

  const loadData = useCallback(async () => {
    try {
      const [settingsRes, rulesRes] = await Promise.allSettled([
        bridgeRequest<Partial<TextExpanderSettingsState>>('textExpander.getSettings', undefined, { silent: true }),
        bridgeRequest<unknown[]>('textExpander.listRules', undefined, { silent: true }),
      ]);

      if (settingsRes.status === 'fulfilled' && settingsRes.value && typeof settingsRes.value === 'object') {
        const s = settingsRes.value;
        setSettings({
          enabled: Boolean(s.enabled),
          autoExcludeSensitiveApps: s.autoExcludeSensitiveApps ?? true,
          restoreClipboard: s.restoreClipboard ?? true,
          excludedProcesses: Array.isArray(s.excludedProcesses)
            ? s.excludedProcesses.filter((v): v is string => typeof v === 'string')
            : [],
          defaultSensitiveProcesses:
            Array.isArray(s.defaultSensitiveProcesses) && s.defaultSensitiveProcesses.length > 0
              ? s.defaultSensitiveProcesses.filter((v): v is string => typeof v === 'string')
              : DEFAULT_SENSITIVE_PROCESSES,
        });
      }

      if (rulesRes.status === 'fulfilled' && Array.isArray(rulesRes.value)) {
        const parsed = rulesRes.value
          .map(normalizeRule)
          .filter((item): item is TextExpanderRuleItem => item !== null);
        setRules(parsed);
      }
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void loadData();
  }, [loadData]);

  useBridgeEvent('textExpander:stateChanged', (raw: unknown) => {
    const payload = raw && typeof raw === 'object' ? (raw as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      setSettings((prev) => ({ ...prev, enabled: payload.enabled as boolean }));
    }
  });

  useBridgeEvent('plugins:changed', (raw: unknown) => {
    const payload = raw && typeof raw === 'object' ? (raw as { id?: string; enabled?: boolean }) : null;
    if ((payload?.id === 'text_expander' || payload?.id === 'textexpander') && typeof payload.enabled === 'boolean') {
      setSettings((prev) => ({ ...prev, enabled: payload.enabled as boolean }));
    }
  });

  useBridgeEvent('textExpander:ruleHit', (raw: unknown) => {
    const payload = raw && typeof raw === 'object' ? (raw as { id?: string; hitCount?: number }) : null;
    if (payload?.id && typeof payload.hitCount === 'number') {
      setRules((prev) =>
        prev.map((rule) => (rule.id === payload.id ? { ...rule, hitCount: payload.hitCount as number } : rule)),
      );
    }
  });

  const patchSettings = useCallback(
    async (patch: Partial<TextExpanderSettingsState>) => {
      const previous = settings;
      const optimistic: TextExpanderSettingsState = { ...settings, ...patch };
      setSettings(optimistic);
      setSavingSettings(true);
      try {
        const res = await bridgeRequest<{ success?: boolean } & Partial<TextExpanderSettingsState>>(
          'textExpander.updateSettings',
          patch as Record<string, unknown>,
          { silent: true },
        );
        if (res?.success === false) {
          setSettings(previous);
          toast.error(t('textExpander.saveFailed', 'Failed to save text expander settings'));
          return false;
        }
        toast.success(t('general.toastSaved', 'Saved'));
        return true;
      } catch {
        setSettings(previous);
        toast.error(t('textExpander.saveFailed', 'Failed to save text expander settings'));
        return false;
      } finally {
        setSavingSettings(false);
      }
    },
    [settings, t],
  );

  // 非 WebView2 预览/测试环境下的本地沙盒展开辅助（原生 WebView2 下由 C++ 底层钩子实时完成）
  const handleSandboxChange = (e: ChangeEvent<HTMLTextAreaElement>) => {
    const nextValue = e.target.value;
    const isNativeHost = typeof window !== 'undefined' && Boolean(window.chrome?.webview);
    if (isNativeHost || !settings.enabled) {
      setSandboxText(nextValue);
      return;
    }

    const caret = e.target.selectionStart ?? nextValue.length;
    const prefix = nextValue.slice(0, caret);
    const suffix = nextValue.slice(caret);
    const activeRules = [...rules]
      .filter((r) => r.enabled && r.trigger.length > 0)
      .sort((a, b) => b.trigger.length - a.trigger.length);

    for (const rule of activeRules) {
      if (rule.mode === 'immediate') {
        const tail = prefix.slice(-rule.trigger.length);
        const matched =
          normalizeTriggerForMatch(tail, rule.caseSensitive, rule.fullwidthSensitive) ===
          normalizeTriggerForMatch(rule.trigger, rule.caseSensitive, rule.fullwidthSensitive);
        if (matched && prefix.length >= rule.trigger.length) {
          const expanded = expandMacrosForPreview(rule.replacement, activeLocale);
          const before = prefix.slice(0, prefix.length - rule.trigger.length);
          const merged = `${before}${expanded.text}${suffix}`;
          const nextCaret = before.length + expanded.text.length - expanded.cursorOffsetFromEnd;
          setSandboxText(merged);
          requestAnimationFrame(() => {
            sandboxRef.current?.setSelectionRange(nextCaret, nextCaret);
          });
          return;
        }
      } else if (prefix.length >= rule.trigger.length + 1) {
        const lastChar = prefix[prefix.length - 1];
        if (lastChar === ' ' || lastChar === '\t' || lastChar === '\n' || lastChar === '\u3000') {
          const body = prefix.slice(0, -1);
          const tail = body.slice(-rule.trigger.length);
          const matched =
            normalizeTriggerForMatch(tail, rule.caseSensitive, rule.fullwidthSensitive) ===
            normalizeTriggerForMatch(rule.trigger, rule.caseSensitive, rule.fullwidthSensitive);
          if (matched) {
            const expanded = expandMacrosForPreview(rule.replacement, activeLocale);
            const trailingDelim = rule.keepDelimiter ? lastChar : '';
            const before = body.slice(0, body.length - rule.trigger.length);
            const merged = `${before}${expanded.text}${trailingDelim}${suffix}`;
            const cursorBack =
              expanded.cursorIndex !== null
                ? expanded.cursorOffsetFromEnd + trailingDelim.length
                : 0;
            const nextCaret = before.length + expanded.text.length + trailingDelim.length - cursorBack;
            setSandboxText(merged);
            requestAnimationFrame(() => {
              sandboxRef.current?.setSelectionRange(nextCaret, nextCaret);
            });
            return;
          }
        }
      }
    }

    setSandboxText(nextValue);
  };

  const openCreateModal = () => {
    setDraft(createEmptyDraft());
    setFormError('');
    setEditorOpen(true);
  };

  const openEditModal = (rule: TextExpanderRuleItem) => {
    setDraft({
      id: rule.id,
      trigger: rule.trigger,
      replacement: rule.replacement,
      label: rule.label,
      enabled: rule.enabled,
      mode: rule.mode,
      caseSensitive: rule.caseSensitive,
      fullwidthSensitive: rule.fullwidthSensitive,
      keepDelimiter: rule.keepDelimiter,
    });
    setFormError('');
    setEditorOpen(true);
  };

  const handleInsertMacro = (token: string) => {
    const el = replacementRef.current;
    if (!el) {
      setDraft((prev) => ({ ...prev, replacement: prev.replacement + token }));
      return;
    }
    const start = el.selectionStart ?? draft.replacement.length;
    const end = el.selectionEnd ?? draft.replacement.length;
    const nextValue = `${draft.replacement.slice(0, start)}${token}${draft.replacement.slice(end)}`;
    setDraft((prev) => ({ ...prev, replacement: nextValue }));
    const nextPos = start + token.length;
    requestAnimationFrame(() => {
      el.focus();
      el.setSelectionRange(nextPos, nextPos);
    });
  };

  const validateDraft = (candidate: RuleFormDraft): string => {
    const trig = candidate.trigger;
    if (!trig.trim()) {
      return t('textExpander.errEmptyTrigger', 'Trigger keyword cannot be empty');
    }
    if (trig.length > MAX_TRIGGER_LENGTH) {
      return t('textExpander.errTriggerTooLong', 'Trigger keyword cannot exceed 64 characters');
    }
    if (/[\r\n]/.test(trig)) {
      return t('textExpander.errTriggerNewline', 'Trigger keyword cannot contain line breaks');
    }
    const dup = rules.some((r) => {
      if (candidate.id && r.id === candidate.id) return false;
      if (r.mode !== candidate.mode) return false;
      const strictCase = candidate.caseSensitive && r.caseSensitive;
      const strictFullwidth = candidate.fullwidthSensitive && r.fullwidthSensitive;
      return (
        normalizeTriggerForMatch(r.trigger, strictCase, strictFullwidth) ===
        normalizeTriggerForMatch(trig, strictCase, strictFullwidth)
      );
    });
    if (dup) {
      return t('textExpander.errDuplicateTrigger', 'A snippet with this trigger keyword already exists');
    }
    return '';
  };

  const handleSaveRule = async () => {
    const validationErr = validateDraft(draft);
    if (validationErr) {
      setFormError(validationErr);
      return;
    }

    setSavingRule(true);
    setFormError('');
    try {
      const res = await bridgeRequest<{
        success?: boolean;
        rules?: unknown[];
        rule?: unknown;
        error?: string;
      }>(
        'textExpander.upsertRule',
        {
          id: draft.id,
          trigger: draft.trigger,
          replacement: draft.replacement,
          label: draft.label.trim(),
          enabled: draft.enabled,
          mode: draft.mode,
          caseSensitive: draft.caseSensitive,
          fullwidthSensitive: draft.fullwidthSensitive,
          keepDelimiter: draft.keepDelimiter,
        },
        { silent: true },
      );

      if (!res?.success) {
        if (res?.error === 'duplicate_trigger') {
          setFormError(t('textExpander.errDuplicateTrigger', 'A snippet with this trigger keyword already exists'));
        } else {
          toast.error(t('textExpander.ruleSaveFailed', 'Failed to save snippet rule'));
        }
        return;
      }

      if (Array.isArray(res.rules)) {
        setRules(
          res.rules.map(normalizeRule).filter((item): item is TextExpanderRuleItem => item !== null),
        );
      } else if (res.rule) {
        const normalized = normalizeRule(res.rule);
        if (normalized) {
          setRules((prev) => {
            const exists = prev.some((r) => r.id === normalized.id);
            return exists ? prev.map((r) => (r.id === normalized.id ? normalized : r)) : [...prev, normalized];
          });
        }
      }

      setEditorOpen(false);
      toast.success(t('textExpander.ruleSavedToast', 'Snippet rule saved'));
    } catch {
      toast.error(t('textExpander.ruleSaveFailed', 'Failed to save snippet rule'));
    } finally {
      setSavingRule(false);
    }
  };

  const handleToggleRule = async (rule: TextExpanderRuleItem, enabled: boolean) => {
    const previous = rules;
    setRules((prev) => prev.map((r) => (r.id === rule.id ? { ...r, enabled } : r)));
    try {
      const res = await bridgeRequest<{ success?: boolean; rules?: unknown[] }>(
        'textExpander.toggleRule',
        { id: rule.id, enabled },
        { silent: true },
      );
      if (res?.success === false) {
        setRules(previous);
        toast.error(t('textExpander.ruleSaveFailed', 'Failed to save snippet rule'));
      } else if (Array.isArray(res?.rules)) {
        setRules(res.rules.map(normalizeRule).filter((item): item is TextExpanderRuleItem => item !== null));
      }
    } catch {
      setRules(previous);
      toast.error(t('textExpander.ruleSaveFailed', 'Failed to save snippet rule'));
    }
  };

  const handleDeleteRule = async (rule: TextExpanderRuleItem) => {
    const previous = rules;
    setRules((prev) => prev.filter((r) => r.id !== rule.id));
    try {
      const res = await bridgeRequest<{ success?: boolean; rules?: unknown[] }>(
        'textExpander.deleteRule',
        { id: rule.id },
        { silent: true },
      );
      if (res?.success === false) {
        setRules(previous);
        toast.error(t('textExpander.ruleDeleteFailed', 'Failed to delete snippet rule'));
        return;
      }
      if (Array.isArray(res?.rules)) {
        setRules(res.rules.map(normalizeRule).filter((item): item is TextExpanderRuleItem => item !== null));
      }
      toast.success(t('textExpander.ruleDeletedToast', 'Snippet rule deleted'));
    } catch {
      setRules(previous);
      toast.error(t('textExpander.ruleDeleteFailed', 'Failed to delete snippet rule'));
    }
  };

  const handleExportRules = async () => {
    try {
      const exported = await bridgeRequest<Record<string, unknown>>('textExpander.exportRules', undefined, {
        silent: true,
      });
      const payload = exported && typeof exported === 'object' && Array.isArray(exported.rules)
        ? exported
        : { version: 1, exportedAt: new Date().toISOString(), rules };

      const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement('a');
      anchor.href = url;
      anchor.download = 'tools3000-text-expander-rules.json';
      document.body.appendChild(anchor);
      anchor.click();
      document.body.removeChild(anchor);
      URL.revokeObjectURL(url);
      toast.success(t('textExpander.exportSuccessToast', 'Snippet rules exported'));
    } catch {
      toast.error(t('textExpander.exportFailedToast', 'Failed to export snippet rules'));
    }
  };

  const handleImportFileChange = async (e: ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;

    try {
      const text = await file.text();
      const parsed = JSON.parse(text) as unknown;
      const incoming = Array.isArray(parsed)
        ? parsed
        : (parsed && typeof parsed === 'object' && Array.isArray((parsed as { rules?: unknown[] }).rules)
            ? (parsed as { rules: unknown[] }).rules
            : null);

      if (!incoming) {
        toast.error(t('textExpander.importInvalidToast', 'Invalid JSON format: missing rules array'));
        return;
      }

      const res = await bridgeRequest<{ success?: boolean; imported?: number; rules?: unknown[] }>(
        'textExpander.importRules',
        { rules: incoming, mode: 'merge' },
        { silent: true },
      );

      if (!res?.success) {
        toast.error(t('textExpander.importFailedToast', 'Failed to import snippet rules'));
        return;
      }

      if (Array.isArray(res.rules)) {
        setRules(res.rules.map(normalizeRule).filter((item): item is TextExpanderRuleItem => item !== null));
      }
      toast.success(
        t('textExpander.importSuccessToast', {
          count: res.imported ?? incoming.length,
          defaultValue: `Imported ${res.imported ?? incoming.length} snippet rules`,
        }),
      );
    } catch {
      toast.error(t('textExpander.importInvalidToast', 'Invalid JSON format: missing rules array'));
    }
  };

  const filteredRules = useMemo(() => {
    const needle = searchQuery.trim().toLowerCase();
    if (!needle) return rules;
    return rules.filter(
      (r) =>
        r.trigger.toLowerCase().includes(needle) ||
        r.label.toLowerCase().includes(needle) ||
        r.replacement.toLowerCase().includes(needle),
    );
  }, [rules, searchQuery]);

  const sampleHintTriggers = useMemo(
    () => rules.filter((r) => r.enabled).slice(0, 5),
    [rules],
  );

  const savedExcludedCsv = settings.excludedProcesses.join(', ');
  const excludedDraft = excludedOverride ?? savedExcludedCsv;
  const excludedDirty = excludedDraft.trim() !== savedExcludedCsv.trim();

  if (loading) {
    return <PageSkeleton />;
  }

  return (
    <div className="text-expander-page">
      {/* 1. 顶部概览与总开关 */}
      <Card className="te-hero">
        <div className="te-hero__row">
          <div className="te-hero__left">
            <span className="te-hero__icon" aria-hidden="true">
              <TextCursorInput size={20} strokeWidth={2} />
            </span>
            <div className="te-hero__meta">
              <div className="te-hero__title-row">
                <h2 className="te-hero__title">
                  {t('textExpander.heroTitle', 'System-Wide Text Expander')}
                </h2>
                <span className={`te-status-pill ${settings.enabled ? 'te-status-pill--active' : ''}`}>
                  <span className="te-status-pill__dot" aria-hidden="true" />
                  <span>
                    {settings.enabled
                      ? t('textExpander.statusActive', 'Active')
                      : t('textExpander.statusDisabled', 'Disabled (Zero Hooks)')}
                  </span>
                </span>
              </div>
              <p className="te-hero__desc">
                {t(
                  'textExpander.heroDesc',
                  'Expand short keywords into full snippets, dynamic dates, clipboard templates, and cursor-positioned code blocks across any Windows app. Toggle off anytime with zero residual hooks.',
                )}
              </p>
              <div className="te-hero__badges">
                <CodeBadge>{t('textExpander.badgeOptIn', 'Zero Hooks When Off')}</CodeBadge>
                <CodeBadge variant="muted">{t('textExpander.badgePrivacy', 'Zero History Pollution')}</CodeBadge>
                <CodeBadge variant="muted">{t('textExpander.badgeMacros', 'Dynamic Macros')}</CodeBadge>
              </div>
            </div>
          </div>
          <div className="te-hero__switch">
            <Toggle
              id="text-expander-master-toggle"
              checked={settings.enabled}
              disabled={savingSettings}
              onChange={(enabled) => void patchSettings({ enabled })}
            />
          </div>
        </div>
      </Card>

      {/* 2. 短语规则管理卡片（首屏黄金位置） */}
      <SettingGroup
        title={t('textExpander.rulesGroup', 'Snippet Rules')}
        icon={<TextCursorInput size={18} strokeWidth={2.2} aria-hidden="true" />}
      >
        <Card>
          <div className="te-toolbar">
            <div className="te-search-box">
              <Search size={14} className="te-search-box__icon" aria-hidden="true" />
              <input
                type="text"
                className="te-search-box__input"
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
                placeholder={t('textExpander.searchPlaceholder', 'Search triggers, labels, or content...')}
                aria-label={t('textExpander.searchPlaceholder', 'Search triggers, labels, or content...')}
              />
              {searchQuery && (
                <button
                  type="button"
                  className="te-search-box__clear"
                  onClick={() => setSearchQuery('')}
                  aria-label={t('textExpander.clearSearch', 'Clear search')}
                >
                  <X size={13} aria-hidden="true" />
                </button>
              )}
            </div>

            <div className="te-toolbar__actions">
              <Button
                variant="ghost"
                size="sm"
                onClick={() => {
                  if (sandboxCollapsed) {
                    setSandboxCollapsed(false);
                    requestAnimationFrame(() => {
                      sandboxSectionRef.current?.scrollIntoView?.({ behavior: 'smooth', block: 'nearest' });
                      sandboxRef.current?.focus();
                    });
                  } else {
                    setSandboxCollapsed(true);
                  }
                }}
              >
                <Sparkles size={14} aria-hidden="true" />
                <span>
                  {sandboxCollapsed
                    ? t('textExpander.toggleSandboxBtn', 'Test Sandbox')
                    : t('textExpander.hideSandboxBtn', 'Hide Sandbox')}
                </span>
              </Button>
              <input
                ref={importInputRef}
                type="file"
                accept=".json,application/json"
                style={{ display: 'none' }}
                onChange={(e) => void handleImportFileChange(e)}
              />
              <Button variant="secondary" size="sm" onClick={() => importInputRef.current?.click()}>
                <Upload size={14} aria-hidden="true" />
                <span>{t('textExpander.importBtn', 'Import JSON')}</span>
              </Button>
              <Button variant="secondary" size="sm" onClick={() => void handleExportRules()}>
                <Download size={14} aria-hidden="true" />
                <span>{t('textExpander.exportBtn', 'Export JSON')}</span>
              </Button>
              <Button variant="primary" size="sm" onClick={openCreateModal}>
                <Plus size={14} aria-hidden="true" />
                <span>{t('textExpander.addRuleBtn', 'Add Snippet')}</span>
              </Button>
            </div>
          </div>

          {filteredRules.length === 0 ? (
            <div className="te-empty-state">
              <TextCursorInput size={24} className="te-empty-state__icon" aria-hidden="true" />
              <p className="te-empty-state__text">
                {searchQuery.trim()
                  ? t('textExpander.emptySearch', 'No snippet rules match your search.')
                  : t('textExpander.emptyRules', 'No snippet rules yet. Create your first shortcut phrase.')}
              </p>
              {!searchQuery.trim() && (
                <Button variant="primary" size="sm" onClick={openCreateModal}>
                  <Plus size={14} aria-hidden="true" />
                  <span>{t('textExpander.addRuleBtn', 'Add Snippet')}</span>
                </Button>
              )}
            </div>
          ) : (
            <div className="te-rules-list" role="list">
              {filteredRules.map((rule) => (
                <div
                  key={rule.id || rule.trigger}
                  role="listitem"
                  className={`te-rule-item ${rule.enabled ? '' : 'te-rule-item--disabled'}`}
                >
                  <div className="te-rule-item__main">
                    <div className="te-rule-item__top">
                      <CodeBadge variant="primary">{rule.trigger}</CodeBadge>
                      {rule.label && <span className="te-rule-item__label">{rule.label}</span>}
                      <span
                        className={`te-mode-chip ${rule.mode === 'immediate' ? 'te-mode-chip--immediate' : ''}`}
                      >
                        {rule.mode === 'immediate' ? (
                          <Zap size={12} aria-hidden="true" />
                        ) : (
                          <CornerDownLeft size={12} aria-hidden="true" />
                        )}
                        <span>
                          {rule.mode === 'immediate'
                            ? t('textExpander.modeImmediateShort', 'Instant')
                            : t('textExpander.modeDelimiterShort', 'Boundary')}
                        </span>
                      </span>
                      {rule.caseSensitive && (
                        <CodeBadge variant="muted" title={t('textExpander.caseSensitiveTitle', 'Case Sensitive')}>
                          Aa
                        </CodeBadge>
                      )}
                      {rule.fullwidthSensitive && (
                        <CodeBadge
                          variant="muted"
                          title={t('textExpander.fullwidthSensitiveTitle', 'Distinguish Fullwidth & Halfwidth')}
                        >
                          {t('textExpander.fullwidthBadge', 'Strict Width')}
                        </CodeBadge>
                      )}
                      <span className="te-rule-item__hits">
                        {t('textExpander.hitCount', {
                          count: rule.hitCount,
                          defaultValue: `${rule.hitCount} hits`,
                        })}
                      </span>
                    </div>
                    <p className="te-rule-item__preview" title={rule.replacement}>
                      {formatPreviewLine(rule.replacement)}
                    </p>
                  </div>

                  <div className="te-rule-item__controls">
                    <Toggle
                      id={`te-rule-toggle-${rule.id}`}
                      size="sm"
                      checked={rule.enabled}
                      onChange={(enabled) => void handleToggleRule(rule, enabled)}
                    />
                    <Button
                      variant="ghost"
                      size="sm"
                      onClick={() => openEditModal(rule)}
                      title={t('textExpander.editRule', 'Edit snippet')}
                      ariaLabel={t('textExpander.editRule', 'Edit snippet')}
                    >
                      <Pencil size={14} aria-hidden="true" />
                    </Button>
                    <Button
                      variant="ghost"
                      size="sm"
                      onClick={() => void handleDeleteRule(rule)}
                      title={t('textExpander.deleteRule', 'Delete snippet')}
                      ariaLabel={t('textExpander.deleteRule', 'Delete snippet')}
                    >
                      <Trash2 size={14} aria-hidden="true" />
                    </Button>
                  </div>
                </div>
              ))}
            </div>
          )}
        </Card>
      </SettingGroup>

      {/* 3. 实时体验沙盒（默认在规则列表下方完整渲染，支持一键收起/展开定位） */}
      <div ref={sandboxSectionRef} hidden={sandboxCollapsed}>
        <SettingGroup
          title={t('textExpander.sandboxGroup', 'Live Try-it Sandbox')}
          icon={<Sparkles size={18} strokeWidth={2.2} aria-hidden="true" />}
        >
          <Card>
            <div className="te-sandbox">
              {!settings.enabled && (
                <div className="te-sandbox__notice" role="status">
                  <span className="te-sandbox__notice-left">
                    <Lock size={15} className="te-sandbox__notice-icon" aria-hidden="true" />
                    <span>
                      {t(
                        'textExpander.sandboxDisabledHint',
                        'Enable the master switch above to test typing triggers live right here.',
                      )}
                    </span>
                  </span>
                  <Button
                    variant="primary"
                    size="sm"
                    disabled={savingSettings}
                    onClick={() => void patchSettings({ enabled: true })}
                  >
                    <span>{t('textExpander.enableNowBtn', 'Enable Now')}</span>
                  </Button>
                </div>
              )}

              <textarea
                ref={sandboxRef}
                id="text-expander-sandbox"
                className="te-sandbox__textarea"
                value={sandboxText}
                onChange={handleSandboxChange}
                placeholder={t(
                  'textExpander.sandboxPlaceholder',
                  'Click here and type a trigger such as ;date, ;now, or ;thx to see instant expansion...',
                )}
                aria-label={t('textExpander.sandboxAria', 'Live text expander sandbox')}
                spellCheck={false}
              />

              <div className="te-sandbox__footer">
                <div className="te-sandbox__hints">
                  <span>{t('textExpander.tryTriggersLabel', 'Try typing:')}</span>
                  {sampleHintTriggers.map((rule) => (
                    <CodeBadge key={rule.id || rule.trigger} variant="primary">
                      {rule.trigger}
                    </CodeBadge>
                  ))}
                </div>
                {sandboxText.length > 0 && (
                  <Button variant="ghost" size="sm" onClick={() => setSandboxText('')}>
                    <RotateCcw size={13} aria-hidden="true" />
                    <span>{t('textExpander.clearSandboxBtn', 'Clear')}</span>
                  </Button>
                )}
              </div>
            </div>
          </Card>
        </SettingGroup>
      </div>

      {/* 4. 隐私与防误触设置卡片 */}
      <SettingGroup
        title={t('textExpander.privacyGroup', 'Privacy & Anti-Mistype Guard')}
        icon={<ShieldCheck size={18} strokeWidth={2.2} aria-hidden="true" />}
      >
        <Card>
          <div className="te-privacy-banner" role="note">
            <ShieldCheck size={18} strokeWidth={2.2} className="te-privacy-banner__icon" aria-hidden="true" />
            <span>
              {t(
                'textExpander.privacyBanner',
                'Short ASCII snippets use zero-clipboard Unicode input. Multi-line or rich text paste attaches Windows privacy exclusion markers so expansions never pollute Tools3000 Clipboard History or Win+V.',
              )}
            </span>
          </div>

          <SettingRow
            label={t('textExpander.autoExcludeTitle', 'Auto-Skip Password Managers & Secure Terminals')}
            description={t(
              'textExpander.autoExcludeDesc',
              'Automatically pause phrase detection when password managers, remote desktop, or credential prompts are in focus.',
            )}
          >
            <Toggle
              id="te-auto-exclude-sensitive"
              checked={settings.autoExcludeSensitiveApps}
              disabled={savingSettings}
              onChange={(autoExcludeSensitiveApps) => void patchSettings({ autoExcludeSensitiveApps })}
            />
          </SettingRow>

          {settings.autoExcludeSensitiveApps && (
            <div className="te-sensitive-chips" aria-label={t('textExpander.protectedAppsAria', 'Built-in protected apps')}>
              {settings.defaultSensitiveProcesses.map((proc) => (
                <CodeBadge key={proc} variant="muted">
                  {proc}
                </CodeBadge>
              ))}
            </div>
          )}

          <SettingRow
            label={t('textExpander.restoreClipboardTitle', 'Restore Original Clipboard After Paste')}
            description={t(
              'textExpander.restoreClipboardDesc',
              'Silently restore your previous clipboard text 140ms after pasting multi-line or non-ASCII expansions.',
            )}
          >
            <Toggle
              id="te-restore-clipboard"
              checked={settings.restoreClipboard}
              disabled={savingSettings}
              onChange={(restoreClipboard) => void patchSettings({ restoreClipboard })}
            />
          </SettingRow>

          <SettingRow
            layout="vertical"
            label={t('textExpander.excludedProcessesTitle', 'Custom Excluded Processes')}
            description={t(
              'textExpander.excludedProcessesDesc',
              'Comma-separated executable names (e.g. games or terminals) where text expansion is always disabled.',
            )}
          >
            <div className="te-excluded-row">
              <div className="te-excluded-row__input">
                <TextInput
                  id="te-excluded-processes-input"
                  value={excludedDraft}
                  onChange={setExcludedOverride}
                  placeholder="valorant.exe, cs2.exe, game.exe"
                />
              </div>
              {excludedDirty && (
                <Button
                  variant="primary"
                  size="sm"
                  disabled={savingSettings}
                  onClick={async () => {
                    const ok = await patchSettings({ excludedProcesses: parseCsvProcessList(excludedDraft) });
                    if (ok) {
                      setExcludedOverride(null);
                    }
                  }}
                >
                  <Save size={14} aria-hidden="true" />
                  <span>{t('textExpander.saveExcludedBtn', 'Save')}</span>
                </Button>
              )}
            </div>
          </SettingRow>
        </Card>
      </SettingGroup>

      {/* 5. 新增 / 编辑短语表单弹窗 */}
      <Modal
        open={editorOpen}
        title={
          draft.id
            ? t('textExpander.modalEditTitle', 'Edit Snippet Rule')
            : t('textExpander.modalAddTitle', 'Add Snippet Rule')
        }
        onClose={() => setEditorOpen(false)}
        footer={
          <div className="te-form__footer">
            <Button variant="secondary" onClick={() => setEditorOpen(false)}>
              <span>{t('common.cancel', 'Cancel')}</span>
            </Button>
            <Button variant="primary" disabled={savingRule} onClick={() => void handleSaveRule()}>
              <Save size={14} aria-hidden="true" />
              <span>{t('textExpander.saveRuleBtn', 'Save Snippet')}</span>
            </Button>
          </div>
        }
      >
        <div className="te-form">
          <div className="te-form__grid">
            <Field
              label={t('textExpander.fieldTrigger', 'Trigger Keyword')}
              hint={t('textExpander.fieldTriggerHint', 'Prefix with ; or / to avoid accidental triggers')}
              error={formError || undefined}
            >
              <TextInput
                id="te-rule-trigger"
                value={draft.trigger}
                onChange={(trigger) => {
                  setDraft((prev) => ({ ...prev, trigger }));
                  if (formError) setFormError('');
                }}
                placeholder=";sig"
              />
            </Field>

            <Field
              label={t('textExpander.fieldLabel', 'Label (Optional)')}
              hint={t('textExpander.fieldLabelHint', 'Friendly name shown in the snippet list')}
            >
              <TextInput
                id="te-rule-label"
                value={draft.label}
                onChange={(label) => setDraft((prev) => ({ ...prev, label }))}
                placeholder={t('textExpander.fieldLabelPlaceholder', 'Email Signature')}
              />
            </Field>
          </div>

          <Field label={t('textExpander.fieldMode', 'Trigger Mode')}>
            <div className="te-mode-selector" role="radiogroup" aria-label={t('textExpander.fieldMode', 'Trigger Mode')}>
              <button
                type="button"
                role="radio"
                aria-checked={draft.mode === 'immediate'}
                className={`te-mode-option ${draft.mode === 'immediate' ? 'te-mode-option--active' : ''}`}
                onClick={() => setDraft((prev) => ({ ...prev, mode: 'immediate' }))}
              >
                <span className="te-mode-option__head">
                  <Zap size={14} aria-hidden="true" />
                  <span>{t('textExpander.modeImmediateTitle', 'Immediate')}</span>
                </span>
                <span className="te-mode-option__desc">
                  {t('textExpander.modeImmediateDesc', 'Expand immediately after typing the last character.')}
                </span>
              </button>

              <button
                type="button"
                role="radio"
                aria-checked={draft.mode === 'delimiter'}
                className={`te-mode-option ${draft.mode === 'delimiter' ? 'te-mode-option--active' : ''}`}
                onClick={() => setDraft((prev) => ({ ...prev, mode: 'delimiter' }))}
              >
                <span className="te-mode-option__head">
                  <CornerDownLeft size={14} aria-hidden="true" />
                  <span>{t('textExpander.modeDelimiterTitle', 'After Boundary Key')}</span>
                </span>
                <span className="te-mode-option__desc">
                  {t('textExpander.modeDelimiterDesc', 'Wait for Space, Tab, or Enter before expanding to prevent word-prefix mistypes.')}
                </span>
              </button>
            </div>
          </Field>

          {draft.mode === 'delimiter' && (
            <SubSettingGroup>
              <Toggle
                id="te-rule-keep-delimiter"
                label={t('textExpander.keepDelimiterTitle', 'Keep Boundary Key After Expansion')}
                description={t(
                  'textExpander.keepDelimiterDesc',
                  'Preserve the trailing Space, Tab, or Enter that triggered expansion for natural continuous typing.',
                )}
                checked={draft.keepDelimiter}
                onChange={(keepDelimiter) => setDraft((prev) => ({ ...prev, keepDelimiter }))}
              />
            </SubSettingGroup>
          )}

          <div className="te-match-toggles">
            <div className="te-match-toggle-card">
              <Toggle
                id="te-rule-case-sensitive"
                label={t('textExpander.caseSensitiveTitle', 'Case Sensitive')}
                description={t('textExpander.caseSensitiveDesc', 'Require exact uppercase and lowercase letter matching.')}
                checked={draft.caseSensitive}
                onChange={(caseSensitive) => setDraft((prev) => ({ ...prev, caseSensitive }))}
              />
            </div>

            <div className="te-match-toggle-card">
              <Toggle
                id="te-rule-fullwidth-sensitive"
                label={t('textExpander.fullwidthSensitiveTitle', 'Distinguish Fullwidth & Halfwidth')}
                description={t(
                  'textExpander.fullwidthSensitiveDesc',
                  'When off, fullwidth letters and CJK punctuation on the same key (e.g. ;sig) match seamlessly.',
                )}
                checked={draft.fullwidthSensitive}
                onChange={(fullwidthSensitive) => setDraft((prev) => ({ ...prev, fullwidthSensitive }))}
              />
            </div>
          </div>

          <Field label={t('textExpander.fieldReplacement', 'Expanded Content')}>
            <textarea
              ref={replacementRef}
              id="te-rule-replacement"
              className="te-form__textarea"
              value={draft.replacement}
              onChange={(e) => setDraft((prev) => ({ ...prev, replacement: e.target.value }))}
              placeholder={t('textExpander.fieldReplacementPlaceholder', 'Enter the expanded text or template...')}
              rows={4}
            />
          </Field>

          {draft.replacement.length > 0 && (() => {
            const preview = expandMacrosForPreview(draft.replacement, activeLocale);
            const hasCursor = preview.cursorIndex !== null;
            const beforeText = hasCursor ? preview.text.slice(0, preview.cursorIndex ?? 0) : preview.text;
            const afterText = hasCursor ? preview.text.slice(preview.cursorIndex ?? 0) : '';
            return (
              <div className="te-live-preview" role="status" aria-live="polite">
                <span className="te-live-preview__label">
                  {t('textExpander.livePreviewLabel', 'Live Expansion Preview')}
                </span>
                <div className="te-live-preview__box">
                  <span>{beforeText}</span>
                  {hasCursor && (
                    <span className="te-live-preview__cursor" aria-label={t('textExpander.macroCursor', 'Cursor')}>
                      |
                    </span>
                  )}
                  <span>{afterText}</span>
                </div>
              </div>
            );
          })()}

          <div className="te-macros">
            <div className="te-macros__head">
              <span className="te-macros__label">
                {t('textExpander.insertMacroTitle', 'Insert Dynamic Variable')}
              </span>
              <span className="te-macros__hint">
                {t('textExpander.livePreviewOffsetHint', {
                  example: '{{date+7d}}',
                  defaultValue: 'Tip: arbitrary day offsets like {{date+7d}} are also supported',
                })}
              </span>
            </div>
            <div className="te-macros__groups">
              {DYNAMIC_MACRO_GROUPS.map((group) => (
                <div key={group.groupId} className="te-macros__group-row">
                  <span className="te-macros__group-title">
                    {t(group.groupLabelKey, group.groupFallback)}
                  </span>
                  <div className="te-macros__bar">
                    {group.items.map((macro) => (
                      <button
                        key={macro.id}
                        type="button"
                        className="te-macro-pill"
                        onClick={() => handleInsertMacro(macro.token)}
                      >
                        <span className="te-macro-pill__token">{macro.token}</span>
                        <span>{t(macro.labelKey, macro.fallbackLabel)}</span>
                      </button>
                    ))}
                  </div>
                </div>
              ))}
            </div>
          </div>
        </div>
      </Modal>
    </div>
  );
};

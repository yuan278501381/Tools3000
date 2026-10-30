/* ─────────────────────────────────────────────────────────────────────────────
 * scopeModel — 作用域规则的共享类型 / 常量 / 纯函数
 * 与 C++ gesture/ScopeRule.h 的 JSON 结构对齐。
 * ───────────────────────────────────────────────────────────────────────────── */

export interface ScopeRule {
  id: string;
  name: string;
  enabled: boolean;
  processName: string;   // 进程名, 如 chrome.exe
  windowClass: string;   // 窗口类名, 如 Chrome_WidgetWin_1
  matchMode: number;     // 0=精确 1=通配符 2=正则
  effect: number;        // 0=启用 1=禁用 2=使用配置集
  profileName: string;   // effect===2 时生效
}

export const MATCH_MODE_KEYS = [
  'scope.matchExact', 'scope.matchWildcard', 'scope.matchRegex',
] as const;

export const EFFECT_KEYS = [
  'scope.effectEnable', 'scope.effectDisable', 'scope.effectProfile',
] as const;

/** 规则的匹配目标: 进程名优先, 否则窗口类名。 */
export function ruleTarget(r: ScopeRule): { kind: 'process' | 'class' | 'none'; value: string } {
  if (r.processName) return { kind: 'process', value: r.processName };
  if (r.windowClass) return { kind: 'class', value: r.windowClass };
  return { kind: 'none', value: '' };
}

export function makeRuleId(): string {
  // crypto.randomUUID 在现代浏览器 / WebView2 中可用
  if (typeof crypto !== 'undefined' && 'randomUUID' in crypto) return crypto.randomUUID();
  return 'rule_' + Math.random().toString(36).slice(2, 10);
}

export function emptyRule(): ScopeRule {
  return {
    id: makeRuleId(), name: '', enabled: true,
    processName: '', windowClass: '', matchMode: 0, effect: 0, profileName: '',
  };
}

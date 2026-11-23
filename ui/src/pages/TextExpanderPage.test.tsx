/* @vitest-environment jsdom */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, fireEvent, waitFor, cleanup } from '@testing-library/react';
import { TextExpanderPage, type TextExpanderRuleItem } from './TextExpanderPage';

const bridgeRequestMock = vi.fn();

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
  useBridgeEvent: vi.fn(),
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, opts?: string | { defaultValue?: string; count?: number }) => {
      if (typeof opts === 'string') return opts;
      if (opts && typeof opts === 'object' && opts.defaultValue) return opts.defaultValue;
      return key;
    },
  }),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
  },
}));

const INITIAL_RULES: TextExpanderRuleItem[] = [
  {
    id: 'sample_date',
    trigger: ';date',
    replacement: '{{date}}',
    label: 'Current Date',
    enabled: true,
    mode: 'immediate',
    caseSensitive: false,
    hitCount: 5,
  },
  {
    id: 'sample_now',
    trigger: ';now',
    replacement: '{{datetime}}',
    label: 'Current Date & Time',
    enabled: true,
    mode: 'immediate',
    caseSensitive: false,
    hitCount: 2,
  },
  {
    id: 'sample_thx',
    trigger: ';thx',
    replacement: 'Thanks for reaching out!',
    label: 'Quick Thanks',
    enabled: true,
    mode: 'delimiter',
    caseSensitive: false,
    hitCount: 0,
  },
];

describe('TextExpanderPage', () => {
  let rulesStore: TextExpanderRuleItem[];

  afterEach(() => cleanup());

  beforeEach(() => {
    rulesStore = INITIAL_RULES.map((r) => ({ ...r }));
    bridgeRequestMock.mockReset();
    bridgeRequestMock.mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'textExpander.getSettings') {
        return {
          enabled: false,
          autoExcludeSensitiveApps: true,
          restoreClipboard: true,
          excludedProcesses: ['game.exe'],
          defaultSensitiveProcesses: ['keepass.exe', '1password.exe', 'bitwarden.exe'],
        };
      }
      if (method === 'textExpander.listRules') {
        return rulesStore;
      }
      if (method === 'textExpander.updateSettings') {
        return { success: true, ...(params || {}) };
      }
      if (method === 'textExpander.toggleRule') {
        const id = String(params?.id ?? '');
        const enabled = Boolean(params?.enabled);
        rulesStore = rulesStore.map((r) => (r.id === id ? { ...r, enabled } : r));
        return { success: true, rules: rulesStore };
      }
      if (method === 'textExpander.upsertRule') {
        const next: TextExpanderRuleItem = {
          id: String(params?.id || 'rule_new_1'),
          trigger: String(params?.trigger || ''),
          replacement: String(params?.replacement || ''),
          label: String(params?.label || ''),
          enabled: params?.enabled !== false,
          mode: params?.mode === 'delimiter' ? 'delimiter' : 'immediate',
          caseSensitive: Boolean(params?.caseSensitive),
          hitCount: 0,
        };
        const exists = rulesStore.some((r) => r.id === next.id);
        rulesStore = exists ? rulesStore.map((r) => (r.id === next.id ? next : r)) : [...rulesStore, next];
        return { success: true, rule: next, rules: rulesStore };
      }
      if (method === 'textExpander.deleteRule') {
        const id = String(params?.id ?? '');
        rulesStore = rulesStore.filter((r) => r.id !== id);
        return { success: true, rules: rulesStore };
      }
      return { success: true };
    });
  });

  it('renders snippet rules, opt-in sandbox notice, and toggles master switch', async () => {
    render(<TextExpanderPage />);

    await waitFor(() => {
      expect(screen.getByText('Current Date')).toBeTruthy();
    });
    expect(screen.getByText('Quick Thanks')).toBeTruthy();
    expect(screen.getByText('5 hits')).toBeTruthy();
    expect(screen.getByText('Enable the master switch above to test typing triggers live right here.')).toBeTruthy();

    const masterToggle = document.getElementById('text-expander-master-toggle') as HTMLInputElement;
    expect(masterToggle).toBeTruthy();
    expect(masterToggle.checked).toBe(false);

    fireEvent.click(masterToggle);
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'textExpander.updateSettings',
        { enabled: true },
        { silent: true },
      );
    });
    expect(screen.queryByText('Enable the master switch above to test typing triggers live right here.')).toBeNull();

    // 切换单条规则开关
    const singleRuleToggle = document.getElementById('te-rule-toggle-sample_thx') as HTMLInputElement;
    expect(singleRuleToggle.checked).toBe(true);
    fireEvent.click(singleRuleToggle);
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'textExpander.toggleRule',
        { id: 'sample_thx', enabled: false },
        { silent: true },
      );
    });
  });

  it('opens add snippet modal, inserts dynamic macro pills, validates duplicates, and saves new rule', async () => {
    render(<TextExpanderPage />);

    await waitFor(() => {
      expect(screen.getByText('Current Date')).toBeTruthy();
    });

    fireEvent.click(screen.getByRole('button', { name: 'Add Snippet' }));

    const triggerInput = document.getElementById('te-rule-trigger') as HTMLInputElement;
    const labelInput = document.getElementById('te-rule-label') as HTMLInputElement;
    const replacementArea = document.getElementById('te-rule-replacement') as HTMLTextAreaElement;

    // 重复触发词前端即时校验
    fireEvent.change(triggerInput, { target: { value: ';DATE' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save Snippet' }));
    expect(screen.getByText('A snippet with this trigger keyword already exists')).toBeTruthy();

    // 填写合法新规则并通过胶囊按钮一键插入 {{date}} 与 {{cursor}}
    fireEvent.change(triggerInput, { target: { value: ';log' } });
    fireEvent.change(labelInput, { target: { value: 'Daily Log Header' } });
    fireEvent.change(replacementArea, { target: { value: 'Log ' } });
    replacementArea.setSelectionRange(4, 4);

    fireEvent.click(screen.getByRole('button', { name: /\{\{date\}\}/ }));
    fireEvent.click(screen.getByRole('button', { name: /\{\{cursor\}\}/ }));
    expect(replacementArea.value).toBe('Log {{date}}{{cursor}}');

    // 切换触发模式至 After Boundary Key
    fireEvent.click(screen.getByRole('radio', { name: /After Boundary Key/ }));

    fireEvent.click(screen.getByRole('button', { name: 'Save Snippet' }));

    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'textExpander.upsertRule',
        {
          id: '',
          trigger: ';log',
          replacement: 'Log {{date}}{{cursor}}',
          label: 'Daily Log Header',
          enabled: true,
          mode: 'delimiter',
          caseSensitive: false,
        },
        { silent: true },
      );
    });

    await waitFor(() => {
      expect(screen.getByText('Daily Log Header')).toBeTruthy();
    });
  });

  it('filters rules via search box and deletes a rule', async () => {
    render(<TextExpanderPage />);

    await waitFor(() => {
      expect(screen.getByText('Current Date')).toBeTruthy();
    });

    const searchInput = screen.getByPlaceholderText('Search triggers, labels, or content...');
    fireEvent.change(searchInput, { target: { value: ';thx' } });

    expect(screen.queryByText('Current Date')).toBeNull();
    expect(screen.getByText('Quick Thanks')).toBeTruthy();

    fireEvent.click(screen.getByRole('button', { name: 'Delete snippet' }));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'textExpander.deleteRule',
        { id: 'sample_thx' },
        { silent: true },
      );
    });
  });
});

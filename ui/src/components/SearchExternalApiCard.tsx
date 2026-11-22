/* ─────────────────────────────────────────────────────────────────────────────
 * SearchExternalApiCard — 外部 CLI 与 MCP (Model Context Protocol) 接入卡片
 *
 * 核心能力:
 *   1. 默认安全关闭的显式开关 (/search/externalApiEnabled)，遵循零信任原则；
 *   2. 内置敏感凭证保护提示 + 用户自定义敏感排除目录与允许根目录白名单；
 *   3. 自动探测本机 tools3000-cli.exe 真实路径，一键复制 Cursor / Claude Desktop /
 *      Antigravity / Claude Code MCP JSON 与常用 CLI 命令。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useMemo, type FC } from 'react';
import { Card, SettingGroup, SettingRow, Toggle, Badge, Button, TextInput, CodeBadge } from './UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { Terminal, ShieldCheck, Copy, Check, Save } from 'lucide-react';
import './SearchExternalApiCard.css';

export interface SearchExternalApiCardProps {
  externalApiEnabled: boolean;
  externalApiExcludePaths: string;
  externalApiAllowedRoots: string;
  onSaveSettings: (patch: {
    externalApiEnabled?: boolean;
    externalApiExcludePaths?: string;
    externalApiAllowedRoots?: string;
  }) => Promise<void>;
}

interface ExternalCliInfo {
  installed: boolean;
  executablePath: string;
  version: string;
}

type McpPresetId = 'standard' | 'claudeCode' | 'cli';

export const SearchExternalApiCard: FC<SearchExternalApiCardProps> = ({
  externalApiEnabled,
  externalApiExcludePaths,
  externalApiAllowedRoots,
  onSaveSettings,
}) => {
  const { t } = useTranslation();
  const [excludeOverride, setExcludeOverride] = useState<string | null>(null);
  const [rootsOverride, setRootsOverride] = useState<string | null>(null);
  const [savingPolicy, setSavingPolicy] = useState(false);
  const [copiedKey, setCopiedKey] = useState<string | null>(null);
  const [presetTab, setPresetTab] = useState<McpPresetId>('standard');
  const [cliInfo, setCliInfo] = useState<ExternalCliInfo>({
    installed: false,
    executablePath: 'C:\\Program Files\\Tools3000\\tools3000-cli.exe',
    version: '1.4.0',
  });

  const excludeDraft = excludeOverride ?? externalApiExcludePaths;
  const rootsDraft = rootsOverride ?? externalApiAllowedRoots;

  useEffect(() => {
    let cancelled = false;
    bridgeRequest<Partial<ExternalCliInfo>>('search.getExternalCliInfo', undefined, { silent: true })
      .then((res) => {
        if (!cancelled && res && typeof res.executablePath === 'string' && res.executablePath.length > 0) {
          setCliInfo({
            installed: Boolean(res.installed),
            executablePath: res.executablePath,
            version: res.version || '1.4.0',
          });
        }
      })
      .catch(() => undefined);
    return () => {
      cancelled = true;
    };
  }, []);

  const policyDirty =
    excludeDraft.trim() !== externalApiExcludePaths.trim() ||
    rootsDraft.trim() !== externalApiAllowedRoots.trim();

  const standardMcpJson = useMemo(() => {
    const config = {
      mcpServers: {
        'tools3000-search': {
          command: cliInfo.executablePath,
          args: ['mcp'],
        },
      },
    };
    return JSON.stringify(config, null, 2);
  }, [cliInfo.executablePath]);

  const claudeCodeCommand = useMemo(
    () => `claude mcp add tools3000-search -- "${cliInfo.executablePath}" mcp`,
    [cliInfo.executablePath],
  );

  const cliExamples = useMemo(
    () => [
      {
        id: 'file',
        desc: t('searchExternal.cliExampleFiles', 'Instant NTFS filename & pinyin search (JSON)'),
        cmd: `"${cliInfo.executablePath}" search "report 2026" --ext pdf,docx --limit 20 --json`,
      },
      {
        id: 'content',
        desc: t('searchExternal.cliExampleContent', 'Full-text document content search with snippet budget'),
        cmd: `"${cliInfo.executablePath}" content "contract clause" --scope "D:\\Docs" --max-snippet-chars 200 --json`,
      },
      {
        id: 'status',
        desc: t('searchExternal.cliExampleStatus', 'Check index status without waking idle service'),
        cmd: `"${cliInfo.executablePath}" status --no-start --json`,
      },
    ],
    [cliInfo.executablePath, t],
  );

  const copyText = async (key: string, text: string) => {
    try {
      await navigator.clipboard.writeText(text);
      setCopiedKey(key);
      toast.success(t('searchExternal.copiedToast', 'Copied to clipboard'));
      window.setTimeout(() => {
        setCopiedKey((prev) => (prev === key ? null : prev));
      }, 1800);
    } catch {
      toast.error(t('searchExternal.copyFailedToast', 'Copy failed'));
    }
  };

  const handleSavePolicy = async () => {
    if (savingPolicy) return;
    setSavingPolicy(true);
    try {
      await onSaveSettings({
        externalApiExcludePaths: excludeDraft.trim(),
        externalApiAllowedRoots: rootsDraft.trim(),
      });
      setExcludeOverride(null);
      setRootsOverride(null);
      toast.success(t('searchExternal.policySavedToast', 'External API security rules saved'));
    } finally {
      setSavingPolicy(false);
    }
  };

  return (
    <SettingGroup
      title={t('searchExternal.groupTitle', 'CLI & AI Agent (MCP) Interface')}
      icon={<Terminal size={18} strokeWidth={2.2} aria-hidden="true" />}
    >
      <Card>
        <div className="external-api-card">
          <SettingRow
            label={
              <div className="external-api-card__header-badges">
                <span>{t('searchExternal.enableTitle', 'Enable External CLI & MCP Search API')}</span>
                <Badge
                  text={
                    externalApiEnabled
                      ? t('searchExternal.badgeEnabled', 'Read-Only Active')
                      : t('searchExternal.badgeDisabled', 'Disabled by Default')
                  }
                  variant={externalApiEnabled ? 'success' : 'muted'}
                />
                <Badge text={t('searchExternal.badgeRust', 'Rust Zero-Dep')} variant="primary" />
              </div>
            }
            description={t(
              'searchExternal.enableDesc',
              'Expose read-only NTFS file & document content search to terminal pipelines and AI coding agents (Cursor, Claude Desktop, Claude Code, Antigravity) via tools3000-cli.exe.',
            )}
          >
            <Toggle
              id="search-external-api-toggle"
              checked={externalApiEnabled}
              onChange={(checked) => void onSaveSettings({ externalApiEnabled: checked })}
            />
          </SettingRow>

          <div className="external-api-card__security-banner" role="note">
            <ShieldCheck size={18} strokeWidth={2.2} className="external-api-card__security-icon" aria-hidden="true" />
            <span>
              {t(
                'searchExternal.securityNotice',
                'Zero-Trust Protection: Strictly read-only, DEMAND_START preserved (MCP handshake never wakes the background service), and sensitive credential paths (.ssh, .aws, .env, browser passwords, private keys) are permanently blocked on both CLI and Service sides.',
              )}
            </span>
          </div>

          <SettingRow
            layout="vertical"
            label={t('searchExternal.policyTitle', 'Access Scope & Extra Exclusions')}
            description={t(
              'searchExternal.policyDesc',
              'Optionally restrict external agents to specific workspace directories, or add extra excluded paths (comma-separated).',
            )}
          >
            <div className="external-api-card__fields">
              <div className="external-api-card__field">
                <label htmlFor="external-api-allowed-roots" className="external-api-card__field-label">
                  {t('searchExternal.allowedRootsLabel', 'Allowed Root Directories (Optional Whitelist)')}
                </label>
                <TextInput
                  id="external-api-allowed-roots"
                  value={rootsDraft}
                  onChange={setRootsOverride}
                  placeholder={t('searchExternal.allowedRootsPlaceholder', 'Leave empty for all indexed drives, e.g. C:\\repo, D:\\Docs')}
                />
                <span className="external-api-card__field-hint">
                  {t('searchExternal.allowedRootsHint', 'When specified, external CLI/MCP queries can only return files inside these directories.')}
                </span>
              </div>

              <div className="external-api-card__field">
                <label htmlFor="external-api-exclude-paths" className="external-api-card__field-label">
                  {t('searchExternal.extraExcludesLabel', 'Additional Excluded Directories / Patterns')}
                </label>
                <TextInput
                  id="external-api-exclude-paths"
                  value={excludeDraft}
                  onChange={setExcludeOverride}
                  placeholder={t('searchExternal.extraExcludesPlaceholder', 'e.g. D:\\Private, node_modules, .venv')}
                />
                <span className="external-api-card__field-hint">
                  {t('searchExternal.extraExcludesHint', 'Stacked on top of built-in credential protection and global search exclusions.')}
                </span>
              </div>
            </div>

            {policyDirty && (
              <div className="external-api-card__save-row">
                <Button
                  variant="primary"
                  disabled={savingPolicy}
                  onClick={() => void handleSavePolicy()}
                >
                  <Save size={14} aria-hidden="true" />
                  <span>{t('searchExternal.savePolicyBtn', 'Save Access Rules')}</span>
                </Button>
              </div>
            )}
          </SettingRow>

          <SettingRow
            layout="vertical"
            label={t('searchExternal.connectTitle', 'Quick Integration & One-Click MCP Config')}
            description={t(
              'searchExternal.connectDesc',
              'Copy the ready-to-use configuration pointing directly to your local tools3000-cli.exe executable.',
            )}
          >
            <div className="external-api-card__snippet-box">
              <div className="external-api-card__snippet-bar">
                <div className="external-api-card__tabs" role="tablist" aria-label={t('searchExternal.presetTabsAria', 'Integration presets')}>
                  <button
                    type="button"
                    role="tab"
                    aria-selected={presetTab === 'standard'}
                    className={`external-api-card__tab-btn${presetTab === 'standard' ? ' external-api-card__tab-btn--active' : ''}`}
                    onClick={() => setPresetTab('standard')}
                  >
                    {t('searchExternal.tabMcpJson', 'Cursor / Claude Desktop / Antigravity')}
                  </button>
                  <button
                    type="button"
                    role="tab"
                    aria-selected={presetTab === 'claudeCode'}
                    className={`external-api-card__tab-btn${presetTab === 'claudeCode' ? ' external-api-card__tab-btn--active' : ''}`}
                    onClick={() => setPresetTab('claudeCode')}
                  >
                    {t('searchExternal.tabClaudeCode', 'Claude Code CLI')}
                  </button>
                  <button
                    type="button"
                    role="tab"
                    aria-selected={presetTab === 'cli'}
                    className={`external-api-card__tab-btn${presetTab === 'cli' ? ' external-api-card__tab-btn--active' : ''}`}
                    onClick={() => setPresetTab('cli')}
                  >
                    {t('searchExternal.tabTerminal', 'Terminal CLI')}
                  </button>
                </div>

                {presetTab !== 'cli' && (
                  <Button
                    variant="secondary"
                    onClick={() =>
                      void copyText(
                        presetTab,
                        presetTab === 'standard' ? standardMcpJson : claudeCodeCommand,
                      )
                    }
                  >
                    {copiedKey === presetTab ? (
                      <Check size={14} aria-hidden="true" />
                    ) : (
                      <Copy size={14} aria-hidden="true" />
                    )}
                    <span>
                      {copiedKey === presetTab
                        ? t('searchExternal.copiedBtn', 'Copied')
                        : t('searchExternal.copyConfigBtn', 'Copy Config')}
                    </span>
                  </Button>
                )}
              </div>

              {presetTab === 'standard' && (
                <pre className="external-api-card__code">{standardMcpJson}</pre>
              )}

              {presetTab === 'claudeCode' && (
                <pre className="external-api-card__code">{claudeCodeCommand}</pre>
              )}

              {presetTab === 'cli' && (
                <div className="external-api-card__cli-list">
                  {cliExamples.map((ex) => (
                    <div key={ex.id} className="external-api-card__cli-item">
                      <div className="external-api-card__cli-main">
                        <span className="external-api-card__cli-desc">{ex.desc}</span>
                        <div className="external-api-card__cli-cmd-scroll">
                          <CodeBadge>{ex.cmd}</CodeBadge>
                        </div>
                      </div>
                      <Button
                        variant="secondary"
                        onClick={() => void copyText(`cli-${ex.id}`, ex.cmd)}
                      >
                        {copiedKey === `cli-${ex.id}` ? (
                          <Check size={14} aria-hidden="true" />
                        ) : (
                          <Copy size={14} aria-hidden="true" />
                        )}
                        <span>{t('searchExternal.copyBtn', 'Copy')}</span>
                      </Button>
                    </div>
                  ))}
                </div>
              )}

              <div className="external-api-card__cli-item">
                <span className="external-api-card__cli-desc">
                  {t('searchExternal.binaryPathLabel', 'Detected CLI Binary:')}
                </span>
                <CodeBadge>{cliInfo.executablePath}</CodeBadge>
              </div>
            </div>
          </SettingRow>
        </div>
      </Card>
    </SettingGroup>
  );
};

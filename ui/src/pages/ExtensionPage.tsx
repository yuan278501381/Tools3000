import { useState, useCallback, useMemo, useEffect, useRef, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Bot,
  Pipette,
  ClipboardList,
  FileCode2,
  ShieldCheck,
  Zap,
  Trash2,
  CheckCircle2,
  Sparkles,
  Play,
  X,
  Copy,
  Keyboard,
  Compass,
  Lightbulb,
  FileText,
  HelpCircle,
} from 'lucide-react';
import { toast } from 'sonner';
import { Card, Toggle, Button, Badge, CodeBadge, ConfirmModal } from '../components/UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import type { PluginStatus } from './PluginsPage';
import './ExtensionPage.css';

const EXTENSION_ICONS = {
  ai_assistant: Bot,
  color_picker: Pipette,
  clipboard_manager: ClipboardList,
  markdown_preview: FileCode2,
} as const;

interface ExtensionGuideInfo {
  triggerKey: string;
  triggerKeyArray: string[];
  conditionKey: string;
  conditionEn: string;
  steps: {
    step: number;
    titleKey: string;
    titleEn: string;
    descKey: string;
    descEn: string;
    kbd?: string[];
  }[];
  proTips: {
    titleKey: string;
    titleEn: string;
    descKey: string;
    descEn: string;
  }[];
  supportedFormats?: string[];
  supportedFormatKeys?: string[];
}

const EXTENSION_GUIDES: Record<string, ExtensionGuideInfo> = {
  markdown_preview: {
    triggerKey: 'Space',
    triggerKeyArray: ['Space'],
    conditionKey: 'extGuide.mdCondition',
    conditionEn: 'Select any .md, code, or text file in File Explorer or Desktop',
    steps: [
      {
        step: 1,
        titleKey: 'extGuide.stepQuickLook1Title',
        titleEn: 'Select Target File',
        descKey: 'extGuide.mdStep1Desc',
        descEn: 'Click once to highlight any Markdown or source code file in Explorer.',
      },
      {
        step: 2,
        titleKey: 'extGuide.stepQuickLook2Title',
        titleEn: 'Press Spacebar',
        descKey: 'extGuide.mdStep2Desc',
        descEn: 'Press Spacebar once without double clicking heavy editors.',
        kbd: ['Space'],
      },
      {
        step: 3,
        titleKey: 'extGuide.mdStep3Title',
        titleEn: 'Instant QuickLook',
        descKey: 'extGuide.mdStep3Desc',
        descEn: 'Instant floating QuickLook preview with code highlighting & LaTeX.',
      },
    ],
    proTips: [
      {
        titleKey: 'extGuide.mdTip1Title',
        titleEn: 'Fast Close',
        descKey: 'extGuide.mdTip1Desc',
        descEn: 'Press Space or Esc again to dismiss the preview instantly.',
      },
      {
        titleKey: 'extGuide.mdTip2Title',
        titleEn: 'Copy & Open in Editor',
        descKey: 'extGuide.mdTip2Desc',
        descEn: 'Quickly copy rendered content or open in your default IDE from top bar.',
      },
      {
        titleKey: 'extGuide.mdTip3Title',
        titleEn: 'Math & Tables Support',
        descKey: 'extGuide.mdTip3Desc',
        descEn: 'Full support for GitHub flavored markdown, KaTeX math and formatted tables.',
      },
    ],
    supportedFormats: ['.md', '.markdown', '.json', '.js', '.ts', '.py', '.cpp', '.rs', '.sql', '.txt'],
  },
  ai_assistant: {
    triggerKey: 'Alt + X',
    triggerKeyArray: ['Alt', 'X'],
    conditionKey: 'extGuide.aiCondition',
    conditionEn: 'Select text anywhere or press shortcut to trigger globally',
    steps: [
      {
        step: 1,
        titleKey: 'extGuide.aiStep1Title',
        titleEn: 'Select Text or Global Call',
        descKey: 'extGuide.aiStep1Desc',
        descEn: 'Highlight any text to translate, rewrite, or ask questions.',
      },
      {
        step: 2,
        titleKey: 'extGuide.aiStep2Title',
        titleEn: 'Press Activation Hotkey',
        descKey: 'extGuide.aiStep2Desc',
        descEn: 'Press Alt + X to pop up the intelligent AI dialogue box.',
        kbd: ['Alt', 'X'],
      },
      {
        step: 3,
        titleKey: 'extGuide.aiStep3Title',
        titleEn: 'Get Instant Answers',
        descKey: 'extGuide.aiStep3Desc',
        descEn: 'Enjoy stream generation, translation, code refactor and smart answers.',
      },
    ],
    proTips: [
      {
        titleKey: 'extGuide.aiTip1Title',
        titleEn: 'Context Awareness',
        descKey: 'extGuide.aiTip1Desc',
        descEn: 'Automatically detects code snippets and provides tailored reviews.',
      },
      {
        titleKey: 'extGuide.aiTip2Title',
        titleEn: 'Prompt Templates',
        descKey: 'extGuide.aiTip2Desc',
        descEn: 'Press Tab to cycle between preset prompt templates quickly.',
      },
    ],
    supportedFormatKeys: [
      'extGuide.formatPlainText',
      'extGuide.formatCodeSnippet',
      'extGuide.formatArticleSummary',
      'extGuide.formatMultiLangTranslation',
    ],
  },
  color_picker: {
    triggerKey: 'Alt + C',
    triggerKeyArray: ['Alt', 'C'],
    conditionKey: 'extGuide.colorCondition',
    conditionEn: 'Pixel precision color picking anywhere on screen (Multi-DPI ready)',
    steps: [
      {
        step: 1,
        titleKey: 'extGuide.colorStep1Title',
        titleEn: 'Activate Magnifier',
        descKey: 'extGuide.colorStep1Desc',
        descEn: 'Press Alt + C to turn cursor into an 8x pixel magnifier.',
        kbd: ['Alt', 'C'],
      },
      {
        step: 2,
        titleKey: 'extGuide.colorStep2Title',
        titleEn: 'Lock Pixel',
        descKey: 'extGuide.colorStep2Desc',
        descEn: 'Hover over target pixel; use arrow keys for 1-pixel precision nudging.',
      },
      {
        step: 3,
        titleKey: 'extGuide.colorStep3Title',
        titleEn: 'Click to Copy',
        descKey: 'extGuide.colorStep3Desc',
        descEn: 'Click left button to copy HEX code and record to your color palette.',
      },
    ],
    proTips: [
      {
        titleKey: 'extGuide.colorTip1Title',
        titleEn: 'Color Spaces',
        descKey: 'extGuide.colorTip1Desc',
        descEn: 'Press Space to toggle between HEX, RGB, HSL and CMYK output formats.',
      },
      {
        titleKey: 'extGuide.colorTip2Title',
        titleEn: 'Zoom Factor',
        descKey: 'extGuide.colorTip2Desc',
        descEn: 'Scroll mouse wheel to smoothly adjust magnification between 2x and 16x.',
      },
    ],
    supportedFormats: ['HEX (#FFFFFF)', 'RGB (rgb(255,255,255))', 'HSL', 'CMYK'],
  },
  clipboard_manager: {
    triggerKey: 'Alt + V',
    triggerKeyArray: ['Alt', 'V'],
    conditionKey: 'extGuide.clipCondition',
    conditionEn: 'Call clipboard history panel in any text input field or active window',
    steps: [
      {
        step: 1,
        titleKey: 'extGuide.clipStep0Title',
        titleEn: 'Copy As Usual',
        descKey: 'extGuide.clipStep0Desc',
        descEn: 'Copy text, code, URLs, or screenshots using Ctrl + C as usual.',
        kbd: ['Ctrl', 'C'],
      },
      {
        step: 2,
        titleKey: 'extGuide.stepClip1Title',
        titleEn: 'Open Clipboard Hub',
        descKey: 'extGuide.stepClip1Desc',
        descEn: 'Press Alt + V at the destination input box.',
        kbd: ['Alt', 'V'],
      },
      {
        step: 3,
        titleKey: 'extGuide.stepClip2Title',
        titleEn: 'Search & Paste',
        descKey: 'extGuide.stepClip2Desc',
        descEn: 'Filter history by typing pinyin/keywords; press 1-9 or Enter to paste.',
      },
    ],
    proTips: [
      {
        titleKey: 'extGuide.stepClip3Title',
        titleEn: 'Pin Snippets',
        descKey: 'extGuide.stepClip3Desc',
        descEn: 'Click star icon on any entry to pin favorite snippets permanently.',
      },
      {
        titleKey: 'extGuide.stepClip4Title',
        titleEn: 'Plain Text Paste',
        descKey: 'extGuide.stepClip4Desc',
        descEn: 'Press Shift + Enter to strip formatting and paste clean plain text.',
      },
    ],
    supportedFormatKeys: ['extGuide.formatRichText', 'extGuide.formatPlainText', 'extGuide.formatImage', 'extGuide.formatFileList', 'extGuide.formatCodeSnippet'],
  },
};

interface ExtensionPageProps {
  pluginId: string;
  plugin?: PluginStatus;
  onUninstall?: (id: string) => void;
}

export const ExtensionPage: FC<ExtensionPageProps> = ({ pluginId, plugin, onUninstall }) => {
  const { t } = useTranslation();
  const [enabledOverrides, setEnabledOverrides] = useState<Record<string, boolean>>({});
  const enabled = enabledOverrides[pluginId] ?? plugin?.enabled ?? true;
  const [saving, setSaving] = useState(false);
  const [uninstalling, setUninstalling] = useState(false);
  const [showPlayground, setShowPlayground] = useState(false);
  const [demoCopied, setDemoCopied] = useState(false);
  const modalCloseBtnRef = useRef<HTMLButtonElement | null>(null);
  const previousFocusRef = useRef<HTMLElement | null>(null);

  useEffect(() => {
    if (!showPlayground) return;
    previousFocusRef.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const focusTimer = window.setTimeout(() => {
      modalCloseBtnRef.current?.focus();
    }, 0);
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        setShowPlayground(false);
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => {
      window.clearTimeout(focusTimer);
      window.removeEventListener('keydown', handleKeyDown);
      if (previousFocusRef.current && document.contains(previousFocusRef.current)) {
        previousFocusRef.current.focus();
      }
    };
  }, [showPlayground]);

  const resolveText = useCallback((key: string, enFallback: string) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    return t(key as any, { defaultValue: enFallback });
  }, [t]);

  const IconComponent = (pluginId in EXTENSION_ICONS)
    ? EXTENSION_ICONS[pluginId as keyof typeof EXTENSION_ICONS]
    : Zap;

  const guide = useMemo(() => {
    return EXTENSION_GUIDES[pluginId] || {
      triggerKey: 'Alt + Space',
      triggerKeyArray: ['Alt', 'Space'],
      conditionKey: 'extGuide.conditionGlobal',
      conditionEn: 'Works across all active windows or desktop environment',
      steps: [
        {
          step: 1,
          titleKey: 'extGuide.stepGeneral1Title',
          titleEn: 'Prepare Target',
          descKey: 'extGuide.stepGeneral1Desc',
          descEn: 'Ensure the extension is currently enabled in settings.',
        },
        {
          step: 2,
          titleKey: 'extGuide.stepGeneral2Title',
          titleEn: 'Trigger Hotkey',
          descKey: 'extGuide.stepGeneral2Desc',
          descEn: 'Use the bound global hotkey to trigger capabilities.',
          kbd: ['Alt', 'Space'],
        },
        {
          step: 3,
          titleKey: 'extGuide.stepGeneral3Title',
          titleEn: 'Enjoy Efficiency',
          descKey: 'extGuide.stepGeneral3Desc',
          descEn: 'Experience instant response and enhanced productivity.',
        },
      ],
      proTips: [
        {
          titleKey: 'extGuide.stepGeneral4Title',
          titleEn: 'Daemon Ready',
          descKey: 'extGuide.stepGeneral4Desc',
          descEn: 'Keep alive in background for sub-millisecond response.',
        },
      ],
      supportedFormatKeys: ['extGuide.formatGlobalInteraction'],
    };
  }, [pluginId]);

  const resolvedFormats = useMemo(() => {
    if (guide.supportedFormatKeys && guide.supportedFormatKeys.length > 0) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return guide.supportedFormatKeys.map((k) => t(k as any));
    }
    return guide.supportedFormats && guide.supportedFormats.length > 0 ? guide.supportedFormats : ['.*'];
  }, [guide, t]);

  const handleToggle = useCallback(async (next: boolean) => {
    setEnabledOverrides((prev) => ({ ...prev, [pluginId]: next }));
    setSaving(true);
    try {
      await bridgeRequest('plugins.setEnabled', { id: pluginId, enabled: next });
      toast.success(next ? t('plugins.enabledSaved') : t('plugins.disabledSaved'));
    } catch (err) {
      setEnabledOverrides((prev) => ({ ...prev, [pluginId]: !next }));
      toast.error(t('plugins.saveFailed'), { description: String(err) });
    } finally {
      setSaving(false);
    }
  }, [pluginId, t]);

  const [confirmUninstallOpen, setConfirmUninstallOpen] = useState(false);

  const handleUninstall = useCallback(async () => {
    setConfirmUninstallOpen(false);
    setUninstalling(true);
    try {
      await bridgeRequest('plugins.uninstall', { id: pluginId });
      toast.success(t('extension.uninstallSuccess'));
      onUninstall?.(pluginId);
    } catch (err) {
      toast.error(String(err));
    } finally {
      setUninstalling(false);
    }
  }, [pluginId, onUninstall, t]);

  const handleCopyDemoCode = useCallback(() => {
    const commentText = t('extGuide.mdDemoCodeComment', 'Summon the instant Markdown rendering engine');
    void navigator.clipboard?.writeText(`\`\`\`typescript\nimport { tools3000 } from "@tools3000/sdk";\n\n// ${commentText}\nconst preview = tools3000.preview("README.md");\n\`\`\``)?.catch(() => {});
    setDemoCopied(true);
    toast.success(t('extension.copySuccess'));
    setTimeout(() => setDemoCopied(false), 2000);
  }, [t]);

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const subtitleKey = `navSubtitle.${pluginId}` as any;

  return (
    <div className="extension-page">
      {/* ── 顶部就绪与演练横幅 ───────────────────────────────────────── */}
      <div className="extension-ready-banner">
        <div className="extension-ready-icon-box">
          <Sparkles size={18} />
        </div>
        <div className="extension-ready-text">
          <strong>{t('extension.installedReadyBanner')}</strong>
          <span>{resolveText(guide.conditionKey, guide.conditionEn)}</span>
        </div>
        <Button
          variant="primary"
          onClick={() => setShowPlayground(true)}
          className="extension-playground-btn"
        >
          <Play size={14} fill="currentColor" />
          {t('extension.tryPlayground')}
        </Button>
      </div>

      {/* ── 头部概览卡片 ─────────────────────────────────────────── */}
      <Card className="extension-hero-card">
        <div className="extension-hero-content">
          <div className="extension-hero-icon-wrap">
            <IconComponent size={36} className="extension-hero-icon" />
          </div>
          <div className="extension-hero-info">
            <div className="extension-hero-title-row">
              <h2 className="extension-hero-title">{plugin?.name || pluginId}</h2>
              <Badge
                text={enabled ? t('extension.statusRunning') : t('extension.statusDisabled')}
                variant={enabled ? 'success' : 'muted'}
              />
              <Badge text={plugin?.version ? `v${plugin.version}` : '—'} variant="primary" />
            </div>
            <p className="extension-hero-desc">
              {t(subtitleKey, { defaultValue: plugin?.fileName || 'Tools3000 Extension Module' })}
            </p>
          </div>
          <div className="extension-hero-toggle">
            <Toggle
              id="ext-hero-toggle"
              checked={enabled}
              onChange={handleToggle}
              disabled={saving}
            />
          </div>
        </div>
      </Card>

      {/* ── 核心三步快速上手向导 (Quick Start 3-Steps) ────────────────── */}
      <Card
        title={t('extension.quickStart')}
        subtitle={t('extension.quickStartDesc')}
        className="extension-guide-card"
        headerAction={
          <span className="extension-trigger-badge">
            <Keyboard size={13} />
            {t('extension.triggerCondition')}: <strong>{guide.triggerKey}</strong>
          </span>
        }
      >
        <div className="extension-steps-container">
          {guide.steps.map((item) => (
            <div key={item.step} className="extension-step-card">
              <div className="extension-step-header">
                <span className="extension-step-number">{item.step}</span>
                <h4 className="extension-step-title">{resolveText(item.titleKey, item.titleEn)}</h4>
              </div>
              <p className="extension-step-desc">{resolveText(item.descKey, item.descEn)}</p>
              {item.kbd && item.kbd.length > 0 && (
                <div className="extension-step-kbd-row">
                  {item.kbd.map((k, idx) => (
                    <span key={idx} className="extension-kbd-item">
                      <kbd>{k}</kbd>
                      {idx < item.kbd!.length - 1 && <span className="extension-kbd-plus">+</span>}
                    </span>
                  ))}
                </div>
              )}
            </div>
          ))}
        </div>
      </Card>

      {/* ── 极客技巧与格式支持 ──────────────────────────────────────── */}
      <div className="extension-grid">
        <Card
          title={t('extension.proTips')}
          className="extension-info-card"
          headerAction={<Lightbulb size={16} className="text-primary" />}
        >
          <div className="extension-tips-list">
            {guide.proTips.map((tip, idx) => (
              <div key={idx} className="extension-tip-item">
                <div className="extension-tip-dot" />
                <div>
                  <strong className="extension-tip-title">{resolveText(tip.titleKey, tip.titleEn)}:</strong>{' '}
                  <span className="extension-tip-desc">{resolveText(tip.descKey, tip.descEn)}</span>
                </div>
              </div>
            ))}
          </div>
        </Card>

        <Card
          title={t('extension.supportedFormats')}
          className="extension-info-card"
          headerAction={<Compass size={16} className="text-primary" />}
        >
          <div className="extension-formats-wrap">
            {resolvedFormats.map((fmt) => (
              <span key={fmt} className="extension-fmt-tag">
                <FileText size={12} />
                {fmt}
              </span>
            ))}
          </div>
        </Card>
      </div>

      {/* ── 能力与沙箱安全 ───────────────────────────────────────── */}
      <div className="extension-grid">
        <Card title={t('extension.capabilities')} className="extension-info-card">
          <div className="extension-badges-wrap">
            {(plugin?.capabilities || ['api-bridge', 'native-hook']).map((cap) => (
              <span key={cap} className="extension-cap-tag">
                <Sparkles size={13} />
                {cap}
              </span>
            ))}
          </div>
        </Card>

        <Card title={t('extension.permissions')} className="extension-info-card">
          <div className="extension-perms-list">
            {(plugin?.permissions || ['clipboard', 'storage']).map((perm) => (
              <div key={perm} className="extension-perm-item">
                <ShieldCheck size={16} className="extension-perm-icon" />
                <span className="extension-perm-label">{perm}</span>
                <CheckCircle2 size={14} className="extension-perm-ok" />
              </div>
            ))}
          </div>
        </Card>
      </div>

      {/* ── 模块管理与卸载 ───────────────────────────────────────── */}
      <Card title={t('extension.settings')} className="extension-settings-card">
        <div className="extension-setting-footer" style={{ borderTop: 'none', paddingTop: 0 }}>
          <Button
            variant="danger"
            onClick={() => setConfirmUninstallOpen(true)}
            disabled={uninstalling}
          >
            <span style={{ display: 'inline-flex', alignItems: 'center', gap: '6px' }}>
              <Trash2 size={16} />
              {t('extension.uninstall')}
            </span>
          </Button>
        </div>
      </Card>

      <ConfirmModal
        open={confirmUninstallOpen}
        title={t('extension.uninstall')}
        description={t('extension.uninstallConfirm')}
        confirmText={t('extension.uninstall')}
        loading={uninstalling}
        onConfirm={handleUninstall}
        onClose={() => setConfirmUninstallOpen(false)}
      />

      {/* ── 内置交互式演练沙箱 (Live Playground Modal) ────────────────── */}
      {showPlayground && (
        <div className="extension-modal-backdrop" onClick={() => setShowPlayground(false)}>
          <div
            className="extension-modal-container"
            role="dialog"
            aria-modal="true"
            aria-labelledby="extension-playground-modal-title"
            onClick={(e) => e.stopPropagation()}
          >
            <div className="extension-modal-header">
              <div className="extension-modal-title-box">
                <IconComponent size={20} className="text-primary" />
                <h3 id="extension-playground-modal-title">{plugin?.name || pluginId} - {t('extension.liveDemo')}</h3>
              </div>
              <button
                ref={modalCloseBtnRef}
                type="button"
                className="extension-modal-close"
                onClick={() => setShowPlayground(false)}
                aria-label={t('extension.closePlayground')}
              >
                <X size={18} />
              </button>
            </div>

            <div className="extension-modal-body">
              {pluginId === 'markdown_preview' && (
                <div className="playground-md-window">
                  <div className="playground-md-topbar">
                    <span className="playground-md-dot dot-red" />
                    <span className="playground-md-dot dot-yellow" />
                    <span className="playground-md-dot dot-green" />
                    <span className="playground-md-title">{t('extGuide.mdDemoTitle', 'README.md - Spacebar QuickLook Demo')}</span>
                    <button
                      type="button"
                      className="playground-copy-btn"
                      onClick={handleCopyDemoCode}
                    >
                      <Copy size={13} />
                      {demoCopied ? t('extGuide.copied', 'Copied!') : t('extGuide.copyCode', 'Copy Code')}
                    </button>
                  </div>
                  <div className="playground-md-content">
                    <h1>{t('extension.suiteTitle', 'Tools3000 Productivity Suite')}</h1>
                    <p>
                      {t('extGuide.mdDemoDesc', 'This is a live preview triggered by pressing Spacebar. It supports syntax highlighting & formulas:')}
                    </p>
                    <div className="playground-code-block">
                      <pre>
                        <code>{`// TypeScript code highlight
import { createEngine } from "@tools3000/core";

const engine = createEngine({
  renderSpeedMs: 0.2,
  enableKaTeX: true
});
console.log("${t('extension.mdReadyLog', 'Markdown preview engine is ready!')}");`}</code>
                      </pre>
                    </div>
                    <div className="playground-math-box">
                      <span><strong>{t('extension.latexFormula', 'LaTeX Formula:')}</strong></span>
                      <code>{'$$ E = mc^2 \\quad \\& \\quad f(x) = \\int_{-\\infty}^\\infty \\hat f(\\xi)\\,e^{2 \\pi i \\xi x}\\,d\\xi $$'}</code>
                    </div>
                    <div className="playground-tips-callout">
                      <HelpCircle size={15} />
                      <span>{t('extension.mdTriggerHint', 'Select any .md file in Explorer and press Space to trigger this!')}</span>
                    </div>
                  </div>
                </div>
              )}

              {pluginId === 'color_picker' && (
                <div className="playground-color-box">
                  <div className="playground-color-picker-header">
                    <h4>{t('extension.colorPickerTitle', 'Color Picker Simulation (Press Alt+C)')}</h4>
                  </div>
                  <div className="playground-color-preview-row">
                    <div className="playground-color-swatch" style={{ background: '#7c3aed' }} />
                    <div className="playground-color-vals">
                      <div className="playground-val-item">
                        <span>HEX:</span> <CodeBadge>#7C3AED</CodeBadge>
                      </div>
                      <div className="playground-val-item">
                        <span>RGB:</span> <CodeBadge>rgb(124, 58, 237)</CodeBadge>
                      </div>
                      <div className="playground-val-item">
                        <span>HSL:</span> <CodeBadge>hsl(262, 83%, 58%)</CodeBadge>
                      </div>
                    </div>
                  </div>
                  <Button
                    variant="secondary"
                    onClick={() => {
                      navigator.clipboard.writeText('#7C3AED');
                      toast.success(t('extension.copySuccess'));
                    }}
                  >
                    <Copy size={14} /> {t('extension.copyColor', 'Copy Color')}
                  </Button>
                </div>
              )}

              {pluginId === 'clipboard_manager' && (
                <div className="playground-clip-box">
                  <div className="playground-clip-header">
                    <h4>{t('extension.clipboardHistoryTitle', 'Clipboard History (Press Alt+V)')}</h4>
                  </div>
                  <div className="playground-clip-list">
                    <div className="playground-clip-item">
                      <span className="playground-clip-index">1</span>
                      <span className="playground-clip-text">https://github.com/yuan278501381/Tools3000</span>
                      <Badge text={t('extension.urlBadge', 'URL')} variant="primary" />
                    </div>
                    <div className="playground-clip-item">
                      <span className="playground-clip-index">2</span>
                      <span className="playground-clip-text">git clone https://github.com/yuan278501381/Tools3000.git</span>
                      <Badge text={t('extension.codeBadge', 'Code')} variant="muted" />
                    </div>
                    <div className="playground-clip-item">
                      <span className="playground-clip-index">3</span>
                      <span className="playground-clip-text">{t('extension.suiteDesc', 'Tools3000: Modern Windows Desktop Productivity Toolbox')}</span>
                      <Badge text={t('extension.textBadge', 'Text')} variant="success" />
                    </div>
                  </div>
                </div>
              )}

              {pluginId === 'ai_assistant' && (
                <div className="playground-ai-box">
                  <div className="playground-ai-header">
                    <Bot size={18} className="text-primary" />
                    <h4>{t('extension.aiAssistantTitle', 'AI Assistant (Press Alt+X)')}</h4>
                  </div>
                  <div className="playground-ai-chat">
                    <div className="playground-ai-msg user">
                      <span>{t('extension.aiPromptExample', 'Optimize this C++ memory trim logic')}</span>
                    </div>
                    <div className="playground-ai-msg bot">
                      <span>
                        {t('extGuide.aiResponse', 'Recommend calling WinUtils::trimWorkingSet() uniformly at lifecycle endpoints (cold paths) with lazy resource re-initialization.')}
                      </span>
                    </div>
                  </div>
                </div>
              )}
            </div>

            <div className="extension-modal-footer">
              <Button variant="primary" onClick={() => setShowPlayground(false)}>
                {t('extension.closePlayground')}
              </Button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};


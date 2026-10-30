import { useState, useCallback, useMemo, type FC } from 'react';
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
import { Card, Toggle, Button, Badge, CodeBadge } from '../components/UIKit';
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
  conditionZh?: string;
  conditionKey?: string;
  conditionEn: string;
  steps: {
    step: number;
    titleZh?: string;
    titleKey?: string;
    titleEn: string;
    descZh?: string;
    descKey?: string;
    descEn: string;
    kbd?: string[];
  }[];
  proTips: {
    titleZh?: string;
    titleKey?: string;
    titleEn: string;
    descZh?: string;
    descKey?: string;
    descEn: string;
  }[];
  supportedFormats?: string[];
  supportedFormatsEn?: string[];
  supportedFormatKeys?: string[];
}

const EXTENSION_GUIDES: Record<string, ExtensionGuideInfo> = {
  markdown_preview: {
    triggerKey: 'Space',
    triggerKeyArray: ['Space'],
    conditionZh: '在资源管理器、桌面或文件对话框中单选任意 .md、代码或文本文件',
    conditionEn: 'Select any .md, code, or text file in File Explorer or Desktop',
    steps: [
      {
        step: 1,
        titleKey: 'extGuide.stepQuickLook1Title',
        titleEn: 'Select Target File',
        descZh: '在 Windows 文件资源管理器中鼠标单击选中任意 Markdown 或代码文件。',
        descEn: 'Click once to highlight any Markdown or source code file in Explorer.',
      },
      {
        step: 2,
        titleKey: 'extGuide.stepQuickLook2Title',
        titleEn: 'Press Spacebar',
        descZh: '轻按键盘空格键 Space，无需双击打开笨重编辑器。',
        descEn: 'Press Spacebar once without double clicking heavy editors.',
        kbd: ['Space'],
      },
      {
        step: 3,
        titleZh: '极速悬浮预览',
        titleEn: 'Instant QuickLook',
        descZh: '即刻唤起 Fluent 玻璃拟态预览浮层，支持代码高亮与 LaTeX 公式渲染。',
        descEn: 'Instant floating QuickLook preview with code highlighting & LaTeX.',
      },
    ],
    proTips: [
      {
        titleZh: '快速关闭',
        titleEn: 'Fast Close',
        descZh: '再次轻按 Space 或按 Esc 键即可毫秒级关闭预览浮层。',
        descEn: 'Press Space or Esc again to dismiss the preview instantly.',
      },
      {
        titleZh: '一键复制与打开',
        titleEn: 'Copy & Open in Editor',
        descZh: '预览窗右上角提供“一键复制代码”与“在默认编辑器中打开”快捷按钮。',
        descEn: 'Quickly copy rendered content or open in your default IDE from top bar.',
      },
      {
        titleZh: '数学公式与表格',
        titleEn: 'Math & Tables Support',
        descZh: '原生支持 KaTeX / MathJax 公式渲染 ($E=mc^2$) 及 GitHub 风格 Markdown 表格。',
        descEn: 'Full support for GitHub flavored markdown, KaTeX math and formatted tables.',
      },
    ],
    supportedFormats: ['.md', '.markdown', '.json', '.js', '.ts', '.py', '.cpp', '.rs', '.sql', '.txt'],
  },
  ai_assistant: {
    triggerKey: 'Alt + X',
    triggerKeyArray: ['Alt', 'X'],
    conditionZh: '在任意软件界面划选文字后按下快捷键，或随时全局唤出',
    conditionEn: 'Select text anywhere or press shortcut to trigger globally',
    steps: [
      {
        step: 1,
        titleZh: '划选文字或直接呼出',
        titleEn: 'Select Text or Global Call',
        descZh: '选中屏幕上任意需要翻译、重构或润色的文字段落。',
        descEn: 'Highlight any text to translate, rewrite, or ask questions.',
      },
      {
        step: 2,
        titleZh: '按下触发快捷键',
        titleEn: 'Press Activation Hotkey',
        descZh: '按下 Alt + X 快捷键，随光标位置即刻弹出 AI 交互悬浮窗。',
        descEn: 'Press Alt + X to pop up the intelligent AI dialogue box.',
        kbd: ['Alt', 'X'],
      },
      {
        step: 3,
        titleZh: '获得流式解答',
        titleEn: 'Get Instant Answers',
        descZh: '支持一键中英互译、代码重构、要点提炼或自由提问。',
        descEn: 'Enjoy stream generation, translation, code refactor and smart answers.',
      },
    ],
    proTips: [
      {
        titleZh: '上下文智能感知',
        titleEn: 'Context Awareness',
        descZh: '若选中文本为代码，AI 将自动识别编程语言并提供优化建议。',
        descEn: 'Automatically detects code snippets and provides tailored reviews.',
      },
      {
        titleZh: '快捷模板切换',
        titleEn: 'Prompt Templates',
        descZh: '按 Tab 键可在“翻译”、“润色”、“解释”、“总结”等常用 Prompt 间快速切换。',
        descEn: 'Press Tab to cycle between preset prompt templates quickly.',
      },
    ],
    supportedFormats: ['纯文本', '代码段落', '文章摘要', '多语言互译'],
    supportedFormatsEn: ['Plain Text', 'Code Snippets', 'Article Summary', 'Multi-language Translation'],
  },
  color_picker: {
    triggerKey: 'Alt + C',
    triggerKeyArray: ['Alt', 'C'],
    conditionZh: '屏幕任意位置像素级取色（支持多显示器与不同 DPI）',
    conditionEn: 'Pixel precision color picking anywhere on screen (Multi-DPI ready)',
    steps: [
      {
        step: 1,
        titleZh: '唤起微距放大镜',
        titleEn: 'Activate Magnifier',
        descZh: '按下 Alt + C 快捷键，鼠标光标即刻变为 8x 像素级微距十字放大镜。',
        descEn: 'Press Alt + C to turn cursor into an 8x pixel magnifier.',
        kbd: ['Alt', 'C'],
      },
      {
        step: 2,
        titleZh: '精准锁定像素',
        titleEn: 'Lock Pixel',
        descZh: '移动鼠标对准目标颜色，亦可用键盘方向键 ↑↓←→ 进行 1 像素级微调。',
        descEn: 'Hover over target pixel; use arrow keys for 1-pixel precision nudging.',
      },
      {
        step: 3,
        titleZh: '单击自动复制',
        titleEn: 'Click to Copy',
        descZh: '鼠标左键单击即刻复制 HEX 色值，并同步更新到历史调色板中。',
        descEn: 'Click left button to copy HEX code and record to your color palette.',
      },
    ],
    proTips: [
      {
        titleZh: '切换色彩格式',
        titleEn: 'Color Spaces',
        descZh: '取色过程中轻按 Space 键可循环切换 HEX、RGB、HSL、CMYK 等输出格式。',
        descEn: 'Press Space to toggle between HEX, RGB, HSL and CMYK output formats.',
      },
      {
        titleZh: '滚轮缩放倍率',
        titleEn: 'Zoom Factor',
        descZh: '滚动鼠标滚轮可在 2x ~ 16x 放大倍率间自由平滑缩放。',
        descEn: 'Scroll mouse wheel to smoothly adjust magnification between 2x and 16x.',
      },
    ],
    supportedFormats: ['HEX (#FFFFFF)', 'RGB (rgb(255,255,255))', 'HSL', 'CMYK'],
  },
  clipboard_manager: {
    triggerKey: 'Alt + V',
    triggerKeyArray: ['Alt', 'V'],
    conditionZh: '在任何文本输入区域或编辑窗口快速唤出历史剪贴板',
    conditionEn: 'Call clipboard history panel in any text input field or active window',
    steps: [
      {
        step: 1,
        titleZh: '常规复制内容',
        titleEn: 'Copy As Usual',
        descZh: '使用 Ctrl + C 正常复制文字、代码、链接或截图图片。',
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
  const { t, i18n } = useTranslation();
  const [enabledOverrides, setEnabledOverrides] = useState<Record<string, boolean>>({});
  const enabled = enabledOverrides[pluginId] ?? plugin?.enabled ?? true;
  const [saving, setSaving] = useState(false);
  const [uninstalling, setUninstalling] = useState(false);
  const [showPlayground, setShowPlayground] = useState(false);
  const [demoCopied, setDemoCopied] = useState(false);

  const isZh = i18n.language.startsWith('zh');

  const resolveText = useCallback((key?: string, zh?: string, en?: string) => {
    const fallback = isZh ? (zh || en || '') : (en || zh || '');
    if (key) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return t(key as any, { defaultValue: fallback });
    }
    return fallback;
  }, [isZh, t]);

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
    const list = isZh ? guide.supportedFormats : (guide.supportedFormatsEn || guide.supportedFormats);
    return list && list.length > 0 ? list : ['.*'];
  }, [guide, isZh, t]);

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

  const handleUninstall = useCallback(async () => {
    if (!window.confirm(t('extension.uninstallConfirm'))) return;
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
    void navigator.clipboard?.writeText('```typescript\nimport { tools3000 } from "@tools3000/sdk";\n\n// 极速唤起 Markdown 渲染引擎\nconst preview = tools3000.preview("README.md");\n```')?.catch(() => {});
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
          <span>{resolveText(guide.conditionKey, guide.conditionZh, guide.conditionEn)}</span>
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
                <h4 className="extension-step-title">{resolveText(item.titleKey, item.titleZh, item.titleEn)}</h4>
              </div>
              <p className="extension-step-desc">{resolveText(item.descKey, item.descZh, item.descEn)}</p>
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
                  <strong className="extension-tip-title">{resolveText(tip.titleKey, tip.titleZh, tip.titleEn)}:</strong>{' '}
                  <span className="extension-tip-desc">{resolveText(tip.descKey, tip.descZh, tip.descEn)}</span>
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

      {/* ── 模块基础配置 ───────────────────────────────────────── */}
      <Card title={t('extension.settings')} className="extension-settings-card">
        <div className="extension-setting-row">
          <div>
            <div className="extension-setting-label">{t('extension.autoStart')}</div>
            <div className="extension-setting-desc">
              {t('extGuide.residentDesc', 'Auto launch and keep memory footprint < 1 MB')}
            </div>
          </div>
          <Toggle id="ext-autostart-toggle" checked={enabled} onChange={handleToggle} disabled={saving} />
        </div>

        <div className="extension-setting-row">
          <div>
            <div className="extension-setting-label">{t('extension.customShortcut')}</div>
            <div className="extension-setting-desc">{t('extension.customShortcutDesc')}</div>
          </div>
          <div className="extension-shortcut-badge">
            <kbd>{guide.triggerKey}</kbd>
          </div>
        </div>

        <div className="extension-setting-footer">
          <Button
            variant="danger"
            onClick={handleUninstall}
            disabled={uninstalling}
          >
            <span style={{ display: 'inline-flex', alignItems: 'center', gap: '6px' }}>
              <Trash2 size={16} />
              {t('extension.uninstall')}
            </span>
          </Button>
        </div>
      </Card>

      {/* ── 内置交互式演练沙箱 (Live Playground Modal) ────────────────── */}
      {showPlayground && (
        <div className="extension-modal-backdrop" onClick={() => setShowPlayground(false)}>
          <div className="extension-modal-container" onClick={(e) => e.stopPropagation()}>
            <div className="extension-modal-header">
              <div className="extension-modal-title-box">
                <IconComponent size={20} className="text-primary" />
                <h3>{plugin?.name || pluginId} - {t('extension.liveDemo')}</h3>
              </div>
              <button
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
                        {isZh
                          ? t('extGuide.aiResponse', 'Recommend calling WinUtils::trimWorkingSet() uniformly at lifecycle endpoints (cold paths) with lazy resource re-initialization.')
                          : 'Recommended to trigger WinUtils::trimWorkingSet() strictly on lifecycle cold paths.'}
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


/* ─────────────────────────────────────────────────────────────────────────────
 * OnboardingModal — 首次使用引导弹窗
 *
 * 4 步引导流程:
 *   1. 欢迎 — 功能亮点卡片
 *   2. 核心快捷键
 *   3. 手势入门
 *   4. 完成
 * ───────────────────────────────────────────────────────────────────────────── */

import { useEffect, useRef, useState, type FC } from 'react';
import { Button } from './UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import {
  Search, Camera, Mouse, Sparkles, FolderSymlink,
  Keyboard, Highlighter, ChevronRight, ChevronLeft, Check, Zap,
  Cast, RotateCcw, Languages, ClipboardList, Video, Image as ImageIcon,
  Pin, SlidersHorizontal, Layers, TextCursorInput, Cloud, LayoutGrid,
} from 'lucide-react';
import './OnboardingModal.css';

interface Props {
  onComplete: () => void;
}

const TOTAL_STEPS = 4;

interface OnboardingHotkeyMap {
  search: string;
  clipboard: string;
  capture: string;
  pin: string;
  record: string;
  ocr: string;
  translate: string;
  dialogJump: string;
  remoteFlush: string;
}

const DEFAULT_ONBOARDING_HOTKEYS: OnboardingHotkeyMap = {
  search: 'Alt + Space',
  clipboard: 'Alt + V',
  capture: 'F1',
  pin: 'F3',
  record: 'Ctrl + Shift + R',
  ocr: 'Ctrl + Shift + O',
  translate: 'Alt + X',
  dialogJump: 'Ctrl + G',
  remoteFlush: 'Ctrl + Alt + Backspace',
};

const formatDisplayHotkey = (raw: string | undefined, fallback: string): string => {
  if (typeof raw !== 'string') return fallback;
  const trimmed = raw.trim();
  if (!trimmed) return fallback;
  return trimmed
    .split('+')
    .map((part) => part.trim())
    .filter(Boolean)
    .join(' + ');
};

export const OnboardingModal: FC<Props> = ({ onComplete }) => {
  const { t } = useTranslation();
  const [step, setStep] = useState(0);
  const [showcaseTab, setShowcaseTab] = useState<'spotlight' | 'remote'>('spotlight');
  const [liveHotkeys, setLiveHotkeys] = useState<OnboardingHotkeyMap>(DEFAULT_ONBOARDING_HOTKEYS);
  const cardRef = useRef<HTMLDivElement>(null);
  const titleRefs = useRef<Array<HTMLHeadingElement | null>>([]);

  useEffect(() => {
    let cancelled = false;
    void Promise.allSettled([
      bridgeRequest<Array<{ name?: string; shortcut?: string }>>('hotkey.getAll', undefined, { silent: true }),
      bridgeRequest<{ emergencyShortcut?: string }>('remote.getSettings', undefined, { silent: true }),
    ]).then(([hkResult, remoteResult]) => {
      if (cancelled) return;
      const entries = hkResult.status === 'fulfilled' && Array.isArray(hkResult.value)
        ? hkResult.value
        : [];
      const findShortcut = (...names: string[]) => {
        const lowerNames = names.map((n) => n.toLowerCase());
        const hit = entries.find((item) => item?.name && lowerNames.includes(item.name.toLowerCase()));
        return hit?.shortcut;
      };
      const remoteCfg = remoteResult.status === 'fulfilled' && remoteResult.value && typeof remoteResult.value === 'object'
        ? remoteResult.value
        : null;
      const flushRaw = remoteCfg?.emergencyShortcut
        || findShortcut('Remote Emergency Flush', 'Emergency Flush', 'Remote Flush');

      setLiveHotkeys((prev) => ({
        search: formatDisplayHotkey(findShortcut('Toggle Search', 'Search'), prev.search),
        clipboard: formatDisplayHotkey(findShortcut('Toggle Clipboard', 'Clipboard', 'Clipboard History'), prev.clipboard),
        capture: formatDisplayHotkey(findShortcut('Screenshot', 'Capture'), prev.capture),
        pin: formatDisplayHotkey(findShortcut('Pin Paste', 'Paste as Pin', 'Pin'), prev.pin),
        record: formatDisplayHotkey(findShortcut('Record', 'Screen Record'), prev.record),
        ocr: formatDisplayHotkey(findShortcut('OCR', 'Text OCR'), prev.ocr),
        translate: formatDisplayHotkey(findShortcut('Translate Selection', 'Quick Translate'), prev.translate),
        dialogJump: formatDisplayHotkey(findShortcut('Dialog Quick Jump'), prev.dialogJump),
        remoteFlush: formatDisplayHotkey(flushRaw, prev.remoteFlush),
      }));
    });
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    titleRefs.current[step]?.focus();
  }, [step]);

  const onCompleteRef = useRef(onComplete);
  useEffect(() => {
    onCompleteRef.current = onComplete;
  }, [onComplete]);

  useEffect(() => {
    const previousFocus = document.activeElement instanceof HTMLElement
      ? document.activeElement : null;
    const initialFocusFrame = requestAnimationFrame(() => titleRefs.current[0]?.focus());
    const focusableSelector = 'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.isComposing || event.keyCode === 229 || event.key === 'Process') return;
      if (event.key === 'Escape') {
        event.preventDefault();
        onCompleteRef.current();
      } else if (event.key === 'ArrowRight') {
        const isTab = document.activeElement?.getAttribute('role') === 'tab';
        if (isTab) {
          event.preventDefault();
          setShowcaseTab((current) => {
            const nextTab = current === 'spotlight' ? 'remote' : 'spotlight';
            const targetId = nextTab === 'spotlight' ? 'onboarding-tab-spotlight' : 'onboarding-tab-remote';
            document.getElementById(targetId)?.focus();
            return nextTab;
          });
          return;
        }
        event.preventDefault();
        setStep((current) => Math.min(TOTAL_STEPS - 1, current + 1));
      } else if (event.key === 'ArrowLeft') {
        const isTab = document.activeElement?.getAttribute('role') === 'tab';
        if (isTab) {
          event.preventDefault();
          setShowcaseTab((current) => {
            const nextTab = current === 'spotlight' ? 'remote' : 'spotlight';
            const targetId = nextTab === 'spotlight' ? 'onboarding-tab-spotlight' : 'onboarding-tab-remote';
            document.getElementById(targetId)?.focus();
            return nextTab;
          });
          return;
        }
        event.preventDefault();
        setStep((current) => Math.max(0, current - 1));
      } else if (event.key === 'Tab' && cardRef.current) {
        const focusable = Array.from(
          cardRef.current.querySelectorAll<HTMLElement>(focusableSelector),
        ).filter((el) => !el.closest('[aria-hidden="true"]'));
        if (focusable.length === 0) return;
        const first = focusable[0];
        const last = focusable[focusable.length - 1];
        if (event.shiftKey && document.activeElement === first) {
          event.preventDefault();
          last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault();
          first.focus();
        }
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => {
      cancelAnimationFrame(initialFocusFrame);
      window.removeEventListener('keydown', onKeyDown);
      previousFocus?.focus();
    };
  }, []);

  const next = () => {
    if (step < TOTAL_STEPS - 1) setStep(step + 1);
    else onComplete();
  };

  const prev = () => {
    if (step > 0) setStep(step - 1);
  };

  const getStepClass = (idx: number) => {
    if (idx === step) return 'onboarding__step onboarding__step--active';
    if (idx < step) return 'onboarding__step onboarding__step--prev';
    return 'onboarding__step onboarding__step--next';
  };

  return (
    <div
      className="onboarding"
      onClick={(e) => {
        if (e.target === e.currentTarget) {
          onComplete();
        }
      }}
      onWheel={(e) => {
        e.stopPropagation();
      }}
    >
      <div
        ref={cardRef}
        className="onboarding__card"
        role="dialog"
        aria-modal="true"
        aria-labelledby={`onboarding-step-${step}-title`}
        onClick={(e) => e.stopPropagation()}
        onWheel={(e) => e.stopPropagation()}
      >
        <div className="onboarding__glow" aria-hidden="true" />
        <button
          className="onboarding__skip"
          onClick={onComplete}
          aria-label={t('onboarding.skip')}
          title={t('onboarding.skip')}
        >
          {t('onboarding.skip')}
        </button>

        <div className="onboarding__body">
          {/* ── Step 0: 欢迎 (2x4 共 8 张旗舰能力卡，全景覆盖 15 大核心模块) ── */}
          <div className={getStepClass(0)} aria-hidden={step !== 0} inert={step !== 0}>
            <h2 id="onboarding-step-0-title" ref={(node) => { titleRefs.current[0] = node; }} tabIndex={-1} className="onboarding__title">{t('onboarding.welcomeTitle')}</h2>
            <p className="onboarding__subtitle">{t('onboarding.welcomeSubtitle')}</p>
            <div className="onboarding__features">
              <div className="onboarding__feature-card">
                <div className="onboarding__feature-icon" aria-hidden="true">
                  <Search size={19} />
                </div>
                <span className="onboarding__feature-name">{t('onboarding.featureSearch')}</span>
                <span className="onboarding__feature-desc">{t('onboarding.featureSearchDesc')}</span>
              </div>
              <div className="onboarding__feature-card">
                <div className="onboarding__feature-icon" aria-hidden="true">
                  <ClipboardList size={19} />
                </div>
                <span className="onboarding__feature-name">{t('onboarding.featureClipboard' as never, 'Persistent Clipboard')}</span>
                <span className="onboarding__feature-desc">{t('onboarding.featureClipboardDesc' as never, 'Alt+V history surviving reboots, merge paste, groups & drag-out')}</span>
              </div>
              <div className="onboarding__feature-card">
                <div className="onboarding__feature-icon" aria-hidden="true">
                  <Camera size={19} />
                </div>
                <span className="onboarding__feature-name">{t('onboarding.featureCapture')}</span>
                <span className="onboarding__feature-desc">{t('onboarding.featureCaptureDesc')}</span>
              </div>
              <div className="onboarding__feature-card">
                <div className="onboarding__feature-icon" aria-hidden="true">
                  <FolderSymlink size={19} />
                </div>
                <span className="onboarding__feature-name">{t('onboarding.featureDialogEnhancer')}</span>
                <span className="onboarding__feature-desc">{t('onboarding.featureDialogEnhancerDesc')}</span>
              </div>
              <div className="onboarding__feature-card">
                <div className="onboarding__feature-icon" aria-hidden="true">
                  <Video size={19} />
                </div>
                <span className="onboarding__feature-name">{t('onboarding.featureStudio' as never, 'Auto-Zoom Recording Studio')}</span>
                <span className="onboarding__feature-desc">{t('onboarding.featureStudioDesc' as never, 'Waveform editing, click auto-zoom & 1-click GIF/MP4 export')}</span>
              </div>
              <div className="onboarding__feature-card">
                <div className="onboarding__feature-icon" aria-hidden="true">
                  <Mouse size={19} />
                </div>
                <span className="onboarding__feature-name">{t('onboarding.featureGesture')}</span>
                <span className="onboarding__feature-desc">{t('onboarding.featureGestureDesc')}</span>
              </div>
              <div className="onboarding__feature-card">
                <div className="onboarding__feature-icon" aria-hidden="true">
                  <LayoutGrid size={19} />
                </div>
                <span className="onboarding__feature-name">{t('onboarding.featureWindowManager' as never, 'Window Manager Suite')}</span>
                <span className="onboarding__feature-desc">{t('onboarding.featureWindowManagerDesc' as never, 'SnapZones tiling, workspaces capture & Fluent Alt+Tab switcher (opt-in)')}</span>
              </div>
              <div className="onboarding__feature-card">
                <div className="onboarding__feature-icon" aria-hidden="true">
                  <Cast size={19} />
                </div>
                <span className="onboarding__feature-name">{t('onboarding.featurePresentRemote' as never, 'Spotlight & Remote Guard')}</span>
                <span className="onboarding__feature-desc">{t('onboarding.featurePresentRemoteDesc' as never, 'Keycast overlay, spotlight highlight & RDP/RustDesk key flush')}</span>
              </div>
            </div>
          </div>

          {/* ── Step 1: 核心快捷键矩阵与融合搜索指令速查 ───────────────── */}
          <div className={getStepClass(1)} aria-hidden={step !== 1} inert={step !== 1}>
            <h2 id="onboarding-step-1-title" ref={(node) => { titleRefs.current[1] = node; }} tabIndex={-1} className="onboarding__title">{t('onboarding.shortcutsTitle')}</h2>
            <p className="onboarding__subtitle">{t('onboarding.shortcutsSubtitle')}</p>
            <div className="onboarding__shortcuts">
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <Search size={14} style={{ color: 'var(--primary)', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutSearch')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{liveHotkeys.search}</kbd>
              </div>
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <ClipboardList size={14} style={{ color: '#0ea5e9', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutClipboard' as never, 'Clipboard History')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{liveHotkeys.clipboard}</kbd>
              </div>
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <Camera size={14} style={{ color: 'var(--success)', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutCapture')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{liveHotkeys.capture}</kbd>
              </div>
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <Pin size={14} style={{ color: '#f97316', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutPin' as never, 'Pin from Clipboard')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{liveHotkeys.pin}</kbd>
              </div>
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <Zap size={14} style={{ color: '#fbbf24', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutRecord')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{liveHotkeys.record}</kbd>
              </div>
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <Sparkles size={14} style={{ color: '#ec4899', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutOcr')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{liveHotkeys.ocr}</kbd>
              </div>
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <Languages size={14} style={{ color: '#14b8a6', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutTranslate' as never, 'Quick Selection Translate')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{liveHotkeys.translate}</kbd>
              </div>
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <FolderSymlink size={14} style={{ color: '#f59e0b', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutDialogJump' as never, 'Dialog Folder Quick Jump')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{liveHotkeys.dialogJump}</kbd>
              </div>
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <Highlighter size={14} style={{ color: '#38bdf8', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutSpotlight')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{t('spotlight.doubleCtrl')}</kbd>
              </div>
              <div className="onboarding__shortcut-row">
                <span className="onboarding__shortcut-name">
                  <Cast size={14} style={{ color: '#818cf8', marginRight: 6 }} aria-hidden="true" />
                  {t('onboarding.shortcutRemoteFlush')}
                </span>
                <kbd className="onboarding__shortcut-kbd">{liveHotkeys.remoteFlush}</kbd>
              </div>
            </div>
            <div className="onboarding__search-tips">
              <span className="onboarding__search-tips-label">
                <Search size={13} aria-hidden="true" />
                <span>{t('onboarding.searchSyntaxLabel' as never, 'Omni Search Prefixes')}</span>
              </span>
              <div className="onboarding__search-tips-chips">
                <kbd className="onboarding__syntax-chip">app:</kbd>
                <kbd className="onboarding__syntax-chip">bm:</kbd>
                <kbd className="onboarding__syntax-chip">cmd:</kbd>
                <kbd className="onboarding__syntax-chip">content:</kbd>
                <span className="onboarding__syntax-sep" aria-hidden="true">+</span>
                <kbd className="onboarding__syntax-chip onboarding__syntax-chip--accent">Space</kbd>
                <span className="onboarding__syntax-note">
                  {t('onboarding.searchSyntaxQuickLook' as never, 'QuickLook Preview')}
                </span>
              </div>
            </div>
          </div>

          {/* ── Step 2: 手势、边缘滚轮与演示/远程双模式展厅 ─────────────── */}
          <div className={getStepClass(2)} aria-hidden={step !== 2} inert={step !== 2}>
            <h2 id="onboarding-step-2-title" ref={(node) => { titleRefs.current[2] = node; }} tabIndex={-1} className="onboarding__title">{t('onboarding.gestureTitle')}</h2>
            <p className="onboarding__subtitle">{t('onboarding.gestureSubtitle')}</p>
            <div className="onboarding__showcase-grid">
              {/* 左侧：鼠标常用手势 + 屏幕边缘滚轮 */}
              <div className="onboarding__showcase-col">
                <div className="onboarding__showcase-col-header">
                  <Mouse size={16} aria-hidden="true" />
                  <span>{t('onboarding.featureGesture')}</span>
                </div>
                <div className="onboarding__gestures">
                  <div className="onboarding__gesture-card">
                    <span className="onboarding__gesture-arrow">↓</span>
                    <span className="onboarding__gesture-label">{t('onboarding.gestureDown')}</span>
                  </div>
                  <div className="onboarding__gesture-card">
                    <span className="onboarding__gesture-arrow">↑</span>
                    <span className="onboarding__gesture-label">{t('onboarding.gestureUp')}</span>
                  </div>
                  <div className="onboarding__gesture-card">
                    <span className="onboarding__gesture-arrow">←</span>
                    <span className="onboarding__gesture-label">{t('onboarding.gestureLeft')}</span>
                  </div>
                  <div className="onboarding__gesture-card">
                    <span className="onboarding__gesture-arrow">→</span>
                    <span className="onboarding__gesture-label">{t('onboarding.gestureRight')}</span>
                  </div>
                  <div className="onboarding__gesture-card">
                    <span className="onboarding__gesture-arrow">↓ →</span>
                    <span className="onboarding__gesture-label">{t('onboarding.gestureCloseTab')}</span>
                  </div>
                  <div className="onboarding__gesture-card">
                    <span className="onboarding__gesture-arrow">→ ↓</span>
                    <span className="onboarding__gesture-label">{t('onboarding.gestureReopenTab')}</span>
                  </div>
                </div>
                <div className="onboarding__edge-wheel-tip">
                  <SlidersHorizontal size={14} className="onboarding__edge-wheel-icon" aria-hidden="true" />
                  <span>
                    {t('onboarding.edgeWheelTip' as never, 'Screen Left/Right Edge Scroll: adjust volume or external monitor hardware brightness directly')}
                  </span>
                </div>
              </div>

              {/* 右侧：屏幕演示与远程协助双模式展厅 */}
              <div className="onboarding__showcase-col">
                <div className="onboarding__showcase-col-header">
                  <div className="onboarding__showcase-tabs" role="tablist" aria-label={t('onboarding.gestureTitle')}>
                    <button
                      type="button"
                      role="tab"
                      id="onboarding-tab-spotlight"
                      aria-controls="onboarding-panel-spotlight"
                      aria-selected={showcaseTab === 'spotlight'}
                      tabIndex={showcaseTab === 'spotlight' ? 0 : -1}
                      className={`onboarding__showcase-tab ${showcaseTab === 'spotlight' ? 'onboarding__showcase-tab--active' : ''}`}
                      onClick={() => setShowcaseTab('spotlight')}
                    >
                      <Highlighter size={14} aria-hidden="true" />
                      <span>{t('onboarding.tabSpotlight')}</span>
                    </button>
                    <button
                      type="button"
                      role="tab"
                      id="onboarding-tab-remote"
                      aria-controls="onboarding-panel-remote"
                      aria-selected={showcaseTab === 'remote'}
                      tabIndex={showcaseTab === 'remote' ? 0 : -1}
                      className={`onboarding__showcase-tab ${showcaseTab === 'remote' ? 'onboarding__showcase-tab--active' : ''}`}
                      onClick={() => setShowcaseTab('remote')}
                    >
                      <Cast size={14} aria-hidden="true" />
                      <span>{t('onboarding.tabRemote')}</span>
                    </button>
                  </div>
                </div>
                {showcaseTab === 'spotlight' ? (
                  <div
                    id="onboarding-panel-spotlight"
                    role="tabpanel"
                    aria-labelledby="onboarding-tab-spotlight"
                    className="onboarding__spotlight-list"
                  >
                    <div className="onboarding__spotlight-item">
                      <div className="onboarding__spotlight-icon-wrap" style={{ background: 'rgba(56, 189, 248, 0.15)', color: '#38bdf8' }}>
                        <Zap size={14} aria-hidden="true" />
                      </div>
                      <span>{t('onboarding.spotlightItem1')}</span>
                    </div>
                    <div className="onboarding__spotlight-item">
                      <div className="onboarding__spotlight-icon-wrap" style={{ background: 'rgba(239, 68, 68, 0.15)', color: '#ef4444' }}>
                        <Highlighter size={14} aria-hidden="true" />
                      </div>
                      <span>{t('onboarding.spotlightItem2')}</span>
                    </div>
                    <div className="onboarding__spotlight-item">
                      <div className="onboarding__spotlight-icon-wrap" style={{ background: 'rgba(52, 211, 153, 0.15)', color: '#34d399' }}>
                        <Sparkles size={14} aria-hidden="true" />
                      </div>
                      <span>{t('onboarding.spotlightItem3')}</span>
                    </div>
                    <div className="onboarding__spotlight-item">
                      <div className="onboarding__spotlight-icon-wrap" style={{ background: 'rgba(167, 139, 250, 0.15)', color: '#a78bfa' }}>
                        <Keyboard size={14} aria-hidden="true" />
                      </div>
                      <span>{t('onboarding.spotlightItem4')}</span>
                    </div>
                  </div>
                ) : (
                  <div
                    id="onboarding-panel-remote"
                    role="tabpanel"
                    aria-labelledby="onboarding-tab-remote"
                    className="onboarding__spotlight-list"
                  >
                    <div className="onboarding__spotlight-item">
                      <div className="onboarding__spotlight-icon-wrap" style={{ background: 'rgba(99, 102, 241, 0.15)', color: '#818cf8' }}>
                        <Zap size={14} aria-hidden="true" />
                      </div>
                      <span>{t('onboarding.remoteItem1')}</span>
                    </div>
                    <div className="onboarding__spotlight-item">
                      <div className="onboarding__spotlight-icon-wrap" style={{ background: 'rgba(239, 68, 68, 0.15)', color: '#ef4444' }}>
                        <RotateCcw size={14} aria-hidden="true" />
                      </div>
                      <span>{t('onboarding.remoteItem2')}</span>
                    </div>
                    <div className="onboarding__spotlight-item">
                      <div className="onboarding__spotlight-icon-wrap" style={{ background: 'rgba(52, 211, 153, 0.15)', color: '#34d399' }}>
                        <Languages size={14} aria-hidden="true" />
                      </div>
                      <span>{t('onboarding.remoteItem3')}</span>
                    </div>
                    <div className="onboarding__spotlight-item">
                      <div className="onboarding__spotlight-icon-wrap" style={{ background: 'rgba(251, 191, 36, 0.15)', color: '#fbbf24' }}>
                        <Cast size={14} aria-hidden="true" />
                      </div>
                      <span>{t('onboarding.remoteItem4')}</span>
                    </div>
                  </div>
                )}
              </div>
            </div>
          </div>

          {/* ── Step 3: 完成 + 开箱实用小贴士 ─────────────────────────── */}
          <div className={getStepClass(3)} aria-hidden={step !== 3} inert={step !== 3}>
            <div className="onboarding__complete-icon" aria-hidden="true">
              <Sparkles size={34} />
            </div>
            <h2 id="onboarding-step-3-title" ref={(node) => { titleRefs.current[3] = node; }} tabIndex={-1} className="onboarding__title">{t('onboarding.completeTitle')}</h2>
            <p className="onboarding__subtitle">{t('onboarding.completeSubtitle')}</p>
            <div className="onboarding__complete-tips">
              <div className="onboarding__complete-tip">
                <ImageIcon size={15} className="onboarding__complete-tip-icon" aria-hidden="true" />
                <span>{t('onboarding.completeTipPicasa' as never, 'Set as your default photo viewer in 1 click inside "Picasa Photo Viewer"')}</span>
              </div>
              <div className="onboarding__complete-tip">
                <TextCursorInput size={15} className="onboarding__complete-tip-icon" aria-hidden="true" />
                <span>{t('onboarding.completeTipTextExpander' as never, 'Try typing ;date or ;now anywhere with built-in "Text Expander" (customize or toggle off anytime)')}</span>
              </div>
              <div className="onboarding__complete-tip">
                <Cloud size={15} className="onboarding__complete-tip-icon" aria-hidden="true" />
                <span>{t('onboarding.completeTipCloudSync' as never, 'Enable E2E-encrypted cloud backup (OneDrive / WebDAV / S3) in General Settings to sync configs & shortcuts across PCs')}</span>
              </div>
              <div className="onboarding__complete-tip">
                <Layers size={15} className="onboarding__complete-tip-icon" aria-hidden="true" />
                <span>{t('onboarding.completeTipModules' as never, 'Every module can be toggled independently anytime in "Modules" or the system tray')}</span>
              </div>
            </div>
          </div>
        </div>

        {/* ── 底部导航 ─────────────────────────────────────────── */}
        <div className="onboarding__footer">
          <div className="onboarding__dots">
            {Array.from({ length: TOTAL_STEPS }).map((_, i) => (
              <button
                key={i}
                className={`onboarding__dot ${i === step ? 'onboarding__dot--active' : ''}`}
                onClick={() => setStep(i)}
                aria-label={t('onboarding.stepLabel', { current: i + 1, total: TOTAL_STEPS })}
                aria-current={i === step ? 'step' : undefined}
              />
            ))}
          </div>
          <div className="onboarding__actions">
            {step > 0 && step < TOTAL_STEPS - 1 && (
              <Button variant="ghost" size="sm" onClick={prev}>
                <ChevronLeft size={16} />
                {t('onboarding.prev')}
              </Button>
            )}
            {step < TOTAL_STEPS - 1 ? (
              <Button variant="primary" size="sm" onClick={next}>
                {t('onboarding.next')}
                <ChevronRight size={16} />
              </Button>
            ) : (
              <Button variant="primary" onClick={onComplete}>
                <Check size={16} />
                {t('onboarding.startButton')}
              </Button>
            )}
          </div>
        </div>
      </div>
    </div>
  );
};

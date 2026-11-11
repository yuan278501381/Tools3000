/* ─────────────────────────────────────────────────────────────────────────────
 * TitleBar.tsx — 一体化沉浸式无缝标题栏组件 (Unified Frameless TitleBar)
 *
 * 世界级桌面端窗口交互标准:
 * 1. 标题栏双击极速切换最大化 / 向下还原 (双击防模态吞没保护)
 * 2. 标题栏按住平滑拖拽移动与 Windows Aero Snap 贴靠支持
 * 3. 标题栏右键呼出 Windows 原生系统窗口菜单 (System Menu)
 * 4. 全链路监听 Win32 WM_SIZE 最大化状态事件，保障 Win+Up/Down 与分屏双向联动
 * ───────────────────────────────────────────────────────────────────────────── */

import { type FC, type MouseEvent, useEffect, useState, useCallback, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { Minus, Square, Copy, X, Globe, Sun, Moon, Monitor, Check, ChevronDown, Palette } from 'lucide-react';
import { LogoGlyph } from './Tools3000Bolt';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import './TitleBar.css';

const ACCENT_PRESETS = [
  { id: 'blue',   labelKey: 'general.accentBlue',   color: '#3b82f6' },
  { id: 'cyan',   labelKey: 'general.accentCyan',   color: '#06b6d4' },
  { id: 'amber',  labelKey: 'general.accentAmber',  color: '#f59e0b' },
  { id: 'mint',   labelKey: 'general.accentMint',   color: '#10b981' },
  { id: 'coral',  labelKey: 'general.accentCoral',  color: '#f43f5e' },
  { id: 'violet', labelKey: 'general.accentViolet', color: '#8b5cf6' },
] as const;

export interface TitleBarProps {
  isElevated?: boolean;
  themePreference?: 'system' | 'dark' | 'light';
  onSelectThemePreference?: (p: 'system' | 'dark' | 'light') => void;
  accent?: string;
  onSelectAccent?: (accent: string) => void;
}

export const TitleBar: FC<TitleBarProps> = ({
  isElevated = false,
  themePreference = 'system',
  onSelectThemePreference,
  accent = 'blue',
  onSelectAccent,
}) => {
  const { t, i18n } = useTranslation();
  const [isMaximized, setIsMaximized] = useState(false);
  const [langMenuOpen, setLangMenuOpen] = useState(false);
  const [themeMenuOpen, setThemeMenuOpen] = useState(false);
  const [isAutoLanguage, setIsAutoLanguage] = useState<boolean>(() => {
    try {
      const stored = localStorage.getItem('tools3000:language');
      return !stored || stored === 'auto';
    } catch {
      return false;
    }
  });

  const langRef = useRef<HTMLDivElement>(null);
  const themeRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const syncLangPreference = (rawPref?: string | null) => {
      const pref = rawPref ?? (() => {
        try {
          return localStorage.getItem('tools3000:language');
        } catch {
          return null;
        }
      })();
      setIsAutoLanguage(!pref || pref === 'auto');
    };
    const onLangChangedEvent = (e: Event) => {
      const detail = (e as CustomEvent<string>).detail;
      syncLangPreference(detail);
    };
    const onStorage = (e: StorageEvent) => {
      if (e.key === 'tools3000:language') {
        syncLangPreference(e.newValue);
      }
    };
    window.addEventListener('tools3000:language-changed', onLangChangedEvent);
    window.addEventListener('storage', onStorage);
    return () => {
      window.removeEventListener('tools3000:language-changed', onLangChangedEvent);
      window.removeEventListener('storage', onStorage);
    };
  }, []);

  // 初始化拉取当前窗口状态
  useEffect(() => {
    bridgeRequest<{ isMaximized: boolean }>('window.isMaximized')
      .then((res) => {
        if (res && typeof res.isMaximized === 'boolean') {
          setIsMaximized(res.isMaximized);
        }
      })
      .catch(() => {});
  }, []);

  // 监听 C++ 原生推送的最大化状态变化（覆盖 Win+方向键、Aero Snap 贴边、系统菜单等全部路径）
  useBridgeEvent('window:maximizedChanged', (data: unknown) => {
    if (data && typeof data === 'object' && 'isMaximized' in data) {
      setIsMaximized(Boolean((data as { isMaximized: boolean }).isMaximized));
    }
  });

  // 点击外部自动关闭下拉菜单
  useEffect(() => {
    if (!langMenuOpen && !themeMenuOpen) return;
    const handleClickOutside = (e: MouseEvent | globalThis.MouseEvent) => {
      const target = e.target as Node;
      if (langRef.current && !langRef.current.contains(target)) {
        setLangMenuOpen(false);
      }
      if (themeRef.current && !themeRef.current.contains(target)) {
        setThemeMenuOpen(false);
      }
    };
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        setLangMenuOpen(false);
        setThemeMenuOpen(false);
      }
    };
    document.addEventListener('pointerdown', handleClickOutside);
    document.addEventListener('keydown', handleKeyDown);
    return () => {
      document.removeEventListener('pointerdown', handleClickOutside);
      document.removeEventListener('keydown', handleKeyDown);
    };
  }, [langMenuOpen, themeMenuOpen]);

  const handleMinimize = useCallback(() => {
    bridgeRequest('window.minimize').catch(console.error);
  }, []);

  const handleToggleMaximize = useCallback(() => {
    bridgeRequest<{ isMaximized?: boolean }>('window.toggleMaximize')
      .then((res) => {
        if (res && typeof res.isMaximized === 'boolean') {
          setIsMaximized(res.isMaximized);
        } else {
          setIsMaximized((prev) => !prev);
        }
      })
      .catch(console.error);
  }, []);

  const handleClose = useCallback(() => {
    bridgeRequest('window.close').catch(console.error);
  }, []);

  // 切换语言
  const handleSelectLanguage = (langKey: 'auto' | 'zh-CN' | 'en-US') => {
    setLangMenuOpen(false);
    setIsAutoLanguage(langKey === 'auto');
    try {
      localStorage.setItem('tools3000:language', langKey);
    } catch {
      // 忽略本地存储异常
    }
    if (langKey === 'auto') {
      void i18n.changeLanguage(navigator.language.toLowerCase().startsWith('zh') ? 'zh' : 'en');
    } else {
      void i18n.changeLanguage(langKey.startsWith('zh') ? 'zh' : 'en');
    }
    bridgeRequest<{ success: boolean }>('general.updateSettings', { language: langKey }).catch(console.error);
    window.dispatchEvent(new CustomEvent('tools3000:language-changed', { detail: langKey }));
  };

  const lastToggleMaximizeTickRef = useRef(0);
  const triggerToggleMaximizeOnce = useCallback(() => {
    const now = Date.now();
    if (now - lastToggleMaximizeTickRef.current < 350) {
      return;
    }
    lastToggleMaximizeTickRef.current = now;
    handleToggleMaximize();
  }, [handleToggleMaximize]);

  // 标题栏按下：区分单击拖拽与双击切换最大化（避免 Win32 ReleaseCapture/WM_NCLBUTTONDOWN 吞没 dblclick，同时防止与 onDoubleClick 自抵消）
  const handleMouseDown = useCallback((e: MouseEvent<HTMLElement>) => {
    if (e.button !== 0) return;
    const target = e.target as HTMLElement | null;
    if (target && target.closest('.titlebar__controls, .titlebar__quick-actions, button, a, input, [role="button"], [role="dialog"]')) {
      return;
    }

    if (e.detail === 2) {
      triggerToggleMaximizeOnce();
      return;
    }

    if (e.detail === 1) {
      bridgeRequest('window.dragMove').catch(console.error);
    }
  }, [triggerToggleMaximizeOnce]);

  // 双击事件兜底
  const handleDoubleClick = useCallback((e: MouseEvent<HTMLElement>) => {
    const target = e.target as HTMLElement | null;
    if (target && target.closest('.titlebar__controls, .titlebar__quick-actions, button, a, input, [role="button"], [role="dialog"]')) {
      return;
    }
    triggerToggleMaximizeOnce();
  }, [triggerToggleMaximizeOnce]);

  // 标题栏右键弹出 Windows 原生系统窗口菜单
  const handleContextMenu = useCallback((e: MouseEvent<HTMLElement>) => {
    const target = e.target as HTMLElement | null;
    if (target && target.closest('.titlebar__controls, .titlebar__quick-actions')) {
      return;
    }
    e.preventDefault();
    bridgeRequest('window.showSystemMenu', { screenX: e.screenX, screenY: e.screenY }).catch(console.error);
  }, []);

  const currentAccent = ACCENT_PRESETS.find((p) => p.id === accent) || ACCENT_PRESETS[0];

  const activeLanguage = (i18n.resolvedLanguage || i18n.language || 'en').toLowerCase();
  const isZhLanguage = activeLanguage.startsWith('zh');
  const currentLangLabel = isZhLanguage ? t('general.langZh') : t('general.langEn');

  const currentThemeLabel = (() => {
    if (themePreference === 'light') return t('general.themeLight');
    if (themePreference === 'dark') return t('general.themeDark');
    return t('general.themeSystem');
  })();

  return (
    <header
      className="titlebar"
      onMouseDown={handleMouseDown}
      onDoubleClick={handleDoubleClick}
      onContextMenu={handleContextMenu}
    >
      {/* ── 左侧品牌标识与标题 ────────────────────────────────────── */}
      <div className="titlebar__brand">
        <span className="titlebar__logo">
          <LogoGlyph size={18} fill="var(--primary)" />
        </span>
        <span className="titlebar__title">{t('app.title', 'Tools3000')}</span>
        {isElevated && (
          <span className="titlebar__admin-badge" title={t('sidebar.adminTitle', 'Running as Administrator')}>
            {t('sidebar.adminBadge', 'Admin')}
          </span>
        )}
      </div>

      {/* ── 中间可拖拽区域 ────────────────────────────────────────── */}
      <div className="titlebar__drag-region" />

      {/* ── 顶部微晶快捷工具矩阵 (Quick Actions) ──────────────────── */}
      <div className="titlebar__quick-actions">
        {/* 1. 多语言微晶切换胶囊 */}
        <div className="titlebar__popover-wrapper" ref={langRef}>
          <button
            type="button"
            className={`titlebar__pill-btn ${langMenuOpen ? 'titlebar__pill-btn--active' : ''}`}
            onClick={() => {
              setLangMenuOpen((prev) => !prev);
              setThemeMenuOpen(false);
            }}
            title={t('general.language')}
            aria-label={t('general.language')}
          >
            <Globe size={13} strokeWidth={2.2} className="titlebar__pill-icon" />
            <span className="titlebar__pill-text">{currentLangLabel}</span>
            <ChevronDown size={11} strokeWidth={2.2} className="titlebar__pill-chevron" />
          </button>

          {langMenuOpen && (
            <div className="titlebar__dropdown-menu" role="dialog" aria-label={t('general.language')}>
              <button
                type="button"
                className={`titlebar__dropdown-item ${isAutoLanguage ? 'active' : ''}`}
                onClick={() => handleSelectLanguage('auto')}
              >
                <div className="titlebar__dropdown-item-left">
                  <Monitor size={14} strokeWidth={2.2} className="titlebar__dropdown-item-icon" />
                  <span className="titlebar__dropdown-item-title">{t('general.langAuto')}</span>
                </div>
                {isAutoLanguage && (
                  <Check size={13} strokeWidth={2.6} className="titlebar__dropdown-item-check" />
                )}
              </button>
              <button
                type="button"
                className={`titlebar__dropdown-item ${!isAutoLanguage && isZhLanguage ? 'active' : ''}`}
                onClick={() => handleSelectLanguage('zh-CN')}
              >
                <div className="titlebar__dropdown-item-left">
                  <span className="titlebar__dropdown-item-badge">ZH</span>
                  <span className="titlebar__dropdown-item-title">{t('general.langZh')}</span>
                </div>
                {!isAutoLanguage && isZhLanguage && (
                  <Check size={13} strokeWidth={2.6} className="titlebar__dropdown-item-check" />
                )}
              </button>
              <button
                type="button"
                className={`titlebar__dropdown-item ${!isAutoLanguage && !isZhLanguage ? 'active' : ''}`}
                onClick={() => handleSelectLanguage('en-US')}
              >
                <div className="titlebar__dropdown-item-left">
                  <span className="titlebar__dropdown-item-badge">EN</span>
                  <span className="titlebar__dropdown-item-title">{t('general.langEn')}</span>
                </div>
                {!isAutoLanguage && !isZhLanguage && (
                  <Check size={13} strokeWidth={2.6} className="titlebar__dropdown-item-check" />
                )}
              </button>
            </div>
          )}
        </div>

        {/* 2. 外观与主题调色板微晶胶囊 */}
        <div className="titlebar__popover-wrapper" ref={themeRef}>
          <button
            type="button"
            className={`titlebar__pill-btn ${themeMenuOpen ? 'titlebar__pill-btn--active' : ''}`}
            onClick={() => {
              setThemeMenuOpen((prev) => !prev);
              setLangMenuOpen(false);
            }}
            title={t('sidebar.appearanceTitle')}
            aria-label={t('sidebar.appearanceTitle')}
          >
            {themePreference === 'light' ? (
              <Sun size={13} strokeWidth={2.2} className="titlebar__pill-icon" />
            ) : themePreference === 'dark' ? (
              <Moon size={13} strokeWidth={2.2} className="titlebar__pill-icon" />
            ) : (
              <Monitor size={13} strokeWidth={2.2} className="titlebar__pill-icon" />
            )}
            <span
              className="titlebar__accent-preview-dot"
              style={{ backgroundColor: currentAccent.color }}
            />
            <span className="titlebar__pill-text">{currentThemeLabel}</span>
            <ChevronDown size={11} strokeWidth={2.2} className="titlebar__pill-chevron" />
          </button>

          {themeMenuOpen && (
            <div className="titlebar__theme-popover" role="dialog" aria-label={t('sidebar.appearanceTitle')}>
              <div className="titlebar__theme-popover-header">
                <Palette size={14} strokeWidth={2.2} className="titlebar__theme-popover-icon" />
                <span>{t('sidebar.appearanceTitle')}</span>
              </div>

              {/* 深浅模式三段式分段切换 */}
              <div className="titlebar__theme-modes">
                <button
                  type="button"
                  className={`titlebar__theme-mode-btn ${themePreference === 'light' ? 'active' : ''}`}
                  onClick={() => onSelectThemePreference?.('light')}
                  title={t('general.modeLight')}
                >
                  <Sun size={13} strokeWidth={2.2} />
                  <span>{t('general.modeLight')}</span>
                </button>
                <button
                  type="button"
                  className={`titlebar__theme-mode-btn ${themePreference === 'dark' ? 'active' : ''}`}
                  onClick={() => onSelectThemePreference?.('dark')}
                  title={t('general.modeDark')}
                >
                  <Moon size={13} strokeWidth={2.2} />
                  <span>{t('general.modeDark')}</span>
                </button>
                <button
                  type="button"
                  className={`titlebar__theme-mode-btn ${themePreference === 'system' ? 'active' : ''}`}
                  onClick={() => onSelectThemePreference?.('system')}
                  title={t('general.modeSystem')}
                >
                  <Monitor size={13} strokeWidth={2.2} />
                  <span>{t('general.modeSystem')}</span>
                </button>
              </div>

              <div className="titlebar__theme-divider" />

              {/* 主题强调色 2 列网格 (宽敞舒展无溢出) */}
              <div className="titlebar__accents-label">
                <span>{t('general.accentColor')}</span>
              </div>
              <div className="titlebar__accents-grid">
                {ACCENT_PRESETS.map((preset) => {
                  const isSelected = accent === preset.id;
                  const label = t(preset.labelKey);
                  return (
                    <button
                      key={preset.id}
                      type="button"
                      className={`titlebar__accent-btn ${isSelected ? 'active' : ''}`}
                      onClick={() => onSelectAccent?.(preset.id)}
                      title={label}
                    >
                      <span
                        className="titlebar__accent-dot"
                        style={{ backgroundColor: preset.color }}
                      >
                        {isSelected && <Check size={11} strokeWidth={3} color="#ffffff" />}
                      </span>
                      <span className="titlebar__accent-name">{label.split(' ')[0]}</span>
                    </button>
                  );
                })}
              </div>
            </div>
          )}
        </div>
      </div>

      {/* ── 右侧窗口控制按钮 ──────────────────────────────────────── */}
      <div className="titlebar__controls">
        <button
          type="button"
          className="titlebar__btn titlebar__btn--minimize"
          onClick={handleMinimize}
          title={t('window.minimize', 'Minimize')}
          aria-label={t('window.minimize', 'Minimize')}
        >
          <Minus size={13} strokeWidth={2.2} />
        </button>

        <button
          type="button"
          className="titlebar__btn titlebar__btn--maximize"
          onClick={handleToggleMaximize}
          title={isMaximized ? t('window.restore', 'Restore') : t('window.maximize', 'Maximize')}
          aria-label={isMaximized ? t('window.restore', 'Restore') : t('window.maximize', 'Maximize')}
        >
          {isMaximized ? (
            <Copy size={11} strokeWidth={2.2} style={{ transform: 'rotate(90deg)' }} />
          ) : (
            <Square size={11} strokeWidth={2.2} />
          )}
        </button>

        <button
          type="button"
          className="titlebar__btn titlebar__btn--close"
          onClick={handleClose}
          title={t('window.close', 'Close')}
          aria-label={t('window.close', 'Close')}
        >
          <X size={13} strokeWidth={2.2} />
        </button>
      </div>
    </header>
  );
};

import React, { useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Search,
  Pin,
  Minus,
  Maximize2,
  Minimize2,
  SlidersHorizontal,
  HelpCircle,
  X,
  History,
  ChevronDown,
  Clock,
  Trash2,
} from 'lucide-react';
import { bridgeRequest } from '../hooks/useBridge';
import type { SearchMode } from './searchTypes';

export interface SearchHistoryEntry {
  search: string;
  searchCount: number;
  lastSearchDate?: number;
}

export interface SearchHeaderProps {
  inputRef: React.RefObject<HTMLInputElement | null>;
  query: string;
  onUpdateQuery: (val: string) => void;
  activeCategory: string;
  searchMode: SearchMode;
  loading: boolean;
  isInitialIndexing: boolean;
  isServiceStarting: boolean;
  isPinned: boolean;
  onTogglePin: () => void;
  isMaximized?: boolean;
  onToggleMaximized?: () => void;
  onMinimizeWindow?: () => void;
  onCloseWindow?: () => void;
  showViewSettings: boolean;
  onToggleViewSettings: () => void;
  showSyntaxHelp: boolean;
  onToggleSyntaxHelp: () => void;
  onResetPlacement: () => void;
  searchHistory?: SearchHistoryEntry[];
  showHistoryMenu?: boolean;
  onToggleHistoryMenu?: () => void;
  onCloseHistoryMenu?: () => void;
  onSelectHistoryItem?: (text: string) => void;
  onRemoveHistoryItem?: (e: React.MouseEvent, text: string) => void;
  onClearAllHistory?: () => void;
  resultsCount: number;
  hasSelectedResult: boolean;
  selectedIndex: number;
  setIsComposing: (val: boolean) => void;
  onUserTypingActivity?: () => void;
}

export const SearchHeader: React.FC<SearchHeaderProps> = ({
  inputRef,
  query,
  onUpdateQuery,
  activeCategory,
  searchMode,
  loading,
  isInitialIndexing,
  isServiceStarting,
  isPinned,
  onTogglePin,
  isMaximized = false,
  onToggleMaximized,
  onMinimizeWindow,
  onCloseWindow,
  showViewSettings,
  onToggleViewSettings,
  showSyntaxHelp,
  onToggleSyntaxHelp,
  searchHistory = [],
  showHistoryMenu = false,
  onToggleHistoryMenu,
  onCloseHistoryMenu,
  onSelectHistoryItem,
  onRemoveHistoryItem,
  onClearAllHistory,
  resultsCount,
  hasSelectedResult,
  selectedIndex,
  setIsComposing,
  onUserTypingActivity,
}) => {
  const { t } = useTranslation();
  const [localValue, setLocalValue] = useState(query);
  const [historyActiveIndex, setHistoryActiveIndex] = useState(0);
  const isComposingRef = useRef(false);
  const historyDropdownRef = useRef<HTMLDivElement>(null);
  const historyTriggerRef = useRef<HTMLButtonElement>(null);

  // 外部 query 变化时（如历史词点击、清空、前缀插入等），若未在组字阶段则同步至本地输入框
  useEffect(() => {
    if (!isComposingRef.current) {
      setLocalValue(query);
    }
  }, [query]);

  // 将匹配当前输入词的历史置顶展示，同时保留其余历史记录方便随时切换回过往检索
  const orderedHistory = useMemo(() => {
    if (searchHistory.length === 0) return [];
    const needle = localValue.trim().toLowerCase();
    if (!needle) return searchHistory;
    const matched: SearchHistoryEntry[] = [];
    const rest: SearchHistoryEntry[] = [];
    for (const item of searchHistory) {
      if (item.search.toLowerCase().includes(needle)) {
        matched.push(item);
      } else {
        rest.push(item);
      }
    }
    return [...matched, ...rest];
  }, [localValue, searchHistory]);

  const effectiveHistoryIndex = orderedHistory.length > 0
    ? Math.min(historyActiveIndex, orderedHistory.length - 1)
    : -1;

  useEffect(() => {
    if (!showHistoryMenu || !onCloseHistoryMenu) return;
    const handleOutside = (event: MouseEvent) => {
      const target = event.target as Node;
      if (
        historyDropdownRef.current?.contains(target) ||
        historyTriggerRef.current?.contains(target) ||
        inputRef.current?.contains(target)
      ) {
        return;
      }
      onCloseHistoryMenu();
    };
    window.addEventListener('mousedown', handleOutside);
    return () => window.removeEventListener('mousedown', handleOutside);
  }, [inputRef, onCloseHistoryMenu, showHistoryMenu]);

  const isLoading = loading || isInitialIndexing || isServiceStarting;

  const isHeaderDragSurface = (target: EventTarget | null): boolean => {
    const el = target as HTMLElement | null;
    if (!el) return false;
    return !el.closest('input, textarea, select, button, a, [role="listbox"], .search-history-dropdown');
  };

  // 使用 4px 拖动启动阈值：防止首次 mousedown 立即进入 Win32 SC_MOVE 模态循环而吞掉双击最大化事件
  const handleTitlebarMouseDown = (e: React.MouseEvent<HTMLDivElement>) => {
    if (e.button !== 0 || e.detail >= 2 || !isHeaderDragSurface(e.target)) {
      return;
    }
    const startX = e.screenX;
    const startY = e.screenY;
    const cleanup = () => {
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', cleanup);
      window.removeEventListener('blur', cleanup);
    };
    const onMove = (moveEvt: MouseEvent) => {
      if ((moveEvt.buttons & 1) === 0) {
        cleanup();
        return;
      }
      const dx = moveEvt.screenX - startX;
      const dy = moveEvt.screenY - startY;
      if (Math.hypot(dx, dy) >= 4) {
        cleanup();
        void bridgeRequest('search.startDrag');
      }
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', cleanup);
    window.addEventListener('blur', cleanup);
  };

  return (
    <div
      className="search-input-wrapper"
      onMouseDown={handleTitlebarMouseDown}
      onDoubleClick={(e) => {
        if (isHeaderDragSurface(e.target) && onToggleMaximized) {
          onToggleMaximized();
        }
      }}
      onContextMenu={(e) => {
        if (isHeaderDragSurface(e.target)) {
          e.preventDefault();
          void bridgeRequest('search.showSystemMenu', { x: e.screenX, y: e.screenY }).catch(() => undefined);
        }
      }}
    >
      <div
        className="search-drag-surface search-icon-drag-handle"
        title={t('search.titlebarDragTip', 'Drag to move window · Double-click to maximize/restore · Right-click for window menu')}
      >
        <Search className="search-icon" size={20} aria-hidden="true" />
      </div>

      <input
        ref={inputRef}
        autoFocus
        className="search-input"
        placeholder={
          activeCategory === 'content'
            ? t('search.placeholderContent', 'Search document and code full text (Word/Excel/PDF/Code/Text)... [F1 Syntax]')
            : searchMode === 'both'
            ? t('search.placeholderHybrid', 'Hybrid Search: Match both filename and document contents... [F1 Syntax]')
            : searchMode === 'content'
            ? t('search.placeholderContentOnly', 'Full-Text Search: Search inside documents and code... [F1 Syntax]')
            : t('search.placeholder', 'Search filename, wildcard (*.txt), ext:png, or keywords... [Use content: for full-text]')
        }
        value={localValue}
        onKeyDown={(e) => {
          if (showHistoryMenu && orderedHistory.length > 0 && !isComposingRef.current) {
            if (e.key === 'ArrowDown') {
              e.preventDefault();
              e.stopPropagation();
              setHistoryActiveIndex((prev) => (prev + 1) % orderedHistory.length);
              return;
            }
            if (e.key === 'ArrowUp') {
              e.preventDefault();
              e.stopPropagation();
              setHistoryActiveIndex((prev) => (prev - 1 + orderedHistory.length) % orderedHistory.length);
              return;
            }
            if (e.key === 'Enter' && effectiveHistoryIndex >= 0) {
              e.preventDefault();
              e.stopPropagation();
              const chosen = orderedHistory[effectiveHistoryIndex];
              if (chosen && onSelectHistoryItem) {
                onSelectHistoryItem(chosen.search);
              }
              onCloseHistoryMenu?.();
              return;
            }
          }
          if (e.key !== 'Alt' && e.key !== 'Control' && e.key !== 'Shift' && e.key !== 'Meta') {
            onUserTypingActivity?.();
          }
        }}
        onPointerDown={() => onUserTypingActivity?.()}
        onChange={(event) => {
          onUserTypingActivity?.();
          const val = event.target.value;
          setLocalValue(val);
          // Windows TSF / IME 组字隔离：组字阶段完全拦截向上层 React 状态总线广播
          if (!isComposingRef.current) {
            onUpdateQuery(val);
          }
        }}
        onCompositionStart={() => {
          onUserTypingActivity?.();
          isComposingRef.current = true;
          setIsComposing(true);
        }}
        onCompositionEnd={(event) => {
          isComposingRef.current = false;
          setIsComposing(false);
          const val = event.currentTarget.value;
          setLocalValue(val);
          onUpdateQuery(val);
        }}
        role="combobox"
        aria-expanded={showHistoryMenu || resultsCount > 0}
        aria-controls={showHistoryMenu ? 'search-history-dropdown' : 'search-results'}
        aria-activedescendant={hasSelectedResult ? `search-result-${selectedIndex}` : undefined}
        spellCheck={false}
      />

      {localValue.length > 0 && (
        <button
          type="button"
          className="search-clear-input-btn"
          onClick={() => {
            setLocalValue('');
            onUpdateQuery('');
            inputRef.current?.focus();
          }}
          title={t('search.clearQuery', 'Clear search query')}
          aria-label={t('search.clearQuery', 'Clear search query')}
        >
          <X size={15} aria-hidden="true" />
        </button>
      )}

      {onToggleHistoryMenu && (
        <button
          ref={historyTriggerRef}
          type="button"
          className={`search-history-trigger-btn ${showHistoryMenu ? 'search-history-trigger-btn--active' : ''}`}
          onClick={() => {
            setHistoryActiveIndex(0);
            onToggleHistoryMenu();
            inputRef.current?.focus();
          }}
          title={t('search.historyDropdownTip', 'Search history (Alt+Down / Ctrl+H)')}
          aria-label={t('search.historyDropdownTip', 'Search history (Alt+Down / Ctrl+H)')}
          aria-haspopup="listbox"
          aria-expanded={showHistoryMenu}
        >
          <History size={15} aria-hidden="true" />
          <ChevronDown size={12} className="search-history-chevron" aria-hidden="true" />
        </button>
      )}

      {/* 零 CLS 几何锁定加载插槽：插槽恒定占据空间，加载图标仅淡入淡出，绝对不引起按钮任何横向位移 */}
      <div className="search-loading-slot search-drag-surface" aria-hidden={!isLoading}>
        <span
          className={`search-loading ${isLoading ? 'search-loading--visible' : ''}`}
          aria-label={t('common.loading', 'Loading...')}
        />
      </div>

      <button
        className={`search-help-btn ${isPinned ? 'search-help-btn--pinned' : ''}`}
        onClick={onTogglePin}
        title={isPinned ? t('search.unpinTitle', 'Unpin Always-on-Top (Ctrl+P)') : t('search.pinTitle', 'Keep Window Always on Top (Ctrl+P)')}
        aria-label={isPinned ? t('search.unpinTitle', 'Unpin Always-on-Top (Ctrl+P)') : t('search.pinTitle', 'Keep Window Always on Top (Ctrl+P)')}
        type="button"
      >
        <Pin size={16} className="search-pin-icon" aria-hidden="true" />
      </button>

      <button
        className={`search-help-btn search-view-settings-btn ${showViewSettings ? 'search-help-btn--active' : ''}`}
        data-popover-trigger="view-settings"
        aria-haspopup="dialog"
        aria-expanded={showViewSettings}
        onMouseDown={(e) => {
          if (showViewSettings) {
            e.stopPropagation();
          }
        }}
        onClick={onToggleViewSettings}
        title={t('search.viewPrefTitle', 'View and column preferences (Window size, density, columns)')}
        aria-label={t('search.viewPrefTitle', 'View and column preferences (Window size, density, columns)')}
        type="button"
      >
        <SlidersHorizontal size={16} aria-hidden="true" />
      </button>

      <button
        className={`search-help-btn ${showSyntaxHelp ? 'search-help-btn--active' : ''}`}
        onClick={onToggleSyntaxHelp}
        title={t('search.syntaxHelpTitle', 'View search syntax guide (F1)')}
        aria-label={t('search.syntaxHelpTitle', 'View search syntax guide (F1)')}
        type="button"
      >
        <HelpCircle size={16} aria-hidden="true" />
      </button>

      {/* 标准 Windows 窗口三联控制按钮（最小化 / 最大化·还原 / 关闭） */}
      {(onMinimizeWindow || onToggleMaximized || onCloseWindow) && (
        <div className="search-window-controls" role="group" aria-label={t('search.windowControlsAria', 'Window controls')}>
          {onMinimizeWindow && (
            <button
              type="button"
              className="search-win-btn search-win-btn--minimize"
              onClick={onMinimizeWindow}
              title={t('search.minimizeWindowTitle', 'Minimize to Taskbar')}
              aria-label={t('search.minimizeWindowTitle', 'Minimize to Taskbar')}
            >
              <Minus size={15} aria-hidden="true" />
            </button>
          )}
          {onToggleMaximized && (
            <button
              type="button"
              className={`search-win-btn search-win-btn--maximize ${isMaximized ? 'search-win-btn--active' : ''}`}
              onClick={onToggleMaximized}
              title={
                isMaximized
                  ? t('search.restoreWindowTitle', 'Restore Window (F11)')
                  : t('search.maximizeWindowTitle', 'Maximize Window (F11)')
              }
              aria-label={
                isMaximized
                  ? t('search.restoreWindowTitle', 'Restore Window (F11)')
                  : t('search.maximizeWindowTitle', 'Maximize Window (F11)')
              }
            >
              {isMaximized ? <Minimize2 size={14} aria-hidden="true" /> : <Maximize2 size={14} aria-hidden="true" />}
            </button>
          )}
          {onCloseWindow && (
            <button
              type="button"
              className="search-win-btn search-win-btn--close"
              onClick={onCloseWindow}
              title={t('search.closeWindowTitle', 'Close Window (Esc · Reopen anytime with Alt+Space)')}
              aria-label={t('search.closeWindowTitle', 'Close Window (Esc · Reopen anytime with Alt+Space)')}
            >
              <X size={16} aria-hidden="true" />
            </button>
          )}
        </div>
      )}

      {/* 随时可选的历史搜索下拉面板（无论搜索框为空或有内容皆可展开） */}
      {showHistoryMenu && (
        <div
          id="search-history-dropdown"
          ref={historyDropdownRef}
          className="search-history-dropdown"
          role="listbox"
          aria-label={t('search.recentSearches', 'Recent Search History')}
        >
          <div className="search-history-dropdown-head">
            <span className="search-history-dropdown-title">
              <Clock size={13} aria-hidden="true" />
              <span>{t('search.recentSearches', 'Recent Search History')}</span>
            </span>
            {orderedHistory.length > 0 && onClearAllHistory && (
              <button
                type="button"
                className="search-history-dropdown-clear"
                onClick={() => {
                  onClearAllHistory();
                  onCloseHistoryMenu?.();
                }}
              >
                <Trash2 size={12} aria-hidden="true" />
                <span>{t('search.clearHistory', 'Clear History')}</span>
              </button>
            )}
          </div>

          {orderedHistory.length === 0 ? (
            <div className="search-history-dropdown-empty">
              {t('search.historyEmptyHint', 'No search history yet. Queries will be saved automatically.')}
            </div>
          ) : (
            <div className="search-history-dropdown-list">
              {orderedHistory.map((item, idx) => {
                const isActive = idx === effectiveHistoryIndex;
                const isExactCurrent = item.search.trim().toLowerCase() === localValue.trim().toLowerCase() && localValue.trim() !== '';
                return (
                  <div
                    key={item.search}
                    role="option"
                    aria-selected={isActive}
                    className={`search-history-dropdown-item ${isActive ? 'is-active' : ''} ${isExactCurrent ? 'is-current' : ''}`}
                    onMouseEnter={() => setHistoryActiveIndex(idx)}
                    onMouseDown={(e) => {
                      e.preventDefault();
                    }}
                    onClick={() => {
                      onSelectHistoryItem?.(item.search);
                      onCloseHistoryMenu?.();
                    }}
                  >
                    <Clock size={13} className="search-history-dropdown-item-icon" aria-hidden="true" />
                    <span className="search-history-dropdown-item-text" title={item.search}>
                      {item.search}
                    </span>
                    {item.searchCount > 1 && (
                      <span className="search-history-dropdown-item-count">
                        {item.searchCount}
                      </span>
                    )}
                    {onRemoveHistoryItem && (
                      <button
                        type="button"
                        className="search-history-dropdown-item-del"
                        onMouseDown={(e) => e.stopPropagation()}
                        onClick={(e) => onRemoveHistoryItem(e, item.search)}
                        title={t('search.deleteHistoryItem', 'Delete this history')}
                        aria-label={t('search.deleteHistoryItem', 'Delete this history')}
                      >
                        <X size={12} aria-hidden="true" />
                      </button>
                    )}
                  </div>
                );
              })}
            </div>
          )}
        </div>
      )}
    </div>
  );
};

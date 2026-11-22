/* ─────────────────────────────────────────────────────────────────────────────
 * ClipboardApp — 极速呼出剪贴板历史无边框悬浮面板 (Alt+V)
 *
 * 核心能力:
 *   1. 全键盘极速流：自动聚焦搜索框，上下导航、Enter 无感贴入、Shift+Enter 纯文本、
 *      Ctrl+C 仅复制、Ctrl+P 置顶、Delete 删除、Esc 秒关；
 *   2. 图片历史零 Base64 直出 (clipboard-blobs.tools3000.local) 并提供 Tools3000
 *      独有生态联动：一键 F3 贴图 / 离线 OCR 提字 / 微信级原位翻译；
 *   3. 100% 零 Emoji + 8 语言国际化 + 实时同步系统主题。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useRef, useCallback, type FC, type KeyboardEvent as ReactKeyboardEvent, type MouseEvent as ReactMouseEvent } from 'react';
import { useTranslation } from 'react-i18next';
import {
  ClipboardList,
  Search,
  X,
  Pin,
  PinOff,
  Maximize2,
  Minimize2,
  Trash2,
  Copy,
  FileText,
  Image as ImageIcon,
  FolderOpen,
  Palette,
  Languages,
  ScanText,
  StickyNote,
  CornerDownLeft,
} from 'lucide-react';
import { CodeBadge } from './components/UIKit';
import { bridgeRequest, useBridgeEvent } from './hooks/useBridge';
import './ClipboardApp.css';

export interface ClipboardItemSummary {
  id: number;
  kind: 'text' | 'image' | 'files' | 'color';
  preview: string;
  text?: string;
  charCount: number;
  lineCount: number;
  imageFile: string;
  imageWidth: number;
  imageHeight: number;
  sourceApp: string;
  colorHex: string;
  byteSize: number;
  createdAt: number;
  updatedAt: number;
  pinned: boolean;
  copyCount: number;
}

interface ClipboardQueryResponse {
  total: number;
  matched: number;
  items: ClipboardItemSummary[];
}

type FilterKind = 'all' | 'pinned' | 'text' | 'image' | 'files' | 'color';

const BLOBS_BASE_URL = 'https://clipboard-blobs.tools3000.local/';

function formatBytesCompact(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

function formatRelativeTime(epochMs: number): string {
  if (!epochMs) return '';
  const diffSec = Math.max(0, Math.floor((Date.now() - epochMs) / 1000));
  if (diffSec < 60) return `${diffSec}s`;
  const diffMin = Math.floor(diffSec / 60);
  if (diffMin < 60) return `${diffMin}m`;
  const diffHr = Math.floor(diffMin / 60);
  if (diffHr < 24) return `${diffHr}h`;
  return `${Math.floor(diffHr / 24)}d`;
}

export const ClipboardApp: FC = () => {
  const { t } = useTranslation();
  const [query, setQuery] = useState('');
  const [filterKind, setFilterKind] = useState<FilterKind>('all');
  const [items, setItems] = useState<ClipboardItemSummary[]>([]);
  const [totalCount, setTotalCount] = useState(0);
  const [selectedIndex, setSelectedIndex] = useState(0);
  const [detailCache, setDetailCache] = useState<{ id: number; text: string } | null>(null);
  const [loading, setLoading] = useState(false);
  const [windowPinned, setWindowPinned] = useState(false);
  const [windowMaximized, setWindowMaximized] = useState(false);

  const searchInputRef = useRef<HTMLInputElement>(null);
  const selectedItem = items[selectedIndex] ?? null;
  const detailText =
    selectedItem && selectedItem.kind !== 'image'
      ? detailCache && detailCache.id === selectedItem.id
        ? detailCache.text
        : selectedItem.preview
      : '';

  const fetchItems = useCallback(async (q: string, kind: FilterKind) => {
    setLoading(true);
    try {
      const res = await bridgeRequest<ClipboardQueryResponse>(
        'clipboard.query',
        { query: q, kind, limit: 120, offset: 0 },
        { silent: true },
      );
      if (res && Array.isArray(res.items)) {
        setItems(res.items);
        setTotalCount(typeof res.total === 'number' ? res.total : res.items.length);
        setSelectedIndex((prev) => (res.items.length === 0 ? 0 : Math.min(prev, res.items.length - 1)));
      }
    } catch {
      // 静默兜底
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    const timer = window.setTimeout(() => {
      void fetchItems(query, filterKind);
    }, query ? 40 : 0);
    return () => window.clearTimeout(timer);
  }, [query, filterKind, fetchItems]);

  useEffect(() => {
    if (!selectedItem || selectedItem.kind === 'image') {
      return;
    }
    let cancelled = false;
    bridgeRequest<{ success?: boolean; item?: ClipboardItemSummary }>(
      'clipboard.getItem',
      { id: selectedItem.id },
      { silent: true },
    )
      .then((res) => {
        if (!cancelled && res?.item?.text !== undefined) {
          setDetailCache({ id: selectedItem.id, text: res.item.text });
        }
      })
      .catch(() => {
        if (!cancelled) setDetailCache({ id: selectedItem.id, text: selectedItem.preview });
      });
    return () => {
      cancelled = true;
    };
  }, [selectedItem]);

  useBridgeEvent('clipboard:shown', () => {
    setSelectedIndex(0);
    void fetchItems(query, filterKind);
    requestAnimationFrame(() => {
      searchInputRef.current?.focus();
      searchInputRef.current?.select();
    });
  });

  useBridgeEvent('clipboard:updated', () => {
    void fetchItems(query, filterKind);
  });

  useBridgeEvent('clipboard:windowState', (rawState) => {
    const state = rawState as { pinned?: boolean; maximized?: boolean } | undefined;
    if (typeof state?.pinned === 'boolean') setWindowPinned(state.pinned);
    if (typeof state?.maximized === 'boolean') setWindowMaximized(state.maximized);
  });

  useEffect(() => {
    bridgeRequest<{ pinned?: boolean; maximized?: boolean }>('clipboard.getWindowState', undefined, { silent: true })
      .then((res) => {
        if (typeof res?.pinned === 'boolean') setWindowPinned(res.pinned);
        if (typeof res?.maximized === 'boolean') setWindowMaximized(res.maximized);
      })
      .catch(() => {});
  }, []);

  const handleToggleWindowPin = useCallback(async () => {
    try {
      const res = await bridgeRequest<{ pinned?: boolean; maximized?: boolean }>(
        'clipboard.setWindowPinned',
        { pinned: !windowPinned },
        { silent: true },
      );
      if (typeof res?.pinned === 'boolean') setWindowPinned(res.pinned);
    } catch {
      setWindowPinned((prev) => !prev);
    }
  }, [windowPinned]);

  const handleToggleWindowMaximize = useCallback(async () => {
    try {
      const res = await bridgeRequest<{ pinned?: boolean; maximized?: boolean }>(
        'clipboard.toggleMaximize',
        undefined,
        { silent: true },
      );
      if (typeof res?.maximized === 'boolean') setWindowMaximized(res.maximized);
    } catch {
      // 静默兜底
    }
  }, []);

  const handleHeaderMouseDown = useCallback((e: ReactMouseEvent<HTMLElement>) => {
    if (e.button !== 0) return;
    const target = e.target as HTMLElement | null;
    if (target?.closest('input, button, a, textarea, [data-no-drag="true"]')) return;
    void bridgeRequest('clipboard.startDrag', undefined, { silent: true });
  }, []);

  const handleHeaderDoubleClick = useCallback(
    (e: ReactMouseEvent<HTMLElement>) => {
      const target = e.target as HTMLElement | null;
      if (target?.closest('input, button, a, textarea, [data-no-drag="true"]')) return;
      void handleToggleWindowMaximize();
    },
    [handleToggleWindowMaximize],
  );

  const handleStartResize = useCallback(
    (edge: 'n' | 's' | 'e' | 'w' | 'ne' | 'nw' | 'se' | 'sw') => (e: ReactMouseEvent<HTMLDivElement>) => {
      if (e.button !== 0 || windowMaximized) return;
      e.preventDefault();
      e.stopPropagation();
      void bridgeRequest('clipboard.startResize', { edge }, { silent: true });
    },
    [windowMaximized],
  );

  const handleActivate = useCallback(
    async (item: ClipboardItemSummary, plainTextOnly: boolean, autoPaste: boolean) => {
      await bridgeRequest('clipboard.pasteItem', {
        id: item.id,
        plainTextOnly,
        autoPaste,
        hideAfter: !windowPinned,
      });
    },
    [windowPinned],
  );

  const handleTogglePin = useCallback(
    async (item: ClipboardItemSummary) => {
      await bridgeRequest('clipboard.togglePin', { id: item.id });
      void fetchItems(query, filterKind);
    },
    [fetchItems, query, filterKind],
  );

  const handleDelete = useCallback(
    async (item: ClipboardItemSummary) => {
      await bridgeRequest('clipboard.deleteItem', { id: item.id });
      void fetchItems(query, filterKind);
    },
    [fetchItems, query, filterKind],
  );

  const handlePinImageAction = useCallback(
    async (item: ClipboardItemSummary, opts: { autoOcr?: boolean; autoTranslate?: boolean }) => {
      await bridgeRequest('clipboard.pinImage', {
        id: item.id,
        autoOcr: Boolean(opts.autoOcr),
        autoTranslate: Boolean(opts.autoTranslate),
      });
    },
    [],
  );

  const handleKeyDown = (e: ReactKeyboardEvent<HTMLDivElement>) => {
    if (e.key === 'Escape') {
      e.preventDefault();
      void bridgeRequest('clipboard.hideWindow', undefined, { silent: true });
      return;
    }
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      setSelectedIndex((prev) => (items.length === 0 ? 0 : (prev + 1) % items.length));
      return;
    }
    if (e.key === 'ArrowUp') {
      e.preventDefault();
      setSelectedIndex((prev) => (items.length === 0 ? 0 : (prev - 1 + items.length) % items.length));
      return;
    }
    if (e.key === 'Enter' && selectedItem) {
      e.preventDefault();
      void handleActivate(selectedItem, e.shiftKey, true);
      return;
    }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'p' && selectedItem) {
      e.preventDefault();
      void handleTogglePin(selectedItem);
      return;
    }
    if (e.key === 'Delete' && selectedItem && document.activeElement !== searchInputRef.current) {
      e.preventDefault();
      void handleDelete(selectedItem);
    }
  };

  const filters: ReadonlyArray<{ id: FilterKind; label: string }> = [
    { id: 'all', label: t('clipboard.filterAll', 'All') },
    { id: 'pinned', label: t('clipboard.filterPinned', 'Pinned') },
    { id: 'text', label: t('clipboard.filterText', 'Text') },
    { id: 'image', label: t('clipboard.filterImage', 'Images') },
    { id: 'files', label: t('clipboard.filterFiles', 'Files') },
    { id: 'color', label: t('clipboard.filterColor', 'Colors') },
  ];

  const renderKindIcon = (kind: ClipboardItemSummary['kind']) => {
    switch (kind) {
      case 'image':
        return <ImageIcon size={14} strokeWidth={2.2} aria-hidden="true" />;
      case 'files':
        return <FolderOpen size={14} strokeWidth={2.2} aria-hidden="true" />;
      case 'color':
        return <Palette size={14} strokeWidth={2.2} aria-hidden="true" />;
      default:
        return <FileText size={14} strokeWidth={2.2} aria-hidden="true" />;
    }
  };

  return (
    <div
      className={`clip-popup${windowMaximized ? ' clip-popup--maximized' : ''}`}
      onKeyDown={handleKeyDown}
    >
      {!windowMaximized && (
        <>
          <div className="clip-popup__resize-edge clip-popup__resize-edge--n" onMouseDown={handleStartResize('n')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--s" onMouseDown={handleStartResize('s')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--e" onMouseDown={handleStartResize('e')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--w" onMouseDown={handleStartResize('w')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--nw" onMouseDown={handleStartResize('nw')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--ne" onMouseDown={handleStartResize('ne')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--sw" onMouseDown={handleStartResize('sw')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--se" onMouseDown={handleStartResize('se')} aria-hidden="true" />
        </>
      )}

      <header
        className="clip-popup__header"
        onMouseDown={handleHeaderMouseDown}
        onDoubleClick={handleHeaderDoubleClick}
      >
        <div className="clip-popup__drag-handle">
          <ClipboardList size={18} strokeWidth={2.2} aria-hidden="true" />
          <span>{t('clipboard.popupTitle', 'Clipboard')}</span>
        </div>

        <div className="clip-popup__search-wrap">
          <Search size={15} strokeWidth={2.2} className="clip-popup__search-icon" aria-hidden="true" />
          <input
            ref={searchInputRef}
            type="text"
            className="clip-popup__search-input"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder={t('clipboard.searchPlaceholder', 'Search history, color, path or source app...')}
            aria-label={t('clipboard.searchPlaceholder', 'Search history, color, path or source app...')}
            autoFocus
          />
          {query.length > 0 && (
            <button
              type="button"
              className="clip-popup__search-clear"
              onClick={() => setQuery('')}
              aria-label={t('clipboard.clearSearch', 'Clear search')}
            >
              <X size={14} aria-hidden="true" />
            </button>
          )}
        </div>

        <div className="clip-popup__win-controls">
          <button
            type="button"
            className={`clip-popup__win-btn${windowPinned ? ' clip-popup__win-btn--pinned' : ''}`}
            onClick={() => void handleToggleWindowPin()}
            aria-pressed={windowPinned}
            title={
              windowPinned
                ? t('clipboard.unpinWindow', 'Unpin window (auto-hide on blur)')
                : t('clipboard.pinWindow', 'Keep window on top')
            }
            aria-label={
              windowPinned
                ? t('clipboard.unpinWindow', 'Unpin window (auto-hide on blur)')
                : t('clipboard.pinWindow', 'Keep window on top')
            }
          >
            {windowPinned ? <Pin size={15} strokeWidth={2.3} aria-hidden="true" /> : <Pin size={15} strokeWidth={2} aria-hidden="true" />}
          </button>

          <button
            type="button"
            className="clip-popup__win-btn"
            onClick={() => void handleToggleWindowMaximize()}
            title={
              windowMaximized
                ? t('clipboard.restoreWindow', 'Restore window size')
                : t('clipboard.maximizeWindow', 'Maximize window')
            }
            aria-label={
              windowMaximized
                ? t('clipboard.restoreWindow', 'Restore window size')
                : t('clipboard.maximizeWindow', 'Maximize window')
            }
          >
            {windowMaximized ? <Minimize2 size={15} strokeWidth={2} aria-hidden="true" /> : <Maximize2 size={15} strokeWidth={2} aria-hidden="true" />}
          </button>

          <button
            type="button"
            className="clip-popup__win-btn clip-popup__win-btn--close"
            onClick={() => void bridgeRequest('clipboard.hideWindow', undefined, { silent: true })}
            title={t('clipboard.closeWindow', 'Close')}
            aria-label={t('clipboard.closeWindow', 'Close')}
          >
            <X size={16} aria-hidden="true" />
          </button>
        </div>
      </header>

      <div className="clip-popup__filters" onMouseDown={handleHeaderMouseDown}>
        <div className="clip-popup__filter-pills" role="tablist" aria-label={t('clipboard.filterBarAria', 'Clipboard categories')}>
          {filters.map((f) => (
            <button
              key={f.id}
              type="button"
              role="tab"
              aria-selected={filterKind === f.id}
              className={`clip-popup__pill${filterKind === f.id ? ' clip-popup__pill--active' : ''}`}
              onClick={() => {
                setFilterKind(f.id);
                setSelectedIndex(0);
              }}
            >
              <span>{f.label}</span>
            </button>
          ))}
        </div>
        <span className="clip-popup__count-badge">
          {t('clipboard.totalCountBadge', { count: totalCount, defaultValue: `${totalCount} items` })}
        </span>
      </div>

      <main className="clip-popup__body">
        <section className="clip-popup__list" role="listbox" aria-label={t('clipboard.listAria', 'Clipboard history list')}>
          {items.length === 0 ? (
            <div className="clip-popup__empty">
              <ClipboardList size={28} strokeWidth={1.8} aria-hidden="true" />
              <span>
                {loading
                  ? t('clipboard.loading', 'Loading...')
                  : t('clipboard.emptyHint', 'No matching clipboard entries yet. Copy text, images, or files to record automatically.')}
              </span>
            </div>
          ) : (
            items.map((item, idx) => (
              <button
                key={item.id}
                type="button"
                role="option"
                aria-selected={idx === selectedIndex}
                className={`clip-popup__item${idx === selectedIndex ? ' clip-popup__item--selected' : ''}`}
                onClick={() => setSelectedIndex(idx)}
                onDoubleClick={() => void handleActivate(item, false, true)}
              >
                <div className="clip-popup__item-top">
                  <span className="clip-popup__item-kind">
                    {renderKindIcon(item.kind)}
                    {item.sourceApp ? <CodeBadge>{item.sourceApp}</CodeBadge> : null}
                  </span>
                  <span style={{ display: 'inline-flex', alignItems: 'center', gap: '6px' }}>
                    {item.copyCount > 1 && <CodeBadge>{`x${item.copyCount}`}</CodeBadge>}
                    {item.pinned && <Pin size={13} strokeWidth={2.4} className="clip-popup__pin-dot" aria-hidden="true" />}
                  </span>
                </div>

                <div className="clip-popup__item-preview">
                  {item.kind === 'color' && item.colorHex && (
                    <span
                      className="clip-popup__color-swatch"
                      style={{ backgroundColor: item.colorHex }}
                      aria-hidden="true"
                    />
                  )}
                  {item.kind === 'image' && item.imageFile && (
                    <img
                      className="clip-popup__thumb"
                      src={`${BLOBS_BASE_URL}${encodeURIComponent(item.imageFile)}`}
                      alt={item.preview}
                      loading="lazy"
                    />
                  )}
                  <span className="clip-popup__text-snippet">
                    {item.kind === 'color' ? `${item.colorHex}  ${item.preview}` : item.preview}
                  </span>
                </div>

                <div className="clip-popup__item-meta">
                  <span>
                    {item.kind === 'image'
                      ? `${item.imageWidth}x${item.imageHeight} · ${formatBytesCompact(item.byteSize)}`
                      : `${item.charCount} ${t('clipboard.charsUnit', 'chars')}`}
                  </span>
                  <span>{formatRelativeTime(item.updatedAt)}</span>
                </div>
              </button>
            ))
          )}
        </section>

        <aside className="clip-popup__detail">
          {selectedItem ? (
            <>
              <div className="clip-popup__detail-body">
                {selectedItem.kind === 'image' && selectedItem.imageFile ? (
                  <div className="clip-popup__detail-image-wrap">
                    <img
                      className="clip-popup__detail-image"
                      src={`${BLOBS_BASE_URL}${encodeURIComponent(selectedItem.imageFile)}`}
                      alt={selectedItem.preview}
                    />
                    <CodeBadge>
                      {`${selectedItem.imageWidth} x ${selectedItem.imageHeight} · ${formatBytesCompact(selectedItem.byteSize)}`}
                    </CodeBadge>
                  </div>
                ) : selectedItem.kind === 'color' && selectedItem.colorHex ? (
                  <div className="clip-popup__detail-color">
                    <div
                      className="clip-popup__detail-color-block"
                      style={{ backgroundColor: selectedItem.colorHex }}
                    />
                    <CodeBadge>{selectedItem.colorHex}</CodeBadge>
                  </div>
                ) : (
                  <pre className="clip-popup__detail-text">{detailText || selectedItem.preview}</pre>
                )}
              </div>

              <div className="clip-popup__detail-actions">
                <button
                  type="button"
                  className="clip-popup__action-btn clip-popup__action-btn--primary"
                  onClick={() => void handleActivate(selectedItem, false, true)}
                >
                  <CornerDownLeft size={14} aria-hidden="true" />
                  <span>{t('clipboard.actionPaste', 'Paste')}</span>
                </button>

                <button
                  type="button"
                  className="clip-popup__action-btn"
                  onClick={() => void handleActivate(selectedItem, true, false)}
                >
                  <Copy size={14} aria-hidden="true" />
                  <span>{t('clipboard.actionCopyOnly', 'Copy')}</span>
                </button>

                {selectedItem.kind === 'image' && (
                  <>
                    <button
                      type="button"
                      className="clip-popup__action-btn"
                      onClick={() => void handlePinImageAction(selectedItem, {})}
                    >
                      <StickyNote size={14} aria-hidden="true" />
                      <span>{t('clipboard.actionPinImage', 'Pin (F3)')}</span>
                    </button>
                    <button
                      type="button"
                      className="clip-popup__action-btn"
                      onClick={() => void handlePinImageAction(selectedItem, { autoOcr: true })}
                    >
                      <ScanText size={14} aria-hidden="true" />
                      <span>{t('clipboard.actionOcrImage', 'OCR')}</span>
                    </button>
                    <button
                      type="button"
                      className="clip-popup__action-btn"
                      onClick={() => void handlePinImageAction(selectedItem, { autoTranslate: true })}
                    >
                      <Languages size={14} aria-hidden="true" />
                      <span>{t('clipboard.actionTranslateImage', 'Translate')}</span>
                    </button>
                  </>
                )}

                <button
                  type="button"
                  className="clip-popup__action-btn"
                  onClick={() => void handleTogglePin(selectedItem)}
                >
                  {selectedItem.pinned ? <PinOff size={14} aria-hidden="true" /> : <Pin size={14} aria-hidden="true" />}
                  <span>{selectedItem.pinned ? t('clipboard.actionUnpin', 'Unpin') : t('clipboard.actionPin', 'Pin')}</span>
                </button>

                <button
                  type="button"
                  className="clip-popup__action-btn"
                  onClick={() => void handleDelete(selectedItem)}
                >
                  <Trash2 size={14} aria-hidden="true" />
                  <span>{t('clipboard.actionDelete', 'Delete')}</span>
                </button>
              </div>
            </>
          ) : (
            <div className="clip-popup__empty">
              <span>{t('clipboard.selectItemHint', 'Select an entry to preview')}</span>
            </div>
          )}
        </aside>
      </main>

      <footer className="clip-popup__footer">
        <div className="clip-popup__hints">
          <span className="clip-popup__hint">
            <CodeBadge>Enter</CodeBadge>
            <span>{t('clipboard.hintPaste', 'Paste')}</span>
          </span>
          <span className="clip-popup__hint">
            <CodeBadge>Shift+Enter</CodeBadge>
            <span>{t('clipboard.hintPlainText', 'Plain Text')}</span>
          </span>
          <span className="clip-popup__hint">
            <CodeBadge>Ctrl+P</CodeBadge>
            <span>{t('clipboard.hintPin', 'Pin')}</span>
          </span>
          <span className="clip-popup__hint">
            <CodeBadge>Esc</CodeBadge>
            <span>{t('clipboard.hintClose', 'Close')}</span>
          </span>
        </div>
        <span>{t('clipboard.persistedBadge', 'Persisted Across Reboots')}</span>
      </footer>
    </div>
  );
};

export default ClipboardApp;

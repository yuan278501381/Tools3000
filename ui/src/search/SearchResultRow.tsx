import {
  memo,
  type CSSProperties,
  type MouseEvent as ReactMouseEvent,
  type ReactNode,
  type RefCallback,
} from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { bridgeRequest } from '../hooks/useBridge';
import {
  Activity,
  AppWindow,
  Cpu,
  File,
  FileArchive,
  FileAudio,
  FileCode,
  FileImage,
  FileText,
  FileVideo,
  Folder,
  Globe,
  HardDrive,
  Monitor,
  Network,
  Settings,
  Shield,
  Sliders,
  Terminal,
  Wrench,
} from 'lucide-react';
import {
  getColumnDisplayLabel,
  resolveColumnDefinition,
  resolveTableCellText,
} from './columnCatalog';
import { IMAGE_EXTENSIONS } from './searchConstants';
import type {
  ColumnLayout,
  PropertyCellValue,
  SearchDensity,
  SearchIconStyle,
  SearchResult,
  TableColumnId,
} from './searchTypes';
import {
  extractParentDirectoryPath,
  formatFileSize,
  formatWindowsTime,
  getVisibleTableColumns,
} from './searchUtils';

const highlightRegexCache = new Map<string, { regex: RegExp; validKeywords: string[] }>();

function getHighlightRegex(queryKeywords: string[]): { regex: RegExp; validKeywords: string[] } | null {
  if (queryKeywords.length === 0) return null;
  const cacheKey = queryKeywords.join('\u0000');
  const cached = highlightRegexCache.get(cacheKey);
  if (cached) return cached;

  const validKeywords = queryKeywords
    .map((keyword) => keyword.trim())
    .filter((keyword) => keyword.length > 0)
    .sort((left, right) => right.length - left.length);
  if (validKeywords.length === 0) return null;

  const escaped = validKeywords
    .map((keyword) => keyword.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'))
    .join('|');
  if (!escaped) return null;

  try {
    const result = { regex: new RegExp(`(${escaped})`, 'gi'), validKeywords };
    if (highlightRegexCache.size > 100) highlightRegexCache.clear();
    highlightRegexCache.set(cacheKey, result);
    return result;
  } catch {
    return null;
  }
}

function highlightMatch(text: string, queryKeywords: string[]): ReactNode {
  if (!text || queryKeywords.length === 0) return text;
  const compiled = getHighlightRegex(queryKeywords);
  if (!compiled) return text;

  const { regex, validKeywords } = compiled;
  try {
    const parts = text.split(regex);
    if (parts.length <= 1) return text;
    return parts.map((part, index) => {
      const isMatch = validKeywords.some((keyword) => keyword.toLowerCase() === part.toLowerCase());
      return isMatch
        ? <mark key={index} className="search-match-highlight">{part}</mark>
        : part;
    });
  } catch {
    return text;
  }
}

function highlightSecondarySnippetMatches(segment: string, queryKeywords: string[], keyPrefix: string): ReactNode {
  if (!segment || queryKeywords.length === 0) return segment;
  const compiled = getHighlightRegex(queryKeywords);
  if (!compiled) return segment;
  const { regex, validKeywords } = compiled;
  try {
    const parts = segment.split(regex);
    if (parts.length <= 1) return segment;
    return parts.map((part, index) => {
      const isMatch = validKeywords.some((keyword) => keyword.toLowerCase() === part.toLowerCase());
      return isMatch
        ? <mark key={`${keyPrefix}-${index}`} className="snippet-highlight">{part}</mark>
        : part;
    });
  } catch {
    return segment;
  }
}

function renderSnippetWithHighlight(
  text: string,
  offset?: number,
  length?: number,
  queryKeywords: string[] = [],
): ReactNode {
  if (offset === undefined || length === undefined || offset < 0 || length <= 0 || offset >= text.length) {
    return highlightSecondarySnippetMatches(text, queryKeywords, 's');
  }
  const before = text.substring(0, offset);
  const primary = text.substring(offset, offset + length);
  const after = text.substring(offset + length);
  return (
    <>
      {highlightSecondarySnippetMatches(before, queryKeywords, 'b')}
      <mark className="snippet-highlight">{primary}</mark>
      {highlightSecondarySnippetMatches(after, queryKeywords, 'a')}
    </>
  );
}

function renderCommandIcon(iconHint: string | undefined, iconSize: number): ReactNode {
  switch (iconHint) {
    case 'sliders':
      return <Sliders className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'settings':
      return <Settings className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'cpu':
      return <Cpu className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'network':
      return <Network className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'wrench':
      return <Wrench className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'activity':
      return <Activity className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'harddrive':
      return <HardDrive className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'filetext':
      return <FileText className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'shield':
      return <Shield className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'monitor':
      return <Monitor className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
    case 'terminal':
    default:
      return <Terminal className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
  }
}

function renderFileIcon(
  name: string,
  isDirectory: boolean,
  density: SearchDensity = 'standard',
  iconStyle: SearchIconStyle = 'native',
  iconBase64?: string,
  resultType?: SearchResult['resultType'],
  iconHint?: string,
): ReactNode {
  const iconSize = density === 'compact' ? 15 : density === 'comfortable' ? 18 : 16;
  if (resultType === 'bookmark') {
    return <Globe className="file-icon file-icon--bookmark" size={iconSize} aria-hidden="true" />;
  }
  if (resultType === 'command') {
    return renderCommandIcon(iconHint, iconSize);
  }
  if (iconStyle === 'native' && iconBase64) {
    return (
      <img
        src={iconBase64}
        alt=""
        className="file-icon file-icon--native"
        style={{
          width: iconSize,
          height: iconSize,
          minWidth: iconSize,
          minHeight: iconSize,
          objectFit: 'contain',
          display: 'inline-block',
          verticalAlign: 'middle',
          userSelect: 'none',
          pointerEvents: 'none',
        }}
        aria-hidden="true"
        draggable={false}
      />
    );
  }

  if (resultType === 'app') {
    return <AppWindow className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
  }
  if (isDirectory) return <Folder className="file-icon file-icon--folder" size={iconSize} aria-hidden="true" />;
  const extension = name.toLowerCase();
  if (IMAGE_EXTENSIONS.test(extension)) return <FileImage className="file-icon file-icon--image" size={iconSize} aria-hidden="true" />;
  if (/\.(cpp|c|h|hpp|rs|go|py|js|ts|tsx|jsx|java|cs|html|css|json|yaml|toml|sql|sh|bat|ps1|lua)$/i.test(extension)) return <FileCode className="file-icon file-icon--code" size={iconSize} aria-hidden="true" />;
  if (/\.(zip|rar|7z|tar|gz|bz2|xz|iso)$/i.test(extension)) return <FileArchive className="file-icon file-icon--archive" size={iconSize} aria-hidden="true" />;
  if (/\.(exe|msi|bat|cmd|ps1|lnk)$/i.test(extension)) return <AppWindow className="file-icon file-icon--exe" size={iconSize} aria-hidden="true" />;
  if (/\.(mp4|mkv|avi|mov|wmv|flv|webm)$/i.test(extension)) return <FileVideo className="file-icon file-icon--media" size={iconSize} aria-hidden="true" />;
  if (/\.(mp3|wav|flac|aac|ogg|m4a|wma)$/i.test(extension)) return <FileAudio className="file-icon file-icon--audio" size={iconSize} aria-hidden="true" />;
  if (/\.(txt|md|pdf|doc|docx|xls|xlsx|ppt|pptx|log|csv)$/i.test(extension)) return <FileText className="file-icon file-icon--doc" size={iconSize} aria-hidden="true" />;
  return <File className="file-icon" size={iconSize} aria-hidden="true" />;
}

interface SearchResultRowProps {
  result: SearchResult;
  index: number;
  selected: boolean;
  density: SearchDensity;
  tableGridTemplate?: string;
  tableMinWidthPx?: number;
  iconStyle: SearchIconStyle;
  iconBase64?: string;
  columns: ColumnLayout;
  rowProperties?: Record<string, PropertyCellValue>;
  queryKeywords: string[];
  onHover: (index: number) => void;
  onSelect: (index: number) => void;
  onOpen: (result: SearchResult) => void;
  onContextMenu: (event: ReactMouseEvent, index: number, result: SearchResult) => void;
  measureRef?: RefCallback<HTMLLIElement>;
  style?: CSSProperties;
  setSize?: number;
}

const CELL_FORMATTERS = {
  formatFileSize,
  formatWindowsTime,
  extractParentDirectoryPath,
};

export const SearchResultRow = memo(function SearchResultRow({
  result,
  index,
  selected,
  density,
  tableGridTemplate,
  tableMinWidthPx,
  iconStyle,
  iconBase64,
  columns,
  rowProperties,
  queryKeywords,
  onHover,
  onSelect,
  onOpen,
  onContextMenu,
  measureRef,
  style,
  setSize,
}: SearchResultRowProps) {
  const { t } = useTranslation();
  const visibleTableCols = getVisibleTableColumns(columns);
  const showRunCountColumn = visibleTableCols.includes('runCount');
  const hasSnippets = Boolean(columns.snippets && result.snippets && result.snippets.length > 0);

  const translateLabel = (key: string, defaultVal?: string) =>
    t(key as never, defaultVal as never) as unknown as string;

  const renderTableCell = (colId: TableColumnId): ReactNode => {
    const isBookmark = result.resultType === 'bookmark';
    const isCommand = result.resultType === 'command';
    const isApp = result.resultType === 'app';
    const displayName = isCommand && result.titleKey
      ? translateLabel(result.titleKey, result.name)
      : result.name;

    if (colId === 'name') {
      return (
        <div key="name" data-table-col="name" className="table-cell table-cell--name" title={displayName}>
          {renderFileIcon(result.name, result.isDirectory, density, iconStyle, iconBase64, result.resultType, result.iconHint)}
          <span className="file-name-text">{highlightMatch(displayName, queryKeywords)}</span>
          {isBookmark && (
            <span
              className="file-bookmark-badge"
              title={result.bookmarkFolder ? `${result.browserSource || 'Browser'} · ${result.bookmarkFolder}` : (result.browserSource || t('search.catBookmarks', 'Bookmarks'))}
            >
              {result.browserSource || t('search.catBookmarks', 'Bookmarks')}
            </span>
          )}
          {isCommand && (
            <span
              className="file-bookmark-badge"
              title={result.commandHint || t('search.badgeCommand', 'Command')}
            >
              {t('search.badgeCommand', 'Command')}
            </span>
          )}
          {isApp && (
            <span
              className="file-bookmark-badge"
              title={result.appSource || t('search.badgeApp', 'App')}
            >
              {t('search.badgeApp', 'App')}
            </span>
          )}
          {/* 严格遵循 MECE 原则：当独立「打开次数」列已显示时绝不在文件名列重复展示徽标；仅当用户主动隐藏该列时回退提示 */}
          {!showRunCountColumn && Boolean(result.runCount && result.runCount > 0) && (
            <span className="file-run-badge" title={t('search.runCountTip', 'Opened {{count}} times in history', { count: result.runCount })}>
              {t('search.runCountBadge', '{{count}} runs', { count: result.runCount })}
            </span>
          )}
        </div>
      );
    }

    if (colId === 'path') {
      const directoryPath = isCommand
        ? (result.commandHint || result.path)
        : isBookmark
          ? result.path
          : (extractParentDirectoryPath(result.path) || result.path);
      return (
        <div key="path" data-table-col="path" className="table-cell table-cell--dir" title={directoryPath || result.path}>
          <span className="table-dir-text">
            {directoryPath ? highlightMatch(directoryPath, queryKeywords) : '—'}
          </span>
        </div>
      );
    }

    if (colId === 'size' && (isBookmark || isCommand || isApp)) {
      return (
        <div key="size" data-table-col="size" className="table-cell table-cell--size">
          —
        </div>
      );
    }

    if (colId === 'runCount') {
      const runs = typeof result.runCount === 'number' && Number.isFinite(result.runCount) && result.runCount > 0
        ? Math.floor(result.runCount)
        : 0;
      return (
        <div
          key="runCount"
          data-table-col="runCount"
          className={`table-cell table-cell--mono-right table-cell--run-count${runs > 0 ? ' table-cell--run-count-active' : ''}`}
          title={
            runs > 0
              ? t('search.runCountTip', 'Opened {{count}} times in history', { count: runs })
              : t('search.colRunCount', 'Open Count')
          }
        >
          {runs > 0 ? runs : '—'}
        </div>
      );
    }

    const def = resolveColumnDefinition(colId);
    const cellText = resolveTableCellText(colId, result, rowProperties, CELL_FORMATTERS);
    const colLabel = getColumnDisplayLabel(colId, translateLabel);

    let cellClass = 'table-cell';
    if (colId === 'size') {
      cellClass += ' table-cell--size';
    } else if (colId === 'modified' || colId === 'created' || colId === 'accessed' || colId === 'dateTaken') {
      cellClass += ' table-cell--date';
    } else if (def.isMono) {
      cellClass += def.align === 'right' ? ' table-cell--mono-right' : ' table-cell--mono';
    } else {
      cellClass += def.align === 'right' ? ' table-cell--text-right' : ' table-cell--text';
    }

    return (
      <div
        key={colId}
        data-table-col={colId}
        className={cellClass}
        title={cellText ? `${colLabel}: ${cellText}` : colLabel}
      >
        {cellText || '—'}
      </div>
    );
  };

  const renderSnippetsBlock = () => {
    if (!hasSnippets || !result.snippets) return null;
    return (
      <div className="file-snippets-container file-snippets-container--table">
        {result.snippets.map((snippet, snippetIndex) => (
          <div key={snippetIndex} className="file-snippet-row">
            <button
              type="button"
              className="snippet-line-num"
              title={t('search.copyLineTip', 'Click to copy line {{line}} content', { line: snippet.lineNumber })}
              onClick={(e) => {
                e.stopPropagation();
                const text = snippet.lineContent.trim();
                void (async () => {
                  try {
                    if (navigator.clipboard?.writeText) {
                      try {
                        await navigator.clipboard.writeText(text);
                      } catch {
                        await bridgeRequest('system.copyText', { text });
                      }
                    } else {
                      await bridgeRequest('system.copyText', { text });
                    }
                    toast.success(t('search.copiedLineToast', 'Copied line {{line}} content to clipboard', { line: snippet.lineNumber }));
                  } catch {
                    toast.error(t('search.copyFailed', 'Failed to copy text to clipboard'));
                  }
                })();
              }}
            >
              L{snippet.lineNumber}
            </button>
            <span className="snippet-text">
              {renderSnippetWithHighlight(snippet.lineContent, snippet.matchOffset, snippet.matchLength, queryKeywords)}
            </span>
          </div>
        ))}
      </div>
    );
  };

  const mainRowStyle: CSSProperties | undefined = tableGridTemplate
    ? {
        gridTemplateColumns: tableGridTemplate,
        ...(tableMinWidthPx ? { minWidth: `${tableMinWidthPx}px` } : {}),
      }
    : undefined;

  return (
    <li
      id={`search-result-${index}`}
      data-virtual-index={index}
      data-virtual-path={result.path}
      className={`search-result-item search-result-item--table ${index % 2 === 1 ? 'search-result-item--even' : ''} ${selected ? 'selected' : ''}`.replace(/\s+/g, ' ').trim()}
      role="option"
      aria-selected={selected}
      aria-posinset={index + 1}
      aria-setsize={setSize}
      ref={measureRef}
      style={style}
      onMouseMove={(event) => {
        if (event.buttons === 0 && (event.movementX !== 0 || event.movementY !== 0)) {
          onHover(index);
        }
      }}
      onMouseDown={(event) => {
        if (event.button === 0 && !event.shiftKey) {
          event.preventDefault();
        }
      }}
      onClick={() => onSelect(index)}
      onDoubleClick={() => onOpen(result)}
      onContextMenu={(event) => onContextMenu(event, index, result)}
    >
      <div className="search-table-row-stack">
        <div className="search-table-main-row" style={mainRowStyle}>
          {visibleTableCols.map(renderTableCell)}
        </div>
        {renderSnippetsBlock()}
      </div>
    </li>
  );
}, (previous, next) => (
  previous.selected === next.selected
  && previous.index === next.index
  && previous.result === next.result
  && previous.density === next.density
  && previous.tableGridTemplate === next.tableGridTemplate
  && previous.tableMinWidthPx === next.tableMinWidthPx
  && previous.iconStyle === next.iconStyle
  && previous.iconBase64 === next.iconBase64
  && previous.columns === next.columns
  && previous.rowProperties === next.rowProperties
  && previous.queryKeywords === next.queryKeywords
  && previous.setSize === next.setSize
  && previous.style?.top === next.style?.top
  && previous.onHover === next.onHover
  && previous.onSelect === next.onSelect
  && previous.onOpen === next.onOpen
  && previous.onContextMenu === next.onContextMenu
));

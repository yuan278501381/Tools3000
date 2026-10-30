import {
  memo,
  type CSSProperties,
  type MouseEvent as ReactMouseEvent,
  type ReactNode,
  type RefCallback,
} from 'react';
import { useTranslation } from 'react-i18next';
import {
  AppWindow,
  File,
  FileArchive,
  FileAudio,
  FileCode,
  FileImage,
  FileText,
  FileVideo,
  Folder,
} from 'lucide-react';
import { CodeBadge } from '../components/UIKit';
import { IMAGE_EXTENSIONS } from './searchConstants';
import type {
  ColumnLayout,
  SearchDensity,
  SearchIconStyle,
  SearchResult,
} from './searchTypes';
import {
  extractParentFolder,
  formatFileSize,
  formatWindowsTime,
  getFileTypeBadge,
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

function renderFileIcon(
  name: string,
  isDirectory: boolean,
  density: SearchDensity = 'standard',
  iconStyle: SearchIconStyle = 'native',
  iconBase64?: string,
): ReactNode {
  const iconSize = density === 'compact' ? 14 : density === 'comfortable' ? 22 : 18;
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

  if (isDirectory) return <Folder className="file-icon file-icon--folder" size={iconSize} aria-hidden="true" />;
  const extension = name.toLowerCase();
  if (IMAGE_EXTENSIONS.test(extension)) return <FileImage className="file-icon file-icon--image" size={iconSize} aria-hidden="true" />;
  if (/\.(cpp|c|h|hpp|rs|go|py|js|ts|tsx|jsx|java|cs|html|css|json|yaml|toml|sql|sh|bat|ps1|lua)$/i.test(extension)) return <FileCode className="file-icon file-icon--code" size={iconSize} aria-hidden="true" />;
  if (/\.(zip|rar|7z|tar|gz|bz2|xz|iso)$/i.test(extension)) return <FileArchive className="file-icon file-icon--archive" size={iconSize} aria-hidden="true" />;
  if (/\.(exe|msi|bat|cmd|ps1|lnk)$/i.test(extension)) return <AppWindow className="file-icon file-icon--app" size={iconSize} aria-hidden="true" />;
  if (/\.(mp4|mkv|avi|mov|wmv|flv|webm)$/i.test(extension)) return <FileVideo className="file-icon file-icon--video" size={iconSize} aria-hidden="true" />;
  if (/\.(mp3|wav|flac|aac|ogg|m4a|wma)$/i.test(extension)) return <FileAudio className="file-icon file-icon--audio" size={iconSize} aria-hidden="true" />;
  if (/\.(txt|md|pdf|doc|docx|xls|xlsx|ppt|pptx|log|csv)$/i.test(extension)) return <FileText className="file-icon file-icon--doc" size={iconSize} aria-hidden="true" />;
  return <File className="file-icon" size={iconSize} aria-hidden="true" />;
}

interface SearchResultRowProps {
  result: SearchResult;
  index: number;
  selected: boolean;
  density: SearchDensity;
  iconStyle: SearchIconStyle;
  iconBase64?: string;
  columns: ColumnLayout;
  queryKeywords: string[];
  onHover: (index: number) => void;
  onSelect: (index: number) => void;
  onOpen: (result: SearchResult) => void;
  onContextMenu: (event: ReactMouseEvent, index: number, result: SearchResult) => void;
  measureRef?: RefCallback<HTMLLIElement>;
  style?: CSSProperties;
  setSize?: number;
}

export const SearchResultRow = memo(function SearchResultRow({
  result,
  index,
  selected,
  density,
  iconStyle,
  iconBase64,
  columns,
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
  const parentFolder = columns.parent ? extractParentFolder(result.path) : '';
  const sizeText = columns.size ? formatFileSize(result.size, result.isDirectory) : '';
  const badge = columns.ext ? getFileTypeBadge(result.name, result.isDirectory) : null;
  const modifiedText = columns.modified ? formatWindowsTime(result.lastWriteTime) : '';
  const createdText = columns.created ? formatWindowsTime(result.creationTime) : '';

  return (
    <li
      id={`search-result-${index}`}
      data-virtual-index={index}
      className={`search-result-item ${selected ? 'selected' : ''}`}
      role="option"
      aria-selected={selected}
      aria-posinset={index + 1}
      aria-setsize={setSize}
      ref={measureRef}
      style={style}
      // Scrolling a virtualized list can move a new row under a stationary
      // pointer and synthesize mouseenter, stealing keyboard selection. Only
      // actual pointer motion with no buttons pressed (not dragging scrollbar) is allowed.
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
      {renderFileIcon(result.name, result.isDirectory, density, iconStyle, iconBase64)}
      <div className="file-info">
        {density === 'compact' ? (
          <div className="file-row-main">
            {columns.name && (
              <span className="file-name" style={{ flex: `${columns.nameFlex} 1 0` }} title={result.name}>
                <span className="file-name-text">{highlightMatch(result.name, queryKeywords)}</span>
                {Boolean(result.runCount && result.runCount > 0) && (
                  <span className="file-run-badge" title={t('search.runCountTip', 'Opened {{count}} times in history', { count: result.runCount })}>
                    {t('search.runCountBadge', '{{count}} runs', { count: result.runCount })}
                  </span>
                )}
              </span>
            )}
            {badge && <span className={`file-ext-badge ${badge.colorClass}`}>{badge.label}</span>}
            {columns.parent && (
              <span className="file-parent-column" title={t('search.parentFolderTip', 'Folder: {{folder}}', { folder: parentFolder || '—' }).replace('{{folder}}', parentFolder || '—')}>
                <Folder size={11} aria-hidden="true" />
                {parentFolder ? highlightMatch(parentFolder, queryKeywords) : '—'}
              </span>
            )}
            {columns.path && (
              <span className="file-path-inline" style={{ flex: `${columns.pathFlex} 1 0` }} title={result.path}>
                <CodeBadge className="file-path-text" variant="muted">{highlightMatch(result.path, queryKeywords)}</CodeBadge>
              </span>
            )}
            <div className="file-meta-top">
              {columns.size && <span className="meta-size-badge" title={t('search.colSize', 'Size')}>{sizeText || '—'}</span>}
              {columns.modified && (
                <span className="meta-date-mod" title={t('search.colModified', 'Modified')}>
                  <span className="meta-field-label">{t('search.colModifiedShort', 'Mod')}</span>{modifiedText || '—'}
                </span>
              )}
              {columns.created && (
                <span className="meta-date-create" title={t('search.colCreated', 'Created')}>
                  <span className="meta-field-label">{t('search.colCreatedShort', 'Cre')}</span>{createdText || '—'}
                </span>
              )}
            </div>
          </div>
        ) : (
          <>
            <div className="file-row-main">
              {columns.name && (
                <span className="file-name" title={result.name}>
                  <span className="file-name-text">{highlightMatch(result.name, queryKeywords)}</span>
                  {Boolean(result.runCount && result.runCount > 0) && (
                    <span className="file-run-badge" title={t('search.runCountTip', 'Opened {{count}} times in history', { count: result.runCount })}>
                      {t('search.runCountBadge', '{{count}} runs', { count: result.runCount })}
                    </span>
                  )}
                </span>
              )}
              {badge && <span className={`file-ext-badge ${badge.colorClass}`}>{badge.label}</span>}
              <div className="file-meta-top">
                {columns.size && <span className="meta-size-badge" title={t('search.colSize', 'Size')}>{sizeText || '—'}</span>}
                {columns.modified && (
                  <span className="meta-date-mod" title={t('search.colModified', 'Modified')}>
                    <span className="meta-field-label">{t('search.colModifiedShort', 'Mod')}</span>{modifiedText || '—'}
                  </span>
                )}
                {columns.created && (
                  <span className="meta-date-create" title={t('search.colCreated', 'Created')}>
                    <span className="meta-field-label">{t('search.colCreatedShort', 'Cre')}</span>{createdText || '—'}
                  </span>
                )}
              </div>
            </div>
            <div className="file-row-sub">
              {columns.parent && (
                <span className="file-parent-column" title={t('search.parentFolderTip', 'Folder: {{folder}}', { folder: parentFolder || '—' }).replace('{{folder}}', parentFolder || '—')}>
                  <Folder size={11} aria-hidden="true" />
                  {parentFolder ? highlightMatch(parentFolder, queryKeywords) : '—'}
                </span>
              )}
              {columns.path && (
                <div className="file-path-wrapper" title={result.path}>
                  <CodeBadge className="file-path" variant="muted">{highlightMatch(result.path, queryKeywords)}</CodeBadge>
                </div>
              )}
            </div>
          </>
        )}

        {columns.snippets && result.snippets && result.snippets.length > 0 && (
          <div className="file-snippets-container">
            {result.snippets.map((snippet, snippetIndex) => (
              <div key={snippetIndex} className="file-snippet-row">
                <span className="snippet-line-num">L{snippet.lineNumber}</span>
                <span className="snippet-text">
                  {renderSnippetWithHighlight(snippet.lineContent, snippet.matchOffset, snippet.matchLength, queryKeywords)}
                </span>
              </div>
            ))}
          </div>
        )}
      </div>
    </li>
  );
}, (previous, next) => (
  previous.selected === next.selected
  && previous.index === next.index
  && previous.result === next.result
  && previous.density === next.density
  && previous.iconStyle === next.iconStyle
  && previous.iconBase64 === next.iconBase64
  && previous.columns === next.columns
  && previous.queryKeywords === next.queryKeywords
  && previous.setSize === next.setSize
  && previous.style?.top === next.style?.top
  && previous.onHover === next.onHover
  && previous.onSelect === next.onSelect
  && previous.onOpen === next.onOpen
  && previous.onContextMenu === next.onContextMenu
));

import { useState, useEffect, useMemo, useRef, useCallback, type MouseEvent as ReactMouseEvent, type WheelEvent as ReactWheelEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { 
  File, 
  Folder, 
  ExternalLink, 
  FolderOpen, 
  Copy, 
  Check,
  Pin,
  X, 
  FileText, 
  Image, 
  Film, 
  Music, 
  Code, 
  FileDigit, 
  Search,
  Minus,
  Plus,
  ArrowUp,
  ArrowDown,
} from 'lucide-react';
import { bridgeRequest, useBridgeEvent } from './hooks/useBridge';
import { useAppearance } from './hooks/useAppearance';
import { WindowResizeHandles } from './components/WindowResizeHandles';
import { countTextLines, highlightCodeLines, renderMarkdownToHtml } from './quickLookMarkup';
import './QuickLookApp.css';

interface FilePreviewData {
  exists: boolean;
  path: string;
  name: string;
  extension: string;
  type: 'markdown' | 'code' | 'image' | 'video' | 'audio' | 'pdf' | 'folder' | 'binary';
  isDirectory: boolean;
  size: number;
  formattedSize: string;
  modified: string;
  content?: string;
  encoding?: string;
  truncated?: boolean;
  dataUri?: string;
  mediaUrl?: string;
  hexDump?: string;
  folderChildren?: Array<{
    name: string;
    isDirectory: boolean;
    size: number;
    formattedSize: string;
  }>;
}

export default function QuickLookApp() {
  const { t } = useTranslation();
  useAppearance();
  const [data, setData] = useState<FilePreviewData | null>(null);
  const [codeWrap, setCodeWrap] = useState(true);
  const [fontSize, setFontSize] = useState(13);
  const [imageScale, setImageScale] = useState(1);
  const [copiedFeedback, setCopiedFeedback] = useState(false);
  const [pinnedFeedback, setPinnedFeedback] = useState(false);
  const contentRef = useRef<HTMLDivElement>(null);
  const copyTimerRef = useRef<number | null>(null);
  const pinTimerRef = useRef<number | null>(null);

  useEffect(() => {
    return () => {
      if (copyTimerRef.current !== null) window.clearTimeout(copyTimerRef.current);
      if (pinTimerRef.current !== null) window.clearTimeout(pinTimerRef.current);
    };
  }, []);

  // 监听原生文件切换事件
  useBridgeEvent('quicklook.fileChanged', (payload: unknown) => {
    setData(payload as FilePreviewData);
    setImageScale(1);
    setCopiedFeedback(false);
    setPinnedFeedback(false);
    if (contentRef.current) {
      contentRef.current.scrollTop = 0;
    }
  });

  // 组件挂载后主动通知原生层推送初始预览文件数据（防止首次懒加载 chunk 晚于 NavigationCompleted 导致首帧丢包）
  useEffect(() => {
    document.documentElement.dataset.surface = 'quicklook';
    void bridgeRequest('quicklook.ready').catch(() => undefined);
  }, []);

  const handleCopyPath = useCallback((path: string) => {
    if (!path) return;
    void bridgeRequest('quicklook.copyPath', { path });
    setCopiedFeedback(true);
    if (copyTimerRef.current !== null) {
      window.clearTimeout(copyTimerRef.current);
    }
    copyTimerRef.current = window.setTimeout(() => {
      setCopiedFeedback(false);
      copyTimerRef.current = null;
    }, 1500);
  }, []);

  const handlePinImage = useCallback((path: string) => {
    if (!path) return;
    void bridgeRequest('capture.pinImageFile', { path });
    setPinnedFeedback(true);
    if (pinTimerRef.current !== null) {
      window.clearTimeout(pinTimerRef.current);
    }
    pinTimerRef.current = window.setTimeout(() => {
      setPinnedFeedback(false);
      pinTimerRef.current = null;
    }, 1500);
  }, []);

  // 快捷键监听
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        void bridgeRequest('quicklook.hide');
        return;
      }
      const target = e.target as HTMLElement | null;
      const isInteractiveTarget = Boolean(
        typeof target?.closest === 'function' &&
          target.closest('button, a, input, select, textarea, video, audio, [role="button"]'),
      );
      if (e.key === ' ' || e.code === 'Space') {
        if (isInteractiveTarget) return;
        e.preventDefault();
        void bridgeRequest('quicklook.hide');
        return;
      }
      if ((e.key === 'ArrowUp' || e.key === 'ArrowDown') && !e.shiftKey) {
        const delta = e.key === 'ArrowDown' ? 1 : -1;
        if (e.altKey || e.ctrlKey || e.metaKey) {
          e.preventDefault();
          void bridgeRequest('quicklook.stepSelection', { delta });
          return;
        }
        if (!isInteractiveTarget && data?.exists) {
          const container = contentRef.current;
          const hasVerticalOverflow = Boolean(
            container && container.scrollHeight > container.clientHeight + 2,
          );
          const nonScrollingType =
            data.type === 'image' ||
            data.type === 'audio' ||
            data.type === 'video' ||
            data.type === 'binary';
          if (nonScrollingType || !hasVerticalOverflow) {
            e.preventDefault();
            void bridgeRequest('quicklook.stepSelection', { delta });
            return;
          }
        }
      }
      if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        if (data?.path) {
          void bridgeRequest('quicklook.showInFolder', { path: data.path });
        }
        return;
      }
      if (e.key === 'Enter') {
        if (isInteractiveTarget) return;
        e.preventDefault();
        if (data?.path) {
          void bridgeRequest('quicklook.open', { path: data.path });
        }
        return;
      }
      if ((e.ctrlKey || e.metaKey) && (e.key === 'c' || e.key === 'C')) {
        if (!window.getSelection()?.toString() && data?.path) {
          e.preventDefault();
          handleCopyPath(data.path);
          return;
        }
      }
      if (data?.type === 'image') {
        if (!e.ctrlKey && !e.metaKey && !e.altKey && (e.key === 'p' || e.key === 'P')) {
          if (!isInteractiveTarget && data.path) {
            e.preventDefault();
            handlePinImage(data.path);
            return;
          }
        }
        if (e.key === '+' || e.key === '=') {
          e.preventDefault();
          setImageScale(prev => Math.min(5, Number((prev + 0.25).toFixed(2))));
        } else if (e.key === '-' || e.key === '_') {
          e.preventDefault();
          setImageScale(prev => Math.max(0.1, Number((prev - 0.25).toFixed(2))));
        } else if (e.key === '0') {
          e.preventDefault();
          setImageScale(1);
        }
      } else if (data?.type === 'code') {
        if ((e.ctrlKey || e.metaKey) && (e.key === '+' || e.key === '=')) {
          e.preventDefault();
          setFontSize(prev => Math.min(24, prev + 1));
        } else if ((e.ctrlKey || e.metaKey) && (e.key === '-' || e.key === '_')) {
          e.preventDefault();
          setFontSize(prev => Math.max(12, prev - 1));
        } else if ((e.ctrlKey || e.metaKey) && e.key === '0') {
          e.preventDefault();
          setFontSize(13);
        } else if (e.altKey && (e.key === 'z' || e.key === 'Z')) {
          e.preventDefault();
          setCodeWrap(prev => !prev);
        }
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => {
      window.removeEventListener('keydown', handleKeyDown);
    };
  }, [data, handleCopyPath, handlePinImage]);

  const handleImageWheel = useCallback((e: ReactWheelEvent<HTMLDivElement>) => {
    e.preventDefault();
    const delta = e.deltaY < 0 ? 0.15 : -0.15;
    setImageScale(prev => Math.min(5, Math.max(0.1, Number((prev + delta).toFixed(2)))));
  }, []);

  const handleImageDoubleClick = useCallback(() => {
    setImageScale(prev => (Math.abs(prev - 1) < 0.05 ? 2 : 1));
  }, []);

  const fileIcon = useMemo(() => {
    if (!data) return <FileText size={18} />;
    if (data.isDirectory) return <Folder size={18} />;
    switch (data.type) {
      case 'markdown': return <FileText size={18} />;
      case 'image': return <Image size={18} />;
      case 'video': return <Film size={18} />;
      case 'audio': return <Music size={18} />;
      case 'code': return <Code size={18} />;
      case 'pdf': return <FileText size={18} />;
      default: return <File size={18} />;
    }
  }, [data]);

  const typeLabel = useMemo(() => {
    if (!data) return '';
    if (data.isDirectory) return t('quicklook.typeFolder', 'Folder');
    switch (data.type) {
      case 'markdown': return t('quicklook.typeMarkdown', 'Markdown Document');
      case 'image': return t('quicklook.typeImage', '{{ext}} Image', { ext: data.extension.toUpperCase() });
      case 'video': return t('quicklook.typeVideo', '{{ext}} Video', { ext: data.extension.toUpperCase() });
      case 'audio': return t('quicklook.typeAudio', '{{ext}} Audio', { ext: data.extension.toUpperCase() });
      case 'code': return t('quicklook.typeCode', '{{ext}} Source Code', { ext: data.extension.toUpperCase() });
      case 'pdf': return t('quicklook.typePdf', 'PDF Document');
      default: return t('quicklook.typeFile', 'File');
    }
  }, [data, t]);

  const linesCount = useMemo(() => {
    if (!data?.content) return 0;
    return countTextLines(data.content);
  }, [data]);

  const highlightedCodeLines = useMemo(() => {
    if (data?.type !== 'code' || !data.content) return [];
    return highlightCodeLines(data.content);
  }, [data]);

  const renderedMarkdownHtml = useMemo(() => {
    if (data?.type !== 'markdown' || !data.content) return '';
    return renderMarkdownToHtml(data.content);
  }, [data]);

  const copiedText = t('quicklook.copied', 'Copied');

  const handleMarkdownClick = useCallback((event: ReactMouseEvent<HTMLDivElement>) => {
    const button = (event.target as HTMLElement).closest<HTMLButtonElement>('.ql-code-copy-btn');
    const encodedCode = button?.dataset.quicklookCode;
    if (!button || !encodedCode) return;
    try {
      void navigator.clipboard.writeText(decodeURIComponent(encodedCode));
      if (button.dataset.copying !== '1') {
        const prevText = button.textContent || 'Copy';
        button.dataset.copying = '1';
        button.textContent = copiedText;
        window.setTimeout(() => {
          if (button.isConnected) {
            button.textContent = prevText;
            delete button.dataset.copying;
          }
        }, 1500);
      }
    } catch {
      // Malformed attributes are ignored; generated values are URI-encoded.
    }
  }, [copiedText]);

  const handleHeaderMouseDown = useCallback((event: ReactMouseEvent<HTMLElement>) => {
    if (event.button !== 0) return;
    const target = event.target as HTMLElement | null;
    if (target?.closest('button, input, a, select, textarea, .ql-actions, .ql-code-controls')) {
      return;
    }
    void bridgeRequest('quicklook.startDrag').catch(() => undefined);
  }, []);

  if (!data || !data.exists) {
    return (
      <div className="ql-container ql-empty-container" onMouseDown={handleHeaderMouseDown}>
        <div className="ql-empty-state">
          <div className="ql-empty-icon"><Search size={32} /></div>
          <div className="ql-empty-title">{t('quicklook.emptyTitle', 'Select a file in File Explorer and press Space')}</div>
          <div className="ql-empty-desc">{t('quicklook.emptyDesc', 'Instant preview for Markdown, code, images, audio, video, PDF and folders')}</div>
        </div>
        <WindowResizeHandles method="quicklook.startResize" showMaximizedCheck={false} />
      </div>
    );
  }

  const resolvedMediaSrc = data.mediaUrl || '';
  const resolvedImageSrc = data.dataUri || data.mediaUrl || '';

  return (
    <div className="ql-container">
      {/* 顶部标题栏与快捷操作 */}
      <header className="ql-header" onMouseDown={handleHeaderMouseDown}>
        <div className="ql-file-info">
          <span className="ql-file-icon">{fileIcon}</span>
          <div className="ql-file-meta">
            <div className="ql-file-name-row">
              <span className="ql-file-name" title={data.path}>{data.name}</span>
              <span className="ql-file-badge">{typeLabel}</span>
            </div>
            <div className="ql-file-sub-row">
              <span>{data.isDirectory ? t('quicklook.typeFolder', 'Folder') : data.formattedSize}</span>
              {data.encoding && <span>· {data.encoding}</span>}
              {linesCount > 0 && <span>· {t('quicklook.linesCount', '{{count}} lines', { count: linesCount })}</span>}
              <span>· {t('quicklook.modifiedAt', 'Modified at {{time}}', { time: data.modified })}</span>
            </div>
          </div>
        </div>

        <div className="ql-actions">
          {data.type === 'code' && (
            <div className="ql-code-controls">
              <button
                type="button"
                className={`ql-btn-sub ${codeWrap ? 'active' : ''}`}
                onClick={() => setCodeWrap(!codeWrap)}
                title={t('quicklook.wrapLines', 'Word Wrap')}
              >
                {t('quicklook.wrapLines', 'Word Wrap')}
              </button>
              <button
                type="button"
                className="ql-btn-sub"
                onClick={() => setFontSize(Math.max(12, fontSize - 1))}
                title={t('quicklook.decreaseFontSize', 'Decrease Font Size')}
                aria-label={t('quicklook.decreaseFontSize', 'Decrease Font Size')}
              >
                <span>A</span>
                <Minus size={11} strokeWidth={2.25} aria-hidden="true" />
              </button>
              <button
                type="button"
                className="ql-btn-sub"
                onClick={() => setFontSize(Math.min(24, fontSize + 1))}
                title={t('quicklook.increaseFontSize', 'Increase Font Size')}
                aria-label={t('quicklook.increaseFontSize', 'Increase Font Size')}
              >
                <span>A</span>
                <Plus size={11} strokeWidth={2.25} aria-hidden="true" />
              </button>
            </div>
          )}

          {data.type === 'image' && (
            <div className="ql-code-controls">
              <button
                type="button"
                className="ql-btn-sub"
                onClick={() => setImageScale(Math.max(0.1, Number((imageScale - 0.25).toFixed(2))))}
                title={t('quicklook.zoomOut', 'Zoom Out')}
                aria-label={t('quicklook.zoomOut', 'Zoom Out')}
              >
                <Minus size={13} strokeWidth={2.2} aria-hidden="true" />
              </button>
              <span className="ql-scale-text">{Math.round(imageScale * 100)}%</span>
              <button
                type="button"
                className="ql-btn-sub"
                onClick={() => setImageScale(Math.min(5, Number((imageScale + 0.25).toFixed(2))))}
                title={t('quicklook.zoomIn', 'Zoom In')}
                aria-label={t('quicklook.zoomIn', 'Zoom In')}
              >
                <Plus size={13} strokeWidth={2.2} aria-hidden="true" />
              </button>
              <button
                type="button"
                className="ql-btn-sub"
                onClick={() => setImageScale(1)}
                title={t('quicklook.resetZoom', 'Reset')}
                aria-label={t('quicklook.resetZoom', 'Reset')}
              >
                100%
              </button>
            </div>
          )}

          {data.type === 'image' && (
            <button
              type="button"
              className={`ql-btn ql-btn-secondary ${pinnedFeedback ? 'ql-btn-feedback' : ''}`}
              onClick={() => handlePinImage(data.path)}
              title={t('quicklook.pinImageTitle', 'Pin image to desktop (P)')}
            >
              {pinnedFeedback ? <Check size={13} aria-hidden="true" /> : <Pin size={13} aria-hidden="true" />}
              <span>{pinnedFeedback ? t('quicklook.pinned', 'Pinned') : t('quicklook.pinImage', 'Pin')}</span>
            </button>
          )}

          <button
            type="button"
            className="ql-btn ql-btn-primary"
            onClick={() => bridgeRequest('quicklook.open', { path: data.path })}
            title={t('quicklook.openFileTitle', 'Open with default application (Enter)')}
          >
            <ExternalLink size={13} aria-hidden="true" />
            <span>{t('quicklook.openFile', 'Open')}</span>
          </button>
          <button
            type="button"
            className="ql-btn ql-btn-secondary"
            onClick={() => bridgeRequest('quicklook.showInFolder', { path: data.path })}
            title={t('quicklook.locateTitle', 'Locate in File Explorer')}
          >
            <FolderOpen size={13} aria-hidden="true" />
            <span>{t('quicklook.locate', 'Locate')}</span>
          </button>
          <button
            type="button"
            className={`ql-btn ql-btn-secondary ${copiedFeedback ? 'ql-btn-feedback' : ''}`}
            onClick={() => handleCopyPath(data.path)}
            title={t('quicklook.copyPathTitle', 'Copy absolute file path')}
          >
            {copiedFeedback ? <Check size={13} aria-hidden="true" /> : <Copy size={13} aria-hidden="true" />}
            <span>{copiedFeedback ? copiedText : t('quicklook.copyPath', 'Copy Path')}</span>
          </button>
          <button
            type="button"
            className="ql-btn ql-btn-close"
            onClick={() => bridgeRequest('quicklook.hide')}
            title={t('quicklook.close', 'Close Preview (Esc / Space)')}
            aria-label={t('quicklook.close', 'Close Preview (Esc / Space)')}
          >
            <X size={14} aria-hidden="true" />
          </button>
        </div>
      </header>

      {/* 主体渲染区 */}
      <main className="ql-main" ref={contentRef}>
        {data.truncated && (
          <div className="ql-truncated-banner" role="status">
            <span>{t('quicklook.truncatedNotice', 'Showing first 256 KB for instant preview.')}</span>
            <button
              type="button"
              className="ql-truncated-open-btn"
              onClick={() => void bridgeRequest('quicklook.open', { path: data.path })}
            >
              <ExternalLink size={12} aria-hidden="true" />
              <span>{t('quicklook.openFullFile', 'Open Full File')}</span>
            </button>
          </div>
        )}

        {data.type === 'markdown' && (
          <div
            className="ql-markdown-view"
            onClick={handleMarkdownClick}
            dangerouslySetInnerHTML={{ __html: renderedMarkdownHtml }}
          />
        )}

        {data.type === 'code' && (
          <div className="ql-code-view" style={{ fontSize: `${fontSize}px` }}>
            <pre className={`ql-code-content ${codeWrap ? 'wrap' : ''}`}>
              <code className="ql-code-grid">
                {highlightedCodeLines.map((lineHtml, idx) => (
                  <div
                    key={idx}
                    className="ql-code-line"
                    dangerouslySetInnerHTML={{
                      __html: lineHtml || '<br />',
                    }}
                  />
                ))}
              </code>
            </pre>
          </div>
        )}

        {data.type === 'image' && (
          <div
            className="ql-image-view"
            onWheel={handleImageWheel}
            onDoubleClick={handleImageDoubleClick}
          >
            <div className="ql-image-canvas">
              {resolvedImageSrc && (
                <img
                  key={resolvedImageSrc}
                  src={resolvedImageSrc}
                  alt={data.name}
                  style={{ transform: `scale(${imageScale})` }}
                  className="ql-image-element"
                />
              )}
            </div>
          </div>
        )}

        {data.type === 'video' && (
          <div className="ql-media-view">
            {resolvedMediaSrc ? (
              <video key={resolvedMediaSrc} controls autoPlay src={resolvedMediaSrc} className="ql-video-player">
                <p>{t('quicklook.videoNotSupported', 'Your browser does not support this video format')}</p>
              </video>
            ) : (
              <div className="ql-binary-card">
                <div className="ql-binary-icon"><Film size={36} /></div>
                <div className="ql-binary-name">{data.name}</div>
              </div>
            )}
          </div>
        )}

        {data.type === 'audio' && (
          <div className="ql-media-view ql-audio-container">
            <div className="ql-audio-card">
              <div className="ql-audio-icon"><Music size={36} /></div>
              <div className="ql-audio-title">{data.name}</div>
              {resolvedMediaSrc && (
                <audio key={resolvedMediaSrc} controls autoPlay src={resolvedMediaSrc} className="ql-audio-player" />
              )}
            </div>
          </div>
        )}

        {data.type === 'pdf' && (
          <div className="ql-pdf-view">
            {data.mediaUrl ? (
              <iframe
                key={data.mediaUrl}
                className="ql-pdf-frame"
                src={data.mediaUrl}
                title={data.name}
              />
            ) : (
              <div className="ql-binary-card">
                <div className="ql-binary-icon"><FileText size={36} /></div>
                <div className="ql-binary-name">{data.name}</div>
                <p className="ql-binary-hint">
                  {t('quicklook.pdfFallbackHint', 'Use Open above to view in your default PDF reader if embedded preview is unavailable.')}
                </p>
              </div>
            )}
          </div>
        )}

        {data.type === 'folder' && (
          <div className="ql-folder-view">
            <div className="ql-folder-header">
              <span>{t('quicklook.folderPreview', 'Folder Content Preview ({{count}} items)', { count: data.folderChildren?.length || 0 })}</span>
            </div>
            <div className="ql-folder-table-container">
              <table className="ql-folder-table">
                <thead>
                  <tr>
                    <th>{t('quicklook.colName', 'Name')}</th>
                    <th>{t('quicklook.colSize', 'Size')}</th>
                    <th>{t('quicklook.colType', 'Type')}</th>
                  </tr>
                </thead>
                <tbody>
                  {data.folderChildren?.map((child, index) => (
                    <tr key={index}>
                      <td className="ql-cell-name">
                        <span className="ql-table-icon" style={{ display: 'inline-flex', alignItems: 'center', marginRight: 4 }}>
                          {child.isDirectory ? <Folder size={14} /> : <File size={14} />}
                        </span>
                        <span>{child.name}</span>
                      </td>
                      <td>{child.formattedSize}</td>
                      <td>{child.isDirectory ? t('quicklook.typeFolder', 'Folder') : t('quicklook.typeFile', 'File')}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
        )}

        {data.type === 'binary' && (
          <div className="ql-binary-view">
            <div className="ql-binary-card">
              <div className="ql-binary-icon"><FileDigit size={36} /></div>
              <div className="ql-binary-name">{data.name}</div>
              <div className="ql-binary-meta">{data.formattedSize} · {t('quicklook.typeBinary', 'Binary File')}</div>
              {data.hexDump ? (
                <pre className="ql-hex-dump">{data.hexDump}</pre>
              ) : (
                <p className="ql-binary-hint">{t('quicklook.binaryHint', 'This file is in binary format. Click the button above to open it in an external application.')}</p>
              )}
            </div>
          </div>
        )}
      </main>

      {/* 底部快捷键提示状态栏 */}
      <footer className="ql-footer">
        <div className="ql-shortcut-hint">
          <kbd>Space</kbd> <span>{t('quicklook.spaceClose', 'Close / Toggle')}</span>
          <kbd>Esc</kbd> <span>{t('quicklook.escExit', 'Exit')}</span>
          <kbd aria-label={t('quicklook.keyUp', 'Up Arrow')}><ArrowUp size={12} strokeWidth={2.2} aria-hidden="true" /></kbd>{' '}
          <kbd aria-label={t('quicklook.keyDown', 'Down Arrow')}><ArrowDown size={12} strokeWidth={2.2} aria-hidden="true" /></kbd>{' '}
          <span>{t('quicklook.arrowSwitch', 'Switch Selected File')}</span>
          <kbd>Enter</kbd> <span>{t('quicklook.enterOpen', 'Open File')}</span>
        </div>
        <div className="ql-footer-status">
          <span>{t('quicklook.engineStatus', 'Tools3000 QuickLook Instant Preview Engine')}</span>
        </div>
      </footer>
      <WindowResizeHandles method="quicklook.startResize" showMaximizedCheck={false} />
    </div>
  );
}

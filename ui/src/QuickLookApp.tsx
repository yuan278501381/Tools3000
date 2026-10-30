import { useState, useEffect, useMemo, useRef, useCallback, type MouseEvent as ReactMouseEvent, type WheelEvent as ReactWheelEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { 
  File, 
  Folder, 
  ExternalLink, 
  FolderOpen, 
  Copy, 
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
import { highlightCode, renderMarkdownToHtml } from './quickLookMarkup';
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
  const contentRef = useRef<HTMLDivElement>(null);

  // 监听原生文件切换事件
  useBridgeEvent('quicklook.fileChanged', (payload: unknown) => {
    setData(payload as FilePreviewData);
    setImageScale(1);
  });

  // 组件挂载后主动通知原生层推送初始预览文件数据（防止首次懒加载 chunk 晚于 NavigationCompleted 导致首帧丢包）
  useEffect(() => {
    document.documentElement.dataset.surface = 'quicklook';
    void bridgeRequest('quicklook.ready').catch(() => undefined);
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
        target?.closest('button, a, input, select, textarea, video, audio, [role="button"]'),
      );
      if (e.key === ' ' || e.code === 'Space') {
        if (isInteractiveTarget) return;
        e.preventDefault();
        void bridgeRequest('quicklook.hide');
        return;
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
          void bridgeRequest('quicklook.copyPath', { path: data.path });
          return;
        }
      }
      if (data?.type === 'image') {
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
  }, [data]);

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
    return data.content.split('\n').length;
  }, [data]);

  const highlightedCodeHtml = useMemo(() => {
    if (data?.type !== 'code' || !data.content) return '';
    return highlightCode(data.content);
  }, [data]);

  const renderedMarkdownHtml = useMemo(() => {
    if (data?.type !== 'markdown' || !data.content) return '';
    return renderMarkdownToHtml(data.content);
  }, [data]);

  const handleMarkdownClick = useCallback((event: ReactMouseEvent<HTMLDivElement>) => {
    const button = (event.target as HTMLElement).closest<HTMLButtonElement>('.ql-code-copy-btn');
    const encodedCode = button?.dataset.quicklookCode;
    if (!encodedCode) return;
    try {
      void navigator.clipboard.writeText(decodeURIComponent(encodedCode));
    } catch {
      // Malformed attributes are ignored; generated values are URI-encoded.
    }
  }, []);

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

          <button
            type="button"
            className="ql-btn ql-btn-primary"
            onClick={() => bridgeRequest('quicklook.open', { path: data.path })}
            title={t('quicklook.openFileTitle', 'Open with default application (Enter)')}
          >
            <ExternalLink size={13} style={{ marginRight: 4, verticalAlign: -1 }} />
            {t('quicklook.openFile', 'Open')}
          </button>
          <button
            type="button"
            className="ql-btn ql-btn-secondary"
            onClick={() => bridgeRequest('quicklook.showInFolder', { path: data.path })}
            title={t('quicklook.locateTitle', 'Locate in File Explorer')}
          >
            <FolderOpen size={13} style={{ marginRight: 4, verticalAlign: -1 }} />
            {t('quicklook.locate', 'Locate')}
          </button>
          <button
            type="button"
            className="ql-btn ql-btn-secondary"
            onClick={() => bridgeRequest('quicklook.copyPath', { path: data.path })}
            title={t('quicklook.copyPathTitle', 'Copy absolute file path')}
          >
            <Copy size={13} style={{ marginRight: 4, verticalAlign: -1 }} />
            {t('quicklook.copyPath', 'Copy Path')}
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
        {data.type === 'markdown' && (
          <div
            className="ql-markdown-view"
            onClick={handleMarkdownClick}
            dangerouslySetInnerHTML={{ __html: renderedMarkdownHtml }}
          />
        )}

        {data.type === 'code' && (
          <div className="ql-code-view" style={{ fontSize: `${fontSize}px` }}>
            <div className="ql-line-numbers">
              {(data.content || '').split('\n').map((_, idx) => (
                <div key={idx} className="ql-line-num">{idx + 1}</div>
              ))}
            </div>
            <pre className={`ql-code-content ${codeWrap ? 'wrap' : ''}`}>
              <code dangerouslySetInnerHTML={{ __html: highlightedCodeHtml }} />
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
              <img
                src={data.dataUri}
                alt={data.name}
                style={{ transform: `scale(${imageScale})` }}
                className="ql-image-element"
              />
            </div>
          </div>
        )}

        {data.type === 'video' && (
          <div className="ql-media-view">
            <video controls autoPlay src={`https://tools3000.local/` + encodeURIComponent(data.name)} className="ql-video-player">
              <p>{t('quicklook.videoNotSupported', 'Your browser does not support this video format')}</p>
            </video>
          </div>
        )}

        {data.type === 'audio' && (
          <div className="ql-media-view ql-audio-container">
            <div className="ql-audio-card">
              <div className="ql-audio-icon"><Music size={36} /></div>
              <div className="ql-audio-title">{data.name}</div>
              <audio controls autoPlay src={`https://tools3000.local/` + encodeURIComponent(data.name)} className="ql-audio-player" />
            </div>
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

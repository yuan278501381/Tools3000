import { useState, useEffect, useMemo, useRef, useCallback, type MouseEvent as ReactMouseEvent, type PointerEvent as ReactPointerEvent, type WheelEvent as ReactWheelEvent, type UIEvent as ReactUIEvent } from 'react';
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
  FileSpreadsheet,
  Table2,
  Lock,
  Loader2,
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
import { countTextLines, highlightCodeLines, highlightSingleCodeLine, renderMarkdownToHtml, splitNormalizedLines } from './quickLookMarkup';
import './QuickLookApp.css';

const VIRTUAL_SCROLL_LINE_THRESHOLD = 120;
const VIRTUAL_OVERSCAN_LINES = 30;
const LAZY_LINE_TOKENIZE_THRESHOLD = 2000;
const DEFAULT_CODE_VIEWPORT_HEIGHT = 640;
const MAX_CSV_PREVIEW_ROWS = 250;
const MAX_CSV_PREVIEW_COLS = 32;

interface SpreadsheetSheetData {
  name: string;
  rows: string[][];
  truncatedRows?: boolean;
  truncatedCols?: boolean;
}

interface FilePreviewData {
  exists: boolean;
  loading?: boolean;
  path: string;
  name: string;
  extension: string;
  type:
    | 'markdown'
    | 'code'
    | 'csv'
    | 'spreadsheet'
    | 'document'
    | 'office_legacy'
    | 'image'
    | 'video'
    | 'audio'
    | 'pdf'
    | 'folder'
    | 'binary';
  isDirectory: boolean;
  size: number;
  formattedSize: string;
  modified: string;
  content?: string;
  encoding?: string;
  truncated?: boolean;
  csvDelimiter?: string;
  sheets?: SpreadsheetSheetData[];
  totalSheetCount?: number;
  encrypted?: boolean;
  officeFallback?: boolean;
  pdfPages?: string[];
  pdfPageCount?: number;
  pdfPasswordProtected?: boolean;
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

interface ParsedCsvTable {
  rows: string[][];
  truncatedRows: boolean;
  truncatedCols: boolean;
}

function detectCsvDelimiter(sample: string, hint?: string): string {
  if (hint === '\t') return '\t';
  const firstLineEnd = sample.indexOf('\n');
  const firstLine = firstLineEnd >= 0 ? sample.slice(0, firstLineEnd) : sample;
  let inQuotes = false;
  let commas = 0;
  let semicolons = 0;
  let tabs = 0;
  for (let i = 0; i < firstLine.length; i += 1) {
    const ch = firstLine[i];
    if (ch === '"') {
      inQuotes = !inQuotes;
    } else if (!inQuotes) {
      if (ch === ',') commas += 1;
      else if (ch === ';') semicolons += 1;
      else if (ch === '\t') tabs += 1;
    }
  }
  if (tabs > commas && tabs >= semicolons) return '\t';
  if (semicolons > commas) return ';';
  return hint || ',';
}

function parseDelimitedTable(rawText: string, hintDelimiter?: string): ParsedCsvTable {
  const cleaned = rawText.replace(/\n\n\/\/ \.\.\. \[Preview truncated at 256 KB[^\n]*$/u, '');
  if (!cleaned.trim()) {
    return { rows: [], truncatedRows: false, truncatedCols: false };
  }
  const delim = detectCsvDelimiter(cleaned, hintDelimiter);
  const rows: string[][] = [];
  let currentRow: string[] = [];
  let currentCell = '';
  let inQuotes = false;
  let truncatedRows = false;
  let truncatedCols = false;
  let maxObservedCols = 0;

  const pushCell = () => {
    if (currentRow.length < MAX_CSV_PREVIEW_COLS) {
      currentRow.push(currentCell);
    } else {
      truncatedCols = true;
    }
    currentCell = '';
  };

  const commitRow = () => {
    pushCell();
    while (currentRow.length > 0 && currentRow[currentRow.length - 1] === '') {
      currentRow.pop();
    }
    if (currentRow.length > 0) {
      if (rows.length >= MAX_CSV_PREVIEW_ROWS) {
        truncatedRows = true;
        currentRow = [];
        return false;
      }
      maxObservedCols = Math.max(maxObservedCols, currentRow.length);
      rows.push(currentRow);
    }
    currentRow = [];
    return true;
  };

  for (let i = 0; i < cleaned.length; i += 1) {
    const ch = cleaned[i];
    if (inQuotes) {
      if (ch === '"') {
        if (i + 1 < cleaned.length && cleaned[i + 1] === '"') {
          currentCell += '"';
          i += 1;
        } else {
          inQuotes = false;
        }
      } else {
        currentCell += ch;
      }
    } else if (ch === '"') {
      inQuotes = true;
    } else if (ch === delim) {
      pushCell();
    } else if (ch === '\r') {
      if (i + 1 < cleaned.length && cleaned[i + 1] === '\n') {
        i += 1;
      }
      if (!commitRow()) break;
    } else if (ch === '\n') {
      if (!commitRow()) break;
    } else {
      currentCell += ch;
    }
  }

  if (!truncatedRows && (currentCell.length > 0 || currentRow.length > 0)) {
    commitRow();
  }

  if (maxObservedCols > 0) {
    for (const r of rows) {
      while (r.length < maxObservedCols) {
        r.push('');
      }
    }
  }
  return { rows, truncatedRows, truncatedCols };
}

export default function QuickLookApp() {
  const { t } = useTranslation();
  useAppearance();
  const [data, setData] = useState<FilePreviewData | null>(null);
  const [codeWrap, setCodeWrap] = useState(true);
  const [fontSize, setFontSize] = useState(13);
  const [imageScale, setImageScale] = useState(1);
  const [pdfScale, setPdfScale] = useState(1);
  const [activeSheetIndex, setActiveSheetIndex] = useState(0);
  const [showHexDump, setShowHexDump] = useState(false);
  const [imagePan, setImagePan] = useState<{ x: number; y: number }>({ x: 0, y: 0 });
  const [isDraggingImage, setIsDraggingImage] = useState(false);
  const [copiedFeedback, setCopiedFeedback] = useState(false);
  const [pinnedFeedback, setPinnedFeedback] = useState(false);
  const [codeScrollTop, setCodeScrollTop] = useState(0);
  const [codeViewportHeight, setCodeViewportHeight] = useState(DEFAULT_CODE_VIEWPORT_HEIGHT);
  const [codeViewportWidth, setCodeViewportWidth] = useState(860);
  const contentRef = useRef<HTMLDivElement>(null);
  const codeScrollRef = useRef<HTMLPreElement>(null);
  const videoRef = useRef<HTMLVideoElement>(null);
  const audioRef = useRef<HTMLAudioElement>(null);
  const iframeRef = useRef<HTMLIFrameElement>(null);
  const copyTimerRef = useRef<number | null>(null);
  const pinTimerRef = useRef<number | null>(null);
  const imageDragRef = useRef<{
    pointerId: number;
    startX: number;
    startY: number;
    originX: number;
    originY: number;
  } | null>(null);

  // 立即停止音视频后台播放并清空媒体/PDF URL，促使 Chromium 立即释放底层磁盘文件锁
  const releaseMediaHandles = useCallback(() => {
    const detachMedia = (el: HTMLMediaElement | null) => {
      if (!el) return;
      try {
        el.pause();
      } catch {
        // Ignore unimplemented pause in synthetic test environments
      }
      el.removeAttribute('src');
      try {
        el.load();
      } catch {
        // Ignore unimplemented load in synthetic test environments
      }
    };
    detachMedia(videoRef.current);
    detachMedia(audioRef.current);
    if (iframeRef.current) {
      iframeRef.current.src = 'about:blank';
    }
  }, []);

  const hidePreviewAndRelease = useCallback(() => {
    releaseMediaHandles();
    setData(null);
    void bridgeRequest('quicklook.hide');
  }, [releaseMediaHandles]);

  useEffect(() => {
    const handleVisibilityChange = () => {
      if (document.hidden) {
        releaseMediaHandles();
        setData(null);
      }
    };
    document.addEventListener('visibilitychange', handleVisibilityChange);
    return () => {
      document.removeEventListener('visibilitychange', handleVisibilityChange);
      if (copyTimerRef.current !== null) window.clearTimeout(copyTimerRef.current);
      if (pinTimerRef.current !== null) window.clearTimeout(pinTimerRef.current);
      releaseMediaHandles();
    };
  }, [releaseMediaHandles]);

  // 监听原生文件切换事件（收到 null 时彻底释放媒体句柄并清空预览状态）
  useBridgeEvent('quicklook.fileChanged', (payload: unknown) => {
    releaseMediaHandles();
    imageDragRef.current = null;
    setIsDraggingImage(false);
    setImagePan({ x: 0, y: 0 });
    if (!payload) {
      setData(null);
      setCodeScrollTop(0);
      return;
    }
    setData(payload as FilePreviewData);
    setImageScale(1);
    setPdfScale(1);
    setActiveSheetIndex(0);
    setShowHexDump(false);
    setCopiedFeedback(false);
    setPinnedFeedback(false);
    setCodeScrollTop(0);
    if (contentRef.current) {
      contentRef.current.scrollTop = 0;
    }
    if (codeScrollRef.current) {
      codeScrollRef.current.scrollTop = 0;
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

  const hasRenderedPdfPages = Boolean(data?.type === 'pdf' && data.pdfPages && data.pdfPages.length > 0);

  // 快捷键监听
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        hidePreviewAndRelease();
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
        hidePreviewAndRelease();
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
            data.type === 'office_legacy' ||
            data.type === 'binary' ||
            Boolean(data.officeFallback);
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
          setImageScale(prev => {
            const next = Math.max(0.1, Number((prev - 0.25).toFixed(2)));
            if (next <= 1) setImagePan({ x: 0, y: 0 });
            return next;
          });
        } else if (e.key === '0') {
          e.preventDefault();
          setImageScale(1);
          setImagePan({ x: 0, y: 0 });
        }
      } else if (hasRenderedPdfPages) {
        if (e.key === '+' || e.key === '=') {
          e.preventDefault();
          setPdfScale(prev => Math.min(2.5, Number((prev + 0.2).toFixed(2))));
        } else if (e.key === '-' || e.key === '_') {
          e.preventDefault();
          setPdfScale(prev => Math.max(0.5, Number((prev - 0.2).toFixed(2))));
        } else if (e.key === '0') {
          e.preventDefault();
          setPdfScale(1);
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
  }, [data, hasRenderedPdfPages, handleCopyPath, handlePinImage, hidePreviewAndRelease]);

  const handleImageWheel = useCallback((e: ReactWheelEvent<HTMLDivElement>) => {
    e.preventDefault();
    const delta = e.deltaY < 0 ? 0.15 : -0.15;
    setImageScale(prev => {
      const next = Math.min(5, Math.max(0.1, Number((prev + delta).toFixed(2))));
      if (next <= 1) setImagePan({ x: 0, y: 0 });
      return next;
    });
  }, []);

  const handleImageDoubleClick = useCallback(() => {
    setImageScale(prev => {
      const next = Math.abs(prev - 1) < 0.05 ? 2 : 1;
      if (next <= 1) setImagePan({ x: 0, y: 0 });
      return next;
    });
  }, []);

  const handleImagePointerDown = useCallback((e: ReactPointerEvent<HTMLDivElement>) => {
    if (e.button !== 0 || imageScale <= 1) return;
    e.preventDefault();
    try {
      e.currentTarget.setPointerCapture?.(e.pointerId);
    } catch {
      // Synthetic pointer events in tests may not register active pointerId
    }
    imageDragRef.current = {
      pointerId: e.pointerId,
      startX: e.clientX,
      startY: e.clientY,
      originX: imagePan.x,
      originY: imagePan.y,
    };
    setIsDraggingImage(true);
  }, [imagePan.x, imagePan.y, imageScale]);

  const handleImagePointerMove = useCallback((e: ReactPointerEvent<HTMLDivElement>) => {
    const drag = imageDragRef.current;
    if (!drag || drag.pointerId !== e.pointerId) return;
    e.preventDefault();
    const dx = e.clientX - drag.startX;
    const dy = e.clientY - drag.startY;
    setImagePan({
      x: Math.round(drag.originX + dx),
      y: Math.round(drag.originY + dy),
    });
  }, []);

  const handleImagePointerEnd = useCallback((e: ReactPointerEvent<HTMLDivElement>) => {
    const drag = imageDragRef.current;
    if (!drag || drag.pointerId !== e.pointerId) return;
    try {
      e.currentTarget.releasePointerCapture?.(e.pointerId);
    } catch {
      // ignore
    }
    imageDragRef.current = null;
    setIsDraggingImage(false);
  }, []);

  const fileIcon = useMemo(() => {
    if (!data) return <FileText size={18} />;
    if (data.isDirectory) return <Folder size={18} />;
    switch (data.type) {
      case 'markdown':
      case 'document':
      case 'office_legacy':
      case 'pdf':
        return <FileText size={18} />;
      case 'csv':
        return <Table2 size={18} />;
      case 'spreadsheet':
        return <FileSpreadsheet size={18} />;
      case 'image':
        return <Image size={18} />;
      case 'video':
        return <Film size={18} />;
      case 'audio':
        return <Music size={18} />;
      case 'code':
        return <Code size={18} />;
      default:
        return <File size={18} />;
    }
  }, [data]);

  const typeLabel = useMemo(() => {
    if (!data) return '';
    if (data.isDirectory) return t('quicklook.typeFolder', 'Folder');
    const upperExt = (data.extension || '').toUpperCase();
    switch (data.type) {
      case 'markdown':
        return t('quicklook.typeMarkdown', 'Markdown Document');
      case 'csv':
        return t('quicklook.typeCsv', '{{ext}} Table', { ext: upperExt || 'CSV' });
      case 'spreadsheet':
        return t('quicklook.typeSpreadsheet', 'Excel Workbook');
      case 'document':
      case 'office_legacy':
        return t('quicklook.typeOfficeDoc', '{{ext}} Document', { ext: upperExt || 'DOC' });
      case 'image':
        return t('quicklook.typeImage', '{{ext}} Image', { ext: upperExt });
      case 'video':
        return t('quicklook.typeVideo', '{{ext}} Video', { ext: upperExt });
      case 'audio':
        return t('quicklook.typeAudio', '{{ext}} Audio', { ext: upperExt });
      case 'code':
        return t('quicklook.typeCode', '{{ext}} Source Code', { ext: upperExt });
      case 'pdf':
        return t('quicklook.typePdf', 'PDF Document');
      default:
        return t('quicklook.typeFile', 'File');
    }
  }, [data, t]);

  const parsedCsvTable = useMemo<ParsedCsvTable | null>(() => {
    if (data?.type !== 'csv' || !data.content) return null;
    return parseDelimitedTable(data.content, data.csvDelimiter);
  }, [data]);

  const activeSpreadsheetSheet = useMemo<SpreadsheetSheetData | null>(() => {
    if (data?.type !== 'spreadsheet' || !data.sheets || data.sheets.length === 0) return null;
    const clampedIdx = Math.min(activeSheetIndex, data.sheets.length - 1);
    return data.sheets[clampedIdx] ?? null;
  }, [data, activeSheetIndex]);

  const linesCount = useMemo(() => {
    if (data?.type === 'csv' && parsedCsvTable) {
      return parsedCsvTable.rows.length;
    }
    if (data?.type === 'spreadsheet' && activeSpreadsheetSheet) {
      return activeSpreadsheetSheet.rows.length;
    }
    if (!data?.content || data.type === 'document') return 0;
    return countTextLines(data.content);
  }, [data, parsedCsvTable, activeSpreadsheetSheet]);

  const rawCodeLines = useMemo(() => {
    if (data?.type !== 'code' || !data.content) return [];
    return splitNormalizedLines(data.content);
  }, [data]);

  const eagerHighlightedCodeLines = useMemo(() => {
    if (data?.type !== 'code' || !data.content) return [];
    if (rawCodeLines.length > LAZY_LINE_TOKENIZE_THRESHOLD) return [];
    return highlightCodeLines(data.content);
  }, [data, rawCodeLines.length]);

  const codeLineHeightPx = useMemo(() => Math.max(18, Math.round(fontSize * 1.6)), [fontSize]);
  const isCodeVirtualized = rawCodeLines.length > VIRTUAL_SCROLL_LINE_THRESHOLD;

  useEffect(() => {
    if (data?.type !== 'code') return;
    const measureViewport = () => {
      const h = codeScrollRef.current?.clientHeight || contentRef.current?.clientHeight || 0;
      const w = codeScrollRef.current?.clientWidth || contentRef.current?.clientWidth || 0;
      if (h > 0) {
        setCodeViewportHeight(h);
      }
      if (w > 0) {
        setCodeViewportWidth(w);
      }
    };
    measureViewport();
    window.addEventListener('resize', measureViewport);
    return () => {
      window.removeEventListener('resize', measureViewport);
    };
  }, [data?.type, data?.path]);

  const handleCodeScroll = useCallback((e: ReactUIEvent<HTMLPreElement>) => {
    const target = e.currentTarget;
    setCodeScrollTop(target.scrollTop);
    if (target.clientHeight > 0 && target.clientHeight !== codeViewportHeight) {
      setCodeViewportHeight(target.clientHeight);
    }
    if (target.clientWidth > 0 && target.clientWidth !== codeViewportWidth) {
      setCodeViewportWidth(target.clientWidth);
    }
  }, [codeViewportHeight, codeViewportWidth]);

  const wrappedLineOffsets = useMemo(() => {
    if (!isCodeVirtualized || !codeWrap || rawCodeLines.length === 0) {
      return null;
    }
    const gutterPx = rawCodeLines.length >= 100000 ? 68 : rawCodeLines.length >= 10000 ? 58 : 48;
    const usableWidthPx = Math.max(180, codeViewportWidth - gutterPx - 36);
    const avgCharWidthPx = Math.max(6.5, fontSize * 0.6);
    const charsPerRow = Math.max(20, Math.floor(usableWidthPx / avgCharWidthPx));
    const offsets = new Int32Array(rawCodeLines.length + 1);
    let acc = 0;
    for (let i = 0; i < rawCodeLines.length; i += 1) {
      const len = rawCodeLines[i]?.length ?? 0;
      const rows = Math.max(1, Math.ceil(Math.max(1, len) / charsPerRow));
      acc += rows * codeLineHeightPx;
      offsets[i + 1] = acc;
    }
    return offsets;
  }, [isCodeVirtualized, codeWrap, rawCodeLines, codeViewportWidth, fontSize, codeLineHeightPx]);

  const virtualCodeSlice = useMemo(() => {
    const total = rawCodeLines.length;
    if (total === 0) {
      return { startIndex: 0, endIndex: 0, topSpacerPx: 0, bottomSpacerPx: 0, items: [] as Array<{ lineNumber: number; html: string }> };
    }
    let startIndex = 0;
    let endIndex = total;
    let topSpacerPx = 0;
    let bottomSpacerPx = 0;

    if (isCodeVirtualized) {
      if (wrappedLineOffsets) {
        const findLineAtOffset = (targetPx: number) => {
          let lo = 0;
          let hi = total;
          while (lo < hi) {
            const mid = (lo + hi) >>> 1;
            if (wrappedLineOffsets[mid + 1] <= targetPx) {
              lo = mid + 1;
            } else {
              hi = mid;
            }
          }
          return Math.min(total - 1, lo);
        };
        const firstVisible = findLineAtOffset(Math.max(0, codeScrollTop));
        const lastVisible = findLineAtOffset(Math.max(0, codeScrollTop + codeViewportHeight));
        startIndex = Math.max(0, firstVisible - VIRTUAL_OVERSCAN_LINES);
        endIndex = Math.min(total, lastVisible + 1 + VIRTUAL_OVERSCAN_LINES);
        topSpacerPx = wrappedLineOffsets[startIndex];
        bottomSpacerPx = Math.max(0, wrappedLineOffsets[total] - wrappedLineOffsets[endIndex]);
      } else {
        startIndex = Math.max(0, Math.floor(codeScrollTop / codeLineHeightPx) - VIRTUAL_OVERSCAN_LINES);
        endIndex = Math.min(total, Math.ceil((codeScrollTop + codeViewportHeight) / codeLineHeightPx) + VIRTUAL_OVERSCAN_LINES);
        topSpacerPx = startIndex * codeLineHeightPx;
        bottomSpacerPx = Math.max(0, (total - endIndex) * codeLineHeightPx);
      }
    }

    const items: Array<{ lineNumber: number; html: string }> = [];
    for (let idx = startIndex; idx < endIndex; idx += 1) {
      const html = eagerHighlightedCodeLines.length > 0
        ? (eagerHighlightedCodeLines[idx] ?? '')
        : highlightSingleCodeLine(rawCodeLines[idx] ?? '');
      items.push({ lineNumber: idx + 1, html });
    }
    return { startIndex, endIndex, topSpacerPx, bottomSpacerPx, items };
  }, [rawCodeLines, eagerHighlightedCodeLines, isCodeVirtualized, wrappedLineOffsets, codeScrollTop, codeViewportHeight, codeLineHeightPx]);

  const copyText = t('common.copy', 'Copy');
  const copiedText = t('quicklook.copied', 'Copied');

  const renderedMarkdownHtml = useMemo(() => {
    if ((data?.type !== 'markdown' && data?.type !== 'document') || !data.content) return '';
    return renderMarkdownToHtml(data.content, copyText);
  }, [data, copyText]);

  const handleMarkdownClick = useCallback((event: ReactMouseEvent<HTMLDivElement>) => {
    const button = (event.target as HTMLElement).closest<HTMLButtonElement>('.ql-code-copy-btn');
    const encodedCode = button?.dataset.quicklookCode;
    if (!button || !encodedCode) return;
    try {
      void navigator.clipboard.writeText(decodeURIComponent(encodedCode));
      if (button.dataset.copying !== '1') {
        const prevText = button.textContent || copyText;
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
  }, [copiedText, copyText]);

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
          <div className="ql-empty-desc">{t('quicklook.emptyDesc', 'Quick preview for Markdown, code, images, CSV/Excel tables, Word/PPT, PDF, audio, video and folders')}</div>
        </div>
        <WindowResizeHandles method="quicklook.startResize" showMaximizedCheck={false} />
      </div>
    );
  }

  const resolvedMediaSrc = data.mediaUrl || '';
  const resolvedImageSrc = data.dataUri || data.mediaUrl || '';
  const isAsyncLoadingSkeleton = Boolean(
    data.loading &&
      !data.content &&
      (!data.sheets || data.sheets.length === 0) &&
      (!data.pdfPages || data.pdfPages.length === 0) &&
      !data.mediaUrl &&
      (!data.folderChildren || data.folderChildren.length === 0),
  );

  const tableRows =
    data.type === 'csv'
      ? (parsedCsvTable?.rows ?? [])
      : data.type === 'spreadsheet'
        ? (activeSpreadsheetSheet?.rows ?? [])
        : [];
  const showTableNoticeBanner = Boolean(
    (data.type === 'csv' && (data.truncated || parsedCsvTable?.truncatedRows || parsedCsvTable?.truncatedCols)) ||
      (data.type === 'spreadsheet' &&
        (data.truncated || activeSpreadsheetSheet?.truncatedRows || activeSpreadsheetSheet?.truncatedCols)),
  );
  const showGenericTruncatedBanner = Boolean(
    data.truncated && data.type !== 'csv' && data.type !== 'spreadsheet',
  );
  const showFallbackCard = Boolean(
    !isAsyncLoadingSkeleton &&
      (data.officeFallback || data.type === 'office_legacy' || data.type === 'binary'),
  );

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
              {data.type === 'pdf' && typeof data.pdfPageCount === 'number' && data.pdfPageCount > 0 && (
                <span>· {t('quicklook.pdfPagesSummary', '{{count}} pages', { count: data.pdfPageCount })}</span>
              )}
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
                onClick={() => {
                  const next = Math.max(0.1, Number((imageScale - 0.25).toFixed(2)));
                  setImageScale(next);
                  if (next <= 1) setImagePan({ x: 0, y: 0 });
                }}
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
                onClick={() => {
                  setImageScale(1);
                  setImagePan({ x: 0, y: 0 });
                }}
                title={t('quicklook.resetZoom', 'Reset')}
                aria-label={t('quicklook.resetZoom', 'Reset')}
              >
                100%
              </button>
            </div>
          )}

          {hasRenderedPdfPages && (
            <div className="ql-code-controls">
              <button
                type="button"
                className="ql-btn-sub"
                onClick={() => setPdfScale(prev => Math.max(0.5, Number((prev - 0.2).toFixed(2))))}
                title={t('quicklook.zoomOut', 'Zoom Out')}
                aria-label={t('quicklook.zoomOut', 'Zoom Out')}
              >
                <Minus size={13} strokeWidth={2.2} aria-hidden="true" />
              </button>
              <span className="ql-scale-text">{Math.round(pdfScale * 100)}%</span>
              <button
                type="button"
                className="ql-btn-sub"
                onClick={() => setPdfScale(prev => Math.min(2.5, Number((prev + 0.2).toFixed(2))))}
                title={t('quicklook.zoomIn', 'Zoom In')}
                aria-label={t('quicklook.zoomIn', 'Zoom In')}
              >
                <Plus size={13} strokeWidth={2.2} aria-hidden="true" />
              </button>
              <button
                type="button"
                className="ql-btn-sub"
                onClick={() => setPdfScale(1)}
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
            onClick={hidePreviewAndRelease}
            title={t('quicklook.close', 'Close Preview (Esc / Space)')}
            aria-label={t('quicklook.close', 'Close Preview (Esc / Space)')}
          >
            <X size={14} aria-hidden="true" />
          </button>
        </div>
      </header>

      {/* 主体渲染区 */}
      <main className="ql-main" ref={contentRef}>
        {isAsyncLoadingSkeleton && (
          <div className="ql-loading-state" role="status">
            <Loader2 size={24} className="ql-loading-spin" aria-hidden="true" />
            <span>{t('quicklook.loadingPreview', 'Preparing instant preview...')}</span>
          </div>
        )}

        {showGenericTruncatedBanner && (
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

        {showTableNoticeBanner && (
          <div className="ql-truncated-banner" role="status">
            <span>{t('quicklook.tableTruncatedNotice', 'Showing top rows and columns for instant preview.')}</span>
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

        {!isAsyncLoadingSkeleton &&
          (data.type === 'markdown' || (data.type === 'document' && Boolean(data.content))) && (
            <div
              className="ql-markdown-view"
              onClick={handleMarkdownClick}
              dangerouslySetInnerHTML={{ __html: renderedMarkdownHtml }}
            />
          )}

        {!isAsyncLoadingSkeleton &&
          (data.type === 'csv' || (data.type === 'spreadsheet' && !data.officeFallback)) && (
            <div className="ql-sheet-view">
              {data.type === 'spreadsheet' && data.sheets && data.sheets.length > 1 && (
                <div className="ql-sheet-tabs" role="tablist">
                  {data.sheets.map((sheet, idx) => (
                    <button
                      key={`${sheet.name}-${idx}`}
                      type="button"
                      role="tab"
                      aria-selected={idx === activeSheetIndex}
                      className={`ql-sheet-tab ${idx === activeSheetIndex ? 'active' : ''}`}
                      onClick={() => setActiveSheetIndex(idx)}
                    >
                      <FileSpreadsheet size={13} aria-hidden="true" />
                      <span>{sheet.name}</span>
                    </button>
                  ))}
                </div>
              )}

              {tableRows.length > 0 ? (
                <div className="ql-data-table-scroll">
                  <table className="ql-data-table">
                    <thead>
                      <tr>
                        <th className="ql-data-row-num">#</th>
                        {tableRows[0].map((cell, cIdx) => (
                          <th key={cIdx}>{cell || `Col ${cIdx + 1}`}</th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {tableRows.slice(1).map((row, rIdx) => (
                        <tr key={rIdx}>
                          <td className="ql-data-row-num">{rIdx + 2}</td>
                          {row.map((cell, cIdx) => (
                            <td key={cIdx}>{cell}</td>
                          ))}
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              ) : (
                <div className="ql-binary-card">
                  <div className="ql-binary-icon"><Table2 size={36} /></div>
                  <div className="ql-binary-name">{data.name}</div>
                  <p className="ql-binary-hint">{t('quicklook.emptySheetHint', 'This sheet has no visible cell data.')}</p>
                </div>
              )}
            </div>
          )}

        {!isAsyncLoadingSkeleton && data.type === 'code' && (
          <div
            className="ql-code-view"
            style={{
              fontSize: `${fontSize}px`,
              ['--ql-code-line-height' as string]: `${codeLineHeightPx}px`,
              ['--ql-gutter-width' as string]: rawCodeLines.length >= 100000 ? '68px' : rawCodeLines.length >= 10000 ? '58px' : '48px',
            }}
          >
            <pre
              ref={codeScrollRef}
              onScroll={handleCodeScroll}
              className={`ql-code-content ${codeWrap ? 'wrap' : ''} ${isCodeVirtualized ? 'virtualized' : ''}`}
            >
              <code
                className="ql-code-grid"
                style={isCodeVirtualized ? {
                  paddingTop: `${virtualCodeSlice.topSpacerPx}px`,
                  paddingBottom: `${virtualCodeSlice.bottomSpacerPx}px`,
                } : undefined}
              >
                {virtualCodeSlice.items.map((item) => (
                  <div
                    key={item.lineNumber}
                    className="ql-code-line"
                    data-line={item.lineNumber}
                    dangerouslySetInnerHTML={{
                      __html: item.html || '<br />',
                    }}
                  />
                ))}
              </code>
            </pre>
          </div>
        )}

        {data.type === 'image' && (
          <div
            className={`ql-image-view ${imageScale > 1 ? 'ql-image-view--zoomed' : ''} ${isDraggingImage ? 'ql-image-view--dragging' : ''}`}
            onWheel={handleImageWheel}
            onDoubleClick={handleImageDoubleClick}
            onPointerDown={handleImagePointerDown}
            onPointerMove={handleImagePointerMove}
            onPointerUp={handleImagePointerEnd}
            onPointerCancel={handleImagePointerEnd}
          >
            <div className="ql-image-canvas">
              {resolvedImageSrc && (
                <img
                  key={resolvedImageSrc}
                  src={resolvedImageSrc}
                  alt={data.name}
                  draggable={false}
                  style={{ transform: `translate3d(${imagePan.x}px, ${imagePan.y}px, 0) scale(${imageScale})` }}
                  className={`ql-image-element ${isDraggingImage ? 'ql-image-element--dragging' : ''}`}
                />
              )}
            </div>
          </div>
        )}

        {data.type === 'video' && (
          <div className="ql-media-view">
            {resolvedMediaSrc ? (
              <video ref={videoRef} key={resolvedMediaSrc} controls autoPlay src={resolvedMediaSrc} className="ql-video-player">
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
                <audio ref={audioRef} key={resolvedMediaSrc} controls autoPlay src={resolvedMediaSrc} className="ql-audio-player" />
              )}
            </div>
          </div>
        )}

        {!isAsyncLoadingSkeleton && data.type === 'pdf' && (
          <div className="ql-pdf-view">
            {data.pdfPasswordProtected ? (
              <div className="ql-binary-view">
                <div className="ql-binary-card">
                  <div className="ql-binary-icon"><Lock size={36} /></div>
                  <div className="ql-binary-name">{data.name}</div>
                  <p className="ql-binary-hint">
                    {t('quicklook.pdfPasswordHint', 'This PDF is password-protected. Press Enter to open it in your default PDF reader.')}
                  </p>
                  <div className="ql-binary-actions">
                    <button
                      type="button"
                      className="ql-btn ql-btn-primary"
                      onClick={() => void bridgeRequest('quicklook.open', { path: data.path })}
                    >
                      <ExternalLink size={14} aria-hidden="true" />
                      <span>{t('quicklook.openInDefaultApp', 'Open with Default App (Enter)')}</span>
                    </button>
                  </div>
                </div>
              </div>
            ) : data.pdfPages && data.pdfPages.length > 0 ? (
              <div className="ql-pdf-pages-scroll">
                {typeof data.pdfPageCount === 'number' && data.pdfPageCount > data.pdfPages.length && (
                  <div className="ql-truncated-banner" role="status">
                    <span>
                      {t('quicklook.pdfPartialNotice', 'Showing first {{shown}} of {{total}} pages for instant preview.', {
                        shown: data.pdfPages.length,
                        total: data.pdfPageCount,
                      })}
                    </span>
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
                <div className="ql-pdf-pages-list">
                  {data.pdfPages.map((pageSrc, idx) => (
                    <figure
                      key={idx}
                      className="ql-pdf-page-card"
                      style={{ width: `${Math.round(820 * pdfScale)}px`, maxWidth: pdfScale <= 1 ? '100%' : 'none' }}
                    >
                      <img
                        src={pageSrc}
                        alt={`${data.name} - ${idx + 1}`}
                        className="ql-pdf-page-img"
                        draggable={false}
                      />
                      <figcaption className="ql-pdf-page-caption">
                        {t('quicklook.pdfPageIndicator', 'Page {{current}} / {{total}}', {
                          current: idx + 1,
                          total: data.pdfPageCount || data.pdfPages?.length || 1,
                        })}
                      </figcaption>
                    </figure>
                  ))}
                </div>
              </div>
            ) : data.mediaUrl ? (
              <iframe
                ref={iframeRef}
                key={data.mediaUrl}
                className="ql-pdf-frame"
                src={data.mediaUrl}
                title={data.name}
              />
            ) : (
              <div className="ql-binary-view">
                <div className="ql-binary-card">
                  <div className="ql-binary-icon"><FileText size={36} /></div>
                  <div className="ql-binary-name">{data.name}</div>
                  <p className="ql-binary-hint">
                    {t('quicklook.pdfFallbackHint', 'Use Open above to view in your default PDF reader if embedded preview is unavailable.')}
                  </p>
                </div>
              </div>
            )}
          </div>
        )}

        {!isAsyncLoadingSkeleton && data.type === 'folder' && (
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

        {showFallbackCard && (
          <div className="ql-binary-view">
            <div className="ql-binary-card">
              <div className="ql-binary-icon">
                {data.encrypted ? <Lock size={36} /> : data.type === 'binary' ? <FileDigit size={36} /> : <FileText size={36} />}
              </div>
              <div className="ql-binary-name">{data.name}</div>
              <div className="ql-binary-meta">
                {data.formattedSize} · {data.type === 'binary' ? t('quicklook.typeBinary', 'Binary File') : typeLabel}
              </div>
              <p className="ql-binary-hint">
                {data.encrypted
                  ? t('quicklook.encryptedDocHint', 'This document is encrypted or password-protected. Press Enter to open it in its native application.')
                  : data.officeFallback || data.type === 'office_legacy'
                    ? t('quicklook.officeFallbackHint', 'Direct preview is unavailable for this document format. Press Enter to open it in your default office app.')
                    : t('quicklook.binaryHint', 'This file is in binary format. Click the button below or press Enter to open it in its default application.')}
              </p>
              <div className="ql-binary-actions">
                <button
                  type="button"
                  className="ql-btn ql-btn-primary"
                  onClick={() => void bridgeRequest('quicklook.open', { path: data.path })}
                >
                  <ExternalLink size={14} aria-hidden="true" />
                  <span>{t('quicklook.openInDefaultApp', 'Open with Default App (Enter)')}</span>
                </button>
                {data.hexDump && (
                  <button
                    type="button"
                    className="ql-btn ql-btn-secondary"
                    onClick={() => setShowHexDump(prev => !prev)}
                  >
                    <FileDigit size={13} aria-hidden="true" />
                    <span>{t('quicklook.toggleHexHeader', 'Inspect File Header (Hex)')}</span>
                  </button>
                )}
              </div>
              {data.hexDump && showHexDump && (
                <pre className="ql-hex-dump">{data.hexDump}</pre>
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

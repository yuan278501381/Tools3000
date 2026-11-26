import { useCallback, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { bridgeRequest } from '../hooks/useBridge';
import {
  DEFAULT_VISIBLE_TABLE_COLUMNS,
  doesItemRequireBackendProperty,
  getColumnDisplayLabel,
  isBackendExtractableColumn,
  resolveTableCellText,
} from './columnCatalog';
import { IMAGE_EXTENSIONS } from './searchConstants';
import type { PropertyCacheMap, SearchResult, TableColumnId } from './searchTypes';
import {
  extractParentDirectoryPath,
  formatFileSize,
  formatWindowsTime,
} from './searchUtils';

interface NativeActionResponse {
  success: boolean;
  error?: string;
}

async function requestNativeAction(method: string, params: Record<string, unknown>): Promise<void> {
  const response = await bridgeRequest<NativeActionResponse>(method, params);
  if (response?.success !== true) {
    throw new Error(response?.error || `${method} failed`);
  }
}

function copyTextWithTextarea(text: string): boolean {
  const textarea = document.createElement('textarea');
  textarea.value = text;
  textarea.style.position = 'fixed';
  textarea.style.opacity = '0';
  document.body.appendChild(textarea);
  try {
    textarea.select();
    return document.execCommand('copy');
  } catch {
    return false;
  } finally {
    document.body.removeChild(textarea);
  }
}

async function requestNativeActionWithFallback(
  primaryMethod: string,
  fallbackMethod: string,
  path: string,
): Promise<void> {
  const params = { path, filepath: path };
  try {
    await requestNativeAction(primaryMethod, params);
  } catch {
    await requestNativeAction(fallbackMethod, params);
  }
}

export interface UseSearchActionsProps {
  isPinned: boolean;
  hideOnOpen?: boolean;
  hide: () => void;
  setActionError: (err: string) => void;
  setContextMenu: React.Dispatch<React.SetStateAction<{ visible: boolean; x: number; y: number; result?: SearchResult }>>;
  renameTarget: { visible: boolean; result?: SearchResult; newName: string };
  setRenameTarget: React.Dispatch<React.SetStateAction<{ visible: boolean; result?: SearchResult; newName: string }>>;
  renameInputRef: React.RefObject<HTMLInputElement | null>;
  searchInputRef?: React.RefObject<HTMLInputElement | null>;
  setResults: React.Dispatch<React.SetStateAction<SearchResult[]>>;
  query: string;
  sortedResults: SearchResult[];
  visibleTableColumns?: readonly TableColumnId[];
  propertyCache?: PropertyCacheMap;
  getLatestPropertyCache?: () => PropertyCacheMap;
  ensureColumnLoadedForSort?: (
    colId: TableColumnId,
    colLabel: string,
    items: SearchResult[],
  ) => Promise<boolean>;
  onRecordSearch?: (searchText: string) => void;
}

export function useSearchActions({
  isPinned,
  hideOnOpen = false,
  hide,
  setActionError,
  setContextMenu,
  renameTarget,
  setRenameTarget,
  renameInputRef,
  searchInputRef,
  setResults,
  query,
  sortedResults,
  visibleTableColumns,
  propertyCache,
  getLatestPropertyCache,
  ensureColumnLoadedForSort,
  onRecordSearch,
}: UseSearchActionsProps) {
  const { t } = useTranslation();
  const exportingCsvRef = useRef(false);

  const maybeHideAfterOpen = useCallback(() => {
    if (hideOnOpen && !isPinned) {
      hide();
    }
  }, [hide, hideOnOpen, isPinned]);

  const bumpLocalRunCount = useCallback((targetPath: string) => {
    if (!targetPath) return;
    setResults((prev) =>
      prev.map((item) =>
        item.path === targetPath
          ? { ...item, runCount: (item.runCount ?? 0) + 1 }
          : item,
      ),
    );
  }, [setResults]);

  const openResult = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    try {
      onRecordSearch?.(query);
      void bridgeRequest('search.recordRun', { path: result.path });
      await requestNativeActionWithFallback('search.openFile', 'system.openFile', result.path);
      bumpLocalRunCount(result.path);
      maybeHideAfterOpen();
    } catch {
      setActionError(t('search.openFailed', 'Could not open this result'));
    }
  }, [bumpLocalRunCount, maybeHideAfterOpen, onRecordSearch, query, setActionError, t]);

  const openFolderResult = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    try {
      onRecordSearch?.(query);
      void bridgeRequest('search.recordRun', { path: result.path });
      await requestNativeActionWithFallback('search.openFolder', 'system.openFolder', result.path);
      bumpLocalRunCount(result.path);
      maybeHideAfterOpen();
    } catch {
      setActionError(t('search.openFolderFailed', 'Could not open containing folder'));
    }
  }, [bumpLocalRunCount, maybeHideAfterOpen, onRecordSearch, query, setActionError, t]);

  const copyPathResult = useCallback((result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    const doNativeCopy = async () => {
      try {
        await requestNativeAction('system.copyText', { text: result.path });
        toast.success(t('search.copiedPath', 'File path copied to clipboard'));
      } catch {
        if (copyTextWithTextarea(result.path)) {
          toast.success(t('search.copiedPath', 'File path copied to clipboard'));
        } else {
          setActionError(t('search.copyFailed', 'Failed to copy to clipboard'));
        }
      }
    };

    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(result.path).then(() => {
        toast.success(t('search.copiedPath', 'File path copied to clipboard'));
      }).catch(() => {
        void doNativeCopy();
      });
    } else {
      void doNativeCopy();
    }
  }, [setActionError, t]);

  const copyFileResult = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    // 书签 / 系统命令等非磁盘实体项自动降级为复制链接或命令文本
    if (result.resultType === 'bookmark' || result.resultType === 'command') {
      copyPathResult(result);
      return;
    }
    try {
      await requestNativeActionWithFallback('search.copyFile', 'system.copyFile', result.path);
      toast.success(t('search.copiedFile', 'File copied to clipboard'));
    } catch {
      copyPathResult(result);
    }
  }, [copyPathResult, setActionError, t]);

  const quickLookResult = useCallback(async (result: SearchResult | undefined, options?: { toggle?: boolean }) => {
    if (!result) return;
    setActionError('');
    try {
      const payload = options?.toggle === false
        ? { path: result.path, toggle: false }
        : { path: result.path };
      await bridgeRequest('search.quickLook', payload);
    } catch {
      // ignore
    }
  }, [setActionError]);

  const pinResult = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    if (result.isDirectory || !IMAGE_EXTENSIONS.test(result.name)) {
      toast.error(t('search.toastImagePinOnly', 'Only image files support desktop pinning'));
      return;
    }
    try {
      onRecordSearch?.(query);
      await bridgeRequest('capture.pinImageFile', { path: result.path });
    } catch {
      toast.error(t('search.toastPinFail', 'Pin failed'));
    }
  }, [onRecordSearch, query, t]);

  const openResultAsAdmin = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    try {
      onRecordSearch?.(query);
      void bridgeRequest('search.recordRun', { path: result.path });
      await requestNativeActionWithFallback('search.openFileAsAdmin', 'system.openFileAsAdmin', result.path);
      maybeHideAfterOpen();
    } catch {
      setActionError(t('search.errRunAsAdmin', 'Failed to run as administrator'));
    }
  }, [maybeHideAfterOpen, onRecordSearch, query, setActionError, t]);

  const showFileProperties = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    try {
      await requestNativeActionWithFallback('search.showFileProperties', 'system.showFileProperties', result.path);
    } catch {
      setActionError(t('search.errFileProperties', 'Unable to open file properties'));
    }
  }, [setActionError, t]);

  const copyText = useCallback((text: string) => {
    if (!text) return;
    const doNative = async () => {
      try {
        await requestNativeAction('system.copyText', { text });
        toast.success(t('search.toastCopied', 'Copied to clipboard'));
      } catch {
        if (copyTextWithTextarea(text)) {
          toast.success(t('search.toastCopied', 'Copied to clipboard'));
        } else {
          toast.error(t('search.toastCopyFail', 'Failed to copy to clipboard'));
        }
      }
    };

    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(() => {
        toast.success(t('search.toastCopied', 'Copied to clipboard'));
      }).catch(() => {
        void doNative();
      });
    } else {
      void doNative();
    }
  }, [t]);

  const openWithNotepad = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    try {
      onRecordSearch?.(query);
      void bridgeRequest('search.recordRun', { path: result.path });
      await requestNativeActionWithFallback('search.openWithNotepad', 'system.openWithNotepad', result.path);
      maybeHideAfterOpen();
    } catch {
      setActionError(t('search.errOpenNotepad', 'Unable to open file with Notepad'));
    }
  }, [maybeHideAfterOpen, onRecordSearch, query, setActionError, t]);

  const startRename = useCallback((result: SearchResult | undefined) => {
    if (!result) return;
    setContextMenu({ visible: false, x: 0, y: 0 });
    setRenameTarget({
      visible: true,
      result,
      newName: result.name
    });
    setTimeout(() => {
      if (renameInputRef.current) {
        renameInputRef.current.focus();
        const dotIdx = result.isDirectory ? -1 : result.name.lastIndexOf('.');
        if (dotIdx > 0) {
          renameInputRef.current.setSelectionRange(0, dotIdx);
        } else {
          renameInputRef.current.select();
        }
      }
    }, 50);
  }, [renameInputRef, setContextMenu, setRenameTarget]);

  const confirmRename = useCallback(async () => {
    if (!renameTarget.result || !renameTarget.newName.trim()) return;
    const oldName = renameTarget.result.name;
    const newName = renameTarget.newName.trim();
    if (newName === oldName) {
      setRenameTarget({ visible: false, newName: '' });
      window.setTimeout(() => searchInputRef?.current?.focus(), 0);
      return;
    }
    if (/[\\/:*?"<>|]/.test(newName)) {
      toast.error(t('search.toastRenameInvalidChar', 'Filename cannot contain \\ / : * ? " < > |'));
      return;
    }
    try {
      const res = await bridgeRequest<{ success: boolean; newPath?: string; error?: string }>('search.renamePath', {
        oldPath: renameTarget.result.path,
        path: renameTarget.result.path,
        newName
      });
      if (res?.success && res.newPath) {
        const newP = res.newPath;
        setResults(prev => prev.map(item => item.path === renameTarget.result?.path ? { ...item, name: newName, path: newP } : item));
        toast.success(t('search.toastRenameSuccess', { name: newName, defaultValue: `Renamed to "${newName}"` }));
        setRenameTarget({ visible: false, newName: '' });
        window.setTimeout(() => searchInputRef?.current?.focus(), 0);
      } else {
        toast.error(res?.error || t('search.toastRenameFail', 'Rename failed, please check if the file is in use'));
      }
    } catch {
      try {
        const res = await bridgeRequest<{ success: boolean; newPath?: string; error?: string }>('system.renamePath', {
          oldPath: renameTarget.result.path,
          path: renameTarget.result.path,
          newName
        });
        if (res?.success && res.newPath) {
          const newP = res.newPath;
          setResults(prev => prev.map(item => item.path === renameTarget.result?.path ? { ...item, name: newName, path: newP } : item));
          toast.success(t('search.toastRenameSuccess', { name: newName, defaultValue: `Renamed to "${newName}"` }));
          setRenameTarget({ visible: false, newName: '' });
          window.setTimeout(() => searchInputRef?.current?.focus(), 0);
        } else {
          toast.error(res?.error || t('search.toastRenameFail', 'Rename failed, please check if the file is in use'));
        }
      } catch {
        toast.error(t('search.toastRenameFail', 'Rename failed, please check if the file is in use'));
      }
    }
  }, [renameTarget, searchInputRef, setRenameTarget, setResults, t]);

  const exportResultsToCsv = useCallback(async () => {
    if (exportingCsvRef.current) return;
    if (sortedResults.length === 0) {
      toast.error(t('search.toastNoExport', 'No search results to export'));
      return;
    }

    exportingCsvRef.current = true;
    const exportCols: TableColumnId[] =
      visibleTableColumns && visibleTableColumns.length > 0
        ? Array.from(new Set(visibleTableColumns))
        : [...DEFAULT_VISIBLE_TABLE_COLUMNS];
    const translateLabel = (key: string, defaultVal?: string) =>
      t(key as never, defaultVal as never) as unknown as string;
    const formatters = { formatFileSize, formatWindowsTime, extractParentDirectoryPath };
    const escapeCsv = (str: string) => `"${String(str).replace(/"/g, '""')}"`;

    let usedLoadingToast = false;
    try {
      let currentCache = getLatestPropertyCache ? getLatestPropertyCache() : (propertyCache ?? {});

      if (ensureColumnLoadedForSort) {
        const columnsNeedingFetch = exportCols.filter((colId) => {
          if (!isBackendExtractableColumn(colId)) return false;
          return sortedResults.some(
            (item) =>
              item.path &&
              doesItemRequireBackendProperty(item, colId) &&
              !Object.prototype.hasOwnProperty.call(currentCache[item.path] ?? {}, colId),
          );
        });

        if (columnsNeedingFetch.length > 0) {
          usedLoadingToast = true;
          toast.loading(
            t(
              'search.exportLoadingPropsToast' as never,
              'Loading extended column properties before exporting CSV...' as never,
            ) as unknown as string,
            { id: 'search-csv-export' },
          );
          for (const colId of columnsNeedingFetch) {
            const colLabel = getColumnDisplayLabel(colId, translateLabel);
            const ok = await ensureColumnLoadedForSort(colId, colLabel, sortedResults);
            if (!ok) {
              toast.info(
                t(
                  'search.exportCancelledToast' as never,
                  'CSV export cancelled' as never,
                ) as unknown as string,
                { id: 'search-csv-export' },
              );
              return;
            }
          }
          currentCache = getLatestPropertyCache ? getLatestPropertyCache() : (propertyCache ?? {});
        }
      }

      const headers = exportCols.map((colId) =>
        escapeCsv(getColumnDisplayLabel(colId, translateLabel)),
      );
      const rows = sortedResults.map((item) => {
        const propRow = currentCache[item.path];
        return exportCols
          .map((colId) => {
            const text = resolveTableCellText(colId, item, propRow, formatters);
            return escapeCsv(text || '');
          })
          .join(',');
      });

      const csvContent = '\uFEFF' + [headers.join(','), ...rows].join('\r\n');
      const blob = new Blob([csvContent], { type: 'text/csv;charset=utf-8;' });
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.setAttribute('href', url);
      const nowStr = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
      const queryTag = query.trim() ? `_${query.trim().slice(0, 20).replace(/[/\\:*?"<>|]/g, '_')}` : '';
      link.setAttribute('download', `Tools3000_Search_Export${queryTag}_${nowStr}.csv`);
      document.body.appendChild(link);
      link.click();
      document.body.removeChild(link);
      URL.revokeObjectURL(url);

      toast.success(
        t('search.toastExportSuccess', {
          count: sortedResults.length,
          defaultValue: `Successfully exported ${sortedResults.length} search results to CSV file`,
        }),
        usedLoadingToast ? { id: 'search-csv-export' } : undefined,
      );
    } finally {
      exportingCsvRef.current = false;
    }
  }, [
    ensureColumnLoadedForSort,
    getLatestPropertyCache,
    propertyCache,
    query,
    sortedResults,
    t,
    visibleTableColumns,
  ]);

  return {
    openResult,
    openFolderResult,
    copyPathResult,
    copyFileResult,
    quickLookResult,
    pinResult,
    openResultAsAdmin,
    showFileProperties,
    copyText,
    openWithNotepad,
    startRename,
    confirmRename,
    exportResultsToCsv,
  };
}

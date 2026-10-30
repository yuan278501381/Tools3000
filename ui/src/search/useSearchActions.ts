import { useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { bridgeRequest } from '../hooks/useBridge';
import { IMAGE_EXTENSIONS } from './searchConstants';
import type { SearchResult } from './searchTypes';
import { formatFileSize, formatWindowsTime } from './searchUtils';

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
  hide: () => void;
  setActionError: (err: string) => void;
  setContextMenu: React.Dispatch<React.SetStateAction<{ visible: boolean; x: number; y: number; result?: SearchResult }>>;
  renameTarget: { visible: boolean; result?: SearchResult; newName: string };
  setRenameTarget: React.Dispatch<React.SetStateAction<{ visible: boolean; result?: SearchResult; newName: string }>>;
  renameInputRef: React.RefObject<HTMLInputElement | null>;
  setResults: React.Dispatch<React.SetStateAction<SearchResult[]>>;
  query: string;
  sortedResults: SearchResult[];
}

export function useSearchActions({
  isPinned,
  hide,
  setActionError,
  setContextMenu,
  renameTarget,
  setRenameTarget,
  renameInputRef,
  setResults,
  query,
  sortedResults,
}: UseSearchActionsProps) {
  const { t } = useTranslation();

  const openResult = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    try {
      void bridgeRequest('search.recordRun', { path: result.path });
      await requestNativeActionWithFallback('search.openFile', 'system.openFile', result.path);
      if (!isPinned) hide();
    } catch {
      setActionError(t('search.openFailed', 'Could not open this result'));
    }
  }, [hide, isPinned, setActionError, t]);

  const openFolderResult = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    try {
      void bridgeRequest('search.recordRun', { path: result.path });
      await requestNativeActionWithFallback('search.openFolder', 'system.openFolder', result.path);
      if (!isPinned) hide();
    } catch {
      setActionError(t('search.openFolderFailed', 'Could not open containing folder'));
    }
  }, [hide, isPinned, setActionError, t]);

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
    try {
      await requestNativeActionWithFallback('search.copyFile', 'system.copyFile', result.path);
      toast.success(t('search.copiedFile', 'File copied to clipboard (ready to paste)'));
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
      await bridgeRequest('capture.pinImageFile', { path: result.path });
      if (!isPinned) hide();
    } catch {
      toast.error(t('search.toastPinFail', 'Pin failed'));
    }
  }, [hide, isPinned, t]);

  const openResultAsAdmin = useCallback(async (result: SearchResult | undefined) => {
    if (!result) return;
    setActionError('');
    try {
      void bridgeRequest('search.recordRun', { path: result.path });
      await requestNativeActionWithFallback('search.openFileAsAdmin', 'system.openFileAsAdmin', result.path);
      if (!isPinned) hide();
    } catch {
      setActionError(t('search.errRunAsAdmin', 'Failed to run as administrator'));
    }
  }, [hide, isPinned, setActionError, t]);

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
      void bridgeRequest('search.recordRun', { path: result.path });
      await requestNativeActionWithFallback('search.openWithNotepad', 'system.openWithNotepad', result.path);
      if (!isPinned) hide();
    } catch {
      setActionError(t('search.errOpenNotepad', 'Unable to open file with Notepad'));
    }
  }, [hide, isPinned, setActionError, t]);

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
        } else {
          toast.error(res?.error || t('search.toastRenameFail', 'Rename failed, please check if the file is in use'));
        }
      } catch {
        toast.error(t('search.toastRenameFail', 'Rename failed, please check if the file is in use'));
      }
    }
  }, [renameTarget, setRenameTarget, setResults, t]);

  const exportResultsToCsv = useCallback(() => {
    if (sortedResults.length === 0) {
      toast.error(t('search.toastNoExport', 'No search results to export'));
      return;
    }

    const headers = [t('search.csvName', 'File Name'), t('search.csvPath', 'Full Path'), t('search.csvType', 'Type'), t('search.csvSizeBytes', 'Size (Bytes)'), t('search.csvSizeHuman', 'Size (Readable)'), t('search.csvModified', 'Modified Time'), t('search.csvCreated', 'Created Time')];
    const rows = sortedResults.map((item) => {
      const isDir = item.isDirectory;
      const typeStr = isDir ? t('search.folderType', 'Folder') : (item.name.includes('.') ? item.name.split('.').pop()?.toUpperCase() || t('search.fileType', 'File') : t('search.fileType', 'File'));
      const sizeBytes = item.size ?? 0;
      const sizeFormatted = isDir ? '-' : formatFileSize(sizeBytes, isDir);
      const modTime = item.lastWriteTime ? formatWindowsTime(item.lastWriteTime) : '-';
      const createTime = item.creationTime ? formatWindowsTime(item.creationTime) : '-';

      const escapeCsv = (str: string) => `"${str.replace(/"/g, '""')}"`;

      return [
        escapeCsv(item.name),
        escapeCsv(item.path),
        escapeCsv(typeStr),
        sizeBytes,
        escapeCsv(sizeFormatted),
        escapeCsv(modTime),
        escapeCsv(createTime)
      ].join(',');
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

    toast.success(t('search.toastExportSuccess', { count: sortedResults.length, defaultValue: `Successfully exported ${sortedResults.length} search results to CSV file` }));
  }, [sortedResults, query, t]);

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

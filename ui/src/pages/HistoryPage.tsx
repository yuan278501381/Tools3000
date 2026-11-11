import { useState, useEffect, useCallback, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { Trash2, Pin, Image as ImageIcon, RefreshCw, FolderOpen, Copy, Download } from 'lucide-react';
import { Button, CodeBadge } from '../components/UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import './HistoryPage.css';

interface HistoryEntry {
  id?: number;
  index: number;
  timestamp: string;
  width: number;
  height: number;
  filePath: string;
  base64?: string; // Loaded on demand
}

const AUTO_REFRESH_THROTTLE_MS = 1500;

const computeEntryMatchKey = (entry: HistoryEntry): string =>
  typeof entry.id === 'number' && entry.id > 0
    ? `id:${entry.id}`
    : `meta:${entry.timestamp}|${entry.width}x${entry.height}|${entry.filePath}`;

const computeHistorySignature = (list: HistoryEntry[]): string =>
  list
    .map((entry) => `${computeEntryMatchKey(entry)}@${entry.index}`)
    .join(';;');

export default function HistoryPage() {
  const { t: rawT } = useTranslation();
  const t = rawT as unknown as (key: string, defaultValue?: string) => string;
  const [entries, setEntries] = useState<HistoryEntry[]>([]);
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [confirmingClear, setConfirmingClear] = useState(false);
  const [busyActionKey, setBusyActionKey] = useState<string | null>(null);
  const lastAutoRefreshAtRef = useRef(0);
  const entriesRef = useRef<HistoryEntry[]>([]);
  const lastSignatureRef = useRef<string>('');

  const commitEntries = useCallback((next: HistoryEntry[]) => {
    entriesRef.current = next;
    lastSignatureRef.current = computeHistorySignature(next);
    setEntries(next);
  }, []);

  const silentRefresh = useCallback(async () => {
    const now = Date.now();
    if (now - lastAutoRefreshAtRef.current < AUTO_REFRESH_THROTTLE_MS) {
      return;
    }
    lastAutoRefreshAtRef.current = now;
    try {
      const metaRaw = await bridgeRequest<HistoryEntry[]>('history.getAll', { includeThumbnails: false });
      const metaList = Array.isArray(metaRaw) ? metaRaw : [];
      const nextSig = computeHistorySignature(metaList);
      if (nextSig === lastSignatureRef.current) {
        return;
      }

      const cachedBase64Map = new Map<string, string>();
      for (const existing of entriesRef.current) {
        if (existing.base64) {
          cachedBase64Map.set(computeEntryMatchKey(existing), existing.base64);
        }
      }

      const hydrated = await Promise.all(
        metaList.map(async (item) => {
          if (item.base64) {
            return item;
          }
          const cached = cachedBase64Map.get(computeEntryMatchKey(item));
          if (cached) {
            return { ...item, base64: cached };
          }
          try {
            const payload = typeof item.id === 'number' && item.id > 0
              ? { id: item.id, index: item.index }
              : { index: item.index };
            const thumbRes = await bridgeRequest<{ success?: boolean; base64?: string }>(
              'history.getThumbnail',
              payload,
              { silent: true },
            );
            if (thumbRes?.base64) {
              return { ...item, base64: thumbRes.base64 };
            }
          } catch {
            // 单图拉取容错降级占位图
          }
          return item;
        }),
      );
      commitEntries(hydrated);
    } catch {
      // 窗口获焦自动同步静默容错，避免干扰用户操作
    }
  }, [commitEntries]);

  useEffect(() => {
    lastAutoRefreshAtRef.current = Date.now();
    bridgeRequest<HistoryEntry[]>('history.getAll', { includeThumbnails: true })
      .then((data) => {
        commitEntries(Array.isArray(data) ? data : []);
      })
      .catch((error) => {
        console.error('Failed to fetch history', error);
        toast.error(t('history.loadFailed'));
      })
      .finally(() => {
        setLoading(false);
      });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const onFocus = () => {
      void silentRefresh();
    };
    const onVisibilityChange = () => {
      if (document.visibilityState === 'visible') {
        void silentRefresh();
      }
    };
    window.addEventListener('focus', onFocus);
    document.addEventListener('visibilitychange', onVisibilityChange);
    return () => {
      window.removeEventListener('focus', onFocus);
      document.removeEventListener('visibilitychange', onVisibilityChange);
    };
  }, [silentRefresh]);

  useEffect(() => {
    if (!confirmingClear) return;
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        setConfirmingClear(false);
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [confirmingClear]);

  const handleRefresh = async () => {
    setRefreshing(true);
    try {
      const data = await bridgeRequest<HistoryEntry[]>('history.getAll', { includeThumbnails: true });
      lastAutoRefreshAtRef.current = Date.now();
      commitEntries(Array.isArray(data) ? data : []);
    } catch (error) {
      console.error('Failed to fetch history', error);
      toast.error(t('history.loadFailed'));
    } finally {
      setRefreshing(false);
    }
  };

  const confirmClearAll = async () => {
    try {
      const result = await bridgeRequest<{ success: boolean; error?: string }>('history.clear', {});
      if (!result.success) throw new Error(result.error || t('history.clearFailed'));
      commitEntries([]);
      setConfirmingClear(false);
    } catch (error) {
      toast.error(t('history.clearFailed'), { description: String(error) });
    }
  };

  const getEntryToken = (entry: HistoryEntry) =>
    typeof entry.id === 'number' && entry.id > 0 ? `id-${entry.id}` : `idx-${entry.index}`;

  const removeHistoryEntry = async (entry: HistoryEntry) => {
    const actionKey = `remove-${getEntryToken(entry)}`;
    if (busyActionKey === actionKey) return;
    setBusyActionKey(actionKey);
    try {
      const hasStableId = typeof entry.id === 'number' && entry.id > 0;
      const payload = hasStableId ? { id: entry.id, index: entry.index } : { index: entry.index };
      const result = await bridgeRequest<{ success: boolean; error?: string }>('history.remove', payload);
      if (!result?.success) {
        throw new Error(result?.error || t('history.deleteFailed', 'Failed to delete capture'));
      }
      const nextEntries = entriesRef.current
        .filter((item) => (hasStableId ? item.id !== entry.id : item.index !== entry.index))
        .map((item, idx) => ({ ...item, index: idx }));
      commitEntries(nextEntries);
      toast.success(t('history.deleteSuccess', 'Capture removed from history'));
    } catch (error) {
      toast.error(t('history.deleteFailed', 'Failed to delete capture'), { description: String(error) });
    } finally {
      setBusyActionKey(null);
    }
  };

  const openEntry = async (entry: HistoryEntry) => {
    try {
      const hasStableId = typeof entry.id === 'number' && entry.id > 0;
      const payload = hasStableId ? { id: entry.id, index: entry.index } : { index: entry.index };
      const result = await bridgeRequest<{ success: boolean; error?: string }>('history.open', payload);
      if (!result.success) throw new Error(result.error || t('history.openFailed'));
    } catch (error) {
      toast.error(t('history.openFailed'), { description: String(error) });
    }
  };

  const copyHistoryEntry = async (entry: HistoryEntry) => {
    const actionKey = `copy-${getEntryToken(entry)}`;
    if (busyActionKey === actionKey) return;
    setBusyActionKey(actionKey);
    try {
      const hasStableId = typeof entry.id === 'number' && entry.id > 0;
      const payload = hasStableId
        ? { id: entry.id, index: entry.index, path: entry.filePath || '' }
        : { index: entry.index, path: entry.filePath || '' };
      const result = await bridgeRequest<{ success: boolean; error?: string }>('capture.copyHistoryItem', payload);
      if (!result?.success) {
        throw new Error(result?.error || t('history.copyFailed', 'Could not copy image'));
      }
      toast.success(t('history.copySuccess', 'Image copied to clipboard'));
    } catch (error) {
      toast.error(t('history.copyFailed', 'Could not copy image'), { description: String(error) });
    } finally {
      setBusyActionKey(null);
    }
  };

  const saveHistoryEntryAs = async (entry: HistoryEntry) => {
    const actionKey = `save-${getEntryToken(entry)}`;
    if (busyActionKey === actionKey) return;
    setBusyActionKey(actionKey);
    try {
      const hasStableId = typeof entry.id === 'number' && entry.id > 0;
      const payload = hasStableId
        ? { id: entry.id, index: entry.index, path: entry.filePath || '' }
        : { index: entry.index, path: entry.filePath || '' };
      const result = await bridgeRequest<{
        success: boolean;
        saved?: boolean;
        cancelled?: boolean;
        filePath?: string;
        error?: string;
      }>('capture.saveHistoryItemAs', payload);
      if (result?.cancelled) {
        return;
      }
      if (!result?.success || !result?.saved) {
        throw new Error(result?.error || t('common.saveFailed'));
      }
      toast.success(t('history.saveSuccess', 'Image saved'));
    } catch (error) {
      toast.error(t('common.saveFailed'), { description: String(error) });
    } finally {
      setBusyActionKey(null);
    }
  };

  const locateEntryFile = async (filePath: string) => {
    if (!filePath) return;
    try {
      await bridgeRequest('system.openFolder', { path: filePath });
    } catch (error) {
      toast.error(t('history.openFailed'), { description: String(error) });
    }
  };

  const getFileName = (filePath: string) => {
    if (!filePath) return '';
    const parts = filePath.split(/[\\/]/);
    return parts[parts.length - 1] || filePath;
  };

  return (
    <div className="page-container history-page">
      <div className="history-toolbar">
        <Button
          variant="ghost"
          size="sm"
          onClick={() => void handleRefresh()}
          disabled={loading || refreshing}
          title={t('common.refresh')}
        >
          <RefreshCw size={15} className={refreshing ? 'history-spin' : ''} aria-hidden="true" />
          <span>{t('common.refresh')}</span>
        </Button>

        {confirmingClear && entries.length > 0 ? (
          <div className="history-clear-confirm" role="group" aria-label={t('history.confirmClear', 'Are you sure you want to clear all history?')}>
            <Button
              variant="danger"
              size="sm"
              onClick={() => void confirmClearAll()}
            >
              <Trash2 size={14} aria-hidden="true" />
              <span>{t('history.confirmClearBtn', 'Confirm Clear')}</span>
            </Button>
            <Button
              variant="ghost"
              size="sm"
              onClick={() => setConfirmingClear(false)}
            >
              <span>{t('history.cancelClearBtn', 'Cancel')}</span>
            </Button>
          </div>
        ) : (
          <Button
            variant="danger"
            size="sm"
            onClick={() => setConfirmingClear(true)}
            disabled={entries.length === 0 || loading}
          >
            <Trash2 size={15} aria-hidden="true" />
            <span>{t('history.clearAll', 'Clear All')}</span>
          </Button>
        )}
      </div>

      <div className="history-grid">
        {loading ? (
          <div className="empty-state">{t('common.loading', 'Loading...')}</div>
        ) : entries.length === 0 ? (
          <div className="empty-state">
            <ImageIcon size={48} opacity={0.5} aria-hidden="true" />
            <p>{t('history.empty', 'No capture history found.')}</p>
          </div>
        ) : (
          entries.map(entry => {
            const entryToken = getEntryToken(entry);
            return (
            <div
              key={typeof entry.id === 'number' && entry.id > 0 ? `id-${entry.id}` : `${entry.index}-${entry.timestamp}`}
              role="button"
              tabIndex={0}
              className="history-card"
              title={t('history.pinImage')}
              onClick={() => void openEntry(entry)}
              onKeyDown={(e) => {
                if (e.key === 'Enter' || e.key === ' ') {
                  e.preventDefault();
                  void openEntry(entry);
                }
              }}
            >
              <div className="history-card-image">
                {entry.base64 ? (
                  <img src={`data:image/png;base64,${entry.base64}`} alt={t('history.thumbnailAlt')} loading="lazy" decoding="async" />
                ) : (
                  <div className="placeholder"><ImageIcon size={24} aria-hidden="true" /></div>
                )}
                <div className="history-card-overlay">
                  <span className="history-card-overlay-pill">
                    <Pin size={15} aria-hidden="true" />
                    <span>{t('history.pinImage')}</span>
                  </span>
                </div>
              </div>
              <div className="history-card-info">
                <div className="history-card-meta-row">
                  <span className="timestamp">{entry.timestamp.replace('T', ' ')}</span>
                  <span className="dimensions">{entry.width} × {entry.height}</span>
                </div>
                <div
                  className="history-card-file-row"
                  onClick={(e) => e.stopPropagation()}
                  onKeyDown={(e) => e.stopPropagation()}
                >
                  {entry.filePath ? (
                    <CodeBadge title={entry.filePath}>{getFileName(entry.filePath)}</CodeBadge>
                  ) : (
                    <CodeBadge>{t('history.inMemory', 'In Memory')}</CodeBadge>
                  )}
                  <div className="history-card-actions">
                    <Button
                      variant="ghost"
                      size="sm"
                      className="history-locate-btn"
                      disabled={busyActionKey === `copy-${entryToken}`}
                      title={t('history.copyImage', 'Copy Image')}
                      aria-label={t('history.copyImage', 'Copy Image')}
                      onClick={() => void copyHistoryEntry(entry)}
                    >
                      <Copy size={14} aria-hidden="true" />
                    </Button>
                    <Button
                      variant="ghost"
                      size="sm"
                      className="history-locate-btn"
                      disabled={busyActionKey === `save-${entryToken}`}
                      title={t('history.saveAs', 'Save Image As...')}
                      aria-label={t('history.saveAs', 'Save Image As...')}
                      onClick={() => void saveHistoryEntryAs(entry)}
                    >
                      <Download size={14} aria-hidden="true" />
                    </Button>
                    {entry.filePath && (
                      <Button
                        variant="ghost"
                        size="sm"
                        className="history-locate-btn"
                        title={t('history.openFolder')}
                        aria-label={t('history.openFolder')}
                        onClick={() => void locateEntryFile(entry.filePath)}
                      >
                        <FolderOpen size={14} aria-hidden="true" />
                      </Button>
                    )}
                    <Button
                      variant="ghost"
                      size="sm"
                      className="history-locate-btn history-delete-btn"
                      disabled={busyActionKey === `remove-${entryToken}`}
                      title={t('history.deleteItem', 'Delete Capture')}
                      aria-label={t('history.deleteItem', 'Delete Capture')}
                      onClick={() => void removeHistoryEntry(entry)}
                    >
                      <Trash2 size={14} aria-hidden="true" />
                    </Button>
                  </div>
                </div>
              </div>
            </div>
            );
          })
        )}
      </div>
    </div>
  );
}

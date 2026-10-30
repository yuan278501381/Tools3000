import { useState, useEffect, useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import { Trash2, ExternalLink, Image as ImageIcon, RefreshCw, FolderOpen, Copy, Download } from 'lucide-react';
import { Button, CodeBadge } from '../components/UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import './HistoryPage.css';

interface HistoryEntry {
  index: number;
  timestamp: string;
  width: number;
  height: number;
  filePath: string;
  base64?: string; // Loaded on demand
}

export default function HistoryPage() {
  const { t: rawT } = useTranslation();
  const t = rawT as unknown as (key: string, defaultValue?: string) => string;
  const [entries, setEntries] = useState<HistoryEntry[]>([]);
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [busyActionKey, setBusyActionKey] = useState<string | null>(null);

  const silentRefresh = useCallback(async () => {
    try {
      const data = await bridgeRequest<HistoryEntry[]>('history.getAll', { includeThumbnails: true });
      setEntries(Array.isArray(data) ? data : []);
    } catch {
      // 窗口获焦自动同步静默容错，避免干扰用户操作
    }
  }, []);

  useEffect(() => {
    bridgeRequest<HistoryEntry[]>('history.getAll', { includeThumbnails: true })
      .then((data) => {
        setEntries(Array.isArray(data) ? data : []);
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

  const handleRefresh = async () => {
    setRefreshing(true);
    try {
      const data = await bridgeRequest<HistoryEntry[]>('history.getAll', { includeThumbnails: true });
      setEntries(Array.isArray(data) ? data : []);
    } catch (error) {
      console.error('Failed to fetch history', error);
      toast.error(t('history.loadFailed'));
    } finally {
      setRefreshing(false);
    }
  };

  const handleClear = async () => {
    if (window.confirm(t('history.confirmClear', 'Are you sure you want to clear all history?'))) {
      try {
        const result = await bridgeRequest<{ success: boolean; error?: string }>('history.clear', {});
        if (!result.success) throw new Error(result.error || t('history.clearFailed'));
        setEntries([]);
      } catch (error) {
        toast.error(t('history.clearFailed'), { description: String(error) });
      }
    }
  };

  const openEntry = async (index: number) => {
    try {
      const result = await bridgeRequest<{ success: boolean; error?: string }>('history.open', { index });
      if (!result.success) throw new Error(result.error || t('history.openFailed'));
    } catch (error) {
      toast.error(t('history.openFailed'), { description: String(error) });
    }
  };

  const copyHistoryEntry = async (entry: HistoryEntry) => {
    const actionKey = `copy-${entry.index}`;
    if (busyActionKey === actionKey) return;
    setBusyActionKey(actionKey);
    try {
      const result = await bridgeRequest<{ success: boolean; error?: string }>('capture.copyHistoryItem', {
        index: entry.index,
        path: entry.filePath || '',
      });
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
    const actionKey = `save-${entry.index}`;
    if (busyActionKey === actionKey) return;
    setBusyActionKey(actionKey);
    try {
      const result = await bridgeRequest<{
        success: boolean;
        saved?: boolean;
        cancelled?: boolean;
        filePath?: string;
        error?: string;
      }>('capture.saveHistoryItemAs', {
        index: entry.index,
        path: entry.filePath || '',
      });
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
        <Button
          variant="danger"
          size="sm"
          onClick={handleClear}
          disabled={entries.length === 0 || loading}
        >
          <Trash2 size={15} aria-hidden="true" />
          <span>{t('history.clearAll', 'Clear All')}</span>
        </Button>
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
          entries.map(entry => (
            <div
              key={entry.index}
              role="button"
              tabIndex={0}
              className="history-card"
              title={t('history.pinImage')}
              onClick={() => void openEntry(entry.index)}
              onKeyDown={(e) => {
                if (e.key === 'Enter' || e.key === ' ') {
                  e.preventDefault();
                  void openEntry(entry.index);
                }
              }}
            >
              <div className="history-card-image">
                {entry.base64 ? (
                  <img src={`data:image/png;base64,${entry.base64}`} alt={t('history.thumbnailAlt')} loading="lazy" />
                ) : (
                  <div className="placeholder"><ImageIcon size={24} aria-hidden="true" /></div>
                )}
                <div className="history-card-overlay">
                  <ExternalLink size={24} aria-hidden="true" />
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
                  <div style={{ display: 'flex', alignItems: 'center', gap: '4px', marginLeft: 'auto' }}>
                    <Button
                      variant="ghost"
                      size="sm"
                      className="history-locate-btn"
                      disabled={busyActionKey === `copy-${entry.index}`}
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
                      disabled={busyActionKey === `save-${entry.index}`}
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
                  </div>
                </div>
              </div>
            </div>
          ))
        )}
      </div>
    </div>
  );
}

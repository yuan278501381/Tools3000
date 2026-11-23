import { useMemo, useState, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { Archive, ExternalLink, File, Folder, Lock, Search } from 'lucide-react';
import { bridgeRequest } from '../hooks/useBridge';

export interface ArchiveEntryItem {
  path: string;
  isDirectory: boolean;
  size: number;
  formattedSize: string;
  compressedSize?: number;
  encrypted?: boolean;
  modified?: string;
}

export interface ArchivePreviewPayload {
  ok: boolean;
  format: string;
  detailNote?: string;
  encrypted?: boolean;
  truncated?: boolean;
  totalEntries: number;
  totalFiles: number;
  totalFolders: number;
  totalUncompressedBytes: number;
  formattedUncompressedSize: string;
  entries: ArchiveEntryItem[];
}

interface QuickLookArchiveViewerProps {
  filePath: string;
  fileName: string;
  formattedFileSize: string;
  archiveSizeBytes: number;
  archiveData: ArchivePreviewPayload;
}

export const QuickLookArchiveViewer: FC<QuickLookArchiveViewerProps> = ({
  filePath,
  fileName,
  formattedFileSize,
  archiveSizeBytes,
  archiveData,
}) => {
  const { t } = useTranslation();
  const [filterText, setFilterText] = useState('');

  const ratioLabel = useMemo(() => {
    if (!archiveData.totalUncompressedBytes || archiveData.totalUncompressedBytes <= 0 || archiveSizeBytes <= 0) {
      return '';
    }
    const pct = Math.max(0, Math.min(999, Math.round((archiveSizeBytes / archiveData.totalUncompressedBytes) * 100)));
    return `${pct}%`;
  }, [archiveData.totalUncompressedBytes, archiveSizeBytes]);

  const filteredEntries = useMemo(() => {
    const q = filterText.trim().toLowerCase();
    if (!q) return archiveData.entries;
    return archiveData.entries.filter((item) => item.path.toLowerCase().includes(q));
  }, [archiveData.entries, filterText]);

  if (!archiveData.ok && archiveData.entries.length === 0) {
    return (
      <div className="ql-binary-view">
        <div className="ql-binary-card">
          <div className="ql-binary-icon">
            {archiveData.encrypted ? <Lock size={36} aria-hidden="true" /> : <Archive size={36} aria-hidden="true" />}
          </div>
          <div className="ql-binary-name">{fileName}</div>
          <div className="ql-binary-meta">
            {formattedFileSize} · {archiveData.format || 'ARCHIVE'}
            {archiveData.detailNote ? ` · ${archiveData.detailNote}` : ''}
          </div>
          <p className="ql-binary-hint">
            {archiveData.encrypted
              ? t('quicklook.archiveEncryptedHint', 'This archive is password-protected or encrypted. Press Enter to open it in your archive manager.')
              : t('quicklook.archiveFallbackHint', 'Direct directory listing is unavailable for this archive format on the current system. Press Enter to open it in your archive manager.')}
          </p>
          <div className="ql-binary-actions">
            <button
              type="button"
              className="ql-btn ql-btn-primary"
              onClick={() => void bridgeRequest('quicklook.open', { path: filePath })}
            >
              <ExternalLink size={14} aria-hidden="true" />
              <span>{t('quicklook.openInDefaultApp', 'Open with Default App (Enter)')}</span>
            </button>
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="ql-archive-view">
      <div className="ql-archive-toolbar">
        <div className="ql-archive-stats">
          <span className="ql-archive-pill">{archiveData.format || 'ZIP'}</span>
          <span>
            {t('quicklook.archiveStatsSummary', '{{files}} files, {{folders}} folders', {
              files: archiveData.totalFiles,
              folders: archiveData.totalFolders,
            })}
          </span>
          {archiveData.totalUncompressedBytes > 0 && (
            <span>
              · {t('quicklook.archiveUncompressed', 'Uncompressed {{size}}', {
                size: archiveData.formattedUncompressedSize,
              })}
              {ratioLabel ? ` (${ratioLabel})` : ''}
            </span>
          )}
          {archiveData.encrypted && (
            <span className="ql-archive-pill ql-archive-pill--warn">
              <Lock size={12} aria-hidden="true" />
              <span>{t('quicklook.archiveEncryptedBadge', 'Encrypted')}</span>
            </span>
          )}
        </div>

        <div className="ql-archive-search-wrap">
          <Search size={14} className="ql-archive-search-icon" aria-hidden="true" />
          <input
            type="text"
            className="ql-archive-search-input"
            value={filterText}
            onChange={(e) => setFilterText(e.target.value)}
            placeholder={t('quicklook.archiveFilterPlaceholder', 'Filter entries inside archive...')}
            aria-label={t('quicklook.archiveFilterPlaceholder', 'Filter entries inside archive...')}
          />
        </div>
      </div>

      {archiveData.truncated && (
        <div className="ql-truncated-banner" role="status">
          <span>
            {t('quicklook.archiveTruncatedNotice', 'Showing first {{shown}} of {{total}} entries for instant preview.', {
              shown: archiveData.entries.length,
              total: archiveData.totalEntries,
            })}
          </span>
        </div>
      )}

      <div className="ql-folder-table-container ql-archive-table-wrap">
        <table className="ql-folder-table">
          <thead>
            <tr>
              <th>{t('quicklook.colName', 'Name')}</th>
              <th>{t('quicklook.colSize', 'Size')}</th>
              <th>{t('quicklook.colModified', 'Modified')}</th>
            </tr>
          </thead>
          <tbody>
            {filteredEntries.map((entry, idx) => {
              const depth = (entry.path.match(/\//g) || []).length;
              const baseName = entry.path.includes('/') ? entry.path.slice(entry.path.lastIndexOf('/') + 1) : entry.path;
              const parentDir = entry.path.includes('/') ? entry.path.slice(0, entry.path.lastIndexOf('/')) : '';
              return (
                <tr key={`${entry.path}-${idx}`}>
                  <td className="ql-cell-name" title={entry.path}>
                    <span className="ql-archive-entry-cell" style={{ paddingLeft: `${Math.min(depth, 6) * 14}px` }}>
                      <span className="ql-table-icon">
                        {entry.isDirectory ? <Folder size={14} aria-hidden="true" /> : <File size={14} aria-hidden="true" />}
                      </span>
                      <span className="ql-archive-entry-name">{baseName}</span>
                      {parentDir && <span className="ql-archive-entry-dir">{parentDir}/</span>}
                      {entry.encrypted && <Lock size={12} className="ql-archive-lock-icon" aria-hidden="true" />}
                    </span>
                  </td>
                  <td className="ql-archive-num">{entry.formattedSize}</td>
                  <td className="ql-archive-num">{entry.modified || '-'}</td>
                </tr>
              );
            })}
            {filteredEntries.length === 0 && (
              <tr>
                <td colSpan={3} className="ql-archive-empty">
                  {t('quicklook.archiveNoMatch', 'No matching entries in archive')}
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
};

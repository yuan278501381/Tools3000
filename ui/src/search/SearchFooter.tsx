import React from 'react';
import { useTranslation } from 'react-i18next';
import { RefreshCw, Square } from 'lucide-react';
import type { SearchMode, SearchResult } from './searchTypes';
import { formatBytes, isEffectiveContentSearch } from './searchUtils';

export interface SearchFooterProps {
  loading: boolean;
  activeCategory: string;
  searchMode?: SearchMode;
  query: string;
  scanElapsedSeconds: number;
  scanProgress?: { scanned: number; total: number } | null;
  onStopSearch?: () => void;
  isServiceStarting: boolean;
  isInitialIndexing: boolean;
  sortedResults: SearchResult[];
  totalIndexedFiles?: number;
  totalResultSize: number;
  selectedIndex: number;
  searchElapsedMs?: number;
  isRebuilding: boolean;
  onRebuildIndex: () => void;
  onOpenResult: (result: SearchResult) => void;
  onOpenFolderResult: (result: SearchResult) => void;
  onCopyPathResult: (result: SearchResult) => void;
  onExportResultsToCsv: () => void;
  onToggleSyntaxHelp: () => void;
  onHide: () => void;
}

export const SearchFooter: React.FC<SearchFooterProps> = ({
  loading,
  activeCategory,
  searchMode = 'name',
  query,
  scanElapsedSeconds,
  scanProgress,
  onStopSearch,
  isServiceStarting,
  isInitialIndexing,
  sortedResults,
  totalIndexedFiles,
  totalResultSize,
  selectedIndex,
  searchElapsedMs,
  isRebuilding,
  onRebuildIndex,
  onOpenResult,
  onOpenFolderResult,
  onCopyPathResult,
  onExportResultsToCsv,
  onToggleSyntaxHelp,
  onHide,
}) => {
  const { t } = useTranslation();

  const isContentScan = isEffectiveContentSearch({
    query,
    activeCategory,
    searchMode,
  });

  const progressPercent =
    scanProgress && scanProgress.total > 0
      ? Math.min(99, Math.max(1, Math.round((scanProgress.scanned / scanProgress.total) * 100)))
      : null;

  return (
    <footer className="search-footer">
      <div className="search-footer-left">
        {loading && sortedResults.length === 0 ? (
          <div className="search-footer-stat-item search-footer-stat-item--scanning" style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
            <RefreshCw size={11} className="spin-animation" style={{ color: 'var(--primary)' }} />
            <span>
              <strong>
                {isContentScan
                  ? t('search.contentSearchingFooter', 'Deep full-disk content search in progress... ({{elapsed}}s)', { elapsed: scanElapsedSeconds.toFixed(1) })
                  : t('search.nameSearchingFooter', 'Scanning full disk file index... ({{elapsed}}s)', { elapsed: scanElapsedSeconds.toFixed(1) })}
                {progressPercent !== null ? ` · ${progressPercent}%` : ''}
              </strong>
            </span>
            {onStopSearch && (
              <button
                type="button"
                className="search-stop-scan-btn"
                onClick={onStopSearch}
                title={t('search.stopScanTitle', 'Stop active background content scan')}
              >
                <Square size={9} fill="currentColor" aria-hidden="true" />
                <span>{t('search.stopScan', 'Stop')}</span>
              </button>
            )}
          </div>
        ) : (
          <>
            {/* 1. 总对象数显示 (Everything 级核心状态) 与一键刷新微按钮 */}
            <div className="search-footer-stat-item search-footer-stat-primary search-footer-stat-item--interactive" title={t('search.rescanIndexTitle', 'Rescan full disk index and save snapshot (F5 / Ctrl+R)')}>
              <span>
                {isServiceStarting ? (
                  <><strong>{t('search.serviceConnectingStatus', 'Connecting to Index Service...')}</strong></>
                ) : isInitialIndexing ? (
                  <><strong>{t('search.initialIndexingTitle', 'Building and persisting full disk index...')}</strong></>
                ) : sortedResults.length > 0 ? (
                  <><strong>{sortedResults.length.toLocaleString()}{loading ? '+' : ''}</strong> {t('search.objectsCount', 'objects')}</>
                ) : (
                  <><strong>{totalIndexedFiles ? totalIndexedFiles.toLocaleString() : '--'}</strong> {t('search.objectsCount', 'objects')}</>
                )}
              </span>
              <button
                type="button"
                className="search-footer-refresh-btn"
                onClick={(e) => {
                  e.stopPropagation();
                  void onRebuildIndex();
                }}
                disabled={isRebuilding || isServiceStarting}
                title={t('search.rescanIndexTitle', 'Rescan full disk index and save snapshot (F5 / Ctrl+R)')}
              >
                <RefreshCw size={11} className={(isRebuilding || isServiceStarting) ? 'spin-animation' : ''} />
              </button>
            </div>

            {/* 2. 当前匹配结果总大小 */}
            {sortedResults.length > 0 && totalResultSize > 0 && (
              <>
                <span className="search-footer-stat-divider search-footer-stat-divider--size" />
                <div className="search-footer-stat-item search-footer-stat-size" title={formatBytes(totalResultSize)}>
                  <span><strong>{formatBytes(totalResultSize)}</strong></span>
                </div>
              </>
            )}

            {/* 3. 当前选中项序号与单项大小 */}
            {sortedResults.length > 0 && sortedResults[selectedIndex] && (
              <>
                <span className="search-footer-stat-divider search-footer-stat-divider--detail" />
                <div className="search-footer-stat-item search-footer-stat-sub search-footer-stat-detail" title={sortedResults[selectedIndex].path}>
                  <span className="search-footer-detail-text">
                    {t('search.selectedLabel', 'Selected')} <strong>{selectedIndex + 1}</strong> / {sortedResults.length}
                    {!sortedResults[selectedIndex].isDirectory && sortedResults[selectedIndex].size !== undefined ? (
                      <> ({formatBytes(sortedResults[selectedIndex].size || 0)})</>
                    ) : (
                      <> ({t('search.folderLabel', 'Folder')})</>
                    )}
                  </span>
                </div>
              </>
            )}

            {/* 4. 引擎匹配耗时或流式扫描进度 */}
            {loading ? (
              <>
                <span className="search-footer-stat-divider search-footer-stat-divider--timing" />
                <div className="search-footer-stat-item search-footer-stat-sub search-footer-stat-timing" style={{ display: 'inline-flex', alignItems: 'center', gap: 5 }}>
                  <RefreshCw size={10} className="spin-animation" style={{ color: 'var(--primary)' }} />
                  <span>
                    {t('search.searchingElapsed', 'Searching... {{elapsed}}s', { elapsed: scanElapsedSeconds.toFixed(1) })}
                    {progressPercent !== null ? ` (${progressPercent}%)` : ''}
                  </span>
                  {onStopSearch && (
                    <button
                      type="button"
                      className="search-stop-scan-btn"
                      onClick={onStopSearch}
                      title={t('search.stopScanTitle', 'Stop active background content scan')}
                    >
                      <Square size={9} fill="currentColor" aria-hidden="true" />
                      <span>{t('search.stopScan', 'Stop')}</span>
                    </button>
                  )}
                </div>
              </>
            ) : (
              searchElapsedMs !== undefined && searchElapsedMs >= 0 && sortedResults.length > 0 && (
                <>
                  <span className="search-footer-stat-divider search-footer-stat-divider--timing" />
                  <div className="search-footer-stat-item search-footer-stat-sub search-footer-stat-timing" title={t('search.searchTimeTitle', 'MFT memory index match elapsed time')}>
                    <span>{searchElapsedMs} ms</span>
                  </div>
                </>
              )
            )}
          </>
        )}
      </div>

      <div className="search-footer-right">
        {sortedResults.length > 0 && (
          <>
            <button
              type="button"
              className="search-hint-btn search-hint-btn--primary"
              onClick={() => onOpenResult(sortedResults[selectedIndex])}
              title={t('search.openFileTitle', 'Open selected file (Enter)')}
            >
              <kbd>Enter</kbd>
              <span className="search-hint-label">{t('search.openShortcut', 'Open')}</span>
            </button>
            <button
              type="button"
              className="search-hint-btn search-hint-btn--primary"
              onClick={() => onOpenFolderResult(sortedResults[selectedIndex])}
              title={t('search.openFolderTitle', 'Locate and select file in Explorer (Ctrl+Enter)')}
            >
              <kbd>Ctrl+Enter</kbd>
              <span className="search-hint-label">{t('search.openFolderShortcut', 'Open Folder')}</span>
            </button>
            <button
              type="button"
              className="search-hint-btn search-hint-btn--primary"
              onClick={() => onCopyPathResult(sortedResults[selectedIndex])}
              title={t('search.copyPathTitle', 'Copy full file path (Ctrl+C)')}
            >
              <kbd>Ctrl+C</kbd>
              <span className="search-hint-label">{t('search.copyPathShortcut', 'Copy Path')}</span>
            </button>
            <button
              type="button"
              className="search-hint-btn search-hint-btn--auxiliary"
              onClick={onExportResultsToCsv}
              title={t('search.exportReportTitle', 'Export search results to CSV report (Ctrl+E)')}
            >
              <kbd>Ctrl+E</kbd>
              <span className="search-hint-label">{t('search.exportShortcut', 'Export')}</span>
            </button>
          </>
        )}
        <button
          type="button"
          className="search-hint-btn search-hint-btn--auxiliary"
          onClick={onRebuildIndex}
          disabled={isRebuilding}
          title={t('search.rescanIndexTitle', 'Rescan full disk index and save snapshot (F5 / Ctrl+R)')}
        >
          <kbd>F5</kbd>
          <span className="search-hint-label">{isRebuilding ? t('search.refreshingShortcut', 'Refreshing...') : t('search.refreshShortcut', 'Refresh')}</span>
        </button>
        <button
          type="button"
          className="search-hint-btn search-hint-btn--auxiliary"
          onClick={onToggleSyntaxHelp}
          title={t('search.syntaxHelpBtnTitle', 'View advanced search syntax and expressions (F1)')}
        >
          <kbd>F1</kbd>
          <span className="search-hint-label">{t('search.syntaxShortcut', 'Syntax')}</span>
        </button>
        <button
          type="button"
          className="search-hint-btn search-hint-btn--auxiliary"
          onClick={onHide}
          title={t('search.closeWindowBtnTitle', 'Close search window (Esc)')}
        >
          <kbd>Esc</kbd>
          <span className="search-hint-label">{t('search.closeShortcut', 'Close')}</span>
        </button>
      </div>
    </footer>
  );
};

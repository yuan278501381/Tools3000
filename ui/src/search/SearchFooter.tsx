import React from 'react';
import { useTranslation } from 'react-i18next';
import { PauseCircle, Play, Radio, RefreshCw, Square } from 'lucide-react';
import type {
  DbStatsSnapshot,
  SearchMode,
  SearchResult,
  SortProgressState,
} from './searchTypes';
import type { ContentScanPauseState } from './useSearchEngine';
import { formatBytes, isEffectiveContentSearch } from './searchUtils';

export interface SearchFooterProps {
  loading: boolean;
  activeCategory: string;
  searchMode?: SearchMode;
  query: string;
  scanElapsedSeconds: number;
  scanProgress?: { scanned: number; total: number } | null;
  contentScanPaused?: ContentScanPauseState | null;
  onStopSearch?: () => void;
  onResumeContentScan?: () => void;
  isServiceStarting: boolean;
  isInitialIndexing: boolean;
  dbStats?: DbStatsSnapshot | null;
  sortProgress?: SortProgressState | null;
  onCancelSort?: () => void;
  sortedResults: SearchResult[];
  totalIndexedFiles?: number;
  totalResultSize: number;
  selectedIndex: number;
  searchElapsedMs?: number;
  isRebuilding: boolean;
  liveRefreshEnabled?: boolean;
  onToggleLiveRefresh?: () => void;
  onRebuildIndex: () => void;
  onOpenResult: (result: SearchResult) => void;
  onOpenFolderResult: (result: SearchResult) => void;
  onCopyPathResult: (result: SearchResult) => void;
  onExportResultsToCsv: () => void;
  onToggleSyntaxHelp: () => void;
  onHide: () => void;
  onTitlebarMouseDown?: (e: React.MouseEvent<HTMLElement>) => void;
  onTitlebarDoubleClick?: (e: React.MouseEvent<HTMLElement>) => void;
}

export const SearchFooter: React.FC<SearchFooterProps> = ({
  loading,
  activeCategory,
  searchMode = 'name',
  query,
  scanElapsedSeconds,
  scanProgress,
  contentScanPaused,
  onStopSearch,
  onResumeContentScan,
  isServiceStarting,
  isInitialIndexing,
  dbStats,
  sortProgress,
  onCancelSort,
  sortedResults,
  totalIndexedFiles,
  totalResultSize,
  selectedIndex,
  searchElapsedMs,
  isRebuilding,
  liveRefreshEnabled = true,
  onToggleLiveRefresh,
  onRebuildIndex,
  onOpenResult,
  onOpenFolderResult,
  onCopyPathResult,
  onExportResultsToCsv,
  onToggleSyntaxHelp,
  onHide,
  onTitlebarMouseDown,
  onTitlebarDoubleClick,
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

  // 实时后台索引/扫描/同步阶段文案（Everything 1.5 同款：正在扫描 OS (C:) / 正在构建索引 / 正在加载快照）
  const activePhase = dbStats?.phase ?? 'idle';
  const isPhaseBusy =
    isInitialIndexing ||
    isRebuilding ||
    dbStats?.indexing === true ||
    dbStats?.rebuildInProgress === true ||
    (activePhase !== 'idle' && activePhase !== 'starting');

  const formatDriveDisplay = (): string => {
    const drive = dbStats?.activeDrive;
    const label = dbStats?.activeVolumeLabel?.trim();
    if (drive && label) return `${label} (${drive}:)`;
    if (drive) return `(${drive}:)`;
    return '';
  };

  const renderIndexPhaseText = (): string => {
    const driveDisplay = formatDriveDisplay();
    if (activePhase === 'scanning') {
      return driveDisplay
        ? t('search.phaseScanningDrive', 'Scanning {{drive}}...', { drive: driveDisplay })
        : t('search.phaseScanningGeneric', 'Scanning local drives...');
    }
    if (activePhase === 'building') {
      return driveDisplay
        ? t('search.phaseBuildingDrive', 'Building folder index {{drive}}...', { drive: driveDisplay })
        : t('search.phaseBuildingGeneric', 'Building folder index...');
    }
    if (activePhase === 'loading') {
      return driveDisplay
        ? t('search.phaseLoadingDrive', 'Loading snapshot {{drive}}...', { drive: driveDisplay })
        : t('search.phaseLoadingGeneric', 'Loading index snapshot...');
    }
    if (activePhase === 'syncing') {
      return driveDisplay
        ? t('search.phaseSyncingDrive', 'Updating index {{drive}}...', { drive: driveDisplay })
        : t('search.phaseSyncingGeneric', 'Updating file index...');
    }
    return t('search.initialIndexingTitle', 'Building and persisting full disk index...');
  };

  const renderStageProgressTrack = (percent: number | null) => (
    <span className="search-footer-progress-track" role="progressbar" aria-valuenow={percent ?? undefined} aria-valuemin={0} aria-valuemax={100}>
      {percent !== null && percent > 0 ? (
        <span className="search-footer-progress-fill" style={{ width: `${Math.min(100, Math.max(4, percent))}%` }} />
      ) : (
        <span className="search-footer-progress-fill search-footer-progress-fill--indeterminate" />
      )}
    </span>
  );

  const sortPercent =
    sortProgress && sortProgress.active && sortProgress.total > 0
      ? Math.min(99, Math.max(1, Math.round((sortProgress.completed / sortProgress.total) * 100)))
      : null;

  const indexPercent =
    typeof dbStats?.progressPercent === 'number' && dbStats.progressPercent > 0 && dbStats.progressPercent < 100
      ? dbStats.progressPercent
      : null;

  return (
    <footer
      className="search-footer"
      onMouseDown={onTitlebarMouseDown}
      onDoubleClick={onTitlebarDoubleClick}
    >
      <div className="search-footer-left">
        {sortProgress?.active ? (
          <div className="search-footer-stat-item search-footer-stat-item--stage">
            <RefreshCw size={11} className="spin-animation search-footer-stage-icon" aria-hidden="true" />
            <span className="search-footer-stage-label">
              <strong>
                {t('search.sortingColumnStage', 'Sorting {{column}}...', { column: sortProgress.columnLabel })}
              </strong>
              <span className="search-footer-stage-count">
                ({sortProgress.completed.toLocaleString()} / {sortProgress.total.toLocaleString()})
              </span>
            </span>
            {renderStageProgressTrack(sortPercent)}
            {onCancelSort && (
              <button
                type="button"
                className="search-stop-scan-btn"
                onClick={onCancelSort}
                title={t('search.stopSortTitle', 'Stop property loading for sort')}
              >
                <Square size={9} fill="currentColor" aria-hidden="true" />
                <span>{t('search.stopScan', 'Stop')}</span>
              </button>
            )}
          </div>
        ) : isServiceStarting ? (
          <div className="search-footer-stat-item search-footer-stat-item--stage">
            <RefreshCw size={11} className="spin-animation search-footer-stage-icon" aria-hidden="true" />
            <span className="search-footer-stage-label">
              <strong>{t('search.serviceConnectingStatus', 'Connecting to Index Service...')}</strong>
            </span>
            {renderStageProgressTrack(null)}
          </div>
        ) : isPhaseBusy && sortedResults.length === 0 ? (
          <div className="search-footer-stat-item search-footer-stat-item--stage">
            <RefreshCw size={11} className="spin-animation search-footer-stage-icon" aria-hidden="true" />
            <span className="search-footer-stage-label">
              <strong>{renderIndexPhaseText()}</strong>
              {typeof dbStats?.scannedRecords === 'number' && dbStats.scannedRecords > 0 && (
                <span className="search-footer-stage-count">
                  {dbStats.scannedRecords.toLocaleString()}
                </span>
              )}
            </span>
            {renderStageProgressTrack(indexPercent)}
          </div>
        ) : loading && sortedResults.length === 0 ? (
          <div className="search-footer-stat-item search-footer-stat-item--scanning search-footer-stat-item--stage">
            <RefreshCw size={11} className="spin-animation search-footer-stage-icon" aria-hidden="true" />
            <span className="search-footer-stage-label">
              <strong>
                {isContentScan
                  ? t('search.contentSearchingFooter', 'Deep full-disk content search in progress... ({{elapsed}}s)', { elapsed: scanElapsedSeconds.toFixed(1) })
                  : t('search.nameSearchingFooter', 'Scanning full disk file index... ({{elapsed}}s)', { elapsed: scanElapsedSeconds.toFixed(1) })}
                {progressPercent !== null ? ` · ${progressPercent}%` : ''}
              </strong>
            </span>
            {renderStageProgressTrack(progressPercent)}
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
                {sortedResults.length > 0 ? (
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
              {onToggleLiveRefresh && (
                <button
                  type="button"
                  className={`search-footer-live-btn ${liveRefreshEnabled ? 'search-footer-live-btn--active' : ''}`}
                  onClick={(e) => {
                    e.stopPropagation();
                    onToggleLiveRefresh();
                  }}
                  aria-pressed={liveRefreshEnabled}
                  title={liveRefreshEnabled
                    ? t('search.liveWatchOnTitle', 'Live file refresh enabled (click to pause automatic updates when new files appear)')
                    : t('search.liveWatchOffTitle', 'Live file refresh paused (click to automatically display newly added files)')}
                >
                  <Radio size={11} aria-hidden="true" />
                  <span>{liveRefreshEnabled ? t('search.liveWatchOn', 'Live') : t('search.liveWatchOff', 'Paused')}</span>
                </button>
              )}
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

            {/* 4. 后台重建/增量扫描实时进度胶囊（常驻可见，不受 960px 断点误隐藏）或检索耗时 */}
            {isPhaseBusy ? (
              <>
                <span className="search-footer-stat-divider search-footer-stat-divider--live" />
                <div className="search-footer-stat-item search-footer-stat-sub search-footer-stat-live search-footer-stat-item--stage">
                  <RefreshCw size={10} className="spin-animation search-footer-stage-icon" aria-hidden="true" />
                  <span className="search-footer-stage-label">{renderIndexPhaseText()}</span>
                  {renderStageProgressTrack(indexPercent)}
                </div>
              </>
            ) : loading ? (
              <>
                <span className="search-footer-stat-divider search-footer-stat-divider--live" />
                <div className="search-footer-stat-item search-footer-stat-sub search-footer-stat-live search-footer-stat-item--stage">
                  <RefreshCw size={10} className="spin-animation search-footer-stage-icon" aria-hidden="true" />
                  <span className="search-footer-stage-label">
                    {t('search.searchingElapsed', 'Searching... {{elapsed}}s', { elapsed: scanElapsedSeconds.toFixed(1) })}
                    {progressPercent !== null ? ` (${progressPercent}%)` : ''}
                  </span>
                  {renderStageProgressTrack(progressPercent)}
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
            ) : contentScanPaused ? (
              <>
                <span className="search-footer-stat-divider search-footer-stat-divider--live" />
                <div className="search-footer-stat-item search-footer-stat-sub search-footer-stat-live search-footer-stat-item--stage">
                  <PauseCircle size={11} className="search-footer-stage-icon search-footer-stage-icon--paused" aria-hidden="true" />
                  <span className="search-footer-stage-label">
                    {contentScanPaused.reason === 'background'
                      ? t('search.contentScanPausedBackground', 'Deep scan paused in background')
                      : contentScanPaused.total > 0
                        ? t('search.contentScanPausedBudget', 'Scanned {{scanned}} / {{total}} docs (auto-paused)', {
                            scanned: contentScanPaused.scanned.toLocaleString(),
                            total: contentScanPaused.total.toLocaleString(),
                          })
                        : t('search.contentScanPausedBudgetGeneric', 'Deep scan auto-paused to save resources')}
                  </span>
                  {onResumeContentScan && (
                    <button
                      type="button"
                      className="search-resume-scan-btn"
                      onClick={onResumeContentScan}
                      title={t('search.resumeDeepScanTitle', 'Continue scanning remaining documents from where it paused')}
                    >
                      <Play size={9} fill="currentColor" aria-hidden="true" />
                      <span>{t('search.resumeDeepScan', 'Continue')}</span>
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

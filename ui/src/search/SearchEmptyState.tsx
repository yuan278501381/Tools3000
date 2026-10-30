import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  FileText,
  Clock,
  Zap,
  RefreshCw,
  ServerOff,
  SearchX,
  Search,
  Lightbulb,
} from 'lucide-react';
import { CodeBadge } from '../components/UIKit';
import type { CategoryFilter, SearchMode, SearchResult } from './searchTypes';
import { isEffectiveContentSearch } from './searchUtils';

export interface SearchEmptyStateProps {
  isInitialIndexing: boolean;
  isServiceStarting: boolean;
  serviceAvailable: boolean;
  loading: boolean;
  activeCategory: string;
  searchMode?: SearchMode;
  query: string;
  sortedResults: SearchResult[];
  searchHistoryLength: number;
  scanElapsedSeconds: number;
  lastColdScanDurationSec: number;
  actionError: string;
  categories: CategoryFilter[];
  onSelectCategory: (cat: CategoryFilter) => void;
  onUpdateQuery: (tag: string) => void;
  inputRef: React.RefObject<HTMLInputElement | null>;
}

export const SearchEmptyState: React.FC<SearchEmptyStateProps> = ({
  isInitialIndexing,
  isServiceStarting,
  serviceAvailable,
  loading,
  activeCategory,
  searchMode = 'name',
  query,
  sortedResults,
  searchHistoryLength,
  scanElapsedSeconds,
  lastColdScanDurationSec,
  actionError,
  categories,
  onSelectCategory,
  onUpdateQuery,
  inputRef,
}) => {
  const { t } = useTranslation();

  const isContentSearch = isEffectiveContentSearch({
    query,
    activeCategory,
    searchMode,
  });

  // 1. 全息深度内容检索加载舱 (Cold Content Scan Holographic State)
  // 严格限定仅在真正的内容检索扫描且结果为空时展示全盘雷达波，普通搜索打字绝不炸屏
  if (!isInitialIndexing && !isServiceStarting && loading && isContentSearch && sortedResults.length === 0) {
    return (
      <div className="search-empty-container search-empty-container--deep-scan" role="status">
        <div className="search-deep-scan-radar">
          <div className="search-deep-scan-radar-wave wave-1" />
          <div className="search-deep-scan-radar-wave wave-2" />
          <div className="search-empty-icon-wrap search-empty-icon-wrap--deep-scan">
            <FileText size={32} className="search-empty-icon" />
          </div>
        </div>
        
        <div className="search-empty-title search-deep-scan-title">
          {t('search.contentScanningTitle', 'Deep Full-Disk Content Search in progress...')}
        </div>

        <div className="search-empty-desc search-deep-scan-desc">
          {t('search.contentScanningDesc', 'First-time query reads file contents directly from disk sectors (cold cache). Subsequent searches are accelerated by Windows memory page cache (instant).')}
        </div>

        <div className="search-deep-scan-dashboard">
          <div className="search-deep-scan-metrics">
            <div className="search-deep-scan-metric-badge search-deep-scan-metric--elapsed">
              <Clock size={13} />
              <span>{t('search.contentScanningElapsed', 'Scanned for {{elapsed}}s', { elapsed: scanElapsedSeconds.toFixed(1) })}</span>
            </div>
            <div className="search-deep-scan-metric-badge search-deep-scan-metric--bench" title={t('search.contentScanningLastBench', 'Last cold scan took ~{{duration}}s', { duration: lastColdScanDurationSec.toFixed(1) })}>
              <Zap size={13} />
              <span>{t('search.contentScanningLastBench', 'Last cold scan took ~{{duration}}s', { duration: lastColdScanDurationSec.toFixed(1) })}</span>
            </div>
          </div>

          <div className="search-deep-scan-progress-bar-wrap">
            <div
              className="search-deep-scan-progress-bar"
              style={{
                width: `${Math.min(94, Math.max(8, (scanElapsedSeconds / lastColdScanDurationSec) * 100))}%`
              }}
            />
          </div>

          <div className="search-deep-scan-tip">
            <span>{t('search.contentScanningSpeedupTip', 'Accelerated 5x+ once Windows memory cache warms up')}</span>
          </div>
        </div>
      </div>
    );
  }

  // 2. 初始索引构建或服务正在唤醒
  if ((isInitialIndexing || isServiceStarting) && sortedResults.length === 0) {
    return (
      <div className="search-empty-container search-empty-container--indexing" role="status">
        <div className="search-empty-icon-wrap search-empty-icon-wrap--indexing">
          <RefreshCw size={32} className="search-empty-icon spin-animation" />
        </div>
        <div className="search-empty-title">
          {isInitialIndexing
             ? t('search.initialIndexingTitle', 'Building and persisting full disk index...')
            : t('search.serviceStartingTitle', 'Connecting to File Indexing Engine...')}
        </div>
        <div className="search-empty-desc">
          {isInitialIndexing
            ? t('search.initialIndexingDesc', 'First run is scanning all local MFT tables and persisting index. Instant search will be ready in seconds.')
            : t('search.serviceStartingDesc', 'Waking up the indexing engine on demand and loading disk cache. Results will appear automatically, please wait...')}
        </div>
        <div className="search-indexing-progress-bar-wrap">
          <div className="search-indexing-progress-bar-indeterminate" />
        </div>
      </div>
    );
  }

  // 3. 服务不可用
  if (!isInitialIndexing && !isServiceStarting && !serviceAvailable) {
    return (
      <div className="search-status" role="status">
        <ServerOff size={18} aria-hidden="true" />
        <span>{t('search.serviceUnavailable', 'The file index service is unavailable. Repair or reinstall Tools3000 to restore instant search.')}</span>
      </div>
    );
  }

  // 4. Action 错误提示
  if (!isInitialIndexing && !isServiceStarting && actionError) {
    return <div className="search-status search-status--error" role="alert">{actionError}</div>;
  }

  // 5. 搜索无匹配结果
  if (!isInitialIndexing && !isServiceStarting && serviceAvailable && query.trim() && !loading && sortedResults.length === 0) {
    return (
      <div className="search-empty-container" role="status">
        <div className="search-empty-icon-wrap">
          <SearchX size={34} className="search-empty-icon" />
        </div>
        <div className="search-empty-title">
          {isContentSearch
            ? t('search.noContentResults', 'No matching content found')
            : (activeCategory !== 'all' && activeCategory !== 'content')
            ? t('search.noMatchInCat', { cat: categories.find(c => c.id === activeCategory)?.label || '', defaultValue: `No matching ${categories.find(c => c.id === activeCategory)?.label || ''} files found` })
            : (query.trim().startsWith('ext:') || query.trim().startsWith('path:') || query.trim().startsWith('folder:') || query.trim().startsWith('dir:') || query.trim().startsWith('file:'))
            ? t('search.noMatchForFilter', { query: query.trim(), defaultValue: `No files matching filter "${query.trim()}"` })
            : t('search.noMatchForName', { query: query.trim(), defaultValue: `No files containing "${query.trim()}"` })}
        </div>
        {!isContentSearch && (
          <button
            className="search-switch-content-btn"
            onClick={() => {
              const contentCat = categories.find(c => c.id === 'content') || categories[1];
              if (contentCat) onSelectCategory(contentCat);
            }}
            type="button"
            title={t('search.searchContentDirectly', { query: query.trim(), defaultValue: `Search full document and code contents for: "${query.trim()}"` })}
          >
            <FileText size={15} />
            <span>{t('search.searchContentDirectly', { query: query.trim(), defaultValue: `Search full document and code contents for: "${query.trim()}"` })}</span>
          </button>
        )}
        {isContentSearch && (
          <div className="search-empty-content-diagnostic">
            <span>{t('search.contentZeroResultTip1', 'Click top-right Customize to add custom file extensions (e.g. .vue, .ts, .log)')}</span>
            <span>{t('search.contentZeroResultTip2', 'Ensure target files are not inside excluded directories (e.g. node_modules, .git)')}</span>
          </div>
        )}
        <div className="search-empty-hint">
          <Lightbulb size={12} className="search-empty-hint-icon" />
          <span>{t('search.searchHint', 'Tip: Press Enter to perform full-text search, or press F1 for advanced syntax.')}</span>
        </div>
      </div>
    );
  }

  // 6. 初始就绪状态 (无输入且无历史)
  if (!isInitialIndexing && !isServiceStarting && serviceAvailable && !query.trim() && sortedResults.length === 0 && searchHistoryLength === 0) {
    return (
      <div className="search-empty-container search-empty-container--initial" role="status">
        <div className="search-empty-icon-wrap search-empty-icon-wrap--initial">
          <Search size={32} className="search-empty-icon" />
        </div>
        <div className="search-empty-title">{t('search.readyTitle', 'Instant Full-Disk Index Ready')}</div>
        <div className="search-empty-desc">{t('search.readyDesc', 'Type keywords, wildcards (*.pdf), or extensions (ext:docx) to search instantly')}</div>
        <div className="search-empty-quick-tags">
          {['*.docx', 'ext:png', 'size:>100mb', 'content:meeting'].map((tag) => (
            <button
              key={tag}
              type="button"
              className="search-empty-quick-tag"
              onClick={() => {
                onUpdateQuery(tag);
                inputRef.current?.focus();
              }}
              title={t('search.clickToFill', 'Click to insert into search box')}
            >
              <CodeBadge variant="muted">{tag}</CodeBadge>
            </button>
          ))}
        </div>
      </div>
    );
  }

  return null;
};

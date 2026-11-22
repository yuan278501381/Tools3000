import React, { useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  SlidersHorizontal,
  X,
  RefreshCw,
  Zap,
  Sparkles,
  FileText,
  RotateCcw,
  Network,
  Disc,
  HardDrive,
  Code2,
  Palette,
  Plus,
  Trash2,
  Lightbulb,
  ChevronRight,
  Columns3,
  Maximize2,
  Minimize2,
} from 'lucide-react';
import { toast } from 'sonner';
import { bridgeRequest } from '../hooks/useBridge';
import { CodeBadge, Toggle } from '../components/UIKit';
import {
  CONTENT_FORMAT_CATEGORIES,
  WINDOW_PRESETS,
} from './searchConstants';
import type {
  ColumnId,
  ColumnSetting,
  DriveInfo,
  FormatCategory,
  SearchDensity,
  SearchIconStyle,
  SearchMode,
} from './searchTypes';
import { formatBytes } from './searchUtils';

export interface SearchViewSettingsModalProps {
  visible: boolean;
  onClose: () => void;
  popoverRef: React.RefObject<HTMLDivElement | null>;
  isRebuilding: boolean;
  onRebuildIndex: () => void;
  searchMode: SearchMode;
  onChangeSearchMode: (mode: SearchMode) => void;
  density: SearchDensity;
  onChangeDensity: (density: SearchDensity) => void;
  iconStyle: SearchIconStyle;
  onChangeIconStyle: (style: SearchIconStyle) => void;
  maxResultLimit: number;
  onChangeMaxResultLimit: (limit: number) => void;
  windowSize: { width: number; height: number };
  onSetWindowSize: (size: { width: number; height: number }) => void;
  isMaximized?: boolean;
  onToggleMaximized?: () => void;
  columns: ColumnSetting[];
  onToggleColumn: (colId: ColumnId) => void;
  onOpenColumnManager?: () => void;
  nameFlex: number;
  pathFlex: number;
  onUpdateNameAndPathFlex: (nameFlex: number) => void;
  systemDrives: DriveInfo[];
  enabledDrives: string[];
  onToggleDrive: (letter: string) => void;
  onSelectAllDrives: () => void;
  onDeselectAllDrives: () => void;
  excludeGitAndModules: boolean;
  onSetExcludeGitAndModules: (val: boolean) => void;
  excludeHidden: boolean;
  onSetExcludeHidden: (val: boolean) => void;
  disabledContentFormats: string[];
  onToggleContentFormat: (ext: string) => void;
  onToggleCategoryFormats: (category: FormatCategory, enabled: boolean) => void;
  onResetContentFormats: () => void;
  customContentFormats: string[];
  newFormatInput: string;
  onSetNewFormatInput: (val: string) => void;
  onAddCustomContentFormat: (val: string) => void;
  onRemoveCustomContentFormat: (ext: string) => void;
  onSetDisabledContentFormats: React.Dispatch<React.SetStateAction<string[]>>;
  dbStats?: { exists: boolean; dbSize: number; totalRecords: number };
  totalIndexedFiles?: number;
  onResetAllViewPreferences: () => void;
}

export const SearchViewSettingsModal: React.FC<SearchViewSettingsModalProps> = ({
  visible,
  onClose,
  popoverRef,
  isRebuilding,
  onRebuildIndex,
  searchMode,
  onChangeSearchMode,
  density,
  onChangeDensity,
  iconStyle,
  onChangeIconStyle,
  maxResultLimit,
  onChangeMaxResultLimit,
  windowSize,
  onSetWindowSize,
  isMaximized = false,
  onToggleMaximized,
  columns,
  onToggleColumn,
  onOpenColumnManager,
  nameFlex,
  pathFlex,
  onUpdateNameAndPathFlex,
  systemDrives,
  enabledDrives,
  onToggleDrive,
  onSelectAllDrives,
  onDeselectAllDrives,
  excludeGitAndModules,
  onSetExcludeGitAndModules,
  excludeHidden,
  onSetExcludeHidden,
  disabledContentFormats,
  onToggleContentFormat,
  onToggleCategoryFormats,
  onResetContentFormats,
  customContentFormats,
  newFormatInput,
  onSetNewFormatInput,
  onAddCustomContentFormat,
  onRemoveCustomContentFormat,
  dbStats,
  totalIndexedFiles,
  onResetAllViewPreferences,
}) => {
  const { t } = useTranslation();
  const [expandedFormatCats, setExpandedFormatCats] = useState<Record<string, boolean>>({});

  const toggleFormatCatExpand = (catId: string) => {
    setExpandedFormatCats((prev) => ({ ...prev, [catId]: !prev[catId] }));
  };

  if (!visible) return null;

  return (
    <div className="search-view-settings-popover" ref={popoverRef}>
      <div className="popover-header">
        <div className="popover-title">
          <SlidersHorizontal size={14} />
          <span>{t('search.drawerTitle', 'View & Search Preferences')}</span>
        </div>
        <button
          className="popover-close"
          onClick={onClose}
          type="button"
          title={t('search.close', 'Close')}
          aria-label={t('search.close', 'Close')}
        >
          <X size={14} aria-hidden="true" />
        </button>
      </div>

      <div className="popover-body">
        {/* 顶部轻量索引维护工具卡（克制高雅、零高饱和大蓝砖喧宾夺主） */}
        <div className="popover-top-rebuild-card">
          <div className="popover-top-rebuild-head">
            <div className="popover-top-rebuild-icon" aria-hidden="true">
              <HardDrive size={14} />
            </div>
            <div className="popover-top-rebuild-info">
              <div className="popover-top-rebuild-title">{t('search.rebuildSectionTitle', 'Full-Disk Index & Snapshot Maintenance')}</div>
              <div className="popover-top-rebuild-desc">
                {t('search.indexMaintenanceDesc', 'Rescan all NTFS partitions for changes and synchronize snapshot to Tools3000.db (F5 / Ctrl+R)')}
              </div>
            </div>
          </div>
          <button
            type="button"
            className={`popover-rebuild-btn popover-rebuild-btn--top ${isRebuilding ? 'popover-rebuild-btn--loading' : ''}`}
            onClick={onRebuildIndex}
            disabled={isRebuilding}
            title={t('search.rebuildButtonTip', 'Rescan all selected disks NTFS MFT partitions and save snapshot (F5 / Ctrl+R)')}
          >
            <RefreshCw size={13} className={`popover-rebuild-btn-icon ${isRebuilding ? 'spin-animation' : ''}`} />
            <span>{isRebuilding ? t('search.rebuildingInProgress', 'Rescanning and updating snapshot...') : t('search.rebuildButton', 'Rescan and Update Index & Snapshot (F5)')}</span>
          </button>
        </div>

        {/* 1. 默认搜索范围与模式 */}
        <div className="popover-section">
          <div className="popover-section-title">
            <span>{t('search.defaultSearchMode', 'Default Search Mode')}</span>
          </div>
          <div className="popover-search-modes-grid" role="radiogroup" aria-label={t('search.defaultSearchMode', 'Default Search Mode')}>
            <button
              type="button"
              role="radio"
              aria-checked={searchMode === 'name'}
              className={`popover-search-mode-card ${searchMode === 'name' ? 'popover-search-mode-card--active' : ''}`}
              onClick={() => onChangeSearchMode('name')}
            >
              <div className="search-mode-icon-box" aria-hidden="true">
                <Zap size={14} />
              </div>
              <div className="search-mode-content">
                <div className="search-mode-header">
                  <span className="search-mode-title">{t('search.modeFileOnlyTitle', 'Search File Name Only (Fast · Default)')}</span>
                  <span className={`search-mode-radio ${searchMode === 'name' ? 'search-mode-radio--checked' : ''}`} aria-hidden="true" />
                </div>
                <div className="search-mode-desc">
                  {t('search.modeNameDesc', 'Sub-millisecond response (<5ms); to search content, click the top "Content" tab or prefix with content:')}
                </div>
              </div>
            </button>

            <button
              type="button"
              role="radio"
              aria-checked={searchMode === 'both'}
              className={`popover-search-mode-card ${searchMode === 'both' ? 'popover-search-mode-card--active' : ''}`}
              onClick={() => onChangeSearchMode('both')}
            >
              <div className="search-mode-icon-box" aria-hidden="true">
                <Sparkles size={14} />
              </div>
              <div className="search-mode-content">
                <div className="search-mode-header">
                  <span className="search-mode-title">{t('search.modeHybrid', 'Filename & Content Hybrid')}</span>
                  <span className={`search-mode-radio ${searchMode === 'both' ? 'search-mode-radio--checked' : ''}`} aria-hidden="true" />
                </div>
                <div className="search-mode-desc">
                  {t('search.modeHybridDesc', 'Search both filename and document contents simultaneously')}
                </div>
              </div>
            </button>

            <button
              type="button"
              role="radio"
              aria-checked={searchMode === 'content'}
              className={`popover-search-mode-card ${searchMode === 'content' ? 'popover-search-mode-card--active' : ''}`}
              onClick={() => onChangeSearchMode('content')}
            >
              <div className="search-mode-icon-box" aria-hidden="true">
                <FileText size={14} />
              </div>
              <div className="search-mode-content">
                <div className="search-mode-header">
                  <span className="search-mode-title">{t('search.modeContentOnly', 'Content Only (Full-Text)')}</span>
                  <span className={`search-mode-radio ${searchMode === 'content' ? 'search-mode-radio--checked' : ''}`} aria-hidden="true" />
                </div>
                <div className="search-mode-desc">
                  {t('search.modeContentOnlyDesc', 'Full-text search inside documents, spreadsheets, PDFs, CAD and code files')}
                </div>
              </div>
            </button>
          </div>
        </div>

        {/* 2. 布局密度 */}
        <div className="popover-section">
          <div className="popover-section-title">
            <span>{t('search.density', 'List Display Density')}</span>
          </div>
          <div className="popover-segmented-control">
            <button
              type="button"
              className={`popover-segment ${density === 'compact' ? 'popover-segment--active' : ''}`}
              onClick={() => onChangeDensity('compact')}
            >
              {t('search.densityCompact', 'Compact')}
            </button>
            <button
              type="button"
              className={`popover-segment ${density === 'standard' ? 'popover-segment--active' : ''}`}
              onClick={() => onChangeDensity('standard')}
            >
              {t('search.densityStandard', 'Standard')}
            </button>
            <button
              type="button"
              className={`popover-segment ${density === 'comfortable' ? 'popover-segment--active' : ''}`}
              onClick={() => onChangeDensity('comfortable')}
            >
              {t('search.densityComfortable', 'Comfortable')}
            </button>
          </div>
        </div>

        {/* 1.1 图标风格 */}
        <div className="popover-section">
          <div className="popover-section-title">
            <span>{t('search.iconStyle', 'Icon Display Style')}</span>
          </div>
          <div className="popover-segmented-control">
            <button
              type="button"
              className={`popover-segment ${iconStyle === 'native' ? 'popover-segment--active' : ''}`}
              onClick={() => onChangeIconStyle('native')}
            >
              {t('search.iconStyleNative', 'Windows Native')}
            </button>
            <button
              type="button"
              className={`popover-segment ${iconStyle === 'vector' ? 'popover-segment--active' : ''}`}
              onClick={() => onChangeIconStyle('vector')}
            >
              {t('search.iconStyleVector', 'Minimalist Vector')}
            </button>
          </div>
        </div>

        {/* 2. 检索结果数量上限 (支持全部返回) */}
        <div className="popover-section">
          <div className="popover-section-title">
            <span>{t('search.maxResults')}</span>
          </div>
          <div className="popover-segmented-control">
            {[50, 100, 200, 0].map((lim) => (
              <button
                key={lim}
                type="button"
                className={`popover-segment ${maxResultLimit === lim ? 'popover-segment--active' : ''}`}
                onClick={() => onChangeMaxResultLimit(lim)}
              >
                {lim === 0 ? t('search.maxResultsAll') : (lim === 50 ? t('search.maxResults50') : lim === 100 ? t('search.maxResults100') : t('search.maxResults200'))}
              </button>
            ))}
          </div>
        </div>

        {/* 2. 窗口尺寸与位置管理 */}
        <div className="popover-section">
          <div className="popover-section-title">
            <span className="popover-section-title-left">
              <span>{t('search.windowSizePlacement', 'Window Size & Placement')}</span>
              <span className="popover-metric-pill">
                {isMaximized ? t('search.maximizedBadge', 'Full Workspace') : `${windowSize.width} × ${windowSize.height}`}
              </span>
            </span>
            <button
              type="button"
              className="popover-mini-link"
              onClick={() => {
                void bridgeRequest('search.resetPlacement');
                onSetWindowSize({ width: 760, height: 520 });
                toast.success(t('search.placementReset', 'Window placement and size reset to default'));
              }}
              title={t('search.resetPlacementBtn', 'Reset to Default Center (760×520)')}
            >
              <RotateCcw size={11} style={{ marginRight: 3, verticalAlign: -1 }} />
              {t('search.resetPlacementBtn', 'Reset to Default Center (760×520)')}
            </button>
          </div>
          {onToggleMaximized && (
            <button
              type="button"
              className={`popover-rebuild-btn popover-maximize-btn ${isMaximized ? 'popover-preset-btn--active' : ''}`}
              onClick={onToggleMaximized}
              style={{ marginBottom: 8 }}
            >
              {isMaximized ? <Minimize2 size={13} /> : <Maximize2 size={13} />}
              <span>
                {isMaximized
                  ? t('search.restoreWindowBtn', 'Restore Floating Window (F11)')
                  : t('search.maximizeWindowBtn', 'Maximize to Full Workspace (F11)')}
              </span>
            </button>
          )}
          <div className="popover-presets-grid">
            {WINDOW_PRESETS.map((preset) => {
              const isMatch = !isMaximized && Math.abs(windowSize.width - preset.width) <= 20 && Math.abs(windowSize.height - preset.height) <= 20;
              const presetKeyMap: Record<string, 'search.presetStandard' | 'search.presetWide' | 'search.presetLarge' | 'search.presetExtra'> = {
                standard: 'search.presetStandard',
                wide: 'search.presetWide',
                large: 'search.presetLarge',
                extra: 'search.presetExtra',
              };
              const presetKey = presetKeyMap[preset.id];
              const rawLabel = presetKey ? t(presetKey, preset.label) : preset.label;
              const shortLabel = rawLabel.replace(/\s*\([^)]*\)\s*$/, '').trim() || rawLabel;
              return (
                <button
                  key={preset.id}
                  type="button"
                  className={`popover-preset-btn ${isMatch ? 'popover-preset-btn--active' : ''}`}
                  onClick={() => {
                    onSetWindowSize({ width: preset.width, height: preset.height });
                    void bridgeRequest('search.setWindowSize', {
                      width: preset.width,
                      height: preset.height,
                      center: true,
                    });
                    toast.success(t('search.presetAppliedToast', 'Applied {{label}} preset ({{width}}×{{height}})', { label: shortLabel, width: preset.width, height: preset.height }));
                  }}
                  title={t('search.applyPresetTooltip', 'Resize window to {{width}}×{{height}}', { width: preset.width, height: preset.height })}
                >
                  <div className="preset-name">{shortLabel}</div>
                  <div className="preset-dim">{preset.width} × {preset.height}</div>
                </button>
              );
            })}
          </div>
        </div>

        {/* 3. 表头核心列与全文摘要行开关（与表头列单一事实源联动） */}
        <div className="popover-section">
          <div className="popover-section-title">
            <span>{t('search.visibleColumns', 'Visible Columns & Layout')}</span>
            {onOpenColumnManager && (
              <button
                type="button"
                className="popover-mini-link"
                onClick={() => {
                  onClose();
                  onOpenColumnManager();
                }}
              >
                <Columns3 size={11} style={{ marginRight: 3, verticalAlign: -1 }} aria-hidden="true" />
                {t('search.organizeColumnsAction', 'Organize Columns...')}
              </button>
            )}
          </div>
          <div className="popover-cols-grid">
            {columns.map((col) => (
              <label key={col.id} className="popover-col-checkbox-label">
                <input
                  type="checkbox"
                  checked={col.visible}
                  disabled={col.id === 'name'}
                  onChange={() => onToggleColumn(col.id)}
                />
                <span>{col.id === 'name' ? t('search.colName', 'File Name') :
                       col.id === 'path' ? t('search.colDirectory', 'Directory') :
                       col.id === 'size' ? t('search.colSize', 'File Size') :
                       col.id === 'modified' ? t('search.colModified', 'Modified Time') :
                       col.id === 'created' ? t('search.colCreated', 'Created Time') :
                       col.id === 'snippets' ? t('search.colSnippets', 'Content Snippets') : col.label}</span>
              </label>
            ))}
          </div>
        </div>

        {/* 4. 名称与路径列宽占比 */}
        <div className="popover-section">
          <div className="popover-section-title">
            <span>{t('search.namePathRatio', 'Name & Path Width Ratio')}</span>
            <span className="popover-metric-pill">{nameFlex}% : {pathFlex}%</span>
          </div>
          <div className="popover-slider-row">
            <span className="slider-label">{t('search.narrow', 'Narrow')}</span>
            <input
              type="range"
              min="15"
              max="65"
              step="1"
              value={nameFlex}
              onChange={(e) => onUpdateNameAndPathFlex(parseInt(e.target.value, 10))}
              className="popover-ratio-slider"
            />
            <span className="slider-label">{t('search.wide', 'Wide')}</span>
          </div>
        </div>

        {/* 5. 搜索驱动器与磁盘位置 */}
        {systemDrives.length > 0 && (
          <div className="popover-section">
            <div className="popover-section-title">
              <span>{t('search.searchDrives', 'Search Drives & Locations')}</span>
              <div className="popover-drives-actions">
                <button
                  type="button"
                  className="popover-mini-link"
                  onClick={onSelectAllDrives}
                >
                  {t('search.selectAll', 'Select All')}
                </button>
                <span className="popover-action-divider">/</span>
                <button
                  type="button"
                  className="popover-mini-link"
                  onClick={onDeselectAllDrives}
                >
                  {t('search.deselectAll', 'Deselect All')}
                </button>
              </div>
            </div>
            <div className="popover-drives-grid">
              {systemDrives.map((drv) => {
                const isChecked = enabledDrives.includes(drv.letter);
                const isRemote = drv.type === 'remote';
                const isRemovable = drv.type === 'removable';
                const totalStr = drv.totalBytes > 0 ? formatBytes(drv.totalBytes) : '';
                const freeStr = drv.freeBytes > 0 ? t('search.freeSpace', '(Free {{size}})', { size: formatBytes(drv.freeBytes) }) : '';
                const label = drv.volumeLabel ? `${drv.volumeLabel} (${drv.letter}:)` : (isRemote ? t('search.remoteDrive', 'Network Drive ({{letter}}:)', { letter: drv.letter }) : t('search.localDrive', 'Local Disk ({{letter}}:)', { letter: drv.letter }));

                return (
                  <div
                    key={drv.letter}
                    className={`popover-drive-card ${isChecked ? 'popover-drive-card--active' : ''}`}
                    onClick={() => onToggleDrive(drv.letter)}
                  >
                    <input
                      type="checkbox"
                      checked={isChecked}
                      onChange={() => {}}
                      className="drive-card-checkbox"
                      aria-label={t('search.enableDriveAria', 'Enable Drive {{letter}}', { letter: drv.letter })}
                    />
                    <div className="drive-card-icon">
                      {isRemote ? <Network size={15} /> : isRemovable ? <Disc size={15} /> : <HardDrive size={15} />}
                    </div>
                    <div className="drive-card-details">
                      <div className="drive-card-title-row">
                        <span className="drive-card-title">{label}</span>
                        <span className="drive-card-tag">{drv.fileSystem || (isRemote ? t('search.remoteShare', 'Network Share') : t('search.localDisk', 'Local'))}</span>
                      </div>
                      <div className="drive-card-meta">
                        {totalStr} {freeStr}
                      </div>
                    </div>
                  </div>
                );
              })}
            </div>
          </div>
        )}

        {/* 6. 排除规则 */}
        <div className="popover-section">
          <div className="popover-section-title">
            <span>{t('search.excludeRules', 'Exclusion Rules')}</span>
          </div>
          <div className="popover-exclude-options">
            <label className="popover-col-checkbox-label">
              <input
                type="checkbox"
                checked={excludeGitAndModules}
                onChange={(e) => onSetExcludeGitAndModules(e.target.checked)}
              />
              <span>{t('search.excludeDevTrash', 'Exclude dev dependencies & Recycle Bin (node_modules, .git, $Recycle.Bin)')}</span>
            </label>
            <label className="popover-col-checkbox-label">
              <input
                type="checkbox"
                checked={excludeHidden}
                onChange={(e) => onSetExcludeHidden(e.target.checked)}
              />
              <span>{t('search.excludeHiddenProtected', 'Exclude hidden & protected system files')}</span>
            </label>
          </div>
        </div>

        {/* 7. 文档内容检索格式定制 (折叠手风琴 + 纯净单层微刻芯片，彻底告别双层蓝框轰炸) */}
        <div className="popover-section">
          <div className="popover-section-title">
            <div className="popover-section-title-left">
              <span>{t('search.contentFormats', 'Supported Content Search Formats')}</span>
              <CodeBadge className="popover-title-badge">content:</CodeBadge>
            </div>
            <button
              type="button"
              className="popover-quick-action"
              onClick={onResetContentFormats}
              title={t('search.resetDefaultFormats', 'Restore Default Formats')}
            >
              <RotateCcw size={11} />
              <span>{t('search.resetDefault')}</span>
            </button>
          </div>

          <div className="popover-format-categories">
            {CONTENT_FORMAT_CATEGORIES.map(cat => {
              const enabledCount = cat.extensions.filter(ext => !disabledContentFormats.includes(ext)).length;
              const isExpanded = Boolean(expandedFormatCats[cat.id]);
              return (
                <div key={cat.id} className={`popover-format-cat-block ${isExpanded ? 'popover-format-cat-block--open' : ''}`}>
                  <div className="popover-format-cat-header">
                    <button
                      type="button"
                      className="popover-format-cat-trigger"
                      aria-expanded={isExpanded}
                      onClick={() => toggleFormatCatExpand(cat.id)}
                    >
                      <ChevronRight
                        size={13}
                        className={`popover-format-chevron ${isExpanded ? 'popover-format-chevron--open' : ''}`}
                        aria-hidden="true"
                      />
                      <div className={`format-cat-icon-badge format-cat-icon-badge--${cat.iconName}`} aria-hidden="true">
                        {cat.iconName === 'code' && <Code2 size={12} />}
                        {cat.iconName === 'doc' && <FileText size={12} />}
                        {cat.iconName === 'config' && <SlidersHorizontal size={12} />}
                        {cat.iconName === 'design' && <Palette size={12} />}
                      </div>
                      <span className="popover-format-cat-name">{t(cat.nameKey as unknown as 'search.catAll')}</span>
                      <span className="popover-format-badge-count">{enabledCount} / {cat.extensions.length}</span>
                    </button>
                    <Toggle
                      id={`format-cat-toggle-${cat.id}`}
                      checked={enabledCount > 0}
                      size="sm"
                      onChange={(checked) => onToggleCategoryFormats(cat, checked)}
                    />
                  </div>
                  {isExpanded && (
                    <div className="popover-format-chips">
                      {cat.extensions.map(ext => {
                        const isEnabled = !disabledContentFormats.includes(ext);
                        return (
                          <button
                            key={ext}
                            type="button"
                            className={`format-chip ${isEnabled ? 'format-chip--enabled' : 'format-chip--disabled'}`}
                            onClick={() => onToggleContentFormat(ext)}
                            title={isEnabled ? t('search.disableExtTip', 'Click to disable .{{ext}}', { ext }) : t('search.enableExtTip', 'Click to enable .{{ext}}', { ext })}
                          >
                            <span className="format-chip-dot" aria-hidden="true" />
                            <span>.{ext}</span>
                          </button>
                        );
                      })}
                    </div>
                  )}
                </div>
              );
            })}

            {/* 用户自定义格式列表 */}
            <div className="popover-format-cat-block popover-format-cat-block--custom">
              <div className="popover-format-cat-header">
                <div className="popover-format-cat-title-wrap">
                  <div className="format-cat-icon-badge format-cat-icon-badge--custom" aria-hidden="true">
                    <Sparkles size={12} />
                  </div>
                  <span className="popover-format-cat-name">{t('search.customFormats')}</span>
                  <span className="popover-format-badge-count">{t('search.customFormatsCount', '{{count}} items', { count: customContentFormats.length })}</span>
                </div>
                {customContentFormats.length > 0 && (
                  <Toggle
                    id="format-cat-toggle-custom"
                    checked={customContentFormats.some(ext => !disabledContentFormats.includes(ext))}
                    size="sm"
                    onChange={(checked) => {
                      onToggleCategoryFormats(
                        { id: 'custom', nameKey: 'search.catCode', iconName: 'code', extensions: customContentFormats },
                        checked,
                      );
                    }}
                  />
                )}
              </div>
              {customContentFormats.length > 0 && (
                <div className="popover-format-chips">
                  {customContentFormats.map(ext => {
                    const isEnabled = !disabledContentFormats.includes(ext);
                    return (
                      <div
                        key={ext}
                        className={`format-chip format-chip--custom ${isEnabled ? 'format-chip--enabled' : 'format-chip--disabled'}`}
                      >
                        <span onClick={() => onToggleContentFormat(ext)} className="format-chip-label">
                          <span className="format-chip-dot" aria-hidden="true" />
                          <span>.{ext}</span>
                        </span>
                        <button
                          type="button"
                          className="format-chip-del"
                          onClick={(e) => {
                            e.stopPropagation();
                            onRemoveCustomContentFormat(ext);
                          }}
                          title={t('search.deleteCustomExt', 'Delete this custom format')}
                          aria-label={t('search.deleteCustomExt', 'Delete this custom format')}
                        >
                          <Trash2 size={10} aria-hidden="true" />
                        </button>
                      </div>
                    );
                  })}
                </div>
              )}

              <div className="popover-format-add-row">
                <input
                  type="text"
                  className="popover-format-add-input"
                  placeholder={t('search.formatInputPlaceholder', 'Add extensions separated by commas/spaces (e.g. ps1, ch, log2)...')}
                  value={newFormatInput}
                  onChange={(e) => onSetNewFormatInput(e.target.value)}
                  onKeyDown={(e) => {
                    e.stopPropagation();
                    if (e.key === 'Enter') {
                      e.preventDefault();
                      onAddCustomContentFormat(newFormatInput);
                    }
                  }}
                />
                <button
                  type="button"
                  className="popover-format-add-btn"
                  onClick={() => onAddCustomContentFormat(newFormatInput)}
                  title={t('search.addCustomExtTip', 'Add custom format (supports comma/space bulk adding)')}
                >
                  <Plus size={13} />
                  <span>{t('search.addBtn', 'Add')}</span>
                </button>
              </div>
              <div className="popover-format-add-tip">
                <Lightbulb size={12} className="popover-format-add-tip-icon" />
                <span>{t('search.formatInputHint', 'Supports batch adding multiple extensions separated by commas or spaces, press Enter to add')}</span>
              </div>
            </div>
          </div>
        </div>

        {/* 8. 索引数据库快照与缓存状态 */}
        <div className="popover-section">
          <div className="popover-section-title">
            <div className="popover-section-title-left">
              <HardDrive size={13} className="popover-title-icon" />
              <span>{t('search.dbSnapshot', 'Disk Snapshot & Database')}</span>
              <CodeBadge className="popover-title-badge">Tools3000.db</CodeBadge>
            </div>
          </div>
          
          <div className="popover-db-card">
            <div className="popover-db-row">
              <span className="popover-db-label">{t('search.snapshotStatus', 'Snapshot Status:')}</span>
              <span className="popover-db-val">
                {dbStats?.exists ? (
                  <span className="popover-db-status--ok">{t('search.snapshotPersisted', 'Persisted')} ({formatBytes(dbStats.dbSize)})</span>
                ) : (
                  <span className="popover-db-status--empty">{t('search.snapshotReady', 'Ready (Auto-persisted on exit or idle)')}</span>
                )}
              </span>
            </div>
            <div className="popover-db-row">
              <span className="popover-db-label">{t('search.totalRecords', 'Indexed Records:')}</span>
              <span className="popover-db-val">{t('search.maxResultsCount', { defaultValue: '{{count}} items', count: Number(dbStats?.totalRecords ?? totalIndexedFiles ?? 0) })}</span>
            </div>
          </div>
        </div>
      </div>

      {/* 底部一键恢复所有视图与搜索偏好固定操作栏 */}
      <div className="popover-footer">
        <button
          type="button"
          className="popover-reset-all-btn"
          onClick={onResetAllViewPreferences}
          title={t('search.resetViewPrefTooltip', 'Reset density, column layout, drive filters, format extensions to factory defaults')}
        >
          <RotateCcw size={13} />
          <span>{t('search.resetViewPrefBtn', 'Reset All View Preferences')}</span>
        </button>
      </div>
    </div>
  );
};

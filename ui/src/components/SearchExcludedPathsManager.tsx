/* ─────────────────────────────────────────────────────────────────────────────
 * SearchExcludedPathsManager — 多路径自定义排除文件夹管理器
 *
 * 复用宿主：
 *   1. SearchPage.tsx (设置中心 -> 搜索设置 -> 排除文件夹与隐私屏蔽)
 *   2. SearchViewSettingsModal.tsx (搜索浮窗 -> 视图与搜索偏好 -> 排除规则)
 *
 * 核心特性：
 *   - 接入原生目录选择器 (search.browseExcludeDirectory) 与多路径批量粘贴 (; / 换行)
 *   - 严格路径归一化与大小写不敏感去重，上限 48 条
 *   - 与后端 PathExclusionMatcher 精确路径边界对齐，全链路覆盖文件名/全文/CLI/MCP
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, type FC, type KeyboardEvent } from 'react';
import { FolderPlus, FolderX, Plus, ShieldCheck, Trash2, X } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { bridgeRequest } from '../hooks/useBridge';
import {
  MAX_CUSTOM_EXCLUDED_PATHS,
  mergeExcludedPaths,
  normalizeExcludedPath,
  parseExcludedPathInput,
} from '../search/excludedPathsUtils';
import { CodeBadge } from './UIKit';
import './SearchExcludedPathsManager.css';

export interface SearchExcludedPathsManagerProps {
  paths: string[];
  onChange: (nextPaths: string[]) => void;
  compact?: boolean;
  disabled?: boolean;
  onBrowse?: () => Promise<string | null>;
}

export const SearchExcludedPathsManager: FC<SearchExcludedPathsManagerProps> = ({
  paths,
  onChange,
  compact = false,
  disabled = false,
  onBrowse,
}) => {
  const { t } = useTranslation();
  const [draft, setDraft] = useState('');
  const [browsing, setBrowsing] = useState(false);

  const commitAdditions = (candidates: string[]) => {
    if (candidates.length === 0) return;
    const merged = mergeExcludedPaths(paths, candidates, MAX_CUSTOM_EXCLUDED_PATHS);
    if (merged.added.length > 0) {
      onChange(merged.paths);
      if (merged.added.length === 1) {
        toast.success(
          t('search.customExcludedAddedSingle', 'Added excluded folder: {{path}}', {
            path: merged.added[0],
          }),
        );
      } else {
        toast.success(
          t('search.customExcludedAddedMulti', 'Added {{count}} excluded folders', {
            count: merged.added.length,
          }),
        );
      }
    } else if (merged.duplicateCount > 0) {
      toast.info(t('search.customExcludedDuplicate', 'This folder is already in the exclusion list'));
    }

    if (merged.capped) {
      toast.warning(
        t('search.customExcludedLimitReached', 'Maximum of {{max}} excluded folders reached', {
          max: MAX_CUSTOM_EXCLUDED_PATHS,
        }),
      );
    }
  };

  const handleAddDraft = () => {
    if (disabled) return;
    const parsed = parseExcludedPathInput(draft);
    if (parsed.length === 0) return;
    commitAdditions(parsed);
    setDraft('');
  };

  const handleBrowseFolder = async () => {
    if (disabled || browsing) return;
    setBrowsing(true);
    try {
      const selected = onBrowse
        ? await onBrowse()
        : await bridgeRequest<string | null>('search.browseExcludeDirectory');
      if (typeof selected === 'string' && selected.trim().length > 0) {
        const normalized = normalizeExcludedPath(selected);
        if (normalized) {
          commitAdditions([normalized]);
        }
      }
    } catch {
      // 错误提示由 bridgeRequest 统一处理
    } finally {
      setBrowsing(false);
    }
  };

  const handleRemovePath = (target: string) => {
    if (disabled) return;
    const targetKey = target.toLowerCase();
    const next = paths.filter((item) => item.toLowerCase() !== targetKey);
    onChange(next);
  };

  const handleClearAll = () => {
    if (disabled || paths.length === 0) return;
    onChange([]);
    toast.success(t('search.customExcludedCleared', 'Cleared all custom excluded folders'));
  };

  const handleInputKeyDown = (e: KeyboardEvent<HTMLInputElement>) => {
    e.stopPropagation();
    if (e.key === 'Enter') {
      e.preventDefault();
      handleAddDraft();
    }
  };

  return (
    <div
      className={`excluded-paths-mgr${compact ? ' excluded-paths-mgr--compact' : ''}`}
      data-testid="search-excluded-paths-manager"
    >
      <div className="excluded-paths-mgr__header">
        <div className="excluded-paths-mgr__title-row">
          <FolderX size={14} className="excluded-paths-mgr__title-icon" aria-hidden="true" />
          <span className="excluded-paths-mgr__title">
            {t('search.customExcludedPathsTitle', 'Custom Excluded Folders')}
          </span>
          <span className="excluded-paths-mgr__count-pill">
            {paths.length} / {MAX_CUSTOM_EXCLUDED_PATHS}
          </span>
        </div>
        {paths.length > 1 && (
          <button
            type="button"
            className="excluded-paths-mgr__clear-btn"
            disabled={disabled}
            onClick={handleClearAll}
            title={t('search.customExcludedClearAll', 'Clear All')}
          >
            <Trash2 size={12} aria-hidden="true" />
            <span>{t('search.customExcludedClearAll', 'Clear All')}</span>
          </button>
        )}
      </div>

      <p className="excluded-paths-mgr__desc">
        {t(
          'search.customExcludedPathsDesc',
          'Hide specified folders and all nested files from filename, content, and CLI/MCP search using exact path-boundary matching.',
        )}
      </p>

      <div className="excluded-paths-mgr__controls">
        <button
          type="button"
          className={`excluded-paths-mgr__browse-btn${browsing ? ' excluded-paths-mgr__browse-btn--loading' : ''}`}
          disabled={disabled || browsing || paths.length >= MAX_CUSTOM_EXCLUDED_PATHS}
          onClick={() => void handleBrowseFolder()}
          title={t('search.customExcludedBrowseTip', 'Choose a folder from your computer to exclude')}
        >
          <FolderPlus size={14} aria-hidden="true" />
          <span>{t('search.customExcludedBrowseBtn', 'Select Folder')}</span>
        </button>

        <div className="excluded-paths-mgr__input-wrap">
          <input
            type="text"
            className="excluded-paths-mgr__input"
            value={draft}
            disabled={disabled || paths.length >= MAX_CUSTOM_EXCLUDED_PATHS}
            onChange={(e) => setDraft(e.target.value)}
            onKeyDown={handleInputKeyDown}
            placeholder={t(
              'search.customExcludedInputPlaceholder',
              'Type or paste folder paths (separate multiple with ;), e.g. D:\\Private',
            )}
            aria-label={t('search.customExcludedInputAria', 'Folder path to exclude')}
          />
          <button
            type="button"
            className="excluded-paths-mgr__add-btn"
            disabled={disabled || !draft.trim() || paths.length >= MAX_CUSTOM_EXCLUDED_PATHS}
            onClick={handleAddDraft}
            title={t('search.customExcludedAddBtn', 'Add')}
          >
            <Plus size={13} aria-hidden="true" />
            <span>{t('search.customExcludedAddBtn', 'Add')}</span>
          </button>
        </div>
      </div>

      {paths.length > 0 ? (
        <ul
          className="excluded-paths-mgr__list"
          aria-label={t('search.customExcludedPathsTitle', 'Custom Excluded Folders')}
        >
          {paths.map((folderPath) => (
            <li key={folderPath.toLowerCase()} className="excluded-paths-mgr__item">
              <FolderX size={13} className="excluded-paths-mgr__item-icon" aria-hidden="true" />
              <div className="excluded-paths-mgr__item-path" title={folderPath}>
                <CodeBadge className="excluded-paths-mgr__code-badge">{folderPath}</CodeBadge>
              </div>
              <button
                type="button"
                className="excluded-paths-mgr__remove-btn"
                disabled={disabled}
                onClick={() => handleRemovePath(folderPath)}
                title={t('search.customExcludedRemoveTip', 'Remove excluded folder')}
                aria-label={`${t('search.customExcludedRemoveTip', 'Remove excluded folder')}: ${folderPath}`}
              >
                <X size={13} aria-hidden="true" />
              </button>
            </li>
          ))}
        </ul>
      ) : (
        <div className="excluded-paths-mgr__empty" role="note">
          <ShieldCheck size={14} className="excluded-paths-mgr__empty-icon" aria-hidden="true" />
          <span>
            {t(
              'search.customExcludedEmptyHint',
              'No custom excluded folders yet. You can select folders here or right-click any search result to hide its folder in one click.',
            )}
          </span>
        </div>
      )}
    </div>
  );
};

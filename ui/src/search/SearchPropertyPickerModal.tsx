import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Check,
  FileText,
  Film,
  Folder,
  Hash,
  Image as ImageIcon,
  Layers,
  Music,
  Plus,
  Search,
  Terminal,
  Wrench,
  X,
} from 'lucide-react';
import {
  BUILTIN_TABLE_COLUMNS,
  getColumnDisplayLabel,
  isValidCustomWindowsPropertyId,
  PROPERTY_CATEGORIES,
  resolveColumnDefinition,
  type PropertyCategoryId,
  type TableColumnDefinition,
} from './columnCatalog';
import type { TableColumnId } from './searchTypes';

export interface SearchPropertyPickerModalProps {
  visible: boolean;
  existingColumns: readonly TableColumnId[];
  onClose: () => void;
  onAddColumns: (colIds: TableColumnId[]) => void;
}

function renderCategoryIcon(catId: PropertyCategoryId) {
  switch (catId) {
    case 'all': return <Layers size={13} aria-hidden="true" />;
    case 'file': return <Folder size={13} aria-hidden="true" />;
    case 'image': return <ImageIcon size={13} aria-hidden="true" />;
    case 'audio': return <Music size={13} aria-hidden="true" />;
    case 'video': return <Film size={13} aria-hidden="true" />;
    case 'document': return <FileText size={13} aria-hidden="true" />;
    case 'program': return <Terminal size={13} aria-hidden="true" />;
    case 'hash': return <Hash size={13} aria-hidden="true" />;
    case 'windows': return <Wrench size={13} aria-hidden="true" />;
  }
}

export const SearchPropertyPickerModal: React.FC<SearchPropertyPickerModalProps> = ({
  visible,
  existingColumns,
  onClose,
  onAddColumns,
}) => {
  const { t } = useTranslation();
  const [activeCategory, setActiveCategory] = useState<PropertyCategoryId>('all');
  const [filterText, setFilterText] = useState('');
  const [selectedIds, setSelectedIds] = useState<Set<TableColumnId>>(new Set());
  const [wasVisible, setWasVisible] = useState(visible);
  const searchInputRef = useRef<HTMLInputElement>(null);

  const translateLabel = useCallback(
    (key: string, defaultVal?: string) => t(key as never, defaultVal as never) as unknown as string,
    [t],
  );

  if (wasVisible !== visible) {
    setWasVisible(visible);
    if (visible) {
      setFilterText('');
      setSelectedIds(new Set());
    }
  }

  useEffect(() => {
    if (!visible) return;
    const timer = window.setTimeout(() => searchInputRef.current?.focus(), 30);
    return () => window.clearTimeout(timer);
  }, [visible]);

  const existingSet = useMemo(() => new Set(existingColumns), [existingColumns]);

  const categoryCounts = useMemo(() => {
    const counts: Record<PropertyCategoryId, number> = {
      all: BUILTIN_TABLE_COLUMNS.length,
      file: 0,
      image: 0,
      audio: 0,
      video: 0,
      document: 0,
      program: 0,
      hash: 0,
      windows: 0,
    };
    for (const col of BUILTIN_TABLE_COLUMNS) {
      counts[col.category] += 1;
    }
    return counts;
  }, []);

  const filteredColumns = useMemo<TableColumnDefinition[]>(() => {
    const keyword = filterText.trim().toLowerCase();
    const list: TableColumnDefinition[] = [];
    for (const col of BUILTIN_TABLE_COLUMNS) {
      if (activeCategory !== 'all' && col.category !== activeCategory) continue;
      if (keyword) {
        const displayName = getColumnDisplayLabel(col.id, translateLabel).toLowerCase();
        const defaultName = col.defaultLabel.toLowerCase();
        const canonical = col.canonicalName.toLowerCase();
        const idLower = col.id.toLowerCase();
        if (
          !displayName.includes(keyword) &&
          !defaultName.includes(keyword) &&
          !canonical.includes(keyword) &&
          !idLower.includes(keyword)
        ) {
          continue;
        }
      }
      list.push(col);
    }

    // 支持直接键入任意合法 Windows Property System 规范名（如 System.Photo.Aperture）动态添加
    const rawTrimmed = filterText.trim();
    if (
      isValidCustomWindowsPropertyId(rawTrimmed) &&
      !list.some((c) => c.id.toLowerCase() === rawTrimmed.toLowerCase())
    ) {
      list.unshift(resolveColumnDefinition(rawTrimmed));
    }
    return list;
  }, [activeCategory, filterText, translateLabel]);

  if (!visible) return null;

  const toggleSelect = (colId: TableColumnId) => {
    if (existingSet.has(colId)) return;
    setSelectedIds((prev) => {
      const next = new Set(prev);
      if (next.has(colId)) next.delete(colId);
      else next.add(colId);
      return next;
    });
  };

  const commitSelected = () => {
    const toAdd = Array.from(selectedIds).filter((id) => !existingSet.has(id));
    if (toAdd.length > 0) {
      onAddColumns(toAdd);
    }
    onClose();
  };

  const getCategoryLabel = (catId: string): string => {
    const meta = PROPERTY_CATEGORIES.find((c) => c.id === catId);
    return meta ? translateLabel(meta.labelKey, meta.defaultLabel) : catId;
  };

  return (
    <div
      className="search-modal-overlay"
      role="dialog"
      aria-modal="true"
      aria-label={t('search.selectPropertyTitle', 'Select Property Columns')}
      onMouseDown={(e) => {
        if (e.target === e.currentTarget) onClose();
      }}
    >
      <div className="search-col-modal-card search-col-modal-card--picker">
        <div className="search-rename-header">
          <div className="search-rename-title">
            <Plus size={15} className="search-rename-icon" aria-hidden="true" />
            <span>{t('search.selectPropertyTitle', 'Select Property Columns')}</span>
          </div>
          <button
            type="button"
            className="search-rename-close"
            onClick={onClose}
            title={t('search.close', 'Close')}
            aria-label={t('search.close', 'Close')}
          >
            <X size={15} aria-hidden="true" />
          </button>
        </div>

        <div className="search-prop-picker-body">
          {/* 左侧分类导航树 */}
          <nav className="search-prop-picker-sidebar" aria-label={t('search.propertyCategoriesAria', 'Property Categories')}>
            {PROPERTY_CATEGORIES.map((cat) => {
              const active = activeCategory === cat.id;
              return (
                <button
                  key={cat.id}
                  type="button"
                  className={`search-prop-cat-btn ${active ? 'is-active' : ''}`}
                  onClick={() => setActiveCategory(cat.id)}
                >
                  <span className="search-prop-cat-left">
                    {renderCategoryIcon(cat.id)}
                    <span>{translateLabel(cat.labelKey, cat.defaultLabel)}</span>
                  </span>
                  <span className="search-prop-cat-count">{categoryCounts[cat.id]}</span>
                </button>
              );
            })}
          </nav>

          {/* 右侧搜索框 + 属性表格 */}
          <div className="search-prop-picker-main">
            <div className="search-prop-search-box">
              <Search size={14} className="search-prop-search-icon" aria-hidden="true" />
              <input
                ref={searchInputRef}
                type="text"
                className="search-prop-search-input"
                value={filterText}
                onChange={(e) => setFilterText(e.target.value)}
                placeholder={t('search.selectPropertySearchPlaceholder', 'Filter by name or enter Windows property (e.g. System.Photo.FNumber)...')}
              />
              {filterText && (
                <button
                  type="button"
                  className="search-prop-search-clear"
                  onClick={() => setFilterText('')}
                  aria-label={t('search.clearInput', 'Clear')}
                >
                  <X size={13} />
                </button>
              )}
            </div>

            <div className="search-prop-table-head">
              <span>{t('search.propColName', 'Name')}</span>
              <span>{t('search.propColType', 'Category')}</span>
              <span>{t('search.propColCanonical', 'Canonical Name')}</span>
            </div>

            <div className="search-prop-table-body" role="listbox" aria-multiselectable="true">
              {filteredColumns.length === 0 ? (
                <div className="search-prop-empty">
                  {t('search.noMatchingProperties', 'No matching properties found')}
                </div>
              ) : (
                filteredColumns.map((col) => {
                  const alreadyAdded = existingSet.has(col.id);
                  const isSelected = selectedIds.has(col.id);
                  const displayLabel = getColumnDisplayLabel(col.id, translateLabel);
                  return (
                    <button
                      key={col.id}
                      type="button"
                      role="option"
                      aria-selected={isSelected || alreadyAdded}
                      disabled={alreadyAdded}
                      className={`search-prop-row ${isSelected ? 'is-selected' : ''} ${alreadyAdded ? 'is-added' : ''}`}
                      onClick={() => toggleSelect(col.id)}
                      onDoubleClick={() => {
                        if (!alreadyAdded) {
                          onAddColumns([col.id]);
                          onClose();
                        }
                      }}
                    >
                      <span className="search-prop-row-name">
                        <span className={`search-table-header-menu-check ${(isSelected || alreadyAdded) ? 'is-checked' : ''}`}>
                          {(isSelected || alreadyAdded) && <Check size={11} aria-hidden="true" />}
                        </span>
                        <span className="search-prop-row-text">{displayLabel}</span>
                        {alreadyAdded && (
                          <span className="search-prop-added-tag">{t('search.propAlreadyAdded', 'Added')}</span>
                        )}
                      </span>
                      <span className="search-prop-row-cat">{getCategoryLabel(col.category)}</span>
                      <span className="search-prop-row-canonical">{col.canonicalName}</span>
                    </button>
                  );
                })
              )}
            </div>
          </div>
        </div>

        <div className="search-rename-footer search-prop-picker-footer">
          <span className="search-prop-picker-hint">
            {t('search.selectPropertyLazyTip', 'Extended columns load on demand for visible rows only, keeping full-disk search instant.')}
          </span>
          <div className="search-prop-picker-footer-btns">
            <button
              type="button"
              className="search-rename-btn search-rename-btn--cancel"
              onClick={onClose}
            >
              {t('search.cancelBtn', 'Cancel')}
            </button>
            <button
              type="button"
              className="search-rename-btn search-rename-btn--confirm"
              disabled={selectedIds.size === 0}
              onClick={commitSelected}
            >
              {selectedIds.size > 0
                ? t('search.addSelectedColumnsCount', 'Add ({{count}})', { count: selectedIds.size })
                : t('search.confirmBtn', 'OK')}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
};

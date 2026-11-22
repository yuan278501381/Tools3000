import React, { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  ArrowDown,
  ArrowUp,
  Columns3,
  Lock,
  Plus,
  RotateCcw,
  Trash2,
  X,
} from 'lucide-react';
import {
  DEFAULT_VISIBLE_TABLE_COLUMNS,
  getColumnDisplayLabel,
  PROPERTY_CATEGORIES,
  resolveColumnDefinition,
} from './columnCatalog';
import type { TableColumnId } from './searchTypes';

export interface SearchColumnManagerModalProps {
  visible: boolean;
  /** 外部生效中的可见列集合（当未传入 draftColumns 时作为初始草稿源）。 */
  activeColumns: readonly TableColumnId[];
  /** 可选受控草稿列集合（供父组件与属性选择器弹窗共享严格草稿事务）。 */
  draftColumns?: readonly TableColumnId[];
  onChangeDraft?: (nextDraft: TableColumnId[]) => void;
  onMarkResetDraft?: () => void;
  onClose: () => void;
  onApply: (nextColumns: TableColumnId[], meta?: { resetRequested?: boolean }) => void;
  onOpenAddPicker: (currentDraft: TableColumnId[]) => void;
  onResetDefault?: () => void;
}

export const SearchColumnManagerModal: React.FC<SearchColumnManagerModalProps> = ({
  visible,
  activeColumns,
  draftColumns,
  onChangeDraft,
  onMarkResetDraft,
  onClose,
  onApply,
  onOpenAddPicker,
}) => {
  const { t } = useTranslation();
  const isControlled = draftColumns !== undefined && onChangeDraft !== undefined;
  const [localDraft, setLocalDraft] = useState<TableColumnId[]>([...activeColumns]);
  const [resetPending, setResetPending] = useState(false);
  const [selectedId, setSelectedId] = useState<TableColumnId>(activeColumns[0] ?? 'name');
  const [wasVisible, setWasVisible] = useState(visible);
  const listRef = useRef<HTMLUListElement>(null);

  const translateLabel = (key: string, defaultVal?: string) =>
    t(key as never, defaultVal as never) as unknown as string;

  // 仅在弹窗打开瞬间同步初值，严禁打开期间外部状态变化冲掉用户草稿
  if (wasVisible !== visible) {
    setWasVisible(visible);
    if (visible) {
      const seed = draftColumns ? [...draftColumns] : [...activeColumns];
      if (!isControlled) {
        setLocalDraft(seed);
      }
      setResetPending(false);
      setSelectedId((prev) => (seed.includes(prev) ? prev : (seed[0] ?? 'name')));
    }
  }

  useEffect(() => {
    if (!visible) return;
    const timer = window.setTimeout(() => {
      listRef.current?.focus({ preventScroll: true });
    }, 20);
    return () => window.clearTimeout(timer);
  }, [visible]);

  if (!visible) return null;

  const draft: TableColumnId[] = isControlled ? [...draftColumns] : localDraft;
  const updateDraft = (updater: (prev: TableColumnId[]) => TableColumnId[]) => {
    const next = updater(draft);
    if (isControlled) {
      onChangeDraft(next);
    } else {
      setLocalDraft(next);
    }
  };

  const effectiveSelectedId = draft.includes(selectedId) ? selectedId : (draft[0] ?? 'name');
  const selectedIndex = draft.indexOf(effectiveSelectedId);
  const selectedDef = effectiveSelectedId ? resolveColumnDefinition(effectiveSelectedId) : null;
  const canMoveUp = selectedIndex > 0;
  const canMoveDown = selectedIndex >= 0 && selectedIndex < draft.length - 1;
  const canRemove = Boolean(selectedDef && !selectedDef.locked && selectedIndex >= 0);

  const handleMoveUp = () => {
    if (!canMoveUp) return;
    setResetPending(false);
    updateDraft((prev) => {
      const next = [...prev];
      const [item] = next.splice(selectedIndex, 1);
      next.splice(selectedIndex - 1, 0, item);
      return next;
    });
  };

  const handleMoveDown = () => {
    if (!canMoveDown) return;
    setResetPending(false);
    updateDraft((prev) => {
      const next = [...prev];
      const [item] = next.splice(selectedIndex, 1);
      next.splice(selectedIndex + 1, 0, item);
      return next;
    });
  };

  const handleRemove = () => {
    if (!canRemove) return;
    setResetPending(false);
    const next = draft.filter((id) => id !== effectiveSelectedId);
    const fallbackIdx = Math.min(selectedIndex, next.length - 1);
    setSelectedId(next[fallbackIdx] ?? 'name');
    updateDraft(() => next);
  };

  // 重置操作严格仅作用于草稿态，绝不提前持久化；仅点击「确定」才真正落盘
  const handleReset = () => {
    updateDraft(() => [...DEFAULT_VISIBLE_TABLE_COLUMNS]);
    setSelectedId('name');
    setResetPending(true);
    onMarkResetDraft?.();
  };

  const handleConfirm = () => {
    onApply(draft, { resetRequested: resetPending });
    onClose();
  };

  const handleListKeyDown = (event: React.KeyboardEvent<HTMLUListElement>) => {
    if (draft.length === 0) return;
    if (event.altKey && event.key === 'ArrowUp') {
      event.preventDefault();
      event.stopPropagation();
      handleMoveUp();
      return;
    }
    if (event.altKey && event.key === 'ArrowDown') {
      event.preventDefault();
      event.stopPropagation();
      handleMoveDown();
      return;
    }
    if (event.key === 'ArrowDown') {
      event.preventDefault();
      event.stopPropagation();
      const nextIdx = (selectedIndex + 1) % draft.length;
      setSelectedId(draft[nextIdx]);
    } else if (event.key === 'ArrowUp') {
      event.preventDefault();
      event.stopPropagation();
      const nextIdx = (selectedIndex - 1 + draft.length) % draft.length;
      setSelectedId(draft[nextIdx]);
    } else if (event.key === 'Home') {
      event.preventDefault();
      event.stopPropagation();
      setSelectedId(draft[0]);
    } else if (event.key === 'End') {
      event.preventDefault();
      event.stopPropagation();
      setSelectedId(draft[draft.length - 1]);
    } else if (event.key === 'Delete' || event.key === 'Backspace') {
      if (canRemove) {
        event.preventDefault();
        event.stopPropagation();
        handleRemove();
      }
    } else if (event.key === 'Enter') {
      event.preventDefault();
      event.stopPropagation();
      handleConfirm();
    }
  };

  const getCategoryName = (catId: string): string => {
    const meta = PROPERTY_CATEGORIES.find((c) => c.id === catId);
    return meta ? translateLabel(meta.labelKey, meta.defaultLabel) : catId;
  };

  return (
    <div
      className="search-modal-overlay"
      role="dialog"
      aria-modal="true"
      aria-label={t('search.organizeColumnsTitle', 'Organize Columns')}
      onMouseDown={(e) => {
        if (e.target === e.currentTarget) onClose();
      }}
    >
      <div className="search-col-modal-card search-col-modal-card--manager">
        <div className="search-rename-header">
          <div className="search-rename-title">
            <Columns3 size={15} className="search-rename-icon" aria-hidden="true" />
            <span>{t('search.organizeColumnsTitle', 'Organize Columns')}</span>
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

        <div className="search-col-manager-body">
          <div className="search-col-manager-list-wrap">
            <div className="search-col-manager-list-head">
              <span>{t('search.columnHeaderName', 'Column')}</span>
              <span>{t('search.columnHeaderCategory', 'Category')}</span>
            </div>
            <ul
              ref={listRef}
              tabIndex={0}
              className="search-col-manager-list"
              role="listbox"
              aria-label={t('search.organizeColumnsTitle', 'Organize Columns')}
              aria-activedescendant={`col-manager-opt-${effectiveSelectedId}`}
              onKeyDown={handleListKeyDown}
            >
              {draft.map((colId, idx) => {
                const def = resolveColumnDefinition(colId);
                const isSelected = colId === effectiveSelectedId;
                return (
                  <li
                    key={colId}
                    id={`col-manager-opt-${colId}`}
                    role="option"
                    aria-selected={isSelected}
                    className={`search-col-manager-item ${isSelected ? 'is-selected' : ''}`}
                    onClick={() => {
                      setSelectedId(colId);
                      listRef.current?.focus({ preventScroll: true });
                    }}
                  >
                    <span className="search-col-manager-index">{idx + 1}</span>
                    <span className="search-col-manager-name">
                      {getColumnDisplayLabel(colId, translateLabel)}
                    </span>
                    {def.locked && (
                      <Lock size={12} className="search-col-manager-lock" aria-hidden="true" />
                    )}
                    <span className="search-col-manager-cat">{getCategoryName(def.category)}</span>
                  </li>
                );
              })}
            </ul>
          </div>

          <div className="search-col-manager-actions">
            <button
              type="button"
              className="search-col-action-btn search-col-action-btn--primary"
              onClick={() => onOpenAddPicker(draft)}
            >
              <Plus size={13} aria-hidden="true" />
              <span>{t('search.addColumnsBtn', 'Add...')}</span>
            </button>
            <div className="search-col-action-sep" />
            <button
              type="button"
              className="search-col-action-btn"
              disabled={!canMoveUp}
              onClick={handleMoveUp}
            >
              <ArrowUp size={13} aria-hidden="true" />
              <span>{t('search.moveUpBtn', 'Move Up')}</span>
            </button>
            <button
              type="button"
              className="search-col-action-btn"
              disabled={!canMoveDown}
              onClick={handleMoveDown}
            >
              <ArrowDown size={13} aria-hidden="true" />
              <span>{t('search.moveDownBtn', 'Move Down')}</span>
            </button>
            <button
              type="button"
              className="search-col-action-btn search-col-action-btn--danger"
              disabled={!canRemove}
              onClick={handleRemove}
            >
              <Trash2 size={13} aria-hidden="true" />
              <span>{t('search.removeBtn', 'Remove')}</span>
            </button>
            <div className="search-col-action-sep" />
            <button
              type="button"
              className="search-col-action-btn"
              onClick={handleReset}
            >
              <RotateCcw size={13} aria-hidden="true" />
              <span>{t('search.resetColumnsBtn', 'Reset')}</span>
            </button>
          </div>
        </div>

        <div className="search-rename-footer">
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
            onClick={handleConfirm}
          >
            {t('search.confirmBtn', 'OK')}
          </button>
        </div>
      </div>
    </div>
  );
};

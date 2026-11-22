/* ─────────────────────────────────────────────────────────────────────────────
 * SearchCategoryBar — 搜索分类栏（固定核心区「全部 / 文档内容」+ 可自由拖拽排序区）
 *
 * 设计亮点:
 *   1. MECE 双区物理隔离：左侧固定「全部 / 文档内容」，右侧全部类型分类支持自由拖拽排序；
 *   2. 纯指针拖拽（5px 启动阈值、跟随浮空毛玻璃胶囊、光学插入亮线、拖至固定区自动磁吸首位、Esc 取消）；
 *   3. 右键极简菜单一键恢复默认分类顺序，聚焦态支持 Ctrl+Shift+←/→ 键盘微调排序；
 *   4. 双端持久化（localStorage 0 闪烁首屏 + /search/uiPreferences.categoryOrder 跨重启永久保存）。
 * ───────────────────────────────────────────────────────────────────────────── */

import React, { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { RotateCcw } from 'lucide-react';
import type { CategoryFilter } from './searchTypes';
import type { SearchCategoryId } from './searchConstants';
import {
  isDefaultSearchCategoryOrder,
  isPinnedSearchCategory,
  reorderSearchCategories,
} from './searchUtils';

const CATEGORY_REORDER_DRAG_THRESHOLD_PX = 5;
const CTX_MENU_PADDING_PX = 8;

interface CategoryDropTarget {
  catId: SearchCategoryId;
  side: 'before' | 'after';
}

interface CategoryDragState {
  sourceId: SearchCategoryId;
  pointerX: number;
  pointerY: number;
  dropTarget: CategoryDropTarget | null;
}

export interface SearchCategoryBarProps {
  categories: readonly CategoryFilter[];
  activeCategory: string;
  onSelectCategory: (cat: CategoryFilter) => void;
  onReorderCategories: (nextOrder: SearchCategoryId[]) => void;
  onResetCategoryOrder: () => void;
}

export const SearchCategoryBar: React.FC<SearchCategoryBarProps> = ({
  categories,
  activeCategory,
  onSelectCategory,
  onReorderCategories,
  onResetCategoryOrder,
}) => {
  const { t } = useTranslation();
  const trackRef = useRef<HTMLDivElement>(null);
  const pillRefs = useRef<Partial<Record<string, HTMLButtonElement | null>>>({});
  const menuRef = useRef<HTMLDivElement>(null);
  const suppressNextClickRef = useRef(false);
  const suppressClickTimerRef = useRef<number | null>(null);
  const cleanupActiveDragRef = useRef<(() => void) | null>(null);

  const [dragState, setDragState] = useState<CategoryDragState | null>(null);
  const [ctxMenu, setCtxMenu] = useState<{ x: number; y: number } | null>(null);
  const [ctxPos, setCtxPos] = useState<{ left: number; top: number }>({ left: 0, top: 0 });

  const pinnedCategories = categories.filter((c) => isPinnedSearchCategory(c.id));
  const customCategories = categories.filter((c) => !isPinnedSearchCategory(c.id));
  const currentOrder = categories.map((c) => c.id as SearchCategoryId);
  const isDefaultOrder = isDefaultSearchCategoryOrder(currentOrder);

  useEffect(() => {
    return () => {
      cleanupActiveDragRef.current?.();
      cleanupActiveDragRef.current = null;
      if (suppressClickTimerRef.current !== null) {
        window.clearTimeout(suppressClickTimerRef.current);
        suppressClickTimerRef.current = null;
      }
    };
  }, []);

  useLayoutEffect(() => {
    if (!ctxMenu || !menuRef.current) return;
    const rect = menuRef.current.getBoundingClientRect();
    const menuW = rect.width > 0 ? rect.width : 196;
    const menuH = rect.height > 0 ? rect.height : 42;
    const maxLeft = Math.max(CTX_MENU_PADDING_PX, window.innerWidth - menuW - CTX_MENU_PADDING_PX);
    const maxTop = Math.max(CTX_MENU_PADDING_PX, window.innerHeight - menuH - CTX_MENU_PADDING_PX);
    setCtxPos({
      left: Math.max(CTX_MENU_PADDING_PX, Math.min(ctxMenu.x, maxLeft)),
      top: Math.max(CTX_MENU_PADDING_PX, Math.min(ctxMenu.y, maxTop)),
    });
  }, [ctxMenu]);

  useEffect(() => {
    if (!ctxMenu) return;
    const closeMenu = () => setCtxMenu(null);
    const onPointerDown = (evt: MouseEvent) => {
      if (menuRef.current && menuRef.current.contains(evt.target as Node)) return;
      closeMenu();
    };
    const onKeyDown = (evt: KeyboardEvent) => {
      if (evt.key === 'Escape') {
        evt.preventDefault();
        evt.stopPropagation();
        closeMenu();
      }
    };
    window.addEventListener('mousedown', onPointerDown, true);
    window.addEventListener('keydown', onKeyDown, true);
    window.addEventListener('blur', closeMenu);
    window.addEventListener('resize', closeMenu);
    return () => {
      window.removeEventListener('mousedown', onPointerDown, true);
      window.removeEventListener('keydown', onKeyDown, true);
      window.removeEventListener('blur', closeMenu);
      window.removeEventListener('resize', closeMenu);
    };
  }, [ctxMenu]);

  const computeDropTarget = useCallback(
    (sourceId: SearchCategoryId, clientX: number): CategoryDropTarget | null => {
      const movableIds = customCategories.map((c) => c.id as SearchCategoryId);
      const sourceIdx = movableIds.indexOf(sourceId);
      if (sourceIdx < 0 || movableIds.length <= 1) return null;

      let best: { catId: SearchCategoryId; side: 'before' | 'after'; dist: number } | null = null;
      let firstMovableLeft = Number.POSITIVE_INFINITY;
      let firstMovableId: SearchCategoryId | null = null;

      for (const candidateId of movableIds) {
        const el = pillRefs.current[candidateId];
        if (!el) continue;
        const rect = el.getBoundingClientRect();
        if (rect.left < firstMovableLeft) {
          firstMovableLeft = rect.left;
          firstMovableId = candidateId;
        }
        const midX = rect.left + rect.width / 2;
        const side: 'before' | 'after' = clientX < midX ? 'before' : 'after';
        const dist = Math.abs(clientX - midX);
        if (!best || dist < best.dist) {
          best = { catId: candidateId, side, dist };
        }
      }

      // 拖到固定区（「全部 / 文档内容」）上方时，智能磁吸到第一个可排序分类之前，手感连贯且绝不越界
      if (firstMovableId && clientX <= firstMovableLeft) {
        best = { catId: firstMovableId, side: 'before', dist: 0 };
      }

      if (!best || best.catId === sourceId) return null;
      const targetIdx = movableIds.indexOf(best.catId);
      if (
        (best.side === 'after' && targetIdx === sourceIdx - 1) ||
        (best.side === 'before' && targetIdx === sourceIdx + 1)
      ) {
        return null;
      }

      return { catId: best.catId, side: best.side };
    },
    [customCategories],
  );

  const handleMovableMouseDown = useCallback(
    (catId: SearchCategoryId, event: React.MouseEvent<HTMLButtonElement>) => {
      if (event.button !== 0 || isPinnedSearchCategory(catId) || customCategories.length <= 1) {
        return;
      }
      setCtxMenu(null);
      cleanupActiveDragRef.current?.();
      if (suppressClickTimerRef.current !== null) {
        window.clearTimeout(suppressClickTimerRef.current);
        suppressClickTimerRef.current = null;
      }
      suppressNextClickRef.current = false;

      const startX = event.clientX;
      const startY = event.clientY;
      const trackRect = trackRef.current?.getBoundingClientRect();
      const ghostY = trackRect ? trackRect.top + trackRect.height / 2 : event.clientY;
      let dragging = false;
      let latestDropTarget: CategoryDropTarget | null = null;

      const onMouseMove = (moveEvt: MouseEvent) => {
        const dx = moveEvt.clientX - startX;
        const dy = moveEvt.clientY - startY;
        if (!dragging) {
          if (Math.hypot(dx, dy) < CATEGORY_REORDER_DRAG_THRESHOLD_PX) return;
          dragging = true;
          suppressNextClickRef.current = true;
          document.body.style.cursor = 'grabbing';
          document.body.style.userSelect = 'none';
        }
        latestDropTarget = computeDropTarget(catId, moveEvt.clientX);
        setDragState({
          sourceId: catId,
          pointerX: moveEvt.clientX,
          pointerY: ghostY,
          dropTarget: latestDropTarget,
        });
      };

      const finish = (commit: boolean) => {
        window.removeEventListener('mousemove', onMouseMove);
        window.removeEventListener('mouseup', onMouseUp);
        window.removeEventListener('keydown', onKeyDown, true);
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
        cleanupActiveDragRef.current = null;

        if (dragging && commit && latestDropTarget) {
          const nextOrder = reorderSearchCategories(
            currentOrder,
            catId,
            latestDropTarget.catId,
            latestDropTarget.side,
          );
          if (nextOrder !== currentOrder) {
            onReorderCategories(nextOrder);
          }
        }
        if (dragging) {
          suppressClickTimerRef.current = window.setTimeout(() => {
            suppressNextClickRef.current = false;
            suppressClickTimerRef.current = null;
          }, 0);
        }
        setDragState(null);
      };

      const onMouseUp = () => finish(true);
      const onKeyDown = (keyEvt: KeyboardEvent) => {
        if (keyEvt.key === 'Escape' && dragging) {
          keyEvt.preventDefault();
          keyEvt.stopPropagation();
          finish(false);
        }
      };

      cleanupActiveDragRef.current = () => finish(false);
      window.addEventListener('mousemove', onMouseMove);
      window.addEventListener('mouseup', onMouseUp);
      window.addEventListener('keydown', onKeyDown, true);
    },
    [computeDropTarget, currentOrder, customCategories.length, onReorderCategories],
  );

  const handleMovableKeyDown = useCallback(
    (catId: SearchCategoryId, event: React.KeyboardEvent<HTMLButtonElement>) => {
      if (!(event.ctrlKey || event.metaKey) || !event.shiftKey || event.altKey) return;
      if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
      const movableIds = customCategories.map((c) => c.id as SearchCategoryId);
      const idx = movableIds.indexOf(catId);
      if (idx < 0) return;

      const targetIdx = event.key === 'ArrowLeft' ? idx - 1 : idx + 1;
      if (targetIdx < 0 || targetIdx >= movableIds.length) return;

      event.preventDefault();
      event.stopPropagation();
      const targetId = movableIds[targetIdx];
      const side: 'before' | 'after' = event.key === 'ArrowLeft' ? 'before' : 'after';
      const nextOrder = reorderSearchCategories(currentOrder, catId, targetId, side);
      if (nextOrder !== currentOrder) {
        onReorderCategories(nextOrder);
      }
    },
    [currentOrder, customCategories, onReorderCategories],
  );

  const handlePillClick = useCallback(
    (cat: CategoryFilter) => {
      if (suppressNextClickRef.current) {
        suppressNextClickRef.current = false;
        return;
      }
      onSelectCategory(cat);
    },
    [onSelectCategory],
  );

  const handleContextMenu = useCallback((event: React.MouseEvent<HTMLDivElement>) => {
    event.preventDefault();
    event.stopPropagation();
    setCtxMenu({ x: event.clientX, y: event.clientY });
  }, []);

  const draggingCat = dragState
    ? categories.find((c) => c.id === dragState.sourceId) ?? null
    : null;

  return (
    <div
      ref={trackRef}
      className={`search-categories${dragState ? ' search-categories--dragging' : ''}`}
      onContextMenu={handleContextMenu}
      role="tablist"
      aria-label={t('search.categoryBarAria', 'Search categories')}
    >
      {/* 左侧固定核心分类（全部 / 文档内容） */}
      {pinnedCategories.map((cat) => (
        <button
          key={cat.id}
          ref={(node) => {
            pillRefs.current[cat.id] = node;
          }}
          type="button"
          role="tab"
          aria-selected={activeCategory === cat.id}
          data-category-id={cat.id}
          data-category-pinned="true"
          className={`category-pill category-pill--pinned${activeCategory === cat.id ? ' category-pill--active' : ''}`}
          title={t('search.categoryPinnedTip', 'Pinned core category')}
          onClick={() => handlePillClick(cat)}
        >
          {cat.icon && <cat.icon size={13} className="category-pill-icon" aria-hidden="true" />}
          <span>{cat.label}</span>
        </button>
      ))}

      {pinnedCategories.length > 0 && customCategories.length > 0 && (
        <span className="search-categories-divider" aria-hidden="true" />
      )}

      {/* 右侧用户可自由拖拽排序分类 */}
      {customCategories.map((cat) => {
        const catId = cat.id as SearchCategoryId;
        const isDraggingSelf = dragState?.sourceId === catId;
        const isDropBefore =
          dragState?.dropTarget?.catId === catId && dragState.dropTarget.side === 'before';
        const isDropAfter =
          dragState?.dropTarget?.catId === catId && dragState.dropTarget.side === 'after';

        return (
          <button
            key={cat.id}
            ref={(node) => {
              pillRefs.current[cat.id] = node;
            }}
            type="button"
            role="tab"
            aria-selected={activeCategory === cat.id}
            data-category-id={cat.id}
            data-category-pinned="false"
            className={[
              'category-pill',
              'category-pill--movable',
              activeCategory === cat.id ? 'category-pill--active' : '',
              isDraggingSelf ? 'category-pill--dragging' : '',
              isDropBefore ? 'category-pill--drop-before' : '',
              isDropAfter ? 'category-pill--drop-after' : '',
            ]
              .filter(Boolean)
              .join(' ')}
            title={t('search.categoryDragTip', 'Drag to reorder, right-click to reset')}
            onMouseDown={(e) => handleMovableMouseDown(catId, e)}
            onKeyDown={(e) => handleMovableKeyDown(catId, e)}
            onClick={() => handlePillClick(cat)}
          >
            {cat.icon && <cat.icon size={13} className="category-pill-icon" aria-hidden="true" />}
            <span>{cat.label}</span>
          </button>
        );
      })}

      {/* 拖拽中的浮空毛玻璃预览胶囊 */}
      {dragState && draggingCat && (
        <div
          className="category-drag-ghost"
          style={{ left: `${dragState.pointerX}px`, top: `${dragState.pointerY}px` }}
          aria-hidden="true"
        >
          {draggingCat.icon && (
            <draggingCat.icon size={13} className="category-pill-icon" aria-hidden="true" />
          )}
          <span>{draggingCat.label}</span>
        </div>
      )}

      {/* 分类栏右键极简菜单：一键恢复默认分类顺序 */}
      {ctxMenu && (
        <div
          ref={menuRef}
          className="category-ctx-menu"
          role="menu"
          style={{ left: `${ctxPos.left}px`, top: `${ctxPos.top}px` }}
        >
          <button
            type="button"
            role="menuitem"
            className="category-ctx-menu__item"
            disabled={isDefaultOrder}
            onClick={() => {
              setCtxMenu(null);
              if (!isDefaultOrder) {
                onResetCategoryOrder();
              }
            }}
          >
            <RotateCcw size={13} aria-hidden="true" />
            <span>{t('search.resetCategoryOrder', 'Reset category order')}</span>
          </button>
        </div>
      )}
    </div>
  );
};

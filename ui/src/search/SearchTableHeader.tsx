import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { ArrowDown, ArrowUp, GripVertical } from 'lucide-react';
import { getColumnDisplayLabel } from './columnCatalog';
import { SearchColumnHeaderMenu } from './SearchColumnHeaderMenu';
import type {
  ColumnId,
  ColumnLayout,
  SortDirection,
  SortField,
  TableColumnId,
} from './searchTypes';
import {
  buildTableLayoutMetrics,
  clampTableColumnWidth,
  getTableColumnSpec,
  normalizeTableColumnOrder,
  reorderTableColumns,
} from './searchUtils';

const REORDER_DRAG_THRESHOLD_PX = 5;

interface DragReorderState {
  sourceId: TableColumnId;
  pointerX: number;
  headerTop: number;
  headerHeight: number;
  dropTarget: {
    colId: TableColumnId;
    side: 'before' | 'after';
  } | null;
}

interface HeaderContextMenuState {
  x: number;
  y: number;
  targetColId: TableColumnId | null;
}

/**
 * 搜索结果表格视图（Table View）吸顶表头：
 * 支持全列（含 10 大基础列与全部扩展属性列）点击升降序排序、任意列边界自由拖拽调宽
 * （双击适应内容或恢复默认）、任意列标题按住左右拖拽换序、多列横向同步滚动，
 * 以及 Everything 风格表头右键列管理菜单。
 */
export interface SearchTableHeaderProps {
  columns: ColumnLayout;
  sortField: SortField;
  sortDirection: SortDirection;
  onSelectSort: (field: SortField) => void;
  onUpdateNameAndPathFlex: (nameFlex: number) => void;
  onUpdateTableColumnWidth?: (colId: TableColumnId, widthPx: number | undefined) => void;
  onBatchUpdateTableColumnWidths?: (patch: Partial<Record<TableColumnId, number | undefined>>) => void;
  onReorderTableColumns?: (nextOrder: TableColumnId[]) => void;
  onToggleColumnVisibility?: (colId: ColumnId) => void;
  onToggleTableColumn?: (colId: TableColumnId) => void;
  onRemoveTableColumn?: (colId: TableColumnId) => void;
  onFitColumnToContent?: (colId: TableColumnId) => void;
  onFitAllColumnsToContent?: () => void;
  onOpenAddColumns?: () => void;
  onOpenOrganizeColumns?: () => void;
  recentTableColumns?: readonly TableColumnId[];
  onResetColumns?: () => void;
  headerScrollRef?: React.RefObject<HTMLDivElement | null>;
  hidden?: boolean;
}

export const SearchTableHeader: React.FC<SearchTableHeaderProps> = ({
  columns,
  sortField,
  sortDirection,
  onSelectSort,
  onUpdateNameAndPathFlex,
  onUpdateTableColumnWidth,
  onBatchUpdateTableColumnWidths,
  onReorderTableColumns,
  onToggleColumnVisibility,
  onToggleTableColumn,
  onRemoveTableColumn,
  onFitColumnToContent,
  onFitAllColumnsToContent,
  onOpenAddColumns,
  onOpenOrganizeColumns,
  recentTableColumns = [],
  onResetColumns,
  headerScrollRef,
  hidden = false,
}) => {
  const { t } = useTranslation();
  const headerRef = useRef<HTMLDivElement>(null);
  const colWrapRefs = useRef<Partial<Record<TableColumnId, HTMLDivElement | null>>>({});
  const suppressNextClickRef = useRef(false);
  const suppressClickTimerRef = useRef<number | null>(null);
  const cleanupActiveDragRef = useRef<(() => void) | null>(null);

  const [dragState, setDragState] = useState<DragReorderState | null>(null);
  const [resizingColId, setResizingColId] = useState<TableColumnId | null>(null);
  const [headerMenu, setHeaderMenu] = useState<HeaderContextMenuState | null>(null);

  const layoutMetrics = useMemo(() => buildTableLayoutMetrics(columns), [columns]);
  const { visibleCols, gridTemplateColumns, minTableWidthPx } = layoutMetrics;
  const fullOrder = useMemo(
    () => normalizeTableColumnOrder([...(columns.tableOrder ?? []), ...visibleCols]),
    [columns.tableOrder, visibleCols],
  );

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

  const translateLabel = useCallback(
    (key: string, defaultVal?: string) => t(key as never, defaultVal as never) as unknown as string,
    [t],
  );

  const getColumnLabel = useCallback(
    (colId: TableColumnId): string => getColumnDisplayLabel(colId, translateLabel),
    [translateLabel],
  );

  const measureColumnWidth = useCallback(
    (colId: TableColumnId): number => {
      const el = colWrapRefs.current[colId];
      const measured = el ? el.getBoundingClientRect().width : 0;
      if (measured > 0) return measured;
      return columns.tableWidths?.[colId] ?? getTableColumnSpec(colId).defaultWidth;
    },
    [columns.tableWidths],
  );

  const handleResizeStart = useCallback(
    (colId: TableColumnId, colIndex: number, event: React.MouseEvent<HTMLDivElement>) => {
      if (event.button !== 0) return;
      event.preventDefault();
      event.stopPropagation();
      setHeaderMenu(null);

      cleanupActiveDragRef.current?.();

      const startX = event.clientX;
      const startColWidth = measureColumnWidth(colId);

      // Everything / 资源管理器同款 1:1 像素级直觉拖拽：
      // 拖哪一列的右分隔线，就精确调整那一列自身宽度；
      // 拖动前若左侧存在尚未锁宽的弹性列 (如拖动 path 时左侧的 name)，先将其冻结为拖动起始瞬间的实测像素宽，
      // 彻底防止左侧 1fr 弹性列反向吞噬 deltaX 造成分割线漂移或完全拖不动。
      const frozenLeftCols: Array<{ id: TableColumnId; width: number }> = [];
      for (let i = 0; i < colIndex; i += 1) {
        const leftId = visibleCols[i];
        if (getTableColumnSpec(leftId).isFlex && columns.tableWidths?.[leftId] === undefined) {
          frozenLeftCols.push({
            id: leftId,
            width: clampTableColumnWidth(leftId, measureColumnWidth(leftId)),
          });
        }
      }
      let hasFrozenLeft = false;

      setResizingColId(colId);

      const onMouseMove = (moveEvent: MouseEvent) => {
        const deltaX = moveEvent.clientX - startX;
        const nextWidth = clampTableColumnWidth(colId, startColWidth + deltaX);

        if (!hasFrozenLeft && frozenLeftCols.length > 0) {
          hasFrozenLeft = true;
          if (onBatchUpdateTableColumnWidths) {
            const patch: Partial<Record<TableColumnId, number | undefined>> = {};
            for (const item of frozenLeftCols) {
              patch[item.id] = item.width;
            }
            patch[colId] = nextWidth;
            onBatchUpdateTableColumnWidths(patch);
            return;
          }
          for (const item of frozenLeftCols) {
            onUpdateTableColumnWidth?.(item.id, item.width);
          }
        }

        onUpdateTableColumnWidth?.(colId, nextWidth);
      };

      const cleanup = () => {
        window.removeEventListener('mousemove', onMouseMove);
        window.removeEventListener('mouseup', onMouseUp);
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
        setResizingColId(null);
        cleanupActiveDragRef.current = null;
      };

      const onMouseUp = () => {
        cleanup();
      };

      cleanupActiveDragRef.current = cleanup;
      document.body.style.cursor = 'col-resize';
      document.body.style.userSelect = 'none';
      window.addEventListener('mousemove', onMouseMove);
      window.addEventListener('mouseup', onMouseUp);
    },
    [
      columns.tableWidths,
      measureColumnWidth,
      onBatchUpdateTableColumnWidths,
      onUpdateTableColumnWidth,
      visibleCols,
    ],
  );

  const handleResizeReset = useCallback(
    (colId: TableColumnId, _colIndex: number, event: React.MouseEvent<HTMLDivElement>) => {
      event.preventDefault();
      event.stopPropagation();
      if (columns.tableWidths?.[colId] !== undefined) {
        onUpdateTableColumnWidth?.(colId, undefined);
        return;
      }
      if (getTableColumnSpec(colId).isFlex) {
        onUpdateNameAndPathFlex(35);
      } else {
        onUpdateTableColumnWidth?.(colId, undefined);
      }
    },
    [columns.tableWidths, onUpdateNameAndPathFlex, onUpdateTableColumnWidth],
  );

  const computeDropTarget = useCallback(
    (sourceId: TableColumnId, clientX: number): DragReorderState['dropTarget'] => {
      const sourceIndex = visibleCols.indexOf(sourceId);
      if (sourceIndex < 0 || visibleCols.length <= 1) return null;

      let best: { colId: TableColumnId; side: 'before' | 'after'; dist: number } | null = null;
      for (let i = 0; i < visibleCols.length; i += 1) {
        const candidateId = visibleCols[i];
        const el = colWrapRefs.current[candidateId];
        if (!el) continue;
        const rect = el.getBoundingClientRect();
        const midX = rect.left + rect.width / 2;
        const side: 'before' | 'after' = clientX < midX ? 'before' : 'after';
        const dist = Math.abs(clientX - midX);
        if (!best || dist < best.dist) {
          best = { colId: candidateId, side, dist };
        }
      }

      if (!best || best.colId === sourceId) return null;
      const targetIndex = visibleCols.indexOf(best.colId);
      if (
        (best.side === 'after' && targetIndex === sourceIndex - 1) ||
        (best.side === 'before' && targetIndex === sourceIndex + 1)
      ) {
        return null;
      }

      return { colId: best.colId, side: best.side };
    },
    [visibleCols],
  );

  const handleColumnMouseDown = useCallback(
    (colId: TableColumnId, event: React.MouseEvent<HTMLElement>) => {
      if (event.button !== 0 || !onReorderTableColumns || visibleCols.length <= 1) return;
      setHeaderMenu(null);

      cleanupActiveDragRef.current?.();
      if (suppressClickTimerRef.current !== null) {
        window.clearTimeout(suppressClickTimerRef.current);
        suppressClickTimerRef.current = null;
      }
      suppressNextClickRef.current = false;

      const startX = event.clientX;
      const startY = event.clientY;
      let dragging = false;
      let latestDropTarget: DragReorderState['dropTarget'] = null;
      const headerRect = headerRef.current?.getBoundingClientRect();
      const headerTop = headerRect?.top ?? 0;
      const headerHeight = headerRect?.height ?? 31;

      const onMouseMove = (moveEvent: MouseEvent) => {
        const dx = moveEvent.clientX - startX;
        const dy = moveEvent.clientY - startY;
        if (!dragging) {
          if (Math.hypot(dx, dy) < REORDER_DRAG_THRESHOLD_PX) return;
          dragging = true;
          suppressNextClickRef.current = true;
          document.body.style.cursor = 'grabbing';
          document.body.style.userSelect = 'none';
        }
        latestDropTarget = computeDropTarget(colId, moveEvent.clientX);
        setDragState({
          sourceId: colId,
          pointerX: moveEvent.clientX,
          headerTop,
          headerHeight,
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
          const nextOrder = reorderTableColumns(
            fullOrder,
            colId,
            latestDropTarget.colId,
            latestDropTarget.side,
          );
          if (nextOrder !== fullOrder) {
            onReorderTableColumns(nextOrder);
          }
        }
        if (dragging) {
          // 跨列松手时浏览器可能不会在原按钮上派发 click，兜底在下一宏任务清空抑制标志避免吞掉下次排序点击
          suppressClickTimerRef.current = window.setTimeout(() => {
            suppressNextClickRef.current = false;
            suppressClickTimerRef.current = null;
          }, 0);
        }
        setDragState(null);
      };

      const onMouseUp = () => finish(true);
      const onKeyDown = (keyEvent: KeyboardEvent) => {
        if (keyEvent.key === 'Escape' && dragging) {
          keyEvent.preventDefault();
          keyEvent.stopPropagation();
          finish(false);
        }
      };

      cleanupActiveDragRef.current = () => finish(false);
      window.addEventListener('mousemove', onMouseMove);
      window.addEventListener('mouseup', onMouseUp);
      window.addEventListener('keydown', onKeyDown, true);
    },
    [computeDropTarget, fullOrder, onReorderTableColumns, visibleCols.length],
  );

  const handleHeaderContextMenu = useCallback(
    (event: React.MouseEvent<HTMLDivElement>) => {
      if (!onToggleTableColumn && !onToggleColumnVisibility && !onResetColumns) return;
      event.preventDefault();
      event.stopPropagation();
      const targetEl = (event.target as HTMLElement | null)?.closest<HTMLElement>('[data-table-col]');
      const targetColId = targetEl?.dataset.tableCol ?? null;
      setHeaderMenu({
        x: event.clientX,
        y: event.clientY,
        targetColId,
      });
    },
    [onResetColumns, onToggleColumnVisibility, onToggleTableColumn],
  );

  const handleToggleColumnFromMenu = useCallback(
    (colId: TableColumnId) => {
      if (onToggleTableColumn) {
        onToggleTableColumn(colId);
        return;
      }
      if (!onToggleColumnVisibility) return;
      if (colId === 'path' || colId === 'size' || colId === 'modified' || colId === 'created') {
        onToggleColumnVisibility(colId);
      }
    },
    [onToggleColumnVisibility, onToggleTableColumn],
  );

  const renderSortArrow = (field: SortField | null) => {
    if (!field || sortField !== field) return null;
    return sortDirection === 'asc' ? (
      <ArrowUp size={12} className="table-sort-icon" aria-hidden="true" />
    ) : (
      <ArrowDown size={12} className="table-sort-icon" aria-hidden="true" />
    );
  };

  const canReorder = Boolean(onReorderTableColumns && visibleCols.length > 1);
  const gridStyle: React.CSSProperties = {
    gridTemplateColumns,
    minWidth: `${minTableWidthPx}px`,
  };
  const activeHeaderMenu = hidden ? null : headerMenu;

  return (
    <div
      ref={headerRef}
      className={`search-table-header ${dragState ? 'search-table-header--reordering' : ''}`}
      role="rowgroup"
      aria-label={t('search.tableHeaderAria', 'Search results table column headers')}
      style={{ display: hidden ? 'none' : undefined }}
      onContextMenu={handleHeaderContextMenu}
    >
      <div ref={headerScrollRef} className="search-table-header-scroll">
        <div className="search-table-header-grid" role="row" style={gridStyle}>
          {visibleCols.map((colId, colIndex) => {
            const spec = getTableColumnSpec(colId);
            const label = getColumnLabel(colId);
            const isSortable = spec.sortField !== null;
            const isRightAligned = spec.align === 'right';
            const isActiveSort = isSortable && sortField === spec.sortField;
            const isDraggingSource = dragState?.sourceId === colId;
            const dropSide = dragState?.dropTarget?.colId === colId
              ? dragState.dropTarget.side
              : null;
            const isLastColumn = colIndex === visibleCols.length - 1;
            const isResizing = resizingColId === colId;

            const colTitle = isSortable
              ? (canReorder
                  ? t('search.sortAndReorderColTip', 'Click to sort · Drag to reorder column')
                  : t('search.sortByColTip', 'Click to sort by this column'))
              : (canReorder
                  ? t('search.reorderColTip', 'Drag left or right to reorder column')
                  : undefined);

            const wrapClassName = [
              'search-table-th-wrap',
              isRightAligned ? 'search-table-th-wrap--right' : '',
              isDraggingSource ? 'search-table-th-wrap--dragging' : '',
              dropSide === 'before' ? 'search-table-th-wrap--drop-before' : '',
              dropSide === 'after' ? 'search-table-th-wrap--drop-after' : '',
            ]
              .filter(Boolean)
              .join(' ');

            return (
              <div
                key={colId}
                ref={(node) => {
                  colWrapRefs.current[colId] = node;
                }}
                data-table-col={colId}
                className={wrapClassName}
                role="columnheader"
                aria-sort={
                  isActiveSort
                    ? (sortDirection === 'asc' ? 'ascending' : 'descending')
                    : 'none'
                }
              >
                <button
                  type="button"
                  className={[
                    'search-table-th',
                    isSortable ? 'search-table-th--sortable' : 'search-table-th--reorderable',
                    isRightAligned ? 'search-table-th--right' : '',
                    isActiveSort ? 'search-table-th--active' : '',
                  ]
                    .filter(Boolean)
                    .join(' ')}
                  onMouseDown={(event) => handleColumnMouseDown(colId, event)}
                  onClick={() => {
                    if (suppressNextClickRef.current) {
                      suppressNextClickRef.current = false;
                      return;
                    }
                    if (spec.sortField) {
                      onSelectSort(spec.sortField);
                    }
                  }}
                  title={colTitle}
                >
                  {isRightAligned && renderSortArrow(spec.sortField)}
                  <span className="search-table-th-label">{label}</span>
                  {!isRightAligned && renderSortArrow(spec.sortField)}
                </button>

                <div
                  className={[
                    'search-table-col-resizer',
                    isLastColumn ? 'search-table-col-resizer--last' : '',
                    isResizing ? 'search-table-col-resizer--active' : '',
                  ]
                    .filter(Boolean)
                    .join(' ')}
                  data-resize-col={colId}
                  onMouseDown={(event) => handleResizeStart(colId, colIndex, event)}
                  onDoubleClick={(event) => handleResizeReset(colId, colIndex, event)}
                  title={t('search.resizeColTip', 'Drag to resize column width · Double-click to reset')}
                  role="separator"
                  aria-orientation="vertical"
                  aria-label={`${label} ${t('search.resizeColTip', 'Drag to resize column width · Double-click to reset')}`}
                />
              </div>
            );
          })}
        </div>
      </div>

      {dragState && (
        <div
          className="search-table-drag-ghost"
          style={{
            left: dragState.pointerX,
            top: dragState.headerTop + dragState.headerHeight / 2,
          }}
          aria-hidden="true"
        >
          <GripVertical size={12} />
          <span>{getColumnLabel(dragState.sourceId)}</span>
        </div>
      )}

      <SearchColumnHeaderMenu
        visible={Boolean(activeHeaderMenu)}
        x={activeHeaderMenu?.x ?? 0}
        y={activeHeaderMenu?.y ?? 0}
        targetColId={activeHeaderMenu?.targetColId ?? null}
        visibleColumns={visibleCols}
        recentColumns={recentTableColumns}
        onClose={() => setHeaderMenu(null)}
        onFitColumn={onFitColumnToContent}
        onFitAllColumns={onFitAllColumnsToContent}
        onToggleTableColumn={handleToggleColumnFromMenu}
        onRemoveColumn={onRemoveTableColumn ?? ((cid) => handleToggleColumnFromMenu(cid))}
        onOpenAddColumns={() => onOpenAddColumns?.()}
        onOpenOrganizeColumns={() => onOpenOrganizeColumns?.()}
        onResetColumns={onResetColumns}
      />
    </div>
  );
};

import React, { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Check,
  ChevronRight,
  Columns3,
  EyeOff,
  History,
  Maximize2,
  MoveHorizontal,
  Plus,
  RotateCcw,
} from 'lucide-react';
import {
  getColumnDisplayLabel,
  resolveColumnDefinition,
  TIER1_QUICK_COLUMN_IDS,
} from './columnCatalog';
import type { TableColumnId } from './searchTypes';

export interface SearchColumnHeaderMenuProps {
  visible: boolean;
  x: number;
  y: number;
  targetColId: TableColumnId | null;
  visibleColumns: readonly TableColumnId[];
  recentColumns: readonly TableColumnId[];
  onClose: () => void;
  onFitColumn?: (colId: TableColumnId) => void;
  onFitAllColumns?: () => void;
  onToggleTableColumn: (colId: TableColumnId) => void;
  onRemoveColumn?: (colId: TableColumnId) => void;
  onOpenAddColumns: () => void;
  onOpenOrganizeColumns: () => void;
  onResetColumns?: () => void;
}

const VIEWPORT_PADDING_PX = 8;
const SUBMENU_MIN_WIDTH_PX = 206;

export const SearchColumnHeaderMenu: React.FC<SearchColumnHeaderMenuProps> = ({
  visible,
  x,
  y,
  targetColId,
  visibleColumns,
  recentColumns,
  onClose,
  onFitColumn,
  onFitAllColumns,
  onToggleTableColumn,
  onRemoveColumn,
  onOpenAddColumns,
  onOpenOrganizeColumns,
  onResetColumns,
}) => {
  const { t } = useTranslation();
  const menuRef = useRef<HTMLDivElement>(null);
  const submenuAnchorRef = useRef<HTMLDivElement>(null);
  const recentTriggerRef = useRef<HTMLButtonElement>(null);
  const submenuRef = useRef<HTMLDivElement>(null);

  const [showRecentSubmenu, setShowRecentSubmenu] = useState(false);
  const [submenuFlipLeft, setSubmenuFlipLeft] = useState(false);
  const [menuPos, setMenuPos] = useState<{ left: number; top: number }>({ left: x, top: y });
  const [prevVisible, setPrevVisible] = useState(visible);

  // 每次重新打开或关闭时同步重置子菜单状态与坐标初值
  if (prevVisible !== visible) {
    setPrevVisible(visible);
    setShowRecentSubmenu(false);
    setSubmenuFlipLeft(false);
    if (visible) {
      setMenuPos({ left: x, top: y });
    }
  }

  const translateLabel = (key: string, defaultVal?: string) =>
    t(key as never, defaultVal as never) as unknown as string;

  // 实测真实菜单宽高并动态夹紧在视口内（超出视口高时贴顶并由 CSS 开启内部滚动）
  useLayoutEffect(() => {
    if (!visible || !menuRef.current) return;
    const rect = menuRef.current.getBoundingClientRect();
    const measuredWidth = rect.width > 0 ? rect.width : 246;
    const measuredHeight = rect.height > 0 ? rect.height : 380;

    const maxLeft = Math.max(VIEWPORT_PADDING_PX, window.innerWidth - measuredWidth - VIEWPORT_PADDING_PX);
    const maxTop = Math.max(VIEWPORT_PADDING_PX, window.innerHeight - measuredHeight - VIEWPORT_PADDING_PX);
    const nextLeft = Math.max(VIEWPORT_PADDING_PX, Math.min(x, maxLeft));
    const nextTop = Math.max(VIEWPORT_PADDING_PX, Math.min(y, maxTop));

    setMenuPos((prev) =>
      prev.left === nextLeft && prev.top === nextTop ? prev : { left: nextLeft, top: nextTop },
    );

    const anchorRect = submenuAnchorRef.current?.getBoundingClientRect();
    if (anchorRect && anchorRect.width > 0) {
      const wouldOverflowRight =
        anchorRect.right + SUBMENU_MIN_WIDTH_PX + VIEWPORT_PADDING_PX > window.innerWidth;
      const hasRoomOnLeft = anchorRect.left - SUBMENU_MIN_WIDTH_PX - VIEWPORT_PADDING_PX >= 0;
      setSubmenuFlipLeft(wouldOverflowRight && hasRoomOnLeft);
    }
  }, [visible, x, y, targetColId, visibleColumns.length]);

  useEffect(() => {
    if (!visible) return;
    const handlePointerDown = (event: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(event.target as Node)) {
        onClose();
      }
    };
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        event.stopPropagation();
        if (showRecentSubmenu) {
          setShowRecentSubmenu(false);
          recentTriggerRef.current?.focus();
        } else {
          onClose();
        }
      }
    };
    window.addEventListener('mousedown', handlePointerDown);
    window.addEventListener('keydown', handleKeyDown, true);
    return () => {
      window.removeEventListener('mousedown', handlePointerDown);
      window.removeEventListener('keydown', handleKeyDown, true);
    };
  }, [onClose, showRecentSubmenu, visible]);

  // 打开菜单时自动聚焦第一个可用菜单项，方便纯键盘巡航
  useEffect(() => {
    if (!visible || !menuRef.current) return;
    const firstItem = menuRef.current.querySelector<HTMLButtonElement>(
      '[data-header-menu-primary="true"]:not(:disabled)',
    );
    firstItem?.focus({ preventScroll: true });
  }, [visible, x, y]);

  const checkSubmenuFlip = () => {
    const anchorRect = submenuAnchorRef.current?.getBoundingClientRect();
    if (!anchorRect || anchorRect.width === 0) return;
    const wouldOverflowRight =
      anchorRect.right + SUBMENU_MIN_WIDTH_PX + VIEWPORT_PADDING_PX > window.innerWidth;
    const hasRoomOnLeft = anchorRect.left - SUBMENU_MIN_WIDTH_PX - VIEWPORT_PADDING_PX >= 0;
    setSubmenuFlipLeft(wouldOverflowRight && hasRoomOnLeft);
  };

  if (!visible) return null;

  const visibleSet = new Set(visibleColumns);
  const targetDef = targetColId ? resolveColumnDefinition(targetColId) : null;
  const canRemoveTarget = Boolean(targetColId && targetDef && !targetDef.locked && onRemoveColumn);
  const filteredRecents = recentColumns.filter((id) => id !== 'name').slice(0, 8);

  const handlePrimaryMenuKeyDown = (event: React.KeyboardEvent<HTMLDivElement>) => {
    if (!menuRef.current) return;
    const target = event.target as HTMLElement | null;
    const inSubmenu = Boolean(submenuRef.current && target && submenuRef.current.contains(target));

    if (inSubmenu && submenuRef.current) {
      const subItems = Array.from(
        submenuRef.current.querySelectorAll<HTMLButtonElement>('button:not(:disabled)'),
      );
      if (subItems.length === 0) return;
      const idx = subItems.indexOf(document.activeElement as HTMLButtonElement);
      if (event.key === 'ArrowDown') {
        event.preventDefault();
        event.stopPropagation();
        subItems[(idx + 1 + subItems.length) % subItems.length]?.focus();
      } else if (event.key === 'ArrowUp') {
        event.preventDefault();
        event.stopPropagation();
        subItems[(idx <= 0 ? subItems.length - 1 : idx - 1)]?.focus();
      } else if (event.key === 'Home') {
        event.preventDefault();
        event.stopPropagation();
        subItems[0]?.focus();
      } else if (event.key === 'End') {
        event.preventDefault();
        event.stopPropagation();
        subItems[subItems.length - 1]?.focus();
      } else if (event.key === 'ArrowLeft') {
        event.preventDefault();
        event.stopPropagation();
        setShowRecentSubmenu(false);
        recentTriggerRef.current?.focus();
      }
      return;
    }

    const primaryItems = Array.from(
      menuRef.current.querySelectorAll<HTMLButtonElement>(
        '[data-header-menu-primary="true"]:not(:disabled)',
      ),
    );
    if (primaryItems.length === 0) return;
    const activeIdx = primaryItems.indexOf(document.activeElement as HTMLButtonElement);

    if (event.key === 'ArrowDown') {
      event.preventDefault();
      event.stopPropagation();
      const next = activeIdx < 0 ? 0 : (activeIdx + 1) % primaryItems.length;
      primaryItems[next]?.focus();
    } else if (event.key === 'ArrowUp') {
      event.preventDefault();
      event.stopPropagation();
      const next = activeIdx <= 0 ? primaryItems.length - 1 : activeIdx - 1;
      primaryItems[next]?.focus();
    } else if (event.key === 'Home') {
      event.preventDefault();
      event.stopPropagation();
      primaryItems[0]?.focus();
    } else if (event.key === 'End') {
      event.preventDefault();
      event.stopPropagation();
      primaryItems[primaryItems.length - 1]?.focus();
    } else if (
      (event.key === 'ArrowRight' || (submenuFlipLeft && event.key === 'ArrowLeft')) &&
      document.activeElement === recentTriggerRef.current &&
      filteredRecents.length > 0
    ) {
      event.preventDefault();
      event.stopPropagation();
      checkSubmenuFlip();
      setShowRecentSubmenu(true);
      window.setTimeout(() => {
        submenuRef.current?.querySelector<HTMLButtonElement>('button:not(:disabled)')?.focus();
      }, 0);
    }
  };

  return (
    <div
      ref={menuRef}
      className="search-table-header-menu search-table-header-menu--extended"
      style={{ left: menuPos.left, top: menuPos.top }}
      role="menu"
      aria-label={t('search.tableMenuTitle', 'Columns & Layout')}
      onKeyDown={handlePrimaryMenuKeyDown}
    >
      {/* 1. 列宽自适应与隐藏当前列 */}
      {targetColId && onFitColumn && (
        <button
          type="button"
          role="menuitem"
          data-header-menu-primary="true"
          className="search-table-header-menu-item"
          onClick={() => {
            onFitColumn(targetColId);
            onClose();
          }}
        >
          <MoveHorizontal size={13} className="header-menu-icon" aria-hidden="true" />
          <span>{t('search.sizeColumnToFit', 'Size Column to Fit')}</span>
        </button>
      )}
      {onFitAllColumns && (
        <button
          type="button"
          role="menuitem"
          data-header-menu-primary="true"
          className="search-table-header-menu-item"
          onClick={() => {
            onFitAllColumns();
            onClose();
          }}
        >
          <Maximize2 size={13} className="header-menu-icon" aria-hidden="true" />
          <span>{t('search.sizeAllColumnsToFit', 'Size All Columns to Fit')}</span>
        </button>
      )}
      {targetColId && onRemoveColumn && (
        <button
          type="button"
          role="menuitem"
          data-header-menu-primary="true"
          disabled={!canRemoveTarget}
          className="search-table-header-menu-item"
          onClick={() => {
            if (canRemoveTarget) {
              onRemoveColumn(targetColId);
              onClose();
            }
          }}
        >
          <EyeOff size={13} className="header-menu-icon" aria-hidden="true" />
          <span>{t('search.removeColumn', 'Remove Column')}</span>
        </button>
      )}

      <div className="search-table-header-menu-sep" />

      {/* 2. 添加扩展属性列 / 最近使用的列 / 管理列 */}
      <button
        type="button"
        role="menuitem"
        data-header-menu-primary="true"
        className="search-table-header-menu-item"
        onClick={() => {
          onClose();
          onOpenAddColumns();
        }}
      >
        <Plus size={13} className="header-menu-icon" aria-hidden="true" />
        <span>{t('search.addColumnsAction', 'Add Columns...')}</span>
      </button>

      <div
        ref={submenuAnchorRef}
        className="search-table-header-submenu-anchor"
        onMouseEnter={() => {
          if (filteredRecents.length > 0) {
            checkSubmenuFlip();
            setShowRecentSubmenu(true);
          }
        }}
        onMouseLeave={() => setShowRecentSubmenu(false)}
      >
        <button
          ref={recentTriggerRef}
          type="button"
          role="menuitem"
          data-header-menu-primary="true"
          aria-haspopup="menu"
          aria-expanded={showRecentSubmenu && filteredRecents.length > 0}
          disabled={filteredRecents.length === 0}
          className="search-table-header-menu-item search-table-header-menu-item--has-sub"
          onClick={() => {
            checkSubmenuFlip();
            setShowRecentSubmenu((prev) => !prev);
          }}
        >
          <History size={13} className="header-menu-icon" aria-hidden="true" />
          <span className="header-menu-label-flex">{t('search.recentColumnsAction', 'Recent Columns')}</span>
          <ChevronRight size={12} className="header-menu-chevron" aria-hidden="true" />
        </button>

        {showRecentSubmenu && filteredRecents.length > 0 && (
          <div
            ref={submenuRef}
            className={`search-table-header-submenu ${submenuFlipLeft ? 'search-table-header-submenu--left' : ''}`}
            role="menu"
            aria-label={t('search.recentColumnsAction', 'Recent Columns')}
          >
            {filteredRecents.map((colId) => {
              const checked = visibleSet.has(colId);
              return (
                <button
                  key={colId}
                  type="button"
                  role="menuitemcheckbox"
                  aria-checked={checked}
                  className="search-table-header-menu-item"
                  onClick={() => {
                    onToggleTableColumn(colId);
                    onClose();
                  }}
                >
                  <span className={`search-table-header-menu-check ${checked ? 'is-checked' : ''}`}>
                    {checked && <Check size={11} aria-hidden="true" />}
                  </span>
                  <span>{getColumnDisplayLabel(colId, translateLabel)}</span>
                </button>
              );
            })}
          </div>
        )}
      </div>

      <button
        type="button"
        role="menuitem"
        data-header-menu-primary="true"
        className="search-table-header-menu-item"
        onClick={() => {
          onClose();
          onOpenOrganizeColumns();
        }}
      >
        <Columns3 size={13} className="header-menu-icon" aria-hidden="true" />
        <span>{t('search.organizeColumnsAction', 'Organize Columns...')}</span>
      </button>

      <div className="search-table-header-menu-sep" />

      {/* 3. Tier 1 10 大核心基础列快捷勾选区 */}
      <div className="search-table-header-menu-scroll">
        {TIER1_QUICK_COLUMN_IDS.map((colId) => {
          const def = resolveColumnDefinition(colId);
          const checked = visibleSet.has(colId);
          const locked = Boolean(def.locked);
          return (
            <button
              key={colId}
              type="button"
              role="menuitemcheckbox"
              data-header-menu-primary="true"
              aria-checked={checked}
              disabled={locked}
              className={`search-table-header-menu-item ${locked ? 'search-table-header-menu-item--locked' : ''}`}
              onClick={() => {
                if (!locked) {
                  onToggleTableColumn(colId);
                  onClose();
                }
              }}
            >
              <span className={`search-table-header-menu-check ${checked ? 'is-checked' : ''}`}>
                {checked && <Check size={11} aria-hidden="true" />}
              </span>
              <span>{getColumnDisplayLabel(colId, translateLabel)}</span>
            </button>
          );
        })}
      </div>

      {onResetColumns && (
        <>
          <div className="search-table-header-menu-sep" />
          <button
            type="button"
            role="menuitem"
            data-header-menu-primary="true"
            className="search-table-header-menu-item search-table-header-menu-item--reset"
            onClick={() => {
              onResetColumns();
              onClose();
            }}
          >
            <RotateCcw size={13} aria-hidden="true" />
            <span>{t('search.resetTableColumns', 'Reset column order & widths')}</span>
          </button>
        </>
      )}
    </div>
  );
};

// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import {
  CATEGORY_DEFS,
  DEFAULT_SEARCH_CATEGORY_ORDER,
  PINNED_SEARCH_CATEGORY_IDS,
  type SearchCategoryId,
} from './searchConstants';
import {
  isDefaultSearchCategoryOrder,
  isPinnedSearchCategory,
  normalizeSearchCategoryOrder,
  reorderSearchCategories,
} from './searchUtils';
import { SearchCategoryBar } from './SearchCategoryBar';
import type { CategoryFilter } from './searchTypes';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, defaultVal?: string) => (typeof defaultVal === 'string' ? defaultVal : key),
  }),
}));

afterEach(() => {
  cleanup();
});

function buildCategoryFilters(order: readonly SearchCategoryId[]): CategoryFilter[] {
  const byId = new Map<string, (typeof CATEGORY_DEFS)[number]>(
    CATEGORY_DEFS.map((d) => [d.id, d]),
  );
  return order
    .map((id) => byId.get(id))
    .filter((d): d is (typeof CATEGORY_DEFS)[number] => Boolean(d))
    .map((d) => ({
      id: d.id,
      label: d.defaultLabel,
      labelKey: d.labelKey,
      defaultLabel: d.defaultLabel,
      prefix: d.prefix,
      icon: d.icon,
    }));
}

describe('SearchCategoryBar pure ordering & pinned invariants', () => {
  it('locks All and Content in the first two slots regardless of corrupted or partial input', () => {
    expect(PINNED_SEARCH_CATEGORY_IDS).toEqual(['all', 'content']);
    expect(isPinnedSearchCategory('all')).toBe(true);
    expect(isPinnedSearchCategory('content')).toBe(true);
    expect(isPinnedSearchCategory('bookmark')).toBe(false);

    const normalized = normalizeSearchCategoryOrder([
      'bookmark',
      'content',
      'invalid_cat',
      'image',
      'all',
      'bookmark',
    ]);
    expect(normalized.slice(0, 4)).toEqual(['all', 'content', 'bookmark', 'image']);
    expect(normalized.length).toBe(DEFAULT_SEARCH_CATEGORY_ORDER.length);
    expect(isDefaultSearchCategoryOrder(normalized)).toBe(false);
    expect(isDefaultSearchCategoryOrder(DEFAULT_SEARCH_CATEGORY_ORDER)).toBe(true);
  });

  it('refuses moving pinned categories or dropping into pinned slots, while reordering movable ones cleanly', () => {
    const base = [...DEFAULT_SEARCH_CATEGORY_ORDER];

    // 严禁拖动固定项或向固定项前/后插队
    expect(reorderSearchCategories(base, 'all', 'doc', 'after')).toEqual(base);
    expect(reorderSearchCategories(base, 'content', 'bookmark', 'after')).toEqual(base);
    expect(reorderSearchCategories(base, 'bookmark', 'all', 'before')).toEqual(base);
    expect(reorderSearchCategories(base, 'bookmark', 'content', 'after')).toEqual(base);

    // 正常将 bookmark 移到 doc 之前（紧随固定区）
    const moved = reorderSearchCategories(base, 'bookmark', 'doc', 'before');
    expect(moved).toEqual([
      'all',
      'content',
      'bookmark',
      'doc',
      'image',
      'video',
      'audio',
      'archive',
      'code',
      'folder',
    ]);
  });
});

describe('SearchCategoryBar drag-and-drop & context menu interaction', () => {
  it('reorders movable pills via mouse drag, snaps to first movable slot when dragged over pinned area, and supports right-click reset', () => {
    const order = [...DEFAULT_SEARCH_CATEGORY_ORDER];
    const categories = buildCategoryFilters(order);
    const onSelectCategory = vi.fn();
    const onReorderCategories = vi.fn();
    const onResetCategoryOrder = vi.fn();

    const { container, rerender } = render(
      <SearchCategoryBar
        categories={categories}
        activeCategory="all"
        onSelectCategory={onSelectCategory}
        onReorderCategories={onReorderCategories}
        onResetCategoryOrder={onResetCategoryOrder}
      />,
    );

    // 给每个胶囊设定模拟横坐标（宽 70px 紧邻排列）
    order.forEach((id, idx) => {
      const btn = container.querySelector<HTMLButtonElement>(`[data-category-id="${id}"]`)!;
      expect(btn).toBeTruthy();
      vi.spyOn(btn, 'getBoundingClientRect').mockReturnValue({
        left: idx * 70,
        right: (idx + 1) * 70,
        width: 70,
        top: 10,
        bottom: 36,
        height: 26,
        x: idx * 70,
        y: 10,
        toJSON: () => ({}),
      });
    });

    // 1. 固定区与可拖拽区之间存在分隔线
    expect(container.querySelector('.search-categories-divider')).toBeTruthy();

    // 2. 尝试拖动固定项「Content」：完全忽略，不触发重排
    const contentBtn = container.querySelector<HTMLButtonElement>('[data-category-id="content"]')!;
    fireEvent.mouseDown(contentBtn, { button: 0, clientX: 90, clientY: 20 });
    fireEvent.mouseMove(window, { clientX: 300, clientY: 20 });
    fireEvent.mouseUp(window, { clientX: 300, clientY: 20 });
    expect(onReorderCategories).not.toHaveBeenCalled();

    // 3. 拖动「Bookmarks (idx=9)」至固定区「All (x=20)」上方 -> 自动磁吸到首个可排序位（doc 之前）
    const bookmarkBtn = container.querySelector<HTMLButtonElement>('[data-category-id="bookmark"]')!;
    fireEvent.mouseDown(bookmarkBtn, { button: 0, clientX: 660, clientY: 20 });
    fireEvent.mouseMove(window, { clientX: 20, clientY: 20 });
    fireEvent.mouseUp(window, { clientX: 20, clientY: 20 });

    const expectedCustomOrder: SearchCategoryId[] = [
      'all',
      'content',
      'bookmark',
      'doc',
      'image',
      'video',
      'audio',
      'archive',
      'code',
      'folder',
    ];
    expect(onReorderCategories).toHaveBeenCalledTimes(1);
    expect(onReorderCategories).toHaveBeenCalledWith(expectedCustomOrder);
    expect(onSelectCategory).not.toHaveBeenCalled();

    // 4. 键盘无障碍微调：在已定制顺序中对「doc」按 Ctrl+Shift+ArrowLeft 挪到 bookmark 之前
    rerender(
      <SearchCategoryBar
        categories={buildCategoryFilters(expectedCustomOrder)}
        activeCategory="all"
        onSelectCategory={onSelectCategory}
        onReorderCategories={onReorderCategories}
        onResetCategoryOrder={onResetCategoryOrder}
      />,
    );
    const docBtn = container.querySelector<HTMLButtonElement>('[data-category-id="doc"]')!;
    fireEvent.keyDown(docBtn, { key: 'ArrowLeft', ctrlKey: true, shiftKey: true });
    expect(onReorderCategories).toHaveBeenLastCalledWith([
      'all',
      'content',
      'doc',
      'bookmark',
      'image',
      'video',
      'audio',
      'archive',
      'code',
      'folder',
    ]);

    // 5. 右键分类栏弹出菜单，点击「恢复默认分类顺序」触发回调
    const bar = container.querySelector<HTMLDivElement>('.search-categories')!;
    fireEvent.contextMenu(bar, { clientX: 200, clientY: 24 });
    const resetItem = screen.getByRole('menuitem', { name: 'Reset category order' });
    expect((resetItem as HTMLButtonElement).disabled).toBe(false);
    fireEvent.click(resetItem);
    expect(onResetCategoryOrder).toHaveBeenCalledTimes(1);
  });
});

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
  it('locks only All in the first slot while allowing Content and all other categories to be freely reordered', () => {
    expect(PINNED_SEARCH_CATEGORY_IDS).toEqual(['all']);
    expect(isPinnedSearchCategory('all')).toBe(true);
    expect(isPinnedSearchCategory('content')).toBe(false);
    expect(isPinnedSearchCategory('bookmark')).toBe(false);

    const normalized = normalizeSearchCategoryOrder([
      'bookmark',
      'content',
      'invalid_cat',
      'image',
      'all',
      'bookmark',
    ]);
    expect(normalized.slice(0, 4)).toEqual(['all', 'bookmark', 'content', 'image']);
    expect(normalized.length).toBe(DEFAULT_SEARCH_CATEGORY_ORDER.length);
    expect(isDefaultSearchCategoryOrder(normalized)).toBe(false);
    expect(isDefaultSearchCategoryOrder(DEFAULT_SEARCH_CATEGORY_ORDER)).toBe(true);

    // 兼容旧版已持久化顺序（未显式包含 content 时平滑接在 all 之后）
    const legacyNormalized = normalizeSearchCategoryOrder(['bookmark', 'doc', 'image']);
    expect(legacyNormalized.slice(0, 4)).toEqual(['all', 'content', 'bookmark', 'doc']);
  });

  it('refuses moving All or dropping into All slot, while reordering Content and other movable ones cleanly', () => {
    const base = [...DEFAULT_SEARCH_CATEGORY_ORDER];

    // 严禁拖动固定项「全部」或向「全部」前/后直接插队
    expect(reorderSearchCategories(base, 'all', 'doc', 'after')).toEqual(base);
    expect(reorderSearchCategories(base, 'bookmark', 'all', 'before')).toEqual(base);

    // 允许自由拖动「文档内容 (content)」至任意位置
    const movedContent = reorderSearchCategories(base, 'content', 'bookmark', 'after');
    expect(movedContent[0]).toBe('all');
    expect(movedContent[1]).toBe('doc');
    expect(movedContent[movedContent.length - 1]).toBe('content');

    // 允许将 app / bookmark 拖至 content 之前（紧随「全部」）
    const moved = reorderSearchCategories(base, 'bookmark', 'content', 'before');
    expect(moved).toEqual([
      'all',
      'bookmark',
      'content',
      'doc',
      'image',
      'video',
      'audio',
      'archive',
      'code',
      'folder',
      'app',
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

    // 2. 尝试拖动固定项「All」：完全忽略，不触发重排
    const allBtn = container.querySelector<HTMLButtonElement>('[data-category-id="all"]')!;
    fireEvent.mouseDown(allBtn, { button: 0, clientX: 20, clientY: 20 });
    fireEvent.mouseMove(window, { clientX: 300, clientY: 20 });
    fireEvent.mouseUp(window, { clientX: 300, clientY: 20 });
    expect(onReorderCategories).not.toHaveBeenCalled();

    // 3. 拖动「Bookmarks (idx=10)」至固定区「All (x=20)」上方 -> 自动磁吸到首个可排序位（content 之前）
    const bookmarkBtn = container.querySelector<HTMLButtonElement>('[data-category-id="bookmark"]')!;
    fireEvent.mouseDown(bookmarkBtn, { button: 0, clientX: 730, clientY: 20 });
    fireEvent.mouseMove(window, { clientX: 20, clientY: 20 });
    fireEvent.mouseUp(window, { clientX: 20, clientY: 20 });

    const expectedCustomOrder: SearchCategoryId[] = [
      'all',
      'bookmark',
      'content',
      'doc',
      'image',
      'video',
      'audio',
      'archive',
      'code',
      'folder',
      'app',
    ];
    expect(onReorderCategories).toHaveBeenCalledTimes(1);
    expect(onReorderCategories).toHaveBeenCalledWith(expectedCustomOrder);
    expect(onSelectCategory).not.toHaveBeenCalled();

    // 4. 拖动「Content (idx=1)」到末尾「Bookmarks」之后 -> 正常可自由排序
    onReorderCategories.mockClear();
    const contentBtn = container.querySelector<HTMLButtonElement>('[data-category-id="content"]')!;
    fireEvent.mouseDown(contentBtn, { button: 0, clientX: 90, clientY: 20 });
    fireEvent.mouseMove(window, { clientX: 760, clientY: 20 });
    fireEvent.mouseUp(window, { clientX: 760, clientY: 20 });
    expect(onReorderCategories).toHaveBeenCalledTimes(1);
    expect(onReorderCategories.mock.calls[0][0][0]).toBe('all');
    expect(onReorderCategories.mock.calls[0][0][onReorderCategories.mock.calls[0][0].length - 1]).toBe('content');

    // 5. 键盘无障碍微调：在已定制顺序中对「doc」按 Ctrl+Shift+ArrowLeft 挪到 content 之前
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
      'bookmark',
      'doc',
      'content',
      'image',
      'video',
      'audio',
      'archive',
      'code',
      'folder',
      'app',
    ]);

    // 6. 右键分类栏弹出菜单，点击「恢复默认分类顺序」触发回调
    const bar = container.querySelector<HTMLDivElement>('.search-categories')!;
    fireEvent.contextMenu(bar, { clientX: 200, clientY: 24 });
    const resetItem = screen.getByRole('menuitem', { name: 'Reset category order' });
    expect((resetItem as HTMLButtonElement).disabled).toBe(false);
    fireEvent.click(resetItem);
    expect(onResetCategoryOrder).toHaveBeenCalledTimes(1);
  });
});

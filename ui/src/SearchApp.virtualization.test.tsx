/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { VirtualSearchResults } from './search/VirtualSearchResults';
import { SearchTableHeader } from './search/SearchTableHeader';
import { SearchHeader } from './search/SearchHeader';
import {
  buildTableGridTemplate,
  getVisibleTableColumns,
  reorderTableColumns,
} from './search/searchUtils';
import type { ColumnLayout, SearchResult, TableColumnId } from './search/searchTypes';

class ResizeObserverMock {
  static instances: ResizeObserverMock[] = [];
  private readonly callback: ResizeObserverCallback;
  readonly observed = new Set<Element>();
  readonly observe = vi.fn((element: Element) => this.observed.add(element));
  readonly unobserve = vi.fn((element: Element) => this.observed.delete(element));
  readonly disconnect = vi.fn(() => this.observed.clear());

  constructor(callback: ResizeObserverCallback) {
    this.callback = callback;
    ResizeObserverMock.instances.push(this);
  }

  emit(element: Element, height: number) {
    this.callback([{
      target: element,
      contentRect: { height },
      borderBoxSize: [{ blockSize: height }],
    } as unknown as ResizeObserverEntry], this as unknown as ResizeObserver);
  }
}

Object.defineProperty(globalThis, 'ResizeObserver', {
  writable: true,
  value: ResizeObserverMock,
});

const columns: ColumnLayout = {
  name: true,
  path: true,
  size: false,
  modified: false,
  created: false,
  snippets: false,
  nameFlex: 35,
  pathFlex: 45,
};

function results(count: number): SearchResult[] {
  return Array.from({ length: count }, (_, index) => ({
    name: `result-${index}`,
    path: `C:\\results\\${index}.txt`,
    isDirectory: false,
  }));
}

function props(items: SearchResult[], selectedIndex = 0) {
  return {
    results: items,
    selectedIndex,
    density: 'standard' as const,
    columns,
    queryKeywords: [],
    onHover: vi.fn(),
    onSelect: vi.fn(),
    onOpen: vi.fn(),
    onContextMenu: vi.fn(),
  };
}

afterEach(() => {
  cleanup();
  ResizeObserverMock.instances = [];
});

describe('VirtualSearchResults (Unified Table View)', () => {
  it('renders every enabled table column including extension, parentName, size, and authoritative timestamps', () => {
    const item: SearchResult = {
      name: 'report.pdf',
      path: 'D:\\Chosen\\Project\\report.pdf',
      isDirectory: false,
      size: 2048,
      creationTime: 134000000000000000,
      lastWriteTime: 134001000000000000,
    };
    const allColumns: ColumnLayout = {
      name: true,
      path: true,
      size: true,
      modified: true,
      created: true,
      snippets: true,
      nameFlex: 35,
      pathFlex: 45,
      tableOrder: ['name', 'ext', 'parentName', 'path', 'size', 'modified', 'created'],
      visibleTableColumns: ['name', 'ext', 'parentName', 'path', 'size', 'modified', 'created'],
    };

    const view = render(<VirtualSearchResults {...props([item])} columns={allColumns} />);

    expect(view.getByText('report.pdf')).not.toBeNull();
    expect(document.querySelector('[data-table-col="ext"]')?.textContent).toBe('pdf');
    expect(document.querySelector('[data-table-col="parentName"]')?.textContent).toBe('Project');
    expect(document.querySelector('[data-table-col="path"]')?.textContent).toBe('D:\\Chosen\\Project');
    expect(document.querySelector('[data-table-col="size"]')?.textContent).toBe('2.0 KB');
    expect(document.querySelector('[data-table-col="modified"]')?.textContent).toMatch(/^\d{4}-\d{2}-\d{2}/);
    expect(document.querySelector('[data-table-col="created"]')?.textContent).toMatch(/^\d{4}-\d{2}-\d{2}/);
  });

  it('keeps a 100k result DOM window bounded and mounts the far active option', () => {
    const items = results(100_000);
    const view = render(<VirtualSearchResults {...props(items)} />);
    const list = view.getByRole('listbox');
    Object.defineProperty(list, 'clientHeight', { configurable: true, value: 480 });

    const initialCount = list.querySelectorAll('[role="option"]').length;
    expect(initialCount).toBeGreaterThan(0);
    expect(initialCount).toBeLessThanOrEqual(40);
    view.rerender(<VirtualSearchResults {...props(items, 99_999)} />);

    const options = list.querySelectorAll('[role="option"]');
    expect(options.length).toBeLessThanOrEqual(25);
    expect(document.getElementById('search-result-99999')).not.toBeNull();
    expect(document.getElementById('search-result-99999')?.getAttribute('data-virtual-index')).toBe('99999');
    expect(document.getElementById('search-result-99999')?.getAttribute('aria-selected')).toBe('true');
    expect(list.scrollTop).toBeGreaterThan(3_000_000);
  });

  it('applies ResizeObserver measurements, guards against stale path mismatch, and disconnects on unmount', () => {
    const initialItems = results(20);
    const view = render(<VirtualSearchResults {...props(initialItems)} />);
    const firstRow = document.getElementById('search-result-0')!;
    const secondRow = document.getElementById('search-result-1')!;
    expect(firstRow.dataset.virtualPath).toBe(initialItems[0].path);

    const rowObserver = ResizeObserverMock.instances.find((observer) => observer.observed.has(firstRow));
    expect(rowObserver).toBeDefined();
    act(() => rowObserver!.emit(firstRow, 120));
    expect(secondRow.style.top).toBe('120px');

    // 模拟异步过期测量（DOM data-virtual-path 与当前索引槽位路径不一致）：不得污染新行高度
    const staleEl = document.createElement('li');
    staleEl.dataset.virtualIndex = '1';
    staleEl.dataset.virtualPath = 'C:\\stale\\other.txt';
    const thirdRow = document.getElementById('search-result-2')!;
    const beforeTop = thirdRow.style.top;
    act(() => rowObserver!.emit(staleEl, 240));
    expect(thirdRow.style.top).toBe(beforeTop);

    view.unmount();
    expect(rowObserver!.disconnect).toHaveBeenCalledTimes(1);
  });

  it('supports persistent singleton mount with hidden prop', () => {
    const items = results(10);
    const view = render(<VirtualSearchResults {...props(items)} hidden={true} />);
    const list = document.getElementById('search-results')!;
    expect(list).not.toBeNull();
    expect(list.style.display).toBe('none');

    view.rerender(<VirtualSearchResults {...props(items)} hidden={false} />);
    expect(list.style.display).toBe('');
  });

  it('resets scroll position cleanly when results change to a new list', () => {
    const itemsA = results(100);
    const view = render(<VirtualSearchResults {...props(itemsA, 50)} />);
    const list = document.getElementById('search-results')!;
    list.scrollTop = 1200;

    const itemsB = results(5);
    view.rerender(<VirtualSearchResults {...props(itemsB, 0)} />);
    expect(list.scrollTop).toBe(0);
    expect(document.getElementById('search-result-0')).not.toBeNull();
  });

  it('keeps scroll position stable during scrollbar drag without snapping back to index 0', () => {
    const onHover = vi.fn();
    const items = results(100);
    const view = render(<VirtualSearchResults {...props(items, 0)} onHover={onHover} />);
    const list = document.getElementById('search-results')!;
    Object.defineProperty(list, 'clientHeight', { configurable: true, value: 480 });

    list.scrollTop = 800;
    fireEvent.scroll(list);

    const observedRow = document.querySelector<HTMLElement>('[data-virtual-index]')!;
    const rowObserver = ResizeObserverMock.instances.find((observer) => observer.observed.has(observedRow));
    expect(rowObserver).toBeDefined();

    act(() => {
      rowObserver!.emit(observedRow, 72);
    });

    expect(list.scrollTop).toBe(800);

    fireEvent.mouseMove(observedRow, { buttons: 1, movementX: 5, movementY: 5 });
    expect(onHover).not.toHaveBeenCalled();

    view.unmount();
  });

  it('invokes updated row callbacks after rerender even when result objects are unchanged', () => {
    const items = results(3);
    const onOpenFirst = vi.fn();
    const onOpenSecond = vi.fn();

    const baseProps = props(items, 0);
    const view = render(<VirtualSearchResults {...baseProps} onOpen={onOpenFirst} />);
    view.rerender(<VirtualSearchResults {...baseProps} onOpen={onOpenSecond} />);

    const firstRow = document.getElementById('search-result-0')!;
    fireEvent.doubleClick(firstRow);

    expect(onOpenFirst).not.toHaveBeenCalled();
    expect(onOpenSecond).toHaveBeenCalledTimes(1);
    expect(onOpenSecond).toHaveBeenCalledWith(items[0]);
  });

  it('requests uncached icons via effect while honoring empty-string negative cache entries', () => {
    const items: SearchResult[] = [
      { name: 'cached.txt', path: 'C:\\results\\cached.txt', isDirectory: false },
      { name: 'uncached.pdf', path: 'C:\\results\\uncached.pdf', isDirectory: false },
    ];
    const onRequestIcon = vi.fn();
    const iconCache: Record<string, string> = {
      '.txt': '',
    };

    render(
      <VirtualSearchResults
        {...props(items, 0)}
        iconCache={iconCache}
        onRequestIcon={onRequestIcon}
      />,
    );

    expect(onRequestIcon).toHaveBeenCalledTimes(1);
    expect(onRequestIcon).toHaveBeenCalledWith('.pdf', false);
  });

  it('requests per-file native icons with full paths for executables and shortcuts (.exe, .lnk) and uses semantic vector icon classes', () => {
    const items: SearchResult[] = [
      { name: 'Tools3000-Setup.exe', path: 'C:\\repo\\Tools3000\\Output\\Tools3000-Setup.exe', isDirectory: false },
      { name: 'Tools3000Tests.exe', path: 'C:\\repo\\Tools3000\\build\\bin\\Release\\Tools3000Tests.exe', isDirectory: false },
      { name: 'movie.mp4', path: 'C:\\Videos\\movie.mp4', isDirectory: false },
    ];
    const onRequestIcon = vi.fn();
    const iconCache: Record<string, string> = {
      'path:c:\\repo\\tools3000\\output\\tools3000-setup.exe': 'data:image/png;base64,AAA',
    };

    const { rerender } = render(
      <VirtualSearchResults
        {...props(items, 0)}
        iconCache={iconCache}
        onRequestIcon={onRequestIcon}
      />,
    );

    expect(onRequestIcon).toHaveBeenCalledTimes(2);
    expect(onRequestIcon).toHaveBeenNthCalledWith(
      1,
      '.exe',
      false,
      'C:\\repo\\Tools3000\\build\\bin\\Release\\Tools3000Tests.exe',
    );
    expect(onRequestIcon).toHaveBeenNthCalledWith(2, '.mp4', false);

    // 切换极简矢量图标时，exe 与视频图标类名与 CSS 配色规则 100% 匹配
    rerender(
      <VirtualSearchResults
        {...props(items, 0)}
        iconStyle="vector"
      />,
    );
    expect(document.querySelector('#search-result-0 .file-icon--exe')).not.toBeNull();
    expect(document.querySelector('#search-result-2 .file-icon--media')).not.toBeNull();
  });

  it('renders Table Mode with aligned columns and orthogonal full-width content search snippets', () => {
    const tableColumns: ColumnLayout = {
      name: true,
      path: true,
      size: true,
      modified: true,
      created: false,
      snippets: true,
      nameFlex: 35,
      pathFlex: 45,
    };
    const items: SearchResult[] = [
      {
        name: 'SearchExpression.cpp',
        path: 'D:\\工作目录\\user\\SearchExpression.cpp',
        isDirectory: false,
        size: 16384,
        lastWriteTime: 134001000000000000,
        snippets: [
          {
            lineNumber: 42,
            lineContent: 'bool matchMultiRepPathSegments(std::wstring_view slashNormPath)',
            matchOffset: 5,
            matchLength: 25,
          },
        ],
      },
      {
        name: 'readme.md',
        path: 'D:\\工作目录\\user\\readme.md',
        isDirectory: false,
        size: 1024,
        lastWriteTime: 134000000000000000,
      },
    ];

    const onSelectSort = vi.fn();
    const view = render(
      <>
        <SearchTableHeader
          columns={tableColumns}
          sortField="modified"
          sortDirection="desc"
          onSelectSort={onSelectSort}
          onUpdateNameAndPathFlex={vi.fn()}
        />
        <VirtualSearchResults
          {...props(items, 0)}
          columns={tableColumns}
          queryKeywords={['matchMultiRepPathSegments', 'user']}
        />
      </>,
    );

    const expectedGrid = buildTableGridTemplate(tableColumns);
    const headerGrid = document.querySelector<HTMLElement>('.search-table-header-grid');
    const firstMainRow = document.querySelector<HTMLElement>('#search-result-0 .search-table-main-row');
    expect(headerGrid).not.toBeNull();
    expect(firstMainRow).not.toBeNull();
    expect(headerGrid!.style.gridTemplateColumns).toBe(expectedGrid);
    expect(firstMainRow!.style.gridTemplateColumns).toBe(expectedGrid);

    const firstDirCell = document.querySelector('#search-result-0 .table-cell--dir');
    expect(firstDirCell?.textContent).toBe('D:\\工作目录\\user');

    const firstSnippets = document.querySelector('#search-result-0 .file-snippets-container--table');
    const secondSnippets = document.querySelector('#search-result-1 .file-snippets-container--table');
    expect(firstSnippets).not.toBeNull();
    expect(firstSnippets?.textContent).toContain('L42');
    expect(firstSnippets?.textContent).toContain('matchMultiRepPathSegments');
    expect(secondSnippets).toBeNull();

    fireEvent.click(view.getByText('File Name'));
    expect(onSelectSort).toHaveBeenCalledWith('name');
  });

  it('renders clean SearchHeader without Card/Table switcher while keeping window controls and history dropdown', () => {
    const onToggleMaximized = vi.fn();
    const onMinimizeWindow = vi.fn();
    const onCloseWindow = vi.fn();
    const onToggleHistoryMenu = vi.fn();
    const onCloseHistoryMenu = vi.fn();
    const onSelectHistoryItem = vi.fn();
    const onRemoveHistoryItem = vi.fn();
    const onClearAllHistory = vi.fn();
    const inputRef = { current: null };

    const historyItems = [
      { search: '*.pdf', searchCount: 3, lastSearchDate: 100 },
      { search: 'report 2026', searchCount: 5, lastSearchDate: 200 },
    ];

    const view = render(
      <SearchHeader
        inputRef={inputRef}
        query="report"
        onUpdateQuery={vi.fn()}
        activeCategory="all"
        searchMode="name"
        loading={false}
        isInitialIndexing={false}
        isServiceStarting={false}
        isPinned={false}
        onTogglePin={vi.fn()}
        isMaximized={false}
        onToggleMaximized={onToggleMaximized}
        onMinimizeWindow={onMinimizeWindow}
        onCloseWindow={onCloseWindow}
        showViewSettings={false}
        onToggleViewSettings={vi.fn()}
        showSyntaxHelp={false}
        onToggleSyntaxHelp={vi.fn()}
        onResetPlacement={vi.fn()}
        searchHistory={historyItems}
        showHistoryMenu={true}
        onToggleHistoryMenu={onToggleHistoryMenu}
        onCloseHistoryMenu={onCloseHistoryMenu}
        onSelectHistoryItem={onSelectHistoryItem}
        onRemoveHistoryItem={onRemoveHistoryItem}
        onClearAllHistory={onClearAllHistory}
        resultsCount={10}
        hasSelectedResult={true}
        selectedIndex={0}
        setIsComposing={vi.fn()}
      />,
    );

    // 卡片/表格分段切换按钮组已彻底移除
    expect(document.querySelector('.search-view-toggle-group')).toBeNull();

    const minBtn = view.getByRole('button', { name: 'Minimize to Taskbar' });
    fireEvent.click(minBtn);
    expect(onMinimizeWindow).toHaveBeenCalledTimes(1);

    const maxBtn = view.getByRole('button', { name: 'Maximize Window (F11)' });
    fireEvent.click(maxBtn);
    expect(onToggleMaximized).toHaveBeenCalledTimes(1);

    const closeBtn = view.getByRole('button', { name: 'Close Window (Esc · Reopen anytime with Alt+Space)' });
    fireEvent.click(closeBtn);
    expect(onCloseWindow).toHaveBeenCalledTimes(1);

    const dragSurface = document.querySelector<HTMLElement>('.search-icon-drag-handle')!;
    fireEvent.doubleClick(dragSurface);
    expect(onToggleMaximized).toHaveBeenCalledTimes(2);

    const historyTrigger = view.getByRole('button', { name: 'Search history (Alt+Down / Ctrl+H)' });
    fireEvent.click(historyTrigger);
    expect(onToggleHistoryMenu).toHaveBeenCalledTimes(1);

    const options = view.getAllByRole('option');
    expect(options).toHaveLength(2);
    expect(options[0].textContent).toContain('report 2026');
    expect(options[1].textContent).toContain('*.pdf');

    const inputEl = view.getByRole('combobox');
    fireEvent.keyDown(inputEl, { key: 'ArrowDown' });
    fireEvent.keyDown(inputEl, { key: 'Enter' });
    expect(onSelectHistoryItem).toHaveBeenCalledWith('*.pdf');
    expect(onCloseHistoryMenu).toHaveBeenCalledTimes(1);

    const delButtons = view.getAllByRole('button', { name: 'Delete this history' });
    fireEvent.click(delButtons[0]);
    expect(onRemoveHistoryItem).toHaveBeenCalledTimes(1);
    expect(onRemoveHistoryItem.mock.calls[0][1]).toBe('report 2026');

    fireEvent.click(view.getByText('Clear History'));
    expect(onClearAllHistory).toHaveBeenCalledTimes(1);
  });

  it('supports resizing every column, prevents locked path resizer from mutating size, avoids swallowed clicks after drag, and preserves DOM when hidden', () => {
    vi.useFakeTimers();
    const initialColumns: ColumnLayout = {
      name: true,
      path: true,
      size: true,
      modified: true,
      created: true,
      snippets: true,
      nameFlex: 35,
      pathFlex: 45,
      tableOrder: ['name', 'path', 'size', 'modified', 'created'],
      tableWidths: {},
    };

    const reordered = reorderTableColumns(
      ['name', 'path', 'size', 'modified', 'created'],
      'modified',
      'path',
      'before',
    );
    expect(reordered).toEqual(['name', 'modified', 'path', 'size', 'created']);

    const customCols: ColumnLayout = {
      ...initialColumns,
      tableOrder: reordered,
      tableWidths: { path: 320, size: 132, modified: 188, created: 164 },
    };
    expect(getVisibleTableColumns(customCols)).toEqual(['name', 'modified', 'path', 'size', 'created']);
    expect(buildTableGridTemplate(customCols)).toBe(
      'minmax(120px, 1fr) 188px 320px 132px 164px',
    );

    const items: SearchResult[] = [
      {
        name: 'report.txt',
        path: 'D:\\工作目录\\user\\report.txt',
        isDirectory: false,
        size: 4096,
        lastWriteTime: 134000000000000000,
        creationTime: 133900000000000000,
      },
    ];

    const onSelectSort = vi.fn();
    const onUpdateWidth = vi.fn();
    const onReorder = vi.fn();
    const onToggleCol = vi.fn();
    const onResetCols = vi.fn();

    const view = render(
      <>
        <SearchTableHeader
          columns={customCols}
          sortField="modified"
          sortDirection="desc"
          onSelectSort={onSelectSort}
          onUpdateNameAndPathFlex={vi.fn()}
          onUpdateTableColumnWidth={onUpdateWidth}
          onReorderTableColumns={onReorder}
          onToggleColumnVisibility={onToggleCol}
          onResetColumns={onResetCols}
          recentTableColumns={['dimensions', 'ext']}
        />
        <VirtualSearchResults
          {...props(items, 0)}
          columns={customCols}
          queryKeywords={[]}
        />
      </>,
    );

    // 1. 表头与数据行 DOM 列顺序 100% 同步一致
    const headerCols = Array.from(document.querySelectorAll<HTMLElement>('.search-table-th-wrap'))
      .map((el) => el.dataset.tableCol);
    const rowCols = Array.from(document.querySelectorAll<HTMLElement>('#search-result-0 .table-cell'))
      .map((el) => el.dataset.tableCol);
    expect(headerCols).toEqual(['name', 'modified', 'path', 'size', 'created']);
    expect(rowCols).toEqual(['name', 'modified', 'path', 'size', 'created']);

    // 2. 当 path 列已锁宽 (tableWidths.path = 320)，拖/双击 path 右分隔线只能操作 path 本身，绝不误改右侧 size 列
    const pathResizer = document.querySelector<HTMLElement>('[data-resize-col="path"]')!;
    fireEvent.mouseDown(pathResizer, { button: 0, clientX: 400 });
    fireEvent.mouseMove(window, { clientX: 440 });
    fireEvent.mouseUp(window);
    expect(onUpdateWidth).toHaveBeenLastCalledWith('path', 360);

    fireEvent.doubleClick(pathResizer);
    expect(onUpdateWidth).toHaveBeenLastCalledWith('path', undefined);

    // 3. 拖拽跨列换序松手（未触发源按钮 click）后，下一拍自动清空抑制标记，后续点击排序绝不被吞
    const colOrder: TableColumnId[] = ['name', 'modified', 'path', 'size', 'created'];
    colOrder.forEach((id, idx) => {
      const el = document.querySelector<HTMLElement>(`.search-table-th-wrap[data-table-col="${id}"]`)!;
      vi.spyOn(el, 'getBoundingClientRect').mockReturnValue({
        left: idx * 140,
        right: (idx + 1) * 140,
        width: 140,
        top: 0,
        bottom: 32,
        height: 32,
        x: idx * 140,
        y: 0,
        toJSON: () => ({}),
      });
    });

    const createdHeaderBtn = view.getByText('Created Time').closest('button')!;
    fireEvent.mouseDown(createdHeaderBtn, { button: 0, clientX: 620, clientY: 16 });
    fireEvent.mouseMove(window, { clientX: 20, clientY: 16 });
    fireEvent.mouseUp(window, { clientX: 20, clientY: 16 });
    // 默认列全集已升级为 6 列（含打开次数 runCount），拖拽重排时必须保留隐藏默认列在 fullOrder 中的位置
    expect(onReorder).toHaveBeenCalledWith(['created', 'name', 'modified', 'path', 'size', 'runCount']);
    expect(onSelectSort).not.toHaveBeenCalled();

    // 跨列松手未派发 click；经过 0ms 宏任务兜底后，再次点击排序必须立刻触发
    act(() => {
      vi.advanceTimersByTime(10);
    });
    const nameHeaderBtn = view.getByText('File Name').closest('button')!;
    fireEvent.click(nameHeaderBtn);
    expect(onSelectSort).toHaveBeenCalledWith('name');

    // 4. 右键表头可打开列显示菜单并支持重置
    const headerEl = document.querySelector<HTMLElement>('.search-table-header')!;
    fireEvent.contextMenu(headerEl, { clientX: 200, clientY: 20 });
    const resetBtn = view.getByRole('menuitem', { name: 'Reset column order & widths' });
    fireEvent.click(resetBtn);
    expect(onResetCols).toHaveBeenCalledTimes(1);

    // 5. hidden=true 时表头保活 DOM (display: none) 而不销毁
    view.rerender(
      <SearchTableHeader
        columns={customCols}
        sortField="modified"
        sortDirection="desc"
        onSelectSort={onSelectSort}
        onUpdateNameAndPathFlex={vi.fn()}
        hidden={true}
      />,
    );
    const hiddenHeader = document.querySelector<HTMLElement>('.search-table-header');
    expect(hiddenHeader).not.toBeNull();
    expect(hiddenHeader!.style.display).toBe('none');
    vi.useRealTimers();
  });

  it('freely resizes the Directory (path) column wider or narrower in default 4-column layout without sticking to Size or 1fr bounce', () => {
    const defaultCols: ColumnLayout = {
      name: true,
      path: true,
      size: true,
      modified: true,
      created: false,
      snippets: false,
      nameFlex: 32,
      pathFlex: 42,
      tableOrder: ['name', 'path', 'size', 'modified'],
      tableWidths: {},
    };

    const onUpdateWidth = vi.fn();
    const onBatchUpdate = vi.fn();

    render(
      <SearchTableHeader
        columns={defaultCols}
        sortField="name"
        sortDirection="asc"
        onSelectSort={vi.fn()}
        onUpdateNameAndPathFlex={vi.fn()}
        onUpdateTableColumnWidth={onUpdateWidth}
        onBatchUpdateTableColumnWidths={onBatchUpdate}
      />,
    );

    const nameWrap = document.querySelector<HTMLElement>('.search-table-th-wrap[data-table-col="name"]')!;
    const pathWrap = document.querySelector<HTMLElement>('.search-table-th-wrap[data-table-col="path"]')!;
    vi.spyOn(nameWrap, 'getBoundingClientRect').mockReturnValue({
      left: 0, right: 300, width: 300, top: 0, bottom: 32, height: 32, x: 0, y: 0, toJSON: () => ({}),
    });
    vi.spyOn(pathWrap, 'getBoundingClientRect').mockReturnValue({
      left: 314, right: 714, width: 400, top: 0, bottom: 32, height: 32, x: 314, y: 0, toJSON: () => ({}),
    });

    // 1. 向右大幅拖宽 path (+220px)：冻结左侧弹性列 name=300px，path 精确增至 620px，绝不改右侧 size
    const pathResizer = document.querySelector<HTMLElement>('[data-resize-col="path"]')!;
    fireEvent.mouseDown(pathResizer, { button: 0, clientX: 714 });
    fireEvent.mouseMove(window, { clientX: 934 });
    expect(onBatchUpdate).toHaveBeenCalledWith({ name: 300, path: 620 });
    expect(onUpdateWidth).not.toHaveBeenCalledWith('size', expect.anything());

    // 2. 向左大幅缩窄 path (-200px)：精确缩至 200px
    fireEvent.mouseMove(window, { clientX: 514 });
    expect(onUpdateWidth).toHaveBeenLastCalledWith('path', 200);
    fireEvent.mouseUp(window);

    // 3. 当 name 和 path 均已锁宽后，CSS Grid 严格渲染固定像素宽，绝不回退为 minmax(200px, 1fr) 导致缩不回去
    const lockedCols: ColumnLayout = {
      ...defaultCols,
      tableWidths: { name: 300, path: 200 },
    };
    expect(buildTableGridTemplate(lockedCols)).toBe('300px 200px 96px 152px');
  });
});

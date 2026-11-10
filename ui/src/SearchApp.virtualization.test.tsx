/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { VirtualSearchResults } from './search/VirtualSearchResults';
import type { ColumnLayout, SearchResult } from './search/searchTypes';

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
  name: true, ext: false, parent: false, path: true, size: false,
  modified: false, created: false, snippets: false, nameFlex: 1, pathFlex: 1,
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

describe('VirtualSearchResults', () => {
  it('renders every enabled result property, including authoritative timestamps', () => {
    const item: SearchResult = {
      name: 'report.pdf',
      path: 'D:\\Chosen\\Project\\report.pdf',
      isDirectory: false,
      size: 2048,
      creationTime: 134000000000000000,
      lastWriteTime: 134001000000000000,
    };
    const allColumns: ColumnLayout = {
      name: true, ext: true, parent: true, path: true, size: true,
      modified: true, created: true, snippets: true, nameFlex: 1, pathFlex: 1,
    };

    const view = render(<VirtualSearchResults {...props([item])} columns={allColumns} />);

    expect(view.getByText('report.pdf')).not.toBeNull();
    expect(view.getByText('PDF')).not.toBeNull();
    expect(view.getByTitle('Folder: Project')).not.toBeNull();
    expect(view.getByTitle(item.path)).not.toBeNull();
    expect(view.getByTitle('Size').textContent).toBe('2.0 KB');
    expect(view.getByTitle('Modified').textContent).toMatch(/^Mod\d{4}-\d{2}-\d{2}/);
    expect(view.getByTitle('Created').textContent).toMatch(/^Cre\d{4}-\d{2}-\d{2}/);
  });

  it('keeps type and parent properties independent from name and full path', () => {
    const item: SearchResult = {
      name: 'report.pdf',
      path: 'D:\\Chosen\\Project\\report.pdf',
      isDirectory: false,
    };
    const independentColumns: ColumnLayout = {
      ...columns,
      name: false,
      ext: true,
      parent: true,
      path: false,
    };

    const view = render(<VirtualSearchResults {...props([item])} columns={independentColumns} />);

    expect(view.getByText('PDF')).not.toBeNull();
    expect(view.getByTitle('Folder: Project')).not.toBeNull();
    expect(view.queryByTitle(item.path)).toBeNull();
  });

  it('keeps a 100k result DOM window bounded and mounts the far active option', () => {
    const items = results(100_000);
    const view = render(<VirtualSearchResults {...props(items)} />);
    const list = view.getByRole('listbox');
    Object.defineProperty(list, 'clientHeight', { configurable: true, value: 480 });

    const initialCount = list.querySelectorAll('[role="option"]').length;
    expect(initialCount).toBeGreaterThan(0);
    expect(initialCount).toBeLessThanOrEqual(20);
    view.rerender(<VirtualSearchResults {...props(items, 99_999)} />);

    const options = list.querySelectorAll('[role="option"]');
    expect(options.length).toBeLessThanOrEqual(16);
    expect(document.getElementById('search-result-99999')).not.toBeNull();
    expect(document.getElementById('search-result-99999')?.getAttribute('data-virtual-index')).toBe('99999');
    expect(document.getElementById('search-result-99999')?.getAttribute('aria-selected')).toBe('true');
    // Selection changes from keyboard navigation must make the active option
    // visible without mounting every item between index 0 and index 99,999.
    expect(list.scrollTop).toBeGreaterThan(5_000_000);
  });

  it('applies ResizeObserver measurements and removes rows from observation on unmount', () => {
    const view = render(<VirtualSearchResults {...props(results(20))} />);
    const firstRow = document.getElementById('search-result-0');
    const secondRow = document.getElementById('search-result-1');
    expect(firstRow).not.toBeNull();
    expect(secondRow).not.toBeNull();

    const rowObserver = ResizeObserverMock.instances.find((observer) => observer.observed.has(firstRow!));
    expect(rowObserver).toBeDefined();
    act(() => rowObserver!.emit(firstRow!, 120));
    expect((secondRow as HTMLElement).style.top).toBe('120px');

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

    // 模拟用户拖拽滚动条至 800px 偏移位置
    list.scrollTop = 800;
    fireEvent.scroll(list);

    // 模拟视口内行元素被 ResizeObserver 动态测高
    const observedRow = document.querySelector<HTMLElement>('[data-virtual-index]');
    expect(observedRow).not.toBeNull();
    const rowObserver = ResizeObserverMock.instances.find((observer) => observer.observed.has(observedRow!));
    expect(rowObserver).toBeDefined();

    act(() => {
      rowObserver!.emit(observedRow!, 72);
    });

    // 验证：动态行高测量绝不强制将已滚动的视口重置回 0
    expect(list.scrollTop).toBe(800);

    // 验证：拖拽滚动条过程中鼠标按键按下 (buttons === 1)，移动光标越过行绝不触发 onHover 盗取选区
    fireEvent.mouseMove(observedRow!, { buttons: 1, movementX: 5, movementY: 5 });
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

  it('requests per-file native icons with full paths for executables and shortcuts (.exe, .lnk)', () => {
    const items: SearchResult[] = [
      { name: 'Tools3000-Setup.exe', path: 'C:\\repo\\Tools3000\\Output\\Tools3000-Setup.exe', isDirectory: false },
      { name: 'Tools3000Tests.exe', path: 'C:\\repo\\Tools3000\\build\\bin\\Release\\Tools3000Tests.exe', isDirectory: false },
      { name: 'Tools3000.lnk', path: 'C:\\Users\\Public\\Desktop\\Tools3000.lnk', isDirectory: false },
    ];
    const onRequestIcon = vi.fn();
    const iconCache: Record<string, string> = {
      'path:c:\\repo\\tools3000\\output\\tools3000-setup.exe': 'data:image/png;base64,AAA',
    };

    render(
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
    expect(onRequestIcon).toHaveBeenNthCalledWith(
      2,
      '.lnk',
      false,
      'C:\\Users\\Public\\Desktop\\Tools3000.lnk',
    );
  });
});


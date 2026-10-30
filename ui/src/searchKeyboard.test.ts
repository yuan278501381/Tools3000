/* @vitest-environment jsdom */

import { renderHook } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { useSearchKeyboard, type UseSearchKeyboardProps } from './search/useSearchKeyboard';
import type { SearchResult } from './search/searchTypes';

function createProps(overrides?: Partial<UseSearchKeyboardProps>): UseSearchKeyboardProps {
  const input = document.createElement('input');
  document.body.appendChild(input);
  const inputRef = { current: input };

  const results: SearchResult[] = [
    { name: 'file1.txt', path: 'C:\\file1.txt', isDirectory: false },
    { name: 'file2.txt', path: 'C:\\file2.txt', isDirectory: false },
  ];

  return {
    inputRef,
    isComposing: false,
    sortedResults: results,
    selectedIndex: 0,
    setSelectedIndex: vi.fn(),
    query: 'test',
    renameTarget: { visible: false, newName: '' },
    setRenameTarget: vi.fn(),
    contextMenu: { visible: false, x: 0, y: 0 },
    setContextMenu: vi.fn(),
    showSortMenu: false,
    setShowSortMenu: vi.fn(),
    showSyntaxHelp: false,
    setShowSyntaxHelp: vi.fn(),
    showViewSettings: false,
    setShowViewSettings: vi.fn(),
    hide: vi.fn(),
    rebuildIndex: vi.fn().mockResolvedValue(undefined),
    togglePin: vi.fn().mockResolvedValue(undefined),
    startRename: vi.fn(),
    handleSelectSort: vi.fn(),
    openResult: vi.fn().mockResolvedValue(undefined),
    openFolderResult: vi.fn().mockResolvedValue(undefined),
    showFileProperties: vi.fn().mockResolvedValue(undefined),
    copyPathResult: vi.fn(),
    exportResultsToCsv: vi.fn(),
    selectCategory: vi.fn(),
    categories: [],
    ...overrides,
  };
}

describe('useSearchKeyboard IME Composition Protection', () => {
  it('does NOT call hide() on Escape when isComposing is true', () => {
    const hide = vi.fn();
    const props = createProps({ isComposing: true, hide });
    const { result } = renderHook(() => useSearchKeyboard(props));

    const event = new KeyboardEvent('keydown', { key: 'Escape', cancelable: true });
    result.current.handleUnifiedKeyDown(event);

    expect(hide).not.toHaveBeenCalled();
    expect(event.defaultPrevented).toBe(false);
  });

  it('does NOT call hide() on Escape when event.isComposing is true', () => {
    const hide = vi.fn();
    const props = createProps({ isComposing: false, hide });
    const { result } = renderHook(() => useSearchKeyboard(props));

    const event = new KeyboardEvent('keydown', { key: 'Escape', cancelable: true });
    Object.defineProperty(event, 'isComposing', { value: true });
    result.current.handleUnifiedKeyDown(event);

    expect(hide).not.toHaveBeenCalled();
    expect(event.defaultPrevented).toBe(false);
  });

  it('does NOT call hide() on Escape when keyCode is 229 (IME Process)', () => {
    const hide = vi.fn();
    const props = createProps({ isComposing: false, hide });
    const { result } = renderHook(() => useSearchKeyboard(props));

    const event = new KeyboardEvent('keydown', { key: 'Process', cancelable: true });
    Object.defineProperty(event, 'keyCode', { value: 229 });
    result.current.handleUnifiedKeyDown(event);

    expect(hide).not.toHaveBeenCalled();
    expect(event.defaultPrevented).toBe(false);
  });

  it('does NOT move selection or prevent default on ArrowDown when isComposing is true', () => {
    const setSelectedIndex = vi.fn();
    const props = createProps({ isComposing: true, setSelectedIndex });
    const { result } = renderHook(() => useSearchKeyboard(props));

    const event = new KeyboardEvent('keydown', { key: 'ArrowDown', cancelable: true });
    result.current.handleUnifiedKeyDown(event);

    expect(setSelectedIndex).not.toHaveBeenCalled();
    expect(event.defaultPrevented).toBe(false);
  });

  it('calls hide() on Escape when NOT composing and no popup is visible', () => {
    const hide = vi.fn();
    const props = createProps({ isComposing: false, hide });
    const { result } = renderHook(() => useSearchKeyboard(props));

    const event = new KeyboardEvent('keydown', { key: 'Escape', cancelable: true });
    result.current.handleUnifiedKeyDown(event);

    expect(hide).toHaveBeenCalledTimes(1);
    expect(event.defaultPrevented).toBe(true);
  });

  it('moves selection on ArrowDown when NOT composing', () => {
    const setSelectedIndex = vi.fn();
    const props = createProps({ isComposing: false, setSelectedIndex });
    const { result } = renderHook(() => useSearchKeyboard(props));

    const event = new KeyboardEvent('keydown', { key: 'ArrowDown', cancelable: true });
    result.current.handleUnifiedKeyDown(event);

    expect(setSelectedIndex).toHaveBeenCalled();
    expect(event.defaultPrevented).toBe(true);
  });

  it('triggers copyFileResult on Ctrl+Shift+C, quickLookResult on F3/Alt+Space, and leaves Ctrl+Space free for OS IME', () => {
    const copyFileResult = vi.fn();
    const quickLookResult = vi.fn();
    const props = createProps({ isComposing: false, copyFileResult, quickLookResult });
    const { result } = renderHook(() => useSearchKeyboard(props));

    const copyEvent = new KeyboardEvent('keydown', { key: 'C', ctrlKey: true, shiftKey: true, cancelable: true });
    result.current.handleUnifiedKeyDown(copyEvent);
    expect(copyFileResult).toHaveBeenCalledTimes(1);
    expect(copyEvent.defaultPrevented).toBe(true);

    const imeEvent = new KeyboardEvent('keydown', { key: ' ', code: 'Space', ctrlKey: true, cancelable: true });
    result.current.handleUnifiedKeyDown(imeEvent);
    expect(quickLookResult).not.toHaveBeenCalled();
    expect(imeEvent.defaultPrevented).toBe(false);

    const f3Event = new KeyboardEvent('keydown', { key: 'F3', cancelable: true });
    result.current.handleUnifiedKeyDown(f3Event);
    expect(quickLookResult).toHaveBeenCalledTimes(1);
    expect(f3Event.defaultPrevented).toBe(true);

    const altSpaceEvent = new KeyboardEvent('keydown', { key: ' ', code: 'Space', altKey: true, cancelable: true });
    result.current.handleUnifiedKeyDown(altSpaceEvent);
    expect(quickLookResult).toHaveBeenCalledTimes(2);
    expect(altSpaceEvent.defaultPrevented).toBe(true);
  });

  it('switches to content search on Enter only when not already in content search mode or syntax', () => {
    const contentCat = { id: 'content', label: 'Content', prefix: 'content:' };
    const selectCategory = vi.fn();

    // 1. Plain name query with empty results -> Enter switches to content category
    const plainProps = createProps({
      sortedResults: [],
      query: 'ext:docx 合同',
      activeCategory: 'doc',
      searchMode: 'name',
      categories: [contentCat],
      selectCategory,
    });
    const { result: plainHook } = renderHook(() => useSearchKeyboard(plainProps));
    plainHook.current.handleUnifiedKeyDown(new KeyboardEvent('keydown', { key: 'Enter', cancelable: true }));
    expect(selectCategory).toHaveBeenCalledTimes(1);
    expect(selectCategory).toHaveBeenCalledWith(contentCat);

    // 2. Mid-query explicit content syntax -> Enter must NOT re-trigger selectCategory
    selectCategory.mockClear();
    const midContentProps = createProps({
      sortedResults: [],
      query: 'ext:docx content:合同',
      activeCategory: 'content',
      searchMode: 'name',
      categories: [contentCat],
      selectCategory,
    });
    const { result: midHook } = renderHook(() => useSearchKeyboard(midContentProps));
    midHook.current.handleUnifiedKeyDown(new KeyboardEvent('keydown', { key: 'Enter', cancelable: true }));
    expect(selectCategory).not.toHaveBeenCalled();

    // 3. searchMode === 'content' -> Enter must NOT re-trigger selectCategory
    const modeContentProps = createProps({
      sortedResults: [],
      query: '合同',
      activeCategory: 'all',
      searchMode: 'content',
      categories: [contentCat],
      selectCategory,
    });
    const { result: modeHook } = renderHook(() => useSearchKeyboard(modeContentProps));
    modeHook.current.handleUnifiedKeyDown(new KeyboardEvent('keydown', { key: 'Enter', cancelable: true }));
    expect(selectCategory).not.toHaveBeenCalled();
  });
});

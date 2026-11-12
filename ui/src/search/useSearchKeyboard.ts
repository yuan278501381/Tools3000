import { useCallback, useEffect, useRef, type KeyboardEvent } from 'react';
import { IMAGE_EXTENSIONS } from './searchConstants';
import type { CategoryFilter, SearchMode, SearchResult, SortField } from './searchTypes';
import { isEffectiveContentSearch } from './searchUtils';

export interface UseSearchKeyboardProps {
  inputRef: React.RefObject<HTMLInputElement | null>;
  isComposing: boolean;
  sortedResults: SearchResult[];
  selectedIndex: number;
  setSelectedIndex: React.Dispatch<React.SetStateAction<number>>;
  query: string;
  activeCategory?: string;
  searchMode?: SearchMode;
  renameTarget: { visible: boolean; newName: string; result?: SearchResult };
  setRenameTarget: React.Dispatch<React.SetStateAction<{ visible: boolean; newName: string; result?: SearchResult }>>;
  contextMenu: { visible: boolean; x: number; y: number; result?: SearchResult };
  setContextMenu: React.Dispatch<React.SetStateAction<{ visible: boolean; x: number; y: number; result?: SearchResult }>>;
  showSortMenu: boolean;
  setShowSortMenu: React.Dispatch<React.SetStateAction<boolean>>;
  showSyntaxHelp: boolean;
  setShowSyntaxHelp: React.Dispatch<React.SetStateAction<boolean>>;
  showViewSettings: boolean;
  setShowViewSettings: React.Dispatch<React.SetStateAction<boolean>>;
  hide: () => void;
  rebuildIndex: () => Promise<void>;
  togglePin: () => Promise<void>;
  startRename: (result: SearchResult) => void;
  handleSelectSort: (field: SortField) => void;
  openResult: (result?: SearchResult) => Promise<void>;
  openFolderResult: (result?: SearchResult) => Promise<void>;
  pinResult?: (result?: SearchResult) => Promise<void>;
  openResultAsAdmin?: (result?: SearchResult) => Promise<void>;
  showFileProperties: (result?: SearchResult) => Promise<void>;
  copyPathResult: (result?: SearchResult) => void;
  copyFileResult?: (result?: SearchResult) => Promise<void> | void;
  quickLookResult?: (result?: SearchResult, options?: { toggle?: boolean }) => Promise<void> | void;
  exportResultsToCsv: () => void;
  selectCategory: (cat: CategoryFilter) => void;
  categories: CategoryFilter[];
}

export function useSearchKeyboard({
  inputRef,
  isComposing,
  sortedResults,
  selectedIndex,
  setSelectedIndex,
  query,
  activeCategory = 'all',
  searchMode = 'name',
  renameTarget,
  setRenameTarget,
  contextMenu,
  setContextMenu,
  showSortMenu,
  setShowSortMenu,
  showSyntaxHelp,
  setShowSyntaxHelp,
  showViewSettings,
  setShowViewSettings,
  hide,
  rebuildIndex,
  togglePin,
  startRename,
  handleSelectSort,
  openResult,
  openFolderResult,
  pinResult,
  openResultAsAdmin,
  showFileProperties,
  copyPathResult,
  copyFileResult,
  quickLookResult,
  exportResultsToCsv,
  selectCategory,
  categories,
}: UseSearchKeyboardProps) {
  const selectedIndexRef = useRef(selectedIndex);

  useEffect(() => {
    selectedIndexRef.current = selectedIndex;
  }, [selectedIndex]);

  const moveSelection = useCallback((direction: 'ArrowDown' | 'ArrowUp') => {
    if (sortedResults.length === 0) return;
    const delta = direction === 'ArrowDown' ? 1 : -1;
    setSelectedIndex((previous) => (
      previous + delta + sortedResults.length
    ) % sortedResults.length);
  }, [setSelectedIndex, sortedResults.length]);

  const handleUnifiedKeyDown = useCallback((event: KeyboardEvent<HTMLElement> | globalThis.KeyboardEvent) => {
    // 0. Windows TSF / IME 组字输入期间，拦截所有全局键盘导航与窗口隐藏逻辑，
    // 保障输入法选词、取消、退格以及系统输入法热键（如 Ctrl+Space）100% 独占不冲突
    if (
      isComposing ||
      ('isComposing' in event && event.isComposing) ||
      ('keyCode' in event && (event.keyCode === 229 || event.which === 229)) ||
      event.key === 'Process'
    ) {
      return;
    }

    const target = event.target as HTMLElement | null;
    const isSearchInput = target === inputRef.current;
    const isOtherInput = target && target !== inputRef.current && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable);

    // 1. Escape 拥有最高全局优先级（逐层退出浮层 -> 最终退出搜索窗口）
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      if (renameTarget.visible) {
        setRenameTarget({ visible: false, newName: '' });
        inputRef.current?.focus();
      } else if (contextMenu.visible) {
        setContextMenu({ visible: false, x: 0, y: 0 });
        inputRef.current?.focus();
      } else if (showSortMenu) {
        setShowSortMenu(false);
        inputRef.current?.focus();
      } else if (showSyntaxHelp) {
        setShowSyntaxHelp(false);
        inputRef.current?.focus();
      } else if (showViewSettings) {
        setShowViewSettings(false);
        inputRef.current?.focus();
      } else {
        hide();
      }
      return;
    }

    // 1b. 模态框、抽屉与弹出菜单打开时，阻断背景结果列表的全局快捷键穿透
    if (renameTarget.visible) {
      return;
    }
    if (showSyntaxHelp) {
      if (!event.ctrlKey && !event.metaKey && !event.altKey && !event.shiftKey && event.key === 'F1') {
        event.preventDefault();
        event.stopPropagation();
        setShowSyntaxHelp(false);
        inputRef.current?.focus();
      }
      return;
    }
    if (showViewSettings || contextMenu.visible || showSortMenu) {
      return;
    }

    // 2. F5 / Ctrl+R 全局重新构建全盘索引
    if (event.key === 'F5' || (event.ctrlKey && !event.shiftKey && !event.altKey && (event.key === 'r' || event.key === 'R'))) {
      event.preventDefault();
      void rebuildIndex();
      return;
    }

    // 3. F1 全局语法帮助
    if (!event.ctrlKey && !event.metaKey && !event.altKey && !event.shiftKey && event.key === 'F1') {
      event.preventDefault();
      setShowSyntaxHelp(prev => !prev);
      return;
    }

    // 4. Ctrl+P 全局切换窗口图钉固定状态 (Pin/Unpin)
    if ((event.ctrlKey || event.metaKey) && !event.shiftKey && !event.altKey && (event.key === 'p' || event.key === 'P')) {
      event.preventDefault();
      void togglePin();
      return;
    }

    if (isOtherInput) {
      return;
    }

    // 4b. Ctrl+T 将当前选中图片独立贴图置顶到桌面
    if ((event.ctrlKey || event.metaKey) && !event.shiftKey && !event.altKey && (event.key === 't' || event.key === 'T')) {
      const current = sortedResults[selectedIndexRef.current];
      if (current && !current.isDirectory && IMAGE_EXTENSIONS.test(current.name) && pinResult) {
        event.preventDefault();
        void pinResult(current);
        return;
      }
    }

    // 5. F2 全局重命名当前选中项
    if (!event.ctrlKey && !event.metaKey && !event.altKey && !event.shiftKey && event.key === 'F2') {
      event.preventDefault();
      const current = sortedResults[selectedIndexRef.current];
      if (current) startRename(current);
      return;
    }

    // 6. Ctrl+Shift+... 排序与复制文件快捷键
    if (event.ctrlKey && event.shiftKey && !event.altKey) {
      const k = event.key.toLowerCase();
      if (k === 'd') { event.preventDefault(); handleSelectSort('modified'); return; }
      if (k === 'n') { event.preventDefault(); handleSelectSort('name'); return; }
      if (k === 's') { event.preventDefault(); handleSelectSort('size'); return; }
      if (k === 'r') { event.preventDefault(); handleSelectSort('relevance'); return; }
      if (k === 'c' && copyFileResult) {
        const current = sortedResults[selectedIndexRef.current];
        if (current) {
          event.preventDefault();
          void copyFileResult(current);
          return;
        }
      }
    }

    // 6a. Tab / Shift+Tab 循环切换顶部分类胶囊（全键盘流零断层）
    if (event.key === 'Tab' && !event.ctrlKey && !event.altKey && !event.metaKey && categories.length > 0) {
      event.preventDefault();
      const currIdx = categories.findIndex(c => c.id === activeCategory);
      const safeIdx = currIdx >= 0 ? currIdx : 0;
      const delta = event.shiftKey ? -1 : 1;
      const nextCat = categories[(safeIdx + delta + categories.length) % categories.length];
      if (nextCat) {
        selectCategory(nextCat);
      }
      return;
    }

    // 6a-2. Shift+F10 或 ContextMenu 键为当前选中结果打开右键上下文菜单
    if (
      (event.shiftKey && !event.ctrlKey && !event.altKey && !event.metaKey && event.key === 'F10') ||
      event.key === 'ContextMenu'
    ) {
      const current = sortedResults[selectedIndexRef.current];
      if (current) {
        event.preventDefault();
        event.stopPropagation();
        const rowEl = document.getElementById(`search-result-${selectedIndexRef.current}`);
        const rect = rowEl?.getBoundingClientRect();
        const menuX = rect && rect.width > 0 ? Math.min(rect.left + 120, window.innerWidth - 250) : 160;
        const menuY = rect && rect.height > 0 ? Math.min(rect.bottom - 4, window.innerHeight - 320) : 140;
        setContextMenu({
          visible: true,
          x: Math.max(10, menuX),
          y: Math.max(10, menuY),
          result: current,
        });
        return;
      }
    }

    // 6b. F3 / Alt+Space（或焦点位于结果列表时单按 Space）快速预览当前选中项 (QuickLook)，避让系统输入法热键 Ctrl+Space
    const isInteractiveButtonOrLink = Boolean(
      typeof target?.closest === 'function' &&
        target.closest('button, a, select, [role="button"], [role="menuitem"]'),
    );
    const isInputFocused = isSearchInput || document.activeElement === inputRef.current;
    const isPlainSpaceInList =
      !isInputFocused &&
      !isInteractiveButtonOrLink &&
      !event.ctrlKey &&
      !event.metaKey &&
      !event.shiftKey &&
      !event.altKey &&
      (event.key === ' ' || event.code === 'Space');
    if (
      (!event.ctrlKey && !event.metaKey && !event.shiftKey && !event.altKey && event.key === 'F3') ||
      (event.altKey && !event.ctrlKey && !event.metaKey && !event.shiftKey && (event.key === ' ' || event.code === 'Space')) ||
      isPlainSpaceInList
    ) {
      const current = sortedResults[selectedIndexRef.current] ?? sortedResults[0];
      if (current && quickLookResult) {
        event.preventDefault();
        event.stopPropagation();
        void quickLookResult(current);
        return;
      }
    }

    if (event.key === 'ArrowDown') {
      event.preventDefault();
      moveSelection('ArrowDown');
      return;
    }

    if (event.key === 'ArrowUp') {
      event.preventDefault();
      moveSelection('ArrowUp');
      return;
    }

    if (event.key === 'PageDown') {
      event.preventDefault();
      if (sortedResults.length > 0) {
        setSelectedIndex((prev) => Math.min(sortedResults.length - 1, prev + 10));
      }
      return;
    }

    if (event.key === 'PageUp') {
      event.preventDefault();
      if (sortedResults.length > 0) {
        setSelectedIndex((prev) => Math.max(0, prev - 10));
      }
      return;
    }

    if (event.key === 'Home' && !isSearchInput) {
      event.preventDefault();
      setSelectedIndex(0);
      return;
    }

    if (event.key === 'End' && !isSearchInput) {
      event.preventDefault();
      if (sortedResults.length > 0) {
        setSelectedIndex(sortedResults.length - 1);
      }
      return;
    }

    if (event.key === 'Enter' && event.ctrlKey) {
      event.preventDefault();
      const current = sortedResults[selectedIndexRef.current];
      if (current) {
        if (event.shiftKey && !current.isDirectory && /\.(exe|bat|cmd|ps1|msi|vbs)$/i.test(current.name) && openResultAsAdmin) {
          void openResultAsAdmin(current);
        } else {
          void openFolderResult(current);
        }
      }
      return;
    }

    if (event.key === 'Enter' && event.altKey) {
      event.preventDefault();
      const current = sortedResults[selectedIndexRef.current];
      if (current) {
        void showFileProperties(current);
      }
      return;
    }

    if (event.key === 'Enter' && !isComposing) {
      event.preventDefault();
      const current = sortedResults[selectedIndexRef.current];
      if (current) {
        void openResult(current);
      } else if (
        query.trim() &&
        !isEffectiveContentSearch({ query, activeCategory, searchMode })
      ) {
        const contentCat = categories.find(c => c.id === 'content') || categories[1];
        if (contentCat) selectCategory(contentCat);
      }
      return;
    }

    if ((event.ctrlKey || event.metaKey) && !event.shiftKey && !event.altKey && (event.key === 'c' || event.key === 'C')) {
      const inputEl = inputRef.current;
      const hasInputSelection = Boolean(
        inputEl &&
        isInputFocused &&
        inputEl.selectionStart !== null &&
        inputEl.selectionEnd !== null &&
        inputEl.selectionStart !== inputEl.selectionEnd,
      );
      if (hasInputSelection || (isInputFocused && window.getSelection()?.toString())) {
        return;
      }
      event.preventDefault();
      const current = sortedResults[selectedIndexRef.current];
      if (current) {
        copyPathResult(current);
      }
      return;
    }

    if ((event.ctrlKey || event.metaKey) && !event.shiftKey && !event.altKey && (event.key === 'e' || event.key === 'E')) {
      event.preventDefault();
      exportResultsToCsv();
      return;
    }

    if (event.altKey && (event.key === '1' || event.key === '2' || event.key === '3' || event.key === '4' || event.key === '5' || event.key === '6' || event.key === '7' || event.key === '8' || event.key === '9')) {
      event.preventDefault();
      const idx = parseInt(event.key, 10) - 1;
      if (idx < categories.length) {
        selectCategory(categories[idx]);
      }
      return;
    }

    if (!isSearchInput && event.key.length === 1 && !event.ctrlKey && !event.altKey && !event.metaKey) {
      inputRef.current?.focus();
    }
  }, [
    inputRef, isComposing, sortedResults, setSelectedIndex, query, activeCategory, searchMode,
    renameTarget.visible, setRenameTarget, contextMenu.visible, setContextMenu,
    showSortMenu, setShowSortMenu, showSyntaxHelp, setShowSyntaxHelp,
    showViewSettings, setShowViewSettings, hide, rebuildIndex, togglePin,
    startRename, handleSelectSort, openResult, openFolderResult, pinResult, openResultAsAdmin,
    showFileProperties, copyPathResult, copyFileResult, quickLookResult, exportResultsToCsv, selectCategory, categories,
    moveSelection
  ]);

  useEffect(() => {
    const handleGlobal = (e: globalThis.KeyboardEvent) => {
      handleUnifiedKeyDown(e);
    };
    window.addEventListener('keydown', handleGlobal, true);
    const handleTriggerQuickLook = () => {
      if (renameTarget.visible || showSyntaxHelp || showViewSettings || contextMenu.visible || showSortMenu) {
        return;
      }
      const current = sortedResults[selectedIndexRef.current] ?? sortedResults[0];
      if (current && quickLookResult) {
        void quickLookResult(current);
      }
    };
    const handleSyncQuickLook = () => {
      if (renameTarget.visible || showSyntaxHelp || showViewSettings || contextMenu.visible || showSortMenu) {
        return;
      }
      const current = sortedResults[selectedIndexRef.current] ?? sortedResults[0];
      if (current && quickLookResult) {
        void quickLookResult(current, { toggle: false });
      }
    };
    const handleStepQuickLook = (event: Event) => {
      if (sortedResults.length === 0) return;
      const customEvent = event as CustomEvent<number>;
      const delta = typeof customEvent.detail === 'number' && customEvent.detail < 0 ? -1 : 1;
      const nextIndex = (selectedIndexRef.current + delta + sortedResults.length) % sortedResults.length;
      selectedIndexRef.current = nextIndex;
      setSelectedIndex(nextIndex);
      const nextItem = sortedResults[nextIndex];
      if (nextItem && quickLookResult) {
        void quickLookResult(nextItem, { toggle: false });
      }
    };
    window.addEventListener('tools3000:triggerQuickLook', handleTriggerQuickLook);
    window.addEventListener('tools3000:syncQuickLook', handleSyncQuickLook);
    window.addEventListener('tools3000:stepQuickLook', handleStepQuickLook);
    return () => {
      window.removeEventListener('keydown', handleGlobal, true);
      window.removeEventListener('tools3000:triggerQuickLook', handleTriggerQuickLook);
      window.removeEventListener('tools3000:syncQuickLook', handleSyncQuickLook);
      window.removeEventListener('tools3000:stepQuickLook', handleStepQuickLook);
    };
  }, [
    contextMenu.visible,
    handleUnifiedKeyDown,
    quickLookResult,
    renameTarget.visible,
    setSelectedIndex,
    showSortMenu,
    showSyntaxHelp,
    showViewSettings,
    sortedResults,
  ]);

  return { handleUnifiedKeyDown };
}

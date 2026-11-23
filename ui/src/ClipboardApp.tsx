/* ─────────────────────────────────────────────────────────────────────────────
 * ClipboardApp — 极速呼出剪贴板历史无边框悬浮面板 (Alt+V)
 *
 * 世界级核心能力:
 *   1. 全键盘极速流：Alt+1..9 秒贴前 9 条（Alt+Shift+1..9 纯文本秒贴）、上下导航、
 *      Enter 贴入、Shift+Enter 纯文本剥离、Ctrl+P 置顶、Delete 删除、Esc 秒关；
 *   2. 批量多选合并粘贴 & 批量删除 & 批量归组：Ctrl 多选 / Shift 范围连选 / 多选开关，
 *      严格按勾选顺序合并粘贴（支持换行/空行/空格分隔符）；
 *   3. 富文本 HTML/RTF 样式原样保留（惰性按需加载，列表 0 大载荷），随时 Shift+Enter 剥离纯文本；
 *   4. 自定义收藏分组 / 文件夹 CRUD 与筛选 + 纯文本详情区一键内联编辑；
 *   5. 100% 零 Emoji + 8 语言国际化 + 实时同步系统主题 + 原生拖动/置顶/8向缩放/最大化。
 * ───────────────────────────────────────────────────────────────────────────── */

import {
  useState,
  useEffect,
  useRef,
  useCallback,
  useMemo,
  type FC,
  type KeyboardEvent as ReactKeyboardEvent,
  type MouseEvent as ReactMouseEvent,
} from 'react';
import { useTranslation } from 'react-i18next';
import {
  ClipboardList,
  Search,
  X,
  Pin,
  PinOff,
  Maximize2,
  Minimize2,
  Trash2,
  Copy,
  FileText,
  Image as ImageIcon,
  FolderOpen,
  FolderPlus,
  Folder,
  Palette,
  Languages,
  ScanText,
  StickyNote,
  CornerDownLeft,
  CheckSquare,
  Square,
  ListChecks,
  Layers,
  Pencil,
  Check,
  Sparkles,
} from 'lucide-react';
import { CodeBadge } from './components/UIKit';
import {
  applyClipboardItemClick,
  applyClipboardSelectAll,
  applyClipboardShiftArrow,
  applyClipboardToggleAtFocus,
  createInitialClipboardSelectionState,
  pruneClipboardSelection,
  type ClipboardSelectionState,
} from './clipboardSelection';
import { bridgeRequest, useBridgeEvent } from './hooks/useBridge';
import './ClipboardApp.css';

export interface ClipboardItemSummary {
  id: number;
  kind: 'text' | 'image' | 'files' | 'color';
  preview: string;
  text?: string;
  hasRichText?: boolean;
  groupId?: string;
  charCount: number;
  lineCount: number;
  imageFile: string;
  imageWidth: number;
  imageHeight: number;
  sourceApp: string;
  colorHex: string;
  byteSize: number;
  createdAt: number;
  updatedAt: number;
  pinned: boolean;
  copyCount: number;
}

export interface ClipboardGroupInfo {
  id: string;
  name: string;
  count: number;
  createdAt: number;
}

interface ClipboardQueryResponse {
  total: number;
  matched: number;
  items: ClipboardItemSummary[];
}

interface ClipboardGroupsResponse {
  groups: ClipboardGroupInfo[];
}

type FilterKind = 'all' | 'pinned' | 'text' | 'image' | 'files' | 'color';
type MergeSeparator = '\n' | '\n\n' | ' ';

const BLOBS_BASE_URL = 'https://clipboard-blobs.tools3000.local/';

function formatBytesCompact(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

function formatRelativeTime(epochMs: number): string {
  if (!epochMs) return '';
  const diffSec = Math.max(0, Math.floor((Date.now() - epochMs) / 1000));
  if (diffSec < 60) return `${diffSec}s`;
  const diffMin = Math.floor(diffSec / 60);
  if (diffMin < 60) return `${diffMin}m`;
  const diffHr = Math.floor(diffMin / 60);
  if (diffHr < 24) return `${diffHr}h`;
  return `${Math.floor(diffHr / 24)}d`;
}

export const ClipboardApp: FC = () => {
  const { t } = useTranslation();
  const [query, setQuery] = useState('');
  const [filterKind, setFilterKind] = useState<FilterKind>('all');
  const [activeGroupId, setActiveGroupId] = useState('');
  const [groups, setGroups] = useState<ClipboardGroupInfo[]>([]);
  const [items, setItems] = useState<ClipboardItemSummary[]>([]);
  const [totalCount, setTotalCount] = useState(0);
  const [detailCache, setDetailCache] = useState<{ id: number; text: string } | null>(null);
  const [loading, setLoading] = useState(false);
  const [windowPinned, setWindowPinned] = useState(false);
  const [windowMaximized, setWindowMaximized] = useState(false);

  // 批量多选状态（固定锚点模型 + 严格保序数组，用于按勾选先后顺序合并粘贴）
  const [multiSelectMode, setMultiSelectMode] = useState(false);
  const [selection, setSelection] = useState<ClipboardSelectionState>(createInitialClipboardSelectionState);
  const [mergeSeparator, setMergeSeparator] = useState<MergeSeparator>('\n');

  // 自定义收藏分组内联创建 / 重命名状态
  const [creatingGroup, setCreatingGroup] = useState(false);
  const [newGroupName, setNewGroupName] = useState('');
  const [renamingGroupId, setRenamingGroupId] = useState<string | null>(null);
  const [renameGroupDraft, setRenameGroupDraft] = useState('');

  // 纯文本详情内联编辑状态
  const [editingItemId, setEditingItemId] = useState<number | null>(null);
  const [editDraft, setEditDraft] = useState('');
  const [savingEdit, setSavingEdit] = useState(false);

  const searchInputRef = useRef<HTMLInputElement>(null);
  const editTextareaRef = useRef<HTMLTextAreaElement>(null);
  const newGroupInputRef = useRef<HTMLInputElement>(null);
  const itemBtnRefs = useRef<Map<number, HTMLButtonElement>>(new Map());
  const handledInMouseDownRef = useRef<number | null>(null);

  const selectedIndex = selection.focusIndex;
  const selectedIds = selection.selectedIds;
  const itemIds = useMemo(() => items.map((it) => it.id), [items]);

  const setFocusIndexOnly = useCallback((nextIdx: number) => {
    setSelection((prev) => ({
      ...prev,
      focusIndex: nextIdx,
      anchorIndex: nextIdx,
      shiftBaseIds: null,
    }));
  }, []);

  const clearMultiSelection = useCallback(() => {
    setSelection((prev) => ({
      ...prev,
      selectedIds: [],
      shiftBaseIds: null,
    }));
    setMultiSelectMode(false);
  }, []);

  const selectedItem = items[selectedIndex] ?? null;
  const selectedIdSet = useMemo(() => new Set(selectedIds), [selectedIds]);
  const selectedTextableCount = useMemo(() => {
    const byId = new Map(items.map((it) => [it.id, it]));
    let n = 0;
    for (const id of selectedIds) {
      const it = byId.get(id);
      if (it && it.kind !== 'image') n += 1;
    }
    return n;
  }, [items, selectedIds]);

  const detailText =
    selectedItem && selectedItem.kind !== 'image'
      ? detailCache && detailCache.id === selectedItem.id
        ? detailCache.text
        : selectedItem.preview
      : '';

  const isEditingCurrent = Boolean(selectedItem && editingItemId === selectedItem.id);

  const fetchGroups = useCallback(async () => {
    try {
      const res = await bridgeRequest<ClipboardGroupsResponse>('clipboard.listGroups', undefined, { silent: true });
      if (res && Array.isArray(res.groups)) {
        setGroups(res.groups);
      }
    } catch {
      // 静默兜底
    }
  }, []);

  const fetchItems = useCallback(async (q: string, kind: FilterKind, groupId: string) => {
    setLoading(true);
    try {
      const res = await bridgeRequest<ClipboardQueryResponse>(
        'clipboard.query',
        { query: q, kind, groupId, limit: 120, offset: 0 },
        { silent: true },
      );
      if (res && Array.isArray(res.items)) {
        setItems(res.items);
        setTotalCount(typeof res.total === 'number' ? res.total : res.items.length);
        const aliveList = res.items.map((it) => it.id);
        setSelection((prev) => pruneClipboardSelection(prev, aliveList));
      }
    } catch {
      // 静默兜底
    } finally {
      setLoading(false);
    }
  }, []);

  const signalFrameReady = useCallback(() => {
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        void bridgeRequest('clipboard.frameReady', undefined, { silent: true }).catch(() => {});
      });
    });
  }, []);

  useEffect(() => {
    let cancelled = false;
    const initTimer = window.setTimeout(() => {
      if (cancelled) return;
      void Promise.all([
        bridgeRequest<ClipboardGroupsResponse>('clipboard.listGroups', undefined, { silent: true })
          .then((res) => {
            if (!cancelled && res && Array.isArray(res.groups)) {
              setGroups(res.groups);
            }
          })
          .catch(() => {}),
        fetchItems('', 'all', ''),
      ]).finally(() => {
        if (!cancelled) {
          signalFrameReady();
        }
      });
    }, 0);
    return () => {
      cancelled = true;
      window.clearTimeout(initTimer);
    };
  }, [fetchItems, signalFrameReady]);

  const initialQueryEffectRef = useRef(true);
  useEffect(() => {
    if (initialQueryEffectRef.current) {
      initialQueryEffectRef.current = false;
      return;
    }
    const timer = window.setTimeout(() => {
      void fetchItems(query, filterKind, activeGroupId);
    }, query ? 40 : 0);
    return () => window.clearTimeout(timer);
  }, [query, filterKind, activeGroupId, fetchItems]);

  useEffect(() => {
    if (!selectedItem || selectedItem.kind === 'image') {
      return;
    }
    let cancelled = false;
    bridgeRequest<{ success?: boolean; item?: ClipboardItemSummary }>(
      'clipboard.getItem',
      { id: selectedItem.id },
      { silent: true },
    )
      .then((res) => {
        if (!cancelled && res?.item?.text !== undefined) {
          setDetailCache({ id: selectedItem.id, text: res.item.text });
        }
      })
      .catch(() => {
        if (!cancelled) setDetailCache({ id: selectedItem.id, text: selectedItem.preview });
      });
    return () => {
      cancelled = true;
    };
  }, [selectedItem]);

  useBridgeEvent('clipboard:shown', () => {
    setFocusIndexOnly(0);
    setEditingItemId(null);
    void Promise.all([
      fetchGroups(),
      fetchItems(query, filterKind, activeGroupId),
    ]).finally(() => {
      signalFrameReady();
      requestAnimationFrame(() => {
        searchInputRef.current?.focus();
        searchInputRef.current?.select();
      });
    });
  });

  useBridgeEvent('clipboard:updated', () => {
    void fetchGroups();
    void fetchItems(query, filterKind, activeGroupId);
  });

  useBridgeEvent('clipboard:windowState', (rawState) => {
    const state = rawState as { pinned?: boolean; maximized?: boolean } | undefined;
    if (typeof state?.pinned === 'boolean') setWindowPinned(state.pinned);
    if (typeof state?.maximized === 'boolean') setWindowMaximized(state.maximized);
  });

  useEffect(() => {
    bridgeRequest<{ pinned?: boolean; maximized?: boolean }>('clipboard.getWindowState', undefined, { silent: true })
      .then((res) => {
        if (typeof res?.pinned === 'boolean') setWindowPinned(res.pinned);
        if (typeof res?.maximized === 'boolean') setWindowMaximized(res.maximized);
      })
      .catch(() => {});
  }, []);

  const handleToggleWindowPin = useCallback(async () => {
    try {
      const res = await bridgeRequest<{ pinned?: boolean; maximized?: boolean }>(
        'clipboard.setWindowPinned',
        { pinned: !windowPinned },
        { silent: true },
      );
      if (typeof res?.pinned === 'boolean') setWindowPinned(res.pinned);
    } catch {
      setWindowPinned((prev) => !prev);
    }
  }, [windowPinned]);

  const handleToggleWindowMaximize = useCallback(async () => {
    try {
      const res = await bridgeRequest<{ pinned?: boolean; maximized?: boolean }>(
        'clipboard.toggleMaximize',
        undefined,
        { silent: true },
      );
      if (typeof res?.maximized === 'boolean') setWindowMaximized(res.maximized);
    } catch {
      // 静默兜底
    }
  }, []);

  const handleHeaderMouseDown = useCallback((e: ReactMouseEvent<HTMLElement>) => {
    if (e.button !== 0 || e.detail >= 2) return;
    const target = e.target as HTMLElement | null;
    if (target?.closest('input, button, a, textarea, select, [data-no-drag="true"]')) return;
    // 4px 拖动启动阈值：避免首次按下立刻进入 Win32 SC_MOVE 模态循环而吞掉双击最大化事件
    const startX = e.screenX;
    const startY = e.screenY;
    const cleanup = () => {
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', cleanup);
      window.removeEventListener('blur', cleanup);
    };
    const onMove = (moveEvt: MouseEvent) => {
      if ((moveEvt.buttons & 1) === 0) {
        cleanup();
        return;
      }
      if (Math.hypot(moveEvt.screenX - startX, moveEvt.screenY - startY) >= 4) {
        cleanup();
        void bridgeRequest('clipboard.startDrag', undefined, { silent: true });
      }
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', cleanup);
    window.addEventListener('blur', cleanup);
  }, []);

  const handleHeaderDoubleClick = useCallback(
    (e: ReactMouseEvent<HTMLElement>) => {
      const target = e.target as HTMLElement | null;
      if (target?.closest('input, button, a, textarea, select, [data-no-drag="true"]')) return;
      void handleToggleWindowMaximize();
    },
    [handleToggleWindowMaximize],
  );

  const handleStartResize = useCallback(
    (edge: 'n' | 's' | 'e' | 'w' | 'ne' | 'nw' | 'se' | 'sw') => (e: ReactMouseEvent<HTMLDivElement>) => {
      if (e.button !== 0 || windowMaximized) return;
      e.preventDefault();
      e.stopPropagation();
      void bridgeRequest('clipboard.startResize', { edge }, { silent: true });
    },
    [windowMaximized],
  );

  const handleActivate = useCallback(
    async (item: ClipboardItemSummary, plainTextOnly: boolean, autoPaste: boolean) => {
      await bridgeRequest('clipboard.pasteItem', {
        id: item.id,
        plainTextOnly,
        autoPaste,
        hideAfter: !windowPinned,
      });
    },
    [windowPinned],
  );

  const handleBatchMergePaste = useCallback(async () => {
    if (selectedIds.length === 0) return;
    await bridgeRequest('clipboard.pasteItemsBatch', {
      ids: selectedIds,
      separator: mergeSeparator,
      autoPaste: true,
      hideAfter: !windowPinned,
    });
    clearMultiSelection();
  }, [selectedIds, mergeSeparator, windowPinned, clearMultiSelection]);

  const handleBatchDelete = useCallback(async () => {
    if (selectedIds.length === 0) return;
    await bridgeRequest('clipboard.deleteItems', {
      ids: selectedIds,
      skipPinned: false,
    });
    clearMultiSelection();
    void fetchGroups();
    void fetchItems(query, filterKind, activeGroupId);
  }, [selectedIds, clearMultiSelection, fetchGroups, fetchItems, query, filterKind, activeGroupId]);

  const handleAssignGroup = useCallback(
    async (ids: number[], groupId: string) => {
      if (ids.length === 0) return;
      await bridgeRequest('clipboard.setItemsGroup', { ids, groupId });
      void fetchGroups();
      void fetchItems(query, filterKind, activeGroupId);
    },
    [fetchGroups, fetchItems, query, filterKind, activeGroupId],
  );

  const handleCreateGroupSubmit = useCallback(async () => {
    const trimmed = newGroupName.trim();
    if (!trimmed) {
      setCreatingGroup(false);
      return;
    }
    const res = await bridgeRequest<{ success?: boolean; group?: ClipboardGroupInfo }>(
      'clipboard.createGroup',
      { name: trimmed },
      { silent: true },
    );
    setNewGroupName('');
    setCreatingGroup(false);
    await fetchGroups();
    if (res?.group?.id) {
      setActiveGroupId(res.group.id);
      setFocusIndexOnly(0);
    }
  }, [newGroupName, fetchGroups, setFocusIndexOnly]);

  const handleRenameGroupSubmit = useCallback(async () => {
    if (!renamingGroupId) return;
    const trimmed = renameGroupDraft.trim();
    const targetId = renamingGroupId;
    setRenamingGroupId(null);
    setRenameGroupDraft('');
    if (!trimmed) return;
    await bridgeRequest('clipboard.renameGroup', { id: targetId, name: trimmed }, { silent: true });
    void fetchGroups();
  }, [renamingGroupId, renameGroupDraft, fetchGroups]);

  const handleDeleteGroup = useCallback(
    async (groupId: string) => {
      await bridgeRequest('clipboard.deleteGroup', { id: groupId }, { silent: true });
      if (activeGroupId === groupId) {
        setActiveGroupId('');
      }
      void fetchGroups();
      void fetchItems(query, filterKind, activeGroupId === groupId ? '' : activeGroupId);
    },
    [activeGroupId, fetchGroups, fetchItems, query, filterKind],
  );

  const handleStartEditItem = useCallback(() => {
    if (!selectedItem || selectedItem.kind === 'image' || selectedItem.kind === 'files') return;
    setEditingItemId(selectedItem.id);
    setEditDraft(detailText || selectedItem.preview);
    requestAnimationFrame(() => {
      editTextareaRef.current?.focus();
    });
  }, [selectedItem, detailText]);

  const handleSaveEditItem = useCallback(async () => {
    if (editingItemId === null || savingEdit) return;
    const trimmed = editDraft.trim();
    if (!trimmed) return;
    setSavingEdit(true);
    try {
      const res = await bridgeRequest<{ success?: boolean }>(
        'clipboard.updateItemText',
        { id: editingItemId, text: editDraft },
      );
      if (res?.success) {
        setDetailCache({ id: editingItemId, text: editDraft });
        setEditingItemId(null);
        void fetchItems(query, filterKind, activeGroupId);
      }
    } finally {
      setSavingEdit(false);
    }
  }, [editingItemId, savingEdit, editDraft, fetchItems, query, filterKind, activeGroupId]);

  const handleTogglePin = useCallback(
    async (item: ClipboardItemSummary) => {
      await bridgeRequest('clipboard.togglePin', { id: item.id });
      void fetchItems(query, filterKind, activeGroupId);
    },
    [fetchItems, query, filterKind, activeGroupId],
  );

  const handleDelete = useCallback(
    async (item: ClipboardItemSummary) => {
      await bridgeRequest('clipboard.deleteItem', { id: item.id });
      void fetchGroups();
      void fetchItems(query, filterKind, activeGroupId);
    },
    [fetchGroups, fetchItems, query, filterKind, activeGroupId],
  );

  const handlePinImageAction = useCallback(
    async (item: ClipboardItemSummary, opts: { autoOcr?: boolean; autoTranslate?: boolean }) => {
      await bridgeRequest('clipboard.pinImage', {
        id: item.id,
        autoOcr: Boolean(opts.autoOcr),
        autoTranslate: Boolean(opts.autoTranslate),
      });
    },
    [],
  );

  const scrollItemIntoView = useCallback((idx: number) => {
    const targetItem = items[idx];
    if (!targetItem) return;
    const el = itemBtnRefs.current.get(targetItem.id);
    el?.scrollIntoView?.({ block: 'nearest' });
  }, [items]);

  const applyItemSelection = useCallback(
    (
      item: ClipboardItemSummary,
      idx: number,
      modifiers: { shiftKey: boolean; ctrlKey: boolean; metaKey: boolean },
    ) => {
      if (editingItemId !== null && editingItemId !== item.id) {
        setEditingItemId(null);
      }
      setSelection((prev) =>
        applyClipboardItemClick(prev, itemIds, idx, {
          shiftKey: modifiers.shiftKey,
          ctrlKey: modifiers.ctrlKey,
          metaKey: modifiers.metaKey,
          multiSelectMode,
        }),
      );
    },
    [editingItemId, itemIds, multiSelectMode],
  );

  const handleItemMouseDown = useCallback(
    (item: ClipboardItemSummary, idx: number, e: ReactMouseEvent<HTMLButtonElement>) => {
      if (e.button !== 0) return;
      const isMultiGesture = e.shiftKey || e.ctrlKey || e.metaKey || multiSelectMode;
      if (!isMultiGesture) {
        handledInMouseDownRef.current = null;
        return;
      }
      // 在 mousedown 当拍拦截 Shift/Ctrl 原生文字蓝底选区并即时完成勾选/连选（对齐 Windows 原生 ListView 手感，0 吞点击）
      e.preventDefault();
      try {
        window.getSelection()?.removeAllRanges();
      } catch {
        // 忽略不可用环境
      }
      e.currentTarget.focus({ preventScroll: true });
      handledInMouseDownRef.current = item.id;
      applyItemSelection(item, idx, e);
    },
    [applyItemSelection, multiSelectMode],
  );

  const handleItemClick = useCallback(
    (item: ClipboardItemSummary, idx: number, e: ReactMouseEvent<HTMLButtonElement>) => {
      if (handledInMouseDownRef.current === item.id && e.detail > 0) {
        handledInMouseDownRef.current = null;
        return;
      }
      handledInMouseDownRef.current = null;
      if (e.shiftKey || e.ctrlKey || e.metaKey) {
        e.preventDefault();
      }
      applyItemSelection(item, idx, e);
    },
    [applyItemSelection],
  );

  const handleKeyDown = (e: ReactKeyboardEvent<HTMLDivElement>) => {
    const activeEl = document.activeElement as HTMLElement | null;
    const activeTag = activeEl?.tagName;
    const isInSearchInput = activeEl === searchInputRef.current;
    const isTypingInEditor = activeTag === 'TEXTAREA' || (activeTag === 'INPUT' && !isInSearchInput);

    if (e.key === 'Escape') {
      e.preventDefault();
      if (editingItemId !== null) {
        setEditingItemId(null);
        return;
      }
      if (creatingGroup) {
        setCreatingGroup(false);
        return;
      }
      if (renamingGroupId) {
        setRenamingGroupId(null);
        return;
      }
      if (selectedIds.length > 0 || multiSelectMode) {
        clearMultiSelection();
        return;
      }
      void bridgeRequest('clipboard.hideWindow', undefined, { silent: true });
      return;
    }

    if (isTypingInEditor) {
      return;
    }

    // Ctrl+A：焦点不在非空搜索框时一键全选当前列表（搜索框有输入词时保留原生全选文本）
    if (
      (e.ctrlKey || e.metaKey) &&
      !e.altKey &&
      !e.shiftKey &&
      e.key.toLowerCase() === 'a' &&
      (!isInSearchInput || query.length === 0)
    ) {
      if (itemIds.length > 0) {
        e.preventDefault();
        setSelection((prev) => applyClipboardSelectAll(prev, itemIds));
      }
      return;
    }

    // Space：焦点不在输入框时快速勾选 / 取消当前焦点行
    if (e.key === ' ' && !isInSearchInput && !e.ctrlKey && !e.metaKey && !e.altKey) {
      if (itemIds.length > 0) {
        e.preventDefault();
        setSelection((prev) => applyClipboardToggleAtFocus(prev, itemIds));
      }
      return;
    }

    // Alt+1..9 随时秒贴前 9 条（Alt+Shift+1..9 纯文本秒贴），绝不与搜索输入数字冲突
    if (e.altKey && !e.ctrlKey && !e.metaKey && e.key >= '1' && e.key <= '9') {
      const slot = Number(e.key) - 1;
      const target = items[slot];
      if (target) {
        e.preventDefault();
        void handleActivate(target, e.shiftKey, true);
      }
      return;
    }

    // 非搜索输入态下直接按 1..9 快速选中对应行
    if (
      !e.altKey &&
      !e.ctrlKey &&
      !e.metaKey &&
      !isInSearchInput &&
      e.key >= '1' &&
      e.key <= '9'
    ) {
      const slot = Number(e.key) - 1;
      if (items[slot]) {
        e.preventDefault();
        setFocusIndexOnly(slot);
        scrollItemIntoView(slot);
      }
      return;
    }

    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      if (items.length === 0) return;
      const delta: -1 | 1 = e.key === 'ArrowDown' ? 1 : -1;
      if (e.shiftKey) {
        const nextIdx = Math.max(0, Math.min(items.length - 1, selectedIndex + delta));
        setSelection((prev) => applyClipboardShiftArrow(prev, itemIds, delta, multiSelectMode));
        scrollItemIntoView(nextIdx);
        return;
      }
      const nextIdx = (selectedIndex + delta + items.length) % items.length;
      setFocusIndexOnly(nextIdx);
      scrollItemIntoView(nextIdx);
      return;
    }
    if (e.key === 'Enter') {
      e.preventDefault();
      if (selectedIds.length >= 2) {
        void handleBatchMergePaste();
        return;
      }
      if (selectedItem) {
        void handleActivate(selectedItem, e.shiftKey, true);
      }
      return;
    }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'p' && selectedItem) {
      e.preventDefault();
      void handleTogglePin(selectedItem);
      return;
    }
    if (e.key === 'Delete' && !isInSearchInput) {
      e.preventDefault();
      if (selectedIds.length > 0) {
        void handleBatchDelete();
      } else if (selectedItem) {
        void handleDelete(selectedItem);
      }
    }
  };

  const filters: ReadonlyArray<{ id: FilterKind; label: string }> = [
    { id: 'all', label: t('clipboard.filterAll', 'All') },
    { id: 'pinned', label: t('clipboard.filterPinned', 'Pinned') },
    { id: 'text', label: t('clipboard.filterText', 'Text') },
    { id: 'image', label: t('clipboard.filterImage', 'Images') },
    { id: 'files', label: t('clipboard.filterFiles', 'Files') },
    { id: 'color', label: t('clipboard.filterColor', 'Colors') },
  ];

  const groupNameById = useMemo(() => {
    const map = new Map<string, string>();
    for (const g of groups) map.set(g.id, g.name);
    return map;
  }, [groups]);

  const renderKindIcon = (kind: ClipboardItemSummary['kind']) => {
    switch (kind) {
      case 'image':
        return <ImageIcon size={14} strokeWidth={2.2} aria-hidden="true" />;
      case 'files':
        return <FolderOpen size={14} strokeWidth={2.2} aria-hidden="true" />;
      case 'color':
        return <Palette size={14} strokeWidth={2.2} aria-hidden="true" />;
      default:
        return <FileText size={14} strokeWidth={2.2} aria-hidden="true" />;
    }
  };

  const showCheckboxes = multiSelectMode || selectedIds.length > 0;

  return (
    <div
      className={`clip-popup${windowMaximized ? ' clip-popup--maximized' : ''}`}
      onKeyDown={handleKeyDown}
    >
      {!windowMaximized && (
        <>
          <div className="clip-popup__resize-edge clip-popup__resize-edge--n" onMouseDown={handleStartResize('n')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--s" onMouseDown={handleStartResize('s')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--e" onMouseDown={handleStartResize('e')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--w" onMouseDown={handleStartResize('w')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--nw" onMouseDown={handleStartResize('nw')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--ne" onMouseDown={handleStartResize('ne')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--sw" onMouseDown={handleStartResize('sw')} aria-hidden="true" />
          <div className="clip-popup__resize-edge clip-popup__resize-edge--se" onMouseDown={handleStartResize('se')} aria-hidden="true" />
        </>
      )}

      <header
        className="clip-popup__header"
        onMouseDown={handleHeaderMouseDown}
        onDoubleClick={handleHeaderDoubleClick}
      >
        <div className="clip-popup__drag-handle">
          <ClipboardList size={18} strokeWidth={2.2} aria-hidden="true" />
          <span>{t('clipboard.popupTitle', 'Clipboard')}</span>
        </div>

        <div className="clip-popup__search-wrap">
          <Search size={15} strokeWidth={2.2} className="clip-popup__search-icon" aria-hidden="true" />
          <input
            ref={searchInputRef}
            type="text"
            className="clip-popup__search-input"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder={t('clipboard.searchPlaceholder', 'Search history, color, path or source app...')}
            aria-label={t('clipboard.searchPlaceholder', 'Search history, color, path or source app...')}
            autoFocus
          />
          {query.length > 0 && (
            <button
              type="button"
              className="clip-popup__search-clear"
              onClick={() => setQuery('')}
              aria-label={t('clipboard.clearSearch', 'Clear search')}
            >
              <X size={14} aria-hidden="true" />
            </button>
          )}
        </div>

        <div className="clip-popup__win-controls">
          <button
            type="button"
            className={`clip-popup__win-btn${windowPinned ? ' clip-popup__win-btn--pinned' : ''}`}
            onClick={() => void handleToggleWindowPin()}
            aria-pressed={windowPinned}
            title={
              windowPinned
                ? t('clipboard.unpinWindow', 'Unpin window (auto-hide on blur)')
                : t('clipboard.pinWindow', 'Keep window on top')
            }
            aria-label={
              windowPinned
                ? t('clipboard.unpinWindow', 'Unpin window (auto-hide on blur)')
                : t('clipboard.pinWindow', 'Keep window on top')
            }
          >
            <Pin size={15} strokeWidth={windowPinned ? 2.3 : 2} aria-hidden="true" />
          </button>

          <button
            type="button"
            className="clip-popup__win-btn"
            onClick={() => void handleToggleWindowMaximize()}
            title={
              windowMaximized
                ? t('clipboard.restoreWindow', 'Restore window size')
                : t('clipboard.maximizeWindow', 'Maximize window')
            }
            aria-label={
              windowMaximized
                ? t('clipboard.restoreWindow', 'Restore window size')
                : t('clipboard.maximizeWindow', 'Maximize window')
            }
          >
            {windowMaximized ? <Minimize2 size={15} strokeWidth={2} aria-hidden="true" /> : <Maximize2 size={15} strokeWidth={2} aria-hidden="true" />}
          </button>

          <button
            type="button"
            className="clip-popup__win-btn clip-popup__win-btn--close"
            onClick={() => void bridgeRequest('clipboard.hideWindow', undefined, { silent: true })}
            title={t('clipboard.closeWindow', 'Close')}
            aria-label={t('clipboard.closeWindow', 'Close')}
          >
            <X size={16} aria-hidden="true" />
          </button>
        </div>
      </header>

      <div className="clip-popup__filters" onMouseDown={handleHeaderMouseDown}>
        <div className="clip-popup__filter-pills" role="tablist" aria-label={t('clipboard.filterBarAria', 'Clipboard categories')}>
          {filters.map((f) => (
            <button
              key={f.id}
              type="button"
              role="tab"
              aria-selected={filterKind === f.id}
              className={`clip-popup__pill${filterKind === f.id ? ' clip-popup__pill--active' : ''}`}
              onClick={() => {
                setFilterKind(f.id);
                setFocusIndexOnly(0);
              }}
            >
              <span>{f.label}</span>
            </button>
          ))}
        </div>

        <div className="clip-popup__filter-right">
          <button
            type="button"
            className={`clip-popup__pill${showCheckboxes ? ' clip-popup__pill--active' : ''}`}
            aria-pressed={showCheckboxes}
            onClick={() => {
              if (showCheckboxes) {
                clearMultiSelection();
              } else {
                setMultiSelectMode(true);
              }
            }}
            title={t('clipboard.multiSelectTip', 'Toggle multi-select mode (or Ctrl/Shift+Click)')}
          >
            <ListChecks size={14} aria-hidden="true" />
            <span>{t('clipboard.multiSelectBtn', 'Select')}</span>
          </button>
          <span className="clip-popup__count-badge">
            {t('clipboard.totalCountBadge', { count: totalCount, defaultValue: `${totalCount} items` })}
          </span>
        </div>
      </div>

      {/* 自定义收藏分组栏 */}
      <div className="clip-popup__groups-bar" onMouseDown={handleHeaderMouseDown}>
        <button
          type="button"
          className={`clip-popup__group-chip${activeGroupId === '' ? ' clip-popup__group-chip--active' : ''}`}
          onClick={() => {
            setActiveGroupId('');
            setFocusIndexOnly(0);
          }}
        >
          <Folder size={13} aria-hidden="true" />
          <span>{t('clipboard.groupAll', 'All Groups')}</span>
        </button>

        {groups.map((grp) => (
          <div
            key={grp.id}
            className={`clip-popup__group-chip-wrap${activeGroupId === grp.id ? ' clip-popup__group-chip-wrap--active' : ''}`}
          >
            {renamingGroupId === grp.id ? (
              <input
                type="text"
                className="clip-popup__group-inline-input"
                value={renameGroupDraft}
                onChange={(e) => setRenameGroupDraft(e.target.value)}
                onBlur={() => void handleRenameGroupSubmit()}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') {
                    e.preventDefault();
                    void handleRenameGroupSubmit();
                  } else if (e.key === 'Escape') {
                    e.preventDefault();
                    setRenamingGroupId(null);
                  }
                }}
                autoFocus
                aria-label={t('clipboard.renameGroupAria', 'Rename collection group')}
              />
            ) : (
              <>
                <button
                  type="button"
                  className={`clip-popup__group-chip${activeGroupId === grp.id ? ' clip-popup__group-chip--active' : ''}`}
                  onClick={() => {
                    setActiveGroupId(grp.id);
                    setFocusIndexOnly(0);
                  }}
                >
                  <Folder size={13} aria-hidden="true" />
                  <span>{grp.name}</span>
                  <span className="clip-popup__group-count">{grp.count}</span>
                </button>
                {activeGroupId === grp.id && (
                  <span className="clip-popup__group-mini-actions">
                    <button
                      type="button"
                      className="clip-popup__group-mini-btn"
                      onClick={() => {
                        setRenamingGroupId(grp.id);
                        setRenameGroupDraft(grp.name);
                      }}
                      title={t('clipboard.renameGroup', 'Rename group')}
                      aria-label={t('clipboard.renameGroup', 'Rename group')}
                    >
                      <Pencil size={12} aria-hidden="true" />
                    </button>
                    <button
                      type="button"
                      className="clip-popup__group-mini-btn clip-popup__group-mini-btn--danger"
                      onClick={() => void handleDeleteGroup(grp.id)}
                      title={t('clipboard.deleteGroup', 'Delete group (items are kept)')}
                      aria-label={t('clipboard.deleteGroup', 'Delete group (items are kept)')}
                    >
                      <X size={12} aria-hidden="true" />
                    </button>
                  </span>
                )}
              </>
            )}
          </div>
        ))}

        {creatingGroup ? (
          <div className="clip-popup__group-create-box">
            <input
              ref={newGroupInputRef}
              type="text"
              className="clip-popup__group-inline-input"
              value={newGroupName}
              onChange={(e) => setNewGroupName(e.target.value)}
              placeholder={t('clipboard.newGroupPlaceholder', 'Group name...')}
              aria-label={t('clipboard.newGroupPlaceholder', 'Group name...')}
              onKeyDown={(e) => {
                if (e.key === 'Enter') {
                  e.preventDefault();
                  void handleCreateGroupSubmit();
                } else if (e.key === 'Escape') {
                  e.preventDefault();
                  setCreatingGroup(false);
                  setNewGroupName('');
                }
              }}
              autoFocus
            />
            <button
              type="button"
              className="clip-popup__group-mini-btn"
              onClick={() => void handleCreateGroupSubmit()}
              aria-label={t('clipboard.confirmCreateGroup', 'Create group')}
            >
              <Check size={13} aria-hidden="true" />
            </button>
          </div>
        ) : (
          <button
            type="button"
            className="clip-popup__group-add-btn"
            onClick={() => {
              setCreatingGroup(true);
              setNewGroupName('');
              requestAnimationFrame(() => newGroupInputRef.current?.focus());
            }}
          >
            <FolderPlus size={13} aria-hidden="true" />
            <span>{t('clipboard.newGroupBtn', 'New Group')}</span>
          </button>
        )}
      </div>

      {/* 批量操作工具条（勾选 >= 1 条时展开） */}
      {selectedIds.length > 0 && (
        <div className="clip-popup__batch-bar" role="region" aria-label={t('clipboard.batchBarAria', 'Batch actions')}>
          <div className="clip-popup__batch-info">
            <span className="clip-popup__batch-count">
              {t('clipboard.selectedCount', { count: selectedIds.length, defaultValue: `${selectedIds.length} selected` })}
            </span>
            <button
              type="button"
              className="clip-popup__batch-link"
              onClick={() => setSelection((prev) => applyClipboardSelectAll(prev, itemIds))}
            >
              {t('clipboard.selectAll', 'Select All')}
            </button>
            <button
              type="button"
              className="clip-popup__batch-link"
              onClick={clearMultiSelection}
            >
              {t('clipboard.clearSelection', 'Clear')}
            </button>
          </div>

          <div className="clip-popup__batch-actions">
            {selectedTextableCount >= 1 && (
              <div className="clip-popup__merge-control">
                <select
                  className="clip-popup__compact-select"
                  value={mergeSeparator}
                  onChange={(e) => setMergeSeparator(e.target.value as MergeSeparator)}
                  aria-label={t('clipboard.mergeSeparatorAria', 'Merge separator')}
                >
                  <option value={'\n'}>{t('clipboard.sepNewline', 'Newline')}</option>
                  <option value={'\n\n'}>{t('clipboard.sepBlankLine', 'Blank Line')}</option>
                  <option value={' '}>{t('clipboard.sepSpace', 'Space')}</option>
                </select>
                <button
                  type="button"
                  className="clip-popup__action-btn clip-popup__action-btn--primary"
                  onClick={() => void handleBatchMergePaste()}
                >
                  <Layers size={14} aria-hidden="true" />
                  <span>{t('clipboard.batchMergePaste', 'Merge Paste')}</span>
                </button>
              </div>
            )}

            {groups.length > 0 && (
              <select
                className="clip-popup__compact-select"
                value=""
                onChange={(e) => {
                  const val = e.target.value;
                  if (val === '__ungroup__') {
                    void handleAssignGroup(selectedIds, '');
                  } else if (val) {
                    void handleAssignGroup(selectedIds, val);
                  }
                }}
                aria-label={t('clipboard.batchMoveGroupAria', 'Move selected items to group')}
              >
                <option value="" disabled>
                  {t('clipboard.batchMoveToGroup', 'Move to Group...')}
                </option>
                <option value="__ungroup__">{t('clipboard.groupNone', 'No Group')}</option>
                {groups.map((g) => (
                  <option key={g.id} value={g.id}>
                    {g.name}
                  </option>
                ))}
              </select>
            )}

            <button
              type="button"
              className="clip-popup__action-btn clip-popup__action-btn--danger"
              onClick={() => void handleBatchDelete()}
            >
              <Trash2 size={14} aria-hidden="true" />
              <span>{t('clipboard.batchDelete', { count: selectedIds.length, defaultValue: `Delete (${selectedIds.length})` })}</span>
            </button>
          </div>
        </div>
      )}

      <main className="clip-popup__body">
        <section className="clip-popup__list" role="listbox" aria-label={t('clipboard.listAria', 'Clipboard history list')}>
          {items.length === 0 ? (
            <div className="clip-popup__empty">
              <ClipboardList size={28} strokeWidth={1.8} aria-hidden="true" />
              <span>
                {loading
                  ? t('clipboard.loading', 'Loading...')
                  : t('clipboard.emptyHint', 'No matching clipboard entries yet. Copy text, images, or files to record automatically.')}
              </span>
            </div>
          ) : (
            items.map((item, idx) => {
              const isChecked = selectedIdSet.has(item.id);
              const orderBadge = isChecked ? selectedIds.indexOf(item.id) + 1 : 0;
              const groupLabel = item.groupId ? groupNameById.get(item.groupId) : undefined;
              return (
                <button
                  key={item.id}
                  ref={(node) => {
                    if (node) {
                      itemBtnRefs.current.set(item.id, node);
                    } else {
                      itemBtnRefs.current.delete(item.id);
                    }
                  }}
                  type="button"
                  role="option"
                  aria-selected={idx === selectedIndex || isChecked}
                  data-clip-id={item.id}
                  className={`clip-popup__item${idx === selectedIndex ? ' clip-popup__item--selected' : ''}${isChecked ? ' clip-popup__item--checked' : ''}`}
                  onMouseDown={(e) => handleItemMouseDown(item, idx, e)}
                  onClick={(e) => handleItemClick(item, idx, e)}
                  onDoubleClick={() => {
                    if (!multiSelectMode && selectedIds.length === 0) {
                      void handleActivate(item, false, true);
                    }
                  }}
                >
                  <div className="clip-popup__item-top">
                    <span className="clip-popup__item-kind">
                      {showCheckboxes ? (
                        <span className="clip-popup__check-indicator" aria-hidden="true">
                          {isChecked ? <CheckSquare size={15} strokeWidth={2.2} /> : <Square size={15} strokeWidth={2} />}
                          {orderBadge > 0 && <span className="clip-popup__order-num">{orderBadge}</span>}
                        </span>
                      ) : idx < 9 ? (
                        <span
                          className="clip-popup__slot-badge"
                          title={t('clipboard.quickSlotTip', { slot: idx + 1, defaultValue: `Alt+${idx + 1} to paste directly` })}
                        >
                          {idx + 1}
                        </span>
                      ) : null}
                      {renderKindIcon(item.kind)}
                      {item.sourceApp ? <CodeBadge>{item.sourceApp}</CodeBadge> : null}
                      {item.hasRichText && (
                        <span className="clip-popup__rich-pill" title={t('clipboard.richTextTooltip', 'Preserves rich HTML/RTF formatting')}>
                          <Sparkles size={11} aria-hidden="true" />
                          <span>{t('clipboard.richTextBadge', 'Rich')}</span>
                        </span>
                      )}
                      {groupLabel && <span className="clip-popup__group-tag">{groupLabel}</span>}
                    </span>
                    <span className="clip-popup__item-badges">
                      {item.copyCount > 1 && <CodeBadge>{`x${item.copyCount}`}</CodeBadge>}
                      {item.pinned && <Pin size={13} strokeWidth={2.4} className="clip-popup__pin-dot" aria-hidden="true" />}
                    </span>
                  </div>

                  <div className="clip-popup__item-preview">
                    {item.kind === 'color' && item.colorHex && (
                      <span
                        className="clip-popup__color-swatch"
                        style={{ backgroundColor: item.colorHex }}
                        aria-hidden="true"
                      />
                    )}
                    {item.kind === 'image' && item.imageFile && (
                      <img
                        className="clip-popup__thumb"
                        src={`${BLOBS_BASE_URL}${encodeURIComponent(item.imageFile)}`}
                        alt={item.preview}
                        loading="lazy"
                      />
                    )}
                    <span className="clip-popup__text-snippet">
                      {item.kind === 'color' ? `${item.colorHex}  ${item.preview}` : item.preview}
                    </span>
                  </div>

                  <div className="clip-popup__item-meta">
                    <span>
                      {item.kind === 'image'
                        ? `${item.imageWidth}x${item.imageHeight} · ${formatBytesCompact(item.byteSize)}`
                        : `${item.charCount} ${t('clipboard.charsUnit', 'chars')}`}
                    </span>
                    <span>{formatRelativeTime(item.updatedAt)}</span>
                  </div>
                </button>
              );
            })
          )}
        </section>

        <aside className="clip-popup__detail">
          {selectedItem ? (
            <>
              <div className="clip-popup__detail-meta-bar">
                <div className="clip-popup__detail-meta-left">
                  {selectedItem.sourceApp ? <CodeBadge>{selectedItem.sourceApp}</CodeBadge> : null}
                  {selectedItem.hasRichText && (
                    <span className="clip-popup__rich-pill">
                      <Sparkles size={11} aria-hidden="true" />
                      <span>{t('clipboard.richTextPreserved', 'Rich Format')}</span>
                    </span>
                  )}
                </div>
                {groups.length > 0 && (
                  <select
                    className="clip-popup__compact-select"
                    value={selectedItem.groupId || ''}
                    onChange={(e) => void handleAssignGroup([selectedItem.id], e.target.value)}
                    aria-label={t('clipboard.assignGroupAria', 'Assign entry to collection group')}
                  >
                    <option value="">{t('clipboard.groupNone', 'No Group')}</option>
                    {groups.map((g) => (
                      <option key={g.id} value={g.id}>
                        {g.name}
                      </option>
                    ))}
                  </select>
                )}
              </div>

              <div className="clip-popup__detail-body">
                {selectedItem.kind === 'image' && selectedItem.imageFile ? (
                  <div className="clip-popup__detail-image-wrap">
                    <img
                      className="clip-popup__detail-image"
                      src={`${BLOBS_BASE_URL}${encodeURIComponent(selectedItem.imageFile)}`}
                      alt={selectedItem.preview}
                    />
                    <CodeBadge>
                      {`${selectedItem.imageWidth} x ${selectedItem.imageHeight} · ${formatBytesCompact(selectedItem.byteSize)}`}
                    </CodeBadge>
                  </div>
                ) : isEditingCurrent ? (
                  <textarea
                    ref={editTextareaRef}
                    className="clip-popup__detail-editor"
                    value={editDraft}
                    onChange={(e) => setEditDraft(e.target.value)}
                    onKeyDown={(e) => {
                      if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
                        e.preventDefault();
                        void handleSaveEditItem();
                      } else if (e.key === 'Escape') {
                        e.preventDefault();
                        setEditingItemId(null);
                      }
                    }}
                    aria-label={t('clipboard.editTextAria', 'Edit clipboard text')}
                  />
                ) : selectedItem.kind === 'color' && selectedItem.colorHex ? (
                  <div className="clip-popup__detail-color">
                    <div
                      className="clip-popup__detail-color-block"
                      style={{ backgroundColor: selectedItem.colorHex }}
                    />
                    <CodeBadge>{selectedItem.colorHex}</CodeBadge>
                  </div>
                ) : (
                  <pre className="clip-popup__detail-text">{detailText || selectedItem.preview}</pre>
                )}
              </div>

              <div className="clip-popup__detail-actions">
                {isEditingCurrent ? (
                  <>
                    <button
                      type="button"
                      className="clip-popup__action-btn clip-popup__action-btn--primary"
                      disabled={savingEdit || !editDraft.trim()}
                      onClick={() => void handleSaveEditItem()}
                    >
                      <Check size={14} aria-hidden="true" />
                      <span>{t('clipboard.actionSaveEdit', 'Save (Ctrl+Enter)')}</span>
                    </button>
                    <button
                      type="button"
                      className="clip-popup__action-btn"
                      onClick={() => setEditingItemId(null)}
                    >
                      <X size={14} aria-hidden="true" />
                      <span>{t('clipboard.actionCancelEdit', 'Cancel')}</span>
                    </button>
                  </>
                ) : (
                  <>
                    <button
                      type="button"
                      className="clip-popup__action-btn clip-popup__action-btn--primary"
                      onClick={() => void handleActivate(selectedItem, false, true)}
                    >
                      <CornerDownLeft size={14} aria-hidden="true" />
                      <span>{t('clipboard.actionPaste', 'Paste')}</span>
                    </button>

                    {selectedItem.hasRichText && (
                      <button
                        type="button"
                        className="clip-popup__action-btn"
                        onClick={() => void handleActivate(selectedItem, true, true)}
                      >
                        <FileText size={14} aria-hidden="true" />
                        <span>{t('clipboard.actionPastePlain', 'Plain Text')}</span>
                      </button>
                    )}

                    <button
                      type="button"
                      className="clip-popup__action-btn"
                      onClick={() => void handleActivate(selectedItem, false, false)}
                    >
                      <Copy size={14} aria-hidden="true" />
                      <span>{t('clipboard.actionCopyOnly', 'Copy')}</span>
                    </button>

                    {(selectedItem.kind === 'text' || selectedItem.kind === 'color') && (
                      <button
                        type="button"
                        className="clip-popup__action-btn"
                        onClick={handleStartEditItem}
                      >
                        <Pencil size={14} aria-hidden="true" />
                        <span>{t('clipboard.actionEdit', 'Edit')}</span>
                      </button>
                    )}

                    {selectedItem.kind === 'image' && (
                      <>
                        <button
                          type="button"
                          className="clip-popup__action-btn"
                          onClick={() => void handlePinImageAction(selectedItem, {})}
                        >
                          <StickyNote size={14} aria-hidden="true" />
                          <span>{t('clipboard.actionPinImage', 'Pin (F3)')}</span>
                        </button>
                        <button
                          type="button"
                          className="clip-popup__action-btn"
                          onClick={() => void handlePinImageAction(selectedItem, { autoOcr: true })}
                        >
                          <ScanText size={14} aria-hidden="true" />
                          <span>{t('clipboard.actionOcrImage', 'OCR')}</span>
                        </button>
                        <button
                          type="button"
                          className="clip-popup__action-btn"
                          onClick={() => void handlePinImageAction(selectedItem, { autoTranslate: true })}
                        >
                          <Languages size={14} aria-hidden="true" />
                          <span>{t('clipboard.actionTranslateImage', 'Translate')}</span>
                        </button>
                      </>
                    )}

                    <button
                      type="button"
                      className="clip-popup__action-btn"
                      onClick={() => void handleTogglePin(selectedItem)}
                    >
                      {selectedItem.pinned ? <PinOff size={14} aria-hidden="true" /> : <Pin size={14} aria-hidden="true" />}
                      <span>{selectedItem.pinned ? t('clipboard.actionUnpin', 'Unpin') : t('clipboard.actionPin', 'Pin')}</span>
                    </button>

                    <button
                      type="button"
                      className="clip-popup__action-btn"
                      onClick={() => void handleDelete(selectedItem)}
                    >
                      <Trash2 size={14} aria-hidden="true" />
                      <span>{t('clipboard.actionDelete', 'Delete')}</span>
                    </button>
                  </>
                )}
              </div>
            </>
          ) : (
            <div className="clip-popup__empty">
              <span>{t('clipboard.selectItemHint', 'Select an entry to preview')}</span>
            </div>
          )}
        </aside>
      </main>

      <footer className="clip-popup__footer">
        <div className="clip-popup__hints">
          <span className="clip-popup__hint">
            <CodeBadge>Alt+1..9</CodeBadge>
            <span>{t('clipboard.hintQuickPaste', 'Quick Paste')}</span>
          </span>
          <span className="clip-popup__hint">
            <CodeBadge>Enter</CodeBadge>
            <span>{t('clipboard.hintPaste', 'Paste')}</span>
          </span>
          <span className="clip-popup__hint">
            <CodeBadge>Shift+Enter</CodeBadge>
            <span>{t('clipboard.hintPlainText', 'Plain Text')}</span>
          </span>
          <span className="clip-popup__hint">
            <CodeBadge>Ctrl/Shift+Click</CodeBadge>
            <span>{t('clipboard.hintMultiSelect', 'Multi-Select')}</span>
          </span>
          <span className="clip-popup__hint">
            <CodeBadge>Esc</CodeBadge>
            <span>{t('clipboard.hintClose', 'Close')}</span>
          </span>
        </div>
        <span>{t('clipboard.persistedBadge', 'Persisted Across Reboots')}</span>
      </footer>
    </div>
  );
};

export default ClipboardApp;

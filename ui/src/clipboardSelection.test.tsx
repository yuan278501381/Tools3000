// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import {
  applyClipboardItemClick,
  applyClipboardSelectAll,
  applyClipboardShiftArrow,
  applyClipboardToggleAtFocus,
  createInitialClipboardSelectionState,
  pruneClipboardSelection,
} from './clipboardSelection';
import { ClipboardApp, type ClipboardItemSummary } from './ClipboardApp';

const MOCK_ITEMS: ClipboardItemSummary[] = [101, 102, 103, 104, 105, 106].map((id, idx) => ({
  id,
  kind: 'text',
  preview: `Clipboard Entry #${idx + 1}`,
  text: `Clipboard Entry #${idx + 1}`,
  hasRichText: false,
  groupId: '',
  charCount: 18,
  lineCount: 1,
  imageFile: '',
  imageWidth: 0,
  imageHeight: 0,
  sourceApp: 'Code.exe',
  colorHex: '',
  byteSize: 18,
  createdAt: Date.now() - (6 - idx) * 1000,
  updatedAt: Date.now() - (6 - idx) * 1000,
  pinned: false,
  copyCount: 1,
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, defaultOrOpts?: string | Record<string, unknown>, opts?: Record<string, unknown>) => {
      if (typeof defaultOrOpts === 'string') {
        const vars = opts ?? {};
        return defaultOrOpts.replace(/\{\{(\w+)\}\}/g, (_, k) => String(vars[k] ?? ''));
      }
      if (defaultOrOpts && typeof defaultOrOpts === 'object' && typeof defaultOrOpts.defaultValue === 'string') {
        return defaultOrOpts.defaultValue.replace(/\{\{(\w+)\}\}/g, (_, k) => String(defaultOrOpts[k] ?? ''));
      }
      return key;
    },
  }),
}));

vi.mock('./hooks/useBridge', () => ({
  bridgeRequest: vi.fn(async (method: string) => {
    if (method === 'clipboard.query') {
      return { total: MOCK_ITEMS.length, matched: MOCK_ITEMS.length, items: MOCK_ITEMS };
    }
    if (method === 'clipboard.listGroups') {
      return { groups: [] };
    }
    if (method === 'clipboard.getWindowState') {
      return { pinned: false, maximized: false };
    }
    if (method === 'clipboard.getItem') {
      return { success: true, item: MOCK_ITEMS[0] };
    }
    return { success: true };
  }),
  useBridgeEvent: vi.fn(),
}));

afterEach(() => {
  cleanup();
  vi.clearAllMocks();
});

describe('clipboardSelection pure state machine', () => {
  const ids = [101, 102, 103, 104, 105, 106];

  it('keeps fixed anchor during consecutive Shift+Click expand and shrink, and preserves direction order', () => {
    let s = createInitialClipboardSelectionState();
    // 1. 普通点第 2 条 (idx=1, id=102) 定锚点
    s = applyClipboardItemClick(s, ids, 1, { shiftKey: false, ctrlKey: false, metaKey: false, multiSelectMode: false });
    expect(s.selectedIds).toEqual([]);
    expect(s.anchorIndex).toBe(1);

    // 2. Shift 点第 5 条 (idx=4, id=105) -> 连选 102..105，锚点保持 1
    s = applyClipboardItemClick(s, ids, 4, { shiftKey: true, ctrlKey: false, metaKey: false, multiSelectMode: false });
    expect(s.selectedIds).toEqual([102, 103, 104, 105]);
    expect(s.anchorIndex).toBe(1);

    // 3. 再次 Shift 点第 3 条 (idx=2, id=103) -> 围绕锚点 1 精准回缩为 [102, 103]
    s = applyClipboardItemClick(s, ids, 2, { shiftKey: true, ctrlKey: false, metaKey: false, multiSelectMode: false });
    expect(s.selectedIds).toEqual([102, 103]);
    expect(s.anchorIndex).toBe(1);

    // 4. 反向 Shift 点第 1 条 (idx=0, id=101) -> 从锚点 1 向上连选 [102, 101]
    s = applyClipboardItemClick(s, ids, 0, { shiftKey: true, ctrlKey: false, metaKey: false, multiSelectMode: false });
    expect(s.selectedIds).toEqual([102, 101]);
    expect(s.anchorIndex).toBe(1);
  });

  it('supports Ctrl additive toggle and Ctrl+Shift range union in both normal and multi-select modes', () => {
    let s = createInitialClipboardSelectionState();
    // 开启批量模式：先点第 1 条 (id=101)，再点第 3 条 (id=103)
    s = applyClipboardItemClick(s, ids, 0, { shiftKey: false, ctrlKey: false, metaKey: false, multiSelectMode: true });
    s = applyClipboardItemClick(s, ids, 2, { shiftKey: false, ctrlKey: false, metaKey: false, multiSelectMode: true });
    expect(s.selectedIds).toEqual([101, 103]);

    // Shift 连选到第 5 条 (idx=4, id=105) -> 保留 101 并连选 103..105
    s = applyClipboardItemClick(s, ids, 4, { shiftKey: true, ctrlKey: false, metaKey: false, multiSelectMode: true });
    expect(s.selectedIds).toEqual([101, 103, 104, 105]);

    // 再次 Shift 回缩到第 4 条 (idx=3, id=104) -> 仍保留 101，区间缩为 103..104
    s = applyClipboardItemClick(s, ids, 3, { shiftKey: true, ctrlKey: false, metaKey: false, multiSelectMode: true });
    expect(s.selectedIds).toEqual([101, 103, 104]);

    // Ctrl 加选第 6 条 (idx=5, id=106)
    s = applyClipboardItemClick(s, ids, 5, { shiftKey: false, ctrlKey: true, metaKey: false, multiSelectMode: true });
    expect(s.selectedIds).toEqual([101, 103, 104, 106]);

    // Ctrl 取消勾选第 3 条 (idx=2, id=103)
    s = applyClipboardItemClick(s, ids, 2, { shiftKey: false, ctrlKey: true, metaKey: false, multiSelectMode: true });
    expect(s.selectedIds).toEqual([101, 104, 106]);

    // Ctrl+Shift 追加区间 [idx=2..idx=0]
    s = applyClipboardItemClick(s, ids, 0, { shiftKey: true, ctrlKey: true, metaKey: false, multiSelectMode: false });
    expect(s.selectedIds).toEqual([101, 104, 106, 103, 102]);
  });

  it('supports Shift+Arrows, Ctrl+A, Space toggle, and stale-id pruning', () => {
    let s = createInitialClipboardSelectionState();
    s = applyClipboardItemClick(s, ids, 1, { shiftKey: false, ctrlKey: false, metaKey: false, multiSelectMode: false });
    s = applyClipboardShiftArrow(s, ids, 1, false);
    s = applyClipboardShiftArrow(s, ids, 1, false);
    expect(s.selectedIds).toEqual([102, 103, 104]);

    // 回缩一步
    s = applyClipboardShiftArrow(s, ids, -1, false);
    expect(s.selectedIds).toEqual([102, 103]);

    // Ctrl+A 全选
    s = applyClipboardSelectAll(s, ids);
    expect(s.selectedIds).toEqual(ids);

    // Space 取消焦点行 (idx=2 -> id=103)
    s = applyClipboardToggleAtFocus(s, ids);
    expect(s.selectedIds).toEqual([101, 102, 104, 105, 106]);

    // 列表裁剪失效 ID
    s = pruneClipboardSelection(s, [102, 105]);
    expect(s.selectedIds).toEqual([102, 105]);
    expect(s.focusIndex).toBe(1);
  });
});

describe('ClipboardApp real mouseDown + click Shift/Ctrl multi-select integration', () => {
  it('handles Shift range select/shrink, Ctrl additive toggle, Ctrl+A, and prevents duplicate mouseDown+click toggling', async () => {
    const { container } = render(<ClipboardApp />);

    await waitFor(() => {
      expect(container.querySelector('button[data-clip-id="101"]')).toBeTruthy();
      expect(container.querySelector('button[data-clip-id="106"]')).toBeTruthy();
    });

    // 开启「多选」模式
    const selectModeBtn = screen.getByRole('button', { name: 'Select' });
    fireEvent.click(selectModeBtn);

    const getRow = (id: number) =>
      container.querySelector<HTMLButtonElement>(`button[data-clip-id="${id}"]`)!;

    const simulateMouseSelect = (
      id: number,
      mods: { shiftKey?: boolean; ctrlKey?: boolean } = {},
    ) => {
      const btn = getRow(id);
      const mdAllowed = fireEvent.mouseDown(btn, { button: 0, ...mods });
      fireEvent.mouseUp(btn, { button: 0, ...mods });
      // 真实浏览器点击 detail === 1；不得因 mouseDown+click 双触发而把刚勾的项反选掉
      fireEvent.click(btn, { button: 0, detail: 1, ...mods });
      return mdAllowed;
    };

    // 1. 多选模式下点第 2 条 (102) -> 应勾中且不被 mouseDown+click 抵消
    const md1 = simulateMouseSelect(102);
    expect(md1).toBe(false); // preventDefault() 已在 mouseDown 调用，杜绝原生选字
    expect(screen.getByText('1 selected')).toBeTruthy();

    // 2. Shift+点击 第 5 条 (105) -> 连选 102..105 共 4 条
    const md2 = simulateMouseSelect(105, { shiftKey: true });
    expect(md2).toBe(false);
    expect(screen.getByText('4 selected')).toBeTruthy();
    expect(getRow(102).classList.contains('clip-popup__item--checked')).toBe(true);
    expect(getRow(103).classList.contains('clip-popup__item--checked')).toBe(true);
    expect(getRow(104).classList.contains('clip-popup__item--checked')).toBe(true);
    expect(getRow(105).classList.contains('clip-popup__item--checked')).toBe(true);

    // 3. 再次 Shift+点击 第 3 条 (103) -> 围绕锚点 102 精准回缩为 2 条 (102, 103)
    simulateMouseSelect(103, { shiftKey: true });
    expect(screen.getByText('2 selected')).toBeTruthy();
    expect(getRow(104).classList.contains('clip-popup__item--checked')).toBe(false);
    expect(getRow(105).classList.contains('clip-popup__item--checked')).toBe(false);

    // 4. Ctrl+点击 第 6 条 (106) -> 加选第 6 条，共 3 条
    simulateMouseSelect(106, { ctrlKey: true });
    expect(screen.getByText('3 selected')).toBeTruthy();
    expect(getRow(106).classList.contains('clip-popup__item--checked')).toBe(true);

    // 5. Ctrl+A -> 全选 6 条
    const root = container.querySelector<HTMLDivElement>('.clip-popup')!;
    fireEvent.keyDown(root, { key: 'a', ctrlKey: true });
    expect(screen.getByText('6 selected')).toBeTruthy();

    // 6. Esc -> 优先退出多选态
    fireEvent.keyDown(root, { key: 'Escape' });
    expect(screen.queryByText(/selected/)).toBeNull();
  });
});

/* @vitest-environment jsdom */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, fireEvent, waitFor, cleanup } from '@testing-library/react';
import { ClipboardApp } from './ClipboardApp';

const bridgeRequestMock = vi.fn();

vi.mock('./hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
  useBridgeEvent: vi.fn(),
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, opts?: string | { defaultValue?: string }) => {
      if (typeof opts === 'string') return opts;
      if (opts && typeof opts === 'object' && opts.defaultValue) return opts.defaultValue;
      return key;
    },
  }),
}));

describe('ClipboardApp', () => {
  afterEach(() => cleanup());

  beforeEach(() => {
    let hidePinnedSetting = false;
    const allMockItems = [
      {
        id: 101,
        kind: 'image',
        preview: 'Image 1920x1080',
        hasRichText: false,
        groupId: '',
        charCount: 15,
        lineCount: 1,
        imageFile: 'abcd1234.png',
        imageWidth: 1920,
        imageHeight: 1080,
        sourceApp: 'Tools3000.exe',
        colorHex: '',
        byteSize: 204800,
        createdAt: Date.now() - 5000,
        updatedAt: Date.now() - 5000,
        pinned: true,
        copyCount: 1,
      },
      {
        id: 102,
        kind: 'text',
        preview: 'Rich formatted note',
        hasRichText: true,
        groupId: 'grp_1',
        charCount: 19,
        lineCount: 1,
        imageFile: '',
        imageWidth: 0,
        imageHeight: 0,
        sourceApp: 'Chrome.exe',
        colorHex: '',
        byteSize: 19,
        createdAt: Date.now() - 3000,
        updatedAt: Date.now() - 3000,
        pinned: false,
        copyCount: 1,
      },
      {
        id: 103,
        kind: 'color',
        preview: '#10B981',
        hasRichText: false,
        groupId: '',
        charCount: 7,
        lineCount: 1,
        imageFile: '',
        imageWidth: 0,
        imageHeight: 0,
        sourceApp: 'Figma.exe',
        colorHex: '#10B981',
        byteSize: 7,
        createdAt: Date.now() - 2000,
        updatedAt: Date.now() - 2000,
        pinned: false,
        copyCount: 2,
      },
    ];

    bridgeRequestMock.mockReset();
    bridgeRequestMock.mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'clipboard.getSettings') {
        return { hidePinnedInOtherCategories: hidePinnedSetting };
      }
      if (method === 'clipboard.updateSettings') {
        if (typeof params?.hidePinnedInOtherCategories === 'boolean') {
          hidePinnedSetting = params.hidePinnedInOtherCategories;
        }
        return { success: true };
      }
      if (method === 'clipboard.listGroups') {
        return {
          groups: [
            { id: 'grp_1', name: 'Work Snippets', count: 1, createdAt: Date.now() - 10000 },
          ],
        };
      }
      if (method === 'clipboard.query') {
        const kind = typeof params?.kind === 'string' ? params.kind : 'all';
        const excludePinned = Boolean(params?.excludePinned) && kind !== 'pinned';
        const filtered = allMockItems.filter((it) => {
          if (kind === 'pinned') return it.pinned;
          if (excludePinned && it.pinned) return false;
          if (kind === 'text') return it.kind === 'text' || it.kind === 'color';
          if (kind === 'image') return it.kind === 'image';
          if (kind === 'files') return it.kind === 'files';
          if (kind === 'color') return it.kind === 'color';
          return true;
        });
        const totalPinned = allMockItems.filter((it) => it.pinned).length;
        return {
          total: allMockItems.length,
          matched: filtered.length,
          pinnedCount: totalPinned,
          hiddenPinnedCount: excludePinned ? totalPinned : 0,
          items: filtered,
        };
      }
      if (method === 'clipboard.getItem') {
        if (params?.id === 102) {
          return { success: true, item: { id: 102, text: 'Rich formatted note', hasRichText: true } };
        }
        return { success: true, item: { id: 103, text: '#10B981' } };
      }
      if (method === 'clipboard.createGroup') {
        return { success: true, group: { id: 'grp_2', name: params?.name, count: 0, createdAt: Date.now() } };
      }
      return { success: true };
    });
  });

  it('renders persisted image and color items and triggers 1-click Translate/OCR/Pin actions', async () => {
    render(<ClipboardApp />);

    await waitFor(() => {
      expect(screen.getByText('Image 1920x1080')).toBeTruthy();
    });

    const translateBtn = screen.getByRole('button', { name: 'Translate' });
    fireEvent.click(translateBtn);
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.pinImage', {
        id: 101,
        autoOcr: false,
        autoTranslate: true,
      });
    });

    const ocrBtn = screen.getByRole('button', { name: 'OCR' });
    fireEvent.click(ocrBtn);
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.pinImage', {
        id: 101,
        autoOcr: true,
        autoTranslate: false,
      });
    });
  });

  it('supports Alt+1..9 quick paste, multi-select merge paste, batch delete, rich-text plain paste, and inline edit', async () => {
    const { container } = render(<ClipboardApp />);

    await waitFor(() => {
      expect(screen.getByText('Rich formatted note')).toBeTruthy();
    });

    // 1. Alt+2 秒贴第 2 条，Alt+Shift+2 纯文本剥离秒贴
    const root = container.querySelector('.clip-popup') as HTMLElement;
    fireEvent.keyDown(root, { key: '2', altKey: true });
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.pasteItem', {
        id: 102,
        plainTextOnly: false,
        autoPaste: true,
        hideAfter: true,
      });
    });

    fireEvent.keyDown(root, { key: '2', altKey: true, shiftKey: true });
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.pasteItem', {
        id: 102,
        plainTextOnly: true,
        autoPaste: true,
        hideAfter: true,
      });
    });

    // 2. 点击选中富文本条目并执行纯文本内联编辑保存
    const options = screen.getAllByRole('option');
    fireEvent.click(options[1]);
    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Edit' })).toBeTruthy();
    });
    fireEvent.click(screen.getByRole('button', { name: 'Edit' }));
    const editor = screen.getByLabelText('Edit clipboard text') as HTMLTextAreaElement;
    fireEvent.change(editor, { target: { value: 'Updated inline text' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save (Ctrl+Enter)' }));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.updateItemText', {
        id: 102,
        text: 'Updated inline text',
      });
      expect(screen.queryByLabelText('Edit clipboard text')).toBeNull();
    });

    // 3. Ctrl+Click 多选第 3 条 -> 第 2 条，验证按勾选顺序合并粘贴
    const optionsAfterEdit = screen.getAllByRole('option');
    fireEvent.click(optionsAfterEdit[2], { ctrlKey: true });
    fireEvent.click(optionsAfterEdit[1], { ctrlKey: true });
    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Merge Paste' })).toBeTruthy();
    });
    fireEvent.click(screen.getByRole('button', { name: 'Merge Paste' }));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.pasteItemsBatch', {
        ids: [103, 102],
        separator: '\n',
        autoPaste: true,
        hideAfter: true,
      });
      expect(screen.queryByRole('button', { name: 'Merge Paste' })).toBeNull();
    });

    // 4. 开启多选模式勾选后执行批量删除
    fireEvent.click(screen.getByRole('button', { name: 'Select' }));
    const optionsForDelete = screen.getAllByRole('option');
    fireEvent.click(optionsForDelete[1]);
    fireEvent.click(optionsForDelete[2]);
    const deleteBatchBtns = await screen.findAllByRole('button', { name: 'Delete (2)' });
    fireEvent.click(deleteBatchBtns[0]);
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.deleteItems', {
        ids: [102, 103],
        skipPinned: false,
      });
    });
  });

  it('keeps multi-selection when clicking checkbox directly, supports Ctrl+A -> Delete from empty search input, and routes detail Delete button to batch delete', async () => {
    const { container } = render(<ClipboardApp />);

    await waitFor(() => {
      expect(screen.getByText('Rich formatted note')).toBeTruthy();
    });

    // 1. Ctrl+Click 选中第 1 条后直接点击第 2 条的复选框区域（不按 Ctrl），绝不清空已选集合
    const options = screen.getAllByRole('option');
    fireEvent.mouseDown(options[0], { button: 0, ctrlKey: true });
    fireEvent.click(options[0], { detail: 1, ctrlKey: true });

    const checkboxes = container.querySelectorAll('[data-clip-checkbox="true"]');
    expect(checkboxes.length).toBe(3);
    fireEvent.mouseDown(checkboxes[1], { button: 0 });
    fireEvent.click(checkboxes[1], { detail: 1 });

    // 多选态下右侧详情底栏删除按钮自动切换为批量删除，点击应调用 clipboard.deleteItems 而非 deleteItem
    const detailBatchDeleteBtn = await screen.findByTestId('detail-batch-delete-btn');
    expect(detailBatchDeleteBtn.textContent).toContain('Delete (2)');
    fireEvent.click(detailBatchDeleteBtn);
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.deleteItems', {
        ids: [101, 102],
        skipPinned: false,
      });
    });
    expect(bridgeRequestMock).not.toHaveBeenCalledWith('clipboard.deleteItem', expect.anything());

    // 2. 焦点留在空搜索框时按 Ctrl+A -> Delete 应一键批量删除全部 3 条
    bridgeRequestMock.mockClear();
    const searchInput = screen.getByPlaceholderText('Search history, color, path or source app...');
    searchInput.focus();
    fireEvent.keyDown(searchInput, { key: 'a', ctrlKey: true });
    await waitFor(() => {
      expect(screen.getAllByRole('button', { name: 'Delete (3)' }).length).toBeGreaterThanOrEqual(1);
    });
    fireEvent.keyDown(searchInput, { key: 'Delete' });
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.deleteItems', {
        ids: [101, 102, 103],
        skipPinned: false,
      });
    });
  });

  it('toggles hiding pinned items in non-pinned categories while preserving the Pinned tab', async () => {
    render(<ClipboardApp />);

    await waitFor(() => {
      expect(screen.getByText('Image 1920x1080')).toBeTruthy();
      expect(screen.getByRole('button', { name: 'Hide Pinned' })).toBeTruthy();
    });

    // 1. 点击「隐藏置顶」：持久化设置并向后端请求 excludePinned: true，置顶条目从「全部」剥离
    fireEvent.click(screen.getByRole('button', { name: 'Hide Pinned' }));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'clipboard.updateSettings',
        { hidePinnedInOtherCategories: true },
        { silent: true },
      );
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'clipboard.query',
        expect.objectContaining({ kind: 'all', excludePinned: true }),
        { silent: true },
      );
      expect(screen.queryByText('Image 1920x1080')).toBeNull();
      expect(screen.getByRole('button', { name: 'Show Pinned' })).toBeTruthy();
    });

    // 2. 切到「置顶」分类时，excludePinned 强制为 false 且完整展示置顶条目，不显示隐藏切换按钮
    fireEvent.click(screen.getByRole('tab', { name: 'Pinned' }));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'clipboard.query',
        expect.objectContaining({ kind: 'pinned', excludePinned: false }),
        { silent: true },
      );
      expect(screen.getByText('Image 1920x1080')).toBeTruthy();
      expect(screen.queryByRole('button', { name: 'Show Pinned' })).toBeNull();
    });

    // 3. 切回「全部」点击「显示置顶」，置顶条目重新归位
    fireEvent.click(screen.getByRole('tab', { name: 'All' }));
    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Show Pinned' })).toBeTruthy();
    });
    fireEvent.click(screen.getByRole('button', { name: 'Show Pinned' }));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'clipboard.updateSettings',
        { hidePinnedInOtherCategories: false },
        { silent: true },
      );
      expect(screen.getByText('Image 1920x1080')).toBeTruthy();
    });
  });

  it('triggers native OLE drag after 8px threshold and suppresses trailing click', async () => {
    render(<ClipboardApp />);

    await waitFor(() => {
      expect(screen.getByText('Rich formatted note')).toBeTruthy();
    });

    const options = screen.getAllByRole('option');
    // 1. 拖动不足 8px 不触发原生拖拽
    fireEvent.mouseDown(options[1], { button: 0, clientX: 20, clientY: 20 });
    fireEvent.mouseMove(window, { clientX: 24, clientY: 22 });
    fireEvent.mouseUp(window);
    expect(bridgeRequestMock).not.toHaveBeenCalledWith(
      'clipboard.startNativeDrag',
      expect.anything(),
      expect.anything(),
    );

    // 2. 拖动 >= 8px 触发 clipboard.startNativeDrag，且抑制紧随其后的 click 误切焦点
    fireEvent.mouseDown(options[1], { button: 0, clientX: 20, clientY: 20 });
    fireEvent.mouseMove(window, { clientX: 40, clientY: 26 });
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'clipboard.startNativeDrag',
        { id: 102 },
        { silent: true },
      );
    });
    fireEvent.click(options[1]);
    expect(options[0].getAttribute('aria-selected')).toBe('true');
    expect(options[1].getAttribute('aria-selected')).toBe('false');
  });

  it('renders native colored source app icons with friendly labels and full exe tooltips', async () => {
    const fakePng = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==';
    bridgeRequestMock.mockImplementation(async (method: string) => {
      if (method === 'clipboard.getSettings') {
        return { hidePinnedInOtherCategories: false };
      }
      if (method === 'clipboard.listGroups') {
        return { groups: [] };
      }
      if (method === 'clipboard.query') {
        return {
          total: 2,
          matched: 2,
          pinnedCount: 0,
          hiddenPinnedCount: 0,
          items: [
            {
              id: 201,
              kind: 'image',
              preview: 'Image 1106x515',
              hasRichText: false,
              groupId: '',
              charCount: 0,
              lineCount: 1,
              imageFile: 'wx.png',
              imageWidth: 1106,
              imageHeight: 515,
              sourceApp: 'Weixin.exe',
              colorHex: '',
              byteSize: 21299,
              createdAt: Date.now() - 60_000,
              updatedAt: Date.now() - 60_000,
              pinned: false,
              copyCount: 1,
            },
            {
              id: 202,
              kind: 'text',
              preview: 'Edge snippet',
              hasRichText: true,
              groupId: '',
              charCount: 12,
              lineCount: 1,
              imageFile: '',
              imageWidth: 0,
              imageHeight: 0,
              sourceApp: 'msedge.exe',
              colorHex: '',
              byteSize: 12,
              createdAt: Date.now() - 30_000,
              updatedAt: Date.now() - 30_000,
              pinned: false,
              copyCount: 1,
            },
          ],
          appMeta: {
            'weixin.exe': { name: '微信', icon: fakePng },
            'msedge.exe': { name: 'Microsoft Edge', icon: fakePng },
          },
        };
      }
      return { success: true };
    });

    const { container } = render(<ClipboardApp />);

    await waitFor(() => {
      expect(screen.getAllByText('微信').length).toBeGreaterThanOrEqual(2); // 列表项 + 详情区
      expect(screen.getByText('Microsoft Edge')).toBeTruthy();
    });

    const badges = container.querySelectorAll('.clip-popup__source-badge');
    expect(badges.length).toBe(3); // 左侧 2 项 + 右侧当前选中详情 1 项
    expect(badges[0].getAttribute('title')).toBe('微信 (Weixin.exe)');
    expect(badges[1].getAttribute('title')).toBe('Microsoft Edge (msedge.exe)');
    const icons = container.querySelectorAll('img.clip-popup__source-icon');
    expect(icons.length).toBe(3);
    expect(icons[0].getAttribute('src')).toBe(fakePng);
  });
});

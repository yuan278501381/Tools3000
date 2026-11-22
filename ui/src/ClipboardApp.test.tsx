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
    bridgeRequestMock.mockReset();
    bridgeRequestMock.mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'clipboard.listGroups') {
        return {
          groups: [
            { id: 'grp_1', name: 'Work Snippets', count: 1, createdAt: Date.now() - 10000 },
          ],
        };
      }
      if (method === 'clipboard.query') {
        return {
          total: 3,
          matched: 3,
          items: [
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
          ],
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
    });

    // 3. Ctrl+Click 多选第 3 条 -> 第 2 条，验证按勾选顺序合并粘贴
    fireEvent.click(options[2], { ctrlKey: true });
    fireEvent.click(options[1], { ctrlKey: true });
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
    });

    // 4. 开启多选模式勾选后执行批量删除
    fireEvent.click(screen.getByRole('button', { name: 'Select' }));
    fireEvent.click(options[1]);
    fireEvent.click(options[2]);
    fireEvent.click(screen.getByRole('button', { name: 'Delete (2)' }));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('clipboard.deleteItems', {
        ids: [102, 103],
        skipPinned: false,
      });
    });
  });
});

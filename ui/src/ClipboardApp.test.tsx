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
    bridgeRequestMock.mockImplementation(async (method: string) => {
      if (method === 'clipboard.query') {
        return {
          total: 2,
          matched: 2,
          items: [
            {
              id: 101,
              kind: 'image',
              preview: 'Image 1920x1080',
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
              kind: 'color',
              preview: '#10B981',
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
        return { success: true, item: { id: 102, text: '#10B981' } };
      }
      return { success: true };
    });
  });

  it('renders persisted image and color items and triggers 1-click Translate/OCR/Pin actions', async () => {
    render(<ClipboardApp />);

    await waitFor(() => {
      expect(screen.getByText('Image 1920x1080')).toBeTruthy();
    });

    // 首条图片条目展示专属贴图 / OCR / 原位翻译联动按钮
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
});

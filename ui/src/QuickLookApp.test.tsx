/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import QuickLookApp from './QuickLookApp';
import * as bridge from './hooks/useBridge';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, defaultValue?: string, vars?: Record<string, string | number>) => {
      let out = defaultValue ?? key;
      if (vars) {
        for (const [k, v] of Object.entries(vars)) {
          out = out.replaceAll(`{{${k}}}`, String(v));
        }
      }
      return out;
    },
  }),
}));

vi.mock('./hooks/useAppearance', () => ({
  useAppearance: () => undefined,
}));

describe('QuickLookApp Usability & Media Pipeline', () => {
  let bridgeSpy: ReturnType<typeof vi.spyOn>;
  let fileChangedHandler: ((payload: unknown) => void) | null = null;

  beforeEach(() => {
    vi.clearAllMocks();
    fileChangedHandler = null;
    vi.spyOn(bridge, 'useBridgeEvent').mockImplementation((event: string, handler: (payload: unknown) => void) => {
      if (event === 'quicklook.fileChanged') {
        fileChangedHandler = handler;
      }
    });
    bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockResolvedValue({});
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('renders wrap-safe code lines, shows 256 KB truncated notice, and opens full file on click', async () => {
    render(<QuickLookApp />);
    expect(bridgeSpy).toHaveBeenCalledWith('quicklook.ready');

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        path: 'C:\\repo\\large_log.ts',
        name: 'large_log.ts',
        extension: 'ts',
        type: 'code',
        isDirectory: false,
        size: 512000,
        formattedSize: '500.0 KB',
        modified: '2026-10-15 10:00:00',
        content: 'const alpha = 1;\nconst beta = 2;\nconst gamma = 3;',
        encoding: 'UTF-8',
        truncated: true,
      });
    });

    const codeLines = document.querySelectorAll('.ql-code-line');
    expect(codeLines.length).toBe(3);
    expect(screen.getByText('Showing first 256 KB for instant preview.')).toBeDefined();

    fireEvent.click(screen.getByText('Open Full File'));
    expect(bridgeSpy).toHaveBeenCalledWith('quicklook.open', { path: 'C:\\repo\\large_log.ts' });
  });

  it('binds mediaUrl for PDF iframe and video player without falling back to 404 main host', async () => {
    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        path: 'C:\\Docs\\spec #1.pdf',
        name: 'spec #1.pdf',
        extension: 'pdf',
        type: 'pdf',
        isDirectory: false,
        size: 102400,
        formattedSize: '100.0 KB',
        modified: '2026-10-15 10:00:00',
        mediaUrl: 'https://quicklook-media.tools3000.local/spec%20%231.pdf',
      });
    });

    const iframe = document.querySelector('iframe.ql-pdf-frame') as HTMLIFrameElement | null;
    expect(iframe).not.toBeNull();
    expect(iframe?.getAttribute('src')).toBe('https://quicklook-media.tools3000.local/spec%20%231.pdf');

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        path: 'C:\\Videos\\demo.mp4',
        name: 'demo.mp4',
        extension: 'mp4',
        type: 'video',
        isDirectory: false,
        size: 2048000,
        formattedSize: '2.0 MB',
        modified: '2026-10-15 10:05:00',
        mediaUrl: 'https://quicklook-media.tools3000.local/demo.mp4',
      });
    });

    const video = document.querySelector('video.ql-video-player') as HTMLVideoElement | null;
    expect(video).not.toBeNull();
    expect(video?.getAttribute('src')).toBe('https://quicklook-media.tools3000.local/demo.mp4');
  });

  it('supports pinning image with P key, stepping sibling files with ArrowDown, and copy feedback', async () => {
    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        path: 'C:\\Pictures\\chart.png',
        name: 'chart.png',
        extension: 'png',
        type: 'image',
        isDirectory: false,
        size: 40960,
        formattedSize: '40.0 KB',
        modified: '2026-10-15 11:00:00',
        mediaUrl: 'https://quicklook-media.tools3000.local/chart.png',
      });
    });

    fireEvent.keyDown(window, { key: 'p' });
    expect(bridgeSpy).toHaveBeenCalledWith('capture.pinImageFile', { path: 'C:\\Pictures\\chart.png' });
    expect(screen.getByText('Pinned')).toBeDefined();

    fireEvent.keyDown(window, { key: 'ArrowDown' });
    expect(bridgeSpy).toHaveBeenCalledWith('quicklook.stepSelection', { delta: 1 });

    fireEvent.click(screen.getByText('Copy Path'));
    expect(bridgeSpy).toHaveBeenCalledWith('quicklook.copyPath', { path: 'C:\\Pictures\\chart.png' });
    expect(screen.getByText('Copied')).toBeDefined();
  });

  it('keeps balanced highlight spans across multi-line block comments and normalizes CRLF line counts', async () => {
    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        path: 'C:\\repo\\comment.ts',
        name: 'comment.ts',
        extension: 'ts',
        type: 'code',
        isDirectory: false,
        size: 128,
        formattedSize: '128 B',
        modified: '2026-10-15 12:00:00',
        content: '/* header line 1\r\n * header line 2 */\r\n\r\nconst ready = true;',
        encoding: 'UTF-8',
        truncated: false,
      });
    });

    const codeLines = Array.from(document.querySelectorAll('.ql-code-line'));
    expect(codeLines.length).toBe(4);
    expect(codeLines[0].querySelector('.tok-comment')?.textContent).toBe('/* header line 1');
    expect(codeLines[1].querySelector('.tok-comment')?.textContent).toBe(' * header line 2 */');
    expect(codeLines[2].querySelector('br')).not.toBeNull();
    expect(codeLines[2].textContent).toBe('');
    expect(codeLines[3].querySelector('.tok-keyword')?.textContent).toBe('const');
    expect(screen.getByText('· 4 lines')).toBeDefined();
  });
});

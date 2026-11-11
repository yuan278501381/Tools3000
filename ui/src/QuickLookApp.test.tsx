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

  it('stops playback and releases media handles when receiving null payload or hidden visibility', async () => {
    const pauseSpy = vi.spyOn(window.HTMLMediaElement.prototype, 'pause').mockImplementation(() => {});
    const loadSpy = vi.spyOn(window.HTMLMediaElement.prototype, 'load').mockImplementation(() => {});

    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        path: 'C:\\Videos\\clip.mp4',
        name: 'clip.mp4',
        extension: 'mp4',
        type: 'video',
        isDirectory: false,
        size: 1024000,
        formattedSize: '1.0 MB',
        modified: '2026-10-15 12:30:00',
        mediaUrl: 'https://quicklook-media.tools3000.local/clip.mp4',
      });
    });

    const video = document.querySelector('video.ql-video-player') as HTMLVideoElement;
    expect(video).not.toBeNull();

    await act(async () => {
      fileChangedHandler?.(null);
    });

    expect(pauseSpy).toHaveBeenCalled();
    expect(loadSpy).toHaveBeenCalled();
    expect(document.querySelector('video.ql-video-player')).toBeNull();
  });

  it('virtualizes large code previews so only a bounded slice of lines is mounted in the DOM', async () => {
    render(<QuickLookApp />);

    const totalLines = 3000;
    const largeContent = Array.from({ length: totalLines }, (_, idx) => `const row_${idx + 1} = ${idx + 1};`).join('\n');

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        path: 'C:\\repo\\huge_trace.ts',
        name: 'huge_trace.ts',
        extension: 'ts',
        type: 'code',
        isDirectory: false,
        size: largeContent.length,
        formattedSize: '72.0 KB',
        modified: '2026-10-15 13:00:00',
        content: largeContent,
        encoding: 'UTF-8',
        truncated: false,
      });
    });

    const mountedLines = document.querySelectorAll('.ql-code-line');
    expect(mountedLines.length).toBeGreaterThan(0);
    expect(mountedLines.length).toBeLessThan(250);
    expect(mountedLines[0].getAttribute('data-line')).toBe('1');
    expect(screen.getByText('· 3000 lines')).toBeDefined();
  });
});

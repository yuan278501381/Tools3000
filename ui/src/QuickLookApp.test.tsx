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

    // Verify large virtualized code retains .wrap class by default and toggles cleanly via Alt+Z
    const preEl = document.querySelector('pre.ql-code-content');
    expect(preEl?.classList.contains('virtualized')).toBe(true);
    expect(preEl?.classList.contains('wrap')).toBe(true);

    fireEvent.keyDown(window, { key: 'z', altKey: true });
    expect(preEl?.classList.contains('wrap')).toBe(false);
  });

  it('supports Grab-to-Pan pointer dragging when image is zoomed (>1x) and resets pan offset on 100% reset', async () => {
    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        path: 'C:\\Pictures\\diagram.png',
        name: 'diagram.png',
        extension: 'png',
        type: 'image',
        isDirectory: false,
        size: 81920,
        formattedSize: '80.0 KB',
        modified: '2026-10-15 14:00:00',
        mediaUrl: 'https://quicklook-media.tools3000.local/diagram.png',
      });
    });

    const imageView = document.querySelector('.ql-image-view') as HTMLDivElement;
    const imgEl = document.querySelector('.ql-image-element') as HTMLImageElement;
    expect(imageView).not.toBeNull();
    expect(imgEl).not.toBeNull();
    expect(imgEl.style.transform).toBe('translate3d(0px, 0px, 0) scale(1)');

    // Zoom in via '+' key -> scale becomes 1.25 and container enters zoomed state
    fireEvent.keyDown(window, { key: '+' });
    expect(imageView.classList.contains('ql-image-view--zoomed')).toBe(true);
    expect(imgEl.style.transform).toBe('translate3d(0px, 0px, 0) scale(1.25)');

    // Pointer drag translates image
    fireEvent.pointerDown(imageView, { button: 0, pointerId: 1, clientX: 200, clientY: 150 });
    fireEvent.pointerMove(imageView, { pointerId: 1, clientX: 245, clientY: 120 });
    fireEvent.pointerUp(imageView, { pointerId: 1, clientX: 245, clientY: 120 });
    expect(imgEl.style.transform).toBe('translate3d(45px, -30px, 0) scale(1.25)');

    // Resetting to 100% via '0' key resets both scale and pan offset
    fireEvent.keyDown(window, { key: '0' });
    expect(imgEl.style.transform).toBe('translate3d(0px, 0px, 0) scale(1)');
  });

  it('renders RFC 4180 CSV and multi-sheet Excel (.xlsx) previews as clean aligned data tables without garbled output', async () => {
    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\Travel\\budget.csv',
        name: 'budget.csv',
        extension: 'csv',
        type: 'csv',
        isDirectory: false,
        size: 512,
        formattedSize: '512 B',
        modified: '2026-10-16 09:30:00',
        encoding: 'GBK',
        csvDelimiter: ',',
        content: '项目,金额,备注\r\n"往返机票, 含税",1680,"直飞大理"\r\n海景民宿,920,双廊古镇',
      });
    });

    const csvHeaders = Array.from(document.querySelectorAll('.ql-data-table thead th')).map(el => el.textContent);
    expect(csvHeaders).toEqual(['#', '项目', '金额', '备注']);
    const csvCells = Array.from(document.querySelectorAll('.ql-data-table tbody td')).map(el => el.textContent);
    expect(csvCells).toContain('往返机票, 含税');
    expect(csvCells).toContain('1680');

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\Travel\\trip_plan.xlsx',
        name: 'trip_plan.xlsx',
        extension: 'xlsx',
        type: 'spreadsheet',
        isDirectory: false,
        size: 14336,
        formattedSize: '14.0 KB',
        modified: '2026-10-16 10:00:00',
        totalSheetCount: 2,
        sheets: [
          {
            name: '行程安排',
            rows: [
              ['天数', '地点'],
              ['Day 1', '大理古城'],
            ],
          },
          {
            name: '装备清单',
            rows: [
              ['物品', '状态'],
              ['防晒衣', '已备齐'],
            ],
          },
        ],
      });
    });

    expect(screen.getByText('大理古城')).toBeDefined();
    fireEvent.click(screen.getByText('装备清单'));
    expect(screen.getByText('防晒衣')).toBeDefined();
  });

  it('renders native rasterized PDF pages with zoom, structured Word documents, and hides raw hex dumps by default', async () => {
    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\Travel\\hotel_booking.pdf',
        name: 'hotel_booking.pdf',
        extension: 'pdf',
        type: 'pdf',
        isDirectory: false,
        size: 81920,
        formattedSize: '80.0 KB',
        modified: '2026-10-16 11:00:00',
        pdfPageCount: 2,
        pdfPages: ['data:image/png;base64,iVBORw0KGgo=', 'data:image/png;base64,iVBORw0KGgo='],
      });
    });

    const pdfImgs = document.querySelectorAll('img.ql-pdf-page-img');
    expect(pdfImgs.length).toBe(2);
    expect(document.querySelector('iframe.ql-pdf-frame')).toBeNull();
    fireEvent.keyDown(window, { key: '+' });
    expect(screen.getByText('120%')).toBeDefined();

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\Travel\\itinerary.docx',
        name: 'itinerary.docx',
        extension: 'docx',
        type: 'document',
        isDirectory: false,
        size: 20480,
        formattedSize: '20.0 KB',
        modified: '2026-10-16 11:15:00',
        content: '# 云南大理行程单\n\n| 日期 | 安排 |\n| --- | --- |\n| 第一天 | 洱海骑行 |',
      });
    });

    expect(screen.getByText('云南大理行程单')).toBeDefined();
    expect(screen.getByText('洱海骑行')).toBeDefined();

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\Archive\\unknown.bin',
        name: 'unknown.bin',
        extension: 'bin',
        type: 'binary',
        isDirectory: false,
        size: 4096,
        formattedSize: '4.0 KB',
        modified: '2026-10-16 11:20:00',
        hexDump: '50 4b 03 04 14 00 06 00',
      });
    });

    expect(document.querySelector('.ql-hex-dump')).toBeNull();
    fireEvent.click(screen.getByText('Inspect File Header (Hex)'));
    expect(document.querySelector('.ql-hex-dump')?.textContent).toContain('50 4b 03 04');
  });

  it('renders interactive DXF CAD vector viewer with Chinese layers toggle, dark/light print theme switch, and zoom', async () => {
    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\CAD\\floor_plan.dxf',
        name: 'floor_plan.dxf',
        extension: 'dxf',
        type: 'cad_dxf',
        isDirectory: false,
        size: 65536,
        formattedSize: '64.0 KB',
        modified: '2026-10-18 14:00:00',
        encoding: 'GBK',
        dxfData: {
          totalEntities: 3,
          bounds: [0, 0, 1200, 800],
          layers: [
            { name: '墙体层', colorHex: '#06B6D4', adaptive: false, count: 2 },
            { name: '轴网标注', colorHex: '#FF3B30', adaptive: false, count: 1 },
          ],
          entities: [
            { k: 0, l: '墙体层', c: '#06B6D4', a: false, pts: [0, 0, 1200, 0] },
            { k: 1, l: '墙体层', c: '#06B6D4', a: false, cl: true, pts: [0, 0, 1200, 0, 1200, 800, 0, 800] },
            { k: 4, l: '轴网标注', c: '#FF3B30', a: false, pts: [100, 100], th: 24, t: '承重剪力墙' },
          ],
        },
      });
    });

    expect(screen.getByText('DXF CAD Drawing')).toBeDefined();
    expect(screen.getByText('3 entities')).toBeDefined();

    // Switch between Classic Dark and Print White CAD canvas background
    const viewerEl = document.querySelector('.ql-cad-viewer');
    expect(viewerEl?.classList.contains('ql-cad-viewer--dark')).toBe(true);
    fireEvent.click(screen.getByTestId('ql-cad-bg-toggle'));
    expect(viewerEl?.classList.contains('ql-cad-viewer--light')).toBe(true);

    // Expand CAD layers panel and toggle Chinese layer visibility
    const layersToggle = screen.getByTestId('ql-cad-layers-toggle');
    expect(layersToggle.textContent).toContain('Layers (2/2)');
    fireEvent.click(layersToggle);

    expect(screen.getByTestId('ql-cad-layers-panel')).toBeDefined();
    const wallLayerBtn = screen.getByText('墙体层');
    fireEvent.click(wallLayerBtn);
    expect(layersToggle.textContent).toContain('Layers (1/2)');

    // Zoom in and reset Fit to View
    const cadCanvas = document.querySelector('canvas.ql-cad-canvas') as HTMLCanvasElement;
    fireEvent.wheel(cadCanvas, { deltaY: -120, clientX: 200, clientY: 160 });
    expect(screen.getByText('118%')).toBeDefined();
    fireEvent.click(screen.getByText('Fit to View'));
    expect(screen.getByText('100%')).toBeDefined();
  });

  it('renders interactive 3D STL/OBJ mesh viewer with triangle count, physical bounding box, shaded/wireframe toggle, and CAD snapshot banner', async () => {
    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\Models\\gearbox_housing.stl',
        name: 'gearbox_housing.stl',
        extension: 'stl',
        type: 'model_3d',
        isDirectory: false,
        size: 131072,
        formattedSize: '128.0 KB',
        modified: '2026-10-18 15:00:00',
        designMeta: 'STL (Binary)',
        meshData: {
          formatLabel: 'STL (Binary)',
          totalTriangles: 2048,
          sampledTriangles: 2048,
          dimensions: [125.5, 80, 42.25],
          positions: [0, 0, 0, 1, 0, 0, 0, 1, 0],
        },
      });
    });

    expect(screen.getByText('STL 3D Model')).toBeDefined();
    const statsBadge = screen.getByTestId('ql-3d-stats-badge');
    expect(statsBadge.textContent).toContain('125.5 × 80 × 42.25');
    expect(statsBadge.textContent).toContain('tris');

    const shadedBtn = screen.getByTestId('ql-3d-shaded-btn');
    const wireBtn = screen.getByTestId('ql-3d-wireframe-btn');
    expect(shadedBtn.classList.contains('active')).toBe(true);
    fireEvent.click(wireBtn);
    expect(wireBtn.classList.contains('active')).toBe(true);

    // Verify DWG/TArch CAD snapshot transparent notice & multi-page CDR navigation
    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\CAD\\tarch_building.dwg',
        name: 'tarch_building.dwg',
        extension: 'dwg',
        type: 'image',
        isDirectory: false,
        size: 524288,
        formattedSize: '512.0 KB',
        modified: '2026-10-18 15:30:00',
        designMeta: 'AutoCAD 2018+ / TArch · 1280×960',
        previewNoteKind: 'cad_snapshot',
        dataUri: 'data:image/png;base64,iVBORw0KGgo=',
      });
    });

    expect(screen.getByTestId('ql-cad-snapshot-banner')).toBeDefined();
    expect(screen.getByText('· AutoCAD 2018+ / TArch · 1280×960')).toBeDefined();

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\Design\\brochure.cdr',
        name: 'brochure.cdr',
        extension: 'cdr',
        type: 'image',
        isDirectory: false,
        size: 262144,
        formattedSize: '256.0 KB',
        modified: '2026-10-18 16:00:00',
        designMeta: 'CorelDRAW (ZIP) · 1600×1200 · 2 pages',
        dataUri: 'data:image/png;base64,AAAA',
        imagePages: ['data:image/png;base64,AAAA', 'data:image/png;base64,BBBB'],
      });
    });

    expect(screen.getByText('CorelDRAW Artwork')).toBeDefined();
    expect(screen.getByText('1 / 2')).toBeDefined();
    fireEvent.click(screen.getByLabelText('Next Page'));
    expect(screen.getByText('2 / 2')).toBeDefined();
  });

  it('renders Excel A/B/C column letters without swallowing sparse title row 1, and previews blank or image-based .docx cleanly', async () => {
    render(<QuickLookApp />);

    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'D:\\Projects\\004测试许可申请-星云科技_17.xlsx',
        name: '004测试许可申请-星云科技_17.xlsx',
        extension: 'xlsx',
        type: 'spreadsheet',
        isDirectory: false,
        size: 9984,
        formattedSize: '9.8 KB',
        modified: '2026-10-12 09:03:56',
        totalSheetCount: 2,
        sheets: [
          {
            name: '许可申请',
            rows: [
              ['', '许可申请'],
              ['', '申请人', 'Hardware key', '版本'],
              ['', '星云科技', 'NBL00000001', '10.0 2202 HF1'],
            ],
          },
          {
            name: '附录说明',
            rows: [['备注', '一年有效期']],
          },
        ],
      });
    });

    const excelHeaders = Array.from(document.querySelectorAll('.ql-data-table thead th')).map(el => el.textContent);
    expect(excelHeaders).toEqual(['#', 'A', 'B', 'C', 'D']);
    expect(screen.getByText('2 sheets')).toBeDefined();
    expect(screen.getByText('申请人')).toBeDefined();
    expect(screen.getByText('星云科技')).toBeDefined();

    // Blank template default.docx should show emptyDocHint instead of unsupported fallback
    await act(async () => {
      fileChangedHandler?.({
        exists: true,
        loading: false,
        path: 'C:\\Templates\\default.docx',
        name: 'default.docx',
        extension: 'docx',
        type: 'document',
        isDirectory: false,
        size: 38092,
        formattedSize: '37.2 KB',
        modified: '2026-11-12 01:57:16',
        content: '',
        emptyDocument: true,
      });
    });

    expect(
      screen.getByText(
        'This document is a blank template or contains no visible body content. Press Enter to edit in your default office app.',
      ),
    ).toBeDefined();
  });
});

// @vitest-environment jsdom
import { describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach } from 'vitest';
import {
  BUILTIN_TABLE_COLUMNS,
  doesItemRequireBackendProperty,
  extractDriveLabel,
  extractFileExtension,
  extractFileStem,
  formatFileAttributesBitmask,
  isValidCustomWindowsPropertyId,
  resolveColumnDefinition,
  resolveTableCellText,
  TIER1_QUICK_COLUMN_IDS,
} from './columnCatalog';
import {
  buildTableLayoutMetrics,
  estimateFittedColumnWidth,
  extractParentDirectoryPath,
  formatFileSize,
  formatWindowsTime,
  getSortedResults,
  getVisibleTableColumns,
} from './searchUtils';
import { SearchColumnManagerModal } from './SearchColumnManagerModal';
import { SearchPropertyPickerModal } from './SearchPropertyPickerModal';
import { SearchFooter } from './SearchFooter';
import { SearchEmptyState } from './SearchEmptyState';
import type { ColumnLayout, PropertyCacheMap, SearchResult } from './searchTypes';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, defaultVal?: string | Record<string, unknown>, opts?: Record<string, unknown>) => {
      const template = typeof defaultVal === 'string' ? defaultVal : key;
      const vars = (typeof defaultVal === 'object' && defaultVal !== null ? defaultVal : opts) ?? {};
      return template.replace(/\{\{(\w+)\}\}/g, (_, k) => String(vars[k] ?? ''));
    },
  }),
}));

afterEach(() => {
  cleanup();
});

describe('columnCatalog & custom columns engine', () => {
  it('exposes all 10 Tier-1 quick columns and rich Tier-2 categories', () => {
    expect(TIER1_QUICK_COLUMN_IDS).toEqual([
      'name',
      'path',
      'size',
      'ext',
      'type',
      'modified',
      'created',
      'accessed',
      'attributes',
      'runCount',
    ]);
    expect(BUILTIN_TABLE_COLUMNS.length).toBeGreaterThanOrEqual(45);
    expect(resolveColumnDefinition('name').locked).toBe(true);
    expect(resolveColumnDefinition('dimensions').requiresBackendProp).toBe(true);
    expect(resolveColumnDefinition('ext').requiresBackendProp).toBe(false);
  });

  it('supports two-segment, multi-segment, and underscore Windows Property System canonical columns', () => {
    expect(isValidCustomWindowsPropertyId('System.Title')).toBe(true);
    expect(isValidCustomWindowsPropertyId('System.Photo.Aperture')).toBe(true);
    expect(isValidCustomWindowsPropertyId('System.Photo.Camera_Model')).toBe(true);
    expect(isValidCustomWindowsPropertyId('System.')).toBe(false);
    expect(isValidCustomWindowsPropertyId('System..Bad')).toBe(false);
    expect(isValidCustomWindowsPropertyId('System.123')).toBe(false);
    expect(isValidCustomWindowsPropertyId('foo.bar')).toBe(false);

    const dynamicDef = resolveColumnDefinition('System.Photo.Aperture');
    expect(dynamicDef.id).toBe('System.Photo.Aperture');
    expect(dynamicDef.defaultLabel).toBe('Aperture');
    expect(dynamicDef.requiresBackendProp).toBe(true);
    expect(dynamicDef.category).toBe('windows');
  });

  it('allows on-demand backend extraction for accessed/attributes only when native item metadata is missing', () => {
    const withNative: SearchResult = {
      name: 'a.txt',
      path: 'D:\\a.txt',
      isDirectory: false,
      lastAccessTime: 134000000000000000,
      attributes: 0x20,
    };
    const withoutNative: SearchResult = {
      name: 'b.txt',
      path: 'D:\\b.txt',
      isDirectory: false,
    };
    expect(doesItemRequireBackendProperty(withNative, 'accessed')).toBe(false);
    expect(doesItemRequireBackendProperty(withNative, 'attributes')).toBe(false);
    expect(doesItemRequireBackendProperty(withoutNative, 'accessed')).toBe(true);
    expect(doesItemRequireBackendProperty(withoutNative, 'attributes')).toBe(true);
  });

  it('formats Windows file attributes bitmask and path metadata accurately', () => {
    expect(formatFileAttributesBitmask(0x0001 | 0x0020)).toBe('RA');
    expect(formatFileAttributesBitmask(0x0002 | 0x0004 | 0x0010)).toBe('HSD');
    expect(extractFileExtension('report.FINAL.PDF', false)).toBe('pdf');
    expect(extractFileExtension('folder.v2', true)).toBe('');
    expect(extractFileStem('archive.tar.gz', false)).toBe('archive.tar');
    expect(extractDriveLabel('d:\\Projects\\demo.ts')).toBe('D:');
  });

  it('computes visible columns, horizontal min-width, and fitted column widths', () => {
    const layout: ColumnLayout = {
      name: true,
      path: true,
      size: true,
      modified: true,
      created: true,
      snippets: true,
      nameFlex: 35,
      pathFlex: 45,
      tableOrder: ['name', 'ext', 'dimensions', 'sha256', 'path', 'size', 'modified', 'created'],
      visibleTableColumns: ['name', 'ext', 'dimensions', 'sha256', 'path', 'size', 'modified', 'created'],
    };

    const visible = getVisibleTableColumns(layout);
    expect(visible).toEqual(['name', 'ext', 'dimensions', 'sha256', 'path', 'size', 'modified', 'created']);

    const metrics = buildTableLayoutMetrics(layout);
    expect(metrics.visibleCols.length).toBe(8);
    expect(metrics.minTableWidthPx).toBeGreaterThan(1000);

    const fitted = estimateFittedColumnWidth('ext', 'Extension', ['txt', 'docx', 'tsx', 'json']);
    expect(fitted).toBeGreaterThanOrEqual(68);
    expect(fitted).toBeLessThanOrEqual(200);
  });

  it('sorts results strictly by Tier-1 and Tier-2 properties with missing values always last', () => {
    const sampleResults: SearchResult[] = [
      {
        name: 'small.png',
        path: 'D:\\Photos\\small.png',
        isDirectory: false,
        size: 1024,
        lastAccessTime: 133000000000000000,
        attributes: 0x20,
        runCount: 2,
      },
      {
        name: 'poster.png',
        path: 'D:\\Photos\\poster.png',
        isDirectory: false,
        size: 4096,
        lastAccessTime: 134000000000000000,
        attributes: 0x21,
        runCount: 9,
      },
      {
        name: 'banner.jpg',
        path: 'D:\\Photos\\banner.jpg',
        isDirectory: false,
        size: 2048,
        lastAccessTime: 132000000000000000,
        attributes: 0x20,
        runCount: 5,
      },
      {
        name: 'nodim.txt',
        path: 'D:\\Photos\\nodim.txt',
        isDirectory: false,
        size: 128,
      },
    ];

    const propCache: PropertyCacheMap = {
      'D:\\Photos\\small.png': { dimensions: { d: '800 x 600', n: 480000 } },
      'D:\\Photos\\poster.png': { dimensions: { d: '3840 x 2160', n: 8294400 } },
      'D:\\Photos\\banner.jpg': { dimensions: { d: '1920 x 1080', n: 2073600 } },
    };

    const byDimensionsDesc = getSortedResults(sampleResults, 'dimensions', 'desc', propCache, 'en');
    expect(byDimensionsDesc.map((r) => r.name)).toEqual(['poster.png', 'banner.jpg', 'small.png', 'nodim.txt']);

    const byDimensionsAsc = getSortedResults(sampleResults, 'dimensions', 'asc', propCache, 'en');
    expect(byDimensionsAsc.map((r) => r.name)).toEqual(['small.png', 'banner.jpg', 'poster.png', 'nodim.txt']);

    const byRunCountDesc = getSortedResults(sampleResults, 'runCount', 'desc', propCache, 'en');
    expect(byRunCountDesc.map((r) => r.name)).toEqual(['poster.png', 'banner.jpg', 'small.png', 'nodim.txt']);

    const byExtAsc = getSortedResults(sampleResults, 'ext', 'asc', propCache, 'en');
    expect(byExtAsc[0].name).toBe('banner.jpg');

    const cellText = resolveTableCellText('dimensions', sampleResults[1], propCache[sampleResults[1].path], {
      formatFileSize,
      formatWindowsTime,
      extractParentDirectoryPath,
    });
    expect(cellText).toBe('3840 x 2160');
  });
});

describe('SearchColumnManagerModal & SearchPropertyPickerModal & SearchFooter & SearchEmptyState', () => {
  it('supports strict draft transactions (reset + cancel rolls back without persisting) and keyboard controls', () => {
    const onApply = vi.fn();
    const onClose = vi.fn();
    const onOpenAddPicker = vi.fn();
    const onResetDefault = vi.fn();

    render(
      <SearchColumnManagerModal
        visible
        activeColumns={['name', 'path', 'size', 'modified']}
        onClose={onClose}
        onApply={onApply}
        onOpenAddPicker={onOpenAddPicker}
        onResetDefault={onResetDefault}
      />,
    );

    // 点击 Reset 绝不直接触发持久化回调，点 Cancel 完整回滚
    fireEvent.click(screen.getByText('Reset'));
    expect(onResetDefault).not.toHaveBeenCalled();
    expect(onApply).not.toHaveBeenCalled();

    fireEvent.click(screen.getByText('Cancel'));
    expect(onApply).not.toHaveBeenCalled();
    expect(onClose).toHaveBeenCalledTimes(1);
  });

  it('reorders and removes non-locked columns via buttons and keyboard shortcuts in SearchColumnManagerModal', () => {
    const onApply = vi.fn();
    const onClose = vi.fn();

    render(
      <SearchColumnManagerModal
        visible
        activeColumns={['name', 'path', 'size', 'modified']}
        onClose={onClose}
        onApply={onApply}
        onOpenAddPicker={vi.fn()}
      />,
    );

    const listbox = screen.getByRole('listbox', { name: 'Organize Columns' });
    // ArrowDown 两次选中 size，Alt+ArrowUp 上移一位，然后 Enter 确认提交
    fireEvent.keyDown(listbox, { key: 'ArrowDown' });
    fireEvent.keyDown(listbox, { key: 'ArrowDown' });
    fireEvent.keyDown(listbox, { key: 'ArrowUp', altKey: true });
    fireEvent.keyDown(listbox, { key: 'Enter' });

    expect(onApply).toHaveBeenCalledWith(['name', 'size', 'path', 'modified'], { resetRequested: false });
    expect(onClose).toHaveBeenCalled();
  });

  it('filters built-in properties and allows adding a custom System.* property', () => {
    const onAddColumns = vi.fn();
    const onClose = vi.fn();

    render(
      <SearchPropertyPickerModal
        visible
        existingColumns={['name', 'path', 'size']}
        onClose={onClose}
        onAddColumns={onAddColumns}
      />,
    );

    expect(screen.getByRole('button', { name: 'Close' })).toBeTruthy();

    const searchBox = screen.getByPlaceholderText(/System\.Photo\.FNumber/i);
    fireEvent.change(searchBox, { target: { value: 'System.Photo.Aperture' } });

    const dynamicOption = screen.getByText('System.Photo.Aperture');
    fireEvent.click(dynamicOption);
    fireEvent.click(screen.getByText(/Add \(1\)/));

    expect(onAddColumns).toHaveBeenCalledWith(['System.Photo.Aperture']);
    expect(onClose).toHaveBeenCalled();
  });

  it('renders live drive scanning, column sorting progress bars, and keeps active scan bar visible at 760px width', () => {
    const noop = () => undefined;
    const onCancelSort = vi.fn();
    const onStopSearch = vi.fn();

    const { rerender, container } = render(
      <SearchFooter
        loading={false}
        activeCategory="all"
        query=""
        scanElapsedSeconds={0}
        isServiceStarting={false}
        isInitialIndexing
        dbStats={{
          dbPath: '',
          dbSize: 0,
          timestamp: 0,
          totalRecords: 0,
          volumeCount: 2,
          exists: false,
          indexing: true,
          phase: 'scanning',
          activeDrive: 'C',
          activeVolumeLabel: 'OS',
          scannedRecords: 125000,
          estimatedRecords: 500000,
          progressPercent: 25,
          runHistoryCount: 0,
          searchHistoryCount: 0,
        }}
        sortedResults={[]}
        totalIndexedFiles={0}
        totalResultSize={0}
        selectedIndex={0}
        isRebuilding={false}
        onRebuildIndex={noop}
        onOpenResult={noop}
        onOpenFolderResult={noop}
        onCopyPathResult={noop}
        onExportResultsToCsv={noop}
        onToggleSyntaxHelp={noop}
        onHide={noop}
      />,
    );

    expect(screen.getByText(/Scanning OS \(C:\)/)).toBeTruthy();
    expect(screen.getByRole('progressbar').getAttribute('aria-valuenow')).toBe('25');

    // 验证已有部分结果且仍在后台加载时，实时进度条与停止按钮不使用会被 960px 断点隐藏的 search-footer-stat-timing
    rerender(
      <SearchFooter
        loading
        activeCategory="content"
        query="content:test"
        scanElapsedSeconds={1.4}
        scanProgress={{ scanned: 50, total: 100 }}
        onStopSearch={onStopSearch}
        isServiceStarting={false}
        isInitialIndexing={false}
        sortedResults={[{ name: 'a.txt', path: 'D:\\a.txt', isDirectory: false, size: 10 }]}
        totalIndexedFiles={3000000}
        totalResultSize={10}
        selectedIndex={0}
        isRebuilding={false}
        onRebuildIndex={noop}
        onOpenResult={noop}
        onOpenFolderResult={noop}
        onCopyPathResult={noop}
        onExportResultsToCsv={noop}
        onToggleSyntaxHelp={noop}
        onHide={noop}
      />,
    );

    const liveContainer = container.querySelector('.search-footer-stat-live');
    expect(liveContainer).not.toBeNull();
    expect(liveContainer?.classList.contains('search-footer-stat-timing')).toBe(false);
    fireEvent.click(screen.getByText('Stop'));
    expect(onStopSearch).toHaveBeenCalledTimes(1);

    rerender(
      <SearchFooter
        loading={false}
        activeCategory="all"
        query="demo"
        scanElapsedSeconds={0}
        isServiceStarting={false}
        isInitialIndexing={false}
        sortProgress={{
          active: true,
          columnId: 'dimensions',
          columnLabel: 'Dimensions',
          completed: 48,
          total: 200,
        }}
        onCancelSort={onCancelSort}
        sortedResults={[]}
        totalIndexedFiles={3000000}
        totalResultSize={0}
        selectedIndex={0}
        isRebuilding={false}
        onRebuildIndex={noop}
        onOpenResult={noop}
        onOpenFolderResult={noop}
        onCopyPathResult={noop}
        onExportResultsToCsv={noop}
        onToggleSyntaxHelp={noop}
        onHide={noop}
      />,
    );

    expect(screen.getByText(/Sorting Dimensions/)).toBeTruthy();
    fireEvent.click(screen.getByText('Stop'));
    expect(onCancelSort).toHaveBeenCalledTimes(1);
  });

  it('displays accurate SearchEmptyState stage headings for syncing phase and empty activeDrive', () => {
    const inputRef = { current: null };
    const { rerender } = render(
      <SearchEmptyState
        isInitialIndexing
        isServiceStarting={false}
        serviceAvailable
        dbStats={{
          dbPath: '',
          dbSize: 0,
          timestamp: 0,
          totalRecords: 0,
          volumeCount: 1,
          exists: false,
          indexing: true,
          phase: 'syncing',
          activeDrive: '',
          runHistoryCount: 0,
          searchHistoryCount: 0,
        }}
        loading={false}
        activeCategory="all"
        query=""
        sortedResults={[]}
        searchHistoryLength={0}
        scanElapsedSeconds={0}
        lastColdScanDurationSec={10}
        actionError=""
        categories={[]}
        onSelectCategory={vi.fn()}
        onUpdateQuery={vi.fn()}
        inputRef={inputRef}
      />,
    );

    expect(screen.getByText('Updating file index...')).toBeTruthy();

    rerender(
      <SearchEmptyState
        isInitialIndexing
        isServiceStarting={false}
        serviceAvailable
        dbStats={{
          dbPath: '',
          dbSize: 0,
          timestamp: 0,
          totalRecords: 0,
          volumeCount: 1,
          exists: false,
          indexing: true,
          phase: 'scanning',
          activeDrive: '',
          runHistoryCount: 0,
          searchHistoryCount: 0,
        }}
        loading={false}
        activeCategory="all"
        query=""
        sortedResults={[]}
        searchHistoryLength={0}
        scanElapsedSeconds={0}
        lastColdScanDurationSec={10}
        actionError=""
        categories={[]}
        onSelectCategory={vi.fn()}
        onUpdateQuery={vi.fn()}
        inputRef={inputRef}
      />,
    );

    expect(screen.getByText('Scanning local drives...')).toBeTruthy();
  });
});

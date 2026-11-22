import { describe, expect, it, vi } from 'vitest';
import {
  buildSmartContentSearchQuery,
  collectColumnFitSampleRows,
  extractExplicitContentTerms,
  extractParentDirectoryPath,
  extractParentFolder,
  getSortedResults,
  hasContentSyntaxInQuery,
  hasEmptyContentKeywordSyntax,
  isEffectiveContentSearch,
  parseExplicitSortDirective,
  parseQueryKeywords,
  stripContentSyntaxFromQuery,
} from './searchUtils';
import {
  safeStorageGetBoolean,
  safeStorageGetEnum,
  safeStorageGetJsonArray,
  safeStorageGetJsonRecord,
  safeStorageGetNumber,
} from './storageUtils';
import type { PropertyCacheMap, SearchResult } from './searchTypes';

const mockResults: SearchResult[] = [
  {
    path: 'C:\\test\\subfolder',
    name: 'subfolder',
    isDirectory: true,
    size: 0,
    lastWriteTime: 1000,
    creationTime: 1000,
    matchRank: 1,
  },
  {
    path: 'C:\\test\\app.exe',
    name: 'app.exe',
    isDirectory: false,
    size: 2048,
    lastWriteTime: 2000,
    creationTime: 2000,
    matchRank: 10,
  },
  {
    path: 'C:\\test\\document.pdf',
    name: 'document.pdf',
    isDirectory: false,
    size: 1024,
    lastWriteTime: 3000,
    creationTime: 3000,
    matchRank: 5,
  },
  {
    path: 'C:\\test\\another_dir',
    name: 'another_dir',
    isDirectory: true,
    size: 0,
    lastWriteTime: 4000,
    creationTime: 4000,
    matchRank: 20,
  },
];

describe('getSortedResults pure header-controlled sorting', () => {
  it('strictly sorts by modified asc/desc without being hijacked by matchRank or path quality tiers', () => {
    const items: SearchResult[] = [
      {
        path: 'C:\\Users\\demo\\AppData\\Local\\Temp\\newest_temp.txt',
        name: 'newest_temp.txt',
        isDirectory: false,
        size: 100,
        lastWriteTime: 9000,
        matchRank: 99,
      },
      {
        path: 'D:\\Projects\\older_local.txt',
        name: 'older_local.txt',
        isDirectory: false,
        size: 200,
        lastWriteTime: 5000,
        matchRank: 0, // 极高 matchRank 也不得截胡修改时间排序
      },
      {
        path: 'C:\\Users\\demo\\xwechat_files\\msg\\file\\mid_chat.txt',
        name: 'mid_chat.txt',
        isDirectory: false,
        size: 300,
        lastWriteTime: 7000,
        matchRank: 50,
      },
    ];

    const desc = getSortedResults(items, 'modified', 'desc', undefined, 'en');
    expect(desc.map((r) => r.name)).toEqual(['newest_temp.txt', 'mid_chat.txt', 'older_local.txt']);

    const asc = getSortedResults(items, 'modified', 'asc', undefined, 'en');
    expect(asc.map((r) => r.name)).toEqual(['older_local.txt', 'mid_chat.txt', 'newest_temp.txt']);
  });

  it('keeps missing values strictly at the bottom for both asc and desc across built-in and extended columns', () => {
    const items: SearchResult[] = [
      { path: 'D:\\a\\missing.png', name: 'missing.png', isDirectory: false, size: 10, lastWriteTime: 0 },
      { path: 'D:\\a\\small.png', name: 'small.png', isDirectory: false, size: 0, lastWriteTime: 1000 },
      { path: 'D:\\a\\large.png', name: 'large.png', isDirectory: false, size: 4096, lastWriteTime: 3000 },
      { path: 'D:\\a\\folder', name: 'folder', isDirectory: true, size: 0, lastWriteTime: 2000 },
      { path: 'D:\\a\\dash.png', name: 'dash.png', isDirectory: false, size: 512, lastWriteTime: 2500 },
    ];

    const propCache: PropertyCacheMap = {
      'D:\\a\\small.png': { dimensions: { d: '800 x 600', n: 480000 }, artist: { d: 'Alpha' } },
      'D:\\a\\large.png': { dimensions: { d: '3840 x 2160', n: 8294400 }, artist: { d: 'Zeta' } },
      'D:\\a\\dash.png': { dimensions: { d: '—' }, artist: { d: '' } },
      'D:\\a\\missing.png': { dimensions: { d: '', n: Number.NaN } },
    };

    // 扩展数值列升序：有效值 800x600 -> 3840x2160 在前，缺失/NaN/— 全部沉底
    const dimAsc = getSortedResults(items, 'dimensions', 'asc', propCache, 'en');
    expect(dimAsc.slice(0, 2).map((r) => r.name)).toEqual(['small.png', 'large.png']);
    expect(new Set(dimAsc.slice(2).map((r) => r.name))).toEqual(new Set(['missing.png', 'folder', 'dash.png']));

    // 扩展数值列降序：有效值 3840x2160 -> 800x600 在前，缺失值同样沉底
    const dimDesc = getSortedResults(items, 'dimensions', 'desc', propCache, 'en');
    expect(dimDesc.slice(0, 2).map((r) => r.name)).toEqual(['large.png', 'small.png']);
    expect(new Set(dimDesc.slice(2).map((r) => r.name))).toEqual(new Set(['missing.png', 'folder', 'dash.png']));

    // 扩展文本列升序：空值绝不霸占顶部
    const artistAsc = getSortedResults(items, 'artist', 'asc', propCache, 'en');
    expect(artistAsc.slice(0, 2).map((r) => r.name)).toEqual(['small.png', 'large.png']);

    // 大小列：0 B 文件是有效值（升序第一），目录无大小显示 '—' 无论升降序均沉底
    const sizeAsc = getSortedResults(items, 'size', 'asc', propCache, 'en');
    expect(sizeAsc[0].name).toBe('small.png');
    expect(sizeAsc[sizeAsc.length - 1].name).toBe('folder');

    const sizeDesc = getSortedResults(items, 'size', 'desc', propCache, 'en');
    expect(sizeDesc[0].name).toBe('large.png');
    expect(sizeDesc[sizeDesc.length - 1].name).toBe('folder');

    // 修改时间列：0 时间戳缺失值无论升降序均沉底
    const modAsc = getSortedResults(items, 'modified', 'asc', propCache, 'en');
    expect(modAsc[0].name).toBe('small.png');
    expect(modAsc[modAsc.length - 1].name).toBe('missing.png');
  });

  it('uses matchRank -> shallow path -> localeCompare numeric tie-breaker chain when column values are equal', () => {
    const tiedItems: SearchResult[] = [
      { path: 'C:\\a\\b\\c\\file10.txt', name: 'file10.txt', isDirectory: false, lastWriteTime: 5000, matchRank: 2 },
      { path: 'C:\\a\\b\\c\\file2.txt', name: 'file2.txt', isDirectory: false, lastWriteTime: 5000, matchRank: 2 },
      { path: 'D:\\top\\shallow.txt', name: 'shallow.txt', isDirectory: false, lastWriteTime: 5000, matchRank: 2 },
      { path: 'C:\\deep\\x\\y\\best_rank.txt', name: 'best_rank.txt', isDirectory: false, lastWriteTime: 5000, matchRank: 1 },
    ];

    const sorted = getSortedResults(tiedItems, 'modified', 'desc', undefined, 'en-US');
    expect(sorted.map((r) => r.name)).toEqual([
      'best_rank.txt',
      'shallow.txt',
      'file2.txt',
      'file10.txt',
    ]);
  });

  it('sorts names naturally with caller-provided locale', () => {
    const sorted = getSortedResults(mockResults, 'name', 'asc', undefined, 'en-US');
    expect(sorted.map((r) => r.name)).toEqual(['another_dir', 'app.exe', 'document.pdf', 'subfolder']);
  });
});

describe('parseExplicitSortDirective & collectColumnFitSampleRows', () => {
  it('parses only standalone sort: tokens without false positives on substrings', () => {
    expect(parseExplicitSortDirective('report sort:size')).toEqual({ field: 'size', direction: 'desc' });
    expect(parseExplicitSortDirective('report sort:size-asc')).toEqual({ field: 'size', direction: 'asc' });
    expect(parseExplicitSortDirective('sort:name-desc ext:txt')).toEqual({ field: 'name', direction: 'desc' });
    expect(parseExplicitSortDirective('my_sort:name.txt')).toBeNull();
    expect(parseExplicitSortDirective('"sort:size"')).toBeNull();
    expect(parseExplicitSortDirective('sort:folder')).toBeNull();
  });

  it('prioritizes visibleRange rows when sampling for column auto-fit', () => {
    const bigList: SearchResult[] = Array.from({ length: 500 }, (_, i) => ({
      name: `item-${i}.txt`,
      path: `D:\\dir\\item-${i}.txt`,
      isDirectory: false,
    }));
    const sampled = collectColumnFitSampleRows(bigList, { start: 300, end: 340 }, 120);
    expect(sampled).toHaveLength(120);
    expect(sampled.some((r) => r.name === 'item-300.txt')).toBe(true);
    expect(sampled.some((r) => r.name === 'item-339.txt')).toBe(true);
    expect(sampled.some((r) => r.name === 'item-0.txt')).toBe(true);
  });
});

describe('storageUtils resilient localStorage helpers', () => {
  it('gracefully handles malformed JSON, NaN, non-arrays, and throwing storage', () => {
    const store = new Map<string, string>([
      ['bad_json', '{not valid'],
      ['obj_not_arr', '{"a":1}'],
      ['arr_not_obj', '[1,2,3]'],
      ['nan_num', 'NaN'],
      ['bad_limit', '999'],
      ['good_limit', '100'],
      ['bool_true', 'true'],
      ['enum_bad', 'invalid'],
    ]);
    const mockStorage = {
      getItem: (k: string) => store.get(k) ?? null,
      setItem: (k: string, v: string) => { store.set(k, v); },
      removeItem: (k: string) => { store.delete(k); },
    };
    vi.stubGlobal('window', { localStorage: mockStorage });

    expect(safeStorageGetJsonArray('bad_json', (v): v is string => typeof v === 'string', ['fallback'])).toEqual(['fallback']);
    expect(safeStorageGetJsonArray('obj_not_arr', (v): v is string => typeof v === 'string', [])).toEqual([]);
    expect(safeStorageGetJsonRecord('arr_not_obj')).toBeNull();
    expect(safeStorageGetNumber('nan_num', 0, { integer: true })).toBe(0);
    expect(safeStorageGetNumber('bad_limit', 0, { integer: true, allowedValues: [0, 50, 100, 200] })).toBe(0);
    expect(safeStorageGetNumber('good_limit', 0, { integer: true, allowedValues: [0, 50, 100, 200] })).toBe(100);
    expect(safeStorageGetBoolean('bool_true', false)).toBe(true);
    expect(safeStorageGetEnum('enum_bad', ['asc', 'desc'] as const, 'desc')).toBe('desc');

    vi.stubGlobal('window', {
      localStorage: {
        getItem: () => { throw new Error('SecurityError'); },
      },
    });
    expect(safeStorageGetNumber('good_limit', 42)).toBe(42);
    vi.unstubAllGlobals();
  });
});

describe('parseQueryKeywords & Content Search Syntax Helpers', () => {
  it('extracts highlightable tokens while filtering out !exclude terms and non-highlightable filter operators', () => {
    expect(parseQueryKeywords('ext:txt document* !exclude')).toEqual(['document']);
    expect(parseQueryKeywords('ext:docx content:"hello world" !"draft v1" dm:today size:>1mb')).toEqual(['hello', 'world']);
    expect(parseQueryKeywords('case:Tools3000 !temp path:src')).toEqual(['Tools3000']);
    expect(parseQueryKeywords('content: 采购合同')).toEqual(['采购合同']);
    expect(parseQueryKeywords('')).toEqual([]);
  });

  it('recognizes content:, 内容:, and c:keyword at any position without mistaking drive paths', () => {
    expect(hasContentSyntaxInQuery('ext:docx content:合同')).toBe(true);
    expect(hasContentSyntaxInQuery('D:\\Work 内容:预算')).toBe(true);
    expect(hasContentSyntaxInQuery('size:>10kb c:甲方')).toBe(true);
    expect(hasContentSyntaxInQuery('content: 违约责任')).toBe(true);
    expect(hasContentSyntaxInQuery('c:\\repo report')).toBe(false);
    expect(hasContentSyntaxInQuery('c: report')).toBe(false);
    expect(hasEmptyContentKeywordSyntax('ext:docx content:')).toBe(true);
    expect(hasEmptyContentKeywordSyntax('ext:docx content:合同')).toBe(false);
    expect(extractExplicitContentTerms('ext:docx content:"hello world" content: 合同')).toEqual([
      'hello world',
      '合同',
    ]);
  });

  it('detects effective content search state across categories, modes, and mid-query syntax', () => {
    expect(isEffectiveContentSearch({ query: 'ext:docx content:合同', activeCategory: 'doc', searchMode: 'name' })).toBe(true);
    expect(isEffectiveContentSearch({ query: '合同', activeCategory: 'all', searchMode: 'content' })).toBe(true);
    expect(isEffectiveContentSearch({ query: '合同', activeCategory: 'all', searchMode: 'both' })).toBe(true);
    expect(isEffectiveContentSearch({ query: 'ext:docx 合同', activeCategory: 'doc', searchMode: 'name' })).toBe(false);
  });

  it('smartly converts queries to content search while preserving file filters and avoiding duplicate prefixes', () => {
    expect(buildSmartContentSearchQuery('合同')).toBe('content:合同');
    expect(buildSmartContentSearchQuery('"hello world"')).toBe('content:"hello world"');
    expect(buildSmartContentSearchQuery('ext:docx D:\\Work 合同')).toBe('ext:docx D:\\Work content:合同');
    expect(buildSmartContentSearchQuery('ext:docx 甲方 违约')).toBe('ext:docx content:甲方 content:违约');
    expect(buildSmartContentSearchQuery('ext:docx content:合同')).toBe('ext:docx content:合同');
    expect(buildSmartContentSearchQuery('ext:docx D:\\Work')).toBe('ext:docx D:\\Work content:');
    expect(stripContentSyntaxFromQuery('ext:docx D:\\Work content:合同')).toBe('ext:docx D:\\Work 合同');
    expect(stripContentSyntaxFromQuery('ext:docx content:')).toBe('ext:docx');
  });

  it('extractParentDirectoryPath returns full parent directory path while extractParentFolder returns short badge label', () => {
    const wechatPath =
      'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2026-09\\report.pdf';
    expect(extractParentFolder(wechatPath)).toBe('WeChat·2026-09');
    expect(extractParentDirectoryPath(wechatPath)).toBe(
      'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2026-09',
    );
    expect(extractParentDirectoryPath('D:\\Projects\\216-北辰智造\\部署手册.txt')).toBe('D:\\Projects\\216-北辰智造');
    expect(extractParentDirectoryPath('D:\\root_file.txt')).toBe('D:\\');
  });
});

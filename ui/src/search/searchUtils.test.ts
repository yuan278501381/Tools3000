import { describe, expect, it } from 'vitest';
import {
  buildSmartContentSearchQuery,
  extractExplicitContentTerms,
  extractParentDirectoryPath,
  extractParentFolder,
  getSortedResults,
  hasContentSyntaxInQuery,
  hasEmptyContentKeywordSyntax,
  isEffectiveContentSearch,
  parseQueryKeywords,
  stripContentSyntaxFromQuery,
} from './searchUtils';
import type { SearchResult } from './searchTypes';

const mockResults: SearchResult[] = [
  {
    path: 'C:\\test\\subfolder',
    name: 'subfolder',
    isDirectory: true,
    size: 0,
    lastWriteTime: 1000,
    creationTime: 1000,
  },
  {
    path: 'C:\\test\\app.exe',
    name: 'app.exe',
    isDirectory: false,
    size: 2048,
    lastWriteTime: 2000,
    creationTime: 2000,
  },
  {
    path: 'C:\\test\\document.pdf',
    name: 'document.pdf',
    isDirectory: false,
    size: 1024,
    lastWriteTime: 3000,
    creationTime: 3000,
  },
  {
    path: 'C:\\test\\another_dir',
    name: 'another_dir',
    isDirectory: true,
    size: 0,
    lastWriteTime: 4000,
    creationTime: 4000,
  },
];

describe('getSortedResults with FileFolderPriority', () => {
  it('places files first before folders when priority is filesFirst', () => {
    const sorted = getSortedResults(mockResults, 'name', 'asc', 'filesFirst', false);
    expect(sorted[0].isDirectory).toBe(false);
    expect(sorted[1].isDirectory).toBe(false);
    expect(sorted[2].isDirectory).toBe(true);
    expect(sorted[3].isDirectory).toBe(true);

    expect(sorted[0].name).toBe('app.exe');
    expect(sorted[1].name).toBe('document.pdf');
    expect(sorted[2].name).toBe('another_dir');
    expect(sorted[3].name).toBe('subfolder');
  });

  it('places folders first when priority is foldersFirst', () => {
    const sorted = getSortedResults(mockResults, 'name', 'asc', 'foldersFirst', false);
    expect(sorted[0].isDirectory).toBe(true);
    expect(sorted[1].isDirectory).toBe(true);
    expect(sorted[2].isDirectory).toBe(false);
    expect(sorted[3].isDirectory).toBe(false);

    expect(sorted[0].name).toBe('another_dir');
    expect(sorted[1].name).toBe('subfolder');
  });

  it('mixes files and folders when priority is mixed', () => {
    const sorted = getSortedResults(mockResults, 'name', 'asc', 'mixed', false);
    expect(sorted.map((r) => r.name)).toEqual(['another_dir', 'app.exe', 'document.pdf', 'subfolder']);
  });

  it('supports legacy boolean parameter correctly', () => {
    const sortedFoldersFirst = getSortedResults(mockResults, 'name', 'asc', true, false);
    expect(sortedFoldersFirst[0].isDirectory).toBe(true);

    const sortedMixed = getSortedResults(mockResults, 'name', 'asc', false, false);
    expect(sortedMixed.map((r) => r.name)).toEqual(['another_dir', 'app.exe', 'document.pdf', 'subfolder']);
  });

  it('respects secondary sort direction for filesFirst', () => {
    const sorted = getSortedResults(mockResults, 'modified', 'desc', 'filesFirst', false);
    expect(sorted[0].name).toBe('document.pdf');
    expect(sorted[1].name).toBe('app.exe');
    expect(sorted[2].name).toBe('another_dir');
    expect(sorted[3].name).toBe('subfolder');
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
});

describe('Semantic Quality Tiering & Original-File-First Deduplication', () => {
  it('ranks local original files in normal user directories above WeChat/QQ chat history copies, .lnk shortcuts, and Temp files even when sorted by modified desc', () => {
    const items: SearchResult[] = [
      {
        path: 'C:\\Users\\yuan2\\AppData\\Roaming\\Microsoft\\Windows\\Recent\\解决大量警报导致特定用户登录SAP缓慢V10002.txt.lnk',
        name: '解决大量警报导致特定用户登录SAP缓慢V10002.txt.lnk',
        isDirectory: false,
        size: 907,
        lastWriteTime: 9000, // 最新，但属于 .lnk + Recent
        creationTime: 9000,
      },
      {
        path: 'C:\\Users\\yuan2\\AppData\\Local\\Temp\\解决临时日志.txt',
        name: '解决临时日志.txt',
        isDirectory: false,
        size: 512,
        lastWriteTime: 9500, // 最新，但属于 Temp 目录
        creationTime: 9500,
      },
      {
        path: 'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2026-09\\解决高级总账科目规则更改时科目报错_字段中的无效值.txt',
        name: '解决高级总账科目规则更改时科目报错_字段中的无效值.txt',
        isDirectory: false,
        size: 8500,
        lastWriteTime: 8000, // 微信发送副本，比本地原始文件更新或同时间
        creationTime: 8000,
      },
      {
        path: 'D:\\Projects\\解决高级总账科目规则更改时科目报错_字段中的无效值.txt',
        name: '解决高级总账科目规则更改时科目报错_字段中的无效值.txt',
        isDirectory: false,
        size: 8500,
        lastWriteTime: 7000, // 本地原始文件
        creationTime: 7000,
      },
      {
        path: 'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2026-09\\自动添加TEMP文件夹Everyone完全控制权限（服务器执行解决打印不传递）.rar',
        name: '自动添加TEMP文件夹Everyone完全控制权限（服务器执行解决打印不传递）.rar',
        isDirectory: false,
        size: 28160,
        lastWriteTime: 8500, // 仅存在于微信中的新文件（文件名含 TEMP，但不应误判为临时目录）
        creationTime: 8500,
      },
      {
        path: 'D:\\Documents\\xwechat_files\\wxid_xxx\\msg\\file\\2026-08\\自动添加TEMP文件夹Everyone完全控制权限（服务器执行解决打印不传递）.rar',
        name: '自动添加TEMP文件夹Everyone完全控制权限（服务器执行解决打印不传递）.rar',
        isDirectory: false,
        size: 28160,
        lastWriteTime: 6000, // 同一微信文件在更早月份发给其他协作方的历史冗余副本
        creationTime: 6000,
      },
      {
        path: 'D:\\Projects\\解决：第三方系统调用接口时产生重复数据V10002_MESID2.ch',
        name: '解决：第三方系统调用接口时产生重复数据V10002_MESID2.ch',
        isDirectory: false,
        size: 184500,
        lastWriteTime: 6500, // 本地原始文件已迭代到 V10002
        creationTime: 6500,
      },
      {
        path: 'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2025-06\\解决：第三方系统调用接口时产生重复数据V10001_MESID2.ch',
        name: '解决：第三方系统调用接口时产生重复数据V10001_MESID2.ch',
        isDirectory: false,
        size: 184320,
        lastWriteTime: 6800, // 微信里旧版本 V10001 副本（即便修改时间较新，也因本地存在同源 V10002 原始文件而沉降到冗余聊天副本层）
        creationTime: 6800,
      },
    ];

    const sorted = getSortedResults(items, 'modified', 'desc', 'filesFirst', false, true);

    // Tier 0: D:\Projects 下的两份本地原始文件必须排在最前面（内部按修改时间倒序：7000 > 6500）
    expect(sorted[0].path).toBe('D:\\Projects\\解决高级总账科目规则更改时科目报错_字段中的无效值.txt');
    expect(sorted[1].path).toBe('D:\\Projects\\解决：第三方系统调用接口时产生重复数据V10002_MESID2.ch');

    // Tier 1: 仅存在于微信中的唯一最新文件排在原始文件之后（且文件名含 TEMP 未被误判为 Temp 目录）
    expect(sorted[2].path).toBe(
      'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2026-09\\自动添加TEMP文件夹Everyone完全控制权限（服务器执行解决打印不传递）.rar'
    );

    // Tier 2: 微信中已有本地原始件的聊天副本 + 历史月份重复发送副本（内部按修改时间倒序：8000 > 6800 > 6000）
    expect(sorted[3].path).toBe(
      'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2026-09\\解决高级总账科目规则更改时科目报错_字段中的无效值.txt'
    );
    expect(sorted[4].path).toBe(
      'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2025-06\\解决：第三方系统调用接口时产生重复数据V10001_MESID2.ch'
    );
    expect(sorted[5].path).toBe(
      'D:\\Documents\\xwechat_files\\wxid_xxx\\msg\\file\\2026-08\\自动添加TEMP文件夹Everyone完全控制权限（服务器执行解决打印不传递）.rar'
    );

    // Tier 4 & Tier 5: .lnk 快捷方式与 Temp/Recent 临时目录排在最底部（内部同样按修改时间倒序：9500 > 9000）
    expect(sorted[6].path).toContain('\\Temp\\');
    expect(sorted[7].path).toContain('.lnk');
  });

  it('extractParentDirectoryPath returns full parent directory path while extractParentFolder returns short badge label', () => {
    const wechatPath =
      'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2026-09\\report.pdf';
    expect(extractParentFolder(wechatPath)).toBe('WeChat·2026-09');
    expect(extractParentDirectoryPath(wechatPath)).toBe(
      'C:\\Users\\yuan2\\AppData\\Roaming\\Tencent\\xwechat\\xwechat_files\\wxid_xxx\\msg\\file\\2026-09'
    );
    expect(extractParentDirectoryPath('D:\\Projects\\216-北辰智造\\部署手册.txt')).toBe('D:\\Projects\\216-北辰智造');
    expect(extractParentDirectoryPath('D:\\root_file.txt')).toBe('D:\\');
  });
});

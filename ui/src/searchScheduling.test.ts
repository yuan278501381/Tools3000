import { beforeEach, describe, expect, it } from 'vitest';
import {
  CONTENT_DEBOUNCE_MS,
  NAME_DEBOUNCE_MS,
  isContentSearch,
  isEmptyContentSyntax,
  nextQueryId,
  resetQueryIdForTest,
  resolveDebounceMs,
} from './searchScheduling';

describe('isContentSearch', () => {
  it('treats the content category as a content search regardless of the query', () => {
    expect(isContentSearch({ query: 'oITT', activeCategory: 'content', searchMode: 'name' })).toBe(true);
  });

  it('treats content and both search modes as content searches', () => {
    expect(isContentSearch({ query: 'report', activeCategory: 'all', searchMode: 'content' })).toBe(true);
    expect(isContentSearch({ query: 'report', activeCategory: 'all', searchMode: 'both' })).toBe(true);
  });

  it('recognises explicit content prefixes both at start and after file filters', () => {
    expect(isContentSearch({ query: 'content:budget', activeCategory: 'all', searchMode: 'name' })).toBe(true);
    expect(isContentSearch({ query: '内容:预算', activeCategory: 'all', searchMode: 'name' })).toBe(true);
    expect(isContentSearch({ query: 'c:budget', activeCategory: 'all', searchMode: 'name' })).toBe(true);
    expect(isContentSearch({ query: 'ext:docx content:合同', activeCategory: 'doc', searchMode: 'name' })).toBe(true);
    expect(isContentSearch({ query: 'D:\\Work 内容:预算', activeCategory: 'all', searchMode: 'name' })).toBe(true);
    expect(isEmptyContentSyntax('ext:docx content:')).toBe(true);
    expect(isEmptyContentSyntax('ext:docx content:合同')).toBe(false);
  });

  it('does not mistake a C drive path for the content shorthand', () => {
    expect(isContentSearch({ query: 'c:\\repo', activeCategory: 'all', searchMode: 'name' })).toBe(false);
    expect(isContentSearch({ query: 'C:/repo', activeCategory: 'all', searchMode: 'name' })).toBe(false);
    expect(isContentSearch({ query: 'c: report', activeCategory: 'all', searchMode: 'name' })).toBe(false);
  });

  it('treats a plain filename query as a name search', () => {
    expect(isContentSearch({ query: 'oITT', activeCategory: 'all', searchMode: 'name' })).toBe(false);
  });
});

describe('resolveDebounceMs', () => {
  it('waits longest for content searches, which have to read files', () => {
    expect(resolveDebounceMs({ query: 'c', activeCategory: 'content', searchMode: 'name' }))
      .toBe(CONTENT_DEBOUNCE_MS.short);
    expect(resolveDebounceMs({ query: 'content:cr', activeCategory: 'all', searchMode: 'name' }))
      .toBe(CONTENT_DEBOUNCE_MS.short);
    expect(resolveDebounceMs({ query: 'ext:docx content:cr', activeCategory: 'doc', searchMode: 'name' }))
      .toBe(CONTENT_DEBOUNCE_MS.short);
    expect(resolveDebounceMs({ query: 'content:create', activeCategory: 'all', searchMode: 'name' }))
      .toBe(CONTENT_DEBOUNCE_MS.normal);
    expect(resolveDebounceMs({ query: 'ext:docx content:create', activeCategory: 'doc', searchMode: 'name' }))
      .toBe(CONTENT_DEBOUNCE_MS.normal);
  });

  it('scales the name search delay down as the prefix gets more selective', () => {
    const scope = { activeCategory: 'all', searchMode: 'name' };
    expect(resolveDebounceMs({ ...scope, query: 'o' })).toBe(NAME_DEBOUNCE_MS.veryShort);
    expect(resolveDebounceMs({ ...scope, query: 'oIT' })).toBe(NAME_DEBOUNCE_MS.short);
    expect(resolveDebounceMs({ ...scope, query: 'oITT' })).toBe(NAME_DEBOUNCE_MS.normal);
  });

  it('provides instant millisecond response for search-as-you-type experience', () => {
    const scope = { activeCategory: 'all', searchMode: 'name' };
    expect(resolveDebounceMs({ ...scope, query: 'a' })).toBe(NAME_DEBOUNCE_MS.veryShort);
    expect(resolveDebounceMs({ ...scope, query: 'ab' })).toBe(NAME_DEBOUNCE_MS.short);
    expect(resolveDebounceMs({ ...scope, query: 'abcd' })).toBe(NAME_DEBOUNCE_MS.normal);
    // 确保打字延迟始终保持在极高响应度（<= 50ms）
    for (const query of ['a', 'ab', 'abc', 'abcd', 'abcdefghij']) {
      expect(resolveDebounceMs({ ...scope, query })).toBeLessThanOrEqual(50);
    }
  });
});

describe('nextQueryId', () => {
  beforeEach(() => {
    resetQueryIdForTest();
  });

  it('increases strictly even when several calls land in the same millisecond', () => {
    const first = nextQueryId(1_700_000_000_000);
    const second = nextQueryId(1_700_000_000_000);
    const third = nextQueryId(1_700_000_000_000);
    expect(second).toBeGreaterThan(first);
    expect(third).toBeGreaterThan(second);
  });

  it('follows the wall clock forward', () => {
    nextQueryId(1_700_000_000_000);
    expect(nextQueryId(1_700_000_000_500)).toBe(1_700_000_000_500);
  });

  it('never goes backwards when the clock does', () => {
    const ahead = nextQueryId(1_700_000_000_000);
    expect(nextQueryId(1_600_000_000_000)).toBeGreaterThan(ahead);
  });
});

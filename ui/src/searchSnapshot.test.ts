import { describe, expect, it } from 'vitest';
import { parseQueryKeywords } from './search/searchUtils';
import type { SearchSnapshot } from './search/searchTypes';

describe('parseQueryKeywords', () => {
  it('extracts tokens from simple queries', () => {
    expect(parseQueryKeywords('apple banana orange')).toEqual(['apple', 'banana', 'orange']);
  });

  it('extracts value-bearing prefixes (content:, folder:) while omitting filter-only operators (ext:, dm:, size:)', () => {
    expect(parseQueryKeywords('content:budget ext:pdf dm:today size:>1mb')).toEqual(['budget']);
    expect(parseQueryKeywords('内容:报告 folder:work')).toEqual(['报告', 'work']);
  });

  it('strips quotes and wildcards', () => {
    expect(parseQueryKeywords('*.docx "financial report"')).toEqual(['.docx', 'financial', 'report']);
  });

  it('excludes negated !terms and !"quoted phrases" from highlight keywords', () => {
    expect(parseQueryKeywords('report !draft !"old copy"')).toEqual(['report']);
  });

  it('deduplicates keywords and ignores empty strings', () => {
    expect(parseQueryKeywords('test test test')).toEqual(['test']);
    expect(parseQueryKeywords('')).toEqual([]);
    expect(parseQueryKeywords('   ')).toEqual([]);
  });

  it('skips scope-only drive letters, absolute paths, and path: filter operators', () => {
    expect(parseQueryKeywords('C:\\Projects\\Tools3000 main.cpp')).toEqual(['main.cpp']);
    expect(parseQueryKeywords('D:')).toEqual([]);
    expect(parseQueryKeywords('path:C:\\Windows kernel')).toEqual(['kernel']);
  });
});

describe('SearchSnapshot', () => {
  it('maintains generation lockstep structure', () => {
    const snapshot: SearchSnapshot = {
      generationId: 42,
      query: 'invoice 2026',
      keywords: parseQueryKeywords('invoice 2026'),
      results: [
        { name: 'invoice_2026.pdf', path: 'C:\\docs\\invoice_2026.pdf', isDirectory: false },
      ],
    };

    expect(snapshot.generationId).toBe(42);
    expect(snapshot.keywords).toEqual(['invoice', '2026']);
    expect(snapshot.results).toHaveLength(1);
    expect(snapshot.results[0].name).toBe('invoice_2026.pdf');
  });
});

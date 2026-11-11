import { describe, expect, it } from 'vitest';
import {
  MAX_HIGHLIGHT_LINE_LENGTH,
  highlightCode,
  highlightSingleCodeLine,
  renderMarkdownToHtml,
  renderSimpleMarkdown,
} from './quickLookMarkup';

describe('highlightCode', () => {
  it('escapes executable HTML before adding highlighting markup', () => {
    const html = highlightCode('<img src=x onerror="alert(1)">');

    expect(html).toContain('&lt;img');
    expect(html).toContain('&gt;');
    expect(html).not.toContain('<img');
    expect(html).not.toContain('onerror="');
  });

  it('does not trust text that already resembles an HTML entity', () => {
    expect(highlightCode('&lt;script&gt;')).toContain('&amp;lt;script&amp;gt;');
  });

  it('does not re-tokenise generated spans or tokens inside comments and strings', () => {
    const html = highlightCode('const value = "class 123"; // return alert(1)');

    expect(html).not.toContain('<span <span');
    expect(html.match(/class="tok-string"/g)).toHaveLength(1);
    expect(html.match(/class="tok-comment"/g)).toHaveLength(1);
    expect(html.match(/class="tok-keyword"/g)).toHaveLength(1);
  });
});

describe('renderMarkdownToHtml', () => {
  it('keeps fenced source inert and does not emit inline event handlers', () => {
    const html = renderMarkdownToHtml('```html\n<img src=x onerror=alert(1)>\n```');

    expect(html).toContain('&lt;img');
    expect(html).not.toContain('<img');
    expect(html).not.toContain('onclick=');
    expect(html).toContain('data-quicklook-code=');
  });

  it('escapes raw HTML in ordinary Markdown text', () => {
    const html = renderMarkdownToHtml('# title\n<img src=x onerror=alert(1)>\n<a href="javascript:alert(2)">open</a>\n[link](javascript:alert(3))');

    expect(html).toContain('&lt;img');
    expect(html).not.toContain('<img');
    expect(html).not.toContain('<a ');
    expect(html).not.toContain('href="javascript:');
  });

  it('preserves literal dollar replacement patterns inside fenced code blocks', () => {
    const snippet = 'const out = str.replace(/foo/g, "$& and $$ and $\'");';
    const html = renderMarkdownToHtml(`\`\`\`js\n${snippet}\n\`\`\``);

    expect(html).toContain('$&amp; and $$ and $&#39;');
    expect(html).not.toContain('__QUICKLOOK_CODE_BLOCK_');
    expect(html).not.toContain('TOOLS3000_CODE_BLOCK');
  });

  it('protects C/C++ pointer asterisks and HTML inside inline backtick code spans from emphasis corruption', () => {
    const html = renderSimpleMarkdown('Dereference `int *a = *b;` and `<script>` while keeping *italic* and **bold**.');

    expect(html).toContain('<code class="ql-inline-code">int *a = *b;</code>');
    expect(html).toContain('<code class="ql-inline-code">&lt;script&gt;</code>');
    expect(html).toContain('<em>italic</em>');
    expect(html).toContain('<strong>bold</strong>');
    expect(html).not.toContain('int <em>a = </em>b;');
    expect(html).not.toContain('TOOLS3000_INLINE_CODE');
  });

  it('bypasses regex tokenization for oversized single lines (> 4096 chars) while escaping HTML safely', () => {
    const hugeMinifiedLine = `const payload = "${'x'.repeat(MAX_HIGHLIGHT_LINE_LENGTH + 128)}<img onerror=1>";`;
    const singleHtml = highlightSingleCodeLine(hugeMinifiedLine);
    expect(singleHtml).not.toContain('tok-keyword');
    expect(singleHtml).toContain('&lt;img onerror=1&gt;');

    const mixedHtml = highlightCode(`const normal = 1;\n${hugeMinifiedLine}\nreturn normal;`);
    const lines = mixedHtml.split('\n');
    expect(lines).toHaveLength(3);
    expect(lines[0]).toContain('class="tok-keyword"');
    expect(lines[1]).not.toContain('class="tok-keyword"');
    expect(lines[1]).toContain('&lt;img onerror=1&gt;');
    expect(lines[2]).toContain('class="tok-keyword"');
  });

  it('localizes fenced code block copy button label when copyLabelOverride is provided', () => {
    const html = renderMarkdownToHtml('```ts\nconst ok = true;\n```', '复制');
    expect(html).toContain('>复制</button>');
  });
});


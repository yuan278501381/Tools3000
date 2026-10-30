import { describe, expect, it } from 'vitest';
import { highlightCode, renderMarkdownToHtml } from './quickLookMarkup';

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
  });
});


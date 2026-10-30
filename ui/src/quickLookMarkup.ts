const HTML_ENTITIES: Record<string, string> = {
  '&': '&amp;',
  '<': '&lt;',
  '>': '&gt;',
  '"': '&quot;',
  "'": '&#39;',
};

/** Escape untrusted file contents before any syntax-highlighting markup is added. */
export function escapeHtml(value: string): string {
  return value.replace(/[&<>"']/g, (character) => HTML_ENTITIES[character]);
}

/**
 * Lightweight syntax highlighting for Quick Look.
 *
 * Tokens and all text between them are escaped independently before the static
 * span is emitted. A single lexical pass avoids matching the generated markup
 * or nesting number/keyword spans inside comments and strings.
 */
export function highlightCode(code: string): string {
  const tokenPattern = /(\/\/[^\n]*|\/\*[\s\S]*?\*\/|#[^\n]*|--[^\n]*)|("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`(?:\\.|[^`\\])*`)|\b(const|let|var|function|return|if|else|for|while|import|export|from|class|public|private|static|void|int|bool|true|false|null|undefined|struct|template|typename|using|namespace|auto|new|delete|async|await|try|catch|throw|typeof|interface|type)\b|\b([0-9]+(?:\.[0-9]+)?)\b|\b([a-zA-Z_$][a-zA-Z0-9_$]*)(?=\()/g;
  let cursor = 0;
  let highlighted = '';

  for (const match of code.matchAll(tokenPattern)) {
    const offset = match.index;
    highlighted += escapeHtml(code.slice(cursor, offset));
    const tokenClass = match[1]
      ? 'tok-comment'
      : match[2]
        ? 'tok-string'
        : match[3]
          ? 'tok-keyword'
          : match[4]
            ? 'tok-number'
            : 'tok-fn';
    highlighted += `<span class="${tokenClass}">${escapeHtml(match[0])}</span>`;
    cursor = offset + match[0].length;
  }

  return highlighted + escapeHtml(code.slice(cursor));
}

/**
 * Render the supported Markdown subset. Fenced code is removed before the
 * general HTML-escape pass so highlightCode receives raw source exactly once.
 */
export function renderMarkdownToHtml(markdown: string): string {
  if (!markdown) return '';

  const codeBlocks: string[] = [];
  const withoutCodeBlocks = markdown.replace(
    /```([a-zA-Z0-9_-]*)\r?\n([\s\S]*?)```/g,
    (_match, language: string, code: string) => {
      const blockIndex = codeBlocks.length;
      const encodedCode = encodeURIComponent(code);
      const highlighted = highlightCode(code);
      const safeLanguage = escapeHtml(language || 'text');
      codeBlocks.push(
        `<div class="ql-code-block"><div class="ql-code-header"><span class="ql-code-lang">${safeLanguage}</span><button type="button" class="ql-code-copy-btn" data-quicklook-code="${encodedCode}">Copy</button></div><pre><code>${highlighted}</code></pre></div>`,
      );
      return `\u0001TOOLS3000_CODE_BLOCK_${blockIndex}\u0002`;
    },
  );

  let html = escapeHtml(withoutCodeBlocks)
    // Headings # - ######
    .replace(/^###### (.*$)/gim, '<h6>$1</h6>')
    .replace(/^##### (.*$)/gim, '<h5>$1</h5>')
    .replace(/^#### (.*$)/gim, '<h4>$1</h4>')
    .replace(/^### (.*$)/gim, '<h3>$1</h3>')
    .replace(/^## (.*$)/gim, '<h2>$1</h2>')
    .replace(/^# (.*$)/gim, '<h1>$1</h1>')
    // Quotes and horizontal rules
    .replace(/^&gt; (.*$)/gim, '<blockquote class="ql-blockquote">$1</blockquote>')
    .replace(/^---$/gim, '<hr class="ql-hr" />')
    // Task and unordered lists
    .replace(/^- \[x\] (.*$)/gim, '<div class="ql-task-item"><input type="checkbox" checked disabled /> <span>$1</span></div>')
    .replace(/^- \[ \] (.*$)/gim, '<div class="ql-task-item"><input type="checkbox" disabled /> <span>$1</span></div>')
    .replace(/^\* (.*$)/gim, '<li>$1</li>')
    .replace(/^- (.*$)/gim, '<li>$1</li>')
    // Emphasis and inline code
    .replace(/\*\*\*(.*?)\*\*\*/g, '<strong><em>$1</em></strong>')
    .replace(/\*\*(.*?)\*\*/g, '<strong>$1</strong>')
    .replace(/\*(.*?)\*/g, '<em>$1</em>')
    .replace(/`([^`]+)`/g, '<code class="ql-inline-code">$1</code>')
    // Paragraph breaks
    .replace(/\n\n/g, '</p><p>')
    .replace(/\n/g, '<br />');

  codeBlocks.forEach((block, index) => {
    html = html.replace(`\u0001TOOLS3000_CODE_BLOCK_${index}\u0002`, () => block);
  });

  return `<div class="ql-markdown-body"><p>${html}</p></div>`;
}

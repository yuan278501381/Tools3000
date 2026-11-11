const HTML_ENTITIES: Record<string, string> = {
  '&': '&amp;',
  '<': '&lt;',
  '>': '&gt;',
  '"': '&quot;',
  "'": '&#39;',
};

/** 单行语法高亮最大字符阈值：超长压缩单行直接跳过正则分词并安全转义，杜绝主线程卡顿 */
export const MAX_HIGHLIGHT_LINE_LENGTH = 4096;

const CODE_TOKEN_PATTERN =
  /(\/\/[^\n]*|\/\*[\s\S]*?\*\/|#[^\n]*|--[^\n]*)|("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`(?:\\.|[^`\\])*`)|\b(const|let|var|function|return|if|else|for|while|import|export|from|class|public|private|static|void|int|bool|true|false|null|undefined|struct|template|typename|using|namespace|auto|new|delete|async|await|try|catch|throw|typeof|interface|type)\b|\b([0-9]+(?:\.[0-9]+)?)\b|\b([a-zA-Z_$][a-zA-Z0-9_$]*)(?=\()/g;

/** Escape untrusted file contents before any syntax-highlighting markup is added. */
export function escapeHtml(value: string): string {
  return value.replace(/[&<>"']/g, (character) => HTML_ENTITIES[character]);
}

function tokenizeNormalizedChunk(chunk: string): string {
  let cursor = 0;
  let highlighted = '';

  for (const match of chunk.matchAll(CODE_TOKEN_PATTERN)) {
    const offset = match.index;
    highlighted += escapeHtml(chunk.slice(cursor, offset));
    const tokenClass = match[1]
      ? 'tok-comment'
      : match[2]
        ? 'tok-string'
        : match[3]
          ? 'tok-keyword'
          : match[4]
            ? 'tok-number'
            : 'tok-fn';
    const rawToken = match[0];
    if (rawToken.includes('\n')) {
      highlighted += rawToken
        .split('\n')
        .map((linePart) => `<span class="${tokenClass}">${escapeHtml(linePart)}</span>`)
        .join('\n');
    } else {
      highlighted += `<span class="${tokenClass}">${escapeHtml(rawToken)}</span>`;
    }
    cursor = offset + rawToken.length;
  }

  return highlighted + escapeHtml(chunk.slice(cursor));
}

/**
 * Lightweight syntax highlighting for Quick Look.
 *
 * Tokens and all text between them are escaped independently before the static
 * span is emitted. Lines exceeding MAX_HIGHLIGHT_LINE_LENGTH bypass regex
 * tokenization and fall back directly to escapeHtml(line).
 */
export function highlightCode(code: string): string {
  if (!code) return '';
  const normalized = code.replace(/\r\n?/g, '\n');
  if (normalized.length <= MAX_HIGHLIGHT_LINE_LENGTH) {
    return tokenizeNormalizedChunk(normalized);
  }

  const lines = normalized.split('\n');
  const hasOversizedLine = lines.some((line) => line.length > MAX_HIGHLIGHT_LINE_LENGTH);
  if (!hasOversizedLine) {
    return tokenizeNormalizedChunk(normalized);
  }

  const outputSegments: string[] = [];
  let normalBuffer: string[] = [];

  const flushNormalBuffer = () => {
    if (normalBuffer.length === 0) return;
    outputSegments.push(tokenizeNormalizedChunk(normalBuffer.join('\n')));
    normalBuffer = [];
  };

  for (const line of lines) {
    if (line.length > MAX_HIGHLIGHT_LINE_LENGTH) {
      flushNormalBuffer();
      outputSegments.push(escapeHtml(line));
    } else {
      normalBuffer.push(line);
    }
  }
  flushNormalBuffer();

  return outputSegments.join('\n');
}

export function highlightSingleCodeLine(line: string): string {
  if (!line) return '';
  if (line.length > MAX_HIGHLIGHT_LINE_LENGTH) {
    return escapeHtml(line);
  }
  return tokenizeNormalizedChunk(line);
}

export function highlightCodeLines(code: string): string[] {
  if (!code) return [];
  return highlightCode(code).split('\n');
}

export function splitNormalizedLines(content: string): string[] {
  if (!content) return [];
  return content.replace(/\r\n?/g, '\n').split('\n');
}

export function countTextLines(content: string): number {
  if (!content) return 0;
  let count = 1;
  for (let i = 0; i < content.length; i += 1) {
    const ch = content.charCodeAt(i);
    if (ch === 10) {
      count += 1;
    } else if (ch === 13) {
      count += 1;
      if (i + 1 < content.length && content.charCodeAt(i + 1) === 10) {
        i += 1;
      }
    }
  }
  return count;
}

/**
 * Render the supported Markdown subset.
 * Fenced code blocks and inline backtick code spans are extracted into inert
 * placeholder tokens BEFORE emphasis regexes execute, preventing `*` pointers
 * or underscores inside code from being corrupted into `<em>`/`<strong>`.
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

  const inlineCodeSpans: string[] = [];
  const withoutInlineCode = withoutCodeBlocks.replace(
    /`([^`\r\n]+)`/g,
    (_match, rawCode: string) => {
      const spanIndex = inlineCodeSpans.length;
      inlineCodeSpans.push(`<code class="ql-inline-code">${escapeHtml(rawCode)}</code>`);
      return `\u0001TOOLS3000_INLINE_CODE_${spanIndex}\u0002`;
    },
  );

  let html = escapeHtml(withoutInlineCode)
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
    // Emphasis (safe now that inline code spans are extracted)
    .replace(/\*\*\*(.*?)\*\*\*/g, '<strong><em>$1</em></strong>')
    .replace(/\*\*(.*?)\*\*/g, '<strong>$1</strong>')
    .replace(/\*(.*?)\*/g, '<em>$1</em>')
    // Paragraph breaks
    .replace(/\n\n/g, '</p><p>')
    .replace(/\n/g, '<br />');

  inlineCodeSpans.forEach((spanHtml, index) => {
    html = html.replace(`\u0001TOOLS3000_INLINE_CODE_${index}\u0002`, () => spanHtml);
  });

  codeBlocks.forEach((block, index) => {
    html = html.replace(`\u0001TOOLS3000_CODE_BLOCK_${index}\u0002`, () => block);
  });

  return `<div class="ql-markdown-body"><p>${html}</p></div>`;
}

export const renderSimpleMarkdown = renderMarkdownToHtml;

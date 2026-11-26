/* ─────────────────────────────────────────────────────────────────────────────
 * whatsNewUtils.ts — 离线更新日志纯数据契约、递归 Markdown 解析器与分类统计工具
 * ───────────────────────────────────────────────────────────────────────────── */

export type ChangelogMode = 'upgrade' | 'manual';

export type ChangelogCategory = 'feature' | 'improvement' | 'fix';

export type ChangelogCategoryFilter = 'all' | ChangelogCategory;

export type ChangelogDensity = 'detailed' | 'compact';

export interface ChangelogAttribution {
  issueNumber?: number;
  label: string;
}

export interface ChangelogSection {
  title: string;
  category?: ChangelogCategory;
  attribution?: ChangelogAttribution;
  items: string[];
}

export interface ChangelogCategoryCounts {
  feature: number;
  improvement: number;
  fix: number;
  total: number;
}

export interface ChangelogLocaleBlock {
  summary: string;
  counts?: ChangelogCategoryCounts;
  sections: ChangelogSection[];
}

export interface ChangelogRelease {
  version: string;
  tag: string;
  kind: 'major' | 'minor' | 'patch';
  publishedAt: string;
  zh: ChangelogLocaleBlock | null;
  en: ChangelogLocaleBlock | null;
}

export interface ChangelogBundle {
  schemaVersion: number;
  totalKnownReleases: number;
  bundledCount: number;
  releases: ChangelogRelease[];
}

export type InlineAstNode =
  | { type: 'text'; value: string }
  | { type: 'code'; value: string }
  | { type: 'strong'; children: InlineAstNode[] };

export const CHANGELOG_CATEGORY_ORDER: readonly ChangelogCategory[] = [
  'feature',
  'improvement',
  'fix',
];

export function compareSemver(a: string, b: string): number {
  const parse = (raw: string): [number, number, number] => {
    const m = String(raw || '').trim().replace(/^v/i, '').match(/^(\d+)\.(\d+)\.(\d+)/);
    if (!m) return [0, 0, 0];
    return [Number(m[1]), Number(m[2]), Number(m[3])];
  };
  const pa = parse(a);
  const pb = parse(b);
  for (let i = 0; i < 3; i += 1) {
    if (pa[i] !== pb[i]) return pa[i] - pb[i];
  }
  return 0;
}

export function formatLocalDate(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return '';
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

export function formatLocalDateTime(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return '';
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

export interface ChangelogDayGroup {
  dateKey: string;
  sampleIso: string;
  releases: ChangelogRelease[];
}

/**
 * 按本地自然日（YYYY-MM-DD）把版本序列分组为时间轴区块，
 * 保持输入列表原有的版本号降序顺序，避免同日多版重复贴日期。
 */
export function groupReleasesByLocalDate(releases: ChangelogRelease[]): ChangelogDayGroup[] {
  const groups: ChangelogDayGroup[] = [];
  for (const rel of releases) {
    const dateKey = formatLocalDate(rel.publishedAt);
    const last = groups[groups.length - 1];
    if (last && last.dateKey === dateKey) {
      last.releases.push(rel);
    } else {
      groups.push({
        dateKey,
        sampleIso: rel.publishedAt,
        releases: [rel],
      });
    }
  }
  return groups;
}

export function computeRelativeDayDiff(iso: string, nowMs = Date.now()): number | null {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return null;
  const now = new Date(nowMs);
  const startOfTarget = new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
  const startOfToday = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
  return Math.max(0, Math.round((startOfToday - startOfTarget) / 86400000));
}

export function resolveReleaseContent(
  release: ChangelogRelease,
  preferChinese: boolean,
): { block: ChangelogLocaleBlock | null; isFallbackLocale: boolean } {
  if (preferChinese) {
    if (release.zh) return { block: release.zh, isFallbackLocale: false };
    return { block: release.en, isFallbackLocale: Boolean(release.en) };
  }
  if (release.en) return { block: release.en, isFallbackLocale: false };
  return { block: release.zh, isFallbackLocale: Boolean(release.zh) };
}

export function resolveSectionCategory(sec: ChangelogSection): ChangelogCategory {
  if (sec.category === 'feature' || sec.category === 'improvement' || sec.category === 'fix') {
    return sec.category;
  }
  const t = (sec.title || '').replace(/`/g, '').trim();
  if (/^(Fix|Fixed|Fixes|Bug\s*Fix|Eliminated|Resolved)\b/i.test(t)) return 'fix';
  if (/^(Added|New|Introducing|Feature)\b/i.test(t)) return 'feature';
  return 'improvement';
}

export function computeBlockCategoryCounts(block: ChangelogLocaleBlock | null): ChangelogCategoryCounts {
  if (!block) {
    return { feature: 0, improvement: 0, fix: 0, total: 0 };
  }
  if (block.counts) {
    return block.counts;
  }
  const counts: ChangelogCategoryCounts = { feature: 0, improvement: 0, fix: 0, total: 0 };
  for (const sec of block.sections) {
    const cat = resolveSectionCategory(sec);
    counts[cat] += 1;
    counts.total += 1;
  }
  return counts;
}

export function sectionMatchesQuery(sec: ChangelogSection, normalizedQuery: string): boolean {
  if (!normalizedQuery) return true;
  if (sec.title.toLowerCase().includes(normalizedQuery)) return true;
  if (sec.attribution?.label.toLowerCase().includes(normalizedQuery)) return true;
  for (const item of sec.items) {
    if (item.toLowerCase().includes(normalizedQuery)) return true;
  }
  return false;
}

export function matchesSearchQuery(
  release: ChangelogRelease,
  normalizedQuery: string,
  categoryFilter: ChangelogCategoryFilter = 'all',
  preferChinese = true,
): boolean {
  const { block } = resolveReleaseContent(release, preferChinese);
  if (categoryFilter !== 'all') {
    if (!block) return false;
    const catSections = block.sections.filter((s) => resolveSectionCategory(s) === categoryFilter);
    if (catSections.length === 0) return false;
    if (!normalizedQuery) return true;
    if (
      release.tag.toLowerCase().includes(normalizedQuery) ||
      release.version.toLowerCase().includes(normalizedQuery)
    ) {
      return true;
    }
    return catSections.some((s) => sectionMatchesQuery(s, normalizedQuery));
  }

  if (!normalizedQuery) return true;
  if (release.tag.toLowerCase().includes(normalizedQuery)) return true;
  if (release.version.toLowerCase().includes(normalizedQuery)) return true;
  for (const b of [release.zh, release.en]) {
    if (!b) continue;
    if (b.summary.toLowerCase().includes(normalizedQuery)) return true;
    for (const sec of b.sections) {
      if (sectionMatchesQuery(sec, normalizedQuery)) return true;
    }
  }
  return false;
}

/**
 * 递归解析行内 Markdown（彻底修复 `**SQL Server (`ssms.exe`)**` 中粗体嵌套反引号裸露 Bug）
 */
export function parseInlineMarkdownAst(input: string, allowStrong = true): InlineAstNode[] {
  const text = String(input || '');
  const nodes: InlineAstNode[] = [];
  let i = 0;
  let plainStart = 0;

  const flushPlain = (end: number) => {
    if (end > plainStart) {
      nodes.push({ type: 'text', value: text.slice(plainStart, end) });
    }
  };

  while (i < text.length) {
    // 1. 行内代码 `...`（内部按字面量保留，不解析星号）
    if (text[i] === '`') {
      const close = text.indexOf('`', i + 1);
      if (close > i + 1) {
        flushPlain(i);
        nodes.push({ type: 'code', value: text.slice(i + 1, close) });
        i = close + 1;
        plainStart = i;
        continue;
      }
    }

    // 2. 粗体 **...**（跳过嵌套在其中的反引号代码段寻找真正闭合 `**`，并对内部递归解析！）
    if (allowStrong && text[i] === '*' && text[i + 1] === '*') {
      let cursor = i + 2;
      let closeIdx = -1;
      while (cursor < text.length) {
        if (text[cursor] === '`') {
          const codeEnd = text.indexOf('`', cursor + 1);
          if (codeEnd > cursor) {
            cursor = codeEnd + 1;
            continue;
          }
        }
        if (text[cursor] === '*' && text[cursor + 1] === '*') {
          closeIdx = cursor;
          break;
        }
        cursor += 1;
      }
      if (closeIdx > i + 2) {
        flushPlain(i);
        const inner = text.slice(i + 2, closeIdx);
        nodes.push({
          type: 'strong',
          children: parseInlineMarkdownAst(inner, false),
        });
        i = closeIdx + 2;
        plainStart = i;
        continue;
      }
    }

    i += 1;
  }

  flushPlain(text.length);
  return nodes;
}

/**
 * 拆分形如 "**三模式自由选择**：在 `设置 -> ...` 中..." 的子项为领起短标题 + 正文说明，
 * 显著降低满屏粗体压迫感，提升中文长句扫读节奏。
 */
export function splitLeadInBullet(rawItem: string): { lead?: string; body: string } {
  const trimmed = String(rawItem || '').trim();
  // 允许粗体领起短语中包含嵌套反引号代码，但不跨越冒号之外的多余句子
  const m = trimmed.match(/^\*\*([^*]{1,42})\*\*\s*[:：]\s*(.+)$/);
  if (!m) {
    return { body: trimmed };
  }
  const lead = m[1].trim();
  const body = m[2].trim();
  if (!lead || !body) {
    return { body: trimmed };
  }
  return { lead, body };
}

/**
 * 格式化单个版本的纯文本更新日志（供一键复制分享/归档使用）
 */
export function formatReleasePlainText(
  release: ChangelogRelease,
  block: ChangelogLocaleBlock,
  labels: Record<ChangelogCategory, string>,
): string {
  const lines: string[] = [];
  const dateStr = formatLocalDate(release.publishedAt);
  lines.push(`Tools3000 ${release.tag}${dateStr ? ` (${dateStr})` : ''}`);
  if (block.summary) {
    lines.push(block.summary.replace(/\*\*/g, '').replace(/`/g, ''));
  }
  for (const cat of CHANGELOG_CATEGORY_ORDER) {
    const group = block.sections.filter((s) => resolveSectionCategory(s) === cat);
    if (group.length === 0) continue;
    lines.push('');
    lines.push(`[${labels[cat]} (${group.length})]`);
    for (const sec of group) {
      const attr = sec.attribution ? ` (${sec.attribution.label})` : '';
      const cleanTitle = sec.title.replace(/\*\*/g, '').replace(/`/g, '');
      lines.push(`- ${cleanTitle}${attr}`);
      for (const item of sec.items) {
        lines.push(`  * ${item.replace(/\*\*/g, '').replace(/`/g, '')}`);
      }
    }
  }
  return lines.join('\n');
}

/* ─────────────────────────────────────────────────────────────────────────────
 * whatsNewUtils.ts — 离线更新日志纯数据契约与无副作用工具函数
 * ───────────────────────────────────────────────────────────────────────────── */

export type ChangelogMode = 'upgrade' | 'manual';

export interface ChangelogSection {
  title: string;
  items: string[];
}

export interface ChangelogLocaleBlock {
  summary: string;
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

export function formatLocalDateTime(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return '';
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
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

export function matchesSearchQuery(release: ChangelogRelease, normalizedQuery: string): boolean {
  if (!normalizedQuery) return true;
  if (release.tag.toLowerCase().includes(normalizedQuery)) return true;
  if (release.version.toLowerCase().includes(normalizedQuery)) return true;
  for (const block of [release.zh, release.en]) {
    if (!block) continue;
    if (block.summary.toLowerCase().includes(normalizedQuery)) return true;
    for (const sec of block.sections) {
      if (sec.title.toLowerCase().includes(normalizedQuery)) return true;
      for (const item of sec.items) {
        if (item.toLowerCase().includes(normalizedQuery)) return true;
      }
    }
  }
  return false;
}

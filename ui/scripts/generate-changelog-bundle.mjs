#!/usr/bin/env node
// ─────────────────────────────────────────────────────────────────────────────
// generate-changelog-bundle.mjs — 构建期离线分页更新日志数据生成器
//
// 职责：
//   1. 扫描仓库 docs/RELEASE_NOTES_v*.md（按 SemVer 降序取最新 50 版）；
//   2. 从 Git Annotated Tag 精确读取各版发布 ISO 8601 时间戳（未打标回退 git log / mtime）；
//   3. 结构化提取中/英双语更新摘要与分组条目，剥离升级步骤与 SHA-256 校验样板；
//   4. 写入 ui/src/generated/changelogData.json，供 WhatsNewModal 动态懒加载分块消费。
// ─────────────────────────────────────────────────────────────────────────────

import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const UI_DIR = path.resolve(__dirname, '..');
const REPO_ROOT = path.resolve(UI_DIR, '..');
const DOCS_DIR = path.join(REPO_ROOT, 'docs');
const OUTPUT_DIR = path.join(UI_DIR, 'src', 'generated');
const OUTPUT_FILE = path.join(OUTPUT_DIR, 'changelogData.json');

const MAX_BUNDLED_RELEASES = 50;
const RELEASE_FILE_RE = /^RELEASE_NOTES_v(\d+)\.(\d+)\.(\d+)\.md$/;

function parseSemver(version) {
  const m = String(version).trim().replace(/^v/i, '').match(/^(\d+)\.(\d+)\.(\d+)/);
  if (!m) return [0, 0, 0];
  return [Number(m[1]), Number(m[2]), Number(m[3])];
}

function compareSemverDesc(a, b) {
  const pa = parseSemver(a);
  const pb = parseSemver(b);
  for (let i = 0; i < 3; i += 1) {
    if (pa[i] !== pb[i]) return pb[i] - pa[i];
  }
  return 0;
}

function classifySemverKind(version) {
  const [, minor, patch] = parseSemver(version);
  if (minor === 0 && patch === 0) return 'major';
  if (patch === 0) return 'minor';
  return 'patch';
}

function loadGitTagTimestamps() {
  const map = new Map();
  try {
    const out = execFileSync(
      'git',
      ['for-each-ref', '--format=%(refname:short)|%(creatordate:iso-strict)', 'refs/tags'],
      { cwd: REPO_ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] },
    );
    for (const rawLine of out.split(/\r?\n/)) {
      const line = rawLine.trim();
      if (!line) continue;
      const sep = line.indexOf('|');
      if (sep <= 0) continue;
      const tag = line.slice(0, sep).trim();
      const iso = line.slice(sep + 1).trim();
      if (tag && iso) {
        map.set(tag, iso);
      }
    }
  } catch {
    // 非 git 源码包构建时平滑回退至文件 mtime
  }
  return map;
}

function resolveReleaseTimestamp(tagName, filePath, tagMap) {
  const fromTag = tagMap.get(tagName);
  if (fromTag) return fromTag;
  try {
    const relPath = path.relative(REPO_ROOT, filePath).replace(/\\/g, '/');
    const out = execFileSync(
      'git',
      ['log', '-1', '--format=%cI', '--', relPath],
      { cwd: REPO_ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] },
    ).trim();
    if (out) return out;
  } catch {
    // 忽略 git 查询失败
  }
  return fs.statSync(filePath).mtime.toISOString();
}

function cleanInlineMarkdown(raw) {
  let text = String(raw || '').trim();
  // 将 Markdown 链接 [label](url) 收敛为易读纯文本 label
  text = text.replace(/\[([^\]]+)\]\((https?:\/\/[^)]+|#[^)]+)\)/g, '$1');
  // 去掉行首残留项目符号或序号前缀
  text = text.replace(/^\s*[-*•]\s+/, '');
  // 规整多余空白
  text = text.replace(/\s+/g, ' ').trim();
  return text;
}

function stripLeadingOrderNumber(title) {
  return cleanInlineMarkdown(title)
    .replace(/^\*{2}(.*)\*{2}$/, '$1')
    .replace(/^\d+\.\s*/, '')
    .replace(/^\*{2}(.*)\*{2}$/, '$1')
    .trim();
}

function isBoilerplateStopHeading(line) {
  const t = line.trim();
  return (
    /^#{2,3}\s*(升级方式|下载与升级|文件完整性校验|校验与安全|SHA-256|Upgrade|Checksums|Verification|Download|Installing)/i.test(t) ||
    /^\|\s*(发布文件|File|Asset)\s*\|/i.test(t)
  );
}

function isStandaloneUpgradeLine(line) {
  const t = line.trim();
  return /^\*{2}(Upgrade|升级方式)\*{2}\s*[:：]/i.test(t);
}

function parseLocaleBlock(rawBlock) {
  const lines = String(rawBlock || '').split(/\r?\n/);
  const summaryParts = [];
  const sections = [];
  let currentSection = null;
  let seenStructured = false;

  const ensureSection = (title) => {
    const cleanTitle = stripLeadingOrderNumber(title);
    const existing = sections.find((s) => s.title === cleanTitle);
    if (existing) {
      currentSection = existing;
      return existing;
    }
    const created = { title: cleanTitle, items: [] };
    sections.push(created);
    currentSection = created;
    return created;
  };

  for (let i = 0; i < lines.length; i += 1) {
    const raw = lines[i];
    const trimmed = raw.trim();
    if (!trimmed) continue;
    if (trimmed === '---' || trimmed === '***') continue;
    if (isBoilerplateStopHeading(trimmed)) break;
    if (isStandaloneUpgradeLine(trimmed)) continue;
    // 跳过一级标题与语言锚点导航栏
    if (/^#\s+/.test(trimmed)) continue;
    if (/^\*\*\[English\]/i.test(trimmed)) continue;
    if (/^>\s*如遇\s*Windows Defender SmartScreen/i.test(trimmed)) break;

    // 跳过独立代码块（通常是 Get-FileHash 示例）
    if (trimmed.startsWith('```')) {
      i += 1;
      while (i < lines.length && !lines[i].trim().startsWith('```')) {
        i += 1;
      }
      continue;
    }

    // 三级标题：明确的新分组（例如 ### 1. 修复 Bilibili...）
    const h3 = trimmed.match(/^###\s+(.+)$/);
    if (h3) {
      seenStructured = true;
      ensureSection(h3[1]);
      continue;
    }

    // 顶层编号行：如 "1. **Title**"（若后续有缩进子列表则作分组标题，否则作为单条变更项）
    const numbered = raw.match(/^(\d+)\.\s+(.+)$/);
    if (numbered) {
      seenStructured = true;
      // 向前探测下一非空行是否为缩进子项
      let nextNonEmpty = '';
      for (let j = i + 1; j < lines.length; j += 1) {
        if (lines[j].trim()) {
          nextNonEmpty = lines[j];
          break;
        }
      }
      const hasSubBullets = /^\s{2,}[-*]\s+/.test(nextNonEmpty);
      const body = numbered[2].trim();
      if (hasSubBullets) {
        ensureSection(body);
      } else {
        if (!currentSection) ensureSection('');
        const item = cleanInlineMarkdown(body);
        if (item) currentSection.items.push(item);
      }
      continue;
    }

    // 无序列表项（含缩进二级列表项）
    const bullet = raw.match(/^(\s*)[-*]\s+(.+)$/);
    if (bullet) {
      seenStructured = true;
      if (!currentSection) ensureSection('');
      const item = cleanInlineMarkdown(bullet[2]);
      if (item) currentSection.items.push(item);
      continue;
    }

    // 二级标题在块内部出现时（例如某些老版本多个 ## 小节），当作分组标题
    const h2 = trimmed.match(/^##\s+(.+)$/);
    if (h2) {
      seenStructured = true;
      ensureSection(h2[1]);
      continue;
    }

    // 普通段落：位于结构化列表前的作为版本概览摘要
    if (!seenStructured) {
      const cleaned = cleanInlineMarkdown(trimmed);
      if (cleaned) summaryParts.push(cleaned);
    }
  }

  const filteredSections = sections.filter((s) => s.items.length > 0 || s.title.length > 0);
  const summary = summaryParts.join(' ').trim();
  if (!summary && filteredSections.length === 0) {
    return null;
  }
  return {
    summary,
    sections: filteredSections,
  };
}

function parseReleaseNotesMarkdown(markdown) {
  const normalized = String(markdown || '').replace(/\r\n/g, '\n');

  // 1. 先按尾部校验/升级样板截断，保证不会把 SHA-256 哈希表混进任何语言分区
  const allLines = normalized.split('\n');
  const trimmedLines = [];
  for (const line of allLines) {
    if (/^###?\s*(文件完整性校验|校验与安全|SHA-256|Checksums|Verification)/i.test(line.trim())) {
      break;
    }
    trimmedLines.push(line);
  }
  const coreText = trimmedLines.join('\n');

  // 2. 检测双语分区：英文区 "## English Release Highlights" 与中文区任意含中文的二级标题（排除升级方式/校验）
  const enHeadingRe = /^##\s+English(?:\s+Release\s+Highlights)?\s*$/im;
  const zhHeadingRe = /^##\s+(?!升级方式|下载与升级|文件完整性校验|校验与安全)[^\n]*[\u4e00-\u9fff][^\n]*$/m;

  const enMatch = enHeadingRe.exec(coreText);
  const zhMatch = zhHeadingRe.exec(coreText);

  let enRaw = '';
  let zhRaw = '';

  if (enMatch && zhMatch) {
    if (enMatch.index < zhMatch.index) {
      enRaw = coreText.slice(enMatch.index + enMatch[0].length, zhMatch.index);
      zhRaw = coreText.slice(zhMatch.index + zhMatch[0].length);
    } else {
      zhRaw = coreText.slice(zhMatch.index + zhMatch[0].length, enMatch.index);
      enRaw = coreText.slice(enMatch.index + enMatch[0].length);
    }
  } else if (enMatch) {
    enRaw = coreText.slice(enMatch.index + enMatch[0].length);
  } else if (zhMatch) {
    // 早期单中文版：一级标题下的导语通常在 ## 之前，需一并纳入 summary
    const intro = coreText.slice(0, zhMatch.index);
    const body = coreText.slice(zhMatch.index + zhMatch[0].length);
    zhRaw = `${intro}\n${body}`;
  } else {
    zhRaw = coreText;
  }

  const zh = zhRaw.trim() ? parseLocaleBlock(zhRaw) : null;
  const en = enRaw.trim() ? parseLocaleBlock(enRaw) : null;

  return { zh, en };
}

function main() {
  if (!fs.existsSync(DOCS_DIR)) {
    throw new Error(`Docs directory not found: ${DOCS_DIR}`);
  }

  const tagMap = loadGitTagTimestamps();
  const candidates = [];

  for (const name of fs.readdirSync(DOCS_DIR)) {
    const match = RELEASE_FILE_RE.exec(name);
    if (!match) continue;
    const version = `${match[1]}.${match[2]}.${match[3]}`;
    candidates.push({
      version,
      tag: `v${version}`,
      filePath: path.join(DOCS_DIR, name),
    });
  }

  candidates.sort((a, b) => compareSemverDesc(a.version, b.version));
  const selected = candidates.slice(0, MAX_BUNDLED_RELEASES);

  const releases = selected.map((entry) => {
    const rawMd = fs.readFileSync(entry.filePath, 'utf8');
    const parsed = parseReleaseNotesMarkdown(rawMd);
    const publishedAt = resolveReleaseTimestamp(entry.tag, entry.filePath, tagMap);
    return {
      version: entry.version,
      tag: entry.tag,
      kind: classifySemverKind(entry.version),
      publishedAt,
      zh: parsed.zh,
      en: parsed.en,
    };
  });

  const payload = {
    schemaVersion: 1,
    totalKnownReleases: candidates.length,
    bundledCount: releases.length,
    releases,
  };

  fs.mkdirSync(OUTPUT_DIR, { recursive: true });
  const serialized = `${JSON.stringify(payload, null, 2)}\n`;

  // 若内容完全未变化则跳过写入，避免触发 Vite/ESLint 不必要重建
  const prev = fs.existsSync(OUTPUT_FILE) ? fs.readFileSync(OUTPUT_FILE, 'utf8') : null;
  if (prev !== serialized) {
    fs.writeFileSync(OUTPUT_FILE, serialized, 'utf8');
  }

  const sizeKb = (Buffer.byteLength(serialized, 'utf8') / 1024).toFixed(1);
  const latest = releases[0]?.tag ?? 'none';
  console.log(`[OK] Changelog bundle ready: ${releases.length} releases (latest ${latest}, ${sizeKb} KB) -> ${path.relative(REPO_ROOT, OUTPUT_FILE)}`);
}

main();

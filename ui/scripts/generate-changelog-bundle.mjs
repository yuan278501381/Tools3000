#!/usr/bin/env node
// ─────────────────────────────────────────────────────────────────────────────
// generate-changelog-bundle.mjs — 构建期世界级离线更新日志结构化数据生成器 (Schema v2)
//
// 职责：
//   1. 扫描仓库 docs/RELEASE_NOTES_v*.md（按 SemVer 降序取最新 50 版）；
//   2. 从 Git Annotated Tag 精确读取各版发布 ISO 8601 时间戳（未打标回退 git log / mtime）；
//   3. 结构化提取中/英双语更新摘要与条目，剔除升级样板与 0 项空壳速查节；
//   4. 自动将聚合修复容器（如「重要问题修复」）拆分为独立条目，精准判定每条语义分类
//      (feature / improvement / fix)，提取 GitHub Issue 与社区致谢徽章，并汇总各项统计计数；
//   5. 写入 ui/src/generated/changelogData.json，供 WhatsNewModal 懒加载分块消费。
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

function isEmptyQuickRefHeading(title) {
  return /(速查|操作路径|快捷键速查|文件位置速查|quick\s*reference|where\s+to\s+find)/i.test(title);
}

/**
 * 提取标题末尾括号内的 GitHub Issue 编号与社区致谢信息，返回干净标题与结构化归因对象。
 * 兼容：
 *   - "双击 Ctrl 电影级交互平滑放大 (#34, @cainiaoNO1)"
 *   - "修复运行后 Windows 任务栏右键控制菜单失效（感谢 LINUX DO @kidcode）"
 *   - "文件对话框（`#30`, `@baowuji`；感谢 Linux.do `@sounder`）"
 */
function extractTitleAndAttribution(rawTitle) {
  const trimmed = String(rawTitle || '').trim();
  const suffixRe = /\s*[（(]([^()（）]*(?:#\d+|@\S+|感谢|thanks?\b)[^()（）]*)[）)]\s*$/i;
  const match = suffixRe.exec(trimmed);
  if (!match) {
    return { title: trimmed, attribution: undefined };
  }

  const innerRaw = match[1].replace(/`/g, '').replace(/\s+/g, ' ').trim();
  const cleanTitle = trimmed.slice(0, match.index).trim();
  if (!cleanTitle) {
    return { title: trimmed, attribution: undefined };
  }

  const issueMatch = innerRaw.match(/#(\d+)/);
  const issueNumber = issueMatch ? Number(issueMatch[1]) : undefined;
  // 精炼展示标签：保留可读的 Issue 与社区鸣谢，替换冗余分隔
  const label = innerRaw
    .replace(/,\s*/g, ' · ')
    .replace(/；\s*/g, ' · ')
    .trim();

  return {
    title: cleanTitle,
    attribution: {
      ...(issueNumber !== undefined ? { issueNumber } : {}),
      label,
    },
  };
}

/**
 * 识别「重要问题修复 / Bug Fixes」等纯分类容器节，将其内部以 "**标题**：说明" 组织的
 * 多条独立修复自动拆解为各自独立的变更条目，确保计数精确无合并失真。
 */
function isAggregateContainerHeading(title) {
  const t = String(title || '').trim();
  if (/^(重要问题修复|问题修复|缺陷修复|关键修复|bug\s*fixes?|key\s*fixes?|fixes?)$/i.test(t)) {
    return 'fix';
  }
  if (/^(新增功能|全新功能|新特性|new\s*features?|features?)$/i.test(t)) {
    return 'feature';
  }
  if (/^(优化与改进|体验优化|性能与优化|improvements?|enhancements?)$/i.test(t)) {
    return 'improvement';
  }
  return null;
}

function splitAggregateBullet(rawItem) {
  const text = String(rawItem || '').trim();
  // 匹配 "**标题** (可选归因)：正文" 或 "**标题**：正文"
  const m = text.match(/^\*\*([^*]+)\*\*\s*([（(][^()（）]+[）)])?\s*[:：]\s*(.+)$/);
  if (!m) return null;
  const baseTitle = m[1].trim();
  const suffix = m[2] ? ` ${m[2].trim()}` : '';
  const body = m[3].trim();
  if (!baseTitle || !body) return null;
  return {
    rawTitle: `${baseTitle}${suffix}`,
    body,
  };
}

/**
 * MECE 三元语义分类器：精准判定每条变更属于
 * - 'feature'     新增功能（全新模块、新模式、新开关、新入口、核心能力上线）
 * - 'fix'         问题修复（Bug 修复、黑屏/闪退/卡死/点穿/失效/报错/泄漏根治）
 * - 'improvement' 优化改进（交互体验打磨、性能/内存优化、文案润色、架构升级与日志治理）
 */
function classifySectionCategory(title, items) {
  const t = String(title || '').replace(/`/g, '').trim();
  const firstItem = String(items?.[0] || '').replace(/`/g, '').trim();

  // 1. 历史 1.0.x ~ 1.2.x 固定三大板块显式映射
  if (/^(业务与功能体验进化|Business\s*&\s*Feature)/i.test(t)) return 'feature';
  if (/^(稳定性与底层演进|质量门禁与稳定性保障|Stability)/i.test(t)) return 'fix';
  if (/^(架构演进与技术重构|Architecture)/i.test(t)) return 'improvement';

  // 2. 强特征前缀优先：以「全新 / 新增」开头或模块冒号后紧跟「新增 / 全新」明确为新功能
  if (
    /^(全新|新增|推出|引入|Added\b|New\b|Introducing\b|Offline\s+"What's\s+New")/i.test(t) ||
    /[:：]\s*(新增|全新|推出|支持\s*\d+|自由切换|自定义多路径)/i.test(t)
  ) {
    return 'feature';
  }

  // 3. 明确问题修复前缀与强缺陷修复词（排除“报错条”这类被识别文本对象）
  if (
    /^(彻底)?(修复|解决|消除|清除|根治)|^(Fix|Fixed|Fixes|Eliminated|Resolved|Cleaned\s+Up|Clean\s+.*Exit)\b/i.test(t) ||
    /(问题修复|故障修复|闪退|黑屏|白屏|卡死|无反应|点穿|透明空框|失效|报错(?!条)|双图重复|异常菜单|滞留|未按需清零|零句柄触碰|防僵死|防误触|零起始光点|bug\s*fix|crash|black\s*flash|not\s+opening|empty\s+translucent|visibility|unresponsive|freeze|leak)/i.test(t)
  ) {
    return 'fix';
  }

  // 4. 明确新增功能信号（新模块、新模式、独立开关、专业软件自动避让/让位、工作流能力上线）
  if (
    /(电影级交互平滑放大|平滑放大|按前台应用自动让位|自动避让|独立开关|自定义多路径排除|端到端加密|云端与同步盘备份|便携版原地热更新|文本扩展器|文件占用解锁|微信级截图原位翻译|持久化剪贴板|MCP\s*Server|Picasa|一键直达|自由切换|支持\s*8\s*方向|自由拖拽|固定选区|边界自动换行|手势图形匹配阈值|硬件亮度控制|演播室录屏|离线文字识别跨代升级|高精度离线文字识别|空格快速预览|全局鼠标手势|触发角|Smooth\s+Zoom|Auto-Yield|Auto-Bypass|Toggle|Text\s+Expander|File\s+Locksmith|In-Place\s+Translation\s+\(Keyless|Persistent\s+Clipboard|Picasa|Hot\s+Corner|Multi-Path\s+Exclusion|Encrypted\s+Cloud\s+Backup)/i.test(t) ||
    /^(文件搜索.*万能启动器|打开\s*\/\s*保存文件弹窗也能)/i.test(t)
  ) {
    return 'feature';
  }

  // 5. 若标题较中性，借助首条正文前缀辅助校验
  if (/^(彻底)?(修复|解决|消除)|^(Fixed|Resolved|Eliminated)\b/i.test(firstItem)) {
    return 'fix';
  }
  if (/^(新增|全新|推出|Added\b|Introduced\b|Launched\b)/i.test(firstItem)) {
    return 'feature';
  }

  // 6. 默认归为优化改进（交互对齐、性能/内存优化、文案润色、模型升级、日志治理等）
  return 'improvement';
}

function summarizeCounts(sections) {
  const counts = { feature: 0, improvement: 0, fix: 0, total: 0 };
  for (const sec of sections) {
    if (sec.category === 'feature') counts.feature += 1;
    else if (sec.category === 'fix') counts.fix += 1;
    else counts.improvement += 1;
    counts.total += 1;
  }
  return counts;
}

function finalizeSections(rawSections) {
  const result = [];
  for (const sec of rawSections) {
    // 剔除无任何内容的空壳速查标题
    if (sec.items.length === 0) {
      if (!sec.title || isEmptyQuickRefHeading(sec.title)) continue;
      continue;
    }

    const containerCat = isAggregateContainerHeading(sec.title);
    if (containerCat && sec.items.length > 0) {
      const parsedChildren = sec.items.map((it) => splitAggregateBullet(it));
      if (parsedChildren.every((c) => c !== null)) {
        for (const child of parsedChildren) {
          const { title, attribution } = extractTitleAndAttribution(child.rawTitle);
          result.push({
            title,
            category: containerCat,
            ...(attribution ? { attribution } : {}),
            items: [child.body],
          });
        }
        continue;
      }
    }

    const { title, attribution } = extractTitleAndAttribution(sec.title);
    const category = containerCat ?? classifySectionCategory(title, sec.items);
    result.push({
      title,
      category,
      ...(attribution ? { attribution } : {}),
      items: sec.items,
    });
  }
  return result;
}

function parseLocaleBlock(rawBlock) {
  const lines = String(rawBlock || '').split(/\r?\n/);
  const summaryParts = [];
  const rawSections = [];
  let currentSection = null;
  let seenStructured = false;

  const ensureSection = (title) => {
    const cleanTitle = stripLeadingOrderNumber(title);
    const existing = rawSections.find((s) => s.title === cleanTitle);
    if (existing) {
      currentSection = existing;
      return existing;
    }
    const created = { title: cleanTitle, items: [] };
    rawSections.push(created);
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
    if (/^#\s+/.test(trimmed)) continue;
    if (/^\*\*\[English\]/i.test(trimmed)) continue;
    if (/^>\s*如遇\s*Windows Defender SmartScreen/i.test(trimmed)) break;

    if (trimmed.startsWith('```')) {
      i += 1;
      while (i < lines.length && !lines[i].trim().startsWith('```')) {
        i += 1;
      }
      continue;
    }

    const h3 = trimmed.match(/^###\s+(.+)$/);
    if (h3) {
      seenStructured = true;
      ensureSection(h3[1]);
      continue;
    }

    const numbered = raw.match(/^(\d+)\.\s+(.+)$/);
    if (numbered) {
      seenStructured = true;
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

    const bullet = raw.match(/^(\s*)[-*]\s+(.+)$/);
    if (bullet) {
      seenStructured = true;
      if (!currentSection) ensureSection('');
      const item = cleanInlineMarkdown(bullet[2]);
      if (item) currentSection.items.push(item);
      continue;
    }

    const h2 = trimmed.match(/^##\s+(.+)$/);
    if (h2) {
      seenStructured = true;
      ensureSection(h2[1]);
      continue;
    }

    if (!seenStructured) {
      const cleaned = cleanInlineMarkdown(trimmed);
      if (cleaned) summaryParts.push(cleaned);
    }
  }

  const sections = finalizeSections(rawSections);
  const summary = summaryParts.join(' ').trim();
  if (!summary && sections.length === 0) {
    return null;
  }
  return {
    summary,
    counts: summarizeCounts(sections),
    sections,
  };
}

function parseReleaseNotesMarkdown(markdown) {
  const normalized = String(markdown || '').replace(/\r\n/g, '\n');

  const allLines = normalized.split('\n');
  const trimmedLines = [];
  for (const line of allLines) {
    if (/^###?\s*(文件完整性校验|校验与安全|SHA-256|Checksums|Verification)/i.test(line.trim())) {
      break;
    }
    trimmedLines.push(line);
  }
  const coreText = trimmedLines.join('\n');

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
    schemaVersion: 2,
    totalKnownReleases: candidates.length,
    bundledCount: releases.length,
    releases,
  };

  fs.mkdirSync(OUTPUT_DIR, { recursive: true });
  const serializedLf = `${JSON.stringify(payload, null, 2)}\n`;

  const prev = fs.existsSync(OUTPUT_FILE) ? fs.readFileSync(OUTPUT_FILE, 'utf8') : null;
  const prevNormalized = prev !== null ? prev.replace(/\r\n/g, '\n') : null;
  if (prevNormalized !== serializedLf) {
    const useCrlf = prev !== null ? prev.includes('\r\n') : process.platform === 'win32';
    const serializedOut = useCrlf ? serializedLf.replace(/\n/g, '\r\n') : serializedLf;
    fs.writeFileSync(OUTPUT_FILE, serializedOut, 'utf8');
  }

  const sizeKb = (Buffer.byteLength(serializedLf, 'utf8') / 1024).toFixed(1);
  const latest = releases[0]?.tag ?? 'none';
  console.log(`[OK] Changelog bundle v2 ready: ${releases.length} releases (latest ${latest}, ${sizeKb} KB) -> ${path.relative(REPO_ROOT, OUTPUT_FILE)}`);
}

main();

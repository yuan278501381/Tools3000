import type { FileFolderPriority, SearchResult, SortDirection, SortField } from './searchTypes';

export function formatBytes(bytes: number): string {
  if (!bytes || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  const index = Math.floor(Math.log(bytes) / Math.log(1024));
  return `${(bytes / Math.pow(1024, index)).toFixed(index === 0 ? 0 : 1)} ${units[index]}`;
}

export function extractParentDirectoryPath(fullPath: string): string {
  if (!fullPath) return '';
  const clean = fullPath.replace(/[/\\]+$/, '');
  const lastSlash = Math.max(clean.lastIndexOf('/'), clean.lastIndexOf('\\'));
  if (lastSlash <= 0) return '';
  const parentPath = clean.slice(0, lastSlash);
  if (/^[a-zA-Z]:$/.test(parentPath)) {
    return `${parentPath}\\`;
  }
  return parentPath;
}

export function extractParentFolder(fullPath: string): string {
  if (!fullPath) return '';
  const clean = fullPath.replace(/[/\\]+$/, '');
  const lastSlash = Math.max(clean.lastIndexOf('/'), clean.lastIndexOf('\\'));
  if (lastSlash <= 0) return '';
  const parentPath = clean.slice(0, lastSlash);
  const secondLastSlash = Math.max(parentPath.lastIndexOf('/'), parentPath.lastIndexOf('\\'));
  const immediateParent = secondLastSlash >= 0 ? parentPath.slice(secondLastSlash + 1) : parentPath;

  const normalizedParentPath = `${parentPath.replace(/\//g, '\\').toLowerCase()}\\`;
  if (
    normalizedParentPath.includes('\\xwechat_files\\') ||
    normalizedParentPath.includes('\\xwechat\\') ||
    normalizedParentPath.includes('\\wechat files\\')
  ) {
    return immediateParent ? `WeChat\u00b7${immediateParent}` : 'WeChat';
  }
  if (normalizedParentPath.includes('\\wxwork\\') || normalizedParentPath.includes('\\wecom\\')) {
    return immediateParent ? `WeCom\u00b7${immediateParent}` : 'WeCom';
  }
  if (normalizedParentPath.includes('\\tencent files\\') || normalizedParentPath.includes('\\nt_qq\\')) {
    return immediateParent ? `QQ\u00b7${immediateParent}` : 'QQ';
  }
  return immediateParent;
}

export function getFileTypeBadge(name: string, isDirectory: boolean): { label: string; colorClass: string } {
  if (isDirectory) return { label: 'DIR', colorClass: 'badge-folder' };
  const dot = name.lastIndexOf('.');
  if (dot === -1) return { label: 'FILE', colorClass: 'badge-generic' };

  const extension = name.slice(dot + 1).toUpperCase();
  const lowerExtension = extension.toLowerCase();
  if (['C', 'CPP', 'H', 'HPP', 'RS', 'GO'].includes(extension)) return { label: `.${lowerExtension}`, colorClass: 'badge-code-c' };
  if (['JS', 'TS', 'TSX', 'JSX', 'VUE', 'HTML', 'CSS', 'SCSS'].includes(extension)) return { label: `.${lowerExtension}`, colorClass: 'badge-code-js' };
  if (['PY', 'JAVA', 'CS', 'SH', 'BAT', 'PS1', 'LUA', 'SQL'].includes(extension)) return { label: `.${lowerExtension}`, colorClass: 'badge-code-other' };
  if (['JSON', 'YAML', 'YML', 'TOML', 'XML', 'INI', 'CONF'].includes(extension)) return { label: `.${lowerExtension}`, colorClass: 'badge-config' };
  if (['PNG', 'JPG', 'JPEG', 'WEBP', 'SVG', 'GIF', 'ICO', 'BMP'].includes(extension)) return { label: extension, colorClass: 'badge-img' };
  if (['ZIP', '7Z', 'RAR', 'TAR', 'GZ', 'BZ2'].includes(extension)) return { label: extension, colorClass: 'badge-archive' };
  if (['PDF', 'DOC', 'DOCX', 'XLS', 'XLSX', 'PPT', 'PPTX', 'TXT', 'MD'].includes(extension)) return { label: extension, colorClass: 'badge-doc' };
  if (['MP4', 'MKV', 'AVI', 'MOV', 'MP3', 'WAV', 'FLAC'].includes(extension)) return { label: extension, colorClass: 'badge-media' };
  if (['EXE', 'DLL', 'SYS', 'SO', 'BIN'].includes(extension)) return { label: extension, colorClass: 'badge-bin' };
  return { label: `.${lowerExtension.slice(0, 5)}`, colorClass: 'badge-generic' };
}

export function formatFileSize(bytes?: number, isDirectory?: boolean): string {
  if (isDirectory) return '';
  if (bytes === undefined || bytes === null || bytes === 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let size = bytes;
  let unitIndex = 0;
  while (size >= 1024 && unitIndex < units.length - 1) {
    size /= 1024;
    unitIndex += 1;
  }
  return `${size.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
}

export function formatWindowsTime(fileTime?: number | string): string {
  if (!fileTime) return '';
  const numericFileTime = typeof fileTime === 'string' ? parseInt(fileTime, 10) : fileTime;
  if (!numericFileTime) return '';
  const milliseconds = Math.floor((numericFileTime - 116444736000000000) / 10000);
  if (milliseconds <= 0) return '';
  const date = new Date(milliseconds);
  if (Number.isNaN(date.getTime())) return '';

  const pad = (value: number) => (value < 10 ? `0${value}` : String(value));
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

/**
 * 搜索结果质量分层枚举（数值越小优先级越高，同一层级内严格按用户选定的修改时间/大小/名称排序）：
 * - Tier 0 (NormalUserFile): 本地正常目录的原始文件与工作区文件（永远排最前）
 * - Tier 1 (UniqueChatFile): 微信/QQ/企微/钉钉/飞书等聊天记录中独有的最新文件（每个主干文件名仅保留最新 1 份）
 * - Tier 2 (RedundantChatCopy): 聊天记录中发给多个客户产生的历史重复副本，或本地已有原始文件的聊天副本
 * - Tier 3 (AppOrSystemData): AppData / ProgramData / Program Files 等应用内部数据目录文件
 * - Tier 4 (ShortcutFile): .lnk / .url 快捷方式文件
 * - Tier 5 (TempOrCache): Temp / Tmp / Cache / Recent 临时目录文件与 .tmp / ~$ 临时锁文件（永远沉底）
 */
export const SearchResultQualityTier = {
  NormalUserFile: 0,
  UniqueChatFile: 1,
  RedundantChatCopy: 2,
  AppOrSystemData: 3,
  ShortcutFile: 4,
  TempOrCache: 5,
} as const;

export type SearchResultQualityTier =
  (typeof SearchResultQualityTier)[keyof typeof SearchResultQualityTier];

const TEMP_OR_CACHE_DIR_SEGMENTS = [
  '\\temp\\',
  '\\tmp\\',
  '\\inetcache\\',
  '\\webcache\\',
  '\\cache\\',
  '\\caches\\',
  '\\code cache\\',
  '\\gpucache\\',
  '\\shadercache\\',
  '\\dawncache\\',
  '\\recent\\',
  '\\automaticdestinations\\',
  '\\customdestinations\\',
  '\\crashpad\\',
  '\\crashdumps\\',
  '\\prefetch\\',
  '\\winsxs\\',
  '\\installer\\',
  '\\package cache\\',
  '\\$recycle.bin\\',
  '\\system volume information\\',
  '\\node_modules\\',
  '\\.git\\',
  '\\__pycache__\\',
  '\\npm-cache\\',
  '\\.gradle\\',
  '\\.vs\\',
  '\\cefcache\\',
  '\\coverage_report\\',
];

const CHAT_HISTORY_DIR_SEGMENTS = [
  '\\xwechat_files\\',
  '\\xwechat\\',
  '\\wechat files\\',
  '\\tencent files\\',
  '\\nt_qq\\',
  '\\wxwork\\',
  '\\wecom\\',
  '\\dingtalk\\',
  '\\feishu\\',
  '\\lark\\',
  '\\telegram desktop\\',
  '\\filestorage\\file\\',
  '\\filestorage\\msgattach\\',
  '\\filestorage\\cache\\',
  '\\msg\\file\\',
  '\\msg\\attach\\',
  '\\file_recv\\',
  '\\filerecv\\',
];

const APP_OR_SYSTEM_DIR_SEGMENTS = [
  '\\appdata\\local\\',
  '\\appdata\\roaming\\',
  '\\appdata\\locallow\\',
  '\\appdata\\',
  '\\programdata\\',
  '\\program files\\',
  '\\program files (x86)\\',
  '\\windows\\',
];

/**
 * 提取文件名的规范化主干键（剔除副本后缀、括号备注与末尾版本号如 V10001/V10002/V01），
 * 用于识别“本地原始文件已升级版本，且曾通过微信/QQ多次发给不同客户”的同源副本关系。
 */
export function extractCanonicalFileKey(fileName: string): string {
  const lower = (fileName || '').trim().toLowerCase();
  if (!lower) return '';

  const dotPos = lower.lastIndexOf('.');
  const ext = dotPos > 0 ? lower.slice(dotPos) : '';
  let stem = dotPos > 0 ? lower.slice(0, dotPos) : lower;

  // 1. 移除常见的系统重复副本后缀：- 副本, (1), (2), （1）
  stem = stem.replace(/(?:\s*-\s*\u526f\u672c|\s*_\u526f\u672c|\s*-\s*copy|\s*_copy|\s*\(\d+\)|\s*\uff08\d+\uff09)+$/gi, '').trim();

  // 2. 移除末尾括号补充说明（如 "V10002(注意：不能清空当前账号...)"），前提是主干长度充足
  const parenMatch = stem.match(/^(.{4,}?)\s*(?:\([^()]*\)|（[^（）]*）)+$/);
  if (parenMatch && parenMatch[1].trim().length >= 4) {
    stem = parenMatch[1].trim();
  }

  // 3. 移除末尾版本号与构建后缀（如 V10002, V10001_MESID2, _v01, -v2.1），前提是前缀主干 >= 4 个字符
  const versionMatch = stem.match(/^(.{4,}?)[_-]?v\d+(?:[._-][a-z0-9]+)*$/i);
  if (versionMatch && versionMatch[1].trim().length >= 4) {
    stem = versionMatch[1].trim();
  }

  return `${stem}${ext}`;
}

export function classifyResultBaseTier(result: SearchResult): SearchResultQualityTier {
  const normalizedPath = (result.path || '').replace(/\//g, '\\').toLowerCase();
  const lastSlash = normalizedPath.lastIndexOf('\\');
  // 仅针对目录路径段匹配，绝不因文件名本身包含 "TEMP" 等词而误判为临时目录
  const dirPath = lastSlash >= 0 ? normalizedPath.slice(0, lastSlash + 1) : '';
  const fileName = (result.name || (lastSlash >= 0 ? normalizedPath.slice(lastSlash + 1) : normalizedPath)).toLowerCase();

  // 1. 临时文件、锁文件与缓存/临时目录判定
  if (!result.isDirectory) {
    if (
      fileName.startsWith('~$') ||
      fileName.endsWith('.tmp') ||
      fileName.endsWith('.temp') ||
      fileName.endsWith('.bak') ||
      fileName.endsWith('.crdownload') ||
      fileName.endsWith('.part') ||
      fileName.endsWith('.partial')
    ) {
      return SearchResultQualityTier.TempOrCache;
    }
  }

  for (const seg of TEMP_OR_CACHE_DIR_SEGMENTS) {
    if (dirPath.includes(seg)) {
      return SearchResultQualityTier.TempOrCache;
    }
  }

  // 2. 快捷方式文件 (.lnk / .url) 独立分层，避免混入正常文档顶部
  if (
    !result.isDirectory &&
    (fileName.endsWith('.lnk') || fileName.endsWith('.url') || fileName.endsWith('.desklink'))
  ) {
    return SearchResultQualityTier.ShortcutFile;
  }

  // 3. 微信 / 企微 / QQ / 钉钉 / 飞书等聊天记录传输文件目录
  for (const seg of CHAT_HISTORY_DIR_SEGMENTS) {
    if (dirPath.includes(seg)) {
      return SearchResultQualityTier.UniqueChatFile;
    }
  }

  // 4. AppData / ProgramData / Program Files / Windows 等系统与应用内部数据目录
  for (const seg of APP_OR_SYSTEM_DIR_SEGMENTS) {
    if (dirPath.includes(seg)) {
      return SearchResultQualityTier.AppOrSystemData;
    }
  }

  // 5. 用户正常本地工作目录与原始文件
  return SearchResultQualityTier.NormalUserFile;
}

export function computeEffectiveResultTiers(results: SearchResult[]): Map<SearchResult, SearchResultQualityTier> {
  const tierMap = new Map<SearchResult, SearchResultQualityTier>();
  const normalCanonicalKeys = new Set<string>();
  const normalExactNames = new Set<string>();
  const chatGroups = new Map<string, SearchResult[]>();

  for (const item of results) {
    const baseTier = classifyResultBaseTier(item);
    tierMap.set(item, baseTier);

    if (!item.isDirectory) {
      const lowerName = (item.name || '').trim().toLowerCase();
      const canonicalKey = extractCanonicalFileKey(item.name);
      if (baseTier === SearchResultQualityTier.NormalUserFile) {
        if (lowerName) normalExactNames.add(lowerName);
        if (canonicalKey) normalCanonicalKeys.add(canonicalKey);
      } else if (baseTier === SearchResultQualityTier.UniqueChatFile) {
        const groupKey = canonicalKey || lowerName;
        if (groupKey) {
          const list = chatGroups.get(groupKey);
          if (list) list.push(item);
          else chatGroups.set(groupKey, [item]);
        }
      }
    }
  }

  // 对聊天记录中的文件进行同源去重与降权：
  // 1) 若本地正常目录 (Tier 0) 已存在同名或同主干的原始文件，则聊天记录中的副本全部降为 Tier 2 (RedundantChatCopy)
  // 2) 若仅存在于聊天记录中但发给多个客户产生了多份副本，仅保留修改时间最新的一份在 Tier 1，其余历史副本降为 Tier 2
  for (const [groupKey, groupItems] of chatGroups.entries()) {
    const hasLocalOriginal =
      normalCanonicalKeys.has(groupKey) ||
      groupItems.some((item) => normalExactNames.has((item.name || '').trim().toLowerCase()));

    if (hasLocalOriginal) {
      for (const item of groupItems) {
        tierMap.set(item, SearchResultQualityTier.RedundantChatCopy);
      }
      continue;
    }

    if (groupItems.length > 1) {
      let newestItem = groupItems[0];
      let newestTime = Number(newestItem.lastWriteTime) || 0;
      for (let i = 1; i < groupItems.length; i += 1) {
        const candidate = groupItems[i];
        const candidateTime = Number(candidate.lastWriteTime) || 0;
        if (candidateTime > newestTime) {
          newestTime = candidateTime;
          newestItem = candidate;
        }
      }
      for (const item of groupItems) {
        if (item !== newestItem) {
          tierMap.set(item, SearchResultQualityTier.RedundantChatCopy);
        }
      }
    }
  }

  return tierMap;
}

function getPathShallowScore(fullPath: string): number {
  if (!fullPath) return 0;
  const clean = fullPath.replace(/\//g, '\\');
  let score = 0;
  if (clean.length >= 2 && clean[1] === ':' && clean[0].toUpperCase() !== 'C') {
    score += 100;
  }
  const slashCount = (clean.match(/\\/g) || []).length;
  score += Math.max(0, 20 - slashCount);
  return score;
}

export function getSortedResults(
  results: SearchResult[],
  sortField: SortField,
  sortDirection: SortDirection,
  folderPriority: FileFolderPriority | boolean,
  groupByType: boolean,
  smartPathPriority: boolean = true,
): SearchResult[] {
  if (results.length <= 1) return results;
  const priorityMode: FileFolderPriority =
    typeof folderPriority === 'boolean'
      ? (folderPriority ? 'foldersFirst' : 'mixed')
      : folderPriority;

  const effectiveTiers = smartPathPriority ? computeEffectiveResultTiers(results) : null;

  return [...results].sort((left, right) => {
    if (left.isDirectory !== right.isDirectory) {
      if (priorityMode === 'filesFirst') {
        return left.isDirectory ? 1 : -1;
      }
      if (priorityMode === 'foldersFirst') {
        return left.isDirectory ? -1 : 1;
      }
    }

    if (effectiveTiers) {
      const leftTier = effectiveTiers.get(left) ?? SearchResultQualityTier.NormalUserFile;
      const rightTier = effectiveTiers.get(right) ?? SearchResultQualityTier.NormalUserFile;
      if (leftTier !== rightTier) {
        return leftTier - rightTier;
      }
    }

    if (groupByType && !left.isDirectory && !right.isDirectory) {
      const leftExtension = left.name.includes('.') ? left.name.slice(left.name.lastIndexOf('.')).toLowerCase() : '';
      const rightExtension = right.name.includes('.') ? right.name.slice(right.name.lastIndexOf('.')).toLowerCase() : '';
      const extensionComparison = leftExtension.localeCompare(rightExtension);
      if (extensionComparison !== 0) return extensionComparison;
    }

    let comparison = 0;
    if (sortField === 'relevance') comparison = (Number(left.frecencyScore) || 0) - (Number(right.frecencyScore) || 0);
    else if (sortField === 'modified') comparison = (Number(left.lastWriteTime) || 0) - (Number(right.lastWriteTime) || 0);
    else if (sortField === 'created') comparison = (Number(left.creationTime) || 0) - (Number(right.creationTime) || 0);
    else if (sortField === 'size') comparison = (left.isDirectory ? -1 : Number(left.size) || 0) - (right.isDirectory ? -1 : Number(right.size) || 0);
    else if (sortField === 'name') comparison = left.name.localeCompare(right.name, 'zh-CN', { numeric: true, sensitivity: 'base' });

    if (comparison !== 0) return sortDirection === 'asc' ? comparison : -comparison;

    if (smartPathPriority) {
      const shallowDiff = getPathShallowScore(right.path) - getPathShallowScore(left.path);
      if (shallowDiff !== 0) return shallowDiff;
    }

    return left.name.localeCompare(right.name, 'zh-CN', { numeric: true, sensitivity: 'base' });
  });
}

const FILE_SCOPE_FILTER_PREFIXES = [
  'ext:',
  'file:',
  'folder:',
  'dir:',
  'path:',
  'parent:',
  'p:',
  'size:',
  'dm:',
  'date:',
  'datemodified:',
  'modified:',
  'dc:',
  'datecreated:',
  'created:',
  'exact:',
  'wfn:',
  'wholefilename:',
  'regex:',
  'r:',
  'case:',
  'nocase:',
  'pinyin:',
  'py:',
  'sort:',
  'orderby:',
] as const;

/**
 * 引号感知分词：使配对双引号内部的空格与竖线（如 `content:"hello world"`、`path:"C:\Program Files"`）
 * 保持在同一个词元内不被劈开，同时将未加引号的逻辑竖线 `|` 拆分为独立词元。
 */
export function tokenizeQueryQuoteAware(query: string): string[] {
  const tokens: string[] = [];
  const n = query.length;
  let i = 0;
  while (i < n) {
    while (i < n && /\s/.test(query[i])) i += 1;
    if (i >= n) break;

    if (query[i] === '|') {
      tokens.push('|');
      i += 1;
      continue;
    }

    const start = i;
    let inQuotes = false;
    while (i < n) {
      const ch = query[i];
      if (ch === '"') {
        inQuotes = !inQuotes;
        i += 1;
        continue;
      }
      if (!inQuotes && (/\s/.test(ch) || ch === '|')) {
        break;
      }
      i += 1;
    }
    if (i > start) {
      tokens.push(query.slice(start, i));
    }
  }
  return tokens;
}

export function stripOuterPairedQuotes(value: string): string {
  const trimmed = value.trim();
  if (trimmed.length >= 2 && trimmed.startsWith('"') && trimmed.endsWith('"')) {
    return trimmed.slice(1, -1);
  }
  return trimmed;
}

interface ContentPrefixMatch {
  isContent: boolean;
  negated: boolean;
  allowsSpaceValue: boolean;
  rawValue: string;
}

function matchContentPrefixToken(token: string): ContentPrefixMatch {
  const negated = token.startsWith('!');
  const body = negated ? token.slice(1) : token;
  const lower = body.toLowerCase();

  if (lower.startsWith('content:')) {
    return {
      isContent: true,
      negated,
      allowsSpaceValue: true,
      rawValue: body.slice('content:'.length),
    };
  }
  if (body.startsWith('\u5185\u5bb9:')) {
    return {
      isContent: true,
      negated,
      allowsSpaceValue: true,
      rawValue: body.slice('\u5185\u5bb9:'.length),
    };
  }
  if (lower.startsWith('c:') && body.length > 2 && body[2] !== '\\' && body[2] !== '/') {
    return {
      isContent: true,
      negated,
      allowsSpaceValue: false,
      rawValue: body.slice(2),
    };
  }
  return {
    isContent: false,
    negated,
    allowsSpaceValue: false,
    rawValue: '',
  };
}

/**
 * 判断一个词元是否属于文件范围限定器或逻辑修饰符（在切换全文搜索时应当原位保留，不冠以 `content:`）。
 */
export function isFileScopeFilterToken(token: string): boolean {
  if (!token) return false;
  if (token === '|') return true;
  if (token.startsWith('!')) return true;

  const isQuoted = token.startsWith('"') && token.endsWith('"') && token.length >= 2;
  if (!isQuoted && (token.includes('*') || token.includes('?'))) {
    return true;
  }

  // 盘符或路径词元，如 c:, D:, D:\Work, C:/repo, \src\
  if (/^[a-zA-Z]:(?:[\\/].*)?$/.test(token)) {
    return true;
  }
  if (!isQuoted && token.includes('\\')) {
    return true;
  }

  const lower = token.toLowerCase();
  if (FILE_SCOPE_FILTER_PREFIXES.some((prefix) => lower.startsWith(prefix))) {
    return true;
  }
  return matchContentPrefixToken(token).isContent;
}

/**
 * 识别查询串任意词元位置是否显式包含 `content:`、`内容:` 或非盘符路径简写 `c:xxx`。
 * 完美支持 `ext:docx content:合同`、`D:\Work 内容:预算`、`size:>10kb c:甲方` 等组合语法。
 */
export function hasContentSyntaxInQuery(query: string): boolean {
  const tokens = tokenizeQueryQuoteAware(query);
  for (const token of tokens) {
    if (matchContentPrefixToken(token).isContent) {
      return true;
    }
  }
  return false;
}

/**
 * 提取查询串中所有显式声明的全文检索词（支持 `content:"hello world"` 与 `content: 合同` 冒号后空格）。
 */
export function extractExplicitContentTerms(query: string): string[] {
  const tokens = tokenizeQueryQuoteAware(query);
  const terms: string[] = [];
  for (let i = 0; i < tokens.length; i += 1) {
    const match = matchContentPrefixToken(tokens[i]);
    if (!match.isContent) continue;

    let value = match.rawValue;
    if (!value && match.allowsSpaceValue && i + 1 < tokens.length) {
      const nextToken = tokens[i + 1];
      if (!isFileScopeFilterToken(nextToken)) {
        value = nextToken;
        i += 1;
      }
    }
    const unquoted = stripOuterPairedQuotes(value);
    if (unquoted.length > 0) {
      terms.push(unquoted);
    }
  }
  return terms;
}

/**
 * 判断查询串是否包含了显式内容搜索语法但尚未输入有效正文关键词（如 `content:` 或 `ext:docx content:`）。
 */
export function hasEmptyContentKeywordSyntax(query: string): boolean {
  const lowered = query.trim().toLowerCase();
  if (lowered === 'content:' || lowered === '\u5185\u5bb9:' || lowered === 'c:') {
    return true;
  }
  if (!hasContentSyntaxInQuery(query)) {
    return false;
  }
  return extractExplicitContentTerms(query).length === 0;
}

/**
 * 统一判断当前搜索上下文是否处于全文内容检索状态（涵盖分类胶囊、搜索模式 `content`/`both` 及任意位置显式语法）。
 */
export function isEffectiveContentSearch(input: {
  query: string;
  activeCategory?: string;
  searchMode?: string;
}): boolean {
  if (input.activeCategory === 'content') return true;
  if (input.searchMode === 'content' || input.searchMode === 'both') return true;
  return hasContentSyntaxInQuery(input.query);
}

/**
 * 智能将当前查询切换为显式全文内容检索：
 * - 若已含任意位置 `content:` / `内容:` / `c:xxx`，原样返回，绝不重复拼接损坏语法；
 * - 否则原位保留所有 `ext:`、盘符路径、大小/时间、通配符与排除项过滤词，仅对自由关键词冠以 `content:`；
 * - 若查询仅含文件过滤词而无自由词（如 `ext:docx D:\Work`），则在末尾追加 ` content:` 等待输入。
 */
export function buildSmartContentSearchQuery(query: string): string {
  const trimmed = query.trim();
  if (!trimmed) return 'content:';
  if (hasContentSyntaxInQuery(trimmed)) {
    return trimmed;
  }

  const tokens = tokenizeQueryQuoteAware(trimmed);
  let convertedFreeTokens = 0;
  const mapped = tokens.map((token) => {
    if (isFileScopeFilterToken(token)) {
      return token;
    }
    convertedFreeTokens += 1;
    return `content:${token}`;
  });

  const joined = mapped.join(' ');
  if (convertedFreeTokens === 0) {
    return `${joined} content:`;
  }
  return joined;
}

/**
 * 从查询串中无损剥离任意位置的显式 `content:` / `内容:` / `c:` 前缀，保留自由关键词与全部文件范围过滤条件。
 */
export function stripContentSyntaxFromQuery(query: string): string {
  const trimmed = query.trim();
  if (!trimmed) return '';

  const tokens = tokenizeQueryQuoteAware(trimmed);
  const output: string[] = [];
  for (let i = 0; i < tokens.length; i += 1) {
    const token = tokens[i];
    const match = matchContentPrefixToken(token);
    if (!match.isContent || match.negated) {
      output.push(token);
      continue;
    }

    if (!match.rawValue && match.allowsSpaceValue) {
      if (i + 1 < tokens.length && !isFileScopeFilterToken(tokens[i + 1])) {
        output.push(tokens[i + 1]);
        i += 1;
      }
      continue;
    }

    if (match.rawValue) {
      output.push(match.rawValue);
    }
  }
  return output.join(' ');
}

/**
 * 从原始查询文本中提取用于行高亮的核心关键词列表，剔除语法前缀与特殊修饰符。
 */
export function parseQueryKeywords(query: string): string[] {
  const trimmed = query.trim();
  if (!trimmed) return [];
  const tokens = trimmed.split(/[\s|]+/).filter(Boolean);
  const keywords: string[] = [];
  for (const token of tokens) {
    let clean = token.replace(/^!/, '');
    const colonPos = clean.indexOf(':');
    if (colonPos !== -1 && colonPos < 8) {
      clean = clean.substring(colonPos + 1);
    }
    clean = clean.replace(/[*?"]/g, '').trim();
    if (clean.length > 0 && !keywords.includes(clean)) {
      keywords.push(clean);
    }
  }
  return keywords;
}

import {
  FileArchive,
  FileAudio,
  FileCode,
  FileImage,
  FileSpreadsheet,
  FileText,
  FileVideo,
  Folder,
  Sparkles,
} from 'lucide-react';
import type {
  ColumnSetting,
  FormatCategory,
  SyntaxExampleItem,
  WindowPreset,
} from './searchTypes';

export const DEFAULT_COLUMNS: ColumnSetting[] = [
  { id: 'name', labelKey: 'search.colName', visible: true, flex: 32 },
  { id: 'ext', labelKey: 'search.colExt', visible: true },
  { id: 'parent', labelKey: 'search.colParent', visible: true },
  { id: 'path', labelKey: 'search.colPath', visible: true, flex: 42 },
  { id: 'size', labelKey: 'search.colSize', visible: true },
  { id: 'modified', labelKey: 'search.colModified', visible: true },
  { id: 'created', labelKey: 'search.colCreated', visible: false },
  { id: 'snippets', labelKey: 'search.colSnippets', visible: true },
];

export const WINDOW_PRESETS: WindowPreset[] = [
  { id: 'standard', label: 'Standard (800×600)', width: 800, height: 600 },
  { id: 'wide', label: 'Widescreen (1000×650)', width: 1000, height: 650 },
  { id: 'large', label: 'Large (1200×750)', width: 1200, height: 750 },
  { id: 'extra', label: 'Ultra-Wide (1400×800)', width: 1400, height: 800 },
];

export const CATEGORY_DEFS = [
  { id: 'all', labelKey: 'search.catAll', defaultLabel: 'All', prefix: '', icon: Sparkles },
  { id: 'content', labelKey: 'search.catContent', defaultLabel: 'Content', prefix: 'content:', icon: FileText },
  { id: 'doc', labelKey: 'search.catDocs', defaultLabel: 'Documents', prefix: 'ext:doc;docx;xls;xlsx;ppt;pptx;pdf;txt;md ', icon: FileSpreadsheet },
  { id: 'image', labelKey: 'search.catImages', defaultLabel: 'Images', prefix: 'ext:jpg;jpeg;png;webp;gif;bmp;svg ', icon: FileImage },
  { id: 'video', labelKey: 'search.catVideos', defaultLabel: 'Videos', prefix: 'ext:mp4;mkv;avi;mov;wmv;flv;webm ', icon: FileVideo },
  { id: 'audio', labelKey: 'search.catAudio', defaultLabel: 'Audio', prefix: 'ext:mp3;wav;flac;aac;m4a;ogg ', icon: FileAudio },
  { id: 'archive', labelKey: 'search.catArchives', defaultLabel: 'Archives', prefix: 'ext:zip;rar;7z;tar;gz ', icon: FileArchive },
  { id: 'code', labelKey: 'search.catCode', defaultLabel: 'Code', prefix: 'ext:cpp;h;ts;tsx;js;py;rs;go;java;lua;json ', icon: FileCode },
  { id: 'folder', labelKey: 'search.catFolders', defaultLabel: 'Folders', prefix: 'folder: ', icon: Folder },
] as const;

export const SYNTAX_CATEGORIES = [
  { id: 'all', labelKey: 'search.syntaxCatAll', defaultLabel: 'All Syntax' },
  { id: 'content', labelKey: 'search.syntaxCatContent', defaultLabel: 'Content Search' },
  { id: 'path', labelKey: 'search.syntaxCatPath', defaultLabel: 'Path & Drive' },
  { id: 'ext', labelKey: 'search.syntaxCatExt', defaultLabel: 'Wildcards & Exts' },
  { id: 'logic', labelKey: 'search.syntaxCatLogic', defaultLabel: 'Logic & Regex' },
];

export const SYNTAX_EXAMPLES: SyntaxExampleItem[] = [
  { category: 'content', syntax: 'content:SELECT', descKey: 'search.syntaxExContent1', defaultDesc: 'Full-text search inside code, documents, designs, and CAD', highlight: true },
  { category: 'content', syntax: 'ext:docx;sql content:order', descKey: 'search.syntaxExContent2', defaultDesc: 'Penetrate search in files of specified extension types' },
  { category: 'content', syntax: 'c:\\ content:report', descKey: 'search.syntaxExContent3', defaultDesc: 'Limit full-text search strictly under directory path' },
  { category: 'path', syntax: 'c:\\', descKey: 'search.syntaxExPath1', defaultDesc: 'Limit search scope strictly within C: root and directory' },
  { category: 'path', syntax: 'c:\\repo\\ *.ts', descKey: 'search.syntaxExPath2', defaultDesc: 'Search specific file types within given subdirectory' },
  { category: 'path', syntax: 'path:windows', descKey: 'search.syntaxExPath3', defaultDesc: 'Match keyword in complete file path' },
  { category: 'path', syntax: 'folder: project', descKey: 'search.syntaxExPath4', defaultDesc: 'Match folders and directories only, exclude files' },
  { category: 'ext', syntax: '*.txt', descKey: 'search.syntaxExExt1', defaultDesc: 'Wildcard match all files with txt extension' },
  { category: 'ext', syntax: 'ext:jpg;png;webp', descKey: 'search.syntaxExExt2', defaultDesc: 'Filter multiple extensions at once (semicolon separated)' },
  { category: 'ext', syntax: 'file: *.pdf', descKey: 'search.syntaxExExt3', defaultDesc: 'Search files only, exclude folders' },
  { category: 'ext', syntax: '"Program Files"', descKey: 'search.syntaxExExt4', defaultDesc: 'Double quotes phrase exact match (handles paths with spaces)' },
  { category: 'logic', syntax: 'report !draft', descKey: 'search.syntaxExLogic1', defaultDesc: 'Contains report but excludes items containing draft (NOT)' },
  { category: 'logic', syntax: 'ext:jpg | ext:png', descKey: 'search.syntaxExLogic2', defaultDesc: 'Logical OR condition combined search' },
  { category: 'logic', syntax: 'regex:^app_\\d+\\.log$', descKey: 'search.syntaxExLogic3', defaultDesc: 'Regex search (starts with app_digits)' },
  { category: 'logic', syntax: 'case:Tools3000', descKey: 'search.syntaxExLogic4', defaultDesc: 'Case-sensitive exact match' },
  { category: 'logic', syntax: 'pinyin:wx', descKey: 'search.syntaxExLogic5', defaultDesc: 'Explicit Pinyin initials / full Pinyin search (e.g. wx for WeChat)' },
  { category: 'logic', syntax: 'size:>10mb', descKey: 'search.syntaxExSize', defaultDesc: 'Filter by file size (supports kb / mb / gb and > < >= <=)' },
  { category: 'logic', syntax: 'dm:today', descKey: 'search.syntaxExDate', defaultDesc: 'Filter by modified date (today / yesterday / thisweek / 2026-09)' },
];

export const CONTENT_FORMAT_CATEGORIES: FormatCategory[] = [
  {
    id: 'code',
    nameKey: 'search.catCode',
    iconName: 'code',
    extensions: [
      'cpp', 'c', 'h', 'hpp', 'hxx', 'inl', 'cs',
      'rs', 'go', 'zig', 'v', 'nim', 'odin', 'd',
      'java', 'kt', 'kts', 'scala', 'groovy', 'dart', 'swift', 'm', 'mm',
      'ts', 'tsx', 'js', 'jsx', 'mjs', 'cjs', 'vue', 'svelte', 'astro',
      'html', 'htm', 'css', 'scss', 'sass', 'less',
      'py', 'pyw', 'rb', 'php', 'pl', 'pm', 'lua',
      'sh', 'bash', 'zsh', 'ps1', 'psm1', 'bat', 'cmd', 'vbs', 'ahk', 'au3',
      'sql', 'prc', 'fnc', 'trg', 'pks', 'pkb', 'pls', 'ch', 'pld',
      'asm', 's', 'glsl', 'hlsl', 'vert', 'frag', 'geom', 'comp', 'shader', 'wgsl',
    ],
  },
  {
    id: 'docs',
    nameKey: 'search.catDocs',
    iconName: 'doc',
    extensions: [
      'doc', 'dot', 'docx', 'docm', 'dotx', 'wps', 'wpt', 'odt', 'rtf',
      'xls', 'xlt', 'xlsx', 'xlsm', 'xltx', 'et', 'ett', 'ods', 'csv', 'tsv',
      'ppt', 'pot', 'pptx', 'pptm', 'potx', 'dps', 'dpt', 'odp',
      'pdf', 'txt', 'md', 'markdown', 'log', 'tex', 'bib', 'rst', 'adoc', 'epub',
      'eml', 'msg', 'chm', 'one', 'diff', 'patch', 'org',
    ],
  },
  {
    id: 'config',
    nameKey: 'search.catConfig',
    iconName: 'config',
    extensions: [
      'json', 'jsonc', 'json5', 'yaml', 'yml', 'toml', 'xml', 'xaml',
      'ini', 'cfg', 'conf', 'config', 'env', 'reg', 'properties',
      'proto', 'graphql', 'gql', 'thrift', 'prisma', 'schema', 'avsc', 'dbml',
      'lock', 'plist', 'prefs',
    ],
  },
  {
    id: 'design',
    nameKey: 'search.catDesign',
    iconName: 'design',
    extensions: ['dxf', 'dwg', 'psd', 'ai', 'cdr', 'xmind', 'svg'],
  },
];

export const IMAGE_EXTENSIONS = /\.(png|jpe?g|webp|bmp|gif|svg|ico)$/i;

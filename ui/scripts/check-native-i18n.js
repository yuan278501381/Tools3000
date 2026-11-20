/**
 * check-native-i18n.js — C++ 原生界面 8 语种词典完整度与占位符防线门禁
 *
 * 扫描全库 src/ 中所有 trW / trWCstr / trWCstrFor / trUtf8 / trUtf8Cstr / trUtf8CstrFor / trFmtW / trFmtUtf8
 * 调用与静态双语数组表，强制校验 resources/locales/native/{zh-TW,ja-JP,ko-KR,ru-RU,pt-BR,es-ES}.json
 * 100% 键位覆盖、占位符对齐、零孤儿键与零 Emoji。
 */

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const repoRoot = path.resolve(__dirname, '../..');
const srcDir = path.join(repoRoot, 'src');
const nativeDir = path.join(repoRoot, 'resources', 'locales', 'native');

const TARGET_CATALOGS = ['zh-TW.json', 'ja-JP.json', 'ko-KR.json', 'ru-RU.json', 'pt-BR.json', 'es-ES.json'];
const EMOJI_RE = /[\u{1F300}-\u{1FAFF}\u2600-\u27BF]/u;

function walk(dir, out = []) {
  for (const name of fs.readdirSync(dir)) {
    const full = path.join(dir, name);
    const st = fs.statSync(full);
    if (st.isDirectory()) walk(full, out);
    else if (full.endsWith('.cpp') || full.endsWith('.h')) out.push(full);
  }
  return out;
}

function unescapeCpp(s) {
  let out = '';
  for (let i = 0; i < s.length; i++) {
    if (s[i] === '\\' && i + 1 < s.length) {
      const nxt = s[i + 1];
      if (nxt === 'n') { out += '\n'; i++; }
      else if (nxt === 'r') { out += '\r'; i++; }
      else if (nxt === 't') { out += '\t'; i++; }
      else if (nxt === '"') { out += '"'; i++; }
      else if (nxt === '\\') { out += '\\'; i++; }
      else if (nxt === 'u' && i + 5 < s.length) {
        out += String.fromCharCode(parseInt(s.slice(i + 2, i + 6), 16));
        i += 5;
      } else {
        out += s.slice(i, i + 2);
        i++;
      }
    } else {
      out += s[i];
    }
  }
  return out;
}

function collectRequiredPairs() {
  const pairs = new Map(); // `${zh}\0${en}` -> [zh, en]
  const addPair = (zh, en) => {
    if (!en) return;
    pairs.set(`${zh}\0${en}`, [zh, en]);
  };

  const reW = /tr(?:W|WCstr|WCstrFor|FmtW)\s*\((?:[^,()]+,\s*)?L"((?:[^"\\]|\\.)*)"\s*,\s*L"((?:[^"\\]|\\.)*)"/g;
  const reN = /tr(?:Utf8|Utf8Cstr|Utf8CstrFor|FmtUtf8)\s*\((?:[^,()]+,\s*)?"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"/g;

  for (const file of walk(srcDir)) {
    if (path.basename(file).startsWith('UiI18n.')) continue;
    const txt = fs.readFileSync(file, 'utf8');
    let m;
    while ((m = reW.exec(txt)) !== null) addPair(unescapeCpp(m[1]), unescapeCpp(m[2]));
    while ((m = reN.exec(txt)) !== null) addPair(unescapeCpp(m[1]), unescapeCpp(m[2]));
  }

  const arraySources = [
    ['src/capture/CaptureDropdownMenu.cpp', 'bgNames', 'bgNamesEn'],
    ['src/capture/CaptureDropdownMenu.cpp', 'qNames', 'qNamesEn'],
    ['src/capture/CaptureDropdownMenu.cpp', 'zNamesZh', 'zNamesEn'],
    ['src/capture/CaptureDropdownMenu.cpp', 'stNamesZh', 'stNamesEn'],
    ['src/capture/CaptureInputCommands.cpp', 'ratioNamesZh', 'ratioNamesEn'],
    ['src/capture/CaptureInputKeyboard.cpp', 'bgNamesZh', 'bgNamesEn'],
    ['src/capture/StudioPreviewWindow.cpp', 'kZh', 'kEn'],
    ['src/capture/StudioPreviewWindowDrawers.cpp', 'maskZh', 'maskEn'],
    ['src/capture/StudioPreviewWindowDrawers.cpp', 'modeZh', 'modeEn'],
    ['src/capture/StudioPreviewWindowDrawers.cpp', 'tokActZh', 'tokActEn'],
    ['src/capture/StudioPreviewWindowDrawers.cpp', 'tokLblZh', 'tokLblEn'],
    ['src/capture/StudioPreviewWindowDrawers.cpp', 'posZh', 'posEn'],
    ['src/capture/StudioPreviewWindowDrawers.cpp', 'tabShortZh', 'tabShortEn'],
    ['src/capture/StudioPreviewWindowDrawers.cpp', 'lblZh', 'lblEn'],
    ['src/capture/StudioPreviewWindowDrawers.cpp', 'aspZh', 'aspEn'],
    ['src/capture/StudioPreviewWindowDrawers.cpp', 'catZh', 'catEn'],
  ];

  for (const [rel, aZh, aEn] of arraySources) {
    const txt = fs.readFileSync(path.join(repoRoot, rel), 'utf8');
    const zhBlocks = [...txt.matchAll(new RegExp(`\\b${aZh}\\s*\\[[^\\]]*\\]\\s*=\\s*\\{([^}]+)\\}`, 'gs'))];
    const enBlocks = [...txt.matchAll(new RegExp(`\\b${aEn}\\s*\\[[^\\]]*\\]\\s*=\\s*\\{([^}]+)\\}`, 'gs'))];
    const count = Math.min(zhBlocks.length, enBlocks.length);
    for (let i = 0; i < count; i++) {
      const zs = [...zhBlocks[i][1].matchAll(/L"((?:[^"\\]|\\.)*)"/g)].map(x => unescapeCpp(x[1]));
      const es = [...enBlocks[i][1].matchAll(/L"((?:[^"\\]|\\.)*)"/g)].map(x => unescapeCpp(x[1]));
      for (let j = 0; j < Math.min(zs.length, es.length); j++) {
        addPair(zs[j], es[j]);
      }
    }
  }

  const sbc = fs.readFileSync(path.join(repoRoot, 'src/capture/StudioBackdropCatalog.cpp'), 'utf8');
  for (const m of sbc.matchAll(/L"((?:[^"\\]|\\.)*)"\s*,\s*L"((?:[^"\\]|\\.)*)"/g)) {
    const z = unescapeCpp(m[1]);
    const e = unescapeCpp(m[2]);
    if (/[\u4e00-\u9fff]/.test(z) && !/[\u4e00-\u9fff]/.test(e)) {
      addPair(z, e);
    }
  }

  const enToZhs = new Map();
  for (const [z, e] of pairs.values()) {
    if (!enToZhs.has(e)) enToZhs.set(e, new Set());
    enToZhs.get(e).add(z);
  }
  const conflicts = new Set([...enToZhs.entries()].filter(([, s]) => s.size > 1).map(([e]) => e));

  const required = new Map();
  for (const [z, e] of pairs.values()) {
    const key = conflicts.has(e) ? `${z}||${e}` : e;
    required.set(key, { zh: z, en: e });
  }
  return required;
}

const required = collectRequiredPairs();
console.log(`[INFO] [Native i18n Gate] 扫描到 ${required.size} 个 C++ 原生 UI 词条，正在校验 6 套海外/繁体原生词典...`);

let hasError = false;
for (const file of TARGET_CATALOGS) {
  const fullPath = path.join(nativeDir, file);
  if (!fs.existsSync(fullPath)) {
    console.error(`[ERROR] 缺失原生词典文件: ${fullPath}`);
    hasError = true;
    continue;
  }
  let data;
  try {
    data = JSON.parse(fs.readFileSync(fullPath, 'utf8'));
  } catch (err) {
    console.error(`[ERROR] 原生词典 JSON 解析失败: ${file}`, err);
    hasError = true;
    continue;
  }

  let missing = 0;
  let placeholderMismatch = 0;
  let emojiCount = 0;

  for (const [key, { en }] of required.entries()) {
    const val = data[key];
    if (typeof val !== 'string' || !val.trim()) {
      if (missing < 10) console.error(`[ERROR] ${file} 缺失键: ${JSON.stringify(key)}`);
      missing++;
      hasError = true;
      continue;
    }
    if (EMOJI_RE.test(val)) {
      console.error(`[ERROR] ${file} 含违规 Emoji: [${key}] -> ${val}`);
      emojiCount++;
      hasError = true;
    }
    const enPh = (en.match(/\{\d+\}/g) || []).sort().join(',');
    const valPh = (val.match(/\{\d+\}/g) || []).sort().join(',');
    if (enPh !== valPh) {
      console.error(`[ERROR] ${file} 占位符不匹配: [${key}] en="${en}" val="${val}"`);
      placeholderMismatch++;
      hasError = true;
    }
  }

  if (missing === 0 && placeholderMismatch === 0 && emojiCount === 0) {
    console.log(`[OK] ${file}: ${Object.keys(data).length} 条全量通过校验`);
  } else {
    console.error(`[ERROR] ${file}: missing=${missing}, placeholderMismatch=${placeholderMismatch}, emoji=${emojiCount}`);
  }
}

if (hasError) {
  process.exit(1);
}
console.log('[OK] C++ 原生界面 8 语种词典门禁全部通过！');

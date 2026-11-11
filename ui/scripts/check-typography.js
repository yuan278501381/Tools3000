/**
 * check-typography.js — Tools3000 世界级排版体系与字号底线门禁
 *
 * 检查规则:
 * 1. 严禁使用硬编码孤立字体栈 (包含 index.css 普通规则、font: 简写属性与 TSX 内联 fontFamily，必须统一引用 var(--font-mono) 或 var(--font-sans) 或 inherit)
 * 2. 严禁出现低于 0.83rem (11.8px) 的微小字号 (含 font-size 与 font: 简写)
 * 3. 严禁出现低于 500 Medium 的细字重 (含 font-weight 与 font: 简写)
 */

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const srcDir = path.resolve(__dirname, '../src');

function walk(dir, filter, list = []) {
  const files = fs.readdirSync(dir);
  for (const f of files) {
    const full = path.join(dir, f);
    const stat = fs.statSync(full);
    if (stat.isDirectory()) {
      if (!['node_modules', 'dist', '.git'].includes(f)) {
        walk(full, filter, list);
      }
    } else if (filter(full)) {
      list.push(full);
    }
  }
  return list;
}

const cssFiles = walk(srcDir, f => f.endsWith('.css'));
const tsxFiles = walk(srcDir, f => f.endsWith('.tsx') || f.endsWith('.ts'));

const errors = [];

const validFontFamilies = new Set([
  'var(--font-sans)',
  'var(--font-mono)',
  'var(--font-mono, monospace)',
  'inherit',
]);

function normalizeCssValue(raw) {
  return raw.replace(/\s*!important\s*$/i, '').trim();
}

// 1. 检查所有 CSS 文件中的 font-family 与 font: 简写 (含 index.css，仅豁免 @font-face 块与 --font-* 令牌定义行)
for (const file of cssFiles) {
  const relFile = path.relative(srcDir, file);
  const content = fs.readFileSync(file, 'utf8');
  const lines = content.split(/\r?\n/);
  let inFontFaceBlock = false;
  let fontFaceBraceDepth = 0;

  lines.forEach((rawLine, idx) => {
    const lineNoComment = rawLine.replace(/\/\*.*?\*\//g, '');
    const trimmed = lineNoComment.trim();

    if (!inFontFaceBlock && /^@font-face\b/i.test(trimmed)) {
      inFontFaceBlock = true;
      fontFaceBraceDepth = 0;
    }
    if (inFontFaceBlock) {
      for (const ch of lineNoComment) {
        if (ch === '{') fontFaceBraceDepth++;
        else if (ch === '}') fontFaceBraceDepth--;
      }
      if (fontFaceBraceDepth <= 0 && lineNoComment.includes('}')) {
        inFontFaceBlock = false;
      }
      return;
    }

    // 跳过根字体令牌声明行 (--font-sans / --font-mono)
    if (/^--font-(?:sans|mono)\s*:/i.test(trimmed)) {
      return;
    }

    // 1a. 检查 font-family 声明
    const ffMatch = lineNoComment.match(/(?:^|[;{]\s*)font-family\s*:\s*([^;{}]+)/i);
    if (ffMatch) {
      const val = normalizeCssValue(ffMatch[1]);
      if (!validFontFamilies.has(val)) {
        errors.push(`[${relFile}:${idx + 1}] 违规使用了孤立 font-family: "${ffMatch[1].trim()}". 请统一使用 var(--font-sans) 或 var(--font-mono) 或 inherit.`);
      }
    }

    // 1b. 检查 CSS font: 简写声明 (防止通过 font: 500 14px/1.5 "Segoe UI" 绕过 font-family / 字号 / 字重门禁)
    const shorthandMatch = lineNoComment.match(/(?:^|[;{]\s*)font\s*:\s*([^;{}]+)/i);
    if (shorthandMatch) {
      const rawShorthand = shorthandMatch[1].trim();
      const val = normalizeCssValue(rawShorthand);
      if (val.toLowerCase() !== 'inherit') {
        if (!val.includes('var(--font-sans)') && !val.includes('var(--font-mono)')) {
          errors.push(`[${relFile}:${idx + 1}] 违规在 font 简写中硬编码字体栈: "${rawShorthand}". 请改用 var(--font-sans) 或 var(--font-mono).`);
        }
        const shorthandPx = val.match(/(?:^|\s)([0-9]+(?:\.[0-9]+)?)px(?=[/\s]|$)/i);
        if (shorthandPx && parseFloat(shorthandPx[1]) < 11.8) {
          errors.push(`[${relFile}:${idx + 1}] font 简写中违规使用了低于 11.8px 的过小字号: "${rawShorthand}".`);
        }
        const shorthandRem = val.match(/(?:^|\s)([0-9]+(?:\.[0-9]+)?)rem(?=[/\s]|$)/i);
        if (shorthandRem && parseFloat(shorthandRem[1]) < 0.83) {
          errors.push(`[${relFile}:${idx + 1}] font 简写中违规使用了低于 0.83rem 的过小字号: "${rawShorthand}".`);
        }
        const shorthandWeight = val.match(/^(?:(?:italic|oblique|normal)\s+)*([1-9]00|lighter)\b/i);
        if (shorthandWeight) {
          const wToken = shorthandWeight[1].toLowerCase();
          if (wToken === 'lighter' || parseInt(wToken, 10) < 500) {
            errors.push(`[${relFile}:${idx + 1}] font 简写中违规使用了低于 500 的细字重: "${rawShorthand}".`);
          }
        }
      }
    }
  });
}

// 2. 检查 TSX / TS 内联样式中的硬编码 fontFamily
for (const file of tsxFiles) {
  if (file.endsWith('.test.ts') || file.endsWith('.test.tsx') || file.endsWith('.spec.tsx')) continue;
  const relFile = path.relative(srcDir, file);
  const content = fs.readFileSync(file, 'utf8');
  const lines = content.split(/\r?\n/);
  lines.forEach((rawLine, idx) => {
    const trimmed = rawLine.trim();
    if (trimmed.startsWith('//') || trimmed.startsWith('*')) return;
    const inlineFfMatch = rawLine.match(/fontFamily\s*:\s*(['"`])([^'"`]+)\1/);
    if (inlineFfMatch) {
      const val = normalizeCssValue(inlineFfMatch[2]);
      if (!validFontFamilies.has(val)) {
        errors.push(`[${relFile}:${idx + 1}] TSX 内联样式违规使用了孤立 fontFamily: "${inlineFfMatch[2]}". 请统一使用 var(--font-sans) 或 var(--font-mono) 或 inherit.`);
      }
    }
  });
}

// 3. 检查全局与 App 级 CSS 中是否对 code / kbd 进行了显式 font-mono 重置 (防宋体回退)
const rootCss = fs.readFileSync(path.join(srcDir, 'index.css'), 'utf8');
if (!rootCss.includes('code') || !rootCss.includes('var(--font-mono)')) {
  errors.push(`[index.css] 必须在全局基础样式中声明 "code, kbd, samp, pre { font-family: var(--font-mono); }" 以防止 Chromium 宋体回退.`);
}

// 4. 检查低于 11.8px / 0.83rem 的小字号与低于 500 的细字重
const allFiles = [...cssFiles, ...tsxFiles];
for (const file of allFiles) {
  if (file.endsWith('check-typography.js') || file.endsWith('.test.ts') || file.endsWith('.test.tsx') || file.endsWith('.spec.tsx')) continue;
  const relFile = path.relative(srcDir, file);
  const content = fs.readFileSync(file, 'utf8');
  const lines = content.split(/\r?\n/);
  let inFontFaceBlock = false;
  let fontFaceBraceDepth = 0;

  lines.forEach((line, idx) => {
    if (file.endsWith('.css')) {
      const trimmed = line.trim();
      if (!inFontFaceBlock && /^@font-face\b/i.test(trimmed)) {
        inFontFaceBlock = true;
        fontFaceBraceDepth = 0;
      }
      if (inFontFaceBlock) {
        for (const ch of line) {
          if (ch === '{') fontFaceBraceDepth++;
          else if (ch === '}') fontFaceBraceDepth--;
        }
        if (fontFaceBraceDepth <= 0 && line.includes('}')) {
          inFontFaceBlock = false;
        }
        return;
      }
    }

    const pxMatch = line.match(/(?:font-size:\s*|fontSize:\s*['"])([0-9]+(?:\.[0-9]+)?)\s*px/i);
    if (pxMatch) {
      const pxVal = parseFloat(pxMatch[1]);
      if (pxVal < 11.8) {
        errors.push(`[${relFile}:${idx + 1}] 违规使用了低于 11.8px 的过小字号: "${line.trim()}". 严禁低于 0.83rem (11.8px)，保障 ClearType 次像素渲染清晰度.`);
      }
    }
    const remMatch = line.match(/(?:font-size:\s*|fontSize:\s*['"])([0-9]+(?:\.[0-9]+)?)\s*rem/i);
    if (remMatch) {
      const remVal = parseFloat(remMatch[1]);
      if (remVal < 0.83) {
        errors.push(`[${relFile}:${idx + 1}] 违规使用了低于 0.83rem 的过小字号: "${line.trim()}". 严禁低于 0.83rem (11.8px)，保障 ClearType 次像素渲染清晰度.`);
      }
    }

    const weightMatch = line.match(/(?:font-weight:\s*|fontWeight:\s*['"]?)([0-9]{3}|normal|lighter)\b/i);
    if (weightMatch) {
      const wVal = weightMatch[1].toLowerCase();
      if (wVal === 'normal' || wVal === 'lighter' || (parseInt(wVal, 10) < 500)) {
        errors.push(`[${relFile}:${idx + 1}] 违规使用了低于 500 的字重: "${line.trim()}". 严禁低于 500 Medium，保障 ClearType 次像素文字字面饱满.`);
      }
    }
  });
}

console.log('[INFO] 正在执行 Tools3000 世界级排版体系与字号底线门禁审查...');
if (errors.length > 0) {
  console.error(`[ERROR] 排版门禁发现 ${errors.length} 处违规:`);
  errors.forEach(e => console.error('  - ' + e));
  process.exit(1);
} else {
  console.log('[OK] 排版门禁审查全部通过！全库 (含 index.css、font 简写与 TSX 内联样式) 100% 遵循单一事实源字体栈与字号字重底线。');
  process.exit(0);
}

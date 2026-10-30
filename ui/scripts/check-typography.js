/**
 * check-typography.js — Tools3000 世界级排版体系与字号底线门禁
 *
 * 检查规则:
 * 1. 严禁使用硬编码孤立字体栈 (如 ui-monospace, Courier New 等，必须统一引用 var(--font-mono) 或 var(--font-sans) 或 inherit)
 * 2. 严禁出现低于 0.83rem (11.8px) 的微小字号 (保障 Windows DirectWrite 次像素渲染字字清晰锐利)
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

let errors = [];

const validFontFamilies = [
  'var(--font-sans)',
  'var(--font-mono)',
  'var(--font-mono, monospace)',
  'inherit',
];

// 1. 检查孤立 font-family
for (const file of cssFiles) {
  // index.css 定义变量本身放行
  if (file.endsWith('index.css')) continue;

  const content = fs.readFileSync(file, 'utf8');
  const lines = content.split('\n');
  lines.forEach((line, idx) => {
    const match = line.match(/font-family:\s*([^;]+);/i);
    if (match) {
      const val = match[1].trim();
      if (!validFontFamilies.includes(val)) {
        errors.push(`[${path.relative(srcDir, file)}:${idx + 1}] 违规使用了孤立 font-family: "${val}". 请统一使用 var(--font-sans) 或 var(--font-mono) 或 inherit.`);
      }
    }
  });
}

// 2. 检查全局与 App 级 CSS 中是否对 code / kbd 进行了显式 font-mono 重置 (防宋体回退)
const rootCss = fs.readFileSync(path.join(srcDir, 'index.css'), 'utf8');
if (!rootCss.includes('code') || !rootCss.includes('var(--font-mono)')) {
  errors.push(`[index.css] 必须在全局基础样式中声明 "code, kbd, samp, pre { font-family: var(--font-mono); }" 以防止 Chromium 宋体回退.`);
}

// 3. 检查低于 11.8px / 0.83rem 的小字号
const allFiles = [...cssFiles, ...tsxFiles];
for (const file of allFiles) {
  if (file.endsWith('check-typography.js') || file.endsWith('.test.ts') || file.endsWith('.test.tsx')) continue;
  const content = fs.readFileSync(file, 'utf8');
  const lines = content.split('\n');
  lines.forEach((line, idx) => {
    const pxMatch = line.match(/(?:font-size:\s*|fontSize:\s*['"])([0-9]+(?:\.[0-9]+)?)\s*px/i);
    if (pxMatch) {
      const pxVal = parseFloat(pxMatch[1]);
      if (pxVal < 11.8) {
        errors.push(`[${path.relative(srcDir, file)}:${idx + 1}] 违规使用了低于 12px 的过小字号: "${line.trim()}". 严禁低于 0.83rem (11.8px)，保障 ClearType 次像素渲染清晰度.`);
      }
    }
    const remMatch = line.match(/(?:font-size:\s*|fontSize:\s*['"])([0-9]+(?:\.[0-9]+)?)\s*rem/i);
    if (remMatch) {
      const remVal = parseFloat(remMatch[1]);
      if (remVal < 0.83) {
        errors.push(`[${path.relative(srcDir, file)}:${idx + 1}] 违规使用了低于 0.83rem 的过小字号: "${line.trim()}". 严禁低于 0.83rem (11.8px)，保障 ClearType 次像素渲染清晰度.`);
      }
    }

    // 4. 检查低于 500 的细字重 (保障方案 B 黄金准则与中文字形饱满度)
    if (file.endsWith('index.css') && idx < 20) {
      // @font-face 声明放行
    } else {
      const weightMatch = line.match(/(?:font-weight:\s*|fontWeight:\s*['"]?)([0-9]{3}|normal|lighter)\b/i);
      if (weightMatch) {
        const wVal = weightMatch[1].toLowerCase();
        if (wVal === 'normal' || wVal === 'lighter' || (parseInt(wVal, 10) < 500)) {
          errors.push(`[${path.relative(srcDir, file)}:${idx + 1}] 违规使用了低于 500 的字重: "${line.trim()}". 严禁低于 500 Medium，保障 ClearType 次像素文字字面饱满.`);
        }
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
  console.log(`[OK] 排版门禁审查全部通过！全库 100% 遵循单一事实源字体栈与字号清晰度底线，0 宋体回退。`);
  process.exit(0);
}

// scripts/check-css-variables.js
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const srcDir = path.join(__dirname, '..', 'src');

function getAllCssFiles(dir) {
  let results = [];
  const list = fs.readdirSync(dir);
  list.forEach(file => {
    const fullPath = path.join(dir, file);
    const stat = fs.statSync(fullPath);
    if (stat && stat.isDirectory()) {
      results = results.concat(getAllCssFiles(fullPath));
    } else if (file.endsWith('.css')) {
      results.push(fullPath);
    }
  });
  return results;
}

const cssFiles = getAllCssFiles(srcDir);
const definedVars = new Set();

// 1. 扫描所有定义的 CSS 变量
cssFiles.forEach(file => {
  const content = fs.readFileSync(file, 'utf8');
  const defineRegex = /(--[a-zA-Z0-9_-]+)\s*:/g;
  let match;
  while ((match = defineRegex.exec(content)) !== null) {
    definedVars.add(match[1]);
  }
});

console.log(`[INFO] 共发现 ${definedVars.size} 个已声明的 CSS 变量。`);

// 2. 扫描所有引用的 CSS 变量
let hasError = false;
cssFiles.forEach(file => {
  const content = fs.readFileSync(file, 'utf8');
  const usageRegex = /var\(\s*(--[a-zA-Z0-9_-]+)(?:\s*,\s*([^)]+))?\)/g;
  let match;
  const relPath = path.relative(srcDir, file);
  while ((match = usageRegex.exec(content)) !== null) {
    const varName = match[1];
    const fallback = match[2];
    if (!definedVars.has(varName)) {
      console.error(`[ERROR] [未定义变量] ${relPath} 中使用了未声明变量: ${varName} (fallback: ${fallback || '无'})`);
      hasError = true;
    }
  }
});

if (!hasError) {
  console.log('[OK] 所有 CSS 变量均已正确声明，0 悬空变量引用！');
} else {
  process.exit(1);
}

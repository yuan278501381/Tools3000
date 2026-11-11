// scripts/check-css-variables.js
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const srcDir = path.join(__dirname, '..', 'src');

function collectFiles(dir, extensions, results = []) {
  const list = fs.readdirSync(dir);
  for (const file of list) {
    const fullPath = path.join(dir, file);
    const stat = fs.statSync(fullPath);
    if (stat && stat.isDirectory()) {
      if (!['node_modules', 'dist', 'coverage_report'].includes(file)) {
        collectFiles(fullPath, extensions, results);
      }
    } else if (extensions.some(ext => file.endsWith(ext))) {
      results.push(fullPath);
    }
  }
  return results;
}

const cssFiles = collectFiles(srcDir, ['.css']);
const sourceFiles = collectFiles(srcDir, ['.css', '.ts', '.tsx']).filter(
  file => !file.endsWith('.test.ts') && !file.endsWith('.test.tsx') && !file.endsWith('.spec.tsx')
);
const definedVars = new Set();

// 1. 扫描所有 CSS 文件中声明的 CSS 变量
cssFiles.forEach(file => {
  const content = fs.readFileSync(file, 'utf8');
  const defineRegex = /(--[a-zA-Z0-9_-]+)\s*:/g;
  let match;
  while ((match = defineRegex.exec(content)) !== null) {
    definedVars.add(match[1]);
  }
});

console.log(`[INFO] 共发现 ${definedVars.size} 个已声明的 CSS 变量，正在扫描 ${sourceFiles.length} 个样式与组件源码文件...`);

// 2. 扫描所有 CSS 与 TS/TSX 内联样式中引用的 var(--xxx)
let hasError = false;
sourceFiles.forEach(file => {
  const content = fs.readFileSync(file, 'utf8');
  const lines = content.split(/\r?\n/);
  const relPath = path.relative(srcDir, file);

  lines.forEach((line, idx) => {
    const trimmed = line.trim();
    if (trimmed.startsWith('//') || trimmed.startsWith('*')) return;
    const usageRegex = /var\(\s*(--[a-zA-Z0-9_-]+)(?:\s*,\s*([^)]+))?\)/g;
    let match;
    while ((match = usageRegex.exec(line)) !== null) {
      const varName = match[1];
      const fallback = match[2];
      if (!definedVars.has(varName)) {
        console.error(`[ERROR] [未定义变量] ${relPath}:${idx + 1} 中使用了未声明变量: ${varName} (fallback: ${fallback || '无'})`);
        hasError = true;
      }
    }
  });
});

if (!hasError) {
  console.log('[OK] 所有 CSS 与 TSX 内联样式引用的 CSS 变量均已正确声明，0 悬空变量引用！');
} else {
  process.exit(1);
}

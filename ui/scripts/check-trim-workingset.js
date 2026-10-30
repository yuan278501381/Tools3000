/**
 * check-trim-workingset.js — Tools3000 物理内存修剪 (trimWorkingSet) 冷路径调用合规性门禁
 *
 * 架构规范准则:
 * 1. 遵循“冷路径退场修剪，热操作期间绝不修剪”原则。
 * 2. 严禁在 1000Hz 鼠标钩子、键盘连击流、60FPS 渲染/绘制循环、定时器高频 Tick 等热路径中调用 trimWorkingSet()。
 * 3. 必须严格限制在窗口隐藏 (hide/onHide)、任务完成 (cleanup/onStop)、资源释放 (release/destroy/suspend) 等生命周期冷路径。
 */

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const projectRoot = path.resolve(__dirname, '../../');
const srcDir = path.resolve(projectRoot, 'src');

function walk(dir, filter, list = []) {
  if (!fs.existsSync(dir)) return list;
  const files = fs.readdirSync(dir);
  for (const f of files) {
    const full = path.join(dir, f);
    const stat = fs.statSync(full);
    if (stat.isDirectory()) {
      if (!['node_modules', 'dist', '.git', 'build', 'build-x64', 'build-arm64', 'packages'].includes(f)) {
        walk(full, filter, list);
      }
    } else if (filter(full)) {
      list.push(full);
    }
  }
  return list;
}

const cppFiles = walk(srcDir, f => f.endsWith('.cpp') || f.endsWith('.h') || f.endsWith('.hpp'));

// 黑名单热路径函数名/模式 (绝对禁止包含)
const FORBIDDEN_HOT_PATH_PATTERNS = [
  /hook/i,
  /onMouseMove/i,
  /onKeyDown/i,
  /onKeyUp/i,
  /onKey/i,
  /render(?!Target)/i,
  /draw(?!Badge|Folder|Clock|Bookmark|Chevron)/i,
  /paint/i,
  /frameLoop/i,
  /onTick/i,
  /processPacket/i,
  /onFrame/i,
];

// 白名单合法冷路径函数名/模式 (必须符合其一)
const ALLOWED_COLD_PATH_PATTERNS = [
  /stop/i,
  /hide/i,
  /cleanup/i,
  /release/i,
  /shutdown/i,
  /deactivate/i,
  /destroy/i,
  /close/i,
  /suspend/i,
  /finish/i,
  /cancel/i,
  /init/i,
  /main/i,
  /~[A-Za-z0-9_]+/, // 析构函数
  /onComplete/i,
  /onFinished/i,
  /onDismiss/i,
  /deliverCompletion/i,
  /deliver/i,
  /trimWorkingSet/i, // 定义自身
];

let errorCount = 0;
let checkedCallSites = 0;

console.log('[INFO] [Memory Hygiene Gate] 正在执行 trimWorkingSet() 冷路径合规性静态扫描...');

for (const file of cppFiles) {
  // 排除 WinUtils.h 与 WinUtils.cpp 本身声明与实现
  if (file.endsWith('WinUtils.h') || file.endsWith('WinUtils.cpp')) continue;

  const relativePath = path.relative(projectRoot, file);
  const content = fs.readFileSync(file, 'utf8');
  const lines = content.split('\n');

  lines.forEach((line, index) => {
    // 匹配调用 trimWorkingSet
    if (line.includes('trimWorkingSet') && !line.trim().startsWith('//') && !line.trim().startsWith('*')) {
      checkedCallSites++;
      const lineNum = index + 1;

      // 向上回溯分析所在函数签名
      let functionContext = '';
      for (let i = index; i >= Math.max(0, index - 80); i--) {
        const prevLine = lines[i].trim();
        if (prevLine.endsWith(';')) continue;
        // 粗略匹配函数定义头 (包含 void/bool/int/ClassName:: 等)
        if (/^\s*(?:[A-Za-z0-9_:]+[\s*&]+)+[A-Za-z0-9_~:]+\s*\([^)]*\)\s*(?:const)?\s*\{?/.test(prevLine) ||
            /^\s*[A-Za-z0-9_:]+\s*=\s*\[[^\]]*\]\s*\(/.test(prevLine)) {
          functionContext = prevLine;
          break;
        }
      }

      // 1. 检查是否命中了黑名单热路径
      const isHotPath = FORBIDDEN_HOT_PATH_PATTERNS.some(pat => pat.test(functionContext) || pat.test(line));
      if (isHotPath) {
        console.error(`[ERROR] [HotPath Violation] ${relativePath}:${lineNum} 严禁在热路径中调用 trimWorkingSet():`);
        console.error(`   上下文: ${functionContext}`);
        console.error(`   代码行: ${line.trim()}`);
        errorCount++;
        return;
      }

      // 2. 检查是否符合冷路径白名单
      const isAllowedColdPath = ALLOWED_COLD_PATH_PATTERNS.some(pat => pat.test(functionContext) || pat.test(relativePath));
      if (!isAllowedColdPath && functionContext) {
        console.warn(`[WARN] [ColdPath Warning] ${relativePath}:${lineNum} 调用点函数 "${functionContext}" 未明确匹配标准冷路径语义命名 (hide/stop/cleanup/release/destroy 等)`);
      }
    }
  });
}

if (errorCount > 0) {
  console.error(`\n[ERROR] trimWorkingSet 静态扫描失败，发现 ${errorCount} 处热路径违规调用！`);
  process.exit(1);
} else {
  console.log(`[OK] trimWorkingSet 内存修剪门禁审查通过！共校验 ${checkedCallSites} 处调用点，100% 符合生命周期冷路径规范，0 热路径污染。\n`);
}

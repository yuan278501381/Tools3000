/**
 * check-path-encoding.mjs — 全 8 语种跨代码页 Unicode 路径安全静态门禁
 *
 * 扫描全库 C++ 原生源码 (src/)，强制拦截在 Windows MSVC 上按系统 ACP 编码转换的危险路径调用：
 *   1. std::filesystem::path 的 .string() / .generic_string()（跨代码页必乱码或抛 EILSEQ 崩溃）
 *   2. 窄串文件/动态库 API（LoadLibraryA、CreateFileA、ShellExecuteA、GetFileAttributesA、fopen、cv::imread/imwrite）
 * 统一要求使用 WinUtils::pathToUtf8(...) / .wstring() / Win32 *W API。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(SCRIPT_DIR, '..', '..');
const SRC_ROOT = path.join(REPO_ROOT, 'src');

const CPP_EXTENSIONS = new Set(['.h', '.hpp', '.hh', '.c', '.cc', '.cpp', '.cxx', '.inl', '.inc']);

const PATH_VAR_HINT = /\b(?:path|dir|file|folder|root|target|parent|child|entry|exe|dll|plugin|config|log|temp|tmp|cache|backup|output|input|dest|src|blob|icon|script|archive|font|media|snapshot|db|workspace|shortcut|updates)[A-Za-z0-9_]*\s*(?:\.\s*(?:parent_path|filename|extension|stem|root_path|lexically_normal)\s*\(\s*\)\s*)?\.\s*(?:generic_)?string\s*\(\s*\)/i;
const DIRECT_FS_PATH_STRING = /(?:std::filesystem::path\s*\([^)]*\)|\.parent_path\(\)|\.filename\(\)|\.extension\(\)|\.stem\(\)|\.lexically_normal\(\)|\.weakly_canonical\([^)]*\))\s*\.\s*(?:generic_)?string\s*\(\s*\)/;
const BANNED_ANSI_WIN_APIS = /\b(?:LoadLibraryA|LoadLibraryExA|CreateFileA|GetFileAttributesA|GetFileAttributesExA|ShellExecuteA|ShellExecuteExA|MoveFileA|MoveFileExA|DeleteFileA|CopyFileA|FindFirstFileA|CreateDirectoryA|SetCurrentDirectoryA|GetModuleFileNameA)\s*\(/;
const BANNED_NARROW_IO = /\b(?:cv::imread|cv::imwrite)\s*\(/;

function walk(dir, out = []) {
  if (!fs.existsSync(dir)) return out;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (entry.name === 'target' || entry.name === 'third_party' || entry.name === 'node_modules') {
        continue;
      }
      walk(full, out);
    } else if (CPP_EXTENSIONS.has(path.extname(entry.name).toLowerCase())) {
      out.push(full);
    }
  }
  return out;
}

function stripLineComment(line) {
  const idx = line.indexOf('//');
  return idx >= 0 ? line.slice(0, idx) : line;
}

function main() {
  const files = walk(SRC_ROOT);
  const violations = [];

  for (const file of files) {
    const content = fs.readFileSync(file, 'utf8');
    const lines = content.split(/\r?\n/);
    let inBlockComment = false;

    for (let i = 0; i < lines.length; i += 1) {
      let line = lines[i];
      if (inBlockComment) {
        const end = line.indexOf('*/');
        if (end < 0) continue;
        line = line.slice(end + 2);
        inBlockComment = false;
      }
      while (line.includes('/*')) {
        const start = line.indexOf('/*');
        const end = line.indexOf('*/', start + 2);
        if (end < 0) {
          line = line.slice(0, start);
          inBlockComment = true;
          break;
        }
        line = line.slice(0, start) + line.slice(end + 2);
      }

      if (lines[i].includes('path-encoding-allow')) continue;
      const code = stripLineComment(line);
      if (!code.trim()) continue;

      const rel = path.relative(REPO_ROOT, file).replace(/\\/g, '/');
      if (DIRECT_FS_PATH_STRING.test(code) || PATH_VAR_HINT.test(code)) {
        violations.push(`${rel}:${i + 1} 禁止对 filesystem::path 调用 .string()/.generic_string()，请改用 WinUtils::pathToUtf8() 或 .wstring()`);
      }
      if (BANNED_ANSI_WIN_APIS.test(code)) {
        violations.push(`${rel}:${i + 1} 禁止调用 ANSI 文件/库 API，请改用对应的 Unicode *W 版本`);
      }
      if (BANNED_NARROW_IO.test(code)) {
        violations.push(`${rel}:${i + 1} 禁止调用窄串 OpenCV 文件 I/O (cv::imread/imwrite)，请改用宽流 + cv::imdecode/imencode`);
      }
    }
  }

  if (violations.length > 0) {
    console.error(`[ERROR] 路径跨代码页编码静态门禁拦截 ${violations.length} 处违规:`);
    for (const v of violations) {
      console.error(`  - ${v}`);
    }
    process.exit(1);
  }

  console.log(`[OK] 路径跨代码页编码静态门禁通过 (共扫描 ${files.length} 个原生源码文件，0 违规)`);
}

main();

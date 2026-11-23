/**
 * run-static-gates.mjs — 并发执行 Tools3000 前端与原生跨边界 6 重静态质量门禁
 *
 * 相比串联执行 6 个 Node 进程，并发调度可将全量静态门禁时间压缩至最慢单门禁耗时 (~0.5s)，
 * 同时保持 100% 相同的检查覆盖范围、严格阻断退出码与按声明顺序输出的清晰日志。
 */
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const UI_ROOT = path.resolve(SCRIPT_DIR, '..');

const STATIC_GATE_SCRIPTS = [
  'check-i18n.js',
  'check-native-i18n.js',
  'check-logger.js',
  'check-css-variables.js',
  'check-typography.js',
  'check-trim-workingset.js',
  'check-path-encoding.mjs',
];

function runGateScript(scriptName) {
  return new Promise((resolve) => {
    const startMs = Date.now();
    const scriptPath = path.join(SCRIPT_DIR, scriptName);
    const child = spawn(process.execPath, [scriptPath], {
      cwd: UI_ROOT,
      stdio: ['ignore', 'pipe', 'pipe'],
      windowsHide: true,
    });

    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (chunk) => {
      stdout += chunk.toString('utf8');
    });
    child.stderr.on('data', (chunk) => {
      stderr += chunk.toString('utf8');
    });

    child.on('error', (err) => {
      resolve({
        scriptName,
        code: 1,
        stdout,
        stderr: `${stderr}\n[ERROR] 无法启动静态门禁脚本 ${scriptName}: ${err.message}`,
        elapsedMs: Date.now() - startMs,
      });
    });

    child.on('close', (code) => {
      resolve({
        scriptName,
        code: code ?? 1,
        stdout,
        stderr,
        elapsedMs: Date.now() - startMs,
      });
    });
  });
}

async function main() {
  const wallStart = Date.now();
  const results = await Promise.all(STATIC_GATE_SCRIPTS.map(runGateScript));

  let failedCount = 0;
  for (const res of results) {
    if (res.stdout) process.stdout.write(res.stdout);
    if (res.stderr) process.stderr.write(res.stderr);
    if (res.code !== 0) {
      failedCount += 1;
      console.error(`[ERROR] 静态门禁未通过: ${res.scriptName} (exit=${res.code}, ${res.elapsedMs}ms)`);
    }
  }

  const wallMs = Date.now() - wallStart;
  if (failedCount > 0) {
    console.error(`[ERROR] 共 ${failedCount}/${STATIC_GATE_SCRIPTS.length} 项静态门禁失败 (${wallMs}ms)`);
    process.exit(1);
  }
  console.log(`[OK] ${STATIC_GATE_SCRIPTS.length} 重并发静态规范门禁全部通过 (${wallMs}ms)`);
}

main();

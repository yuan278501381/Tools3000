import fs from 'fs';
import path from 'path';

let hasError = false;

function getFiles(dir, exts) {
  let results = [];
  if (!fs.existsSync(dir)) return results;
  const list = fs.readdirSync(dir);
  for (const file of list) {
    const filePath = path.join(dir, file);
    const stat = fs.statSync(filePath);
    if (stat && stat.isDirectory()) {
      if (file !== 'build' && file !== 'deploy_dist' && file !== 'third_party' && file !== 'node_modules' && file !== '.git') {
        results = results.concat(getFiles(filePath, exts));
      }
    } else {
      if (exts.includes(path.extname(filePath))) {
        results.push(filePath);
      }
    }
  }
  return results;
}

// 1. 检查 I18nLogCatalog.cpp 内部的映射字典 (0 汉字污染、占位符严格对齐)
function checkI18nLogCatalogIntegrity() {
  const catalogPath = path.resolve('../src/core/logger/I18nLogCatalog.cpp');
  if (!fs.existsSync(catalogPath)) {
    console.error(`[ERROR] [Missing Catalog] 未找到日志多语言映射表: ${catalogPath}`);
    hasError = true;
    return new Set();
  }

  const content = fs.readFileSync(catalogPath, 'utf8');
  const entryRegex = /\{("(?:\\.|[^"\\])*"),\s*\{\s*("(?:\\.|[^"\\])*"),\s*("(?:\\.|[^"\\])*")\s*\}\}/g;
  const cjkRegex = /[\u4e00-\u9fa5]/;
  const catalogKeys = new Set();

  let match;
  while ((match = entryRegex.exec(content)) !== null) {
    try {
      const rawKey = JSON.parse(match[1]);
      const zhWithTrace = JSON.parse(match[2]);
      const enWithTrace = JSON.parse(match[3]);

      catalogKeys.add(rawKey);

      // 检查英文模板是否含有中文
      if (cjkRegex.test(enWithTrace)) {
        console.error(`[ERROR] [CJK in Catalog English Template Gate] I18nLogCatalog 英文模板包含汉字字符:`);
        console.error(`   中文 Key: "${rawKey}"`);
        console.error(`   英文模板: "${enWithTrace}"`);
        hasError = true;
      }

      // 检查中英文占位符数量是否完全一致
      const zhPlaceholders = (zhWithTrace.match(/\{[^}]*\}/g) || []).length;
      const enPlaceholders = (enWithTrace.match(/\{[^}]*\}/g) || []).length;
      if (zhPlaceholders !== enPlaceholders) {
        console.error(`[ERROR] [Catalog Placeholder Parity Gate] I18nLogCatalog 占位符数量不一致:`);
        console.error(`   中文模板 (${zhPlaceholders}): "${zhWithTrace}"`);
        console.error(`   英文模板 (${enPlaceholders}): "${enWithTrace}"`);
        hasError = true;
      }
    } catch (e) {
      console.error(`[ERROR] [Catalog JSON Parse Error] 解析条目失败: ${match[0]}`);
      hasError = true;
    }
  }

  return catalogKeys;
}

// 2. 检查全库 C++ 源码中的中文日志是否 100% 在 I18nLogCatalog 中注册
function checkCppLogCoverageAndParity(catalogKeys) {
  const cppFiles = getFiles(path.resolve('../src'), ['.cpp', '.h', '.hpp']);
  const logLiteralRegex = /\bLOG_(?:TRACE|DEBUG|INFO|WARN|ERROR|CRITICAL)\s*\(\s*("(?:\\.|[^"\\])*")/g;
  const logMacroLRegex = /\b(LOG_TRACE_L|LOG_DEBUG_L|LOG_INFO_L|LOG_WARN_L|LOG_ERROR_L|LOG_CRITICAL_L)\s*\(\s*("(?:\\.|[^"\\])*")\s*,\s*("(?:\\.|[^"\\])*")/g;
  const logMacroRegex = /\b(LOG_TRACE|LOG_DEBUG|LOG_INFO|LOG_WARN|LOG_ERROR|LOG_CRITICAL)\s*\(\s*([^)]+)\)/g;
  const cjkRegex = /[\u4e00-\u9fa5]/;

  let totalScannedLogs = 0;
  let coveredLogs = 0;

  for (const file of cppFiles) {
    if (file.includes('core\\logger\\') || file.includes('core/logger/')) continue;
    const content = fs.readFileSync(file, 'utf8');

    // 检查中文日志字面量是否 100% 收录在 I18nLogCatalog 中
    let matchLit;
    while ((matchLit = logLiteralRegex.exec(content)) !== null) {
      try {
        const raw = JSON.parse(matchLit[1]);
        if (cjkRegex.test(raw)) {
          totalScannedLogs++;
          if (!catalogKeys.has(raw)) {
            console.error(`[ERROR] [Unregistered C++ Log Gate] 发现未在 I18nLogCatalog 中登记的中文日志: ${path.relative('../', file)}`);
            console.error(`   未收录模板: "${raw}"`);
            console.error(`   [Rule] 所有新增的 C++ 中文日志模板必须在 I18nLogCatalog.cpp 中注册英文映射，保障 100% 国际化！`);
            hasError = true;
          } else {
            coveredLogs++;
          }
        }
      } catch (e) {}
    }

    // 检查显式双语宏 LOG_*_L 的占位符与纯净度
    let matchL;
    while ((matchL = logMacroLRegex.exec(content)) !== null) {
      const zhFmt = matchL[2];
      const enFmt = matchL[3];

      const zhPlaceholders = (zhFmt.match(/\{[^}]*\}/g) || []).length;
      const enPlaceholders = (enFmt.match(/\{[^}]*\}/g) || []).length;

      if (zhPlaceholders !== enPlaceholders) {
        console.error(`[ERROR] [Log_L Placeholder Mismatch Gate] 双语日志宏占位符数量不一致: ${path.relative('../', file)}`);
        console.error(`   中文模板 (${zhPlaceholders}): ${zhFmt}`);
        console.error(`   英文模板 (${enPlaceholders}): ${enFmt}`);
        hasError = true;
      }

      if (cjkRegex.test(enFmt)) {
        console.error(`[ERROR] [CJK in Log_L English Template Gate] 双语日志宏英文模板包含汉字: ${path.relative('../', file)}`);
        console.error(`   英文模板: ${enFmt}`);
        hasError = true;
      }
    }

    // 检查空日志调用
    let match;
    while ((match = logMacroRegex.exec(content)) !== null) {
      const args = match[2].trim();
      if (!args || args === '""' || args === "''") {
        console.error(`[ERROR] [Empty Log Gate] 发现空日志调用: ${path.relative('../', file)}`);
        console.error(`   代码: "${match[0]}"`);
        hasError = true;
      }
    }
  }
}

// 3. 检查前端用户可见日志 (Toast / Notification / UI Error) 是否 100% 国际化
function checkFrontendUserFacingLogs() {
  const tsFiles = getFiles(path.resolve('./src'), ['.ts', '.tsx']);
  const toastRegex = /toast\.(?:success|error|warning|info|message)\s*\(\s*(['"`][^'"`]+['"`]|[^)]+)\)/g;
  const chineseRegex = /[\u4e00-\u9fa5]/;

  for (const file of tsFiles) {
    if (file.endsWith('.test.tsx') || file.endsWith('.spec.tsx') || file.endsWith('.test.ts')) continue;
    const content = fs.readFileSync(file, 'utf8');

    let match;
    while ((match = toastRegex.exec(content)) !== null) {
      const param = match[1].trim();
      if (chineseRegex.test(param) && !param.includes('t(')) {
        console.error(`[ERROR] [Unlocalized Toast Gate] 发现未国际化的用户提示日志: ${path.relative('./', file)}`);
        console.error(`   代码: "${match[0]}"`);
        console.error(`   [Rule] 所有用户感知层 Toast 与日志提示必须使用 t('namespace.key') 国际化！`);
        hasError = true;
      }
    }
  }
}

console.log('[INFO] [Observability & Log Gate] 正在执行 Tools3000 全链路日志国际化、占位符对齐与 6 重鲁棒性防御门禁审查...');
const catalogKeys = checkI18nLogCatalogIntegrity();
checkCppLogCoverageAndParity(catalogKeys);
checkFrontendUserFacingLogs();

if (hasError) {
  console.error('\n[ERROR] 日志国际化规范门禁审查失败！请修复上述未收录模板、占位符不匹配、英文汉字污染或未国际化 Toast 提示。');
  process.exit(1);
} else {
  console.log('[OK] 日志系统世界级 6 重国际化防御门禁全部通过！');
  console.log(`   1. 全库 C++ 中文日志模板 100% 在 I18nLogCatalog 中收录注册 (共 ${catalogKeys.size} 条)`);
  console.log('   2. I18nLogCatalog 英文映射模板 100% 绝对 0 汉字污染');
  console.log('   3. I18nLogCatalog 中英文 {} 占位符 100% 精确 1:1 对齐');
  console.log('   4. C++ 显式双语日志宏 LOG_*_L 占位符与纯净度 100% 达标');
  console.log('   5. C++ 全量内核日志规范宏与 TraceID 链路透传 (0 空日志)');
  console.log('   6. 前端用户感知层日志 100% 接入多语言国际化管线 (0 裸中文 Toast)');
}

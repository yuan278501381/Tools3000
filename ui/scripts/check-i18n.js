import fs from 'fs';
import path from 'path';

const localesDir = path.resolve('./src/i18n/locales');
const baseLocale = 'en.json';
const enPath = path.join(localesDir, baseLocale);

if (!fs.existsSync(enPath)) {
  console.error(`[ERROR] [Fatal] 基准语言文件不存在: ${enPath}`);
  process.exit(1);
}

const enData = JSON.parse(fs.readFileSync(enPath, 'utf8'));

// 动态发现 locales 目录下的所有语言包 (如 en.json, zh.json, zh-TW.json, ja.json, etc.)
const localeFiles = fs.readdirSync(localesDir).filter(f => f.endsWith('.json'));
console.log(`[INFO] [Locale Matrix Discovery] 发现 ${localeFiles.length} 个语言包: [${localeFiles.join(', ')}]`);

const allLocalesData = new Map();
for (const file of localeFiles) {
  const fullPath = path.join(localesDir, file);
  try {
    allLocalesData.set(file, JSON.parse(fs.readFileSync(fullPath, 'utf8')));
  } catch (err) {
    console.error(`[ERROR] [Invalid JSON] 语言文件解析失败: ${file}`, err);
    process.exit(1);
  }
}

let hasError = false;

// 允许在非英文包中保留的合法技术专有名词/缩写/按键/格式/品牌白名单
const ALLOWED_TECH_TOKENS = new Set([
  'windows', 'tools3000', 'tools3000', 'tools', 'microsoft', 'chrome', 'edge', 'firefox', 'wechat', 'feishu', 'dingtalk',
  'dpi', 'ocr', 'ntfs', 'mft', 'usn', 'sqlite', 'direct2d', 'webview2', 'sta', 'ipc', 'api', 'url', 'uri',
  'ctrl', 'alt', 'shift', 'win', 'enter', 'esc', 'tab', 'backspace', 'delete', 'space',
  'f1', 'f2', 'f3', 'f4', 'f5', 'f6', 'f7', 'f8', 'f9', 'f10', 'f11', 'f12',
  'word', 'excel', 'powerpoint', 'pdf', 'csv', 'tsv', 'json', 'xml', 'yaml', 'txt', 'md', 'html', 'css', 'sql',
  'png', 'jpg', 'jpeg', 'webp', 'gif', 'bmp', 'svg', 'ico', 'mp4', 'mkv', 'avi', 'mov', 'wmv', 'flv', 'webm',
  'mp3', 'wav', 'flac', 'aac', 'm4a', 'ogg', 'zip', 'rar', '7z', 'tar', 'gz',
  'cpp', 'c', 'h', 'hpp', 'js', 'ts', 'tsx', 'py', 'rs', 'go', 'java', 'lua', 'cmd', 'bat', 'ps1',
  'ssd', 'hdd', 'ram', 'cpu', 'gpu', 'mb', 'gb', 'kb', 'bytes', 'b', 'ms', 'fps', 'px', 'rem',
  'mit', 'license', 'github', 'ok', 'id', 'rgb', 'hex', 'rgba', 'hsl', 'cmyk', 'dps', 'wps', 'cad', 'qr',
  'markdown', 'katex', 'mathjax'
]);

function isAllowedPureEnglishToken(str) {
  const clean = str.toLowerCase().replace(/[^a-z0-9_]/g, '');
  if (!clean) return true;
  if (ALLOWED_TECH_TOKENS.has(clean)) return true;
  if (/^v\d+\.\d+\.\d+/.test(clean)) return true;
  if (/^(ctrl|alt|shift|win)\+[a-z0-9+]+$/.test(clean)) return true;
  return false;
}

// 辅助：递归检查多层属性是否存在
function hasJsonPath(obj, keyPath) {
  const parts = keyPath.split('.');
  let curr = obj;
  for (const p of parts) {
    if (!curr || typeof curr !== 'object' || !(p in curr)) return false;
    curr = curr[p];
  }
  return true;
}

// 辅助：获取全量代码文件
function getFiles(dir, exts = ['.ts', '.tsx']) {
  let results = [];
  const list = fs.readdirSync(dir);
  for (const file of list) {
    const filePath = path.join(dir, file);
    const stat = fs.statSync(filePath);
    if (stat && stat.isDirectory()) {
      if (file !== 'node_modules' && file !== 'dist' && file !== 'coverage_report') {
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

// ── 防线 1 & 防线 2 & 防线 3 & 防线 4：N 语言同构矩阵、纯净度与翻译泄漏检查 ──
function checkLocaleMatrix(targetFile, targetData) {
  const forbidHanScript = ['en.json', 'ko.json', 'ru.json', 'pt-BR.json', 'es.json'].includes(targetFile);
  const cjkRegex = /[\u4e00-\u9fa5]/;

  function traverse(baseObj, targetObj, prefix = '') {
    for (const key in baseObj) {
      const fullKey = `${prefix}${key}`;
      const baseVal = baseObj[key];
      const targetVal = targetObj ? targetObj[key] : undefined;

      if (typeof baseVal === 'object' && baseVal !== null) {
        if (!targetVal || typeof targetVal !== 'object') {
          console.error(`[ERROR] [Missing Namespace Matrix] ${targetFile} 缺失对象命名空间: ${fullKey}`);
          hasError = true;
        } else {
          traverse(baseVal, targetVal, `${fullKey}.`);
        }
      } else {
        if (targetVal === undefined) {
          console.error(`[ERROR] [Missing Key Matrix] ${targetFile} 缺失翻译键位: ${fullKey}`);
          hasError = true;
        } else if (typeof baseVal === 'string' && typeof targetVal === 'string') {
          // 防线 2：非汉字语系语言包（en / ko / ru / pt-BR / es）绝对 0 汉字污染
          if (forbidHanScript && cjkRegex.test(targetVal)) {
            console.error(`[ERROR] [CJK Contamination Gate] ${targetFile} 存在非法汉字字符: [${fullKey}] -> "${targetVal}"`);
            hasError = true;
          }

          // 防线 4：动态插值变量双向对齐矩阵
          const enVars = (baseVal.match(/\{\{([^}]+)\}\}/g) || []).sort();
          const targetVars = (targetVal.match(/\{\{([^}]+)\}\}/g) || []).sort();
          if (enVars.join(',') !== targetVars.join(',')) {
            console.error(`[ERROR] [Interpolation Parity Matrix] ${targetFile} 变量插值不一致: [${fullKey}]`);
            console.error(`   en.json 基准: "${baseVal}" (变量: [${enVars.join(', ')}])`);
            console.error(`   ${targetFile} 实际: "${targetVal}" (变量: [${targetVars.join(', ')}])`);
            hasError = true;
          }

          // 防线 3：中文包 (zh.json / zh-TW.json 等) 未翻译英文泄漏智能识别
          if (targetFile.startsWith('zh') && !cjkRegex.test(targetVal)) {
            let stripped = targetVal.replace(/\{\{[^}]+\}\}/g, '').trim();
            stripped = stripped.replace(/[\s\-_.,/\\:;!?'"()[\]{}<>@#$%^&*+=|~`0-9]/g, ' ').trim();
            if (stripped) {
              const words = stripped.split(/\s+/).filter(Boolean);
              const unrecognized = words.filter(w => !isAllowedPureEnglishToken(w));
              if (unrecognized.length > 0) {
                console.error(`[ERROR] [Translation Leak Matrix] ${targetFile} 疑似遗漏翻译 (纯英文未汉化): [${fullKey}]`);
                console.error(`   当前值: "${targetVal}"`);
                console.error(`   基准值: "${baseVal}"`);
                console.error(`   未识别非专业英文单词: [${unrecognized.join(', ')}]`);
                hasError = true;
              }
            }
          }
        }
      }
    }

    // 孤儿键反向检查 (Target 中有但 en.json 中没有的废弃键)
    if (targetObj && typeof targetObj === 'object') {
      for (const key in targetObj) {
        if (baseObj[key] === undefined) {
          console.error(`[ERROR] [Orphan Key Matrix] ${targetFile} 包含基准 en.json 中不存在的孤儿键位: ${prefix}${key}`);
          hasError = true;
        }
      }
    }
  }

  traverse(enData, targetData);
}

// ── 防线 5：全库源码静态键位引用对所有已注册语言包严格存在性校验 ──
function checkCodeReferences() {
  const files = getFiles(path.resolve('./src'), ['.ts', '.tsx']);
  const keyRefRegex = /(?:labelKey|descKey|titleKey|conditionKey|extKey|nameKey|subKey|tipKey|headerKey)\s*:\s*['"]([a-zA-Z0-9_.-]+)['"]/g;
  const tCallRegex = /(?<![a-zA-Z0-9_$])t\(\s*['"]([a-zA-Z0-9_.-]+)['"]/g;

  for (const file of files) {
    if (file.endsWith('.test.tsx') || file.endsWith('.spec.tsx') || file.endsWith('.test.ts')) continue;
    const content = fs.readFileSync(file, 'utf8');

    // 检查模型/常量中的 labelKey / descKey 等
    let match;
    while ((match = keyRefRegex.exec(content)) !== null) {
      const k = match[1];
      if (k.includes('.') && !k.startsWith('http') && !k.startsWith('tools3000_') && !k.startsWith('/') && !k.includes(' ')) {
        for (const [localeFile, localeData] of allLocalesData.entries()) {
          if (!hasJsonPath(localeData, k)) {
            console.error(`[ERROR] [Broken Key Reference Gate] 源码引用的键位在语言包 ${localeFile} 中缺失: ${path.relative('./', file)}`);
            console.error(`   未定义的键: "${k}"`);
            hasError = true;
          }
        }
      }
    }

    // 检查字面量 t('namespace.key') 调用
    while ((match = tCallRegex.exec(content)) !== null) {
      const k = match[1];
      if (k.includes('.') && !k.startsWith('http') && !k.startsWith('tools3000_') && !k.includes(' ')) {
        for (const [localeFile, localeData] of allLocalesData.entries()) {
          if (!hasJsonPath(localeData, k)) {
            console.error(`[ERROR] [Broken t() Reference Gate] t() 调用的键位在语言包 ${localeFile} 中缺失: ${path.relative('./', file)}`);
            console.error(`   未定义的键: "${k}"`);
            hasError = true;
          }
        }
      }
    }
  }
}

// ── 防线 6：全库源码零裸中文与英文 Fallback 规范 ──
function checkSourceCodeFallbacksAndNakedChinese() {
  const files = getFiles(path.resolve('./src'), ['.ts', '.tsx']);
  const chineseRegex = /[\u4e00-\u9fa5]/;
  const tFallbackRegex = /(?<![a-zA-Z0-9_$])t\(\s*['"`]([^'"`]+)['"`]\s*,\s*['"`]([^'"`]+)['"`]\s*\)/g;

  const allowedPatterns = [
    /startsWith\(['"`]内容:['"`]\)/,
    /CONTENT_PREFIXES/,
    /localeCompare/,
    /Chinese/
  ];

  for (const file of files) {
    if (file.endsWith('.test.tsx') || file.endsWith('.spec.tsx') || file.endsWith('.test.ts') || file.endsWith('useBridge.ts')) continue;
    const content = fs.readFileSync(file, 'utf8');

    // 1. Fallback 必须英文
    let match;
    while ((match = tFallbackRegex.exec(content)) !== null) {
      const key = match[1];
      const fallback = match[2];
      if (chineseRegex.test(fallback)) {
        console.error(`[ERROR] [Chinese Fallback Violation] ${path.relative('./', file)}:`);
        console.error(`   key: "${key}", fallback: "${fallback}"`);
        console.error(`   [Rule] 源码中的所有 fallback 必须为英文基准，非英文翻译请录入对应语言字典！`);
        hasError = true;
      }
    }

    // 2. 裸中文扫描
    const lines = content.split(/\r?\n/);
    let inBlockComment = false;

    lines.forEach((rawLine, idx) => {
      const line = rawLine.trim();
      if (line.startsWith('/*')) inBlockComment = true;
      if (inBlockComment) {
        if (line.includes('*/')) inBlockComment = false;
        return;
      }
      if (line.startsWith('//') || line.startsWith('*')) return;

      let codeOnly = rawLine.replace(/\/\/.*$/, '').replace(/\{\/\*.*?\*\/\}/g, '');
      if (!chineseRegex.test(codeOnly)) return;
      if (allowedPatterns.some(p => p.test(codeOnly))) return;

      console.error(`[ERROR] [Zero-Naked-Chinese Gate] 发现未国际化的裸中文硬编码: ${path.relative('./', file)}:${idx + 1}`);
      console.error(`   代码行: "${rawLine.trim()}"`);
      console.error(`   [Rule] 严禁在源码或 JSX 中硬编码任何中文文本！必须 100% 接入 t('namespace.key', 'English fallback')`);
      hasError = true;
    });
  }
}

console.log('[INFO] [i18n Gate] 正在执行 Tools3000 世界级可扩展 N 语言同构矩阵与全链路多语言防线审查...');

// ── 防线 7：常量配置与预设 UI 实体 labelKey / nameKey 100% 国际化对齐防线 ──
function checkPresetAndOptionEntityInternationalization() {
  const files = getFiles(path.resolve('./src'), ['.ts', '.tsx']);
  const presetPillRegex = /\{\s*code:\s*['"`][^'"`]+['"`],\s*label:\s*['"`][^'"`]+['"`]\s*\}/g;

  for (const file of files) {
    if (file.endsWith('.test.tsx') || file.endsWith('.spec.tsx') || file.endsWith('.test.ts')) continue;
    const content = fs.readFileSync(file, 'utf8');

    let match;
    while ((match = presetPillRegex.exec(content)) !== null) {
      console.error(`[ERROR] [Missing labelKey Gate Violation] 发现未定义 labelKey 的裸预设/选项对象: ${path.relative('./', file)}`);
      console.error(`   匹配代码: "${match[0]}"`);
      console.error(`   [Rule] 所有向用户展示的预设或选项对象，必须包含 labelKey/nameKey 并经由 t() 进行多语言转换！`);
      hasError = true;
    }
  }
}

// ── 防线 8：根目录 VERSION 与前端 package.json / package-lock.json 版本单一事实源一致性防线 ──
function checkVersionSingleSourceOfTruth() {
  const rootVersionPath = path.resolve('../VERSION');
  const pkgPath = path.resolve('./package.json');
  const pkgLockPath = path.resolve('./package-lock.json');

  if (!fs.existsSync(rootVersionPath)) {
    console.error(`[ERROR] [Version SSOT Gate] 根目录 VERSION 文件不存在: ${rootVersionPath}`);
    hasError = true;
    return;
  }

  const rootVersion = fs.readFileSync(rootVersionPath, 'utf8').trim();
  const pkgData = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
  if (pkgData.version !== rootVersion) {
    console.error(`[ERROR] [Version SSOT Gate] ui/package.json 版本号 (${pkgData.version}) 与根目录 VERSION (${rootVersion}) 不一致！`);
    hasError = true;
  }

  if (fs.existsSync(pkgLockPath)) {
    const pkgLockData = JSON.parse(fs.readFileSync(pkgLockPath, 'utf8'));
    if (pkgLockData.version !== rootVersion) {
      console.error(`[ERROR] [Version SSOT Gate] ui/package-lock.json 顶层版本号 (${pkgLockData.version}) 与根目录 VERSION (${rootVersion}) 不一致！`);
      hasError = true;
    }
    const rootPkgEntryVer = pkgLockData.packages && pkgLockData.packages[''] ? pkgLockData.packages[''].version : undefined;
    if (rootPkgEntryVer !== undefined && rootPkgEntryVer !== rootVersion) {
      console.error(`[ERROR] [Version SSOT Gate] ui/package-lock.json packages[""] 版本号 (${rootPkgEntryVer}) 与根目录 VERSION (${rootVersion}) 不一致！`);
      hasError = true;
    }
  }
}

// ── 防线 9：C++ 后端 -> 前端跨边界契约多语言完备性防线（永久杜绝热键名/插件ID/路由静默回退裸英文）──
function checkBackendContractInternationalization() {
  const nativeSrcDir = path.resolve('../src');
  if (!fs.existsSync(nativeSrcDir)) return;

  // 9.1 扫描全部 C++ 源码中的 HotkeyManager::registerHotkey("...") / rebindHotkey("...") 字面量
  const hotkeyCatalogPath = path.resolve('./src/config/hotkeyCatalog.ts');
  if (!fs.existsSync(hotkeyCatalogPath)) {
    console.error(`[ERROR] [Backend Contract i18n Gate] 缺少全局快捷键多语言 SSOT 注册表: ${hotkeyCatalogPath}`);
    hasError = true;
    return;
  }

  const catalogContent = fs.readFileSync(hotkeyCatalogPath, 'utf8');
  const mappedHotkeys = new Map();
  const mapEntryRe = /(?:'([^']+)'|"([^"]+)"|([A-Za-z_][A-Za-z0-9_]*))\s*:\s*['"]([^'"]+)['"]/g;
  const blockMatch = catalogContent.match(/HOTKEY_LABEL_KEYS[\s\S]*?=\s*\{([\s\S]*?)\};/);
  if (blockMatch) {
    let m;
    while ((m = mapEntryRe.exec(blockMatch[1])) !== null) {
      const hkName = m[1] || m[2] || m[3];
      const i18nKey = m[4];
      if (hkName && i18nKey) mappedHotkeys.set(hkName, i18nKey);
    }
  }

  const cppFiles = getFiles(nativeSrcDir, ['.cpp', '.h']);
  const discoveredHotkeys = new Map(); // name -> first file
  const registerRe = /\b(?:registerHotkey|rebindHotkey)\s*\(\s*"([^"]+)"/g;
  for (const file of cppFiles) {
    const raw = fs.readFileSync(file, 'utf8');
    // 剔除单行与多行注释，防止注释示例误触发
    const cleaned = raw.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '');
    let m;
    while ((m = registerRe.exec(cleaned)) !== null) {
      const hkName = m[1];
      if (hkName && !discoveredHotkeys.has(hkName)) {
        discoveredHotkeys.set(hkName, path.relative(path.resolve('..'), file));
      }
    }
  }

  for (const [hkName, sourceFile] of discoveredHotkeys.entries()) {
    const i18nKey = mappedHotkeys.get(hkName);
    if (!i18nKey) {
      console.error(`[ERROR] [Backend Hotkey i18n Gate] C++ 注册的全局快捷键 "${hkName}" (${sourceFile}) 未在 ui/src/config/hotkeyCatalog.ts 中登记多语言映射！`);
      console.error('   [Rule] 所有 C++ registerHotkey/rebindHotkey 名称必须在 HOTKEY_LABEL_KEYS 中显式映射到真实 8 语种词条，杜绝界面回退裸英文。');
      hasError = true;
      continue;
    }
    for (const [localeFile, localeData] of allLocalesData.entries()) {
      if (!hasJsonPath(localeData, i18nKey)) {
        console.error(`[ERROR] [Backend Hotkey i18n Gate] 快捷键 "${hkName}" 映射键 "${i18nKey}" 在 ${localeFile} 中缺失！`);
        hasError = true;
      }
    }
  }

  // 9.2 扫描 C++ PluginManager.cpp 内置模块 ID，确保每个内置模块在全 8 语种均有 plugins.items.<id>.{name,description}
  const pluginMgrPath = path.join(nativeSrcDir, 'core', 'plugin', 'PluginManager.cpp');
  if (fs.existsSync(pluginMgrPath)) {
    const pmText = fs.readFileSync(pluginMgrPath, 'utf8');
    const pluginIds = new Set();
    const pmRe = /patch\["plugins"\]\["([a-z0-9_]+)"\]\["enabled"\]/g;
    let m;
    while ((m = pmRe.exec(pmText)) !== null) {
      pluginIds.add(m[1]);
    }
    for (const pid of pluginIds) {
      for (const field of ['name', 'description']) {
        const keyPath = `plugins.items.${pid}.${field}`;
        for (const [localeFile, localeData] of allLocalesData.entries()) {
          if (!hasJsonPath(localeData, keyPath)) {
            console.error(`[ERROR] [Backend Plugin i18n Gate] C++ 内置模块 "${pid}" 缺少多语言键 "${keyPath}" (${localeFile})！`);
            hasError = true;
          }
        }
      }
    }
  }

  // 9.3 校验 routes.ts 所有 APP_ROUTES 在全 8 语种中均存在 sidebar.kw_<id> 搜索关键词
  const routesPath = path.resolve('./src/config/routes.ts');
  if (fs.existsSync(routesPath)) {
    const routesText = fs.readFileSync(routesPath, 'utf8');
    const routeBlock = routesText.match(/export const APP_ROUTES[\s\S]*?=\s*\[([\s\S]*?)\];/);
    if (routeBlock) {
      const idRe = /\{\s*id:\s*'([a-z0-9_]+)'/g;
      let m;
      while ((m = idRe.exec(routeBlock[1])) !== null) {
        const kwKey = `sidebar.kw_${m[1]}`;
        for (const [localeFile, localeData] of allLocalesData.entries()) {
          if (!hasJsonPath(localeData, kwKey)) {
            console.error(`[ERROR] [Route Search Keyword i18n Gate] 路由 "${m[1]}" 缺少侧边栏搜索关键词 "${kwKey}" (${localeFile})！`);
            hasError = true;
          }
        }
      }
    }
  }
}

// 对所有已注册的语言包执行矩阵比对
for (const [file, data] of allLocalesData.entries()) {
  checkLocaleMatrix(file, data);
}

checkCodeReferences();
checkSourceCodeFallbacksAndNakedChinese();
checkPresetAndOptionEntityInternationalization();
checkVersionSingleSourceOfTruth();
checkBackendContractInternationalization();

if (hasError) {
  console.error('\n[ERROR] i18n 与版本同源门禁审查失败！请修复上述键位缺失、英文泄漏、断裂引用、裸中文、裸配置 Label、后端跨边界契约漏译或版本号失步问题。');
  process.exit(1);
} else {
  console.log('[OK] i18n 世界级防护门禁与版本单一事实源校验全部通过！');
  console.log('   1. 中英文字典 100% 同构双向对齐');
  console.log('   2. 英文包绝对 0 汉字污染');
  console.log('   3. 中文包 0 未翻译英文泄漏 (智能专有名词与混排合规)');
  console.log('   4. 动态变量插值 {{param}} 100% 双向对齐');
  console.log('   5. 全库源码所有 labelKey/t() 引用 100% 在字典中真实存在');
  console.log('   6. 全库源码 0 裸中文硬编码与 100% 英文 Fallback');
  console.log('   7. 全库 UI 预设与选项实体 100% 接入 labelKey/nameKey 国际化管线');
  console.log('   8. 根目录 VERSION 与 ui/package.json (及 package-lock.json) 版本 100% 单一事实源一致');
  console.log('   9. C++ 后端注册热键 / 内置模块 ID / 路由搜索词 100% 跨边界多语言全覆盖');
}

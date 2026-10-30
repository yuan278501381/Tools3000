import fs from 'node:fs';
import path from 'node:path';

const sourcePath = path.resolve('index.html');
const distPath = path.resolve('dist/index.html');
const failures = [];

function inspectHtml(filePath, label) {
  if (!fs.existsSync(filePath)) {
    failures.push(`${label} does not exist: ${filePath}`);
    return;
  }

  const html = fs.readFileSync(filePath, 'utf8');
  const rootCount = (html.match(/\bid=["']root["']/gi) ?? []).length;
  if (rootCount !== 1) {
    failures.push(`${label} must contain exactly one #root mount point (found ${rootCount})`);
  }
  if (!/<title>Tools3000<\/title>/i.test(html)) {
    failures.push(`${label} must use the production Tools3000 title`);
  }
  if (/WEBVIEW2\s+IS\s+VISIBLE/i.test(html)) {
    failures.push(`${label} contains the WebView2 diagnostic overlay`);
  }

  const closingHtml = html.toLowerCase().lastIndexOf('</html>');
  if (closingHtml < 0 || html.slice(closingHtml + '</html>'.length).trim()) {
    failures.push(`${label} contains content outside the document root`);
  }
}

inspectHtml(sourcePath, 'source index.html');
inspectHtml(distPath, 'production dist/index.html');

if (failures.length) {
  console.error('Production HTML validation failed:');
  failures.forEach((failure) => console.error(`- ${failure}`));
  process.exit(1);
}

console.log('Production HTML validation passed.');

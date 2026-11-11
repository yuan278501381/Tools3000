/* eslint-disable react-refresh/only-export-components -- application entrypoint owns stable lazy surface boundaries */
import { StrictMode, Component, Suspense, lazy } from 'react'
import type { ErrorInfo, ReactNode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import i18n from 'i18next'
import './i18n/config'

// 每个 WebView 表面只解析自己的组件树。生产构建会为这些动态入口保留独立
// chunk；WebView2 的虚拟主机映射会从同一 ui 目录安全加载它们。
const SettingsApp = lazy(() => import('./App.tsx'))
const SearchApp = lazy(() => import('./SearchApp.tsx'))
const TrayApp = lazy(() => import('./TrayApp.tsx'))
const QuickLookApp = lazy(() => import('./QuickLookApp.tsx'))

class ErrorBoundary extends Component<{children: ReactNode}, {hasError: boolean, error: Error | null}> {
  constructor(props: {children: ReactNode}) {
    super(props);
    this.state = { hasError: false, error: null };
  }
  static getDerivedStateFromError(error: Error) {
    return { hasError: true, error };
  }
  componentDidCatch(error: Error, errorInfo: ErrorInfo) {
    console.error("React Error:", error, errorInfo);
  }
  render() {
    if (this.state.hasError) {
      return (
        <div role="alert" style={{ maxWidth: '640px', margin: '10vh auto', padding: '24px', color: 'CanvasText', background: 'Canvas', fontFamily: 'var(--font-sans)' }}>
          <h1 style={{ fontSize: '1.4rem' }}>{i18n.t('main.errorTitle', 'UI Failed to Render')}</h1>
          <p style={{ margin: '12px 0' }}>{i18n.t('main.errorDesc', 'Settings will not be lost after reload.')}</p>
          <button type="button" onClick={() => window.location.reload()} style={{ padding: '8px 16px' }}>
            {i18n.t('main.reload', 'Reload')}
          </button>
          <details style={{ marginTop: '16px' }}>
            <summary>{i18n.t('main.techDetails', 'Technical Details')}</summary>
            <pre style={{ marginTop: '8px', whiteSpace: 'pre-wrap', userSelect: 'text' }}>
              {this.state.error?.stack || this.state.error?.message}
            </pre>
          </details>
        </div>
      );
    }
    return this.props.children;
  }
}

const isSearch = window.location.pathname === '/search' || window.location.hash.includes('/search') || window.location.search.includes('search=1');
const isTray = window.location.search.includes('tray=1');
const isQuickLook = window.location.pathname === '/quicklook' || window.location.hash.includes('/quicklook') || window.location.search.includes('quicklook=1');

try {
  const storedTheme = localStorage.getItem('tools3000:theme');
  const prefersDark = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
  const resolvedTheme = storedTheme === 'light' || storedTheme === 'dark'
    ? storedTheme
    : (prefersDark ? 'dark' : 'light');
  document.documentElement.setAttribute('data-theme', resolvedTheme);
  document.documentElement.style.colorScheme = resolvedTheme;

  const initialAccent = localStorage.getItem('tools3000:accent-color') || 'blue';
  document.documentElement.setAttribute('data-accent', initialAccent);
} catch (e) {
  void e;
}

if (isTray) {
  document.documentElement.dataset.surface = 'tray';
} else if (isSearch) {
  document.documentElement.dataset.surface = 'search';
} else if (isQuickLook) {
  document.documentElement.dataset.surface = 'quicklook';
}

window.onerror = function (msg, url, lineNo, columnNo, error) {
  if (document.getElementById('tools3000-global-error')) return;
  const errDiv = document.createElement('div');
  errDiv.id = 'tools3000-global-error';
  errDiv.setAttribute('role', 'alert');
  errDiv.style.cssText = 'position:fixed;inset:0;background:Canvas;color:CanvasText;z-index:9999;padding:32px;font-family:var(--font-sans);white-space:pre-wrap;';
  const message = document.createElement('pre');
  message.textContent = `UI encountered an error, please reload.\n\n${String(msg)}\n${url}:${lineNo}:${columnNo}\n${error?.stack || ''}`;
  message.style.userSelect = 'text';
  const reload = document.createElement('button');
  reload.type = 'button';
  reload.textContent = 'Reload';
  reload.style.cssText = 'margin-top:16px;padding:8px 16px;';
  reload.addEventListener('click', () => window.location.reload());
  errDiv.append(message, reload);
  document.body.appendChild(errDiv);
  reload.focus();
};

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <ErrorBoundary>
      <Suspense fallback={<div role="status" aria-live="polite" className="surface-loading">{i18n.t('main.loading', 'Loading...')}</div>}>
        {isTray ? <TrayApp /> : (isSearch ? <SearchApp /> : (isQuickLook ? <QuickLookApp /> : <SettingsApp />))}
      </Suspense>
    </ErrorBoundary>
  </StrictMode>,
)

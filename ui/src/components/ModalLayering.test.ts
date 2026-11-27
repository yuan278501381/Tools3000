import { describe, expect, it } from 'vitest';

async function loadReadCss(): Promise<(relPath: string) => string> {
  const fsSpecifier = ['node', 'fs'].join(':');
  const fsModule = (await import(/* @vite-ignore */ fsSpecifier)) as {
    readFileSync: (file: string, encoding: string) => string;
  };
  return (relPath: string) => fsModule.readFileSync(`src/${relPath}`, 'utf8');
}

function parseRootZTokens(css: string): Record<string, number> {
  const tokens: Record<string, number> = {};
  const regex = /(--z-[a-z0-9-]+)\s*:\s*(\d+)\s*;/g;
  let match: RegExpExecArray | null;
  while ((match = regex.exec(css)) !== null) {
    tokens[match[1]] = Number(match[2]);
  }
  return tokens;
}

function extractRuleBlock(css: string, selector: string): string {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const regex = new RegExp(`${escaped}\\s*\\{([^}]*)\\}`, 'm');
  const match = regex.exec(css);
  expect(match, `Missing CSS rule block for ${selector}`).not.toBeNull();
  return match![1];
}

function resolveZIndex(ruleBody: string, tokens: Record<string, number>): number {
  const match = /z-index\s*:\s*([^;]+);/.exec(ruleBody);
  expect(match, `Missing z-index declaration in rule: ${ruleBody}`).not.toBeNull();
  const raw = match![1].trim();
  const varMatch = /^var\(\s*(--z-[a-z0-9-]+)\s*(?:,\s*(\d+)\s*)?\)$/.exec(raw);
  if (varMatch) {
    const tokenName = varMatch[1];
    expect(tokens[tokenName], `Undefined z-index token: ${tokenName}`).toBeDefined();
    return tokens[tokenName];
  }
  const numeric = Number(raw);
  expect(Number.isFinite(numeric), `Non-numeric z-index value: ${raw}`).toBe(true);
  return numeric;
}

describe('Global Z-Index Layering & Modal Isolation Guardrails', () => {
  it('defines a monotonic Single-Source-of-Truth z-index token scale in index.css', async () => {
    const readCss = await loadReadCss();
    const tokens = parseRootZTokens(readCss('index.css'));

    expect(tokens['--z-sidebar']).toBeGreaterThan(0);
    expect(tokens['--z-sticky-dock']).toBeGreaterThan(tokens['--z-sidebar']);
    expect(tokens['--z-titlebar']).toBeGreaterThan(tokens['--z-sticky-dock']);
    expect(tokens['--z-dropdown']).toBeGreaterThan(tokens['--z-titlebar']);
    expect(tokens['--z-popover']).toBeGreaterThanOrEqual(tokens['--z-dropdown']);
    expect(tokens['--z-modal']).toBeGreaterThan(tokens['--z-popover']);
    expect(tokens['--z-modal-context']).toBeGreaterThan(tokens['--z-modal']);
    expect(tokens['--z-toast']).toBeGreaterThan(tokens['--z-modal-context']);
    expect(tokens['--z-resize-handles']).toBeGreaterThan(tokens['--z-toast']);
  });

  it('pins WorkspaceWorkbench preview dock with opaque backdrop strictly below TitleBar and modals', async () => {
    const readCss = await loadReadCss();
    const tokens = parseRootZTokens(readCss('index.css'));
    const titlebarZ = resolveZIndex(
      extractRuleBlock(readCss('components/TitleBar.css'), '.titlebar'),
      tokens,
    );
    const stickyDockRule = extractRuleBlock(
      readCss('components/business/WorkspaceWorkbench.css'),
      '.ws-editor-sticky-dock',
    );
    const stickyZ = resolveZIndex(stickyDockRule, tokens);

    expect(stickyDockRule).toMatch(/position\s*:\s*sticky\s*;/);
    expect(stickyDockRule).toMatch(/top\s*:\s*0\s*;/);
    expect(stickyDockRule).toMatch(/background\s*:\s*var\(--bg-base\)\s*;/);
    expect(stickyZ).toBeGreaterThan(50);
    expect(stickyZ).toBeLessThan(titlebarZ);
  });

  it('ensures every full-screen modal backdrop strictly covers TitleBar and blocks drag bleed-through', async () => {
    const readCss = await loadReadCss();
    const tokens = parseRootZTokens(readCss('index.css'));
    const titlebarRule = extractRuleBlock(readCss('components/TitleBar.css'), '.titlebar');
    const titlebarZ = resolveZIndex(titlebarRule, tokens);

    const modals: Array<{ file: string; selector: string }> = [
      { file: 'components/WhatsNewModal.css', selector: '.wn-backdrop' },
      { file: 'components/FeedbackModal.css', selector: '.fb-backdrop' },
      { file: 'components/OnboardingModal.css', selector: '.onboarding' },
      { file: 'components/UIKit.css', selector: '.uikit-modal__overlay' },
      { file: 'styles/search/modals-overlays.css', selector: '.search-modal-overlay' },
    ];

    for (const modal of modals) {
      const rule = extractRuleBlock(readCss(modal.file), modal.selector);
      const modalZ = resolveZIndex(rule, tokens);
      expect(
        modalZ,
        `${modal.file} (${modal.selector}) z-index ${modalZ} must exceed .titlebar (${titlebarZ})`,
      ).toBeGreaterThan(titlebarZ);
      expect(
        rule,
        `${modal.file} (${modal.selector}) must set -webkit-app-region: no-drag to prevent titlebar drag bleed-through`,
      ).toMatch(/-webkit-app-region\s*:\s*no-drag\s*;/);
    }
  });

  it('keeps search context menus above search history dropdowns and view-settings popovers', async () => {
    const readCss = await loadReadCss();
    const tokens = parseRootZTokens(readCss('index.css'));

    const historyZ = resolveZIndex(
      extractRuleBlock(readCss('styles/search/base-header.css'), '.search-history-dropdown'),
      tokens,
    );
    const formatZ = resolveZIndex(
      extractRuleBlock(readCss('styles/search/popover-formats.css'), '.search-view-settings-popover'),
      tokens,
    );
    const catCtxZ = resolveZIndex(
      extractRuleBlock(readCss('styles/search/filters-syntax.css'), '.category-ctx-menu'),
      tokens,
    );
    const catGhostZ = resolveZIndex(
      extractRuleBlock(readCss('styles/search/filters-syntax.css'), '.category-drag-ghost'),
      tokens,
    );
    const searchCtxZ = resolveZIndex(
      extractRuleBlock(readCss('styles/search/modals-overlays.css'), '.search-context-menu'),
      tokens,
    );

    expect(catCtxZ).toBeGreaterThan(Math.max(historyZ, formatZ));
    expect(catGhostZ).toBeGreaterThan(Math.max(historyZ, formatZ));
    expect(searchCtxZ).toBeGreaterThan(Math.max(historyZ, formatZ));
  });

  it('keeps SavedToast and WindowResizeHandles layered above modals', async () => {
    const readCss = await loadReadCss();
    const tokens = parseRootZTokens(readCss('index.css'));

    const toastZ = resolveZIndex(
      extractRuleBlock(readCss('components/SavedToast.css'), '.saved-toast-host'),
      tokens,
    );
    const resizeZ = resolveZIndex(
      extractRuleBlock(readCss('components/WindowResizeHandles.css'), '.window-resize-handles'),
      tokens,
    );
    expect(toastZ).toBeGreaterThan(tokens['--z-modal']);
    expect(resizeZ).toBeGreaterThan(toastZ);
  });
});

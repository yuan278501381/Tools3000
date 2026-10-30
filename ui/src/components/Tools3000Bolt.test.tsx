/* @vitest-environment jsdom */

import { describe, expect, it } from 'vitest';
import { render } from '@testing-library/react';
import { LogoGlyph, Tools3000Bolt, Tools3000Logo, TRibbonLogo } from './Tools3000Bolt';

describe('LogoGlyph & Tools3000Bolt Component', () => {
  it('renders with default props', () => {
    const { container } = render(<LogoGlyph />);
    const svg = container.querySelector('svg');
    expect(svg).toBeTruthy();
    expect(svg?.getAttribute('width')).toBe('24');
    expect(svg?.getAttribute('height')).toBe('24');
    expect(svg?.getAttribute('viewBox')).toBe('0 0 64 64');
    expect(svg?.getAttribute('aria-hidden')).toBe('true');

    // All 3 T-ribbon facet paths should exist and use default fill
    const paths = container.querySelectorAll('path');
    expect(paths.length).toBe(3);
    paths.forEach((p) => {
      expect(p.getAttribute('fill')).toBe('currentColor');
      expect(p.getAttribute('fill-opacity')).toBeTruthy();
    });
  });

  it('supports custom size and theme fill', () => {
    const { container } = render(
      <LogoGlyph size={18} fill="var(--primary)" className="brand-logo" />
    );
    const svg = container.querySelector('svg');
    expect(svg?.getAttribute('width')).toBe('18');
    expect(svg?.getAttribute('height')).toBe('18');
    expect(svg?.classList.contains('brand-logo')).toBe(true);

    const paths = container.querySelectorAll('path');
    expect(paths.length).toBe(3);
    paths.forEach((p) => {
      expect(p.getAttribute('fill')).toBe('var(--primary)');
    });
  });

  it('supports colorVariant branding mode with authentic ribbon colors', () => {
    const { container } = render(<LogoGlyph colorVariant={true} />);
    const paths = container.querySelectorAll('path');
    expect(paths.length).toBe(3);
    expect(paths[0]?.getAttribute('fill')).toBe('#F15B42');
    expect(paths[1]?.getAttribute('fill')).toBe('#FF8B62');
    expect(paths[2]?.getAttribute('fill')).toBe('#D64132');
  });

  it('supports themeAdaptive mode with adaptive className and data attributes', () => {
    const { container } = render(<LogoGlyph themeAdaptive={true} />);
    const svg = container.querySelector('svg');
    expect(svg?.classList.contains('logo-glyph--theme-adaptive')).toBe(true);
    expect(svg?.getAttribute('data-theme-adaptive')).toBe('true');
  });

  it('merges custom styles and preserves flexShrink', () => {
    const { container } = render(
      <LogoGlyph size={32} style={{ opacity: 0.8, transform: 'scale(1.1)' }} />
    );
    const svg = container.querySelector('svg') as unknown as SVGSVGElement;
    expect(svg.style.display).toBe('inline-block');
    expect(svg.style.flexShrink).toBe('0');
    expect(svg.style.opacity).toBe('0.8');
    expect(svg.style.transform).toBe('scale(1.1)');
  });

  it('exports aliases as identical backward-compatible components', () => {
    expect(Tools3000Bolt).toBe(LogoGlyph);
    expect(Tools3000Logo).toBe(LogoGlyph);
    expect(TRibbonLogo).toBe(LogoGlyph);
    const { container } = render(<Tools3000Bolt size={20} fill="#6366f1" />);
    const svg = container.querySelector('svg');
    expect(svg?.getAttribute('width')).toBe('20');
    const firstPath = container.querySelector('path');
    expect(firstPath?.getAttribute('fill')).toBe('#6366f1');
  });
});

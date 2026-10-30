import { describe, expect, it } from 'vitest';
import { DynamicRowLayout, isSelectedOutsideVirtualRange, virtualizedNodeCount } from './searchVirtualization';

describe('search virtualized render window', () => {
  it('keeps a far End selection mounted without rendering the intervening results', () => {
    const range = { start: 0, end: 120 };
    expect(isSelectedOutsideVirtualRange(100_000, 99_999, range)).toBe(true);
    expect(virtualizedNodeCount(100_000, 99_999, range)).toBe(121);
  });

  it('does not duplicate an option already inside the visible window', () => {
    const range = { start: 4_000, end: 4_120 };
    expect(isSelectedOutsideVirtualRange(100_000, 4_050, range)).toBe(false);
    expect(virtualizedNodeCount(100_000, 4_050, range)).toBe(120);
  });

  it('rejects absent selections and clamps an invalid visual range', () => {
    expect(isSelectedOutsideVirtualRange(10, -1, { start: 0, end: 3 })).toBe(false);
    expect(virtualizedNodeCount(10, 9, { start: -20, end: 50 })).toBe(10);
  });

  it('updates dynamic row offsets without rebuilding every prefix', () => {
    const layout = new DynamicRowLayout(100_000, 60);
    expect(layout.totalHeight()).toBe(6_000_000);
    expect(layout.updateHeight(2, 120)).toBe(true);
    expect(layout.updateHeight(90_000, 30)).toBe(true);
    expect(layout.offsetOf(3)).toBe(240);
    expect(layout.offsetOf(90_001)).toBe(5_400_090);
    expect(layout.indexAtOffset(239)).toBe(2);
    expect(layout.indexAtOffset(240)).toBe(3);
    expect(layout.updateHeight(2, 120)).toBe(false);
  });
});

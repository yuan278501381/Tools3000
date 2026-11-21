import { describe, it, expect } from 'vitest';
import {
  buildActivitySeries,
  buildMonotoneAreaPath,
  buildMonotoneCurvePath,
  computeNiceYAxisMax,
  computeVisibleXTickIndices,
  getRequiredDaysForRange,
  summarizeActivitySeries,
  type DailyStat,
} from './keyStatsModel';

const makeStat = (date: string, keystrokes: number, mouseClicks = 0): DailyStat => ({
  date,
  keystrokes,
  mouseClicks,
  mouseDistance: 0,
  keyMap: {},
});

describe('keyStatsModel', () => {
  const fixedNow = new Date(2026, 10, 21, 12, 0, 0); // 2026-11-21

  it('computes accurate required backend days for 7d, month, and year', () => {
    expect(getRequiredDaysForRange('7d', fixedNow)).toBe(7);
    expect(getRequiredDaysForRange('month', fixedNow)).toBe(21);
    expect(getRequiredDaysForRange('year', fixedNow)).toBe(325);
    // 月初第 1 天仍保持至少 7 天以缓存 7 天视图
    expect(getRequiredDaysForRange('month', new Date(2026, 10, 1, 10, 0, 0))).toBe(7);
  });

  it('builds continuous 7d, month, and monthly-aggregated year series', () => {
    const dailyMap: Record<string, DailyStat> = {
      '2026-01-10': makeStat('2026-01-10', 12000, 500),
      '2026-01-11': makeStat('2026-01-11', 8000, 300),
      '2026-11-01': makeStat('2026-11-01', 4000, 120),
      '2026-11-16': makeStat('2026-11-16', 1956, 200),
      '2026-11-21': makeStat('2026-11-21', 5000, 400),
    };

    const s7 = buildActivitySeries(dailyMap, '7d', 'zh-CN', fixedNow);
    expect(s7).toHaveLength(7);
    expect(s7[0].key).toBe('2026-11-15');
    expect(s7[6].key).toBe('2026-11-21');
    expect(s7[1].keys).toBe(1956);

    const sMonth = buildActivitySeries(dailyMap, 'month', 'zh-CN', fixedNow);
    expect(sMonth).toHaveLength(21);
    expect(sMonth[0].key).toBe('2026-11-01');
    expect(sMonth[20].key).toBe('2026-11-21');
    expect(sMonth[0].keys).toBe(4000);

    const sYear = buildActivitySeries(dailyMap, 'year', 'zh-CN', fixedNow);
    expect(sYear).toHaveLength(11);
    expect(sYear[0].axisLabel).toBe('1月');
    expect(sYear[0].keys).toBe(20000);
    expect(sYear[0].activeDays).toBe(2);
    expect(sYear[0].isMonthly).toBe(true);
    expect(sYear[10].axisLabel).toBe('11月');
    expect(sYear[10].keys).toBe(10956);
    expect(sYear[10].daysSpan).toBe(21);

    const summary = summarizeActivitySeries(sYear);
    expect(summary.totalKeys).toBe(30956);
    expect(summary.peakKeys).toBe(20000);
    expect(summary.peakLabel).toBe('1月');
  });

  it('produces valid monotone curve & area paths including single-point edge cases', () => {
    const single = [{ x: 200, y: 80 }];
    expect(buildMonotoneCurvePath(single)).toBe('M 200.00,80.00');
    const singleArea = buildMonotoneAreaPath(single, 210);
    expect(singleArea.endsWith('Z')).toBe(true);
    expect(singleArea).not.toContain('NaN');

    const pts = [
      { x: 50, y: 40 },
      { x: 150, y: 200 },
      { x: 250, y: 200 },
      { x: 350, y: 90 },
    ];
    const curve = buildMonotoneCurvePath(pts);
    const area = buildMonotoneAreaPath(pts, 200);
    expect(curve).toContain('C ');
    expect(area.endsWith('Z')).toBe(true);
    // 水平零值段 (150,200) -> (250,200) 的控制点 Y 绝不下潜超过 200
    expect(curve).toContain('C 183.33,200.00 216.67,200.00 250.00,200.00');
  });

  it('computes clean Y ticks and collision-free X tick indices', () => {
    expect(computeNiceYAxisMax(0)).toBe(100);
    expect(computeNiceYAxisMax(27400)).toBe(30000);

    const ticks = computeVisibleXTickIndices(31, 760, 62);
    expect(ticks.has(0)).toBe(true);
    expect(ticks.has(30)).toBe(true);
    expect(ticks.size).toBeLessThanOrEqual(14);
  });
});

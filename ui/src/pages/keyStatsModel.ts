/* ─────────────────────────────────────────────────────────────────────────────
 * keyStatsModel.ts — 活动趋势纯数据建模与平滑样条曲线数学内核
 *
 * 职责：
 * 1. 计算「近 7 天 / 本月 / 本年」所需后端历史天数；
 * 2. 构建零缺口连续时间序列（7天每日、本月1日至今每日、本年1月至今按月聚合）；
 * 3. Fritsch-Carlson 单调三次贝塞尔样条，确保面积曲线丝滑且绝不下潜穿底；
 * 4. 自适应 Y 轴可读刻度与 X 轴防重叠步长计算。
 * ───────────────────────────────────────────────────────────────────────────── */

export type ActivityRange = '7d' | 'month' | 'year';

export interface DailyStat {
  date: string;
  keystrokes: number;
  mouseClicks: number;
  mouseDistance: number;
  keyMap: Record<number, number>;
}

export interface ActivityPoint {
  key: string;
  axisLabel: string;
  tooltipTitle: string;
  keys: number;
  clicks: number;
  /** 本节点覆盖的自然天数（日粒度为 1，月粒度为该月已过天数） */
  daysSpan: number;
  /** 本节点内按键 > 0 的活跃天数 */
  activeDays: number;
  isMonthly: boolean;
}

export interface ActivitySummary {
  totalKeys: number;
  dailyAvg: number;
  peakKeys: number;
  peakLabel: string;
}

export interface SvgPoint {
  x: number;
  y: number;
}

export const localDateKey = (date = new Date()): string => {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  return `${year}-${month}-${day}`;
};

const MS_PER_DAY = 86_400_000;

/**
 * 计算指定周期需要向后端请求的最小历史天数（兼容后端 1..366 天范围）
 */
export function getRequiredDaysForRange(range: ActivityRange, now = new Date()): number {
  if (range === '7d') return 7;
  if (range === 'month') {
    return Math.max(7, now.getDate());
  }
  const utcToday = Date.UTC(now.getFullYear(), now.getMonth(), now.getDate());
  const utcJan1 = Date.UTC(now.getFullYear(), 0, 1);
  const dayOfYear = Math.floor((utcToday - utcJan1) / MS_PER_DAY) + 1;
  return Math.min(366, Math.max(7, dayOfYear));
}

function formatMonthAxisLabel(year: number, monthZeroBased: number, locale: string): string {
  try {
    return new Intl.DateTimeFormat(locale, { month: 'short' }).format(new Date(year, monthZeroBased, 1));
  } catch {
    return `${monthZeroBased + 1}`;
  }
}

function formatMonthTooltipTitle(year: number, monthZeroBased: number, locale: string): string {
  try {
    return new Intl.DateTimeFormat(locale, { year: 'numeric', month: 'long' }).format(new Date(year, monthZeroBased, 1));
  } catch {
    return `${year}-${String(monthZeroBased + 1).padStart(2, '0')}`;
  }
}

/**
 * 基于按日映射表构建无断点的活动序列
 */
export function buildActivitySeries(
  dailyMap: Record<string, DailyStat>,
  range: ActivityRange,
  locale: string,
  now = new Date(),
): ActivityPoint[] {
  const year = now.getFullYear();
  const month = now.getMonth();
  const day = now.getDate();

  if (range === '7d') {
    const points: ActivityPoint[] = [];
    for (let offset = 6; offset >= 0; offset--) {
      const d = new Date(year, month, day - offset);
      const dateStr = localDateKey(d);
      const entry = dailyMap[dateStr];
      const keys = entry?.keystrokes ?? 0;
      const clicks = entry?.mouseClicks ?? 0;
      points.push({
        key: dateStr,
        axisLabel: `${String(d.getMonth() + 1).padStart(2, '0')}/${String(d.getDate()).padStart(2, '0')}`,
        tooltipTitle: dateStr,
        keys,
        clicks,
        daysSpan: 1,
        activeDays: keys > 0 ? 1 : 0,
        isMonthly: false,
      });
    }
    return points;
  }

  if (range === 'month') {
    const points: ActivityPoint[] = [];
    for (let dNum = 1; dNum <= day; dNum++) {
      const d = new Date(year, month, dNum);
      const dateStr = localDateKey(d);
      const entry = dailyMap[dateStr];
      const keys = entry?.keystrokes ?? 0;
      const clicks = entry?.mouseClicks ?? 0;
      points.push({
        key: dateStr,
        axisLabel: `${d.getMonth() + 1}/${dNum}`,
        tooltipTitle: dateStr,
        keys,
        clicks,
        daysSpan: 1,
        activeDays: keys > 0 ? 1 : 0,
        isMonthly: false,
      });
    }
    return points;
  }

  // range === 'year': 本年 1 月至当前月按自然月聚合
  const points: ActivityPoint[] = [];
  for (let m = 0; m <= month; m++) {
    const daysInThisSlice = m === month ? day : new Date(year, m + 1, 0).getDate();
    let sumKeys = 0;
    let sumClicks = 0;
    let activeDays = 0;
    for (let dNum = 1; dNum <= daysInThisSlice; dNum++) {
      const dateStr = `${year}-${String(m + 1).padStart(2, '0')}-${String(dNum).padStart(2, '0')}`;
      const entry = dailyMap[dateStr];
      if (entry) {
        sumKeys += entry.keystrokes;
        sumClicks += entry.mouseClicks;
        if (entry.keystrokes > 0) activeDays++;
      }
    }
    points.push({
      key: `${year}-${String(m + 1).padStart(2, '0')}`,
      axisLabel: formatMonthAxisLabel(year, m, locale),
      tooltipTitle: formatMonthTooltipTitle(year, m, locale),
      keys: sumKeys,
      clicks: sumClicks,
      daysSpan: daysInThisSlice,
      activeDays,
      isMonthly: true,
    });
  }
  return points;
}

export function summarizeActivitySeries(points: ActivityPoint[]): ActivitySummary {
  if (points.length === 0) {
    return { totalKeys: 0, dailyAvg: 0, peakKeys: 0, peakLabel: '' };
  }
  let totalKeys = 0;
  let totalDays = 0;
  let peakKeys = 0;
  let peakLabel = points[0].axisLabel;
  for (const pt of points) {
    totalKeys += pt.keys;
    totalDays += pt.daysSpan;
    if (pt.keys > peakKeys) {
      peakKeys = pt.keys;
      peakLabel = pt.axisLabel;
    }
  }
  return {
    totalKeys,
    dailyAvg: totalDays > 0 ? Math.round(totalKeys / totalDays) : 0,
    peakKeys,
    peakLabel,
  };
}

/**
 * 计算整洁优雅的 4 等分 Y 轴上限（如 400 / 2,000 / 12,000 / 28,000）
 */
export function computeNiceYAxisMax(rawMax: number): number {
  if (!Number.isFinite(rawMax) || rawMax <= 0) return 100;
  const stepRaw = rawMax / 4;
  const pow10 = 10 ** Math.floor(Math.log10(stepRaw));
  const fraction = stepRaw / pow10;
  let niceFraction: number;
  if (fraction <= 1) niceFraction = 1;
  else if (fraction <= 1.5) niceFraction = 1.5;
  else if (fraction <= 2) niceFraction = 2;
  else if (fraction <= 2.5) niceFraction = 2.5;
  else if (fraction <= 5) niceFraction = 5;
  else if (fraction <= 7.5) niceFraction = 7.5;
  else niceFraction = 10;
  return niceFraction * pow10 * 4;
}

/**
 * 格式化 Y 轴紧凑数值（大数值如本年单月 320,000 自动优雅缩放，避免左侧挤占绘图区）
 */
export function formatAxisNumber(value: number): string {
  const rounded = Math.round(value);
  if (rounded >= 1_000_000) {
    return `${(rounded / 1_000_000).toFixed(rounded % 1_000_000 === 0 ? 0 : 1)}M`;
  }
  if (rounded >= 100_000) {
    return `${(rounded / 1_000).toFixed(0)}k`;
  }
  return rounded.toLocaleString();
}

/**
 * 计算 X 轴防重叠标签显示索引集合（始终保留首尾，中间均匀分布）
 */
export function computeVisibleXTickIndices(count: number, plotWidth: number, minGapPx = 56): Set<number> {
  const visible = new Set<number>();
  if (count <= 0) return visible;
  if (count === 1) {
    visible.add(0);
    return visible;
  }
  const maxLabels = Math.max(2, Math.floor(plotWidth / minGapPx) + 1);
  if (count <= maxLabels) {
    for (let i = 0; i < count; i++) visible.add(i);
    return visible;
  }
  const step = Math.ceil((count - 1) / (maxLabels - 1));
  for (let i = 0; i < count; i += step) {
    visible.add(i);
  }
  // 如果倒数第二个刻度距离末尾太近，移除它以防和最后一个标签重叠
  const lastIdx = count - 1;
  const lastAdded = Math.max(...visible);
  if (lastIdx - lastAdded < Math.max(1, Math.floor(step * 0.55))) {
    visible.delete(lastAdded);
  }
  visible.add(lastIdx);
  return visible;
}

/**
 * Fritsch-Carlson 单调三次贝塞尔样条 (Monotone Cubic Hermite Spline)
 * 保证 C1 平滑连续、极值零过冲、零值基线永不穿底。
 */
export function buildMonotoneCurvePath(points: SvgPoint[]): string {
  const n = points.length;
  if (n === 0) return '';
  if (n === 1) return `M ${points[0].x.toFixed(2)},${points[0].y.toFixed(2)}`;
  if (n === 2) {
    return `M ${points[0].x.toFixed(2)},${points[0].y.toFixed(2)} L ${points[1].x.toFixed(2)},${points[1].y.toFixed(2)}`;
  }

  const dx = new Array<number>(n - 1);
  const dy = new Array<number>(n - 1);
  const slopes = new Array<number>(n - 1);
  for (let i = 0; i < n - 1; i++) {
    dx[i] = Math.max(0.0001, points[i + 1].x - points[i].x);
    dy[i] = points[i + 1].y - points[i].y;
    slopes[i] = dy[i] / dx[i];
  }

  const tangents = new Array<number>(n);
  tangents[0] = slopes[0];
  tangents[n - 1] = slopes[n - 2];

  for (let i = 1; i < n - 1; i++) {
    if (slopes[i - 1] * slopes[i] <= 0) {
      tangents[i] = 0;
    } else {
      const w1 = 2 * dx[i] + dx[i - 1];
      const w2 = dx[i] + 2 * dx[i - 1];
      tangents[i] = (w1 + w2) / (w1 / slopes[i - 1] + w2 / slopes[i]);
    }
  }

  // Fritsch-Carlson 严格单调约束（防任何过冲）
  for (let i = 0; i < n - 1; i++) {
    if (Math.abs(slopes[i]) < 1e-6) {
      tangents[i] = 0;
      tangents[i + 1] = 0;
    } else {
      const a = tangents[i] / slopes[i];
      const b = tangents[i + 1] / slopes[i];
      const h = Math.hypot(a, b);
      if (h > 3) {
        const t = 3 / h;
        tangents[i] = t * a * slopes[i];
        tangents[i + 1] = t * b * slopes[i];
      }
    }
  }

  let d = `M ${points[0].x.toFixed(2)},${points[0].y.toFixed(2)}`;
  for (let i = 0; i < n - 1; i++) {
    const p1 = points[i];
    const p2 = points[i + 1];
    const seg = dx[i] / 3;
    const cp1x = p1.x + seg;
    const cp1y = p1.y + tangents[i] * seg;
    const cp2x = p2.x - seg;
    const cp2y = p2.y - tangents[i + 1] * seg;
    d += ` C ${cp1x.toFixed(2)},${cp1y.toFixed(2)} ${cp2x.toFixed(2)},${cp2y.toFixed(2)} ${p2.x.toFixed(2)},${p2.y.toFixed(2)}`;
  }
  return d;
}

export function buildMonotoneAreaPath(points: SvgPoint[], baselineY: number): string {
  if (points.length === 0) return '';
  if (points.length === 1) {
    // 单点边界（如每月1号 / 每年1月）：渲染柔和钟形微光面积
    const p = points[0];
    const spread = 42;
    const leftX = p.x - spread;
    const rightX = p.x + spread;
    return [
      `M ${leftX.toFixed(2)},${baselineY.toFixed(2)}`,
      `C ${(p.x - spread * 0.45).toFixed(2)},${baselineY.toFixed(2)} ${(p.x - spread * 0.35).toFixed(2)},${p.y.toFixed(2)} ${p.x.toFixed(2)},${p.y.toFixed(2)}`,
      `C ${(p.x + spread * 0.35).toFixed(2)},${p.y.toFixed(2)} ${(p.x + spread * 0.45).toFixed(2)},${baselineY.toFixed(2)} ${rightX.toFixed(2)},${baselineY.toFixed(2)}`,
      'Z',
    ].join(' ');
  }
  const curve = buildMonotoneCurvePath(points);
  const first = points[0];
  const last = points[points.length - 1];
  return `${curve} L ${last.x.toFixed(2)},${baselineY.toFixed(2)} L ${first.x.toFixed(2)},${baselineY.toFixed(2)} Z`;
}

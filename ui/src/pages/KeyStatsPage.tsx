import { useEffect, useState, useMemo, useCallback, type FC } from 'react';
import { Card, SettingGroup, Badge, Button } from '../components/UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { KeyboardHeatmap } from '../components/KeyboardHeatmap';
import { LineChart as LineChartIcon, Flame, Trophy, RefreshCw, RotateCcw } from 'lucide-react';
import { toast } from 'sonner';
import './KeyStatsPage.css';

interface DailyStat {
  date: string;
  keystrokes: number;
  mouseClicks: number;
  mouseDistance: number;
  keyMap: Record<number, number>;
}

interface TotalStat {
  totalKeystrokes: number;
}

interface HistoryStat {
  totalKeys?: number;
  leftClicks?: number;
  rightClicks?: number;
  mouseDistance?: number;
  keyMap?: Record<number, number>;
}

const safeCount = (value: unknown): number =>
  typeof value === 'number' && Number.isFinite(value) && value >= 0 ? value : 0;

const localDateKey = (date = new Date()): string => {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  return `${year}-${month}-${day}`;
};

const normalizeKeyMap = (value: unknown): Record<number, number> => {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return {};

  return Object.fromEntries(
    Object.entries(value)
      .map(([key, count]) => [Number(key), safeCount(count)] as const)
      .filter(([key]) => Number.isInteger(key) && key >= 0 && key <= 255)
  );
};

interface ChartPoint {
  date: string;
  keys: number;
}

interface Point {
  x: number;
  y: number;
}

/**
 * 生成平滑单调三次贝塞尔曲线 SVG 路径 (Monotone Cubic Spline)
 * 达到 C1 级平滑度，同时在水平基准处严防下潜穿底或极值外溢。
 */
function getSmoothCurvePath(points: Point[]): string {
  if (points.length === 0) return '';
  if (points.length === 1) return `M ${points[0].x.toFixed(2)},${points[0].y.toFixed(2)}`;
  if (points.length === 2) {
    return `M ${points[0].x.toFixed(2)},${points[0].y.toFixed(2)} L ${points[1].x.toFixed(2)},${points[1].y.toFixed(2)}`;
  }

  const n = points.length;
  let d = `M ${points[0].x.toFixed(2)},${points[0].y.toFixed(2)}`;

  for (let i = 0; i < n - 1; i++) {
    const p0 = i > 0 ? points[i - 1] : points[i];
    const p1 = points[i];
    const p2 = points[i + 1];
    const p3 = i < n - 2 ? points[i + 2] : p2;

    const tension = 0.22;
    const cp1x = p1.x + (p2.x - p0.x) * tension;
    let cp1y = p1.y + (p2.y - p0.y) * tension;

    const cp2x = p2.x - (p3.x - p1.x) * tension;
    let cp2y = p2.y - (p3.y - p1.y) * tension;

    if (p1.y === p2.y) {
      cp1y = p1.y;
      cp2y = p2.y;
    } else {
      const minY = Math.min(p1.y, p2.y);
      const maxY = Math.max(p1.y, p2.y);
      cp1y = Math.min(Math.max(cp1y, minY), maxY);
      cp2y = Math.min(Math.max(cp2y, minY), maxY);
    }

    d += ` C ${cp1x.toFixed(2)},${cp1y.toFixed(2)} ${cp2x.toFixed(2)},${cp2y.toFixed(2)} ${p2.x.toFixed(2)},${p2.y.toFixed(2)}`;
  }

  return d;
}

function getSmoothAreaPath(points: Point[], baselineY: number): string {
  if (points.length === 0) return '';
  const curve = getSmoothCurvePath(points);
  const first = points[0];
  const last = points[points.length - 1];
  return `${curve} L ${last.x.toFixed(2)},${baselineY.toFixed(2)} L ${first.x.toFixed(2)},${baselineY.toFixed(2)} Z`;
}

const ActivityChart: FC<{ data: ChartPoint[]; label: string }> = ({ data, label }) => {
  const [hoverIndex, setHoverIndex] = useState<number | null>(null);
  const width = 720;
  const height = 260;
  const padding = { top: 22, right: 24, bottom: 36, left: 54 };
  const plotWidth = width - padding.left - padding.right;
  const plotHeight = height - padding.top - padding.bottom;
  const baselineY = padding.top + plotHeight;

  const rawMax = Math.max(1, ...data.map(item => item.keys));
  const magnitude = 10 ** Math.max(0, Math.floor(Math.log10(rawMax)) - 1);
  const yMax = Math.max(magnitude * 4, Math.ceil(rawMax / (magnitude * 4)) * magnitude * 4);

  const yAt = (value: number) => padding.top + plotHeight - (value / yMax) * plotHeight;
  const chartPoints: Point[] = useMemo(() => {
    const xFn = (index: number) => padding.left + (data.length <= 1 ? plotWidth / 2 : (index * plotWidth) / (data.length - 1));
    const yFn = (value: number) => padding.top + plotHeight - (value / yMax) * plotHeight;
    return data.map((item, index) => ({ x: xFn(index), y: yFn(item.keys) }));
  }, [data, yMax, padding.left, padding.top, plotWidth, plotHeight]);

  const smoothCurvePath = useMemo(() => getSmoothCurvePath(chartPoints), [chartPoints]);
  const smoothAreaPath = useMemo(() => getSmoothAreaPath(chartPoints, baselineY), [chartPoints, baselineY]);

  const yTicks = Array.from({ length: 5 }, (_, index) => (yMax * index) / 4);

  const handleMouseMove = (e: React.MouseEvent<SVGSVGElement>) => {
    if (data.length === 0) return;
    const rect = e.currentTarget.getBoundingClientRect();
    const svgX = ((e.clientX - rect.left) / rect.width) * width;

    let closest = 0;
    let minDiff = Infinity;
    chartPoints.forEach((pt, idx) => {
      const diff = Math.abs(pt.x - svgX);
      if (diff < minDiff) {
        minDiff = diff;
        closest = idx;
      }
    });
    setHoverIndex(closest);
  };

  const handleMouseLeave = () => {
    setHoverIndex(null);
  };

  const activePt = hoverIndex !== null ? chartPoints[hoverIndex] : null;
  const activeData = hoverIndex !== null ? data[hoverIndex] : null;

  return (
    <svg
      className="activity-chart"
      viewBox={`0 0 ${width} ${height}`}
      role="img"
      aria-label={label}
      onMouseMove={handleMouseMove}
      onMouseLeave={handleMouseLeave}
    >
      <defs>
        <linearGradient id="activity-area-gradient" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor="var(--primary)" stopOpacity="0.35" />
          <stop offset="50%" stopColor="var(--primary)" stopOpacity="0.10" />
          <stop offset="100%" stopColor="var(--primary)" stopOpacity="0.00" />
        </linearGradient>
      </defs>

      {/* 水平刻度网格线与 Y 轴标签 */}
      {yTicks.map(value => {
        const y = yAt(value);
        return (
          <g key={value}>
            <line className="activity-chart__grid" x1={padding.left} x2={width - padding.right} y1={y} y2={y} />
            <text className="activity-chart__axis" x={padding.left - 12} y={y + 4} textAnchor="end">
              {Math.round(value).toLocaleString()}
            </text>
          </g>
        );
      })}

      {/* 平滑面积渐变 */}
      {smoothAreaPath && <path className="activity-chart__area" d={smoothAreaPath} fill="url(#activity-area-gradient)" />}

      {/* 悬停竖向对齐指示虚线 */}
      {activePt && (
        <line
          className="activity-chart__guide"
          x1={activePt.x}
          x2={activePt.x}
          y1={padding.top}
          y2={baselineY}
        />
      )}

      {/* 平滑主折线 */}
      {smoothCurvePath && <path className="activity-chart__line" d={smoothCurvePath} />}

      {/* 数据节点与 X 轴日期 */}
      {data.map((item, index) => {
        const pt = chartPoints[index];
        const isHovered = hoverIndex === index;
        return (
          <g key={`${item.date}-${index}`} className="activity-chart__node">
            <circle
              className={`activity-chart__point ${isHovered ? 'activity-chart__point--hover' : ''}`}
              cx={pt.x}
              cy={pt.y}
              r={isHovered ? 5.5 : 4}
            />
            <text
              className={`activity-chart__axis ${isHovered ? 'activity-chart__axis--active' : ''}`}
              x={pt.x}
              y={height - 10}
              textAnchor="middle"
            >
              {item.date}
            </text>
          </g>
        );
      })}

      {/* 激活节点的高亮发光环 */}
      {activePt && (
        <g className="activity-chart__active-marker" pointerEvents="none">
          <circle className="activity-chart__point-halo" cx={activePt.x} cy={activePt.y} r="9" />
          <circle className="activity-chart__point-active" cx={activePt.x} cy={activePt.y} r="5" />
        </g>
      )}

      {/* 浮动提示卡片 */}
      {activePt && activeData && (
        <g
          className="activity-chart__tooltip"
          transform={`translate(${Math.max(padding.left + 58, Math.min(width - padding.right - 58, activePt.x))}, ${Math.max(padding.top + 42, activePt.y - 14)})`}
          pointerEvents="none"
        >
          <rect
            className="activity-chart__tooltip-bg"
            x={-56}
            y={-42}
            width={112}
            height={38}
            rx={8}
          />
          <text className="activity-chart__tooltip-date" x={0} y={-25} textAnchor="middle">
            {activeData.date}
          </text>
          <text className="activity-chart__tooltip-value" x={0} y={-10} textAnchor="middle">
            {activeData.keys.toLocaleString()} {label}
          </text>
        </g>
      )}
    </svg>
  );
};

export const KeyStatsPage: FC = () => {
  const [dailyStats, setDailyStats] = useState<DailyStat[]>([]);
  const [totalStats, setTotalStats] = useState<TotalStat>({ totalKeystrokes: 0 });
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [clearing, setClearing] = useState(false);
  const { t } = useTranslation();

  const chartData = useMemo(() => dailyStats.map(s => ({
    date: s.date.split('-').slice(1).join('/'),
    keys: s.keystrokes
  })), [dailyStats]);

  const todayStats = useMemo(() => {
    const today = localDateKey();
    return dailyStats.find(s => s.date === today) || { keystrokes: 0, mouseClicks: 0, mouseDistance: 0, keyMap: {} };
  }, [dailyStats]);

  const totalOverview = useMemo(() => ({
    totalKeystrokes: totalStats.totalKeystrokes
  }), [totalStats]);

  const applyStatsPayload = useCallback((historyMap: Record<string, HistoryStat> | undefined, total: TotalStat | undefined) => {
    const history = historyMap && typeof historyMap === 'object' && !Array.isArray(historyMap)
      ? historyMap
      : {};
    const daily = Object.entries(history).map(([date, data]) => ({
      date,
      keystrokes: safeCount(data?.totalKeys),
      mouseClicks: safeCount(data?.leftClicks) + safeCount(data?.rightClicks),
      mouseDistance: safeCount(data?.mouseDistance),
      keyMap: normalizeKeyMap(data?.keyMap)
    })).sort((a, b) => a.date.localeCompare(b.date));

    setDailyStats(daily);
    setTotalStats({ totalKeystrokes: safeCount(total?.totalKeystrokes) });
  }, []);

  useEffect(() => {
    Promise.all([
      bridgeRequest<Record<string, HistoryStat>>('stats.getHistory', { days: 7 }),
      bridgeRequest<TotalStat>('stats.getTotal')
    ]).then(([historyMap, total]) => {
      applyStatsPayload(historyMap, total);
      setLoading(false);
    }).catch(err => {
      console.error('Failed to load stats:', err);
      setLoading(false);
    });
  }, [applyStatsPayload]);

  const handleRefresh = async () => {
    if (refreshing) return;
    setRefreshing(true);
    try {
      const [historyMap, total] = await Promise.all([
        bridgeRequest<Record<string, HistoryStat>>('stats.getHistory', { days: 7 }),
        bridgeRequest<TotalStat>('stats.getTotal')
      ]);
      applyStatsPayload(historyMap, total);
    } catch (err) {
      console.error('Failed to refresh stats:', err);
    } finally {
      setRefreshing(false);
    }
  };

  const handleClearToday = async () => {
    if (clearing) return;
    if (!window.confirm(t('stats.confirmClearToday'))) return;
    setClearing(true);
    try {
      await bridgeRequest('stats.clearToday');
      const [historyMap, total] = await Promise.all([
        bridgeRequest<Record<string, HistoryStat>>('stats.getHistory', { days: 7 }),
        bridgeRequest<TotalStat>('stats.getTotal')
      ]);
      applyStatsPayload(historyMap, total);
      toast.success(t('stats.clearTodaySuccess'));
    } catch (err) {
      toast.error(t('stats.clearTodayFailed'), { description: String(err) });
    } finally {
      setClearing(false);
    }
  };

  if (loading) {
    return <div style={{ padding: '2rem', opacity: 0.5 }}>{t('common.loading')}</div>;
  }

  return (
    <div className="stats-page" style={{ animation: 'fadeIn 0.3s ease' }}>
      <div className="stats-page__toolbar">
        <Button
          variant="ghost"
          size="sm"
          onClick={() => void handleRefresh()}
          disabled={refreshing || clearing}
          title={t('common.refresh')}
        >
          <RefreshCw size={15} className={refreshing ? 'stats-page__spin' : ''} />
          <span>{t('common.refresh')}</span>
        </Button>
        <Button
          variant="ghost"
          size="sm"
          onClick={() => void handleClearToday()}
          disabled={clearing || refreshing}
          title={t('stats.clearToday')}
        >
          <RotateCcw size={15} />
          <span>{t('stats.clearToday')}</span>
        </Button>
      </div>

      <SettingGroup title={t('stats.dailyTitle')} icon={<LineChartIcon size={20} strokeWidth={2.5} />}>
        <Card>
          <div style={{ height: '300px', width: '100%', marginTop: '16px' }}>
            <ActivityChart data={chartData} label={t('stats.keys')} />
          </div>
        </Card>
      </SettingGroup>

      <SettingGroup title={t('stats.heatmapTitle')} icon={<Flame size={20} strokeWidth={2.5} />}>
        <KeyboardHeatmap keyMap={todayStats.keyMap || {}} />
      </SettingGroup>

      <SettingGroup title={t('stats.totalOverview')} icon={<Trophy size={20} strokeWidth={2.5} />}>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(180px, 1fr))', gap: '16px' }}>
          <Card>
            <div style={{ padding: '16px', textAlign: 'center' }}>
              <div className="stats-page__counter" style={{ fontSize: '2rem', fontWeight: 700, color: 'var(--primary)' }}>
                {totalOverview.totalKeystrokes.toLocaleString()}
              </div>
              <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)', marginTop: '8px' }}>{t('stats.allTimeKeys')}</div>
            </div>
          </Card>
          <Card>
            <div style={{ padding: '16px', textAlign: 'center' }}>
              <div className="stats-page__counter" style={{ fontSize: '2rem', fontWeight: 700, color: '#a78bfa' }}>
                {todayStats.keystrokes.toLocaleString()}
              </div>
              <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)', marginTop: '8px' }}>{t('stats.totalKeys')}</div>
            </div>
          </Card>
          <Card>
            <div style={{ padding: '16px', textAlign: 'center' }}>
              <div className="stats-page__counter" style={{ fontSize: '2rem', fontWeight: 700, color: '#34d399' }}>
                {todayStats.mouseClicks.toLocaleString()}
              </div>
              <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)', marginTop: '8px' }}>{t('stats.mouseClicks')}</div>
            </div>
          </Card>
          <Card>
            <div
              style={{ padding: '16px', textAlign: 'center' }}
              title={`${Math.round(todayStats.mouseDistance).toLocaleString()} ${t('stats.pixels', 'Pixels')}`}
            >
              <div className="stats-page__counter" style={{ fontSize: '2rem', fontWeight: 700, color: '#60a5fa', display: 'flex', alignItems: 'baseline', justifyContent: 'center', gap: '4px' }}>
                <span>{(todayStats.mouseDistance * (0.0254 / 96)).toLocaleString(undefined, { minimumFractionDigits: 1, maximumFractionDigits: 1 })}</span>
                <span style={{ fontSize: '1.1rem', fontWeight: 600, opacity: 0.85 }}>m</span>
              </div>
              <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)', marginTop: '8px' }}>
                {t('stats.mouseDistance')} ({t('stats.meters', 'Meters')})
              </div>
            </div>
          </Card>
        </div>
      </SettingGroup>

      <div style={{ textAlign: 'center', marginTop: '2rem' }}>
        <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>
          {t('stats.allTimeKeysSummary')}: <Badge text={totalStats.totalKeystrokes.toLocaleString() + ' ' + t('stats.times')} variant="primary" />
        </p>
      </div>
    </div>
  );
};

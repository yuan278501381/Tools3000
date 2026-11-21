import { useEffect, useState, useMemo, useCallback, useRef, useId, type FC } from 'react';
import { Card, SettingGroup, Button, ConfirmModal, PageSkeleton } from '../components/UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { KeyboardHeatmap } from '../components/KeyboardHeatmap';
import { LineChart as LineChartIcon, Flame, Trophy, RefreshCw, RotateCcw } from 'lucide-react';
import { toast } from 'sonner';
import {
  type ActivityRange,
  type ActivityPoint,
  type DailyStat,
  type SvgPoint,
  localDateKey,
  getRequiredDaysForRange,
  buildActivitySeries,
  summarizeActivitySeries,
  computeNiceYAxisMax,
  formatAxisNumber,
  computeVisibleXTickIndices,
  buildMonotoneCurvePath,
  buildMonotoneAreaPath,
} from './keyStatsModel';
import './KeyStatsPage.css';

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

const normalizeKeyMap = (value: unknown): Record<number, number> => {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return {};

  return Object.fromEntries(
    Object.entries(value)
      .map(([key, count]) => [Number(key), safeCount(count)] as const)
      .filter(([key]) => Number.isInteger(key) && key >= 0 && key <= 511)
  );
};

interface ActivityChartProps {
  data: ActivityPoint[];
  range: ActivityRange;
  keysLabel: string;
  clicksLabel: string;
  dailyAvgLabel: string;
  activeDaysLabel: string;
  daysUnit: string;
  ariaLabel: string;
}

const CHART_HEIGHT = 248;

const ActivityChart: FC<ActivityChartProps> = ({
  data,
  range,
  keysLabel,
  clicksLabel,
  dailyAvgLabel,
  activeDaysLabel,
  daysUnit,
  ariaLabel,
}) => {
  const stageRef = useRef<HTMLDivElement>(null);
  const [stageWidth, setStageWidth] = useState(860);
  const [hoverIndex, setHoverIndex] = useState<number | null>(null);
  const gradientUid = useId().replace(/:/g, '');

  // 真实物理像素宽度自适应：ResizeObserver 1:1 驱动 SVG 坐标系，杜绝左右大白边与文字拉伸
  useEffect(() => {
    const el = stageRef.current;
    if (!el) return;
    const syncWidth = () => {
      const next = Math.round(el.getBoundingClientRect().width);
      if (next > 120) {
        setStageWidth((prev) => (Math.abs(prev - next) >= 2 ? next : prev));
      }
    };
    syncWidth();
    if (typeof ResizeObserver === 'undefined') return;
    const observer = new ResizeObserver((entries) => {
      const w = Math.round(entries[0]?.contentRect?.width ?? 0);
      if (w > 120) {
        setStageWidth((prev) => (Math.abs(prev - w) >= 2 ? w : prev));
      }
    });
    observer.observe(el);
    return () => observer.disconnect();
  }, []);

  // 切换周期时复位悬停态，避免越界
  const safeHoverIndex = hoverIndex !== null && hoverIndex < data.length ? hoverIndex : null;

  const width = Math.max(320, stageWidth);
  const height = CHART_HEIGHT;
  const padding = { top: 20, right: 18, bottom: 32, left: 54 };
  const plotWidth = Math.max(120, width - padding.left - padding.right);
  const plotHeight = height - padding.top - padding.bottom;
  const baselineY = padding.top + plotHeight;

  const rawMax = Math.max(0, ...data.map((item) => item.keys));
  const yMax = computeNiceYAxisMax(rawMax);
  const yTicks = useMemo(() => Array.from({ length: 5 }, (_, index) => (yMax * index) / 4), [yMax]);

  const yAt = useCallback(
    (value: number) => padding.top + plotHeight - (value / yMax) * plotHeight,
    [padding.top, plotHeight, yMax],
  );

  const chartPoints: SvgPoint[] = useMemo(() => {
    const xFn = (index: number) =>
      padding.left + (data.length <= 1 ? plotWidth / 2 : (index * plotWidth) / (data.length - 1));
    return data.map((item, index) => ({ x: xFn(index), y: yAt(item.keys) }));
  }, [data, padding.left, plotWidth, yAt]);

  const smoothCurvePath = useMemo(() => buildMonotoneCurvePath(chartPoints), [chartPoints]);
  const smoothAreaPath = useMemo(() => buildMonotoneAreaPath(chartPoints, baselineY), [chartPoints, baselineY]);

  const visibleXIndices = useMemo(
    () => computeVisibleXTickIndices(data.length, plotWidth, range === 'month' ? 62 : 54),
    [data.length, plotWidth, range],
  );

  // 月视图点数多（最多31天）时隐藏常态密集圆点，保留丝绸般平滑渐变曲线，悬停即刻点亮锚点
  const showAllStaticDots = data.length <= 12;

  const handleMouseMove = (e: React.MouseEvent<SVGSVGElement>) => {
    if (data.length === 0) return;
    const rect = e.currentTarget.getBoundingClientRect();
    if (rect.width <= 0) return;
    const svgX = ((e.clientX - rect.left) / rect.width) * width;

    let closest = 0;
    let minDiff = Infinity;
    for (let idx = 0; idx < chartPoints.length; idx++) {
      const diff = Math.abs(chartPoints[idx].x - svgX);
      if (diff < minDiff) {
        minDiff = diff;
        closest = idx;
      }
    }
    setHoverIndex(closest);
  };

  const handleMouseLeave = () => {
    setHoverIndex(null);
  };

  const activePt = safeHoverIndex !== null ? chartPoints[safeHoverIndex] : null;
  const activeData = safeHoverIndex !== null ? data[safeHoverIndex] : null;

  const areaGradId = `activity-area-grad-${gradientUid}`;
  const lineGradId = `activity-line-grad-${gradientUid}`;

  // 浮动毛玻璃 Tooltip 边缘安全定位（左/右自动防裁切，高点自动翻转至节点下方）
  const tooltipStyle = useMemo(() => {
    if (!activePt) return undefined;
    const halfW = 92;
    const clampedX = Math.max(padding.left + halfW - 12, Math.min(width - halfW - 6, activePt.x));
    const placeBelow = activePt.y < 84;
    const topPx = placeBelow ? Math.min(baselineY - 12, activePt.y + 16) : Math.max(8, activePt.y - 14);
    return {
      left: `${clampedX}px`,
      top: `${topPx}px`,
      transform: placeBelow ? 'translate(-50%, 0)' : 'translate(-50%, -100%)',
    };
  }, [activePt, baselineY, padding.left, width]);

  return (
    <div className="activity-chart-stage" ref={stageRef}>
      <svg
        className="activity-chart"
        viewBox={`0 0 ${width} ${height}`}
        width={width}
        height={height}
        role="img"
        aria-label={ariaLabel}
        onMouseMove={handleMouseMove}
        onMouseLeave={handleMouseLeave}
      >
        <defs>
          {/* 三段式建筑流光渐变面积填充 */}
          <linearGradient id={areaGradId} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" className="activity-chart__grad-top" />
            <stop offset="48%" className="activity-chart__grad-mid" />
            <stop offset="88%" className="activity-chart__grad-low" />
            <stop offset="100%" className="activity-chart__grad-end" />
          </linearGradient>
          {/* 曲线水平微流光渐变 */}
          <linearGradient id={lineGradId} x1="0" y1="0" x2="1" y2="0">
            <stop offset="0%" stopColor="var(--primary)" stopOpacity="0.88" />
            <stop offset="50%" stopColor="var(--primary)" stopOpacity="1" />
            <stop offset="100%" stopColor="var(--primary-hover)" stopOpacity="0.96" />
          </linearGradient>
        </defs>

        {/* 水平虚线网格与 Y 轴刻度 */}
        {yTicks.map((value, idx) => {
          const y = yAt(value);
          const isBaseline = idx === 0;
          return (
            <g key={value}>
              <line
                className={isBaseline ? 'activity-chart__baseline' : 'activity-chart__grid'}
                x1={padding.left}
                x2={width - padding.right}
                y1={y}
                y2={y}
              />
              <text className="activity-chart__axis" x={padding.left - 12} y={y + 4} textAnchor="end">
                {formatAxisNumber(value)}
              </text>
            </g>
          );
        })}

        {/* 渐变面积主体 */}
        {smoothAreaPath && (
          <path className="activity-chart__area" d={smoothAreaPath} fill={`url(#${areaGradId})`} />
        )}

        {/* 悬停纵向对齐引导线 */}
        {activePt && (
          <line
            className="activity-chart__guide"
            x1={activePt.x}
            x2={activePt.x}
            y1={padding.top}
            y2={baselineY}
          />
        )}

        {/* 单调平滑流光主曲线 */}
        {smoothCurvePath && data.length > 1 && (
          <path className="activity-chart__line" d={smoothCurvePath} stroke={`url(#${lineGradId})`} />
        )}

        {/* 数据节点与 X 轴刻度 */}
        {data.map((item, index) => {
          const pt = chartPoints[index];
          const isHovered = safeHoverIndex === index;
          const showLabel = visibleXIndices.has(index);
          const textAnchor =
            data.length > 1 && index === 0
              ? 'start'
              : data.length > 1 && index === data.length - 1
                ? 'end'
                : 'middle';
          const labelX =
            textAnchor === 'start'
              ? Math.max(padding.left, pt.x - 4)
              : textAnchor === 'end'
                ? Math.min(width - padding.right, pt.x + 4)
                : pt.x;

          return (
            <g key={item.key} className="activity-chart__node">
              {(showAllStaticDots || isHovered) && (
                <circle
                  className={`activity-chart__point ${isHovered ? 'activity-chart__point--hover' : ''}`}
                  cx={pt.x}
                  cy={pt.y}
                  r={isHovered ? 5 : 3.6}
                />
              )}
              {showLabel && (
                <text
                  className={`activity-chart__axis ${isHovered ? 'activity-chart__axis--active' : ''}`}
                  x={labelX}
                  y={height - 8}
                  textAnchor={textAnchor}
                >
                  {item.axisLabel}
                </text>
              )}
            </g>
          );
        })}

        {/* 激活节点的双层脉冲光环 */}
        {activePt && (
          <g className="activity-chart__active-marker" pointerEvents="none">
            <circle className="activity-chart__point-halo" cx={activePt.x} cy={activePt.y} r="10" />
            <circle className="activity-chart__point-active" cx={activePt.x} cy={activePt.y} r="4.8" />
          </g>
        )}
      </svg>

      {/* 雕塑白瓷/曜石毛玻璃悬浮详情卡片 */}
      {activePt && activeData && (
        <div className="activity-chart__popover" style={tooltipStyle} aria-hidden="true">
          <div className="activity-chart__popover-date">{activeData.tooltipTitle}</div>
          <div className="activity-chart__popover-primary">
            <span className="activity-chart__popover-num">{activeData.keys.toLocaleString()}</span>
            <span className="activity-chart__popover-unit">{keysLabel}</span>
          </div>
          <div className="activity-chart__popover-meta">
            {activeData.isMonthly ? (
              <>
                <span>
                  {dailyAvgLabel} {Math.round(activeData.keys / Math.max(1, activeData.daysSpan)).toLocaleString()}
                </span>
                <span className="activity-chart__popover-sep">·</span>
                <span>
                  {activeDaysLabel} {activeData.activeDays}
                  {daysUnit}
                </span>
              </>
            ) : (
              <span>
                {activeData.clicks.toLocaleString()} {clicksLabel}
              </span>
            )}
          </div>
        </div>
      )}
    </div>
  );
};

export const KeyStatsPage: FC = () => {
  const [dailyMap, setDailyMap] = useState<Record<string, DailyStat>>({});
  const [totalStats, setTotalStats] = useState<TotalStat>({ totalKeystrokes: 0 });
  const [range, setRange] = useState<ActivityRange>('7d');
  const [loading, setLoading] = useState(true);
  const [rangeLoading, setRangeLoading] = useState(false);
  const [refreshing, setRefreshing] = useState(false);
  const [clearing, setClearing] = useState(false);
  const [confirmClearOpen, setConfirmClearOpen] = useState(false);
  const loadedDaysRef = useRef(0);
  const { t, i18n } = useTranslation();

  const applyStatsPayload = useCallback(
    (historyMap: Record<string, HistoryStat> | undefined, total: TotalStat | undefined, replaceAll = false) => {
      const history = historyMap && typeof historyMap === 'object' && !Array.isArray(historyMap) ? historyMap : {};
      const nextEntries: Record<string, DailyStat> = {};
      for (const [date, data] of Object.entries(history)) {
        nextEntries[date] = {
          date,
          keystrokes: safeCount(data?.totalKeys),
          mouseClicks: safeCount(data?.leftClicks) + safeCount(data?.rightClicks),
          mouseDistance: safeCount(data?.mouseDistance),
          keyMap: normalizeKeyMap(data?.keyMap),
        };
      }
      setDailyMap((prev) => (replaceAll ? nextEntries : { ...prev, ...nextEntries }));
      if (total !== undefined) {
        setTotalStats({ totalKeystrokes: safeCount(total?.totalKeystrokes) });
      }
    },
    [],
  );

  useEffect(() => {
    const initialDays = getRequiredDaysForRange('7d');
    Promise.all([
      bridgeRequest<Record<string, HistoryStat>>('stats.getHistory', { days: initialDays }),
      bridgeRequest<TotalStat>('stats.getTotal'),
    ])
      .then(([historyMap, total]) => {
        loadedDaysRef.current = Math.max(loadedDaysRef.current, initialDays);
        applyStatsPayload(historyMap, total, true);
        setLoading(false);
      })
      .catch((err) => {
        console.error('Failed to load stats:', err);
        setLoading(false);
      });
  }, [applyStatsPayload]);

  const handleRangeChange = useCallback(
    async (nextRange: ActivityRange) => {
      setRange(nextRange);
      const neededDays = getRequiredDaysForRange(nextRange);
      if (loadedDaysRef.current >= neededDays) return;
      setRangeLoading(true);
      try {
        const historyMap = await bridgeRequest<Record<string, HistoryStat>>('stats.getHistory', { days: neededDays });
        loadedDaysRef.current = Math.max(loadedDaysRef.current, neededDays);
        applyStatsPayload(historyMap, undefined, false);
      } catch (err) {
        console.error('Failed to load activity range:', err);
      } finally {
        setRangeLoading(false);
      }
    },
    [applyStatsPayload],
  );

  const handleRefresh = async () => {
    if (refreshing) return;
    setRefreshing(true);
    try {
      const days = Math.max(loadedDaysRef.current, getRequiredDaysForRange(range));
      const [historyMap, total] = await Promise.all([
        bridgeRequest<Record<string, HistoryStat>>('stats.getHistory', { days }),
        bridgeRequest<TotalStat>('stats.getTotal'),
      ]);
      loadedDaysRef.current = Math.max(loadedDaysRef.current, days);
      applyStatsPayload(historyMap, total, true);
    } catch (err) {
      console.error('Failed to refresh stats:', err);
    } finally {
      setRefreshing(false);
    }
  };

  const handleClearToday = async () => {
    if (clearing) return;
    setConfirmClearOpen(false);
    setClearing(true);
    try {
      await bridgeRequest('stats.clearToday');
      const days = Math.max(loadedDaysRef.current, getRequiredDaysForRange(range));
      const [historyMap, total] = await Promise.all([
        bridgeRequest<Record<string, HistoryStat>>('stats.getHistory', { days }),
        bridgeRequest<TotalStat>('stats.getTotal'),
      ]);
      loadedDaysRef.current = Math.max(loadedDaysRef.current, days);
      applyStatsPayload(historyMap, total, true);
      toast.success(t('stats.clearTodaySuccess'));
    } catch (err) {
      toast.error(t('stats.clearTodayFailed'), { description: String(err) });
    } finally {
      setClearing(false);
    }
  };

  const locale = i18n.resolvedLanguage || i18n.language || 'zh-CN';

  const chartData = useMemo(
    () => buildActivitySeries(dailyMap, range, locale),
    [dailyMap, range, locale],
  );

  const chartSummary = useMemo(() => summarizeActivitySeries(chartData), [chartData]);

  const todayStats = useMemo(() => {
    const today = localDateKey();
    return dailyMap[today] || { keystrokes: 0, mouseClicks: 0, mouseDistance: 0, keyMap: {} };
  }, [dailyMap]);

  const totalOverview = useMemo(
    () => ({ totalKeystrokes: totalStats.totalKeystrokes }),
    [totalStats],
  );

  if (loading) {
    return <PageSkeleton cards={2} rowsPerCard={2} />;
  }

  const rangeOptions: Array<{ id: ActivityRange; label: string }> = [
    { id: '7d', label: t('stats.range7d', '7 Days') },
    { id: 'month', label: t('stats.rangeMonth', 'This Month') },
    { id: 'year', label: t('stats.rangeYear', 'This Year') },
  ];

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
          onClick={() => setConfirmClearOpen(true)}
          disabled={clearing || refreshing}
          title={t('stats.clearToday')}
        >
          <RotateCcw size={15} />
          <span>{t('stats.clearToday')}</span>
        </Button>
      </div>

      <SettingGroup title={t('stats.dailyTitle')} icon={<LineChartIcon size={20} strokeWidth={2.5} />}>
        <Card className="activity-chart-card">
          <div className="activity-chart-card__header">
            <div className="activity-chart-summary">
              <div className="activity-chart-summary__item">
                <span className="activity-chart-summary__label">{t('stats.periodTotal', 'Period Total')}</span>
                <span className="activity-chart-summary__value">
                  {chartSummary.totalKeys.toLocaleString()} {t('stats.keys')}
                </span>
              </div>
              <span className="activity-chart-summary__divider" aria-hidden="true" />
              <div className="activity-chart-summary__item">
                <span className="activity-chart-summary__label">{t('stats.dailyAvg', 'Daily Avg')}</span>
                <span className="activity-chart-summary__value">
                  {chartSummary.dailyAvg.toLocaleString()} {t('stats.keys')}
                </span>
              </div>
              <span className="activity-chart-summary__divider" aria-hidden="true" />
              <div className="activity-chart-summary__item">
                <span className="activity-chart-summary__label">{t('stats.peakValue', 'Peak')}</span>
                <span className="activity-chart-summary__value">
                  {chartSummary.peakKeys.toLocaleString()} {t('stats.keys')}
                </span>
              </div>
            </div>

            <div className="activity-range-switch" role="tablist" aria-label={t('stats.dailyTitle')}>
              {rangeOptions.map((opt) => {
                const active = range === opt.id;
                return (
                  <button
                    key={opt.id}
                    type="button"
                    role="tab"
                    aria-selected={active}
                    className={`activity-range-switch__btn ${active ? 'activity-range-switch__btn--active' : ''}`}
                    onClick={() => void handleRangeChange(opt.id)}
                    disabled={rangeLoading && active}
                  >
                    {opt.label}
                  </button>
                );
              })}
            </div>
          </div>

          <div className={`activity-chart-wrap ${rangeLoading ? 'activity-chart-wrap--loading' : ''}`}>
            <ActivityChart
              data={chartData}
              range={range}
              keysLabel={t('stats.keys')}
              clicksLabel={t('stats.clicks')}
              dailyAvgLabel={t('stats.dailyAvg', 'Daily Avg')}
              activeDaysLabel={t('stats.activeDays', 'Active')}
              daysUnit={t('stats.daysUnit', 'd')}
              ariaLabel={t('stats.dailyTitle')}
            />
          </div>
        </Card>
      </SettingGroup>

      <SettingGroup title={t('stats.heatmapTitle')} icon={<Flame size={20} strokeWidth={2.5} />}>
        <KeyboardHeatmap keyMap={todayStats.keyMap || {}} />
      </SettingGroup>

      <SettingGroup title={t('stats.totalOverview')} icon={<Trophy size={20} strokeWidth={2.5} />}>
        <div className="stats-kpi-grid">
          <Card>
            <div className="stats-kpi-card">
              <div className="stats-kpi-label">
                <span className="stats-kpi-dot stats-kpi-dot--primary" aria-hidden="true" />
                <span>{t('stats.allTimeKeys')}</span>
              </div>
              <div className="stats-page__counter stats-kpi-value">
                {totalOverview.totalKeystrokes.toLocaleString()}
              </div>
            </div>
          </Card>
          <Card>
            <div className="stats-kpi-card">
              <div className="stats-kpi-label">
                <span className="stats-kpi-dot stats-kpi-dot--accent" aria-hidden="true" />
                <span>{t('stats.totalKeys')}</span>
              </div>
              <div className="stats-page__counter stats-kpi-value">
                {todayStats.keystrokes.toLocaleString()}
              </div>
            </div>
          </Card>
          <Card>
            <div className="stats-kpi-card">
              <div className="stats-kpi-label">
                <span className="stats-kpi-dot stats-kpi-dot--success" aria-hidden="true" />
                <span>{t('stats.mouseClicks')}</span>
              </div>
              <div className="stats-page__counter stats-kpi-value">
                {todayStats.mouseClicks.toLocaleString()}
              </div>
            </div>
          </Card>
          <Card>
            <div
              className="stats-kpi-card"
              title={`${Math.round(todayStats.mouseDistance).toLocaleString()} ${t('stats.pixels', 'Pixels')}`}
            >
              <div className="stats-kpi-label">
                <span className="stats-kpi-dot stats-kpi-dot--info" aria-hidden="true" />
                <span>{t('stats.mouseDistance')}</span>
              </div>
              <div className="stats-page__counter stats-kpi-value">
                <span>
                  {(todayStats.mouseDistance * (0.0254 / 96)).toLocaleString(undefined, {
                    minimumFractionDigits: 1,
                    maximumFractionDigits: 1,
                  })}
                </span>
                <span className="stats-kpi-unit">m</span>
              </div>
            </div>
          </Card>
        </div>
      </SettingGroup>

      <ConfirmModal
        open={confirmClearOpen}
        title={t('stats.clearToday')}
        description={t('stats.confirmClearToday')}
        confirmText={t('common.confirm')}
        loading={clearing}
        onConfirm={handleClearToday}
        onClose={() => setConfirmClearOpen(false)}
      />
    </div>
  );
};

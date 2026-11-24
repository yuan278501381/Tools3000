import { useCallback, useEffect, useRef, useState, type FC } from 'react';
import {
  Activity,
  ArrowUpRight,
  Clock,
  Cpu,
  Gauge,
  HardDrive,
  Layers,
  RotateCw,
  ShieldAlert,
  Sparkles,
} from 'lucide-react';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { Badge, Button, Card, CodeBadge, SettingGroup, Toggle } from '../components/UIKit';
import type { NavId } from '../config/routes';
import { emitDeepNavigation } from '../config/settingsSearchIndex';
import { bridgeRequest } from '../hooks/useBridge';
import './PerformancePage.css';

export interface PerformancePageProps {
  onNavigate?: (nav: NavId) => void;
}

interface RawPerfMetrics {
  memoryMB?: number;
  privateMemoryMB?: number;
  cpuPercent?: number;
  privateBytes?: number;
  workingSetBytes?: number;
  cpuUsagePercent?: number;
  screenshotLatencyMs?: number;
  gestureLatencyMs?: number;
  uiRenderLatencyMs?: number;
}

interface NormalizedPerfMetrics {
  privateWsMb: number;
  totalWsMb: number;
  cpuPercent: number;
  screenshotLatencyMs: number;
  gestureLatencyMs: number;
  uiRenderLatencyMs: number;
}

interface LinkedGeneralSettings {
  autoReleaseSettingsMemory?: boolean;
}

interface LinkedSearchSettings {
  memoryMode?: string;
  idleTrimMinutes?: number;
  idleStopMinutes?: number;
}

const BYTES_PER_MB = 1024 * 1024;
const TELEMETRY_POLL_INTERVAL_MS = 1000;
const QUIET_TRIM_REFRESH_DELAY_MS = 350;

const safeFiniteNumber = (val: unknown): number =>
  typeof val === 'number' && Number.isFinite(val) && val >= 0 ? val : 0;

function normalizePerfMetrics(raw: RawPerfMetrics | null | undefined): NormalizedPerfMetrics {
  if (!raw || typeof raw !== 'object') {
    return {
      privateWsMb: 0,
      totalWsMb: 0,
      cpuPercent: 0,
      screenshotLatencyMs: 0,
      gestureLatencyMs: 0,
      uiRenderLatencyMs: 0,
    };
  }

  const totalWsMb =
    safeFiniteNumber(raw.memoryMB) > 0
      ? safeFiniteNumber(raw.memoryMB)
      : safeFiniteNumber(raw.workingSetBytes) / BYTES_PER_MB;

  const privateWsMb =
    safeFiniteNumber(raw.privateMemoryMB) > 0
      ? safeFiniteNumber(raw.privateMemoryMB)
      : safeFiniteNumber(raw.privateBytes) > 0
        ? safeFiniteNumber(raw.privateBytes) / BYTES_PER_MB
        : totalWsMb;

  const cpuPercent =
    typeof raw.cpuPercent === 'number' && Number.isFinite(raw.cpuPercent)
      ? Math.max(0, raw.cpuPercent)
      : safeFiniteNumber(raw.cpuUsagePercent);

  return {
    privateWsMb,
    totalWsMb,
    cpuPercent,
    screenshotLatencyMs: safeFiniteNumber(raw.screenshotLatencyMs),
    gestureLatencyMs: safeFiniteNumber(raw.gestureLatencyMs),
    uiRenderLatencyMs: safeFiniteNumber(raw.uiRenderLatencyMs),
  };
}

export const PerformancePage: FC<PerformancePageProps> = ({ onNavigate }) => {
  const { t } = useTranslation();
  const [liveMonitor, setLiveMonitor] = useState(true);
  const [trimming, setTrimming] = useState(false);
  const [metrics, setMetrics] = useState<NormalizedPerfMetrics>({
    privateWsMb: 0,
    totalWsMb: 0,
    cpuPercent: 0,
    screenshotLatencyMs: 0,
    gestureLatencyMs: 0,
    uiRenderLatencyMs: 0,
  });
  const [autoReleaseSettingsMemory, setAutoReleaseSettingsMemory] = useState<boolean>(true);
  const [searchPolicy, setSearchPolicy] = useState<{
    memoryMode: string;
    idleTrimMinutes: number;
    idleStopMinutes: number;
  }>({
    memoryMode: 'idle_trim',
    idleTrimMinutes: 5,
    idleStopMinutes: 30,
  });

  const trimTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const mountedRef = useRef(true);

  const fetchMetricsOnce = useCallback(async () => {
    try {
      const raw = await bridgeRequest<RawPerfMetrics>('perf.getMetrics', undefined, { silent: true });
      if (mountedRef.current) {
        setMetrics(normalizePerfMetrics(raw));
      }
    } catch {
      // 忽略单次采样静默异常
    }
  }, []);

  useEffect(() => {
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
      if (trimTimerRef.current !== null) {
        clearTimeout(trimTimerRef.current);
        trimTimerRef.current = null;
      }
    };
  }, []);

  // 初始化并行拉取真实关联设置（只读映射）及首帧指标快照
  useEffect(() => {
    let cancelled = false;

    void Promise.allSettled([
      bridgeRequest<RawPerfMetrics>('perf.getMetrics', undefined, { silent: true }),
      bridgeRequest<LinkedGeneralSettings>('general.getSettings', undefined, { silent: true }),
      bridgeRequest<LinkedSearchSettings>('search.getSettings', undefined, { silent: true }),
    ]).then(([perfRes, genRes, searchRes]) => {
      if (cancelled) return;
      if (perfRes.status === 'fulfilled') {
        setMetrics(normalizePerfMetrics(perfRes.value));
      }
      if (genRes.status === 'fulfilled' && genRes.value && typeof genRes.value === 'object') {
        setAutoReleaseSettingsMemory(genRes.value.autoReleaseSettingsMemory !== false);
      }
      if (searchRes.status === 'fulfilled' && searchRes.value && typeof searchRes.value === 'object') {
        setSearchPolicy((prev) => ({
          memoryMode: typeof searchRes.value.memoryMode === 'string' ? searchRes.value.memoryMode : prev.memoryMode,
          idleTrimMinutes:
            typeof searchRes.value.idleTrimMinutes === 'number' && searchRes.value.idleTrimMinutes > 0
              ? searchRes.value.idleTrimMinutes
              : prev.idleTrimMinutes,
          idleStopMinutes:
            typeof searchRes.value.idleStopMinutes === 'number' && searchRes.value.idleStopMinutes > 0
              ? searchRes.value.idleStopMinutes
              : prev.idleStopMinutes,
        }));
      }
    });

    return () => {
      cancelled = true;
    };
  }, []);

  // 实时遥测采样生命周期（离开页面或隐藏窗口自动清除定时器，绝对零后台开销）
  useEffect(() => {
    if (!liveMonitor) return;

    let intervalId: ReturnType<typeof setInterval> | null = null;
    let initialFrame: number | null = null;

    const stopPolling = () => {
      if (initialFrame !== null) {
        cancelAnimationFrame(initialFrame);
        initialFrame = null;
      }
      if (intervalId !== null) {
        clearInterval(intervalId);
        intervalId = null;
      }
    };

    const startPolling = () => {
      if (document.hidden) return;
      initialFrame = requestAnimationFrame(() => {
        initialFrame = null;
        void fetchMetricsOnce();
      });
      if (intervalId === null) {
        intervalId = setInterval(() => {
          if (!document.hidden) {
            void fetchMetricsOnce();
          }
        }, TELEMETRY_POLL_INTERVAL_MS);
      }
    };

    const handleVisibilityChange = () => {
      if (document.hidden) {
        stopPolling();
      } else {
        startPolling();
      }
    };

    startPolling();
    document.addEventListener('visibilitychange', handleVisibilityChange);

    return () => {
      stopPolling();
      document.removeEventListener('visibilitychange', handleVisibilityChange);
    };
  }, [liveMonitor, fetchMetricsOnce]);

  const handleTrimNow = useCallback(async () => {
    if (trimming) return;
    setTrimming(true);
    try {
      await bridgeRequest('perf.trimMemory');
      if (trimTimerRef.current !== null) {
        clearTimeout(trimTimerRef.current);
      }
      trimTimerRef.current = setTimeout(() => {
        trimTimerRef.current = null;
        void fetchMetricsOnce().finally(() => {
          if (mountedRef.current) {
            setTrimming(false);
            toast.success(t('performancePage.trimNowDone'));
          }
        });
      }, QUIET_TRIM_REFRESH_DELAY_MS);
    } catch (error) {
      setTrimming(false);
      toast.error(t('common.saveFailed'), { description: String(error) });
    }
  }, [trimming, fetchMetricsOnce, t]);

  const resolveSearchServiceScheduleText = (): string => {
    const mode = searchPolicy.memoryMode;
    if (mode === 'full_resident' || mode === 'resident' || mode === 'full') {
      return t('performancePage.itemSearchSvcValResident');
    }
    if (mode === 'idle_stop' || mode === 'stop') {
      return t('performancePage.itemSearchSvcValStop', { minutes: searchPolicy.idleStopMinutes });
    }
    return t('performancePage.itemSearchSvcValTrim', { minutes: searchPolicy.idleTrimMinutes });
  };

  const handleJumpToSearchStatus = () => {
    emitDeepNavigation({ navId: 'search', tabId: 'status' });
    onNavigate?.('search');
  };

  return (
    <div className="perf-page">
      {/* Section 1: 实时资源与响应延迟监控 */}
      <SettingGroup title={t('performancePage.telemetryGroup')} icon={<Gauge size={20} strokeWidth={2.2} />}>
        <Card>
          <div className="perf-telemetry-head">
            <Toggle
              id="perf-live-monitor"
              checked={liveMonitor}
              onChange={setLiveMonitor}
              label={t('performancePage.enableLiveMonitor')}
              description={t('performancePage.enableLiveMonitorDesc')}
              className="perf-telemetry-head__toggle"
            />
            <Button
              variant="secondary"
              size="sm"
              disabled={trimming}
              onClick={() => void handleTrimNow()}
              className="perf-telemetry-head__trim-btn"
            >
              {trimming ? (
                <RotateCw size={14} className="perf-spin" aria-hidden="true" />
              ) : (
                <Sparkles size={14} aria-hidden="true" />
              )}
              <span>{t('performancePage.trimNowBtn')}</span>
            </Button>
          </div>

          <div className="perf-telemetry-grid">
            <div className="perf-metric-tile">
              <div className="perf-metric-tile__label">
                <span className="perf-metric-dot perf-metric-dot--primary" aria-hidden="true" />
                <span>{t('performancePage.metricPrivateWs')}</span>
              </div>
              <div className="perf-metric-tile__value">
                <span className="perf-metric-tile__num">{metrics.privateWsMb.toFixed(1)}</span>
                <span className="perf-metric-tile__unit">MB</span>
              </div>
            </div>

            <div className="perf-metric-tile">
              <div className="perf-metric-tile__label">
                <span className="perf-metric-dot perf-metric-dot--info" aria-hidden="true" />
                <span>{t('performancePage.metricTotalWs')}</span>
              </div>
              <div className="perf-metric-tile__value">
                <span className="perf-metric-tile__num">{metrics.totalWsMb.toFixed(1)}</span>
                <span className="perf-metric-tile__unit">MB</span>
              </div>
            </div>

            <div className="perf-metric-tile">
              <div className="perf-metric-tile__label">
                <span className="perf-metric-dot perf-metric-dot--success" aria-hidden="true" />
                <span>{t('performancePage.metricCpu')}</span>
              </div>
              <div className="perf-metric-tile__value">
                <span className="perf-metric-tile__num">{metrics.cpuPercent.toFixed(1)}</span>
                <span className="perf-metric-tile__unit">%</span>
              </div>
            </div>

            <div className="perf-metric-tile">
              <div className="perf-metric-tile__label">
                <Activity size={13} className="perf-metric-icon" aria-hidden="true" />
                <span>{t('performancePage.metricCaptureLatency')}</span>
              </div>
              <div className="perf-metric-tile__value">
                {metrics.screenshotLatencyMs <= 0 ? (
                  <span className="perf-metric-tile__idle">{t('performancePage.notTriggeredYet')}</span>
                ) : (
                  <>
                    <span className="perf-metric-tile__num">{metrics.screenshotLatencyMs.toFixed(1)}</span>
                    <span className="perf-metric-tile__unit">ms</span>
                  </>
                )}
              </div>
            </div>

            <div className="perf-metric-tile">
              <div className="perf-metric-tile__label">
                <Clock size={13} className="perf-metric-icon" aria-hidden="true" />
                <span>{t('performancePage.metricGestureLatency')}</span>
              </div>
              <div className="perf-metric-tile__value">
                <span className="perf-metric-tile__num">{metrics.gestureLatencyMs.toFixed(1)}</span>
                <span className="perf-metric-tile__unit">ms</span>
              </div>
            </div>

            <div className="perf-metric-tile">
              <div className="perf-metric-tile__label">
                <Layers size={13} className="perf-metric-icon" aria-hidden="true" />
                <span>{t('performancePage.metricUiLatency')}</span>
              </div>
              <div className="perf-metric-tile__value">
                <span className="perf-metric-tile__num">{metrics.uiRenderLatencyMs.toFixed(1)}</span>
                <span className="perf-metric-tile__unit">ms</span>
              </div>
            </div>
          </div>
        </Card>
      </SettingGroup>

      {/* Section 2: 全链路内存生命周期与自动回收时间表（只读透明看板） */}
      <SettingGroup title={t('performancePage.lifecycleGroup')} icon={<Cpu size={20} strokeWidth={2.2} />}>
        <Card
          subtitle={t('performancePage.lifecycleSubtitle')}
          headerAction={<Badge text={t('performancePage.badgeReadonly')} variant="muted" />}
        >
          <div className="perf-lifecycle">
            {/* Layer 1: WebView2 UI Host Surfaces */}
            <div className="perf-layer">
              <div className="perf-layer__header">
                <Layers size={14} className="perf-layer__icon" aria-hidden="true" />
                <h3 className="perf-layer__title">{t('performancePage.layerWebView')}</h3>
              </div>
              <div className="perf-layer__list">
                <div className="perf-schedule-row">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemSettingsTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemSettingsDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <Badge text={t('performancePage.badgeLinkedSetting')} variant="primary" />
                    <CodeBadge>
                      {autoReleaseSettingsMemory
                        ? t('performancePage.itemSettingsValEnabled')
                        : t('performancePage.itemSettingsValDisabled')}
                    </CodeBadge>
                    {onNavigate && (
                      <Button
                        variant="ghost"
                        size="sm"
                        className="perf-schedule-row__jump"
                        onClick={() => onNavigate('general')}
                      >
                        <span>{t('performancePage.jumpToSetting')}</span>
                        <ArrowUpRight size={13} aria-hidden="true" />
                      </Button>
                    )}
                  </div>
                </div>

                <div className="perf-schedule-row">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemClipboardTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemClipboardDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <CodeBadge>{t('performancePage.itemClipboardVal')}</CodeBadge>
                  </div>
                </div>

                <div className="perf-schedule-row">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemQuickLookTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemQuickLookDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <CodeBadge>{t('performancePage.itemQuickLookVal')}</CodeBadge>
                  </div>
                </div>

                <div className="perf-schedule-row">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemSearchWndTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemSearchWndDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <CodeBadge>{t('performancePage.itemSearchWndVal')}</CodeBadge>
                  </div>
                </div>
              </div>
            </div>

            {/* Layer 2: Offline Vision & Deep Learning Engines */}
            <div className="perf-layer">
              <div className="perf-layer__header">
                <Cpu size={14} className="perf-layer__icon" aria-hidden="true" />
                <h3 className="perf-layer__title">{t('performancePage.layerAiModels')}</h3>
              </div>
              <div className="perf-layer__list">
                <div className="perf-schedule-row">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemOcrWorkerTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemOcrWorkerDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <CodeBadge>{t('performancePage.itemOcrWorkerVal')}</CodeBadge>
                  </div>
                </div>

                <div className="perf-schedule-row">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemDeepOcrTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemDeepOcrDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <CodeBadge>{t('performancePage.itemDeepOcrVal')}</CodeBadge>
                  </div>
                </div>

                <div className="perf-schedule-row">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemMiganTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemMiganDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <CodeBadge>{t('performancePage.itemMiganVal')}</CodeBadge>
                  </div>
                </div>
              </div>
            </div>

            {/* Layer 3: Instant File Index Service */}
            <div className="perf-layer">
              <div className="perf-layer__header">
                <HardDrive size={14} className="perf-layer__icon" aria-hidden="true" />
                <h3 className="perf-layer__title">{t('performancePage.layerSearchService')}</h3>
              </div>
              <div className="perf-layer__list">
                <div className="perf-schedule-row">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemSearchSvcTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemSearchSvcDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <Badge text={t('performancePage.badgeLinkedSetting')} variant="primary" />
                    <CodeBadge>{resolveSearchServiceScheduleText()}</CodeBadge>
                    {onNavigate && (
                      <Button
                        variant="ghost"
                        size="sm"
                        className="perf-schedule-row__jump"
                        onClick={handleJumpToSearchStatus}
                      >
                        <span>{t('performancePage.jumpToSetting')}</span>
                        <ArrowUpRight size={13} aria-hidden="true" />
                      </Button>
                    )}
                  </div>
                </div>
              </div>
            </div>

            {/* Layer 4: GPU Composition Surfaces & Kernel Anti-Thrash Guards */}
            <div className="perf-layer">
              <div className="perf-layer__header">
                <ShieldAlert size={14} className="perf-layer__icon" aria-hidden="true" />
                <h3 className="perf-layer__title">{t('performancePage.layerKernelGuard')}</h3>
              </div>
              <div className="perf-layer__list">
                <div className="perf-schedule-row">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemMouseDcompTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemMouseDcompDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <CodeBadge>{t('performancePage.itemMouseDcompVal')}</CodeBadge>
                  </div>
                </div>

                <div className="perf-schedule-row perf-schedule-row--kernel-guard">
                  <div className="perf-schedule-row__info">
                    <span className="perf-schedule-row__title">{t('performancePage.itemHeapGuardTitle')}</span>
                    <span className="perf-schedule-row__desc">{t('performancePage.itemHeapGuardDesc')}</span>
                  </div>
                  <div className="perf-schedule-row__meta">
                    <Badge text={t('performancePage.badgeKernelLocked')} variant="warning" />
                    <CodeBadge>{t('performancePage.itemHeapGuardVal')}</CodeBadge>
                  </div>
                </div>
              </div>
            </div>
          </div>
        </Card>
      </SettingGroup>
    </div>
  );
};

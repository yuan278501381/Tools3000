import { useEffect, useMemo, useState, type FC, type ReactNode } from 'react';
import {
  ArrowUpRight,
  Cpu,
  Gauge,
  HardDrive,
  Image as ImageIcon,
  Layers,
  Power,
  ShieldAlert,
  ShieldCheck,
  Sparkles,
} from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { Badge, Button, Card, CodeBadge, SettingGroup } from '../components/UIKit';
import type { NavId } from '../config/routes';
import { emitDeepNavigation, type AnyPageSubTab } from '../config/settingsSearchIndex';
import { bridgeRequest } from '../hooks/useBridge';
import './PerformancePage.css';

export interface PerformancePageProps {
  onNavigate?: (nav: NavId) => void;
}

type PolicyMechanism =
  | 'on_demand'
  | 'budget_cap'
  | 'instant_release'
  | 'timed_reclaim'
  | 'kernel_guard';

type FilterMechanism = 'all' | PolicyMechanism;

interface LinkedGeneralSettings {
  autoReleaseSettingsMemory?: boolean;
}

interface LinkedSearchSettings {
  memoryMode?: string;
  idleTrimMinutes?: number;
  idleStopMinutes?: number;
}

interface LinkedWindowManagerSettings {
  enabled?: boolean;
  running?: boolean;
  settings?: {
    features?: Record<string, boolean>;
  };
}

interface MemoryPolicyItem {
  id: string;
  title: string;
  desc: string;
  value: string;
  mechanism: PolicyMechanism;
  linkedSetting?: boolean;
  kernelLocked?: boolean;
  jumpTarget?: {
    navId: NavId;
    tabId?: AnyPageSubTab;
  };
}

interface MemoryPolicyLayer {
  id: string;
  title: string;
  icon: ReactNode;
  items: MemoryPolicyItem[];
}

export const PerformancePage: FC<PerformancePageProps> = ({ onNavigate }) => {
  const { t } = useTranslation();
  const [activeFilter, setActiveFilter] = useState<FilterMechanism>('all');
  const [autoReleaseSettingsMemory, setAutoReleaseSettingsMemory] = useState<boolean>(true);
  const [searchPolicy, setSearchPolicy] = useState<{
    memoryMode: string;
    idleTrimMinutes: number;
    idleStopMinutes: number;
  }>({
    memoryMode: 'idle_trim',
    idleTrimMinutes: 2,
    idleStopMinutes: 30,
  });
  const [windowKitActive, setWindowKitActive] = useState<boolean>(false);

  // 初始化并行拉取真实关联设置（纯只读映射至全景内存治理策略，零后台轮询）
  useEffect(() => {
    let cancelled = false;

    void Promise.allSettled([
      bridgeRequest<LinkedGeneralSettings>('general.getSettings', undefined, { silent: true }),
      bridgeRequest<LinkedSearchSettings>('search.getSettings', undefined, { silent: true }),
      bridgeRequest<LinkedWindowManagerSettings>('windowManager.getSettings', undefined, { silent: true }),
    ]).then(([genRes, searchRes, wmRes]) => {
      if (cancelled) return;
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
      if (wmRes.status === 'fulfilled' && wmRes.value && typeof wmRes.value === 'object') {
        const enabled = Boolean(wmRes.value.enabled);
        const running = Boolean(wmRes.value.running);
        const featureValues = wmRes.value.settings?.features
          ? Object.values(wmRes.value.settings.features)
          : [];
        const hasAnyFeature = featureValues.some(Boolean);
        setWindowKitActive(running || (enabled && hasAnyFeature));
      }
    });

    return () => {
      cancelled = true;
    };
  }, []);

  const searchServiceScheduleText = useMemo((): string => {
    const mode = searchPolicy.memoryMode;
    if (mode === 'full_resident' || mode === 'resident' || mode === 'full') {
      return t('performancePage.itemSearchSvcValResident');
    }
    if (mode === 'idle_stop' || mode === 'stop') {
      return t('performancePage.itemSearchSvcValStop', { minutes: searchPolicy.idleStopMinutes });
    }
    return t('performancePage.itemSearchSvcValTrim', { minutes: searchPolicy.idleTrimMinutes });
  }, [searchPolicy.idleStopMinutes, searchPolicy.idleTrimMinutes, searchPolicy.memoryMode, t]);

  const handleJump = (target: { navId: NavId; tabId?: AnyPageSubTab }) => {
    if (target.tabId) {
      emitDeepNavigation({ navId: target.navId, tabId: target.tabId });
    }
    onNavigate?.(target.navId);
  };

  const layers = useMemo<MemoryPolicyLayer[]>(() => [
    {
      id: 'isolation',
      title: t('performancePage.layerIsolation'),
      icon: <HardDrive size={14} className="perf-layer__icon" aria-hidden="true" />,
      items: [
        {
          id: 'search-svc',
          title: t('performancePage.itemSearchSvcTitle'),
          desc: t('performancePage.itemSearchSvcDesc'),
          value: searchServiceScheduleText,
          mechanism: 'on_demand',
          linkedSetting: true,
          jumpTarget: { navId: 'search', tabId: 'status' },
        },
        {
          id: 'window-kit',
          title: t('performancePage.itemWindowKitTitle'),
          desc: t('performancePage.itemWindowKitDesc'),
          value: windowKitActive
            ? t('performancePage.itemWindowKitValActive')
            : t('performancePage.itemWindowKitValOff'),
          mechanism: 'on_demand',
          linkedSetting: true,
          jumpTarget: { navId: 'window_manager' },
        },
        {
          id: 'short-workers',
          title: t('performancePage.itemShortWorkersTitle'),
          desc: t('performancePage.itemShortWorkersDesc'),
          value: t('performancePage.itemShortWorkersVal'),
          mechanism: 'instant_release',
        },
        {
          id: 'modular-plugins',
          title: t('performancePage.itemModularPluginsTitle'),
          desc: t('performancePage.itemModularPluginsDesc'),
          value: t('performancePage.itemModularPluginsVal'),
          mechanism: 'on_demand',
          jumpTarget: { navId: 'plugins' },
        },
      ],
    },
    {
      id: 'media-budget',
      title: t('performancePage.layerMediaBudget'),
      icon: <ImageIcon size={14} className="perf-layer__icon" aria-hidden="true" />,
      items: [
        {
          id: 'pin-pixel-pool',
          title: t('performancePage.itemPinPixelPoolTitle'),
          desc: t('performancePage.itemPinPixelPoolDesc'),
          value: t('performancePage.itemPinPixelPoolVal'),
          mechanism: 'budget_cap',
          jumpTarget: { navId: 'capture', tabId: 'pin_region' },
        },
        {
          id: 'pin-ocr-queue',
          title: t('performancePage.itemPinOcrQueueTitle'),
          desc: t('performancePage.itemPinOcrQueueDesc'),
          value: t('performancePage.itemPinOcrQueueVal'),
          mechanism: 'budget_cap',
          jumpTarget: { navId: 'capture', tabId: 'pin_region' },
        },
        {
          id: 'capture-history',
          title: t('performancePage.itemCaptureHistoryTitle'),
          desc: t('performancePage.itemCaptureHistoryDesc'),
          value: t('performancePage.itemCaptureHistoryVal'),
          mechanism: 'budget_cap',
          jumpTarget: { navId: 'history' },
        },
        {
          id: 'clipboard-store',
          title: t('performancePage.itemClipboardStoreTitle'),
          desc: t('performancePage.itemClipboardStoreDesc'),
          value: t('performancePage.itemClipboardStoreVal'),
          mechanism: 'budget_cap',
          jumpTarget: { navId: 'clipboard' },
        },
      ],
    },
    {
      id: 'webview',
      title: t('performancePage.layerWebView'),
      icon: <Layers size={14} className="perf-layer__icon" aria-hidden="true" />,
      items: [
        {
          id: 'settings-wnd',
          title: t('performancePage.itemSettingsTitle'),
          desc: t('performancePage.itemSettingsDesc'),
          value: autoReleaseSettingsMemory
            ? t('performancePage.itemSettingsValEnabled')
            : t('performancePage.itemSettingsValDisabled'),
          mechanism: 'timed_reclaim',
          linkedSetting: true,
          jumpTarget: { navId: 'general' },
        },
        {
          id: 'clipboard-wnd',
          title: t('performancePage.itemClipboardTitle'),
          desc: t('performancePage.itemClipboardDesc'),
          value: t('performancePage.itemClipboardVal'),
          mechanism: 'timed_reclaim',
          jumpTarget: { navId: 'clipboard' },
        },
        {
          id: 'quicklook-wnd',
          title: t('performancePage.itemQuickLookTitle'),
          desc: t('performancePage.itemQuickLookDesc'),
          value: t('performancePage.itemQuickLookVal'),
          mechanism: 'timed_reclaim',
          jumpTarget: { navId: 'plugins' },
        },
        {
          id: 'search-wnd',
          title: t('performancePage.itemSearchWndTitle'),
          desc: t('performancePage.itemSearchWndDesc'),
          value: t('performancePage.itemSearchWndVal'),
          mechanism: 'timed_reclaim',
        },
      ],
    },
    {
      id: 'ai-models',
      title: t('performancePage.layerAiModels'),
      icon: <Cpu size={14} className="perf-layer__icon" aria-hidden="true" />,
      items: [
        {
          id: 'ocr-worker',
          title: t('performancePage.itemOcrWorkerTitle'),
          desc: t('performancePage.itemOcrWorkerDesc'),
          value: t('performancePage.itemOcrWorkerVal'),
          mechanism: 'timed_reclaim',
          jumpTarget: { navId: 'ocr', tabId: 'models' },
        },
        {
          id: 'deep-ocr',
          title: t('performancePage.itemDeepOcrTitle'),
          desc: t('performancePage.itemDeepOcrDesc'),
          value: t('performancePage.itemDeepOcrVal'),
          mechanism: 'timed_reclaim',
        },
        {
          id: 'migan',
          title: t('performancePage.itemMiganTitle'),
          desc: t('performancePage.itemMiganDesc'),
          value: t('performancePage.itemMiganVal'),
          mechanism: 'timed_reclaim',
        },
      ],
    },
    {
      id: 'gpu-overlays',
      title: t('performancePage.layerGpuOverlays'),
      icon: <Sparkles size={14} className="perf-layer__icon" aria-hidden="true" />,
      items: [
        {
          id: 'gesture-trail',
          title: t('performancePage.itemGestureTrailTitle'),
          desc: t('performancePage.itemGestureTrailDesc'),
          value: t('performancePage.itemGestureTrailVal'),
          mechanism: 'timed_reclaim',
          jumpTarget: { navId: 'gesture', tabId: 'appearance' },
        },
        {
          id: 'mouse-dcomp',
          title: t('performancePage.itemMouseDcompTitle'),
          desc: t('performancePage.itemMouseDcompDesc'),
          value: t('performancePage.itemMouseDcompVal'),
          mechanism: 'timed_reclaim',
          jumpTarget: { navId: 'spotlight' },
        },
        {
          id: 'instant-overlays',
          title: t('performancePage.itemInstantOverlaysTitle'),
          desc: t('performancePage.itemInstantOverlaysDesc'),
          value: t('performancePage.itemInstantOverlaysVal'),
          mechanism: 'instant_release',
        },
      ],
    },
    {
      id: 'kernel-guard',
      title: t('performancePage.layerKernelGuard'),
      icon: <ShieldAlert size={14} className="perf-layer__icon" aria-hidden="true" />,
      items: [
        {
          id: 'watermark-guard',
          title: t('performancePage.itemWatermarkGuardTitle'),
          desc: t('performancePage.itemWatermarkGuardDesc'),
          value: t('performancePage.itemWatermarkGuardVal'),
          mechanism: 'kernel_guard',
          kernelLocked: true,
        },
        {
          id: 'heap-guard',
          title: t('performancePage.itemHeapGuardTitle'),
          desc: t('performancePage.itemHeapGuardDesc'),
          value: t('performancePage.itemHeapGuardVal'),
          mechanism: 'kernel_guard',
          kernelLocked: true,
        },
      ],
    },
  ], [autoReleaseSettingsMemory, searchServiceScheduleText, t, windowKitActive]);

  const mechanismCounts = useMemo(() => {
    const counts: Record<FilterMechanism, number> = {
      all: 0,
      on_demand: 0,
      budget_cap: 0,
      instant_release: 0,
      timed_reclaim: 0,
      kernel_guard: 0,
    };
    for (const layer of layers) {
      for (const item of layer.items) {
        counts.all += 1;
        counts[item.mechanism] += 1;
      }
    }
    return counts;
  }, [layers]);

  const mechanismLabels = useMemo<Record<PolicyMechanism, string>>(() => ({
    on_demand: t('performancePage.mechOnDemand'),
    budget_cap: t('performancePage.mechBudgetCap'),
    instant_release: t('performancePage.mechInstantRelease'),
    timed_reclaim: t('performancePage.mechTimedReclaim'),
    kernel_guard: t('performancePage.mechKernelGuard'),
  }), [t]);

  const filterOptions = useMemo<Array<{ id: FilterMechanism; label: string }>>(() => [
    { id: 'all', label: t('performancePage.filterAll') },
    { id: 'on_demand', label: t('performancePage.mechOnDemand') },
    { id: 'budget_cap', label: t('performancePage.mechBudgetCap') },
    { id: 'instant_release', label: t('performancePage.mechInstantRelease') },
    { id: 'timed_reclaim', label: t('performancePage.mechTimedReclaim') },
    { id: 'kernel_guard', label: t('performancePage.mechKernelGuard') },
  ], [t]);

  const visibleLayers = useMemo(() => {
    if (activeFilter === 'all') return layers;
    return layers
      .map((layer) => ({
        ...layer,
        items: layer.items.filter((item) => item.mechanism === activeFilter),
      }))
      .filter((layer) => layer.items.length > 0);
  }, [activeFilter, layers]);

  const pillars = useMemo(() => [
    {
      id: 'zero-idle',
      icon: <Power size={15} className="perf-pillar__icon" aria-hidden="true" />,
      title: t('performancePage.pillarZeroIdleTitle'),
      desc: t('performancePage.pillarZeroIdleDesc'),
    },
    {
      id: 'bounded-peak',
      icon: <Gauge size={15} className="perf-pillar__icon" aria-hidden="true" />,
      title: t('performancePage.pillarBoundedPeakTitle'),
      desc: t('performancePage.pillarBoundedPeakDesc'),
    },
    {
      id: 'cold-path',
      icon: <ShieldCheck size={15} className="perf-pillar__icon" aria-hidden="true" />,
      title: t('performancePage.pillarColdPathTitle'),
      desc: t('performancePage.pillarColdPathDesc'),
    },
  ], [t]);

  return (
    <div className="perf-page">
      {/* 全链路内存治理与透明预期总览（纯只读透明看板，零后台轮询） */}
      <SettingGroup title={t('performancePage.lifecycleGroup')} icon={<Cpu size={20} strokeWidth={2.2} />}>
        <Card
          subtitle={t('performancePage.lifecycleSubtitle')}
          headerAction={<Badge text={t('performancePage.badgeReadonly')} variant="muted" />}
        >
          <div className="perf-lifecycle">
            {/* 顶部 3 项核心内存预期摘要 */}
            <div className="perf-pillars" aria-label={t('performancePage.pillarsAria')}>
              {pillars.map((pillar) => (
                <div key={pillar.id} className="perf-pillar">
                  <div className="perf-pillar__head">
                    {pillar.icon}
                    <span className="perf-pillar__title">{pillar.title}</span>
                  </div>
                  <p className="perf-pillar__desc">{pillar.desc}</p>
                </div>
              ))}
            </div>

            {/* 策略机制筛选药丸条 */}
            <div
              className="perf-filter-bar"
              role="tablist"
              aria-label={t('performancePage.filterAria')}
            >
              {filterOptions.map((option) => {
                const selected = activeFilter === option.id;
                return (
                  <button
                    key={option.id}
                    type="button"
                    role="tab"
                    aria-selected={selected}
                    className={`perf-filter-pill ${selected ? 'perf-filter-pill--active' : ''}`}
                    onClick={() => setActiveFilter(option.id)}
                  >
                    <span>{option.label}</span>
                    <span className="perf-filter-pill__count">{mechanismCounts[option.id]}</span>
                  </button>
                );
              })}
            </div>

            {/* 6 大架构层级全透明内存治理清单 */}
            {visibleLayers.map((layer) => (
              <div key={layer.id} className="perf-layer">
                <div className="perf-layer__header">
                  {layer.icon}
                  <h3 className="perf-layer__title">{layer.title}</h3>
                  <span className="perf-layer__count">{layer.items.length}</span>
                </div>
                <div className="perf-layer__list">
                  {layer.items.map((item) => (
                    <div
                      key={item.id}
                      className={`perf-schedule-row ${
                        item.kernelLocked ? 'perf-schedule-row--kernel-guard' : ''
                      }`}
                    >
                      <div className="perf-schedule-row__info">
                        <div className="perf-schedule-row__head">
                          <span className="perf-schedule-row__title">{item.title}</span>
                          <span
                            className={`perf-mech-tag perf-mech-tag--${item.mechanism}`}
                          >
                            {mechanismLabels[item.mechanism]}
                          </span>
                        </div>
                        <span className="perf-schedule-row__desc">{item.desc}</span>
                      </div>
                      <div className="perf-schedule-row__meta">
                        {item.linkedSetting && (
                          <Badge text={t('performancePage.badgeLinkedSetting')} variant="primary" />
                        )}
                        {item.kernelLocked && (
                          <Badge text={t('performancePage.badgeKernelLocked')} variant="warning" />
                        )}
                        <CodeBadge>{item.value}</CodeBadge>
                        {onNavigate && item.jumpTarget && (
                          <Button
                            variant="ghost"
                            size="sm"
                            className="perf-schedule-row__jump"
                            onClick={() => handleJump(item.jumpTarget!)}
                          >
                            <span>{t('performancePage.jumpToSetting')}</span>
                            <ArrowUpRight size={13} aria-hidden="true" />
                          </Button>
                        )}
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        </Card>
      </SettingGroup>
    </div>
  );
};

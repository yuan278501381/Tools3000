import { useEffect, useState, type FC } from 'react';
import {
  ArrowUpRight,
  Cpu,
  HardDrive,
  Layers,
  ShieldAlert,
} from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { Badge, Button, Card, CodeBadge, SettingGroup } from '../components/UIKit';
import type { NavId } from '../config/routes';
import { emitDeepNavigation } from '../config/settingsSearchIndex';
import { bridgeRequest } from '../hooks/useBridge';
import './PerformancePage.css';

export interface PerformancePageProps {
  onNavigate?: (nav: NavId) => void;
}

interface LinkedGeneralSettings {
  autoReleaseSettingsMemory?: boolean;
}

interface LinkedSearchSettings {
  memoryMode?: string;
  idleTrimMinutes?: number;
  idleStopMinutes?: number;
}

export const PerformancePage: FC<PerformancePageProps> = ({ onNavigate }) => {
  const { t } = useTranslation();
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

  // 初始化并行拉取真实关联设置（只读映射至内存回收时间表，零后台轮询）
  useEffect(() => {
    let cancelled = false;

    void Promise.allSettled([
      bridgeRequest<LinkedGeneralSettings>('general.getSettings', undefined, { silent: true }),
      bridgeRequest<LinkedSearchSettings>('search.getSettings', undefined, { silent: true }),
    ]).then(([genRes, searchRes]) => {
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
    });

    return () => {
      cancelled = true;
    };
  }, []);

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
      {/* 全链路内存生命周期与自动回收时间表（只读透明看板） */}
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

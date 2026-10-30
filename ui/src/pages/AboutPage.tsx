import { useState, useEffect, type FC } from 'react';
import { Card, SettingGroup, Badge, Toggle, Button } from '../components/UIKit';
import { useTranslation } from 'react-i18next';
import { ExternalLink, Info, Layers, Cpu, RefreshCw, User, ShieldCheck, Download, GitBranch } from 'lucide-react';
import { LogoGlyph } from '../components/Tools3000Bolt';
import { bridgeRequest, useBridgeEvent, openExternalUrl } from '../hooks/useBridge';
import { toast } from 'sonner';
import './AboutPage.css';

type DepPurposeKey =
  | 'about.depPurpose.cppStandard'
  | 'about.depPurpose.direct2d'
  | 'about.depPurpose.llHooks'
  | 'about.depPurpose.ntfsEngine'
  | 'about.depPurpose.pinyin'
  | 'about.depPurpose.opencv'
  | 'about.depPurpose.ffmpeg'
  | 'about.depPurpose.windowsOcr'
  | 'about.depPurpose.webview2'
  | 'about.depPurpose.react'
  | 'about.depPurpose.vite'
  | 'about.depPurpose.lucideIcons'
  | 'about.depPurpose.designTokens'
  | 'about.depPurpose.luaSol2'
  | 'about.depPurpose.spdlog'
  | 'about.depPurpose.nlohmannJson'
  | 'about.depPurpose.gtest'
  | 'about.depPurpose.innoSetup';

interface DependencyInfo {
  name: string;
  version: string;
  purpose: DepPurposeKey;
}

const DEPENDENCIES: DependencyInfo[] = [
  // 第一梯队：核心底座与硬核原生性能 (Native Core & Hardware Acceleration)
  { name: 'C++20',                        version: 'MSVC 19.4x', purpose: 'about.depPurpose.cppStandard' },
  { name: 'Direct2D & DirectWrite',       version: 'Hardware',   purpose: 'about.depPurpose.direct2d' },
  { name: 'Win32 LL Hooks & Raw Input',   version: 'Native',     purpose: 'about.depPurpose.llHooks' },

  // 第二梯队：垂直业务引擎与多媒体管线 (Domain Engines & Multimedia)
  { name: 'NTFS MFT / USN Journal',       version: 'Win32 API',  purpose: 'about.depPurpose.ntfsEngine' },
  { name: 'PinyinEngine',                 version: 'Native',     purpose: 'about.depPurpose.pinyin' },
  { name: 'OpenCV',                       version: '4.12+',      purpose: 'about.depPurpose.opencv' },
  { name: 'FFmpeg',                       version: '8.x',        purpose: 'about.depPurpose.ffmpeg' },
  { name: 'Windows.Media.Ocr',            version: 'System',     purpose: 'about.depPurpose.windowsOcr' },

  // 第三梯队：现代化人机交互与展现容器 (Modern UI, Shell & Design Tokens)
  { name: 'WebView2',                     version: 'Evergreen',  purpose: 'about.depPurpose.webview2' },
  { name: 'React & TypeScript',           version: '19.x / 5.8+',purpose: 'about.depPurpose.react' },
  { name: 'Vite',                         version: '6.x',        purpose: 'about.depPurpose.vite' },
  { name: 'Lucide Icons',                 version: '1.x',        purpose: 'about.depPurpose.lucideIcons' },
  { name: 'Tailwind CSS / Design Tokens', version: 'Tokens',     purpose: 'about.depPurpose.designTokens' },

  // 第四梯队：扩展性、可观测性与工程门禁 (Infrastructure, Sandboxing & QA)
  { name: 'Lua 5.4 + sol2',               version: '5.4.7',      purpose: 'about.depPurpose.luaSol2' },
  { name: 'spdlog',                       version: '1.17+',      purpose: 'about.depPurpose.spdlog' },
  { name: 'nlohmann/json',                version: '3.12+',      purpose: 'about.depPurpose.nlohmannJson' },
  { name: 'Google Test (GTest)',          version: '1.17+',      purpose: 'about.depPurpose.gtest' },
  { name: 'Inno Setup',                   version: '6.7+',       purpose: 'about.depPurpose.innoSetup' },
];

const TECH_STACK = DEPENDENCIES;
void TECH_STACK;

interface PerfMetrics {
  memoryMB: number;
  privateMemoryMB: number;
  cpuPercent: number;
  screenshotLatencyMs: number;
  gestureLatencyMs: number;
  uiRenderLatencyMs: number;
}

interface UpdateResult {
  status: 'available' | 'upToDate' | 'unavailable' | 'error';
  currentVersion?: string;
  latestVersion?: string;
  releaseUrl?: string;
  error?: string;
}

const safeMetric = (value: unknown) =>
  typeof value === 'number' && Number.isFinite(value) ? value : 0;

export const AboutPage: FC = () => {
  const { t } = useTranslation();
  const [geekMode, setGeekMode] = useState(false);
  const [metrics, setMetrics] = useState<PerfMetrics | null>(null);
  const [version, setVersion] = useState(__TOOLS3000_VERSION__);
  const [checkingUpdate, setCheckingUpdate] = useState(false);
  const [updateResult, setUpdateResult] = useState<UpdateResult | null>(null);

  useBridgeEvent('update.result', (data) => {
    if (!data || typeof data !== 'object') return;
    const result = data as UpdateResult;
    if (!['available', 'upToDate', 'unavailable', 'error'].includes(result.status)) return;
    setCheckingUpdate(false);
    setUpdateResult(result);
    if (result.status === 'available') {
      toast.success(t('about.updateAvailable', { version: result.latestVersion ?? '' }));
    } else if (result.status === 'upToDate') {
      toast.success(t('about.upToDate'));
    } else if (result.status === 'unavailable') {
      toast.info(t('about.noPublishedRelease'));
    } else {
      toast.error(t('about.updateFailed'));
    }
  });

  useEffect(() => {
    void bridgeRequest<{ version?: unknown }>('app.getSystemInfo')
      .then((info) => {
        if (typeof info.version === 'string' && info.version.trim()) setVersion(info.version);
      })
      .catch(() => undefined);
  }, []);

  useEffect(() => {
    if (!geekMode) return;
    let active = true;
    const fetchMetrics = async () => {
      try {
        const data = await bridgeRequest<PerfMetrics>('perf.getMetrics');
        if (active) setMetrics({
          memoryMB: safeMetric(data?.memoryMB),
          privateMemoryMB: safeMetric(data?.privateMemoryMB),
          cpuPercent: safeMetric(data?.cpuPercent),
          screenshotLatencyMs: safeMetric(data?.screenshotLatencyMs),
          gestureLatencyMs: safeMetric(data?.gestureLatencyMs),
          uiRenderLatencyMs: safeMetric(data?.uiRenderLatencyMs),
        });
      } catch (e) {
        console.error('Failed to fetch perf metrics', e);
      }
    };
    fetchMetrics();
    const timer = setInterval(fetchMetrics, 1000);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [geekMode]);

  const checkForUpdates = async () => {
    if (checkingUpdate) return;
    setCheckingUpdate(true);
    try {
      const response = await bridgeRequest<{ success: boolean; started?: boolean }>('app.checkForUpdates');
      if (!response.success) throw new Error('check failed');
      if (response.started === false) {
        setCheckingUpdate(false);
        toast.info(t('about.updateCheckBusy'));
      }
    } catch {
      setCheckingUpdate(false);
      toast.error(t('about.updateFailed'));
    }
  };

  const openReleasePage = async () => {
    const url = updateResult?.releaseUrl;
    if (!url?.startsWith('https://github.com/yuan278501381/Tools3000/')) return;
    const success = await openExternalUrl(url);
    if (!success) toast.error(t('about.openReleaseFailed'));
  };

  const handleExportLogs = async () => {
    try {
      const res = await bridgeRequest<{ success: boolean; cancelled?: boolean; error?: string }>('app.exportLogs');
      if (res.cancelled) return;
      if (!res.success) throw new Error(res.error || t('about.exportFailed', 'Export failed'));
      toast.success(t('about.exportLogsSuccess', 'Diagnostic logs exported and highlighted successfully'));
    } catch (e) {
      toast.error(t('about.exportLogsFailed', 'Failed to export diagnostic logs'), { description: String(e) });
    }
  };

  return (
    <div className="about-page" style={{ animation: 'fadeIn 0.3s ease' }}>

      <div className="about-grand-showcase">
        <div className="about-grand-showcase__aura"></div>
        <div className="about-grand-showcase__logo-container">
          <img
            src="/logo.svg"
            width={180}
            height={180}
            className="about-grand-showcase__logo"
            alt="Tools3000"
            draggable={false}
            decoding="async"
          />
        </div>
        <div className="about-grand-showcase__text">
          <div className="about-grand-showcase__title-row">
            <h1 className="about-grand-showcase__title">Tools3000</h1>
            <span className="about-grand-showcase__version">v{version}</span>
          </div>
          <p className="about-grand-showcase__subtitle">The Ultimate Windows Productivity Suite</p>
        </div>
      </div>

      <SettingGroup title={t('about.title')} icon={<Info size={20} strokeWidth={2.5} />}>
        <Card className="about-card-hero">
          <div className="about-hero">
            <div className="about-hero__brand">
              <div className="about-hero__logo-box">
                <LogoGlyph size={36} colorVariant />
              </div>
              <div className="about-hero__info">
                <div className="about-hero__title-row">
                  <h2 className="about-hero__title">Tools3000</h2>
                  <Badge text={`v${version}`} variant="primary" />
                  <Badge text="C++20 & Direct2D" variant="success" />
                  <Badge text={t('about.authorBadge', 'Author · Yy1 (@yuan278501381)')} variant="muted" />
                </div>
                <p className="about-hero__subtitle">{t('about.subtitle')}</p>
              </div>
            </div>

            <div className="about-hero__actions">
              <Button variant="secondary" onClick={() => void checkForUpdates()} disabled={checkingUpdate}>
                <RefreshCw size={14} className={checkingUpdate ? 'about-update-spin' : undefined} />
                <span>{checkingUpdate ? t('about.checkingUpdate') : t('about.checkUpdate')}</span>
              </Button>
              <Button variant="secondary" onClick={() => void handleExportLogs()} title={t('about.exportLogsTip', 'Export system diagnostic logs and environment report')}>
                <Download size={14} />
                <span>{t('about.exportLogs', 'Export Logs')}</span>
              </Button>
              {updateResult?.status === 'available' && updateResult.releaseUrl && (
                <Button variant="primary" onClick={() => void openReleasePage()}>
                  <ExternalLink size={14} />
                  <span>{t('about.openRelease')}</span>
                </Button>
              )}
            </div>
          </div>

          <div className="about-desc-box">
            <p className="about-desc">
              {t('about.description')}
            </p>
          </div>

          <div className="about-meta-row">
            <div className="about-meta-item">
              <User size={14} className="about-meta-icon" />
              <span className="about-meta-label">{t('about.author')}:</span>
              <a
                href="https://github.com/yuan278501381"
                target="_blank"
                rel="noopener noreferrer"
                className="about-meta-link"
                onClick={(e) => {
                  e.preventDefault();
                  void openExternalUrl('https://github.com/yuan278501381');
                }}
                title={t('about.authorGithubTip', 'Visit original author GitHub profile')}
              >
                <strong className="about-meta-val">Yy1</strong>
                <span className="about-meta-handle">(@yuan278501381)</span>
                <ExternalLink size={12} className="about-meta-ext" />
              </a>
            </div>
            <div className="about-meta-item">
              <GitBranch size={14} className="about-meta-icon" />
              <span className="about-meta-label">{t('about.repository')}:</span>
              <a
                href="https://github.com/yuan278501381/Tools3000"
                target="_blank"
                rel="noopener noreferrer"
                className="about-meta-link"
                onClick={(e) => {
                  e.preventDefault();
                  void openExternalUrl('https://github.com/yuan278501381/Tools3000');
                }}
                title={t('about.repositoryTip', 'Visit GitHub open source repository')}
              >
                <strong className="about-meta-val">yuan278501381/Tools3000</strong>
                <ExternalLink size={12} className="about-meta-ext" />
              </a>
            </div>
            <div className="about-meta-item">
              <ShieldCheck size={14} className="about-meta-icon" />
              <span className="about-meta-label">{t('about.license')}:</span>
              <a
                href="https://github.com/yuan278501381/Tools3000/blob/main/LICENSE"
                target="_blank"
                rel="noopener noreferrer"
                className="about-meta-link"
                onClick={(e) => {
                  e.preventDefault();
                  void openExternalUrl('https://github.com/yuan278501381/Tools3000/blob/main/LICENSE');
                }}
                title={t('about.licenseTip', 'View MIT License details')}
              >
                <strong className="about-meta-val">MIT License</strong>
                <ExternalLink size={12} className="about-meta-ext" />
              </a>
            </div>
          </div>
        </Card>
      </SettingGroup>

      <SettingGroup title={t('about.techStack')} icon={<Layers size={20} strokeWidth={2.5} />}>
        <Card>
          <div className="about-deps">
            {DEPENDENCIES.map((dep) => (
              <div key={dep.name} className="about-dep-item">
                <span className="about-dep-name">{dep.name}</span>
                <Badge text={dep.version} variant="muted" />
                <span className="about-dep-purpose">{t(dep.purpose)}</span>
              </div>
            ))}
          </div>
        </Card>
      </SettingGroup>

      <SettingGroup title={t('about.geekMode')} icon={<Cpu size={20} strokeWidth={2.5} />}>
        <Card>
          <Toggle
            id="geek-mode-toggle"
            label={t('about.enablePerfMonitor')}
            description={t('about.enablePerfMonitorDesc')}
            checked={geekMode}
            onChange={(enabled) => {
              setGeekMode(enabled);
              if (!enabled) setMetrics(null);
            }}
          />
          {geekMode && metrics && (
            <div className="about-perf-panel" style={{ marginTop: '1rem', padding: '1rem', background: 'var(--bg-secondary)', borderRadius: '8px' }}>
              <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '1rem' }}>
                <div>
                  <div style={{ fontSize: '0.875rem', color: 'var(--text-secondary)' }}>{t('about.perfMemory')}</div>
                  <div style={{ fontSize: '1.25rem', fontWeight: 600 }}>{((metrics.privateMemoryMB && metrics.privateMemoryMB > 0) ? metrics.privateMemoryMB : metrics.memoryMB).toFixed(1)} MB</div>
                </div>
                <div>
                  <div style={{ fontSize: '0.875rem', color: 'var(--text-secondary)' }}>{t('about.perfWorkingSet')}</div>
                  <div style={{ fontSize: '1.25rem', fontWeight: 600 }}>{metrics.memoryMB.toFixed(1)} MB</div>
                </div>
                <div>
                  <div style={{ fontSize: '0.875rem', color: 'var(--text-secondary)' }}>{t('about.perfCpu')}</div>
                  <div style={{ fontSize: '1.25rem', fontWeight: 600 }}>{metrics.cpuPercent.toFixed(1)} %</div>
                </div>
                <div>
                  <div style={{ fontSize: '0.875rem', color: 'var(--text-secondary)' }}>{t('about.perfGestureLatency')}</div>
                  <div style={{ fontSize: '1.25rem', fontWeight: 600 }}>{metrics.gestureLatencyMs.toFixed(1)} ms</div>
                </div>
              </div>
            </div>
          )}
        </Card>
      </SettingGroup>

      <div className="about-footer-copyright">
        <p>
          Copyright © 2026{' '}
          <a
            href="https://github.com/yuan278501381"
            target="_blank"
            rel="noopener noreferrer"
            className="about-footer-link"
            onClick={(e) => {
              e.preventDefault();
              void openExternalUrl('https://github.com/yuan278501381');
            }}
          >
            <strong>Yy1 (GitHub yuan278501381)</strong>
          </a>{' '}
          & Tools3000 Contributors.
        </p>
        <p className="about-footer-sub">
          Licensed under the{' '}
          <a
            href="https://github.com/yuan278501381/Tools3000/blob/main/LICENSE"
            target="_blank"
            rel="noopener noreferrer"
            className="about-footer-link"
            onClick={(e) => {
              e.preventDefault();
              void openExternalUrl('https://github.com/yuan278501381/Tools3000/blob/main/LICENSE');
            }}
          >
            <strong>MIT License</strong>
          </a>
          . Designed with passion for extreme productivity.
        </p>
      </div>
    </div>
  );
};

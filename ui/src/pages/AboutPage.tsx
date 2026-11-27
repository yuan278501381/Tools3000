import { useState, useEffect, type FC } from 'react';
import { SettingGroup, Button, CodeBadge } from '../components/UIKit';
import { useTranslation } from 'react-i18next';
import {
  ExternalLink,
  Layers,
  RefreshCw,
  User,
  ShieldCheck,
  Download,
  GitBranch,
  BellOff,
  MemoryStick,
  ChevronDown,
  ScrollText,
} from 'lucide-react';
import type { NavId } from '../config/routes';
import { bridgeRequest, useBridgeEvent, openExternalUrl } from '../hooks/useBridge';
import { toast } from 'sonner';
import './AboutPage.css';

export interface AboutPageProps {
  onNavigate?: (nav: NavId) => void;
  onOpenChangelog?: () => void;
}

type DepPurposeKey =
  | 'about.depPurpose.cppStandard'
  | 'about.depPurpose.rustSimd'
  | 'about.depPurpose.rustClipboard'
  | 'about.depPurpose.rustCliMcp'
  | 'about.depPurpose.rustPicasaViewer'
  | 'about.depPurpose.rustWindowKitSlint'
  | 'about.depPurpose.direct2d'
  | 'about.depPurpose.llHooks'
  | 'about.depPurpose.ddcBrightness'
  | 'about.depPurpose.ntfsEngine'
  | 'about.depPurpose.pinyin'
  | 'about.depPurpose.ppocrDirectml'
  | 'about.depPurpose.winhttpTranslate'
  | 'about.depPurpose.opencv'
  | 'about.depPurpose.ffmpeg'
  | 'about.depPurpose.windowsOcr'
  | 'about.depPurpose.ifilterShellProps'
  | 'about.depPurpose.uiaShell'
  | 'about.depPurpose.webview2'
  | 'about.depPurpose.react'
  | 'about.depPurpose.vite'
  | 'about.depPurpose.i18next'
  | 'about.depPurpose.lucideIcons'
  | 'about.depPurpose.designTokens'
  | 'about.depPurpose.sqliteZlib'
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

const TECH_STACK: DependencyInfo[] = [
  // 第一梯队：核心底座与硬核原生性能 (Native Core & Hardware Acceleration)
  { name: 'C++20',                          version: 'MSVC v145 · x64 / ARM64',        purpose: 'about.depPurpose.cppStandard' },
  { name: 'Rust (SIMD Content Kernel)',     version: 'Edition 2021 · AVX2 / NEON',     purpose: 'about.depPurpose.rustSimd' },
  { name: 'Rust Clipboard Kernel',          version: 'Edition 2021 · Zero-Deps',       purpose: 'about.depPurpose.rustClipboard' },
  { name: 'Rust CLI & MCP Server',          version: 'Edition 2021 · Zero-Deps · MCP', purpose: 'about.depPurpose.rustCliMcp' },
  { name: 'Rust Picasa Viewer',             version: 'D3D11 · DComp · resvg 0.45',     purpose: 'about.depPurpose.rustPicasaViewer' },
  { name: 'Rust WindowKit & Slint Fluent',  version: 'Edition 2021 · Slint 1.19.1',    purpose: 'about.depPurpose.rustWindowKitSlint' },
  { name: 'Direct2D & DirectComposition',   version: 'GPU Hardware',                   purpose: 'about.depPurpose.direct2d' },
  { name: 'Win32 LL Hooks & Raw Input',     version: 'Native API',                     purpose: 'about.depPurpose.llHooks' },
  { name: 'DDC/CI & WMI Display',           version: 'dxva2 · root\\wmi',              purpose: 'about.depPurpose.ddcBrightness' },

  // 第二梯队：垂直业务引擎与多媒体管线 (Domain Engines & Multimedia)
  { name: 'NTFS MFT & USN Journal',         version: 'Kernel I/O',                     purpose: 'about.depPurpose.ntfsEngine' },
  { name: 'PinyinEngine',                   version: 'Built-in',                       purpose: 'about.depPurpose.pinyin' },
  { name: 'PP-OCRv6 & MI-GAN 512',          version: 'DirectML GPU · 100% Offline',    purpose: 'about.depPurpose.ppocrDirectml' },
  { name: 'WinHTTP & DPAPI Translate',      version: 'Free AI · Dual-Source · BYOK',   purpose: 'about.depPurpose.winhttpTranslate' },
  { name: 'OpenCV & quirc',                 version: '4.12',                           purpose: 'about.depPurpose.opencv' },
  { name: 'FFmpeg & WASAPI',                version: '8.1',                            purpose: 'about.depPurpose.ffmpeg' },
  { name: 'Windows.Media.Ocr & Pdf',        version: '100% Offline',                   purpose: 'about.depPurpose.windowsOcr' },
  { name: 'Windows IFilter & Shell Props',  version: 'COM · Property System',          purpose: 'about.depPurpose.ifilterShellProps' },
  { name: 'UIAutomation & Shell COM',       version: 'System API',                     purpose: 'about.depPurpose.uiaShell' },

  // 第三梯队：现代化人机交互与展现容器 (Modern UI, Shell & Design Tokens)
  { name: 'WebView2',                       version: '1.0.4022 · Evergreen',           purpose: 'about.depPurpose.webview2' },
  { name: 'React & TypeScript',             version: '19.2 / 6.0',                     purpose: 'about.depPurpose.react' },
  { name: 'Vite',                           version: '8.3',                            purpose: 'about.depPurpose.vite' },
  { name: 'i18next & react-i18next',        version: '26.7 / 17.0 · 8 Locales',        purpose: 'about.depPurpose.i18next' },
  { name: 'Lucide Icons',                   version: '1.21',                           purpose: 'about.depPurpose.lucideIcons' },
  { name: 'CSS Design Tokens',              version: 'Native CSS',                     purpose: 'about.depPurpose.designTokens' },

  // 第四梯队：数据存储、扩展性与工程门禁 (Storage, Scripting & QA Gates)
  { name: 'WinSQLite3 & zlib',              version: 'System API / 1.3',               purpose: 'about.depPurpose.sqliteZlib' },
  { name: 'Lua + sol2',                     version: '5.5 / 3.5',                      purpose: 'about.depPurpose.luaSol2' },
  { name: 'spdlog',                         version: '1.17',                           purpose: 'about.depPurpose.spdlog' },
  { name: 'nlohmann/json',                  version: '3.12',                           purpose: 'about.depPurpose.nlohmannJson' },
  { name: 'GoogleTest & Vitest',            version: '1.17 / 4.1',                     purpose: 'about.depPurpose.gtest' },
  { name: 'Inno Setup',                     version: '6.7',                            purpose: 'about.depPurpose.innoSetup' },
];

interface UpdateResult {
  status: 'idle' | 'checking' | 'available' | 'downloading' | 'readyToInstall' | 'upToDate' | 'unavailable' | 'error';
  hasUpdate?: boolean;
  badgeVisible?: boolean;
  dismissed?: boolean;
  isPortable?: boolean;
  currentVersion?: string;
  latestVersion?: string;
  releaseUrl?: string;
  releaseNotes?: string;
  assetName?: string;
  assetSize?: number;
  downloadedBytes?: number;
  progress?: number;
  readyToInstall?: boolean;
  error?: string;
}

function formatUpdateMB(bytes?: number): string {
  if (!bytes || bytes <= 0) return '0 MB';
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export const AboutPage: FC<AboutPageProps> = ({ onNavigate, onOpenChangelog }) => {
  const { t } = useTranslation();
  const [version, setVersion] = useState(__TOOLS3000_VERSION__);
  const [buildDate, setBuildDate] = useState(__TOOLS3000_BUILD_DATE__);
  const [checkingUpdate, setCheckingUpdate] = useState(false);
  const [installingUpdate, setInstallingUpdate] = useState(false);
  const [dismissingUpdate, setDismissingUpdate] = useState(false);
  const [updateResult, setUpdateResult] = useState<UpdateResult | null>(null);

  useBridgeEvent('update.result', (data) => {
    if (!data || typeof data !== 'object') return;
    const result = data as UpdateResult;
    if (!['available', 'downloading', 'readyToInstall', 'upToDate', 'unavailable', 'error'].includes(result.status)) return;
    setCheckingUpdate(false);
    setUpdateResult((prev) => ({ ...prev, ...result }));
    if (result.status === 'available' || result.status === 'readyToInstall') {
      toast.success(t('about.updateAvailable', { version: result.latestVersion ?? '' }));
    } else if (result.status === 'upToDate') {
      toast.success(t('about.upToDate'));
    } else if (result.status === 'unavailable') {
      toast.info(t('about.noPublishedRelease'));
    } else if (result.status === 'error') {
      toast.error(t('about.updateFailed'));
    }
  });

  useBridgeEvent('update.state', (data) => {
    if (!data || typeof data !== 'object') return;
    const st = data as UpdateResult;
    if (st.status !== 'checking') {
      setCheckingUpdate(false);
    }
    setUpdateResult((prev) => ({ ...prev, ...st }));
  });

  useEffect(() => {
    void bridgeRequest<{ version?: unknown; buildDate?: unknown }>('app.getSystemInfo')
      .then((info) => {
        if (typeof info.version === 'string' && info.version.trim()) setVersion(info.version);
        if (typeof info.buildDate === 'string' && info.buildDate.trim()) setBuildDate(info.buildDate.trim());
      })
      .catch(() => undefined);
    void bridgeRequest<UpdateResult>('app.getUpdateState', undefined, { silent: true })
      .then((st) => {
        if (st && typeof st.status === 'string' && st.status !== 'idle') {
          setUpdateResult(st);
          setCheckingUpdate(st.status === 'checking');
        }
      })
      .catch(() => undefined);
  }, []);

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

  const handleDownloadUpdate = async () => {
    try {
      const res = await bridgeRequest<UpdateResult & { success?: boolean }>('app.downloadUpdate');
      if (res && typeof res.status === 'string') {
        setUpdateResult((prev) => ({ ...prev, ...res }));
      }
    } catch {
      toast.error(t('about.updateFailed'));
    }
  };

  const handleInstallUpdate = async () => {
    if (installingUpdate) return;
    setInstallingUpdate(true);
    try {
      const res = await bridgeRequest<{ success?: boolean; cancelled?: boolean; error?: string }>('app.installUpdate');
      if (res?.cancelled || res?.error === 'cancelled') {
        setInstallingUpdate(false);
        return;
      }
      if (!res?.success) {
        setInstallingUpdate(false);
        toast.error(t('about.installUpdateFailed', 'Failed to launch update installer'));
        return;
      }
    } catch {
      setInstallingUpdate(false);
      toast.error(t('about.installUpdateFailed', 'Failed to launch update installer'));
    }
  };

  const handleDismissUpdate = async () => {
    if (dismissingUpdate) return;
    setDismissingUpdate(true);
    try {
      const res = await bridgeRequest<{ success?: boolean }>('app.dismissUpdate');
      if (res?.success !== false) {
        setUpdateResult((prev) => (prev ? { ...prev, dismissed: true, badgeVisible: false } : prev));
        toast.info(String(t('about.versionDismissedToast' as never, {
          version: updateResult?.latestVersion ?? '',
          defaultValue: `Dismissed update badge for ${updateResult?.latestVersion ?? ''}`,
        } as never)));
      }
    } catch {
      toast.error(t('about.updateFailed'));
    } finally {
      setDismissingUpdate(false);
    }
  };

  const openReleasePage = async () => {
    const url = updateResult?.releaseUrl;
    if (!url || !url.toLowerCase().startsWith('https://github.com/yuan278501381/tools3000/')) return;
    const success = await openExternalUrl(url);
    if (!success) toast.error(t('about.openReleaseFailed'));
  };

  return (
    <div className="about-page" style={{ animation: 'fadeIn 0.3s ease' }}>
      {/* ── 单体旗舰品牌名片卡 (精简去重 · 聚焦核心操作与元信息) ── */}
      <section className="about-showcase-card" aria-label={t('about.title')}>
        <div className="about-showcase-card__top">
          <div className="about-showcase-card__brand">
            <div className="about-showcase-card__logo-frame">
              <img
                src="/logo.svg"
                width={44}
                height={44}
                className="about-showcase-card__logo"
                alt="Tools3000"
                draggable={false}
                decoding="async"
              />
            </div>
            <div className="about-showcase-card__identity">
              <div className="about-showcase-card__title-row">
                <h2 className="about-showcase-card__title">Tools3000</h2>
                <CodeBadge>{buildDate ? `v${version} · ${buildDate}` : `v${version}`}</CodeBadge>
              </div>
              <p className="about-showcase-card__subtitle">{t('about.subtitle')}</p>
            </div>
          </div>

          <div className="about-hero__actions">
            <Button variant="secondary" size="sm" onClick={() => void checkForUpdates()} disabled={checkingUpdate}>
              <RefreshCw size={13} className={checkingUpdate ? 'about-update-spin' : undefined} />
              <span>{checkingUpdate ? t('about.checkingUpdate') : t('about.checkUpdate')}</span>
            </Button>

            <Button
              variant="secondary"
              size="sm"
              onClick={() => {
                if (onOpenChangelog) {
                  onOpenChangelog();
                } else {
                  window.dispatchEvent(new CustomEvent('tools3000:open-changelog'));
                }
              }}
            >
              <ScrollText size={13} />
              <span>{t('about.viewChangelogBtn')}</span>
            </Button>

            {onNavigate && (
              <Button variant="secondary" size="sm" onClick={() => onNavigate('performance')}>
                <MemoryStick size={13} />
                <span>{t('about.openPerformancePageBtn')}</span>
              </Button>
            )}

            {updateResult?.readyToInstall ? (
              <Button variant="primary" size="sm" onClick={() => void handleInstallUpdate()} disabled={installingUpdate}>
                <Download size={13} className={installingUpdate ? 'about-update-spin' : undefined} />
                <span>
                  {t('about.installUpdateNow', { version: updateResult.latestVersion ?? '', defaultValue: `Install ${updateResult.latestVersion ?? ''}` })}
                </span>
              </Button>
            ) : updateResult?.hasUpdate && updateResult.status !== 'downloading' ? (
              <Button variant="primary" size="sm" onClick={() => void handleDownloadUpdate()}>
                <Download size={13} />
                <span>{t('about.downloadUpdateNow', { version: updateResult.latestVersion ?? '', defaultValue: `Download ${updateResult.latestVersion ?? ''}` })}</span>
              </Button>
            ) : null}

            {updateResult?.hasUpdate && updateResult.releaseUrl && (
              <Button variant="secondary" size="sm" onClick={() => void openReleasePage()}>
                <ExternalLink size={13} />
                <span>{t('about.openRelease')}</span>
              </Button>
            )}
          </div>
        </div>

        {updateResult?.hasUpdate && (
          <div
            className={`about-update-panel${updateResult.readyToInstall ? ' about-update-panel--ready' : ''}${updateResult.dismissed ? ' about-update-panel--dismissed' : ''}`}
          >
            <div className="about-update-panel__row">
              <span className="about-update-panel__status">
                <span className="about-update-panel__dot" aria-hidden="true" />
                {updateResult.status === 'downloading'
                  ? t('about.downloadingDetail', {
                      version: updateResult.latestVersion ?? '',
                      progress: updateResult.progress ?? 0,
                      defaultValue: `Downloading ${updateResult.latestVersion ?? ''} (${updateResult.progress ?? 0}%)`,
                    })
                  : updateResult.readyToInstall
                    ? t('about.updateReadyHint', {
                        version: updateResult.latestVersion ?? '',
                        defaultValue: `Version ${updateResult.latestVersion ?? ''} verified and ready to install`,
                      })
                    : t('about.updateAvailable', { version: updateResult.latestVersion ?? '' })}
              </span>
              {updateResult.status === 'downloading' && (
                <span className="about-update-panel__bytes">
                  {`${formatUpdateMB(updateResult.downloadedBytes)} / ${formatUpdateMB(updateResult.assetSize)}`}
                </span>
              )}
            </div>
            {updateResult.status === 'downloading' && (
              <div className="about-update-panel__bar" role="progressbar" aria-valuenow={updateResult.progress ?? 0} aria-valuemin={0} aria-valuemax={100}>
                <div className="about-update-panel__bar-fill" style={{ width: `${Math.min(100, Math.max(2, updateResult.progress ?? 0))}%` }} />
              </div>
            )}
            {updateResult.dismissed ? (
              <div className="about-update-panel__footer">
                <span className="about-update-panel__dismissed-note">
                  <BellOff size={13} aria-hidden="true" />
                  <span>
                    {t('about.versionDismissedHint' as never, 'Badge reminder dismissed for this version. Check for updates or download anytime to resume.')}
                  </span>
                </span>
              </div>
            ) : updateResult.status !== 'downloading' ? (
              <div className="about-update-panel__footer">
                <Button
                  variant="ghost"
                  size="sm"
                  onClick={() => void handleDismissUpdate()}
                  disabled={dismissingUpdate}
                >
                  <BellOff size={13} aria-hidden="true" />
                  <span>{t('about.dismissVersion' as never, 'Dismiss this version')}</span>
                </Button>
              </div>
            ) : null}
          </div>
        )}

        <p className="about-desc">{t('about.description')}</p>

        <div className="about-meta-row">
          <div className="about-meta-item">
            <User size={13} className="about-meta-icon" />
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
              <ExternalLink size={11} className="about-meta-ext" />
            </a>
          </div>
          <div className="about-meta-item">
            <GitBranch size={13} className="about-meta-icon" />
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
              <ExternalLink size={11} className="about-meta-ext" />
            </a>
          </div>
          <div className="about-meta-item">
            <ShieldCheck size={13} className="about-meta-icon" />
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
              title={t('about.licenseTip', 'View GNU GPL v3.0 license details')}
            >
              <strong className="about-meta-val">GPL-3.0 License</strong>
              <ExternalLink size={11} className="about-meta-ext" />
            </a>
          </div>
        </div>
      </section>

      {/* ── 可折叠技术栈与开源组件矩阵（默认收起，保持首屏清爽） ── */}
      <SettingGroup title={t('about.techStack')} icon={<Layers size={20} strokeWidth={2.5} />}>
        <details className="about-tech-details">
          <summary className="about-tech-details__summary">
            <span className="about-tech-details__summary-text">
              {t('about.techStackSummary', { count: TECH_STACK.length })}
            </span>
            <ChevronDown size={16} strokeWidth={2} className="about-tech-details__chevron" aria-hidden="true" />
          </summary>
          <div className="about-tech-details__body">
            <div className="about-deps">
              {TECH_STACK.map((dep) => (
                <div key={dep.name} className="about-dep-item">
                  <div className="about-dep-item__top">
                    <span className="about-dep-name">{dep.name}</span>
                    <CodeBadge variant="muted">{dep.version}</CodeBadge>
                  </div>
                  <span className="about-dep-purpose">{t(dep.purpose)}</span>
                </div>
              ))}
            </div>
          </div>
        </details>
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
            <strong>GPL-3.0 License</strong>
          </a>
          . Designed with passion for extreme productivity.
        </p>
      </div>
    </div>
  );
};

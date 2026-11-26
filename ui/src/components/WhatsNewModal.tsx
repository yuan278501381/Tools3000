/* ─────────────────────────────────────────────────────────────────────────────
 * WhatsNewModal — 世界级离线分页更新日志与跨版迁移亮点弹窗（方案 A）
 *
 * 核心能力：
 *   1. 动态懒加载独立离线分块 (generated/changelogData.json)，0 首屏主包开销、0 网络依赖；
 *   2. 双模式自适应：
 *      - upgrade：展示跨版迁移横幅、NEW 徽章、跨 <=3 版全展开 / >3 版展最新 3 版防过载；
 *      - manual ：首屏展示最新 10 版（默认展开最新 1 版）、关键词实时检索、+10 分页与 GitHub 外链；
 *   3. 每版附带本地化精确日期时间 (YYYY-MM-DD HH:mm) + 相对时间胶囊 + Major/Minor/Patch 语义徽章；
 *   4. 左下角「更新后自动显示更新日志」偏好与通用设置页实时双向同步，关闭自动标记已读。
 * ───────────────────────────────────────────────────────────────────────────── */

import {
  useState,
  useEffect,
  useMemo,
  useCallback,
  useRef,
  Fragment,
  type FC,
  type ReactNode,
} from 'react';
import { useTranslation } from 'react-i18next';
import {
  X,
  Sparkles,
  ScrollText,
  ChevronDown,
  ChevronRight,
  Search,
  Clock,
  ExternalLink,
  Check,
  Loader2,
  ArrowRight,
  Layers,
} from 'lucide-react';
import { Button, CodeBadge } from './UIKit';
import { bridgeRequest, openExternalUrl } from '../hooks/useBridge';
import {
  compareSemver,
  formatLocalDateTime,
  computeRelativeDayDiff,
  resolveReleaseContent,
  matchesSearchQuery,
  type ChangelogMode,
  type ChangelogBundle,
} from './whatsNewUtils';
import './WhatsNewModal.css';

export interface WhatsNewModalProps {
  open: boolean;
  mode: ChangelogMode;
  currentVersion?: string;
  previousVersion?: string;
  showChangelogOnUpdate: boolean;
  onToggleShowOnUpdate: (enabled: boolean) => void;
  onClose: () => void;
}

const PAGE_SIZE = 10;
const MAX_AUTO_EXPANDED_UPGRADE_RELEASES = 3;
const GITHUB_RELEASES_URL = 'https://github.com/yuan278501381/Tools3000/releases';

function renderInlineMarkdown(text: string): ReactNode {
  const tokens: ReactNode[] = [];
  const pattern = /(`[^`]+`|\*\*[^*]+\*\*)/g;
  let lastIndex = 0;
  let match: RegExpExecArray | null = pattern.exec(text);
  let keyIdx = 0;

  while (match !== null) {
    if (match.index > lastIndex) {
      tokens.push(text.slice(lastIndex, match.index));
    }
    const raw = match[0];
    if (raw.startsWith('`') && raw.endsWith('`') && raw.length > 2) {
      tokens.push(
        <code key={`c-${keyIdx}`} className="wn-inline-code">
          {raw.slice(1, -1)}
        </code>,
      );
    } else if (raw.startsWith('**') && raw.endsWith('**') && raw.length > 4) {
      tokens.push(
        <strong key={`b-${keyIdx}`} className="wn-inline-strong">
          {raw.slice(2, -2)}
        </strong>,
      );
    } else {
      tokens.push(raw);
    }
    keyIdx += 1;
    lastIndex = match.index + raw.length;
    match = pattern.exec(text);
  }
  if (lastIndex < text.length) {
    tokens.push(text.slice(lastIndex));
  }
  return tokens.map((node, idx) => <Fragment key={idx}>{node}</Fragment>);
}

export const WhatsNewModal: FC<WhatsNewModalProps> = ({
  open,
  mode,
  currentVersion,
  previousVersion,
  showChangelogOnUpdate,
  onToggleShowOnUpdate,
  onClose,
}) => {
  const { t, i18n } = useTranslation();
  const [bundle, setBundle] = useState<ChangelogBundle | null>(null);
  const [loadFailed, setLoadFailed] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [extraVisibleCount, setExtraVisibleCount] = useState(0);
  const [toggledVersions, setToggledVersions] = useState<Set<string>>(() => new Set());
  const [savingPref, setSavingPref] = useState(false);
  const panelRef = useRef<HTMLDivElement>(null);

  const preferChinese = useMemo(() => {
    const lang = (i18n.language || '').toLowerCase();
    return lang.startsWith('zh');
  }, [i18n.language]);

  // 仅在弹窗打开时懒加载离线 JSON 分块，0 首屏主包开销，且不在 effect 中同步触发 setState
  useEffect(() => {
    if (!open || bundle || loadFailed) return;
    let cancelled = false;
    void import('../generated/changelogData.json')
      .then((mod) => {
        if (cancelled) return;
        const data = (mod.default ?? mod) as ChangelogBundle;
        setBundle(data);
      })
      .catch((err) => {
        if (cancelled) return;
        console.error('[WhatsNewModal] Failed to load changelog bundle:', err);
        setLoadFailed(true);
      });
    return () => {
      cancelled = true;
    };
  }, [open, bundle, loadFailed]);

  const loading = open && bundle === null && !loadFailed;
  const allReleases = useMemo(() => bundle?.releases ?? [], [bundle]);

  const newReleaseVersionSet = useMemo(() => {
    const set = new Set<string>();
    if (mode !== 'upgrade' || !previousVersion) return set;
    for (const rel of allReleases) {
      if (compareSemver(rel.version, previousVersion) > 0) {
        set.add(rel.version);
      }
    }
    return set;
  }, [allReleases, mode, previousVersion]);

  const newReleaseCount = newReleaseVersionSet.size;

  // 纯派生智能默认展开集（升级态跨 <=3 版全展开 / >3 版仅展开最新 3 版防过载；归档态默认展开最新 1 版）
  const defaultExpandedSet = useMemo(() => {
    const set = new Set<string>();
    if (allReleases.length === 0) return set;
    if (mode === 'upgrade' && newReleaseCount > 0) {
      const newOnes = allReleases.filter((r) => newReleaseVersionSet.has(r.version));
      const expandLimit = newOnes.length <= MAX_AUTO_EXPANDED_UPGRADE_RELEASES
        ? newOnes.length
        : MAX_AUTO_EXPANDED_UPGRADE_RELEASES;
      for (let i = 0; i < expandLimit; i += 1) {
        set.add(newOnes[i].version);
      }
    } else {
      set.add(allReleases[0].version);
    }
    return set;
  }, [allReleases, mode, newReleaseCount, newReleaseVersionSet]);

  const baseVisibleCount = useMemo(() => {
    if (mode === 'upgrade' && newReleaseCount > PAGE_SIZE) {
      return newReleaseCount;
    }
    return PAGE_SIZE;
  }, [mode, newReleaseCount]);

  // 键盘 Escape 关闭 + 焦点落入对话框
  useEffect(() => {
    if (!open) return;
    panelRef.current?.focus();
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        onClose();
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [open, onClose]);

  const normalizedQuery = searchQuery.trim().toLowerCase();

  const filteredReleases = useMemo(() => {
    if (!normalizedQuery) return allReleases;
    return allReleases.filter((rel) => matchesSearchQuery(rel, normalizedQuery));
  }, [allReleases, normalizedQuery]);

  const displayedReleases = useMemo(() => {
    if (normalizedQuery) return filteredReleases;
    return filteredReleases.slice(0, baseVisibleCount + extraVisibleCount);
  }, [filteredReleases, normalizedQuery, baseVisibleCount, extraVisibleCount]);

  const remainingCount = normalizedQuery
    ? 0
    : Math.max(0, filteredReleases.length - displayedReleases.length);

  const toggleExpand = useCallback((version: string) => {
    setToggledVersions((prev) => {
      const next = new Set(prev);
      if (next.has(version)) next.delete(version);
      else next.add(version);
      return next;
    });
  }, []);

  const formatRelativeLabel = useCallback((iso: string): string => {
    const days = computeRelativeDayDiff(iso);
    if (days === null) return '';
    if (days === 0) return t('changelog.relativeToday');
    if (days === 1) return t('changelog.relativeYesterday');
    if (days < 30) return t('changelog.relativeDaysAgo', { count: days });
    if (days < 365) return t('changelog.relativeMonthsAgo', { count: Math.max(1, Math.floor(days / 30)) });
    return t('changelog.relativeYearsAgo', { count: Math.max(1, Math.floor(days / 365)) });
  }, [t]);

  const handleToggleAutoShow = async (checked: boolean) => {
    if (savingPref) return;
    setSavingPref(true);
    onToggleShowOnUpdate(checked);
    try {
      await bridgeRequest('general.updateSettings', { showChangelogOnUpdate: checked }, { silent: true });
      window.dispatchEvent(new CustomEvent<boolean>('tools3000:changelog-pref-changed', { detail: checked }));
    } catch {
      onToggleShowOnUpdate(!checked);
    } finally {
      setSavingPref(false);
    }
  };

  const handleOpenGitHubReleases = () => {
    void openExternalUrl(GITHUB_RELEASES_URL);
  };

  if (!open) return null;

  const effectiveCurrent = currentVersion || allReleases[0]?.version || '';
  const latestTag = effectiveCurrent ? `v${effectiveCurrent.replace(/^v/i, '')}` : '';
  const prevTag = previousVersion ? `v${previousVersion.replace(/^v/i, '')}` : '';

  return (
    <div
      className="wn-backdrop"
      role="presentation"
      onMouseDown={(e) => {
        if (e.target === e.currentTarget) {
          onClose();
        }
      }}
    >
      <div
        ref={panelRef}
        className="wn-panel"
        role="dialog"
        aria-modal="true"
        aria-labelledby="whats-new-title"
        tabIndex={-1}
      >
        <div className="wn-glow" aria-hidden="true" />

        {/* ── 头部：模式感知标题与关闭按钮 ── */}
        <header className="wn-header">
          <div className="wn-header__left">
            <div className="wn-header__icon" aria-hidden="true">
              {mode === 'upgrade' ? <Sparkles size={18} strokeWidth={2.2} /> : <ScrollText size={18} strokeWidth={2.2} />}
            </div>
            <div className="wn-header__titles">
              <div className="wn-header__title-row">
                <h2 id="whats-new-title" className="wn-header__title">
                  {mode === 'upgrade' ? t('changelog.upgradeTitle') : t('changelog.archiveTitle')}
                </h2>
                {latestTag && <CodeBadge>{latestTag}</CodeBadge>}
              </div>
              <p className="wn-header__subtitle">
                {mode === 'upgrade'
                  ? t('changelog.upgradeSubtitle')
                  : t('changelog.archiveSubtitle', { count: bundle?.bundledCount ?? 0 })}
              </p>
            </div>
          </div>
          <button
            type="button"
            className="wn-close-btn"
            onClick={onClose}
            aria-label={t('common.close')}
            title={t('common.close')}
          >
            <X size={16} strokeWidth={2.2} aria-hidden="true" />
          </button>
        </header>

        {/* ── 升级态：跨版迁移聚合横幅 ── */}
        {mode === 'upgrade' && prevTag && latestTag && (
          <div className="wn-upgrade-banner" role="status">
            <div className="wn-upgrade-banner__route">
              <CodeBadge variant="muted">{prevTag}</CodeBadge>
              <ArrowRight size={14} strokeWidth={2.2} className="wn-upgrade-banner__arrow" aria-hidden="true" />
              <CodeBadge>{latestTag}</CodeBadge>
            </div>
            <span className="wn-upgrade-banner__text">
              {t('changelog.upgradeSpanBanner', {
                from: prevTag,
                to: latestTag,
                count: Math.max(1, newReleaseCount),
              })}
            </span>
          </div>
        )}

        {/* ── 归档态与大跨度升级态：实时关键词过滤栏 ── */}
        <div className="wn-toolbar">
          <div className="wn-search">
            <Search size={15} strokeWidth={2} className="wn-search__icon" aria-hidden="true" />
            <input
              type="text"
              className="wn-search__input"
              value={searchQuery}
              onChange={(e) => setSearchQuery(e.target.value)}
              placeholder={t('changelog.searchPlaceholder')}
              aria-label={t('changelog.searchPlaceholder')}
            />
            {searchQuery && (
              <button
                type="button"
                className="wn-search__clear"
                onClick={() => setSearchQuery('')}
                aria-label={t('changelog.clearSearch')}
              >
                <X size={14} strokeWidth={2.2} aria-hidden="true" />
              </button>
            )}
          </div>
          <div className="wn-toolbar__meta">
            <Layers size={14} strokeWidth={2} aria-hidden="true" />
            <span>
              {normalizedQuery
                ? t('changelog.searchMatchesCount', { count: filteredReleases.length })
                : t('changelog.showingCount', {
                    shown: displayedReleases.length,
                    total: filteredReleases.length,
                  })}
            </span>
          </div>
        </div>

        {/* ── 主体时间线滚动区 ── */}
        <div className="wn-body">
          {loading ? (
            <div className="wn-state-empty" role="status">
              <Loader2 size={20} className="wn-spin" aria-hidden="true" />
              <span>{t('common.loading')}</span>
            </div>
          ) : displayedReleases.length === 0 ? (
            <div className="wn-state-empty" role="status">
              <Search size={20} aria-hidden="true" />
              <span>{t('changelog.noSearchResults')}</span>
            </div>
          ) : (
            <div className="wn-timeline">
              {displayedReleases.map((release) => {
                const isNew = newReleaseVersionSet.has(release.version);
                const isDefaultExpanded = defaultExpandedSet.has(release.version);
                const isExpanded = Boolean(normalizedQuery) || (
                  toggledVersions.has(release.version) ? !isDefaultExpanded : isDefaultExpanded
                );
                const { block, isFallbackLocale } = resolveReleaseContent(release, preferChinese);
                const formattedDate = formatLocalDateTime(release.publishedAt);
                const relativeLabel = formatRelativeLabel(release.publishedAt);
                const kindLabelKey = release.kind === 'major'
                  ? 'changelog.kindMajor'
                  : release.kind === 'minor'
                    ? 'changelog.kindMinor'
                    : 'changelog.kindPatch';

                return (
                  <article
                    key={release.version}
                    className={`wn-release${isExpanded ? ' wn-release--expanded' : ''}${isNew ? ' wn-release--new' : ''}`}
                  >
                    <button
                      type="button"
                      className="wn-release__header"
                      aria-expanded={isExpanded}
                      onClick={() => toggleExpand(release.version)}
                    >
                      <div className="wn-release__header-left">
                        <span className="wn-release__chevron" aria-hidden="true">
                          {isExpanded ? <ChevronDown size={15} strokeWidth={2.3} /> : <ChevronRight size={15} strokeWidth={2.3} />}
                        </span>
                        <CodeBadge>{release.tag}</CodeBadge>
                        {isNew && (
                          <span className="wn-pill wn-pill--new">{t('changelog.badgeNew')}</span>
                        )}
                        <span className={`wn-pill wn-pill--${release.kind}`}>
                          {t(kindLabelKey)}
                        </span>
                      </div>

                      <div className="wn-release__header-right">
                        {formattedDate && (
                          <span className="wn-release__time">
                            <Clock size={13} strokeWidth={2} aria-hidden="true" />
                            <span className="wn-release__datetime">{formattedDate}</span>
                          </span>
                        )}
                        {relativeLabel && (
                          <span className="wn-release__relative">{relativeLabel}</span>
                        )}
                      </div>
                    </button>

                    {isExpanded && block && (
                      <div className="wn-release__content">
                        {isFallbackLocale && (
                          <p className="wn-release__fallback-note">
                            {t('changelog.fallbackLocaleNote')}
                          </p>
                        )}
                        {block.summary && (
                          <p className="wn-release__summary">{renderInlineMarkdown(block.summary)}</p>
                        )}
                        {block.sections.map((sec, sIdx) => (
                          <div key={`${release.version}-sec-${sIdx}`} className="wn-section">
                            {sec.title && (
                              <h3 className="wn-section__title">{renderInlineMarkdown(sec.title)}</h3>
                            )}
                            {sec.items.length > 0 && (
                              <ul className="wn-section__list">
                                {sec.items.map((item, iIdx) => (
                                  <li key={`${release.version}-item-${sIdx}-${iIdx}`} className="wn-section__item">
                                    {renderInlineMarkdown(item)}
                                  </li>
                                ))}
                              </ul>
                            )}
                          </div>
                        ))}
                      </div>
                    )}
                  </article>
                );
              })}

              {remainingCount > 0 && (
                <div className="wn-load-more">
                  <Button
                    variant="secondary"
                    size="sm"
                    onClick={() => setExtraVisibleCount((prev) => prev + PAGE_SIZE)}
                  >
                    <span>
                      {t('changelog.loadMoreBtn', {
                        step: Math.min(PAGE_SIZE, remainingCount),
                        remaining: remainingCount,
                      })}
                    </span>
                  </Button>
                </div>
              )}
            </div>
          )}
        </div>

        {/* ── 底部操作栏：双向自动弹窗偏好 + GitHub 外链 + 关闭确认 ── */}
        <footer className="wn-footer">
          <label className="wn-footer__pref">
            <input
              type="checkbox"
              className="wn-footer__checkbox-native"
              checked={showChangelogOnUpdate}
              disabled={savingPref}
              onChange={(e) => void handleToggleAutoShow(e.target.checked)}
            />
            <span className={`wn-footer__checkbox${showChangelogOnUpdate ? ' is-checked' : ''}`} aria-hidden="true">
              {showChangelogOnUpdate && <Check size={12} strokeWidth={3} />}
            </span>
            <span className="wn-footer__pref-label">{t('changelog.autoShowAfterUpdate')}</span>
          </label>

          <div className="wn-footer__actions">
            <Button variant="ghost" size="sm" onClick={handleOpenGitHubReleases}>
              <ExternalLink size={14} strokeWidth={2.1} aria-hidden="true" />
              <span>{t('changelog.viewAllOnGitHub')}</span>
            </Button>
            <Button variant="primary" size="sm" onClick={onClose}>
              <span>{mode === 'upgrade' ? t('changelog.gotItBtn') : t('common.close')}</span>
            </Button>
          </div>
        </footer>
      </div>
    </div>
  );
};

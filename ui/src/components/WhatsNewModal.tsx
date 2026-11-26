/* ─────────────────────────────────────────────────────────────────────────────
 * WhatsNewModal — 世界级结构化离线更新日志与跨版迁移亮点弹窗
 *
 * 核心体验特性：
 *   1. 构建期预分类 (feature / improvement / fix) + 数量统计胶囊，一眼分辨新功能与修 Bug；
 *   2. 语义分组 + 独立微卡片 + 领起短标题双层排版 + 降噪行内代码芯片，彻底告别文字墙；
 *   3. 递归 AST 行内 Markdown 解析（完美支持粗体嵌套反引号代码）+ 搜索词实时荧光高亮；
 *   4. 支持按「新功能 / 优化改进 / 问题修复」一键筛选、「详览 / 简览」密度切换、
 *      「全部展开 / 收起」、单条渐进展开、一键复制本版日志、GitHub Tag & Issue 直达。
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
  Calendar,
  ExternalLink,
  Check,
  Copy,
  Loader2,
  ArrowRight,
  Layers,
  Wrench,
  SlidersHorizontal,
  List,
  LayoutList,
  ChevronsUpDown,
  ChevronsDownUp,
  RotateCcw,
  GitPullRequest,
  HeartHandshake,
} from 'lucide-react';
import { Button, CodeBadge } from './UIKit';
import { bridgeRequest, openExternalUrl } from '../hooks/useBridge';
import {
  CHANGELOG_CATEGORY_ORDER,
  compareSemver,
  computeRelativeDayDiff,
  groupReleasesByLocalDate,
  resolveReleaseContent,
  resolveSectionCategory,
  computeBlockCategoryCounts,
  sectionMatchesQuery,
  matchesSearchQuery,
  parseInlineMarkdownAst,
  splitLeadInBullet,
  formatReleasePlainText,
  type ChangelogMode,
  type ChangelogBundle,
  type ChangelogCategory,
  type ChangelogCategoryFilter,
  type ChangelogDensity,
  type ChangelogRelease,
  type ChangelogLocaleBlock,
  type InlineAstNode,
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
const GITHUB_REPO_URL = 'https://github.com/yuan278501381/Tools3000';
const GITHUB_RELEASES_URL = `${GITHUB_REPO_URL}/releases`;

function renderHighlightedPlainText(text: string, query: string): ReactNode {
  if (!query) return text;
  const lower = text.toLowerCase();
  const qLen = query.length;
  const parts: ReactNode[] = [];
  let cursor = 0;
  let idx = lower.indexOf(query, cursor);
  let k = 0;
  while (idx !== -1) {
    if (idx > cursor) {
      parts.push(text.slice(cursor, idx));
    }
    parts.push(
      <mark key={`m-${k}`} className="wn-mark">
        {text.slice(idx, idx + qLen)}
      </mark>,
    );
    k += 1;
    cursor = idx + qLen;
    idx = lower.indexOf(query, cursor);
  }
  if (parts.length === 0) return text;
  if (cursor < text.length) {
    parts.push(text.slice(cursor));
  }
  return parts.map((p, i) => <Fragment key={i}>{p}</Fragment>);
}

function renderAstNodes(nodes: InlineAstNode[], query: string): ReactNode {
  return nodes.map((node, idx) => {
    if (node.type === 'text') {
      return <Fragment key={idx}>{renderHighlightedPlainText(node.value, query)}</Fragment>;
    }
    if (node.type === 'code') {
      return (
        <code key={idx} className="wn-inline-code">
          {renderHighlightedPlainText(node.value, query)}
        </code>
      );
    }
    return (
      <strong key={idx} className="wn-inline-strong">
        {renderAstNodes(node.children, query)}
      </strong>
    );
  });
}

function renderInlineMarkdown(text: string, highlightQuery = ''): ReactNode {
  const ast = parseInlineMarkdownAst(text);
  return renderAstNodes(ast, highlightQuery);
}

const CategoryIcon: FC<{ category: ChangelogCategory; size?: number }> = ({ category, size = 14 }) => {
  if (category === 'feature') {
    return <Sparkles size={size} strokeWidth={2.2} aria-hidden="true" />;
  }
  if (category === 'fix') {
    return <Wrench size={size} strokeWidth={2.2} aria-hidden="true" />;
  }
  return <SlidersHorizontal size={size} strokeWidth={2.2} aria-hidden="true" />;
};

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
  const [categoryFilter, setCategoryFilter] = useState<ChangelogCategoryFilter>('all');
  const [density, setDensity] = useState<ChangelogDensity>('detailed');
  const [extraVisibleCount, setExtraVisibleCount] = useState(0);
  const [toggledVersions, setToggledVersions] = useState<Set<string>>(() => new Set());
  const [expandAllOverride, setExpandAllOverride] = useState<boolean | null>(null);
  const [toggledEntries, setToggledEntries] = useState<Set<string>>(() => new Set());
  const [copiedVersion, setCopiedVersion] = useState<string | null>(null);
  const [savingPref, setSavingPref] = useState(false);
  const panelRef = useRef<HTMLDivElement>(null);
  const searchInputRef = useRef<HTMLInputElement>(null);
  const copyTimerRef = useRef<number | null>(null);
  const onCloseRef = useRef(onClose);
  const searchQueryRef = useRef(searchQuery);

  useEffect(() => {
    onCloseRef.current = onClose;
  }, [onClose]);

  useEffect(() => {
    searchQueryRef.current = searchQuery;
  }, [searchQuery]);

  const preferChinese = useMemo(() => {
    const lang = (i18n.language || '').toLowerCase();
    return lang.startsWith('zh');
  }, [i18n.language]);

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

  useEffect(() => {
    return () => {
      if (copyTimerRef.current !== null) {
        window.clearTimeout(copyTimerRef.current);
      }
    };
  }, []);

  const loading = open && bundle === null && !loadFailed;
  const allReleases = useMemo(() => bundle?.releases ?? [], [bundle]);

  const normalizedQuery = searchQuery.trim().toLowerCase();

  // 分面聚合三类条目总数：未输入关键词时统计全量版本，输入关键词时实时统计命中条目的分面数量
  const globalCategoryTotals = useMemo(() => {
    const totals = { feature: 0, improvement: 0, fix: 0, total: 0 };
    for (const rel of allReleases) {
      const { block } = resolveReleaseContent(rel, preferChinese);
      if (!block) continue;
      if (!normalizedQuery) {
        const c = computeBlockCategoryCounts(block);
        totals.feature += c.feature;
        totals.improvement += c.improvement;
        totals.fix += c.fix;
        totals.total += c.total;
        continue;
      }
      const versionMatched =
        rel.tag.toLowerCase().includes(normalizedQuery) ||
        rel.version.toLowerCase().includes(normalizedQuery);
      const matchedSections = block.sections.filter((sec) => sectionMatchesQuery(sec, normalizedQuery));
      const effectiveSections = matchedSections.length > 0
        ? matchedSections
        : versionMatched
          ? block.sections
          : [];
      for (const sec of effectiveSections) {
        const cat = resolveSectionCategory(sec);
        totals[cat] += 1;
        totals.total += 1;
      }
    }
    return totals;
  }, [allReleases, preferChinese, normalizedQuery]);

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

  // 键盘 Escape（兼容中文 IME，且先清搜索词再关窗）+ Ctrl+F 聚焦检索框
  useEffect(() => {
    if (!open) return;
    panelRef.current?.focus();
    const onKeyDown = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'f') {
        e.preventDefault();
        searchInputRef.current?.focus();
        searchInputRef.current?.select();
        return;
      }
      if (e.key === 'Escape') {
        if (e.isComposing || e.keyCode === 229) return;
        e.preventDefault();
        if (searchQueryRef.current.trim().length > 0) {
          setSearchQuery('');
          searchInputRef.current?.focus();
          return;
        }
        onCloseRef.current();
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [open]);

  const hasActiveFilter = Boolean(normalizedQuery) || categoryFilter !== 'all';

  const filteredReleases = useMemo(() => {
    if (!hasActiveFilter) return allReleases;
    return allReleases.filter((rel) =>
      matchesSearchQuery(rel, normalizedQuery, categoryFilter, preferChinese),
    );
  }, [allReleases, hasActiveFilter, normalizedQuery, categoryFilter, preferChinese]);

  const displayedReleases = useMemo(() => {
    if (normalizedQuery) return filteredReleases;
    return filteredReleases.slice(0, baseVisibleCount + extraVisibleCount);
  }, [filteredReleases, normalizedQuery, baseVisibleCount, extraVisibleCount]);

  const dayGroups = useMemo(
    () => groupReleasesByLocalDate(displayedReleases),
    [displayedReleases],
  );

  const remainingCount = normalizedQuery
    ? 0
    : Math.max(0, filteredReleases.length - displayedReleases.length);

  const isReleaseExpanded = useCallback(
    (version: string): boolean => {
      if (normalizedQuery) return true;
      const base = expandAllOverride !== null ? expandAllOverride : defaultExpandedSet.has(version);
      return toggledVersions.has(version) ? !base : base;
    },
    [normalizedQuery, expandAllOverride, defaultExpandedSet, toggledVersions],
  );

  const allDisplayedExpanded = useMemo(() => {
    if (displayedReleases.length === 0) return false;
    return displayedReleases.every((r) => isReleaseExpanded(r.version));
  }, [displayedReleases, isReleaseExpanded]);

  const toggleExpand = useCallback((version: string) => {
    setToggledVersions((prev) => {
      const next = new Set(prev);
      if (next.has(version)) next.delete(version);
      else next.add(version);
      return next;
    });
  }, []);

  const handleToggleExpandAll = useCallback(() => {
    setToggledVersions(new Set());
    setExpandAllOverride(!allDisplayedExpanded);
  }, [allDisplayedExpanded]);

  const toggleEntryExpanded = useCallback((entryKey: string) => {
    setToggledEntries((prev) => {
      const next = new Set(prev);
      if (next.has(entryKey)) next.delete(entryKey);
      else next.add(entryKey);
      return next;
    });
  }, []);

  const handleResetFilters = useCallback(() => {
    setSearchQuery('');
    setCategoryFilter('all');
    searchInputRef.current?.focus();
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

  const categoryLabels = useMemo<Record<ChangelogCategory, string>>(
    () => ({
      feature: t('changelog.categoryFeature'),
      improvement: t('changelog.categoryImprovement'),
      fix: t('changelog.categoryFix'),
    }),
    [t],
  );

  const handleCopyRelease = useCallback(
    (release: ChangelogRelease, block: ChangelogLocaleBlock | null) => {
      if (!block) return;
      const text = formatReleasePlainText(release, block, categoryLabels);
      if (navigator.clipboard?.writeText) {
        void navigator.clipboard.writeText(text).catch(() => {});
      }
      setCopiedVersion(release.version);
      if (copyTimerRef.current !== null) {
        window.clearTimeout(copyTimerRef.current);
      }
      copyTimerRef.current = window.setTimeout(() => {
        setCopiedVersion((cur) => (cur === release.version ? null : cur));
      }, 1800);
    },
    [categoryLabels],
  );

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

  const handleOpenReleaseTag = (tag: string) => {
    void openExternalUrl(`${GITHUB_RELEASES_URL}/tag/${encodeURIComponent(tag)}`);
  };

  const handleOpenIssue = (issueNumber: number) => {
    void openExternalUrl(`${GITHUB_REPO_URL}/issues/${issueNumber}`);
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

        {/* ── 双层智能工具栏：搜索 + 阅读密度 + 全展控 + 三元语义类型筛选统计 ── */}
        <div className="wn-toolbar">
          <div className="wn-toolbar__row">
            <div className="wn-search">
              <Search size={15} strokeWidth={2} className="wn-search__icon" aria-hidden="true" />
              <input
                ref={searchInputRef}
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

            <div className="wn-toolbar__controls">
              <div className="wn-segmented" role="group" aria-label={t('changelog.densityDetailed')}>
                <button
                  type="button"
                  className={`wn-segmented__btn${density === 'detailed' ? ' is-active' : ''}`}
                  aria-pressed={density === 'detailed'}
                  onClick={() => {
                    setDensity('detailed');
                    setToggledEntries(new Set());
                  }}
                >
                  <LayoutList size={14} strokeWidth={2.1} aria-hidden="true" />
                  <span>{t('changelog.densityDetailed')}</span>
                </button>
                <button
                  type="button"
                  className={`wn-segmented__btn${density === 'compact' ? ' is-active' : ''}`}
                  aria-pressed={density === 'compact'}
                  onClick={() => {
                    setDensity('compact');
                    setToggledEntries(new Set());
                  }}
                >
                  <List size={14} strokeWidth={2.1} aria-hidden="true" />
                  <span>{t('changelog.densityCompact')}</span>
                </button>
              </div>

              {!normalizedQuery && displayedReleases.length > 1 && (
                <button
                  type="button"
                  className="wn-icon-text-btn"
                  onClick={handleToggleExpandAll}
                >
                  {allDisplayedExpanded ? (
                    <ChevronsDownUp size={14} strokeWidth={2.1} aria-hidden="true" />
                  ) : (
                    <ChevronsUpDown size={14} strokeWidth={2.1} aria-hidden="true" />
                  )}
                  <span>
                    {allDisplayedExpanded ? t('changelog.collapseAll') : t('changelog.expandAll')}
                  </span>
                </button>
              )}
            </div>
          </div>

          <div className="wn-toolbar__filters">
            <div className="wn-filter-pills" role="tablist" aria-label={t('changelog.filterAll')}>
              <button
                type="button"
                role="tab"
                aria-selected={categoryFilter === 'all'}
                className={`wn-filter-pill wn-filter-pill--all${categoryFilter === 'all' ? ' is-active' : ''}`}
                onClick={() => setCategoryFilter('all')}
              >
                <span>{t('changelog.filterAll')}</span>
                <span className="wn-filter-pill__count">{globalCategoryTotals.total}</span>
              </button>

              <button
                type="button"
                role="tab"
                aria-selected={categoryFilter === 'feature'}
                className={`wn-filter-pill wn-filter-pill--feature${categoryFilter === 'feature' ? ' is-active' : ''}`}
                onClick={() => setCategoryFilter(categoryFilter === 'feature' ? 'all' : 'feature')}
              >
                <CategoryIcon category="feature" size={13} />
                <span>{t('changelog.categoryFeature')}</span>
                <span className="wn-filter-pill__count">{globalCategoryTotals.feature}</span>
              </button>

              <button
                type="button"
                role="tab"
                aria-selected={categoryFilter === 'improvement'}
                className={`wn-filter-pill wn-filter-pill--improvement${categoryFilter === 'improvement' ? ' is-active' : ''}`}
                onClick={() => setCategoryFilter(categoryFilter === 'improvement' ? 'all' : 'improvement')}
              >
                <CategoryIcon category="improvement" size={13} />
                <span>{t('changelog.categoryImprovement')}</span>
                <span className="wn-filter-pill__count">{globalCategoryTotals.improvement}</span>
              </button>

              <button
                type="button"
                role="tab"
                aria-selected={categoryFilter === 'fix'}
                className={`wn-filter-pill wn-filter-pill--fix${categoryFilter === 'fix' ? ' is-active' : ''}`}
                onClick={() => setCategoryFilter(categoryFilter === 'fix' ? 'all' : 'fix')}
              >
                <CategoryIcon category="fix" size={13} />
                <span>{t('changelog.categoryFix')}</span>
                <span className="wn-filter-pill__count">{globalCategoryTotals.fix}</span>
              </button>
            </div>

            <div className="wn-toolbar__meta">
              <Layers size={14} strokeWidth={2} aria-hidden="true" />
              <span>
                {hasActiveFilter
                  ? t('changelog.searchMatchesCount', { count: filteredReleases.length })
                  : t('changelog.showingCount', {
                      shown: displayedReleases.length,
                      total: filteredReleases.length,
                    })}
              </span>
            </div>
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
              {hasActiveFilter && (
                <Button variant="secondary" size="sm" onClick={handleResetFilters}>
                  <RotateCcw size={13} strokeWidth={2.2} aria-hidden="true" />
                  <span>{t('changelog.clearFilters')}</span>
                </Button>
              )}
            </div>
          ) : (
            <div className="wn-timeline">
              {dayGroups.map((dayGroup, groupIdx) => {
                const relativeLabel = dayGroup.dateKey ? formatRelativeLabel(dayGroup.sampleIso) : '';
                const dayCount = dayGroup.releases.length;

                return (
                  <section
                    key={dayGroup.dateKey || `undated-${groupIdx}`}
                    className="wn-day-group"
                    aria-label={dayGroup.dateKey || t('changelog.archiveTitle')}
                  >
                    {dayGroup.dateKey && (
                      <header className="wn-day-group__header">
                        <span className="wn-day-group__date">
                          <Calendar size={14} strokeWidth={2.1} aria-hidden="true" />
                          <span className="wn-day-group__date-text">{dayGroup.dateKey}</span>
                        </span>
                        {relativeLabel && (
                          <span className="wn-day-group__relative">{relativeLabel}</span>
                        )}
                        <span className="wn-day-group__rule" aria-hidden="true" />
                        {dayCount > 1 && (
                          <span className="wn-day-group__count">
                            {t('changelog.dayReleaseCount', { count: dayCount })}
                          </span>
                        )}
                      </header>
                    )}

                    <div className="wn-day-group__rail">
                      {dayGroup.releases.map((release) => {
                        const isNew = newReleaseVersionSet.has(release.version);
                        const isExpanded = isReleaseExpanded(release.version);
                        const { block, isFallbackLocale } = resolveReleaseContent(release, preferChinese);
                        const counts = computeBlockCategoryCounts(block);
                        const isMajorOrMinor = release.kind === 'major' || release.kind === 'minor';
                        const kindLabelKey = release.kind === 'major'
                          ? 'changelog.kindMajor'
                          : 'changelog.kindMinor';

                        // 计算该版本下当前可见的语义分组（遵循分类过滤 + 关键词聚焦）
                        const groupedCategories = block
                          ? CHANGELOG_CATEGORY_ORDER.map((cat) => {
                              if (categoryFilter !== 'all' && categoryFilter !== cat) {
                                return { category: cat, entries: [] };
                              }
                              const catEntries = block.sections
                                .map((sec, origIdx) => ({ sec, origIdx }))
                                .filter(({ sec }) => resolveSectionCategory(sec) === cat);

                              if (!normalizedQuery) {
                                return { category: cat, entries: catEntries };
                              }
                              // 若搜索词直接命中版本号，保留该版内全部分组条目；否则优先聚焦匹配条目
                              const versionMatched =
                                release.tag.toLowerCase().includes(normalizedQuery) ||
                                release.version.toLowerCase().includes(normalizedQuery);
                              const matched = catEntries.filter(({ sec }) => sectionMatchesQuery(sec, normalizedQuery));
                              return {
                                category: cat,
                                entries: matched.length > 0 ? matched : versionMatched ? catEntries : [],
                              };
                            }).filter((g) => g.entries.length > 0)
                          : [];

                        const isCopied = copiedVersion === release.version;

                        return (
                          <article
                            key={release.version}
                            className={`wn-release${isExpanded ? ' wn-release--expanded' : ''}${isNew ? ' wn-release--new' : ''}`}
                          >
                            <div className="wn-release__bar">
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
                                  {isMajorOrMinor && (
                                    <span className={`wn-pill wn-pill--${release.kind}`}>
                                      {t(kindLabelKey)}
                                    </span>
                                  )}

                                  {/* 语义分类数量微晶胶囊：不点开也能一眼看清本版几项新功能、几项优化、几项修复 */}
                                  <div className="wn-release__counts">
                                    {counts.feature > 0 && (
                                      <span className="wn-stat-pill wn-stat-pill--feature">
                                        <CategoryIcon category="feature" size={12} />
                                        <span>{t('changelog.badgeFeatureCount', { count: counts.feature })}</span>
                                      </span>
                                    )}
                                    {counts.improvement > 0 && (
                                      <span className="wn-stat-pill wn-stat-pill--improvement">
                                        <CategoryIcon category="improvement" size={12} />
                                        <span>{t('changelog.badgeImprovementCount', { count: counts.improvement })}</span>
                                      </span>
                                    )}
                                    {counts.fix > 0 && (
                                      <span className="wn-stat-pill wn-stat-pill--fix">
                                        <CategoryIcon category="fix" size={12} />
                                        <span>{t('changelog.badgeFixCount', { count: counts.fix })}</span>
                                      </span>
                                    )}
                                  </div>
                                </div>
                              </button>

                              {/* 轻量右侧快捷操作（复制本版日志 / 直达 GitHub Tag） */}
                              <div className="wn-release__quick-actions">
                                <button
                                  type="button"
                                  className={`wn-release__action-btn${isCopied ? ' is-success' : ''}`}
                                  onClick={() => handleCopyRelease(release, block)}
                                  title={isCopied ? t('changelog.copiedRelease') : t('changelog.copyRelease')}
                                  aria-label={isCopied ? t('changelog.copiedRelease') : t('changelog.copyRelease')}
                                >
                                  {isCopied ? (
                                    <Check size={14} strokeWidth={2.4} aria-hidden="true" />
                                  ) : (
                                    <Copy size={14} strokeWidth={2} aria-hidden="true" />
                                  )}
                                </button>
                                <button
                                  type="button"
                                  className="wn-release__action-btn"
                                  onClick={() => handleOpenReleaseTag(release.tag)}
                                  title={t('changelog.openReleaseOnGitHub')}
                                  aria-label={t('changelog.openReleaseOnGitHub')}
                                >
                                  <ExternalLink size={14} strokeWidth={2} aria-hidden="true" />
                                </button>
                              </div>
                            </div>

                            {isExpanded && block && (
                              <div className="wn-release__content">
                                {isFallbackLocale && (
                                  <p className="wn-release__fallback-note">
                                    {t('changelog.fallbackLocaleNote')}
                                  </p>
                                )}
                                {block.summary && (
                                  <p className="wn-release__summary">
                                    {renderInlineMarkdown(block.summary, normalizedQuery)}
                                  </p>
                                )}

                                <div className="wn-groups">
                                  {groupedCategories.map(({ category, entries }) => (
                                    <section
                                      key={`${release.version}-cat-${category}`}
                                      className={`wn-group wn-group--${category}`}
                                      aria-label={categoryLabels[category]}
                                    >
                                      <div className="wn-group__header">
                                        <span className={`wn-group__badge wn-group__badge--${category}`}>
                                          <CategoryIcon category={category} size={13} />
                                          <span>{categoryLabels[category]}</span>
                                        </span>
                                        <span className="wn-group__count">
                                          {t('changelog.itemsCount', { count: entries.length })}
                                        </span>
                                        <span className="wn-group__rule" aria-hidden="true" />
                                      </div>

                                      <div className="wn-group__cards">
                                        {entries.map(({ sec, origIdx }) => {
                                          const entryKey = `${release.version}-${origIdx}`;
                                          const hasDetails = sec.items.length > 0;
                                          const defaultShowBody = density === 'detailed' || Boolean(normalizedQuery);
                                          const showBody = hasDetails && (
                                            toggledEntries.has(entryKey) ? !defaultShowBody : defaultShowBody
                                          );

                                          return (
                                            <div
                                              key={entryKey}
                                              className={`wn-entry wn-entry--${category}${showBody ? ' is-open' : ' is-compact'}`}
                                            >
                                              <div className="wn-entry__top">
                                                {hasDetails ? (
                                                  <button
                                                    type="button"
                                                    className="wn-entry__toggle"
                                                    aria-expanded={showBody}
                                                    aria-label={t('changelog.toggleDetails')}
                                                    onClick={() => toggleEntryExpanded(entryKey)}
                                                  >
                                                    <span className={`wn-entry__dot wn-entry__dot--${category}`} aria-hidden="true" />
                                                    <span className="wn-entry__title">
                                                      {renderInlineMarkdown(sec.title, normalizedQuery)}
                                                    </span>
                                                    <span className="wn-entry__caret" aria-hidden="true">
                                                      {showBody ? <ChevronDown size={14} strokeWidth={2.2} /> : <ChevronRight size={14} strokeWidth={2.2} />}
                                                    </span>
                                                  </button>
                                                ) : (
                                                  <div className="wn-entry__static">
                                                    <span className={`wn-entry__dot wn-entry__dot--${category}`} aria-hidden="true" />
                                                    <span className="wn-entry__title">
                                                      {renderInlineMarkdown(sec.title, normalizedQuery)}
                                                    </span>
                                                  </div>
                                                )}

                                                {sec.attribution && (
                                                  sec.attribution.issueNumber ? (
                                                    <button
                                                      type="button"
                                                      className="wn-attr-chip wn-attr-chip--link"
                                                      onClick={() => handleOpenIssue(sec.attribution!.issueNumber!)}
                                                      title={t('changelog.openIssueOnGitHub', { num: sec.attribution.issueNumber })}
                                                    >
                                                      <GitPullRequest size={12} strokeWidth={2.2} aria-hidden="true" />
                                                      <span>{renderHighlightedPlainText(sec.attribution.label, normalizedQuery)}</span>
                                                    </button>
                                                  ) : (
                                                    <span className="wn-attr-chip">
                                                      <HeartHandshake size={12} strokeWidth={2.1} aria-hidden="true" />
                                                      <span>{renderHighlightedPlainText(sec.attribution.label, normalizedQuery)}</span>
                                                    </span>
                                                  )
                                                )}
                                              </div>

                                              {showBody && (
                                                <ul className="wn-entry__list">
                                                  {sec.items.map((item, iIdx) => {
                                                    const { lead, body } = splitLeadInBullet(item);
                                                    return (
                                                      <li key={`${entryKey}-i-${iIdx}`} className="wn-entry__item">
                                                        {lead ? (
                                                          <>
                                                            <span className="wn-entry__lead">
                                                              {renderInlineMarkdown(lead, normalizedQuery)}
                                                            </span>
                                                            <span className="wn-entry__sep" aria-hidden="true">·</span>
                                                            <span className="wn-entry__desc">
                                                              {renderInlineMarkdown(body, normalizedQuery)}
                                                            </span>
                                                          </>
                                                        ) : (
                                                          <span className="wn-entry__desc">
                                                            {renderInlineMarkdown(body, normalizedQuery)}
                                                          </span>
                                                        )}
                                                      </li>
                                                    );
                                                  })}
                                                </ul>
                                              )}
                                            </div>
                                          );
                                        })}
                                      </div>
                                    </section>
                                  ))}
                                </div>
                              </div>
                            )}
                          </article>
                        );
                      })}
                    </div>
                  </section>
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

/* ─────────────────────────────────────────────────────────────────────────────
 * HotkeysPage — 全局快捷键一览独立管理页（系统总控固定第 3 菜单）
 *
 * 核心能力:
 *   1. 实时健康度概览（全部 / 生效中 / 仅录制态 / 内部冲突 / 外部冲突 / 未绑定）+ 一键复检；
 *   2. 页内即时过滤（按功能名称或快捷键组合快速检索）；
 *   3. 严格按 hotkeyCatalog 6 大业务域有序分组（含独立「窗口管理」分组）；
 *   4. 支持单条热键重绑、单热键专业软件自动让位弹窗与全局快捷键自动让位应用列表。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, useMemo, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  Keyboard,
  RefreshCw,
  CheckCircle2,
  Layers,
  AlertTriangle,
  AlertOctagon,
  MinusCircle,
  Shield,
  Search,
  X,
} from 'lucide-react';
import {
  Card,
  SettingRow,
  SettingGroup,
  Button,
  Modal,
  PageSkeleton,
} from '../components/UIKit';
import { HotkeyAppExclusionBar } from '../components/business/HotkeyAppExclusionBar';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import { groupHotkeysByDomain, resolveHotkeyLabel, sortHotkeysByPriority } from '../config/hotkeyCatalog';
import './HotkeysPage.css';

interface OperationResult {
  success: boolean;
  error?: string;
  shortcut?: string;
}

export const HotkeysPage: FC = () => {
  const { t } = useTranslation();
  const [loading, setLoading] = useState(true);
  const [refreshingHotkeys, setRefreshingHotkeys] = useState(false);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [globalExcludedApps, setGlobalExcludedApps] = useState<string[]>([]);
  const [editingExclusionHotkey, setEditingExclusionHotkey] = useState<HotkeyEntry | null>(null);
  const [filterText, setFilterText] = useState('');

  useEffect(() => {
    let cancelled = false;
    Promise.all([
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll'),
      bridgeRequest<{ global?: string[] }>('hotkey.getExclusions').catch(() => ({ global: [] })),
    ])
      .then(([hotkeyData, exclusionData]) => {
        if (cancelled) return;
        setHotkeys(Array.isArray(hotkeyData) ? sortHotkeysByPriority(hotkeyData) : []);
        if (exclusionData && Array.isArray(exclusionData.global)) {
          setGlobalExcludedApps(exclusionData.global);
        }
      })
      .catch((error) => {
        if (cancelled) return;
        console.error(error);
        toast.error(t('general.loadFailed'));
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => {
      cancelled = true;
    };
    // 仅挂载时初始化拉取一次，避免切换语言更新 t 引用导致重复 IPC
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useBridgeEvent('plugins:changed', () => {
    void Promise.all([
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll'),
      bridgeRequest<{ global?: string[] }>('hotkey.getExclusions').catch(() => ({ global: [] })),
    ])
      .then(([hotkeyData, exclusionData]) => {
        setHotkeys(Array.isArray(hotkeyData) ? sortHotkeysByPriority(hotkeyData) : []);
        if (exclusionData && Array.isArray(exclusionData.global)) {
          setGlobalExcludedApps(exclusionData.global);
        }
      })
      .catch(() => {
        // 静默忽略插件列表热刷新偶发异常
      });
  });

  const refreshHotkeys = useCallback(async () => {
    setRefreshingHotkeys(true);
    try {
      const data = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      setHotkeys(Array.isArray(data) ? sortHotkeysByPriority(data) : []);
      toast.success(t('general.recheckShortcuts'));
    } catch {
      toast.error(t('general.loadFailed'));
    } finally {
      setRefreshingHotkeys(false);
    }
  }, [t]);

  const rebindHotkey = async (entry: HotkeyEntry, shortcut: string) => {
    const previous = entry.shortcut;
    const previousRegistered = entry.registered;
    setHotkeys((items) =>
      items.map((item) => (item.name === entry.name ? { ...item, shortcut } : item)),
    );
    try {
      const result = await bridgeRequest<OperationResult>('hotkey.rebind', {
        name: entry.name,
        hotkey: shortcut,
      });
      if (!result.success) throw new Error(result.error || t('hotkey.bindFailed'));
      const applied = result.shortcut ?? shortcut;
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      setHotkeys(Array.isArray(refreshed) ? sortHotkeysByPriority(refreshed) : []);
      const appliedDouble = /^Double\+(Ctrl|Alt|Shift)$/i.exec(applied);
      const displayApplied = appliedDouble
        ? t('hotkey.doubleTapDisplay', 'Double {{key}}', { key: appliedDouble[1] })
        : applied;
      toast.success(`${resolveHotkeyLabel(t, entry.name)}: ${displayApplied || t('general.shortcutDisabled')}`);
    } catch (error) {
      setHotkeys((items) =>
        items.map((item) =>
          item.name === entry.name && item.shortcut === shortcut
            ? { ...item, shortcut: previous, registered: previousRegistered }
            : item,
        ),
      );
      toast.error(t('hotkey.bindFailed'), { description: String(error) });
    }
  };

  const handleSaveGlobalExclusions = async (nextApps: string[]) => {
    const previous = globalExcludedApps;
    setGlobalExcludedApps(nextApps);
    try {
      const res = await bridgeRequest<OperationResult & { apps?: string[] }>('hotkey.setGlobalExclusions', {
        apps: nextApps,
      });
      if (res && res.success === false) {
        throw new Error(res.error || t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'));
      }
      if (res && Array.isArray(res.apps)) {
        setGlobalExcludedApps(res.apps);
      }
    } catch (error) {
      setGlobalExcludedApps(previous);
      toast.error(t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'), {
        description: String(error),
      });
    }
  };

  const handleSavePerHotkeyExclusions = async (hotkeyName: string, nextApps: string[]) => {
    try {
      const res = await bridgeRequest<OperationResult>('hotkey.setHotkeyExclusions', {
        name: hotkeyName,
        apps: nextApps,
      });
      if (res && res.success === false) {
        throw new Error(res.error || t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'));
      }
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) {
        const sorted = sortHotkeysByPriority(refreshed);
        setHotkeys(sorted);
        const updated = sorted.find((item) => item.name === hotkeyName) ?? null;
        setEditingExclusionHotkey(updated);
      }
    } catch (error) {
      toast.error(t('hotkeyExclusion.saveFailed', 'Failed to save excluded apps'), {
        description: String(error),
      });
    }
  };

  const internalConflictsCount = hotkeys.filter((h) => h.conflictType === 'internal').length;
  const externalConflictsCount = hotkeys.filter(
    (h) => h.conflictType === 'external' || (h.registered === false && Boolean(h.shortcut) && h.armed !== false),
  ).length;
  const sessionOnlyCount = hotkeys.filter(
    (h) =>
      Boolean(h.shortcut) &&
      h.armed === false &&
      !h.conflict &&
      h.conflictType !== 'external' &&
      h.conflictType !== 'internal',
  ).length;
  const globalActiveCount = hotkeys.filter(
    (h) =>
      Boolean(h.shortcut) &&
      h.registered !== false &&
      h.armed !== false &&
      !h.conflict &&
      h.conflictType !== 'internal' &&
      h.conflictType !== 'external',
  ).length;
  const unboundCount = hotkeys.filter((h) => !h.shortcut).length;

  const filteredHotkeys = useMemo(() => {
    const q = filterText.trim().toLowerCase();
    if (!q) return hotkeys;
    return hotkeys.filter((hk) => {
      const label = resolveHotkeyLabel(t, hk.name).toLowerCase();
      const rawShortcut = hk.shortcut || '';
      const shortcut = rawShortcut.toLowerCase();
      const doubleMatch = /^Double\+(Ctrl|Alt|Shift)$/i.exec(rawShortcut);
      const displayShortcut = doubleMatch
        ? t('hotkey.doubleTapDisplay', 'Double {{key}}', { key: doubleMatch[1] }).toLowerCase()
        : shortcut;
      const rawName = hk.name.toLowerCase();
      return label.includes(q) || shortcut.includes(q) || displayShortcut.includes(q) || rawName.includes(q);
    });
  }, [filterText, hotkeys, t]);

  const sections = useMemo(() => groupHotkeysByDomain(filteredHotkeys), [filteredHotkeys]);

  if (loading) {
    return <PageSkeleton cards={2} rowsPerCard={6} />;
  }

  return (
    <div className="hotkeys-page" style={{ animation: 'fadeIn 0.25s ease' }}>
      <SettingGroup title={t('general.keyboardShortcuts')} icon={<Keyboard size={20} strokeWidth={2.5} />}>
        <Card>
          <div className="hotkeys-page__toolbar">
            <div className="hotkeys-page__health-bar">
              <div className="hotkeys-page__health-stats">
                <span className="hotkeys-page__stat-pill total">
                  <Keyboard size={12} aria-hidden="true" />{' '}
                  {t('general.hotkeysAllCount', 'All {{count}} items', { count: hotkeys.length })}
                </span>
                <span className="hotkeys-page__stat-pill ok">
                  <CheckCircle2 size={12} aria-hidden="true" />{' '}
                  {t('general.hotkeysActiveCount', 'Active {{count}} items', { count: globalActiveCount })}
                </span>
                {sessionOnlyCount > 0 && (
                  <span className="hotkeys-page__stat-pill session">
                    <Layers size={12} aria-hidden="true" />{' '}
                    {t('general.hotkeysSessionCount', 'Scoped {{count}} items', { count: sessionOnlyCount })}
                  </span>
                )}
                {internalConflictsCount > 0 && (
                  <span className="hotkeys-page__stat-pill warning">
                    <AlertTriangle size={12} aria-hidden="true" />{' '}
                    {t('general.hotkeysInternalConflictsCount', 'Internal Conflicts {{count}} items', {
                      count: internalConflictsCount,
                    })}
                  </span>
                )}
                {externalConflictsCount > 0 && (
                  <span className="hotkeys-page__stat-pill danger">
                    <AlertOctagon size={12} aria-hidden="true" />{' '}
                    {t('general.hotkeysExternalConflictsCount', 'External Conflicts {{count}} items', {
                      count: externalConflictsCount,
                    })}
                  </span>
                )}
                {unboundCount > 0 && (
                  <span className="hotkeys-page__stat-pill unbound">
                    <MinusCircle size={12} aria-hidden="true" />{' '}
                    {t('general.hotkeysUnboundCount', 'Unbound {{count}} items', { count: unboundCount })}
                  </span>
                )}
              </div>
              <Button
                variant="ghost"
                className="hotkeys-page__recheck-btn"
                onClick={() => void refreshHotkeys()}
                disabled={refreshingHotkeys}
                title={t('general.recheckShortcuts')}
              >
                <RefreshCw size={12} className={refreshingHotkeys ? 'hotkeys-page__spin' : ''} aria-hidden="true" />
                <span>{t('general.recheckShortcuts')}</span>
              </Button>
            </div>

            {hotkeys.length > 0 && (
              <div className="hotkeys-page__search-box">
                <Search size={14} className="hotkeys-page__search-icon" aria-hidden="true" />
                <input
                  type="text"
                  className="hotkeys-page__search-input"
                  value={filterText}
                  onChange={(e) => setFilterText(e.target.value)}
                  placeholder={t('hotkeysPage.searchPlaceholder', 'Filter by feature name or shortcut (e.g. Win, F1, Screenshot)...')}
                  aria-label={t('hotkeysPage.searchPlaceholder', 'Filter by feature name or shortcut (e.g. Win, F1, Screenshot)...')}
                />
                {filterText && (
                  <button
                    type="button"
                    className="hotkeys-page__search-clear"
                    onClick={() => setFilterText('')}
                    aria-label={t('common.close')}
                  >
                    <X size={13} aria-hidden="true" />
                  </button>
                )}
              </div>
            )}
          </div>

          {hotkeys.length === 0 ? (
            <div className="hotkeys-page__empty">{t('general.noShortcuts')}</div>
          ) : sections.length === 0 ? (
            <div className="hotkeys-page__empty">
              {t('hotkeysPage.noMatch', 'No matching shortcuts found')}
            </div>
          ) : (
            <div className="hotkeys-page__sections">
              {sections.map((section) => (
                <section
                  key={section.id}
                  className="hotkeys-page__section"
                  aria-label={String(t(section.titleKey as never, { defaultValue: section.defaultTitle }))}
                >
                  <div className="hotkeys-page__section-header">
                    <span className="hotkeys-page__section-title">
                      {String(t(section.titleKey as never, { defaultValue: section.defaultTitle }))}
                    </span>
                    <span className="hotkeys-page__section-count">{section.items.length}</span>
                  </div>
                  <div className="hotkeys-page__list">
                    {section.items.map((hk) => {
                      const isInternal = hk.conflictType === 'internal';
                      const isExternal =
                        hk.conflictType === 'external' ||
                        (hk.registered === false && Boolean(hk.shortcut) && hk.armed !== false);

                      return (
                        <div
                          key={hk.name}
                          className={`hotkeys-page__item ${isInternal ? 'is-internal' : ''} ${isExternal ? 'is-external' : ''}`}
                        >
                          <div className="hotkeys-page__info">
                            <div className="hotkeys-page__name-row">
                              <span className="hotkeys-page__name">{resolveHotkeyLabel(t, hk.name)}</span>
                              <HotkeyStatusBadge entry={hk} />
                            </div>
                          </div>
                          <div className="hotkeys-page__control">
                            <button
                              type="button"
                              className={`hotkeys-page__exclude-btn ${(hk.excludedApps?.length ?? 0) > 0 ? 'has-apps' : ''}`}
                              onClick={() => setEditingExclusionHotkey(hk)}
                              title={
                                (hk.excludedApps?.length ?? 0) > 0
                                  ? t('hotkeyExclusion.badgeTooltip', 'Auto-yields in: {{apps}}', {
                                      apps: hk.excludedApps!.join(', '),
                                    })
                                  : t('hotkeyExclusion.configurePerHotkeyTip', 'Configure per-app auto-yield rules')
                              }
                            >
                              <Shield size={13} aria-hidden="true" />
                              {(hk.excludedApps?.length ?? 0) > 0 && (
                                <span>
                                  {t('hotkeyExclusion.excludedCountBadge', 'Yields {{count}}', {
                                    count: hk.excludedApps!.length,
                                  })}
                                </span>
                              )}
                            </button>
                            <HotkeyRecorder
                              id={`hotkeys-page-${hk.name.replace(/\s+/g, '-').toLowerCase()}`}
                              value={hk.shortcut}
                              ariaLabel={resolveHotkeyLabel(t, hk.name)}
                              onChange={(value) => void rebindHotkey(hk, value)}
                            />
                          </div>
                        </div>
                      );
                    })}
                  </div>
                </section>
              ))}
            </div>
          )}
        </Card>
      </SettingGroup>

      <SettingGroup
        title={t('general.globalHotkeyExclusionsTitle', 'Global Hotkey Auto-Yield Apps')}
        icon={<Shield size={19} strokeWidth={2.3} />}
      >
        <Card>
          <SettingRow
            layout="vertical"
            label={t('general.globalHotkeyExclusionsTitle', 'Global Hotkey Auto-Yield Apps')}
            description={t(
              'general.globalHotkeyExclusionsDesc',
              'Automatically yields all Tools3000 global shortcuts when any listed app is focused, and restores them immediately on focus switch',
            )}
          >
            <HotkeyAppExclusionBar
              apps={globalExcludedApps}
              compact
              emptyText={t('hotkeyExclusion.emptyGlobal', 'No global do-not-disturb apps configured')}
              pickerTitle={t('hotkeyExclusion.pickerGlobalTitle', 'Pick App to Auto-Yield All Global Shortcuts')}
              onChange={(nextApps) => void handleSaveGlobalExclusions(nextApps)}
            />
          </SettingRow>
        </Card>
      </SettingGroup>

      {editingExclusionHotkey && (
        <Modal
          open
          title={t('hotkeyExclusion.modalTitle', 'Auto-Yield Apps — {{name}}', {
            name: resolveHotkeyLabel(t, editingExclusionHotkey.name),
          })}
          onClose={() => setEditingExclusionHotkey(null)}
          footer={
            <Button variant="primary" onClick={() => setEditingExclusionHotkey(null)}>
              {t('common.done', 'Done')}
            </Button>
          }
        >
          <p className="hotkeys-page__exclusion-modal-desc">
            {t(
              'hotkeyExclusion.modalDesc',
              'When any listed process is in the foreground, only this shortcut is released to the active app while all other Tools3000 shortcuts stay active.',
            )}
          </p>
          <HotkeyAppExclusionBar
            apps={editingExclusionHotkey.excludedApps ?? []}
            emptyText={t('hotkeyExclusion.emptyPerHotkey', 'Active in all apps; add process names to auto-yield')}
            pickerTitle={t('hotkeyExclusion.pickerPerHotkeyTitle', 'Pick App to Auto-Yield This Shortcut')}
            onChange={(nextApps) => void handleSavePerHotkeyExclusions(editingExclusionHotkey.name, nextApps)}
          />
        </Modal>
      )}
    </div>
  );
};

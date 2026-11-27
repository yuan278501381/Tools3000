/* ─────────────────────────────────────────────────────────────────────────────
 * WorkspaceWorkbench.tsx — Workspaces v2 双层交互式桌面工作台
 *
 * 复刻并超越 PowerToys Workspaces (图 1 / 3 / 4 / 5 / 6):
 *   1. 列表视图: 搜索 + 排序（上次启动/名称/应用数）+ 「+ 创建工作区」触发纯 Rust 4px 正红细边摆拍
 *      + 横向真实应用彩色图标卡片 + 启动/编辑/创建桌面快捷方式/删除
 *   2. 编辑工作台: 顶部多屏双层微缩舞台（屏幕内真实窗坐标叠层 + 底部任务栏最小化图标条），
 *      与下方分组应用列表双向实时悬停高亮联动；支持手风琴展开精调管理员/CLI/状态/坐标/分区槽位/置顶/透明度
 * ───────────────────────────────────────────────────────────────────────────── */

import { useMemo, useState, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import {
  AppWindow,
  ArrowLeft,
  Camera,
  ChevronDown,
  ChevronUp,
  Clock,
  ExternalLink,
  Layers,
  Loader2,
  Monitor,
  Pencil,
  Pin,
  Play,
  Plus,
  RotateCcw,
  Save,
  Search,
  ShieldCheck,
  Trash2,
  X,
} from 'lucide-react';
import {
  Badge,
  Button,
  Card,
  CodeBadge,
  NumberInput,
  Select,
  SettingRow,
  TextInput,
  Toggle,
} from '../UIKit';
import type {
  MonitorDescriptor,
  SnapZoneLayout,
  UnrelatedWindowAction,
  WorkspaceAppEntry,
  WorkspaceItem,
} from '../../types/windowManager';
import './WorkspaceWorkbench.css';

type SortKey = 'lastLaunched' | 'name' | 'appCount';
type AppWindowState = 'normal' | 'maximized' | 'minimized';

export interface WorkspaceWorkbenchProps {
  items: WorkspaceItem[];
  unrelatedAction: UnrelatedWindowAction;
  minimizeUnrelated: boolean;
  monitors: MonitorDescriptor[];
  layouts: SnapZoneLayout[];
  busyAction: string | null;
  editingWorkspace: WorkspaceItem | null;
  isNewDraft: boolean;
  onUpdateGlobalUnrelatedAction: (action: UnrelatedWindowAction) => void;
  onStartRedFrameCapture: (targetWorkspaceId?: string) => void;
  onQuickCaptureDesktop: (name: string) => void;
  onLaunchWorkspace: (workspaceId: string) => void;
  onDeleteWorkspace: (workspaceId: string) => void;
  onCreateDesktopShortcut: (workspaceId: string, name: string) => void;
  onOpenEditor: (ws: WorkspaceItem, isNew?: boolean) => void;
  onChangeEditingWorkspace: (next: WorkspaceItem) => void;
  onSaveEditingWorkspace: () => void;
  onCancelEditingWorkspace: () => void;
  onRestoreEditingSnapshot: () => void;
  canRestoreSnapshot: boolean;
}

function getAppWindowState(app: WorkspaceAppEntry): AppWindowState {
  if (app.minimized) return 'minimized';
  if (app.maximized) return 'maximized';
  return 'normal';
}

function computeZoneCountForLayout(layout: SnapZoneLayout | undefined): number {
  if (!layout) return 0;
  if (layout.type === 'none') return 0;
  if (layout.type === 'focus') return Math.max(1, layout.zoneCount ?? 3);
  if (layout.type === 'canvas') return layout.zones?.length ?? 0;
  if (Array.isArray(layout.cellMap) && layout.cellMap.length > 0) {
    const uniq = new Set<number>();
    for (const row of layout.cellMap) {
      for (const cell of row) uniq.add(cell);
    }
    return uniq.size;
  }
  return Math.max(1, (layout.rows ?? 1) * (layout.columns ?? 2));
}

function getNormalizedStageBox(
  app: WorkspaceAppEntry,
  mon?: MonitorDescriptor,
): { left: number; top: number; width: number; height: number } {
  if (app.maximized) {
    return { left: 2, top: 2, width: 96, height: 96 };
  }
  if (app.relRect) {
    const rx = app.relRect.relX ?? app.relRect.x;
    const ry = app.relRect.relY ?? app.relRect.y;
    const rw = app.relRect.relW ?? app.relRect.w;
    const rh = app.relRect.relH ?? app.relRect.h;
    if (
      typeof rx === 'number' &&
      typeof ry === 'number' &&
      typeof rw === 'number' &&
      typeof rh === 'number' &&
      rw > 0 &&
      rh > 0
    ) {
      const w = Math.max(14, Math.min(98, rw * 100));
      const h = Math.max(16, Math.min(98, rh * 100));
      const left = Math.max(1, Math.min(99 - w, rx * 100));
      const top = Math.max(1, Math.min(99 - h, ry * 100));
      return { left, top, width: w, height: h };
    }
  }
  if (app.rect && app.rect.w > 0 && app.rect.h > 0) {
    const monW = mon?.workArea?.w ?? mon?.width ?? 1920;
    const monH = mon?.workArea?.h ?? mon?.height ?? 1080;
    const monX = mon?.workArea?.x ?? mon?.rect?.x ?? 0;
    const monY = mon?.workArea?.y ?? mon?.rect?.y ?? 0;
    const w = Math.max(14, Math.min(98, (app.rect.w / Math.max(1, monW)) * 100));
    const h = Math.max(16, Math.min(98, (app.rect.h / Math.max(1, monH)) * 100));
    const left = Math.max(1, Math.min(99 - w, ((app.rect.x - monX) / Math.max(1, monW)) * 100));
    const top = Math.max(1, Math.min(99 - h, ((app.rect.y - monY) / Math.max(1, monH)) * 100));
    return { left, top, width: w, height: h };
  }
  return { left: 12, top: 10, width: 76, height: 78 };
}

export const WorkspaceWorkbench: FC<WorkspaceWorkbenchProps> = ({
  items,
  unrelatedAction,
  minimizeUnrelated,
  monitors,
  layouts,
  busyAction,
  editingWorkspace,
  isNewDraft,
  onUpdateGlobalUnrelatedAction,
  onStartRedFrameCapture,
  onQuickCaptureDesktop,
  onLaunchWorkspace,
  onDeleteWorkspace,
  onCreateDesktopShortcut,
  onOpenEditor,
  onChangeEditingWorkspace,
  onSaveEditingWorkspace,
  onCancelEditingWorkspace,
  onRestoreEditingSnapshot,
  canRestoreSnapshot,
}) => {
  const { t, i18n } = useTranslation();
  const [searchQuery, setSearchQuery] = useState('');
  const [sortBy, setSortBy] = useState<SortKey>('lastLaunched');
  const [quickCaptureName, setQuickCaptureName] = useState('');
  const [hoveredAppIdx, setHoveredAppIdx] = useState<number | null>(null);
  const [expandedAppIndices, setExpandedAppIndices] = useState<Record<number, boolean>>({});

  const effectiveUnrelated: UnrelatedWindowAction =
    unrelatedAction || (minimizeUnrelated ? 'minimize' : 'keep');

  const formatLastLaunched = (tsSec?: number): string => {
    if (!tsSec || tsSec <= 0) {
      return t('windowManager.wsNeverLaunched', 'Never');
    }
    try {
      const date = new Date(tsSec * 1000);
      return new Intl.DateTimeFormat(i18n.language || undefined, {
        month: 'short',
        day: 'numeric',
        hour: '2-digit',
        minute: '2-digit',
      }).format(date);
    } catch {
      return new Date(tsSec * 1000).toLocaleString();
    }
  };

  const filteredAndSortedItems = useMemo(() => {
    const q = searchQuery.trim().toLowerCase();
    const list = q
      ? items.filter((ws) => {
          if (ws.name.toLowerCase().includes(q)) return true;
          return ws.apps.some(
            (a) =>
              (a.appName ?? '').toLowerCase().includes(q) ||
              a.processName.toLowerCase().includes(q),
          );
        })
      : [...items];

    list.sort((a, b) => {
      if (sortBy === 'name') return a.name.localeCompare(b.name);
      if (sortBy === 'appCount') return b.apps.length - a.apps.length;
      return (b.lastLaunchedAt ?? 0) - (a.lastLaunchedAt ?? 0);
    });
    return list;
  }, [items, searchQuery, sortBy]);

  const layoutMap = useMemo(() => {
    const m = new Map<string, SnapZoneLayout>();
    for (const l of layouts) m.set(l.id, l);
    return m;
  }, [layouts]);

  // ── 视图 B：交互式编辑 / 捕获工作台（复刻并超越 PowerToys 图 3 / 4 / 6） ──
  if (editingWorkspace) {
    const stageMonitors: MonitorDescriptor[] =
      monitors.length > 0
        ? monitors
        : [
            {
              id: 'primary',
              name: t('windowManager.wsScreenLabel', 'Screen {{num}}', { num: 1 }),
              width: 1920,
              height: 1080,
              dpi: 96,
              isPrimary: true,
            },
          ];

    const resolveMonitorBucket = (app: WorkspaceAppEntry): number => {
      if (typeof app.monitorIndex === 'number' && app.monitorIndex >= 0 && app.monitorIndex < stageMonitors.length) {
        return app.monitorIndex;
      }
      if (app.monitorId) {
        const idx = stageMonitors.findIndex(
          (m) => m.id.toLowerCase() === app.monitorId.toLowerCase(),
        );
        if (idx >= 0) return idx;
      }
      return 0;
    };

    const indexedApps = editingWorkspace.apps.map((app, index) => ({ app, index }));
    const minimizedApps = indexedApps.filter(({ app }) => Boolean(app.minimized));
    const screenGroups = stageMonitors.map((mon, screenIdx) => ({
      mon,
      screenIdx,
      entries: indexedApps.filter(
        ({ app }) => !app.minimized && resolveMonitorBucket(app) === screenIdx,
      ),
    }));

    const boundLayout = editingWorkspace.boundLayoutId
      ? layoutMap.get(editingWorkspace.boundLayoutId)
      : undefined;
    const boundZoneCount = computeZoneCountForLayout(boundLayout);

    const toggleExpand = (idx: number) => {
      setExpandedAppIndices((prev) => ({ ...prev, [idx]: !prev[idx] }));
    };

    const updateAppAt = (idx: number, patch: Partial<WorkspaceAppEntry>) => {
      const nextApps = editingWorkspace.apps.map((a, i) => (i === idx ? { ...a, ...patch } : a));
      onChangeEditingWorkspace({ ...editingWorkspace, apps: nextApps });
    };

    const removeAppAt = (idx: number) => {
      const nextApps = editingWorkspace.apps.filter((_, i) => i !== idx);
      onChangeEditingWorkspace({ ...editingWorkspace, apps: nextApps });
      if (hoveredAppIdx === idx) setHoveredAppIdx(null);
    };

    const renderAppAccordionRow = (app: WorkspaceAppEntry, idx: number) => {
      const isExpanded = Boolean(expandedAppIndices[idx]);
      const isHovered = hoveredAppIdx === idx;
      const winState = getAppWindowState(app);
      const displayTitle = app.appName || app.processName || app.exePath;

      return (
        <div
          key={`${app.id ?? app.processName}-${idx}`}
          className={`ws-app-row ${isHovered ? 'is-hovered' : ''} ${isExpanded ? 'is-expanded' : ''}`}
          onMouseEnter={() => setHoveredAppIdx(idx)}
          onMouseLeave={() => setHoveredAppIdx((prev) => (prev === idx ? null : prev))}
        >
          <div className="ws-app-row__summary">
            <button
              type="button"
              className="ws-app-row__summary-trigger"
              aria-expanded={isExpanded}
              onClick={() => toggleExpand(idx)}
            >
              <span className="ws-app-row__icon">
                {app.iconDataUrl ? (
                  <img src={app.iconDataUrl} alt="" className="ws-app-row__icon-img" />
                ) : (
                  <AppWindow size={18} aria-hidden="true" />
                )}
              </span>

              <span className="ws-app-row__identity">
                <span className="ws-app-row__name">{displayTitle}</span>
                <span className="ws-app-row__meta-line">
                  {app.runAsAdmin && (
                    <span className="ws-admin-badge">
                      <ShieldCheck size={13} aria-hidden="true" />
                      <span>{t('windowManager.wsAdminBadge', 'Administrator')}</span>
                    </span>
                  )}
                  {app.processName && app.processName !== displayTitle && (
                    <CodeBadge variant="muted">{app.processName}</CodeBadge>
                  )}
                  {typeof app.zoneIndex === 'number' && !app.minimized && (
                    <Badge
                      variant="primary"
                      text={t('windowManager.zoneBadge', 'Zone {{num}}', { num: app.zoneIndex + 1 })}
                    />
                  )}
                  {app.alwaysOnTop && (
                    <Badge variant="success" text={t('windowManager.wsPinnedTag', 'Pinned')} />
                  )}
                  {typeof app.opacityPercent === 'number' && app.opacityPercent < 100 && (
                    <CodeBadge variant="muted">{app.opacityPercent}%</CodeBadge>
                  )}
                </span>
              </span>
            </button>

            <div className="ws-app-row__actions">
              <Button
                variant="ghost"
                size="sm"
                onClick={() => removeAppAt(idx)}
                ariaLabel={t('windowManager.wsRemoveApp', 'Remove')}
              >
                <span>{t('windowManager.wsRemoveApp', 'Remove')}</span>
              </Button>
              <button
                type="button"
                className="ws-app-row__chevron"
                aria-label={t('windowManager.wsExpandAppAria', 'Expand app details')}
                aria-expanded={isExpanded}
                onClick={() => toggleExpand(idx)}
              >
                {isExpanded ? (
                  <ChevronUp size={15} aria-hidden="true" />
                ) : (
                  <ChevronDown size={15} aria-hidden="true" />
                )}
              </button>
            </div>
          </div>

          {isExpanded && (
            <div className="ws-app-row__details">
              <div className="ws-app-detail-row">
                <label className="ws-inline-check">
                  <Toggle
                    id={`ws-app-admin-${idx}`}
                    checked={Boolean(app.runAsAdmin)}
                    onChange={(checked) => updateAppAt(idx, { runAsAdmin: checked })}
                  />
                  <span>{t('windowManager.wsRunAsAdminLabel', 'Launch as administrator')}</span>
                </label>

                <label className="ws-inline-check">
                  <Toggle
                    id={`ws-app-pin-${idx}`}
                    checked={Boolean(app.alwaysOnTop)}
                    onChange={(checked) => updateAppAt(idx, { alwaysOnTop: checked })}
                  />
                  <Pin size={13} aria-hidden="true" />
                  <span>{t('windowManager.wsAlwaysOnTopLabel', 'Pin on top after launch')}</span>
                </label>
              </div>

              <div className="ws-app-detail-grid">
                <div className="ws-detail-field ws-detail-field--wide">
                  <span className="ws-detail-label">
                    {t('windowManager.wsCliArgsLabel', 'CLI Arguments')}
                  </span>
                  <TextInput
                    value={app.args}
                    placeholder={t('windowManager.launchArgsPlaceholder', 'Optional launch arguments')}
                    ariaLabel={t('windowManager.wsCliArgsLabel', 'CLI Arguments')}
                    onChange={(args) => updateAppAt(idx, { args })}
                  />
                </div>

                <div className="ws-detail-field">
                  <span className="ws-detail-label">
                    {t('windowManager.wsWindowStateLabel', 'Window State')}
                  </span>
                  <Select
                    value={winState}
                    options={[
                      { value: 'normal', label: t('windowManager.wsStateNormal', 'Normal') },
                      { value: 'maximized', label: t('windowManager.wsStateMaximized', 'Maximized') },
                      { value: 'minimized', label: t('windowManager.wsStateMinimized', 'Minimized') },
                    ]}
                    onChange={(val) => {
                      const st = val as AppWindowState;
                      updateAppAt(idx, {
                        maximized: st === 'maximized',
                        minimized: st === 'minimized',
                      });
                    }}
                  />
                </div>

                {boundZoneCount > 0 && !app.minimized && (
                  <div className="ws-detail-field">
                    <span className="ws-detail-label">
                      {t('windowManager.wsZoneSlotLabel', 'SnapZone Slot')}
                    </span>
                    <Select
                      value={typeof app.zoneIndex === 'number' ? String(app.zoneIndex) : 'none'}
                      options={[
                        { value: 'none', label: t('windowManager.wsZoneSlotCustom', 'Exact Coordinates') },
                        ...Array.from({ length: boundZoneCount }, (_, zi) => ({
                          value: String(zi),
                          label: t('windowManager.zoneBadge', 'Zone {{num}}', { num: zi + 1 }),
                        })),
                      ]}
                      onChange={(val) => {
                        if (val === 'none') {
                          updateAppAt(idx, { zoneIndex: undefined, zoneIndices: undefined });
                        } else {
                          const z = Number(val);
                          updateAppAt(idx, { zoneIndex: z, zoneIndices: [z] });
                        }
                      }}
                    />
                  </div>
                )}

                <div className="ws-detail-field">
                  <span className="ws-detail-label">
                    {t('windowManager.wsOpacityLabel', 'Launch Opacity')}
                  </span>
                  <div className="ws-opacity-slider-wrap">
                    <input
                      type="range"
                      min={25}
                      max={100}
                      step={5}
                      value={app.opacityPercent ?? 100}
                      aria-label={t('windowManager.wsOpacityLabel', 'Launch Opacity')}
                      className="wm-range-slider"
                      onChange={(e) => updateAppAt(idx, { opacityPercent: Number(e.target.value) })}
                    />
                    <CodeBadge>{app.opacityPercent ?? 100}%</CodeBadge>
                  </div>
                </div>
              </div>

              <div className="ws-coords-bar">
                <span className="ws-detail-label">
                  {t('windowManager.wsWindowPosLabel', 'Window Coordinates')}
                </span>
                <div className="ws-coords-inputs">
                  <label className="ws-coord-item">
                    <span>{t('windowManager.wsCoordLeft', 'Left')}</span>
                    <NumberInput
                      value={app.rect?.x ?? 0}
                      min={-15360}
                      max={15360}
                      step={10}
                      ariaLabel={t('windowManager.wsCoordLeft', 'Left')}
                      onChange={(x) =>
                        updateAppAt(idx, {
                          rect: {
                            x,
                            y: app.rect?.y ?? 0,
                            w: app.rect?.w ?? 1200,
                            h: app.rect?.h ?? 800,
                          },
                          relRect: undefined,
                        })
                      }
                    />
                  </label>
                  <label className="ws-coord-item">
                    <span>{t('windowManager.wsCoordTop', 'Top')}</span>
                    <NumberInput
                      value={app.rect?.y ?? 0}
                      min={-8640}
                      max={8640}
                      step={10}
                      ariaLabel={t('windowManager.wsCoordTop', 'Top')}
                      onChange={(y) =>
                        updateAppAt(idx, {
                          rect: {
                            x: app.rect?.x ?? 0,
                            y,
                            w: app.rect?.w ?? 1200,
                            h: app.rect?.h ?? 800,
                          },
                          relRect: undefined,
                        })
                      }
                    />
                  </label>
                  <label className="ws-coord-item">
                    <span>{t('windowManager.wsCoordWidth', 'Width')}</span>
                    <NumberInput
                      value={app.rect?.w ?? 1200}
                      min={160}
                      max={15360}
                      step={20}
                      ariaLabel={t('windowManager.wsCoordWidth', 'Width')}
                      onChange={(w) =>
                        updateAppAt(idx, {
                          rect: {
                            x: app.rect?.x ?? 0,
                            y: app.rect?.y ?? 0,
                            w,
                            h: app.rect?.h ?? 800,
                          },
                          relRect: undefined,
                        })
                      }
                    />
                  </label>
                  <label className="ws-coord-item">
                    <span>{t('windowManager.wsCoordHeight', 'Height')}</span>
                    <NumberInput
                      value={app.rect?.h ?? 800}
                      min={120}
                      max={8640}
                      step={20}
                      ariaLabel={t('windowManager.wsCoordHeight', 'Height')}
                      onChange={(h) =>
                        updateAppAt(idx, {
                          rect: {
                            x: app.rect?.x ?? 0,
                            y: app.rect?.y ?? 0,
                            w: app.rect?.w ?? 1200,
                            h,
                          },
                          relRect: undefined,
                        })
                      }
                    />
                  </label>
                </div>
              </div>
            </div>
          )}
        </div>
      );
    };

    return (
      <div className="ws-workbench-editor">
        {/* 面包屑顶栏 */}
        <div className="ws-editor-header">
          <div className="ws-editor-breadcrumb">
            <Button variant="ghost" size="sm" onClick={onCancelEditingWorkspace}>
              <ArrowLeft size={14} aria-hidden="true" />
              <span>{t('windowManager.tabWorkspaces', 'Workspaces')}</span>
            </Button>
            <span className="ws-editor-breadcrumb__sep" aria-hidden="true">
              /
            </span>
            <h3 className="ws-editor-breadcrumb__title">
              {isNewDraft
                ? t('windowManager.wsCreateWorkspaceTitle', 'Create Workspace')
                : t('windowManager.editWorkspaceModalTitle', 'Edit Workspace')}
            </h3>
          </div>
          <div className="ws-editor-header__actions">
            <Button
              variant="primary"
              size="sm"
              disabled={!editingWorkspace.name.trim()}
              onClick={onSaveEditingWorkspace}
            >
              <Save size={14} aria-hidden="true" />
              <span>{t('common.save', 'Save')}</span>
            </Button>
            <Button variant="ghost" size="sm" onClick={onCancelEditingWorkspace}>
              <X size={14} aria-hidden="true" />
              <span>{t('common.cancel', 'Cancel')}</span>
            </Button>
          </div>
        </div>

        {/* 顶部双层交互式微缩桌面舞台 + 底部任务栏（复刻并超越图 3 & 图 4） */}
        <Card className="ws-stage-card">
          <div className="ws-stage-monitors">
            {screenGroups.map(({ mon, screenIdx, entries }) => {
              const hoveredEntry = entries.find((e) => e.index === hoveredAppIdx);
              const aspectRatio = `${Math.max(1, mon.width)} / ${Math.max(1, mon.height)}`;
              return (
                <div key={`${mon.id}-${screenIdx}`} className="ws-stage-screen-col">
                  <div className="ws-stage-screen" style={{ aspectRatio }}>
                    {entries.length === 0 ? (
                      <div className="ws-stage-empty-screen">
                        <Monitor size={18} aria-hidden="true" />
                      </div>
                    ) : (
                      entries.map(({ app, index }, order) => {
                        const box = getNormalizedStageBox(app, mon);
                        const isActive = hoveredAppIdx === index;
                        const isDimmed = hoveredAppIdx !== null && !isActive;
                        return (
                          <button
                            key={`stage-win-${index}`}
                            type="button"
                            className={`ws-stage-window ${isActive ? 'is-active' : ''} ${
                              isDimmed ? 'is-dimmed' : ''
                            }`}
                            style={{
                              left: `${box.left}%`,
                              top: `${box.top}%`,
                              width: `${box.width}%`,
                              height: `${box.height}%`,
                              zIndex: isActive ? 50 : order + 1,
                            }}
                            title={app.appName || app.processName}
                            onMouseEnter={() => setHoveredAppIdx(index)}
                            onMouseLeave={() =>
                              setHoveredAppIdx((prev) => (prev === index ? null : prev))
                            }
                            onClick={() => toggleExpand(index)}
                          >
                            {app.iconDataUrl ? (
                              <img
                                src={app.iconDataUrl}
                                alt=""
                                className="ws-stage-window__icon"
                              />
                            ) : (
                              <AppWindow size={18} aria-hidden="true" />
                            )}
                          </button>
                        );
                      })
                    )}
                    {hoveredEntry && (
                      <div className="ws-stage-hover-caption">
                        {hoveredEntry.app.appName || hoveredEntry.app.processName}
                      </div>
                    )}
                  </div>

                  {/* 底部微缩任务栏（展示全部最小化应用图标，悬停双向联动发光） */}
                  {screenIdx === 0 && minimizedApps.length > 0 && (
                    <div
                      className={`ws-stage-taskbar ${
                        minimizedApps.some((m) => m.index === hoveredAppIdx) ? 'is-active' : ''
                      }`}
                      aria-label={t('windowManager.wsMinimizedAppsGroup', 'Minimized apps')}
                    >
                      {minimizedApps.map(({ app, index }) => {
                        const isActive = hoveredAppIdx === index;
                        return (
                          <button
                            key={`tb-${index}`}
                            type="button"
                            className={`ws-stage-taskbar__item ${isActive ? 'is-active' : ''}`}
                            title={app.appName || app.processName}
                            onMouseEnter={() => setHoveredAppIdx(index)}
                            onMouseLeave={() =>
                              setHoveredAppIdx((prev) => (prev === index ? null : prev))
                            }
                            onClick={() => toggleExpand(index)}
                          >
                            {app.iconDataUrl ? (
                              <img src={app.iconDataUrl} alt="" className="ws-stage-taskbar__icon" />
                            ) : (
                              <AppWindow size={13} aria-hidden="true" />
                            )}
                          </button>
                        );
                      })}
                    </div>
                  )}

                  <span className="ws-stage-screen-label">
                    {t('windowManager.wsScreenLabel', 'Screen {{num}}', { num: screenIdx + 1 })} ·{' '}
                    {mon.width}x{mon.height}
                  </span>
                </div>
              );
            })}
          </div>

          <div className="ws-stage-footer">
            <span className="ws-stage-hint">
              {t(
                'windowManager.wsStageInteractiveHint',
                'Hover any app below to highlight its live position on screen or taskbar; click to fine-tune.',
              )}
            </span>
            <div className="ws-stage-footer__btns">
              <Button
                variant="secondary"
                size="sm"
                disabled={busyAction === 'start_workspace_capture'}
                onClick={() => onStartRedFrameCapture(editingWorkspace.id)}
              >
                <Camera size={13} aria-hidden="true" />
                <span>{t('windowManager.wsLaunchAndEditBtn', 'Launch and edit')}</span>
              </Button>
              <Button
                variant="ghost"
                size="sm"
                disabled={!canRestoreSnapshot}
                onClick={onRestoreEditingSnapshot}
              >
                <RotateCcw size={13} aria-hidden="true" />
                <span>{t('windowManager.wsRestoreSnapshotBtn', 'Revert')}</span>
              </Button>
            </div>
          </div>
        </Card>

        {/* 工作区名称、桌面快捷方式、移动现有窗口、SnapZones 联动布局与三态清场策略 */}
        <Card className="ws-meta-card">
          <div className="ws-meta-grid">
            <div className="ws-detail-field">
              <span className="ws-detail-label">
                {t('windowManager.wsNameFieldLabel', 'Workspace name')}
              </span>
              <TextInput
                value={editingWorkspace.name}
                placeholder={t(
                  'windowManager.workspaceNamePlaceholder',
                  'Workspace name (e.g. Deep Coding, Morning Briefing)',
                )}
                ariaLabel={t('windowManager.wsNameFieldLabel', 'Workspace name')}
                onChange={(name) => onChangeEditingWorkspace({ ...editingWorkspace, name })}
              />
            </div>

            <div className="ws-detail-field">
              <span className="ws-detail-label">
                {t('windowManager.wsBoundLayoutLabel', 'Linked SnapZones Layout')}
              </span>
              <Select
                value={editingWorkspace.boundLayoutId || ''}
                options={[
                  { value: '', label: t('windowManager.wsNoBoundLayout', 'None (Keep Current)') },
                  ...layouts
                    .filter((l) => l.type !== 'none')
                    .map((l) => ({ value: l.id, label: l.name })),
                ]}
                onChange={(val) =>
                  onChangeEditingWorkspace({ ...editingWorkspace, boundLayoutId: val })
                }
              />
            </div>

            <div className="ws-detail-field">
              <span className="ws-detail-label">
                {t('windowManager.wsUnrelatedActionLabel', 'Unrelated Windows')}
              </span>
              <Select
                value={editingWorkspace.unrelatedAction || ''}
                options={[
                  { value: '', label: t('windowManager.wsUnrelatedFollowGlobal', 'Follow Global Setting') },
                  { value: 'keep', label: t('windowManager.wsUnrelatedKeep', 'Keep Unchanged') },
                  { value: 'minimize', label: t('windowManager.wsUnrelatedMinimize', 'Minimize to Taskbar') },
                  { value: 'minimize_to_tray', label: t('windowManager.wsUnrelatedTray', 'Tuck into System Tray') },
                ]}
                onChange={(val) =>
                  onChangeEditingWorkspace({
                    ...editingWorkspace,
                    unrelatedAction: (val || undefined) as UnrelatedWindowAction | undefined,
                  })
                }
              />
            </div>
          </div>

          <div className="ws-meta-checks">
            <label className="ws-inline-check">
              <Toggle
                id="ws-edit-create-shortcut"
                checked={Boolean(editingWorkspace.createDesktopShortcut)}
                onChange={(checked) =>
                  onChangeEditingWorkspace({ ...editingWorkspace, createDesktopShortcut: checked })
                }
              />
              <span>{t('windowManager.wsCreateDesktopShortcutLabel', 'Create desktop shortcut')}</span>
            </label>
            <label className="ws-inline-check">
              <Toggle
                id="ws-edit-move-existing"
                checked={editingWorkspace.moveExistingWindows !== false}
                onChange={(checked) =>
                  onChangeEditingWorkspace({ ...editingWorkspace, moveExistingWindows: checked })
                }
              />
              <span>{t('windowManager.wsMoveExistingLabel', 'Move existing windows')}</span>
            </label>
          </div>
        </Card>

        {/* 分组应用列表：Screen 1 / Screen 2 ... + 最小化应用 */}
        {editingWorkspace.apps.length === 0 ? (
          <Card>
            <div className="wm-empty-state">
              <AppWindow size={24} aria-hidden="true" />
              <div className="wm-empty-state__title">
                {t('windowManager.workspaceNoApps', 'No applications recorded in this workspace')}
              </div>
            </div>
          </Card>
        ) : (
          <>
            {screenGroups.map(({ screenIdx, entries }) => {
              if (entries.length === 0) return null;
              return (
                <div key={`group-screen-${screenIdx}`} className="ws-app-group">
                  <div className="ws-app-group__title">
                    <Monitor size={14} aria-hidden="true" />
                    <span>
                      {t('windowManager.wsScreenLabel', 'Screen {{num}}', { num: screenIdx + 1 })}
                    </span>
                    <Badge variant="muted" text={String(entries.length)} />
                  </div>
                  <div className="ws-app-group__list">
                    {entries.map(({ app, index }) => renderAppAccordionRow(app, index))}
                  </div>
                </div>
              );
            })}

            {minimizedApps.length > 0 && (
              <div className="ws-app-group">
                <div className="ws-app-group__title">
                  <Layers size={14} aria-hidden="true" />
                  <span>{t('windowManager.wsMinimizedAppsGroup', 'Minimized apps')}</span>
                  <Badge variant="muted" text={String(minimizedApps.length)} />
                </div>
                <div className="ws-app-group__list">
                  {minimizedApps.map(({ app, index }) => renderAppAccordionRow(app, index))}
                </div>
              </div>
            )}
          </>
        )}
      </div>
    );
  }

  // ── 视图 A：工作区列表主页（复刻并超越 PowerToys 图 1 & 图 5） ─────────────
  return (
    <div className="ws-workbench-list">
      <Card>
        <div className="ws-toolbar">
          <div className="ws-toolbar__search">
            <Search size={14} className="ws-toolbar__search-icon" aria-hidden="true" />
            <TextInput
              value={searchQuery}
              onChange={setSearchQuery}
              placeholder={t('windowManager.wsSearchPlaceholder', 'Search workspaces or apps...')}
              ariaLabel={t('windowManager.wsSearchPlaceholder', 'Search workspaces or apps...')}
            />
          </div>

          <div className="ws-toolbar__right">
            <div className="ws-sort-wrap">
              <span className="ws-sort-label">{t('windowManager.wsSortByLabel', 'Sort by')}</span>
              <Select
                value={sortBy}
                options={[
                  { value: 'lastLaunched', label: t('windowManager.wsSortLastLaunched', 'Last launched') },
                  { value: 'name', label: t('windowManager.wsSortName', 'Name') },
                  { value: 'appCount', label: t('windowManager.wsSortAppCount', 'App count') },
                ]}
                onChange={(v) => setSortBy(v as SortKey)}
              />
            </div>

            <Button
              variant="primary"
              size="sm"
              disabled={busyAction === 'start_workspace_capture'}
              onClick={() => onStartRedFrameCapture()}
            >
              {busyAction === 'start_workspace_capture' ? (
                <Loader2 size={14} className="wm-spin" aria-hidden="true" />
              ) : (
                <Plus size={14} aria-hidden="true" />
              )}
              <span>{t('windowManager.wsCreateWorkspaceBtn', 'Create workspace')}</span>
            </Button>
          </div>
        </div>

        {/* 全局三态清场策略 + 快捷一键快照条（保留快捷无框即时捕获通道） */}
        <SettingRow
          label={t('windowManager.wsGlobalUnrelatedLabel', 'Unrelated Windows on Launch')}
          description={t(
            'windowManager.wsGlobalUnrelatedDesc',
            'Choose what happens to open windows that are not part of the launched workspace',
          )}
        >
          <Select
            value={effectiveUnrelated}
            options={[
              { value: 'keep', label: t('windowManager.wsUnrelatedKeep', 'Keep Unchanged') },
              { value: 'minimize', label: t('windowManager.wsUnrelatedMinimize', 'Minimize to Taskbar') },
              { value: 'minimize_to_tray', label: t('windowManager.wsUnrelatedTray', 'Tuck into System Tray') },
            ]}
            onChange={(val) => onUpdateGlobalUnrelatedAction(val as UnrelatedWindowAction)}
          />
        </SettingRow>

        <div className="wm-ws-capture-bar">
          <div className="wm-ws-capture-bar__input">
            <TextInput
              value={quickCaptureName}
              onChange={setQuickCaptureName}
              placeholder={t(
                'windowManager.workspaceNamePlaceholder',
                'Workspace name (e.g. Deep Coding, Morning Briefing)',
              )}
              ariaLabel={t('windowManager.workspaceNameAria', 'New workspace name')}
            />
          </div>
          <Button
            variant="secondary"
            size="sm"
            disabled={busyAction === 'capture_workspace'}
            onClick={() => {
              onQuickCaptureDesktop(quickCaptureName);
              setQuickCaptureName('');
            }}
          >
            {busyAction === 'capture_workspace' ? (
              <Loader2 size={14} className="wm-spin" aria-hidden="true" />
            ) : (
              <Camera size={14} aria-hidden="true" />
            )}
            <span>{t('windowManager.captureDesktopBtn', 'Capture Current Desktop')}</span>
          </Button>
        </div>
      </Card>

      {filteredAndSortedItems.length === 0 ? (
        <Card>
          <div className="wm-empty-state">
            <Layers size={28} aria-hidden="true" />
            <div className="wm-empty-state__title">
              {items.length === 0
                ? t('windowManager.workspacesEmptyTitle', 'No Saved Workspaces Yet')
                : t('windowManager.wsNoSearchMatchTitle', 'No Matching Workspaces')}
            </div>
            <p className="wm-empty-state__desc">
              {items.length === 0
                ? t(
                    'windowManager.wsEmptyV2Desc',
                    'Click Create workspace to enter red-border live staging across all monitors, arrange your apps, and capture everything in one click.',
                  )
                : t('windowManager.wsNoSearchMatchDesc', 'Try a different keyword or clear the search box.')}
            </p>
          </div>
        </Card>
      ) : (
        <div className="ws-card-stack">
          {filteredAndSortedItems.map((ws) => {
            const linkedLayout = ws.boundLayoutId ? layoutMap.get(ws.boundLayoutId) : undefined;
            return (
              <Card key={ws.id} className="ws-list-card">
                <div className="ws-list-card__main">
                  <div className="ws-list-card__top">
                    <h3 className="wm-ws-card__title">{ws.name}</h3>
                    {linkedLayout && <Badge variant="primary" text={linkedLayout.name} />}
                    {ws.desktopName && <CodeBadge variant="muted">{ws.desktopName}</CodeBadge>}
                  </div>

                  {/* 横向真实彩色应用图标带（图 5 核心视觉） */}
                  <div className="ws-icon-strip">
                    {ws.apps.slice(0, 12).map((app, idx) => (
                      <span
                        key={`${app.processName}-${idx}`}
                        className={`ws-icon-tile ${app.minimized ? 'is-minimized' : ''}`}
                        title={app.appName || app.processName}
                      >
                        {app.iconDataUrl ? (
                          <img src={app.iconDataUrl} alt="" className="ws-icon-tile__img" />
                        ) : (
                          <AppWindow size={15} aria-hidden="true" />
                        )}
                      </span>
                    ))}
                    {ws.apps.length > 12 && (
                      <CodeBadge variant="muted">+{ws.apps.length - 12}</CodeBadge>
                    )}
                    <span className="ws-icon-strip__count">
                      {t('windowManager.workspaceAppsCount', '{{count}} App(s)', {
                        count: ws.apps.length,
                      })}
                    </span>
                  </div>

                  <div className="ws-list-card__meta">
                    <Clock size={12} aria-hidden="true" />
                    <span>
                      {t('windowManager.wsLastLaunchedLabel', 'Last launched: {{time}}', {
                        time: formatLastLaunched(ws.lastLaunchedAt),
                      })}
                    </span>
                  </div>
                </div>

                <div className="ws-list-card__actions">
                  <Button
                    variant="primary"
                    size="sm"
                    disabled={busyAction === 'launch_workspace'}
                    onClick={() => onLaunchWorkspace(ws.id)}
                  >
                    <Play size={13} aria-hidden="true" />
                    <span>{t('windowManager.launchWorkspaceBtn', 'Launch')}</span>
                  </Button>
                  <Button
                    variant="ghost"
                    size="sm"
                    onClick={() => onOpenEditor(ws, false)}
                    ariaLabel={t('common.edit', 'Edit')}
                    title={t('common.edit', 'Edit')}
                  >
                    <Pencil size={13} aria-hidden="true" />
                  </Button>
                  <Button
                    variant="ghost"
                    size="sm"
                    disabled={busyAction === 'create_shortcut'}
                    onClick={() => onCreateDesktopShortcut(ws.id, ws.name)}
                    ariaLabel={t('windowManager.wsCreateDesktopShortcutLabel', 'Create desktop shortcut')}
                    title={t('windowManager.wsCreateDesktopShortcutLabel', 'Create desktop shortcut')}
                  >
                    <ExternalLink size={13} aria-hidden="true" />
                  </Button>
                  <Button
                    variant="ghost"
                    size="sm"
                    onClick={() => onDeleteWorkspace(ws.id)}
                    ariaLabel={t('common.delete', 'Delete')}
                    title={t('common.delete', 'Delete')}
                  >
                    <Trash2 size={13} aria-hidden="true" />
                  </Button>
                </div>
              </Card>
            );
          })}
        </div>
      )}
    </div>
  );
};
